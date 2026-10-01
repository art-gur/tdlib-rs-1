//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

/**
 * \file
 * C interface for keeping TDLib's files in an application-provided file system.
 * Every file and directory strictly below one path prefix is read, written, listed and deleted through
 * the callbacks of a TdFileSystem instead of the OS. Paths outside the prefix, including the database
 * and the binlog when they live elsewhere, go to the OS as before.
 */

#include "td/telegram/tdjson_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The version of struct TdFileSystem described by this header. */
#define TD_FILE_SYSTEM_VERSION 1

/** Flags of TdFileSystem.open; they can be combined. */
#define TD_FILE_SYSTEM_WRITE 1
#define TD_FILE_SYSTEM_READ 2
#define TD_FILE_SYSTEM_TRUNCATE 4
#define TD_FILE_SYSTEM_CREATE 8
#define TD_FILE_SYSTEM_APPEND 16
/** Creates the file and fails with TD_FILE_SYSTEM_ERROR_EXISTS if it already exists. */
#define TD_FILE_SYSTEM_CREATE_NEW 32

/** Results of the callbacks: 0 on success, or one of these negative codes. */
#define TD_FILE_SYSTEM_ERROR -1
#define TD_FILE_SYSTEM_ERROR_NOT_FOUND -2
#define TD_FILE_SYSTEM_ERROR_EXISTS -3
#define TD_FILE_SYSTEM_ERROR_NOT_EMPTY -4
#define TD_FILE_SYSTEM_ERROR_NO_SPACE -5
#define TD_FILE_SYSTEM_ERROR_IS_DIRECTORY -6
#define TD_FILE_SYSTEM_ERROR_NOT_DIRECTORY -7

/** What TdFileSystem.stat reports about a file or a directory. */
typedef struct TdFileStat {
  /** 1 for a directory, 0 for a regular file. */
  int32_t is_directory;
  /** The length of a file in bytes. */
  int64_t size;
  /** The bytes a file takes in storage; shown in storage statistics. */
  int64_t allocated_size;
  /** The time of the last access, in nanoseconds since the Unix epoch; orders the removal of old files. */
  uint64_t access_time_nsec;
  /** The time of the last write, in nanoseconds since the Unix epoch. It must not change unless the file is written. */
  uint64_t modification_time_nsec;
} TdFileStat;

/**
 * Receives one entry of a listed directory.
 * \param[in]  list_context The list_context passed to TdFileSystem.list.
 * \param[in]  name Null-terminated name of the entry without the directory.
 * \param[in]  is_directory 1 for a directory, 0 for a regular file.
 * \return 0 to continue the listing, or any other value to stop it.
 */
typedef int32_t (*td_file_system_entry_ptr)(void *list_context, const char *name, int32_t is_directory);

/**
 * The callbacks of a file system. Every path is UTF-8, null-terminated, absolute and starts with the prefix
 * passed to td_set_file_system; a directory path may end with a separator.
 * Callbacks are called from several threads at once, and several handles may be open on the same file.
 */
typedef struct TdFileSystem {
  /** Must be TD_FILE_SYSTEM_VERSION. */
  uint32_t version;
  /** Passed to every callback; must stay valid until the process exits. */
  void *context;

  /** Opens a file with TD_FILE_SYSTEM_* flags and stores its handle. */
  int32_t (*open)(void *context, const char *path, int32_t flags, uint64_t *handle);
  /** Closes a handle. A file unlinked while open is freed after its last handle is closed. */
  void (*close)(void *context, uint64_t handle);
  /** Reads size bytes at offset, fewer only at the end of the file; returns the number of bytes read, or an error. */
  int64_t (*read_at)(void *context, uint64_t handle, void *buffer, size_t size, int64_t offset);
  /** Writes all size bytes at offset or fails; returns size. A write past the end extends the file; reading the gap
   *  may return zeros or fail, as TDLib reads only what it has written. */
  int64_t (*write_at)(void *context, uint64_t handle, const void *data, size_t size, int64_t offset);
  /** Stores the length of an open file. */
  int32_t (*size)(void *context, uint64_t handle, int64_t *size);
  /** Changes the length of an open file. */
  int32_t (*truncate)(void *context, uint64_t handle, int64_t size);
  /** Makes the data written through the handle durable. */
  int32_t (*sync)(void *context, uint64_t handle);
  /** Describes a file or a directory by its path. */
  int32_t (*stat)(void *context, const char *path, TdFileStat *stat);
  /** Renames a file, replacing an existing target atomically; open handles stay valid. */
  int32_t (*rename)(void *context, const char *from, const char *to);
  /** Removes the name of a file at once; open handles stay valid. */
  int32_t (*unlink)(void *context, const char *path);
  /** Creates a directory; an existing directory is not an error. */
  int32_t (*mkdir)(void *context, const char *path);
  /** Removes an empty directory. */
  int32_t (*rmdir)(void *context, const char *path);
  /** Calls entry for every entry of a directory until it returns a non-zero value. */
  int32_t (*list)(void *context, const char *path, td_file_system_entry_ptr entry, void *list_context);
} TdFileSystem;

/**
 * Installs a file system for every path strictly below a prefix. The prefix directory itself stays on the OS.
 * The database_directory of a client must be outside the prefix: SQLite opens its files itself.
 * It must be called before the first client is created and can be called only once; the file system stays installed
 * until the process exits. While a client's files_directory is below the prefix, TDLib keeps all its files there,
 * as if the option store_all_files_in_files_directory were set.
 *
 * \param[in]  prefix Null-terminated UTF-8 absolute path of the directory, in the form files_directory takes after
 *                    TDLib's realpath; on Windows compared without regard to ASCII case and to the kind of the
 *                    separators.
 * \param[in]  file_system The callbacks; the struct is copied.
 * \return 1 on success, or 0 if the file system is invalid or one is already installed.
 */
TDJSON_EXPORT int td_set_file_system(const char *prefix, const TdFileSystem *file_system);

#ifdef __cplusplus
}  // extern "C"
#endif
