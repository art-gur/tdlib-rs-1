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

struct Installed {
  string prefix;
  VirtualFileSystem *file_system;
};

// Never freed: TDLib threads may still open files while static objects are destroyed at exit.
std::atomic<Installed *> installed{nullptr};
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

bool is_absolute(Slice path) {
#if TD_PORT_WINDOWS
  return (path.size() >= 3 && path[1] == ':' && is_separator(path[2])) ||
         (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1]));
#else
  return !path.empty() && path[0] == '/';
#endif
}

}  // namespace

Status set_virtual_file_system(CSlice prefix, unique_ptr<VirtualFileSystem> file_system) {
  if (file_system == nullptr) {
    return Status::Error("File system must be non-empty");
  }
  // a relative prefix would never match TDLib's absolute paths, and the files would silently stay on disk
  if (!is_absolute(prefix)) {
    return Status::Error("File system prefix must be an absolute path");
  }
  std::lock_guard<std::mutex> guard(set_mutex);
  if (installed.load(std::memory_order_acquire) != nullptr) {
    return Status::Error("File system is already installed");
  }
  auto *entry = new Installed{prefix.str(), file_system.release()};
  if (!is_separator(entry->prefix.back())) {
    entry->prefix += TD_DIR_SLASH;
  }
  installed.store(entry, std::memory_order_release);
  return Status::OK();
}

VirtualFileSystem *get_virtual_file_system(Slice path) {
  const auto *entry = installed.load(std::memory_order_acquire);
  if (entry == nullptr) {
    return nullptr;
  }
  const string &prefix = entry->prefix;
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
      return entry->file_system;
    }
  }
  return nullptr;
}

}  // namespace td
