#pragma once

#include <wasi/0.2.3/filesystem.hpp>
#include <wasi/0.2.3/io_host.hpp>

#include <psio1/structural.hpp>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wasi_host {

struct descriptor_data
{
   RealFd          fd;
   descriptor_type type  = descriptor_type::unknown;
   std::string     path;
};

struct dir_stream_data
{
   DIR*        dir = nullptr;
   std::string path;
};

inline filesystem_error_code errno_to_fs_error()
{
   switch (errno)
   {
      case EACCES:       return filesystem_error_code::access;
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
                         return filesystem_error_code::would_block;
      case EBADF:        return filesystem_error_code::bad_descriptor;
      case EBUSY:        return filesystem_error_code::busy;
      case EEXIST:       return filesystem_error_code::exist;
      case EFBIG:        return filesystem_error_code::file_too_large;
      case EINTR:        return filesystem_error_code::interrupted;
      case EINVAL:       return filesystem_error_code::invalid;
      case EIO:          return filesystem_error_code::io;
      case EISDIR:       return filesystem_error_code::is_directory;
      case ELOOP:        return filesystem_error_code::loop;
      case EMLINK:       return filesystem_error_code::too_many_links;
      case ENAMETOOLONG: return filesystem_error_code::name_too_long;
      case ENODEV:       return filesystem_error_code::no_device;
      case ENOENT:       return filesystem_error_code::no_entry;
      case ENOMEM:       return filesystem_error_code::insufficient_memory;
      case ENOSPC:       return filesystem_error_code::insufficient_space;
      case ENOTDIR:      return filesystem_error_code::not_directory;
      case ENOTEMPTY:    return filesystem_error_code::not_empty;
      case ENOTSUP:      return filesystem_error_code::unsupported;
      case EPERM:        return filesystem_error_code::not_permitted;
      case EPIPE:        return filesystem_error_code::pipe;
      case EROFS:        return filesystem_error_code::read_only;
      case ESPIPE:       return filesystem_error_code::invalid_seek;
      case ETXTBSY:      return filesystem_error_code::text_file_busy;
      case EXDEV:        return filesystem_error_code::cross_device;
      default:           return filesystem_error_code::io;
   }
}

inline descriptor_type stat_mode_to_type(mode_t mode)
{
   if (S_ISREG(mode))  return descriptor_type::regular_file;
   if (S_ISDIR(mode))  return descriptor_type::directory;
   if (S_ISLNK(mode))  return descriptor_type::symbolic_link;
   if (S_ISBLK(mode))  return descriptor_type::block_device;
   if (S_ISCHR(mode))  return descriptor_type::character_device;
   if (S_ISFIFO(mode)) return descriptor_type::fifo;
   if (S_ISSOCK(mode)) return descriptor_type::socket;
   return descriptor_type::unknown;
}

inline descriptor_stat stat_to_descriptor_stat(const struct stat& st)
{
   descriptor_stat ds;
   ds.type           = stat_mode_to_type(st.st_mode);
   ds.link_count_val = static_cast<uint64_t>(st.st_nlink);
   ds.size           = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
   ds.data_access_timestamp = datetime{
       static_cast<uint64_t>(st.st_atimespec.tv_sec),
       static_cast<uint32_t>(st.st_atimespec.tv_nsec)};
   ds.data_modification_timestamp = datetime{
       static_cast<uint64_t>(st.st_mtimespec.tv_sec),
       static_cast<uint32_t>(st.st_mtimespec.tv_nsec)};
   ds.status_change_timestamp = datetime{
       static_cast<uint64_t>(st.st_ctimespec.tv_sec),
       static_cast<uint32_t>(st.st_ctimespec.tv_nsec)};
#else
   ds.data_access_timestamp = datetime{
       static_cast<uint64_t>(st.st_atim.tv_sec),
       static_cast<uint32_t>(st.st_atim.tv_nsec)};
   ds.data_modification_timestamp = datetime{
       static_cast<uint64_t>(st.st_mtim.tv_sec),
       static_cast<uint32_t>(st.st_mtim.tv_nsec)};
   ds.status_change_timestamp = datetime{
       static_cast<uint64_t>(st.st_ctim.tv_sec),
       static_cast<uint32_t>(st.st_ctim.tv_nsec)};
#endif
   return ds;
}

struct WasiFilesystemHost
{
   using fserr = filesystem_error_code;

   WasiIoHost& io;

   psizam::handle_table<descriptor_data, 256>   descriptors{256};
   psizam::handle_table<dir_stream_data, 64>    dir_streams{64};

   std::vector<std::pair<std::string, std::string>> preopens;

   explicit WasiFilesystemHost(WasiIoHost& io_host) : io(io_host) {}

   void add_preopen(const std::string& guest_path, const std::string& host_path)
   {
      preopens.emplace_back(guest_path, host_path);
   }

   // ── wasi:filesystem/preopens ──────────────────────────────────────

   std::vector<std::tuple<psio1::own<descriptor>, std::string>> get_directories()
   {
      std::vector<std::tuple<psio1::own<descriptor>, std::string>> result;
      for (auto& [guest, host] : preopens)
      {
         int fd = ::open(host.c_str(), O_RDONLY | O_DIRECTORY);
         if (fd >= 0)
         {
            auto h = descriptors.create(descriptor_data{
                RealFd{fd}, descriptor_type::directory, host});
            result.emplace_back(psio1::own<descriptor>{h}, guest);
         }
      }
      return result;
   }

   // ── wasi:filesystem/types — descriptor methods ────────────────────

   fs_result<psio1::own<input_stream>> descriptor_read_via_stream(
       psio1::borrow<descriptor> self, filesize offset)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      int fd2 = ::dup(*d->fd);
      if (fd2 < 0)
         return fs_detail::err(errno_to_fs_error());
      ::fcntl(fd2, F_SETFL, ::fcntl(fd2, F_GETFL, 0) | O_NONBLOCK);
      if (offset > 0)
         ::lseek(fd2, static_cast<off_t>(offset), SEEK_SET);
      return fs_detail::ok(io.create_input_stream(RealFd{fd2}));
   }

   fs_result<psio1::own<output_stream>> descriptor_write_via_stream(
       psio1::borrow<descriptor> self, filesize offset)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      int fd2 = ::dup(*d->fd);
      if (fd2 < 0)
         return fs_detail::err(errno_to_fs_error());
      ::fcntl(fd2, F_SETFL, ::fcntl(fd2, F_GETFL, 0) | O_NONBLOCK);
      if (offset > 0)
         ::lseek(fd2, static_cast<off_t>(offset), SEEK_SET);
      return fs_detail::ok(io.create_output_stream(RealFd{fd2}));
   }

   fs_result<psio1::own<output_stream>> descriptor_append_via_stream(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      int fd2 = ::dup(*d->fd);
      if (fd2 < 0)
         return fs_detail::err(errno_to_fs_error());
      ::fcntl(fd2, F_SETFL, ::fcntl(fd2, F_GETFL, 0) | O_NONBLOCK | O_APPEND);
      return fs_detail::ok(io.create_output_stream(RealFd{fd2}));
   }

   fs_result_void descriptor_advise(
       psio1::borrow<descriptor> self, filesize /*offset*/,
       filesize /*length*/, advice /*adv*/)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      return fs_detail::ok();
   }

   fs_result_void descriptor_sync_data(psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
#ifdef __APPLE__
      if (::fsync(*d->fd) < 0)
#else
      if (::fdatasync(*d->fd) < 0)
#endif
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result<descriptor_flags> descriptor_get_flags(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      int fl = ::fcntl(*d->fd, F_GETFL, 0);
      descriptor_flags flags;
      flags.read  = !(fl & O_WRONLY);
      flags.write = (fl & O_WRONLY) || (fl & O_RDWR);
      return fs_detail::ok(flags);
   }

   fs_result<descriptor_type> descriptor_get_type(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      struct stat st;
      if (::fstat(*d->fd, &st) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok(stat_mode_to_type(st.st_mode));
   }

   fs_result_void descriptor_set_size(
       psio1::borrow<descriptor> self, filesize size)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::ftruncate(*d->fd, static_cast<off_t>(size)) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result_void descriptor_set_times(
       psio1::borrow<descriptor> self,
       new_timestamp /*data_access*/, new_timestamp /*data_mod*/)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      return fs_detail::ok();
   }

   fs_result<std::tuple<std::vector<uint8_t>, bool>> descriptor_read(
       psio1::borrow<descriptor> self, filesize length, filesize offset)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      uint64_t cap = length > 65536 ? 65536 : length;
      std::vector<uint8_t> buf(cap);
      ssize_t n = ::pread(*d->fd, buf.data(), cap, static_cast<off_t>(offset));
      if (n < 0)
         return fs_detail::err(errno_to_fs_error());
      buf.resize(static_cast<size_t>(n));
      bool eof = (static_cast<uint64_t>(n) < cap);
      return fs_detail::ok(std::make_tuple(std::move(buf), eof));
   }

   fs_result<filesize> descriptor_write(
       psio1::borrow<descriptor> self,
       std::vector<uint8_t> buffer, filesize offset)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      ssize_t n = ::pwrite(*d->fd, buffer.data(), buffer.size(),
                           static_cast<off_t>(offset));
      if (n < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok(static_cast<filesize>(n));
   }

   fs_result<psio1::own<directory_entry_stream>> descriptor_read_directory(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      int fd2 = ::dup(*d->fd);
      if (fd2 < 0)
         return fs_detail::err(errno_to_fs_error());
      DIR* dir = ::fdopendir(fd2);
      if (!dir)
      {
         ::close(fd2);
         return fs_detail::err(errno_to_fs_error());
      }
      auto h = dir_streams.create(dir_stream_data{dir, d->path});
      return fs_detail::ok(psio1::own<directory_entry_stream>{h});
   }

   fs_result_void descriptor_sync(psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::fsync(*d->fd) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result_void descriptor_create_directory_at(
       psio1::borrow<descriptor> self, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::mkdirat(*d->fd, path.c_str(), 0755) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result<descriptor_stat> descriptor_stat_fn(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      struct stat st;
      if (::fstat(*d->fd, &st) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok(stat_to_descriptor_stat(st));
   }

   fs_result<descriptor_stat> descriptor_stat_at(
       psio1::borrow<descriptor> self, path_flags pf, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      struct stat st;
      int flags = pf.symlink_follow ? 0 : AT_SYMLINK_NOFOLLOW;
      if (::fstatat(*d->fd, path.c_str(), &st, flags) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok(stat_to_descriptor_stat(st));
   }

   fs_result_void descriptor_set_times_at(
       psio1::borrow<descriptor> self, path_flags /*pf*/,
       std::string /*path*/, new_timestamp /*dat*/, new_timestamp /*dmt*/)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      return fs_detail::ok();
   }

   fs_result_void descriptor_link_at(
       psio1::borrow<descriptor> self, path_flags /*pf*/,
       std::string old_path, psio1::borrow<descriptor> new_desc,
       std::string new_path)
   {
      auto* d  = descriptors.get(self.handle);
      auto* nd = descriptors.get(new_desc.handle);
      if (!d || !nd)
         return fs_detail::err(fserr::bad_descriptor);
      if (::linkat(*d->fd, old_path.c_str(), *nd->fd, new_path.c_str(), 0) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result<psio1::own<descriptor>> descriptor_open_at(
       psio1::borrow<descriptor> self, path_flags pf,
       std::string path, open_flags ofl, descriptor_flags dfl)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);

      int flags = O_CLOEXEC;
      if (dfl.read && dfl.write)
         flags |= O_RDWR;
      else if (dfl.write)
         flags |= O_WRONLY;
      else
         flags |= O_RDONLY;
      if (ofl.create)
         flags |= O_CREAT;
      if (ofl.exclusive)
         flags |= O_EXCL;
      if (ofl.truncate)
         flags |= O_TRUNC;
      if (ofl.directory)
         flags |= O_DIRECTORY;
      if (!pf.symlink_follow)
         flags |= O_NOFOLLOW;

      int fd = ::openat(*d->fd, path.c_str(), flags, 0644);
      if (fd < 0)
         return fs_detail::err(errno_to_fs_error());

      struct stat st;
      ::fstat(fd, &st);
      auto h = descriptors.create(descriptor_data{
          RealFd{fd}, stat_mode_to_type(st.st_mode), d->path + "/" + path});
      return fs_detail::ok(psio1::own<descriptor>{h});
   }

   fs_result<std::string> descriptor_readlink_at(
       psio1::borrow<descriptor> self, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      char buf[4096];
      ssize_t n = ::readlinkat(*d->fd, path.c_str(), buf, sizeof(buf));
      if (n < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok(std::string(buf, static_cast<size_t>(n)));
   }

   fs_result_void descriptor_remove_directory_at(
       psio1::borrow<descriptor> self, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::unlinkat(*d->fd, path.c_str(), AT_REMOVEDIR) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result_void descriptor_rename_at(
       psio1::borrow<descriptor> self, std::string old_path,
       psio1::borrow<descriptor> new_desc, std::string new_path)
   {
      auto* d  = descriptors.get(self.handle);
      auto* nd = descriptors.get(new_desc.handle);
      if (!d || !nd)
         return fs_detail::err(fserr::bad_descriptor);
      if (::renameat(*d->fd, old_path.c_str(), *nd->fd, new_path.c_str()) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result_void descriptor_symlink_at(
       psio1::borrow<descriptor> self, std::string old_path,
       std::string new_path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::symlinkat(old_path.c_str(), *d->fd, new_path.c_str()) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   fs_result_void descriptor_unlink_file_at(
       psio1::borrow<descriptor> self, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      if (::unlinkat(*d->fd, path.c_str(), 0) < 0)
         return fs_detail::err(errno_to_fs_error());
      return fs_detail::ok();
   }

   bool descriptor_is_same_object(
       psio1::borrow<descriptor> self, psio1::borrow<descriptor> other)
   {
      auto* a = descriptors.get(self.handle);
      auto* b = descriptors.get(other.handle);
      if (!a || !b)
         return false;
      struct stat sa, sb;
      if (::fstat(*a->fd, &sa) < 0 || ::fstat(*b->fd, &sb) < 0)
         return false;
      return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
   }

   fs_result<metadata_hash_value> descriptor_metadata_hash(
       psio1::borrow<descriptor> self)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      struct stat st;
      if (::fstat(*d->fd, &st) < 0)
         return fs_detail::err(errno_to_fs_error());
      metadata_hash_value h;
      h.lower = static_cast<uint64_t>(st.st_ino);
      h.upper = static_cast<uint64_t>(st.st_dev);
      return fs_detail::ok(h);
   }

   fs_result<metadata_hash_value> descriptor_metadata_hash_at(
       psio1::borrow<descriptor> self, path_flags pf, std::string path)
   {
      auto* d = descriptors.get(self.handle);
      if (!d)
         return fs_detail::err(fserr::bad_descriptor);
      struct stat st;
      int flags = pf.symlink_follow ? 0 : AT_SYMLINK_NOFOLLOW;
      if (::fstatat(*d->fd, path.c_str(), &st, flags) < 0)
         return fs_detail::err(errno_to_fs_error());
      metadata_hash_value h;
      h.lower = static_cast<uint64_t>(st.st_ino);
      h.upper = static_cast<uint64_t>(st.st_dev);
      return fs_detail::ok(h);
   }

   // ── directory-entry-stream ────────────────────────────────────────

   fs_result<std::optional<directory_entry>>
   directory_entry_stream_read_directory_entry(
       psio1::borrow<directory_entry_stream> self)
   {
      auto* ds = dir_streams.get(self.handle);
      if (!ds || !ds->dir)
         return fs_detail::err(fserr::bad_descriptor);

      errno = 0;
      struct dirent* de = ::readdir(ds->dir);
      if (!de)
      {
         if (errno != 0)
            return fs_detail::err(errno_to_fs_error());
         return fs_detail::ok<std::optional<directory_entry>>(std::nullopt);
      }

      directory_entry entry;
      entry.name = de->d_name;
      switch (de->d_type)
      {
         case DT_REG: entry.type = descriptor_type::regular_file; break;
         case DT_DIR: entry.type = descriptor_type::directory; break;
         case DT_LNK: entry.type = descriptor_type::symbolic_link; break;
         case DT_BLK: entry.type = descriptor_type::block_device; break;
         case DT_CHR: entry.type = descriptor_type::character_device; break;
         case DT_FIFO: entry.type = descriptor_type::fifo; break;
         case DT_SOCK: entry.type = descriptor_type::socket; break;
         default:     entry.type = descriptor_type::unknown; break;
      }
      return fs_detail::ok<std::optional<directory_entry>>(std::move(entry));
   }

   // ── filesystem-error-code ─────────────────────────────────────────

   std::optional<filesystem_error_code> filesystem_error_code_fn(
       psio1::borrow<io_error> /*err*/)
   {
      return std::nullopt;
   }
};

}  // namespace wasi_host

PSIO1_HOST_MODULE(wasi_host::WasiFilesystemHost,
   interface(wasi_filesystem_preopens, get_directories),
   interface(wasi_filesystem_types,
      descriptor_read_via_stream, descriptor_write_via_stream,
      descriptor_append_via_stream, descriptor_advise,
      descriptor_sync_data, descriptor_get_flags,
      descriptor_get_type, descriptor_set_size,
      descriptor_set_times, descriptor_read,
      descriptor_write, descriptor_read_directory,
      descriptor_sync, descriptor_create_directory_at,
      descriptor_stat_fn, descriptor_stat_at,
      descriptor_set_times_at, descriptor_link_at,
      descriptor_open_at, descriptor_readlink_at,
      descriptor_remove_directory_at, descriptor_rename_at,
      descriptor_symlink_at, descriptor_unlink_file_at,
      descriptor_is_same_object, descriptor_metadata_hash,
      descriptor_metadata_hash_at,
      directory_entry_stream_read_directory_entry,
      filesystem_error_code_fn))
