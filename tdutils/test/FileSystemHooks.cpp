//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/utils/BufferedFd.h"
#include "td/utils/common.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/FileSystemHooks.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Status.h"
#include "td/utils/tests.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace {

// a plain in-memory file system with the semantics TdFileSystem asks for
class MemoryFileSystem final : public td::VirtualFileSystem {
 public:
  explicit MemoryFileSystem(td::string root) : root_(std::move(root)) {
    dirs_.insert(root_);
  }

  td::Result<td::uint64> open(td::CSlice path, td::int32 flags) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto name = normalize(path);
    auto it = files_.find(name);
    std::shared_ptr<File> file;
    if (it != files_.end()) {
      if (flags & td::FileFd::CreateNew) {
        return td::Status::Error(-3, "exists");
      }
      file = it->second;
      if (flags & td::FileFd::Truncate) {
        file->data.clear();
        file->mtime = ++clock_;
      }
    } else {
      if (dirs_.count(name) != 0) {
        return td::Status::Error(-6, "is a directory");
      }
      if ((flags & (td::FileFd::Create | td::FileFd::CreateNew)) == 0) {
        return td::Status::Error(-2, "not found");
      }
      if (dirs_.count(parent(name)) == 0) {
        return td::Status::Error(-2, "no parent directory");
      }
      file = std::make_shared<File>();
      file->mtime = ++clock_;
      files_[name] = file;
    }
    auto handle = next_handle_++;
    handles_[handle] = file;
    return handle;
  }

  void close(td::uint64 handle) final {
    std::lock_guard<std::mutex> guard(mutex_);
    CHECK(handles_.erase(handle) == 1);
  }

  td::Result<size_t> read_at(td::uint64 handle, td::MutableSlice data, td::int64 offset) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto &file = *handles_.at(handle);
    auto from = static_cast<size_t>(offset);
    if (from >= file.data.size()) {
      return static_cast<size_t>(0);
    }
    auto size = td::min(data.size(), file.data.size() - from);
    data.copy_from(td::Slice(file.data).substr(from, size));
    return size;
  }

  td::Result<size_t> write_at(td::uint64 handle, td::Slice data, td::int64 offset) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto &file = *handles_.at(handle);
    auto from = static_cast<size_t>(offset);
    if (file.data.size() < from + data.size()) {
      file.data.resize(from + data.size(), '\0');
    }
    td::MutableSlice(file.data).substr(from).copy_from(data);
    file.mtime = ++clock_;
    return data.size();
  }

  td::Result<td::int64> get_size(td::uint64 handle) final {
    std::lock_guard<std::mutex> guard(mutex_);
    return static_cast<td::int64>(handles_.at(handle)->data.size());
  }

  td::Status truncate(td::uint64 handle, td::int64 size) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto &file = *handles_.at(handle);
    file.data.resize(static_cast<size_t>(size), '\0');
    file.mtime = ++clock_;
    return td::Status::OK();
  }

  td::Status sync(td::uint64 handle) final {
    return td::Status::OK();
  }

  td::Result<td::Stat> stat(td::CSlice path) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto name = normalize(path);
    td::Stat res;
    res.is_symbolic_link_ = false;
    if (dirs_.count(name) != 0) {
      res.is_dir_ = true;
      res.is_reg_ = false;
      res.size_ = 0;
      res.real_size_ = 0;
      res.atime_nsec_ = 0;
      res.mtime_nsec_ = 0;
      return res;
    }
    auto it = files_.find(name);
    if (it == files_.end()) {
      return td::Status::Error(-2, "not found");
    }
    res.is_dir_ = false;
    res.is_reg_ = true;
    res.size_ = static_cast<td::int64>(it->second->data.size());
    res.real_size_ = (res.size_ + 16383) / 16384 * 16384;
    res.atime_nsec_ = it->second->mtime;
    res.mtime_nsec_ = it->second->mtime;
    return res;
  }

  td::Status rename(td::CSlice from, td::CSlice to) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = files_.find(normalize(from));
    if (it == files_.end()) {
      return td::Status::Error(-2, "not found");
    }
    auto file = it->second;
    files_.erase(it);
    files_[normalize(to)] = file;
    return td::Status::OK();
  }

  td::Status unlink(td::CSlice path) final {
    std::lock_guard<std::mutex> guard(mutex_);
    return files_.erase(normalize(path)) == 1 ? td::Status::OK() : td::Status::Error(-2, "not found");
  }

  td::Status mkdir(td::CSlice path) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto name = normalize(path);
    if (dirs_.count(parent(name)) == 0) {
      return td::Status::Error(-2, "no parent directory");
    }
    dirs_.insert(name);
    return td::Status::OK();
  }

  td::Status rmdir(td::CSlice path) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto name = normalize(path);
    for (auto &file : files_) {
      if (parent(file.first) == name) {
        return td::Status::Error(-4, "not empty");
      }
    }
    return dirs_.erase(name) == 1 ? td::Status::OK() : td::Status::Error(-2, "not found");
  }

  td::Status list(td::CSlice path, const std::function<bool(td::Slice name, bool is_dir)> &entry) final {
    std::lock_guard<std::mutex> guard(mutex_);
    auto name = normalize(path);
    if (dirs_.count(name) == 0) {
      return td::Status::Error(-2, "not found");
    }
    for (auto &dir : dirs_) {
      if (dir != name && parent(dir) == name && !entry(base_name(dir), true)) {
        return td::Status::OK();
      }
    }
    for (auto &file : files_) {
      if (parent(file.first) == name && !entry(base_name(file.first), false)) {
        return td::Status::OK();
      }
    }
    return td::Status::OK();
  }

  size_t open_handle_count() {
    std::lock_guard<std::mutex> guard(mutex_);
    return handles_.size();
  }

 private:
  struct File {
    td::string data;
    td::uint64 mtime = 0;
  };

  static bool is_separator(char c) {
    return c == '/' || c == '\\';
  }

  static td::string normalize(td::Slice path) {
    while (!path.empty() && is_separator(path.back())) {
      path.remove_suffix(1);
    }
    return path.str();
  }

  static td::string parent(const td::string &path) {
    auto pos = path.find_last_of("/\\");
    return pos == td::string::npos ? td::string() : path.substr(0, pos);
  }

  static td::string base_name(const td::string &path) {
    auto pos = path.find_last_of("/\\");
    return pos == td::string::npos ? path : path.substr(pos + 1);
  }

  std::mutex mutex_;
  td::string root_;
  std::set<td::string> dirs_;
  std::map<td::string, std::shared_ptr<File>> files_;
  std::map<td::uint64, std::shared_ptr<File>> handles_;
  td::uint64 next_handle_ = 1;
  td::uint64 clock_ = 1;
};

const td::string &root() {
  // the prefix directory itself stays on the OS and must be absolute
  static const td::string root = [] {
    td::mkdir("virtual_file_system_test_root").ensure();
    auto path = td::realpath("virtual_file_system_test_root").move_as_ok();
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
      path.pop_back();
    }
    return path;
  }();
  return root;
}

MemoryFileSystem &memory_file_system() {
  static MemoryFileSystem *file_system = [] {
    auto owned = td::make_unique<MemoryFileSystem>(root());
    auto *raw = owned.get();
    td::set_virtual_file_system(root(), std::move(owned)).ensure();
    return raw;
  }();
  return *file_system;
}

td::string path(td::Slice name) {
  memory_file_system();
  return PSTRING() << root() << TD_DIR_SLASH << name;
}

}  // namespace

TEST(VirtualFileSystem, prefix) {
  memory_file_system();
  ASSERT_TRUE(td::get_virtual_file_system(root()) == nullptr);
  ASSERT_TRUE(td::get_virtual_file_system(root() + TD_DIR_SLASH) == nullptr);
  ASSERT_TRUE(td::get_virtual_file_system(root() + "x" + TD_DIR_SLASH + "a") == nullptr);
  ASSERT_TRUE(td::get_virtual_file_system(path("a")) == &memory_file_system());
  ASSERT_TRUE(td::get_virtual_file_system(root() + "/a") == &memory_file_system());
  ASSERT_TRUE(td::get_virtual_file_system("test_dir") == nullptr);
  ASSERT_TRUE(td::set_virtual_file_system("other_root", td::make_unique<MemoryFileSystem>("other_root")).is_error());
  ASSERT_TRUE(td::set_virtual_file_system(root() + "2", td::make_unique<MemoryFileSystem>("x")).is_error());
}

TEST(VirtualFileSystem, create_new) {
  auto dir = path("create_new");
  auto name = PSTRING() << dir << TD_DIR_SLASH << "file";
  ASSERT_TRUE(td::FileFd::open(name, td::FileFd::Read).is_error());
  ASSERT_TRUE(td::FileFd::open(name, td::FileFd::Read | td::FileFd::Write | td::FileFd::CreateNew).is_error());
  td::mkdir(dir).ensure();
  td::mkdir(dir).ensure();
  auto fd = td::FileFd::open(name, td::FileFd::Read | td::FileFd::Write | td::FileFd::CreateNew).move_as_ok();
  ASSERT_TRUE(fd.is_virtual());
  ASSERT_TRUE(td::FileFd::open(name, td::FileFd::Read | td::FileFd::Write | td::FileFd::CreateNew).is_error());
  fd.close();
  ASSERT_EQ(0u, memory_file_system().open_handle_count());
}

TEST(VirtualFileSystem, sparse_writes) {
  auto dir = path("sparse");
  td::mkdir(dir).ensure();
  auto name = PSTRING() << dir << TD_DIR_SLASH << "file";
  auto fd = td::FileFd::open(name, td::FileFd::Read | td::FileFd::Write | td::FileFd::Create).move_as_ok();
  ASSERT_EQ(3u, fd.pwrite("abc", 10).move_as_ok());
  ASSERT_EQ(13, fd.get_size().move_as_ok());
  td::string buf(13, 'x');
  ASSERT_EQ(13u, fd.pread(buf, 0).move_as_ok());
  ASSERT_EQ(td::string(10, '\0') + "abc", buf);
  ASSERT_EQ(0u, fd.pread(td::MutableSlice(buf), 13).move_as_ok());
  ASSERT_EQ(td::string(10, '\0') + "abc", td::read_file_str(name).move_as_ok());
  ASSERT_EQ("bc", td::read_file_str(name, 2, 11).move_as_ok());
}

TEST(VirtualFileSystem, positions) {
  auto dir = path("positions");
  td::mkdir(dir).ensure();
  auto name = PSTRING() << dir << TD_DIR_SLASH << "file";
  {
    auto fd = td::FileFd::open(name, td::FileFd::Write | td::FileFd::Create).move_as_ok();
    ASSERT_EQ(5u, fd.write("hello").move_as_ok());
    ASSERT_EQ(6u, fd.write(" world").move_as_ok());
    ASSERT_EQ(1u, fd.pwrite("H", 0).move_as_ok());
    ASSERT_EQ(1u, fd.write("!").move_as_ok());
    fd.seek(5).ensure();
    fd.truncate_to_current_position(5).ensure();
    ASSERT_EQ(5, fd.get_size().move_as_ok());
  }
  {
    auto fd = td::FileFd::open(name, td::FileFd::Write | td::FileFd::Append).move_as_ok();
    ASSERT_EQ(4u, fd.write(", td").move_as_ok());
  }
  auto fd = td::FileFd::open(name, td::FileFd::Read).move_as_ok();
  td::string buf(4, ' ');
  ASSERT_EQ(4u, fd.read(buf).move_as_ok());
  ASSERT_EQ("Hell", buf);
  ASSERT_EQ(4u, fd.read(buf).move_as_ok());
  ASSERT_EQ("o, t", buf);
  ASSERT_EQ(1u, fd.read(buf).move_as_ok());
  ASSERT_EQ(0u, fd.read(buf).move_as_ok());

  auto buffered = td::BufferedFd<td::FileFd>(td::FileFd::open(name, td::FileFd::Read).move_as_ok());
  buffered.get_poll_info().add_flags(td::PollFlags::Read());
  ASSERT_EQ(9u, buffered.flush_read().move_as_ok());
  ASSERT_EQ("Hello, td", buffered.input_buffer().move_as_buffer_slice().as_slice().str());
}

TEST(VirtualFileSystem, stat_path) {
  auto dir = path("stat");
  td::mkdir(dir).ensure();
  auto name = PSTRING() << dir << TD_DIR_SLASH << "file";
  ASSERT_TRUE(td::stat(name).is_error());
  td::write_file(name, "12345").ensure();

  auto dir_stat = td::stat(PSLICE() << dir << TD_DIR_SLASH).move_as_ok();
  ASSERT_TRUE(dir_stat.is_dir_);
  ASSERT_TRUE(!dir_stat.is_reg_);

  auto first = td::stat(name).move_as_ok();
  ASSERT_TRUE(first.is_reg_);
  ASSERT_EQ(5, first.size_);
  ASSERT_EQ(16384, first.real_size_);
  auto second = td::stat(name).move_as_ok();
  ASSERT_EQ(first.mtime_nsec_, second.mtime_nsec_);

  td::write_file(name, "123456").ensure();
  auto third = td::stat(name).move_as_ok();
  ASSERT_TRUE(third.mtime_nsec_ != first.mtime_nsec_);
  ASSERT_EQ(6, third.size_);

  auto fd = td::FileFd::open(name, td::FileFd::Read).move_as_ok();
  ASSERT_EQ(6, fd.stat().move_as_ok().size_);
  ASSERT_EQ(6, fd.get_real_size().move_as_ok());
}

TEST(VirtualFileSystem, rename_and_unlink) {
  auto dir = path("rename");
  td::mkdir(dir).ensure();
  auto temp = PSTRING() << dir << TD_DIR_SLASH << "temp";
  auto final_name = PSTRING() << dir << TD_DIR_SLASH << "final";
  td::write_file(temp, "data").ensure();
  // a placeholder reserves the final name, as create_from_temp does
  td::FileFd::open(final_name, td::FileFd::Read | td::FileFd::Write | td::FileFd::CreateNew).move_as_ok().close();
  td::rename(temp, final_name).ensure();
  ASSERT_TRUE(td::stat(temp).is_error());
  ASSERT_EQ("data", td::read_file_str(final_name).move_as_ok());

  ASSERT_TRUE(td::rename(final_name, "virtual_file_system_test_outside").is_error());
  ASSERT_TRUE(td::stat(final_name).is_ok());

  auto fd = td::FileFd::open(final_name, td::FileFd::Read).move_as_ok();
  td::unlink(final_name).ensure();
  ASSERT_TRUE(td::stat(final_name).is_error());
  ASSERT_TRUE(td::unlink(final_name).is_error());
  td::string buf(4, ' ');
  ASSERT_EQ(4u, fd.pread(buf, 0).move_as_ok());
  ASSERT_EQ("data", buf);

  ASSERT_TRUE(td::rmdir(dir).is_ok());
  ASSERT_TRUE(td::stat(dir).is_error());
}

TEST(VirtualFileSystem, walk) {
  auto dir = path("walk");
  td::mkpath(PSLICE() << dir << TD_DIR_SLASH << "a" << TD_DIR_SLASH << "b" << TD_DIR_SLASH).ensure();
  td::mkdir(PSLICE() << dir << TD_DIR_SLASH << "c").ensure();
  td::write_file(PSLICE() << dir << TD_DIR_SLASH << "a" << TD_DIR_SLASH << "b" << TD_DIR_SLASH << "f1", "1").ensure();
  td::write_file(PSLICE() << dir << TD_DIR_SLASH << "c" << TD_DIR_SLASH << "f2", "2").ensure();
  td::write_file(PSLICE() << dir << TD_DIR_SLASH << ".hidden", "3").ensure();

  int enter_count = 0;
  int exit_count = 0;
  std::set<td::string> files;
  td::WalkPath::run(PSLICE() << dir << TD_DIR_SLASH, [&](td::CSlice name, td::WalkPath::Type type) {
    switch (type) {
      case td::WalkPath::Type::EnterDir:
        enter_count++;
        break;
      case td::WalkPath::Type::ExitDir:
        exit_count++;
        break;
      case td::WalkPath::Type::RegularFile:
        files.insert(name.str());
        break;
      case td::WalkPath::Type::Symlink:
        UNREACHABLE();
    }
  }).ensure();
  ASSERT_EQ(4, enter_count);
  ASSERT_EQ(4, exit_count);
  ASSERT_EQ(2u, files.size());
  ASSERT_TRUE(files.count(PSTRING() << dir << TD_DIR_SLASH << "a" << TD_DIR_SLASH << "b" << TD_DIR_SLASH << "f1") == 1);
  ASSERT_TRUE(files.count(PSTRING() << dir << TD_DIR_SLASH << "c" << TD_DIR_SLASH << "f2") == 1);

  // as over OS directories, a directory that can't be listed reports nothing
  int reported = 0;
  ASSERT_TRUE(td::WalkPath::run(PSLICE() << dir << TD_DIR_SLASH << "missing",
                                [&](td::CSlice, td::WalkPath::Type) { reported++; })
                  .is_error());
  ASSERT_EQ(0, reported);

  int visited = 0;
  td::WalkPath::run(dir, [&](td::CSlice name, td::WalkPath::Type type) {
    visited++;
    return type == td::WalkPath::Type::RegularFile ? td::WalkPath::Action::Abort : td::WalkPath::Action::Continue;
  }).ensure();
  ASSERT_EQ(4, visited);
}

TEST(VirtualFileSystem, locks) {
  auto dir = path("locks");
  td::mkdir(dir).ensure();
  auto name = PSTRING() << dir << TD_DIR_SLASH << "file";
  auto fd = td::FileFd::open(name, td::FileFd::Write | td::FileFd::Create).move_as_ok();
  fd.lock(td::FileFd::LockFlags::Write, name, 1).ensure();
  auto fd2 = td::FileFd::open(name, td::FileFd::Write).move_as_ok();
  ASSERT_TRUE(fd2.lock(td::FileFd::LockFlags::Write, name, 1).is_error());
  fd.lock(td::FileFd::LockFlags::Unlock, name, 1).ensure();
  fd2.lock(td::FileFd::LockFlags::Write, name, 1).ensure();
  fd2.lock(td::FileFd::LockFlags::Unlock, name, 1).ensure();
  fd.sync().ensure();
  fd.sync_barrier().ensure();
}
