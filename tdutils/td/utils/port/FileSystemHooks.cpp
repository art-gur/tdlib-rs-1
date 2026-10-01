//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/utils/port/FileSystemHooks.h"

#include "td/utils/misc.h"

#include <atomic>
#include <mutex>

namespace td {

namespace {

// Never freed: handles may still be closed while static objects are destroyed at exit.
std::atomic<VirtualFileSystem *> virtual_file_system{nullptr};
string virtual_file_system_prefix;
std::mutex set_mutex;

bool is_separator(char c) {
  return c == '/' || c == '\\';
}

bool is_same_char(char a, char b) {
  if (is_separator(a) && is_separator(b)) {
    return true;
  }
#if TD_PORT_WINDOWS
  return to_lower(a) == to_lower(b);
#else
  return a == b;
#endif
}

}  // namespace

Status set_virtual_file_system(CSlice prefix, unique_ptr<VirtualFileSystem> file_system) {
  if (file_system == nullptr) {
    return Status::Error("File system must be non-empty");
  }
  if (prefix.empty()) {
    return Status::Error("File system prefix must be non-empty");
  }
  std::lock_guard<std::mutex> guard(set_mutex);
  if (virtual_file_system.load(std::memory_order_acquire) != nullptr) {
    return Status::Error("File system is already installed");
  }
  virtual_file_system_prefix = prefix.str();
  if (!is_separator(virtual_file_system_prefix.back())) {
    virtual_file_system_prefix += TD_DIR_SLASH;
  }
  virtual_file_system.store(file_system.release(), std::memory_order_release);
  return Status::OK();
}

VirtualFileSystem *get_virtual_file_system(Slice path) {
  auto *file_system = virtual_file_system.load(std::memory_order_acquire);
  if (file_system == nullptr) {
    return nullptr;
  }
  const string &prefix = virtual_file_system_prefix;
  if (path.size() <= prefix.size()) {
    return nullptr;
  }
  for (size_t i = 0; i < prefix.size(); i++) {
    if (!is_same_char(path[i], prefix[i])) {
      return nullptr;
    }
  }
  // the prefix directory itself belongs to the OS, so that it can be created and checked as usual
  for (size_t i = prefix.size(); i < path.size(); i++) {
    if (!is_separator(path[i])) {
      return file_system;
    }
  }
  return nullptr;
}

}  // namespace td
