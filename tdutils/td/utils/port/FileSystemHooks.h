//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/utils/common.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <functional>

namespace td {

// A file system that owns every path strictly below one directory prefix.
// FileFd, stat() and the functions from port/path.h send such paths here; all other paths go to the OS as before.
// Calls come from any thread, concurrently, and several handles may be open on the same file.
class VirtualFileSystem {
 public:
  VirtualFileSystem() = default;
  VirtualFileSystem(const VirtualFileSystem &) = delete;
  VirtualFileSystem &operator=(const VirtualFileSystem &) = delete;
  VirtualFileSystem(VirtualFileSystem &&) = delete;
  VirtualFileSystem &operator=(VirtualFileSystem &&) = delete;
  virtual ~VirtualFileSystem() = default;

  // flags are FileFd::Flags without Direct; CreateNew must fail if the file exists
  virtual Result<uint64> open(CSlice path, int32 flags) = 0;
  virtual void close(uint64 handle) = 0;

  // a write past the end of the file extends it, leaving the gap filled with zeros
  virtual Result<size_t> read_at(uint64 handle, MutableSlice data, int64 offset) = 0;
  virtual Result<size_t> write_at(uint64 handle, Slice data, int64 offset) = 0;

  virtual Result<int64> get_size(uint64 handle) = 0;
  virtual Status truncate(uint64 handle, int64 size) = 0;
  virtual Status sync(uint64 handle) = 0;

  // a path may end with a directory separator; mtime must not change unless the file was written
  virtual Result<Stat> stat(CSlice path) = 0;

  // replaces an existing target atomically; open handles stay valid
  virtual Status rename(CSlice from, CSlice to) = 0;
  // the name disappears at once; open handles stay valid until closed
  virtual Status unlink(CSlice path) = 0;
  // an existing directory is not an error
  virtual Status mkdir(CSlice path) = 0;
  virtual Status rmdir(CSlice path) = 0;

  // calls entry(name, is_dir) for every entry of the directory until it returns false
  virtual Status list(CSlice path, const std::function<bool(Slice name, bool is_dir)> &entry) = 0;
};

// Installs file_system for every path strictly below prefix. It can be installed once, before any such path is used,
// and stays installed until the process exits.
Status set_virtual_file_system(CSlice prefix, unique_ptr<VirtualFileSystem> file_system) TD_WARN_UNUSED_RESULT;

// Returns the file system that owns path, or nullptr if path belongs to the OS.
VirtualFileSystem *get_virtual_file_system(Slice path);

}  // namespace td
