//! Keeps TDLib's files in an application-provided file system.
//!
//! Every file and directory strictly below one path prefix is opened, read, written, listed and
//! deleted through a [`FileSystem`] instead of the OS. Paths outside the prefix, including the
//! database and the binlog when they live elsewhere, go to the OS as before. Needs a tdjson built
//! with `td_set_file_system` (TDLib with the file-system hook).
use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};

const VERSION: u32 = 1;

const ERROR: i32 = -1;
const ERROR_NOT_FOUND: i32 = -2;
const ERROR_EXISTS: i32 = -3;
const ERROR_NOT_EMPTY: i32 = -4;
const ERROR_NO_SPACE: i32 = -5;
const ERROR_IS_DIRECTORY: i32 = -6;
const ERROR_NOT_DIRECTORY: i32 = -7;

/// Why a [`FileSystem`] call failed; TDLib only logs it, apart from telling success from failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FsError {
    Other,
    NotFound,
    Exists,
    NotEmpty,
    NoSpace,
    IsDirectory,
    NotDirectory,
}

impl FsError {
    fn code(self) -> i32 {
        match self {
            FsError::Other => ERROR,
            FsError::NotFound => ERROR_NOT_FOUND,
            FsError::Exists => ERROR_EXISTS,
            FsError::NotEmpty => ERROR_NOT_EMPTY,
            FsError::NoSpace => ERROR_NO_SPACE,
            FsError::IsDirectory => ERROR_IS_DIRECTORY,
            FsError::NotDirectory => ERROR_NOT_DIRECTORY,
        }
    }
}

/// How [`FileSystem::open`] opens a file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OpenFlags(i32);

impl OpenFlags {
    pub const WRITE: OpenFlags = OpenFlags(1);
    pub const READ: OpenFlags = OpenFlags(2);
    pub const TRUNCATE: OpenFlags = OpenFlags(4);
    pub const CREATE: OpenFlags = OpenFlags(8);
    pub const APPEND: OpenFlags = OpenFlags(16);
    /// Creates the file and fails with [`FsError::Exists`] if it already exists.
    pub const CREATE_NEW: OpenFlags = OpenFlags(32);

    pub fn from_bits(bits: i32) -> OpenFlags {
        OpenFlags(bits)
    }

    pub fn bits(self) -> i32 {
        self.0
    }

    pub fn contains(self, other: OpenFlags) -> bool {
        self.0 & other.0 == other.0
    }
}

impl std::ops::BitOr for OpenFlags {
    type Output = OpenFlags;

    fn bitor(self, other: OpenFlags) -> OpenFlags {
        OpenFlags(self.0 | other.0)
    }
}

/// What [`FileSystem::stat`] reports about a file or a directory.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FileStat {
    /// 1 for a directory, 0 for a regular file.
    pub is_directory: i32,
    /// The length of a file in bytes.
    pub size: i64,
    /// The bytes a file takes in storage; shown in storage statistics.
    pub allocated_size: i64,
    /// The last access, in nanoseconds since the Unix epoch; orders the removal of old files.
    pub access_time_nsec: u64,
    /// The last write, in nanoseconds since the Unix epoch. It must not change unless the file
    /// is written, or TDLib treats the file as modified and drops it.
    pub modification_time_nsec: u64,
}

/// A file system for TDLib's files. Every path is absolute and starts with the prefix given to
/// [`set_file_system`]; a directory path may end with a separator. TDLib calls it from several
/// threads at once and may keep several handles open on the same file.
pub trait FileSystem: Send + Sync + 'static {
    /// Opens a file and returns its handle.
    fn open(&self, path: &str, flags: OpenFlags) -> Result<u64, FsError>;
    /// Closes a handle. A file unlinked while open is freed after its last handle is closed.
    fn close(&self, handle: u64);
    /// Reads `buffer.len()` bytes at `offset`, fewer only at the end of the file: TDLib treats a
    /// short read of a regular file as an error.
    fn read_at(&self, handle: u64, buffer: &mut [u8], offset: u64) -> Result<usize, FsError>;
    /// Writes all of `data` at `offset` or fails; a write past the end leaves the gap filled with
    /// zeros.
    fn write_at(&self, handle: u64, data: &[u8], offset: u64) -> Result<usize, FsError>;
    /// The length of an open file.
    fn size(&self, handle: u64) -> Result<u64, FsError>;
    /// Changes the length of an open file.
    fn truncate(&self, handle: u64, size: u64) -> Result<(), FsError>;
    /// Makes the data written through the handle durable.
    fn sync(&self, handle: u64) -> Result<(), FsError>;
    /// Describes a file or a directory.
    fn stat(&self, path: &str) -> Result<FileStat, FsError>;
    /// Renames a file, replacing an existing target atomically; open handles stay valid.
    fn rename(&self, from: &str, to: &str) -> Result<(), FsError>;
    /// Removes the name of a file at once; open handles stay valid.
    fn unlink(&self, path: &str) -> Result<(), FsError>;
    /// Creates a directory; an existing directory is not an error.
    fn mkdir(&self, path: &str) -> Result<(), FsError>;
    /// Removes an empty directory.
    fn rmdir(&self, path: &str) -> Result<(), FsError>;
    /// Calls `entry(name, is_directory)` for every entry of a directory until it returns false.
    fn list(&self, path: &str, entry: &mut dyn FnMut(&str, bool) -> bool) -> Result<(), FsError>;
}

/// Why [`set_file_system`] failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SetFileSystemError {
    /// The prefix contains a NUL byte.
    InvalidPrefix,
    /// A file system is already installed, or TDLib refused the table.
    Rejected,
}

type EntryFn = unsafe extern "C" fn(*mut c_void, *const c_char, i32) -> i32;

#[repr(C)]
struct TdFileSystem {
    version: u32,
    context: *mut c_void,
    open: unsafe extern "C" fn(*mut c_void, *const c_char, i32, *mut u64) -> i32,
    close: unsafe extern "C" fn(*mut c_void, u64),
    read_at: unsafe extern "C" fn(*mut c_void, u64, *mut c_void, usize, i64) -> i64,
    write_at: unsafe extern "C" fn(*mut c_void, u64, *const c_void, usize, i64) -> i64,
    size: unsafe extern "C" fn(*mut c_void, u64, *mut i64) -> i32,
    truncate: unsafe extern "C" fn(*mut c_void, u64, i64) -> i32,
    sync: unsafe extern "C" fn(*mut c_void, u64) -> i32,
    stat: unsafe extern "C" fn(*mut c_void, *const c_char, *mut FileStat) -> i32,
    rename: unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> i32,
    unlink: unsafe extern "C" fn(*mut c_void, *const c_char) -> i32,
    mkdir: unsafe extern "C" fn(*mut c_void, *const c_char) -> i32,
    rmdir: unsafe extern "C" fn(*mut c_void, *const c_char) -> i32,
    list: unsafe extern "C" fn(*mut c_void, *const c_char, EntryFn, *mut c_void) -> i32,
}

unsafe extern "C" {
    fn td_set_file_system(prefix: *const c_char, file_system: *const TdFileSystem) -> c_int;
}

/// Installs `file_system` for every path strictly below `prefix`, an absolute path in the form
/// `files_directory` takes after TDLib's realpath; the prefix directory itself stays on the OS,
/// and a client's `database_directory` must be outside it. Call it before the first client is created. It can be installed once and
/// stays installed until the process exits. While a client's `files_directory` is below the
/// prefix, TDLib keeps all its files there, thumbnails and stickers included.
pub fn set_file_system(
    prefix: &str,
    file_system: Box<dyn FileSystem>,
) -> Result<(), SetFileSystemError> {
    let prefix = CString::new(prefix).map_err(|_| SetFileSystemError::InvalidPrefix)?;
    let context = Box::into_raw(Box::new(file_system));
    let table = table(context.cast());
    if unsafe { td_set_file_system(prefix.as_ptr(), &table) } == 1 {
        // TDLib calls into it until the process exits
        Ok(())
    } else {
        drop(unsafe { Box::from_raw(context) });
        Err(SetFileSystemError::Rejected)
    }
}

fn table(context: *mut c_void) -> TdFileSystem {
    TdFileSystem {
        version: VERSION,
        context,
        open: fs_open,
        close: fs_close,
        read_at: fs_read_at,
        write_at: fs_write_at,
        size: fs_size,
        truncate: fs_truncate,
        sync: fs_sync,
        stat: fs_stat,
        rename: fs_rename,
        unlink: fs_unlink,
        mkdir: fs_mkdir,
        rmdir: fs_rmdir,
        list: fs_list,
    }
}

/// Runs a call into the file system; a panic must not unwind into TDLib.
fn guarded<T>(fallback: T, call: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(call)).unwrap_or(fallback)
}

unsafe fn file_system<'a>(context: *mut c_void) -> &'a dyn FileSystem {
    unsafe { &**(context as *const Box<dyn FileSystem>) }
}

unsafe fn path<'a>(path: *const c_char) -> Result<&'a str, FsError> {
    if path.is_null() {
        return Err(FsError::Other);
    }
    unsafe { CStr::from_ptr(path) }
        .to_str()
        .map_err(|_| FsError::Other)
}

fn status(result: Result<(), FsError>) -> i32 {
    match result {
        Ok(()) => 0,
        Err(error) => error.code(),
    }
}

unsafe extern "C" fn fs_open(
    context: *mut c_void,
    path_ptr: *const c_char,
    flags: i32,
    handle: *mut u64,
) -> i32 {
    guarded(ERROR, || {
        let opened = unsafe { path(path_ptr) }.and_then(|path| unsafe {
            file_system(context).open(path, OpenFlags::from_bits(flags))
        });
        match opened {
            Ok(value) => {
                unsafe { *handle = value };
                0
            }
            Err(error) => error.code(),
        }
    })
}

unsafe extern "C" fn fs_close(context: *mut c_void, handle: u64) {
    guarded((), || unsafe { file_system(context).close(handle) })
}

unsafe extern "C" fn fs_read_at(
    context: *mut c_void,
    handle: u64,
    buffer: *mut c_void,
    size: usize,
    offset: i64,
) -> i64 {
    guarded(ERROR as i64, || {
        let Ok(offset) = u64::try_from(offset) else {
            return ERROR as i64;
        };
        let buffer: &mut [u8] = if size == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(buffer.cast(), size) }
        };
        match unsafe { file_system(context).read_at(handle, buffer, offset) } {
            Ok(read) => read.min(size) as i64,
            Err(error) => error.code() as i64,
        }
    })
}

unsafe extern "C" fn fs_write_at(
    context: *mut c_void,
    handle: u64,
    data: *const c_void,
    size: usize,
    offset: i64,
) -> i64 {
    guarded(ERROR as i64, || {
        let Ok(offset) = u64::try_from(offset) else {
            return ERROR as i64;
        };
        let data: &[u8] = if size == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(data.cast(), size) }
        };
        match unsafe { file_system(context).write_at(handle, data, offset) } {
            Ok(written) => written.min(size) as i64,
            Err(error) => error.code() as i64,
        }
    })
}

unsafe extern "C" fn fs_size(context: *mut c_void, handle: u64, size: *mut i64) -> i32 {
    guarded(ERROR, || {
        match unsafe { file_system(context).size(handle) } {
            Ok(value) => match i64::try_from(value) {
                Ok(value) => {
                    unsafe { *size = value };
                    0
                }
                Err(_) => ERROR,
            },
            Err(error) => error.code(),
        }
    })
}

unsafe extern "C" fn fs_truncate(context: *mut c_void, handle: u64, size: i64) -> i32 {
    guarded(ERROR, || match u64::try_from(size) {
        Ok(size) => status(unsafe { file_system(context).truncate(handle, size) }),
        Err(_) => ERROR,
    })
}

unsafe extern "C" fn fs_sync(context: *mut c_void, handle: u64) -> i32 {
    guarded(ERROR, || {
        status(unsafe { file_system(context).sync(handle) })
    })
}

unsafe extern "C" fn fs_stat(
    context: *mut c_void,
    path_ptr: *const c_char,
    stat: *mut FileStat,
) -> i32 {
    guarded(ERROR, || {
        match unsafe { path(path_ptr) }.and_then(|path| unsafe { file_system(context).stat(path) })
        {
            Ok(value) => {
                unsafe { *stat = value };
                0
            }
            Err(error) => error.code(),
        }
    })
}

unsafe extern "C" fn fs_rename(
    context: *mut c_void,
    from: *const c_char,
    to: *const c_char,
) -> i32 {
    guarded(ERROR, || {
        status(unsafe {
            path(from)
                .and_then(|from| path(to).and_then(|to| file_system(context).rename(from, to)))
        })
    })
}

unsafe extern "C" fn fs_unlink(context: *mut c_void, path_ptr: *const c_char) -> i32 {
    guarded(ERROR, || {
        status(unsafe { path(path_ptr).and_then(|path| file_system(context).unlink(path)) })
    })
}

unsafe extern "C" fn fs_mkdir(context: *mut c_void, path_ptr: *const c_char) -> i32 {
    guarded(ERROR, || {
        status(unsafe { path(path_ptr).and_then(|path| file_system(context).mkdir(path)) })
    })
}

unsafe extern "C" fn fs_rmdir(context: *mut c_void, path_ptr: *const c_char) -> i32 {
    guarded(ERROR, || {
        status(unsafe { path(path_ptr).and_then(|path| file_system(context).rmdir(path)) })
    })
}

unsafe extern "C" fn fs_list(
    context: *mut c_void,
    path_ptr: *const c_char,
    entry: EntryFn,
    list_context: *mut c_void,
) -> i32 {
    guarded(ERROR, || {
        let mut on_entry = |name: &str, is_directory: bool| -> bool {
            // a name with a NUL can't reach TDLib; it is skipped
            let Ok(name) = CString::new(name) else {
                return true;
            };
            unsafe { entry(list_context, name.as_ptr(), i32::from(is_directory)) == 0 }
        };
        status(unsafe {
            path(path_ptr).and_then(|path| file_system(context).list(path, &mut on_entry))
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    #[derive(Default)]
    struct Recorder {
        calls: Mutex<Vec<String>>,
    }

    impl FileSystem for Recorder {
        fn open(&self, path: &str, flags: OpenFlags) -> Result<u64, FsError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("open {path} {}", flags.bits()));
            if flags.contains(OpenFlags::CREATE_NEW) {
                Err(FsError::Exists)
            } else {
                Ok(42)
            }
        }
        fn close(&self, handle: u64) {
            self.calls.lock().unwrap().push(format!("close {handle}"));
        }
        fn read_at(&self, _: u64, buffer: &mut [u8], offset: u64) -> Result<usize, FsError> {
            buffer[0] = offset as u8;
            Ok(1)
        }
        fn write_at(&self, _: u64, data: &[u8], _: u64) -> Result<usize, FsError> {
            Ok(data.len())
        }
        fn size(&self, _: u64) -> Result<u64, FsError> {
            Ok(7)
        }
        fn truncate(&self, _: u64, _: u64) -> Result<(), FsError> {
            Err(FsError::NoSpace)
        }
        fn sync(&self, _: u64) -> Result<(), FsError> {
            panic!("sync failed loudly")
        }
        fn stat(&self, _: &str) -> Result<FileStat, FsError> {
            Ok(FileStat {
                size: 3,
                modification_time_nsec: 9,
                ..FileStat::default()
            })
        }
        fn rename(&self, from: &str, to: &str) -> Result<(), FsError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("rename {from} {to}"));
            Ok(())
        }
        fn unlink(&self, _: &str) -> Result<(), FsError> {
            Err(FsError::NotFound)
        }
        fn mkdir(&self, _: &str) -> Result<(), FsError> {
            Ok(())
        }
        fn rmdir(&self, _: &str) -> Result<(), FsError> {
            Err(FsError::NotEmpty)
        }
        fn list(&self, _: &str, entry: &mut dyn FnMut(&str, bool) -> bool) -> Result<(), FsError> {
            for (name, is_directory) in [("a", true), ("b", false), ("c", false)] {
                if !entry(name, is_directory) {
                    break;
                }
            }
            Ok(())
        }
    }

    unsafe extern "C" fn collect(context: *mut c_void, name: *const c_char, is_dir: i32) -> i32 {
        let names = unsafe { &mut *(context as *mut Vec<String>) };
        let name = unsafe { CStr::from_ptr(name) }.to_str().unwrap();
        names.push(format!("{name}:{is_dir}"));
        i32::from(names.len() == 2)
    }

    #[test]
    fn trampolines_map_results_and_codes() {
        let file_system: Box<dyn FileSystem> = Box::new(Recorder::default());
        let context = Box::into_raw(Box::new(file_system));
        let t = table(context.cast());
        let c = t.context;
        let p = CString::new("C:\\root\\data\\photos\\x.jpg").unwrap();
        unsafe {
            let mut handle = 0;
            assert_eq!((t.open)(c, p.as_ptr(), 2, &mut handle), 0);
            assert_eq!(handle, 42);
            assert_eq!((t.open)(c, p.as_ptr(), 32 | 2, &mut handle), ERROR_EXISTS);

            let mut buffer = [0u8; 4];
            assert_eq!((t.read_at)(c, 42, buffer.as_mut_ptr().cast(), 4, 5), 1);
            assert_eq!(buffer[0], 5);
            assert_eq!(
                (t.read_at)(c, 42, buffer.as_mut_ptr().cast(), 4, -1),
                ERROR as i64
            );
            assert_eq!((t.write_at)(c, 42, buffer.as_ptr().cast(), 4, 0), 4);

            let mut size = 0;
            assert_eq!((t.size)(c, 42, &mut size), 0);
            assert_eq!(size, 7);
            assert_eq!((t.truncate)(c, 42, 1), ERROR_NO_SPACE);
            assert_eq!((t.sync)(c, 42), ERROR, "a panic becomes an error");

            let mut stat = FileStat::default();
            assert_eq!((t.stat)(c, p.as_ptr(), &mut stat), 0);
            assert_eq!((stat.size, stat.modification_time_nsec), (3, 9));

            let to = CString::new("C:\\root\\data\\photos\\y.jpg").unwrap();
            assert_eq!((t.rename)(c, p.as_ptr(), to.as_ptr()), 0);
            assert_eq!((t.unlink)(c, p.as_ptr()), ERROR_NOT_FOUND);
            assert_eq!((t.mkdir)(c, p.as_ptr()), 0);
            assert_eq!((t.rmdir)(c, p.as_ptr()), ERROR_NOT_EMPTY);
            assert_eq!((t.open)(c, std::ptr::null(), 2, &mut handle), ERROR);

            let mut names: Vec<String> = Vec::new();
            let list_context = (&mut names as *mut Vec<String>).cast();
            assert_eq!((t.list)(c, p.as_ptr(), collect, list_context), 0);
            assert_eq!(names, ["a:1", "b:0"], "a non-zero answer stops the listing");

            (t.close)(c, 42);
            drop(Box::from_raw(context));
        }
    }

    #[test]
    fn flags_match_the_c_header() {
        let all = OpenFlags::READ | OpenFlags::WRITE | OpenFlags::CREATE_NEW;
        assert_eq!(all.bits(), 2 | 1 | 32);
        assert!(all.contains(OpenFlags::READ | OpenFlags::CREATE_NEW));
        assert!(!all.contains(OpenFlags::TRUNCATE));
        // int32 + padding + 4 × 64-bit, as the C struct TdFileStat
        assert_eq!(std::mem::size_of::<FileStat>(), 40);
    }
}
