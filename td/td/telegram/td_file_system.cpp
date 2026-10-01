//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2026
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/td_file_system.h"

#include "td/utils/common.h"
#include "td/utils/port/FileFd.h"
#include "td/utils/port/FileSystemHooks.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Slice.h"
#include "td/utils/SliceBuilder.h"
#include "td/utils/Status.h"

#include <functional>

namespace td {

namespace {

static_assert(FileFd::Write == TD_FILE_SYSTEM_WRITE && FileFd::Read == TD_FILE_SYSTEM_READ &&
                  FileFd::Truncate == TD_FILE_SYSTEM_TRUNCATE && FileFd::Create == TD_FILE_SYSTEM_CREATE &&
                  FileFd::Append == TD_FILE_SYSTEM_APPEND && FileFd::CreateNew == TD_FILE_SYSTEM_CREATE_NEW,
              "TdFileSystem flags must match FileFd flags");

Slice error_name(int64 code) {
  switch (code) {
    case TD_FILE_SYSTEM_ERROR_NOT_FOUND:
      return Slice("not found");
    case TD_FILE_SYSTEM_ERROR_EXISTS:
      return Slice("already exists");
    case TD_FILE_SYSTEM_ERROR_NOT_EMPTY:
      return Slice("directory isn't empty");
    case TD_FILE_SYSTEM_ERROR_NO_SPACE:
      return Slice("no space left");
    case TD_FILE_SYSTEM_ERROR_IS_DIRECTORY:
      return Slice("is a directory");
    case TD_FILE_SYSTEM_ERROR_NOT_DIRECTORY:
      return Slice("isn't a directory");
    default:
      return Slice("failed");
  }
}

Status to_status(int64 code, Slice action, Slice path) {
  if (code >= 0) {
    return Status::OK();
  }
  return Status::Error(static_cast<int>(code), PSLICE() << action << " \"" << path << "\": " << error_name(code));
}

class CallbackFileSystem final : public VirtualFileSystem {
 public:
  explicit CallbackFileSystem(const TdFileSystem &callbacks) : callbacks_(callbacks) {
  }

  Result<uint64> open(CSlice path, int32 flags) final {
    uint64_t handle = 0;
    TRY_STATUS(to_status(callbacks_.open(callbacks_.context, path.c_str(), flags, &handle), "Can't open", path));
    return static_cast<uint64>(handle);
  }

  void close(uint64 handle) final {
    callbacks_.close(callbacks_.context, handle);
  }

  Result<size_t> read_at(uint64 handle, MutableSlice data, int64 offset) final {
    auto result = callbacks_.read_at(callbacks_.context, handle, data.data(), data.size(), offset);
    TRY_STATUS(to_status(result, "Can't read", PSLICE() << "handle " << handle));
    if (static_cast<uint64>(result) > data.size()) {
      return Status::Error("File system read more bytes than asked");
    }
    return static_cast<size_t>(result);
  }

  Result<size_t> write_at(uint64 handle, Slice data, int64 offset) final {
    auto result = callbacks_.write_at(callbacks_.context, handle, data.data(), data.size(), offset);
    TRY_STATUS(to_status(result, "Can't write", PSLICE() << "handle " << handle));
    if (static_cast<uint64>(result) > data.size()) {
      return Status::Error("File system wrote more bytes than given");
    }
    return static_cast<size_t>(result);
  }

  Result<int64> get_size(uint64 handle) final {
    int64_t size = 0;
    TRY_STATUS(to_status(callbacks_.size(callbacks_.context, handle, &size), "Can't get size of",
                         PSLICE() << "handle " << handle));
    return static_cast<int64>(size);
  }

  Status truncate(uint64 handle, int64 size) final {
    return to_status(callbacks_.truncate(callbacks_.context, handle, size), "Can't truncate",
                     PSLICE() << "handle " << handle);
  }

  Status sync(uint64 handle) final {
    return to_status(callbacks_.sync(callbacks_.context, handle), "Can't sync", PSLICE() << "handle " << handle);
  }

  Result<Stat> stat(CSlice path) final {
    TdFileStat file_stat{};
    TRY_STATUS(to_status(callbacks_.stat(callbacks_.context, path.c_str(), &file_stat), "Can't stat", path));
    Stat res;
    res.is_dir_ = file_stat.is_directory != 0;
    res.is_reg_ = !res.is_dir_;
    res.is_symbolic_link_ = false;
    res.size_ = file_stat.size;
    res.real_size_ = file_stat.allocated_size;
    res.atime_nsec_ = file_stat.access_time_nsec;
    res.mtime_nsec_ = file_stat.modification_time_nsec;
    return res;
  }

  Status rename(CSlice from, CSlice to) final {
    return to_status(callbacks_.rename(callbacks_.context, from.c_str(), to.c_str()), "Can't rename", from);
  }

  Status unlink(CSlice path) final {
    return to_status(callbacks_.unlink(callbacks_.context, path.c_str()), "Can't unlink", path);
  }

  Status mkdir(CSlice path) final {
    return to_status(callbacks_.mkdir(callbacks_.context, path.c_str()), "Can't create directory", path);
  }

  Status rmdir(CSlice path) final {
    return to_status(callbacks_.rmdir(callbacks_.context, path.c_str()), "Can't delete directory", path);
  }

  Status list(CSlice path, const std::function<bool(Slice name, bool is_dir)> &entry) final {
    auto on_entry = [](void *list_context, const char *name, int32_t is_directory) -> int32_t {
      const auto &entry = *static_cast<const std::function<bool(Slice, bool)> *>(list_context);
      return entry(Slice(name), is_directory != 0) ? 0 : 1;
    };
    auto list_context = const_cast<void *>(static_cast<const void *>(&entry));
    return to_status(callbacks_.list(callbacks_.context, path.c_str(), on_entry, list_context), "Can't list", path);
  }

 private:
  TdFileSystem callbacks_;
};

bool is_complete(const TdFileSystem &file_system) {
  return file_system.version == TD_FILE_SYSTEM_VERSION && file_system.open != nullptr &&
         file_system.close != nullptr && file_system.read_at != nullptr && file_system.write_at != nullptr &&
         file_system.size != nullptr && file_system.truncate != nullptr && file_system.sync != nullptr &&
         file_system.stat != nullptr && file_system.rename != nullptr && file_system.unlink != nullptr &&
         file_system.mkdir != nullptr && file_system.rmdir != nullptr && file_system.list != nullptr;
}

}  // namespace

}  // namespace td

int td_set_file_system(const char *prefix, const TdFileSystem *file_system) {
  if (prefix == nullptr || file_system == nullptr || !td::is_complete(*file_system)) {
    return 0;
  }
  auto status = td::set_virtual_file_system(td::CSlice(prefix), td::make_unique<td::CallbackFileSystem>(*file_system));
  return status.is_ok() ? 1 : 0;
}
