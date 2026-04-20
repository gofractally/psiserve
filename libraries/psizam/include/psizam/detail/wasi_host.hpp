#pragma once

// WASI Preview1 host implementation for psizam.
// Provides the wasi_snapshot_preview1 functions needed to run WASI-targeting WASM modules.

#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace psizam::detail {

   // WASI errno codes
   enum wasi_errno : uint32_t {
      WASI_ESUCCESS     = 0,
      WASI_E2BIG        = 1,
      WASI_EACCES       = 2,
      WASI_EBADF        = 8,
      WASI_EEXIST       = 20,
      WASI_EFAULT       = 21,
      WASI_EINVAL       = 28,
      WASI_EIO          = 29,
      WASI_EISDIR       = 31,
      WASI_ELOOP        = 32,
      WASI_ENAMETOOLONG = 37,
      WASI_ENOENT       = 44,
      WASI_EMLINK       = 36,
      WASI_ENOSPC       = 51,
      WASI_ENOSYS       = 52,
      WASI_ENOTDIR      = 54,
      WASI_ENOTEMPTY    = 55,
      WASI_EPERM        = 63,
      WASI_EROFS        = 69,
      WASI_ESPIPE       = 70,
      WASI_EXDEV        = 75,
   };

   // WASI file types
   enum wasi_filetype : uint8_t {
      WASI_FILETYPE_UNKNOWN          = 0,
      WASI_FILETYPE_BLOCK_DEVICE     = 1,
      WASI_FILETYPE_CHARACTER_DEVICE = 2,
      WASI_FILETYPE_DIRECTORY        = 3,
      WASI_FILETYPE_REGULAR_FILE     = 4,
      WASI_FILETYPE_SYMBOLIC_LINK    = 7,
   };

   // WASI rights (simplified — grant all)
   static constexpr uint64_t WASI_RIGHTS_ALL = ~uint64_t(0);

   // WASI whence
   enum wasi_whence : uint8_t {
      WASI_WHENCE_SET = 0,
      WASI_WHENCE_CUR = 1,
      WASI_WHENCE_END = 2,
   };

   // WASI oflags
   enum wasi_oflags : uint16_t {
      WASI_OFLAG_CREAT     = 1,
      WASI_OFLAG_DIRECTORY = 2,
      WASI_OFLAG_EXCL      = 4,
      WASI_OFLAG_TRUNC     = 8,
   };

   // WASI clock IDs
   enum wasi_clockid : uint32_t {
      WASI_CLOCK_REALTIME           = 0,
      WASI_CLOCK_MONOTONIC          = 1,
      WASI_CLOCK_PROCESS_CPUTIME_ID = 2,
      WASI_CLOCK_THREAD_CPUTIME_ID  = 3,
   };

   // Convert host errno to WASI errno
   inline uint32_t errno_to_wasi(int err) {
      switch (err) {
         case 0:       return WASI_ESUCCESS;
         case EACCES:  return WASI_EACCES;
         case EBADF:   return WASI_EBADF;
         case EEXIST:  return WASI_EEXIST;
         case EFAULT:  return WASI_EFAULT;
         case EINVAL:  return WASI_EINVAL;
         case EIO:     return WASI_EIO;
         case EISDIR:  return WASI_EISDIR;
#ifdef ELOOP
         case ELOOP:   return WASI_ELOOP;
#endif
         case ENAMETOOLONG: return WASI_ENAMETOOLONG;
         case ENOENT:  return WASI_ENOENT;
         case ENOSYS:  return WASI_ENOSYS;
         case ENOTDIR: return WASI_ENOTDIR;
         case ENOTEMPTY: return WASI_ENOTEMPTY;
         case EPERM:   return WASI_EPERM;
         case EROFS:   return WASI_EROFS;
#ifdef EMLINK
         case EMLINK:  return WASI_EMLINK;
#endif
         case ENOSPC:  return WASI_ENOSPC;
         case ESPIPE:  return WASI_ESPIPE;
         case EXDEV:   return WASI_EXDEV;
         default:      return WASI_EIO;
      }
   }

   /// WASI Preview1 host implementation.
   /// Provides file I/O, args, env, clock, and random to WASM modules.
   struct wasi_host {
      // Memory base — updated before each host function call by the trampoline
      char* memory = nullptr;

      // Command-line arguments
      std::vector<std::string> args;

      // Environment variables
      std::vector<std::string> env;

      // Pre-opened directories: fd -> path
      // fd 0 = stdin, 1 = stdout, 2 = stderr
      // fd 3+ = pre-opened directories
      struct preopen {
         std::string guest_path; // path as seen by the WASM module
         std::string host_path;  // actual host path
      };
      std::vector<preopen> preopens; // indexed by (fd - 3)

      // Open file descriptors: fd -> host fd
      // We reserve fd 0-2 for stdio, 3+ for preopened dirs
      // Files opened by path_open get fd = next_fd++
      struct open_fd {
         int     host_fd  = -1;
         uint8_t filetype = WASI_FILETYPE_UNKNOWN;
         bool    is_preopen = false;
         std::string preopen_path; // guest path for preopened dirs
      };
      std::vector<open_fd> fds;
      int exit_code = 0;

      wasi_host() {
         // stdio fds
         fds.push_back({STDIN_FILENO, WASI_FILETYPE_CHARACTER_DEVICE, false, ""});
         fds.push_back({STDOUT_FILENO, WASI_FILETYPE_CHARACTER_DEVICE, false, ""});
         fds.push_back({STDERR_FILENO, WASI_FILETYPE_CHARACTER_DEVICE, false, ""});
      }

      void add_preopen(const std::string& guest, const std::string& host) {
         uint32_t fd = static_cast<uint32_t>(fds.size());
         int hfd = ::open(host.c_str(), O_RDONLY | O_DIRECTORY);
         if (hfd < 0) hfd = -1; // will fail on use
         fds.push_back({hfd, WASI_FILETYPE_DIRECTORY, true, guest});
         preopens.push_back({guest, host});
      }

      // Helper: validate iov offset arithmetic won't overflow (guard pages handle actual OOB)
      static void check_iov_bounds(uint32_t iovs_ptr, uint32_t iovs_len) {
         PSIZAM_ASSERT(uint64_t(iovs_ptr) + uint64_t(iovs_len) * 8 <= UINT32_MAX,
                       wasm_memory_exception, "WASI: iov offset overflow");
      }

      // Helper: get wasm memory pointer (guard pages protect OOB)
      template<typename T = char>
      T* mem(uint32_t offset) { return reinterpret_cast<T*>(memory + offset); }

      template<typename T = char>
      const T* mem(uint32_t offset) const { return reinterpret_cast<const T*>(memory + offset); }

      // Helper: write uint32 to wasm memory
      void write_u32(uint32_t offset, uint32_t val) {
         memcpy(memory + offset, &val, 4);
      }

      // Helper: write uint64 to wasm memory
      void write_u64(uint32_t offset, uint64_t val) {
         memcpy(memory + offset, &val, 8);
      }

      // Helper: read uint32 from wasm memory
      uint32_t read_u32(uint32_t offset) const {
         uint32_t val;
         memcpy(&val, memory + offset, 4);
         return val;
      }

      // Helper: valid fd check
      bool valid_fd(uint32_t fd) const {
         return fd < fds.size() && fds[fd].host_fd >= 0;
      }

      // Resolve a guest path to a host path using preopens.
      // Returns empty string on invalid fd or path traversal attempt.
      std::string resolve_path(uint32_t dir_fd, const char* path, uint32_t path_len) const {
         if (dir_fd < 3 || dir_fd >= fds.size() || !fds[dir_fd].is_preopen)
            return "";
         uint32_t preopen_idx = dir_fd - 3;
         if (preopen_idx >= preopens.size()) return "";
         std::string guest(path, path_len);
         const auto& po = preopens[preopen_idx];
         if (guest.empty() || guest == ".") return po.host_path;

         // Reject path traversal: no ".." components allowed
         if (guest.find("..") != std::string::npos)
            return "";
         // Reject absolute paths
         if (!guest.empty() && guest[0] == '/')
            return "";

         std::string resolved = po.host_path + "/" + guest;

         // Resolve the real path and verify it stays under the preopen root
         char real[PATH_MAX];
         // First resolve the preopen root
         char real_root[PATH_MAX];
         if (!::realpath(po.host_path.c_str(), real_root))
            return "";
         std::string root_prefix(real_root);

         // Try to resolve the full path; if the file doesn't exist yet,
         // resolve the parent directory instead
         if (::realpath(resolved.c_str(), real)) {
            std::string real_str(real);
            if (real_str.compare(0, root_prefix.size(), root_prefix) != 0)
               return "";
            // Must be exactly the root or followed by '/'
            if (real_str.size() > root_prefix.size() && real_str[root_prefix.size()] != '/')
               return "";
         } else {
            // File doesn't exist — resolve parent to check containment
            auto last_slash = resolved.rfind('/');
            if (last_slash != std::string::npos) {
               std::string parent = resolved.substr(0, last_slash);
               if (parent.empty()) parent = "/";
               if (!::realpath(parent.c_str(), real))
                  return "";
               std::string real_parent(real);
               if (real_parent.compare(0, root_prefix.size(), root_prefix) != 0)
                  return "";
               if (real_parent.size() > root_prefix.size() && real_parent[root_prefix.size()] != '/')
                  return "";
            }
         }

         return resolved;
      }

      // ── WASI functions ──────────────────────────────────────────────

      // args_get: write arg pointers and strings into wasm memory
      uint32_t args_get(uint32_t argv_ptr, uint32_t argv_buf_ptr) {
         uint32_t buf_offset = argv_buf_ptr;
         for (size_t i = 0; i < args.size(); i++) {
            write_u32(argv_ptr + i * 4, buf_offset);
            memcpy(mem(buf_offset), args[i].c_str(), args[i].size() + 1);
            buf_offset += args[i].size() + 1;
         }
         return WASI_ESUCCESS;
      }

      // args_sizes_get: return count and total buffer size
      uint32_t args_sizes_get(uint32_t count_ptr, uint32_t size_ptr) {
         uint32_t total = 0;
         for (auto& a : args) total += a.size() + 1;
         write_u32(count_ptr, static_cast<uint32_t>(args.size()));
         write_u32(size_ptr, total);
         return WASI_ESUCCESS;
      }

      // environ_get
      uint32_t environ_get(uint32_t environ_ptr, uint32_t buf_ptr) {
         if (std::getenv("PSIZAM_TRACE_WASI")) {
            fprintf(stderr, "[wasi] environ_get(environ=0x%x, buf=0x%x) env.size=%zu\n",
                    environ_ptr, buf_ptr, env.size());
         }
         uint32_t buf_offset = buf_ptr;
         for (size_t i = 0; i < env.size(); i++) {
            write_u32(environ_ptr + i * 4, buf_offset);
            memcpy(mem(buf_offset), env[i].c_str(), env[i].size() + 1);
            buf_offset += env[i].size() + 1;
         }
         if (std::getenv("PSIZAM_TRACE_WASI")) {
            fprintf(stderr, "[wasi] environ_get done, wrote %zu ptrs, last buf=0x%x\n",
                    env.size(), buf_offset);
         }
         return WASI_ESUCCESS;
      }

      // environ_sizes_get
      uint32_t environ_sizes_get(uint32_t count_ptr, uint32_t size_ptr) {
         uint32_t total = 0;
         for (auto& e : env) total += e.size() + 1;
         if (std::getenv("PSIZAM_TRACE_WASI")) {
            fprintf(stderr, "[wasi] environ_sizes_get(count_ptr=0x%x, size_ptr=0x%x) → count=%zu total=%u\n",
                    count_ptr, size_ptr, env.size(), total);
         }
         write_u32(count_ptr, static_cast<uint32_t>(env.size()));
         write_u32(size_ptr, total);
         return WASI_ESUCCESS;
      }

      // clock_time_get
      uint32_t clock_time_get(uint32_t clock_id, uint64_t /*precision*/, uint32_t time_ptr) {
         struct timespec ts;
         clockid_t cid;
         switch (clock_id) {
            case WASI_CLOCK_REALTIME:  cid = CLOCK_REALTIME; break;
            case WASI_CLOCK_MONOTONIC: cid = CLOCK_MONOTONIC; break;
            default: cid = CLOCK_REALTIME; break;
         }
         if (clock_gettime(cid, &ts) != 0)
            return errno_to_wasi(errno);
         uint64_t nanos = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
         write_u64(time_ptr, nanos);
         return WASI_ESUCCESS;
      }

      // fd_close
      uint32_t fd_close(uint32_t fd) {
         if (fd >= fds.size()) return WASI_EBADF;
         if (fds[fd].host_fd < 0) return WASI_EBADF;
         // Don't close stdio
         if (fd < 3) return WASI_ESUCCESS;
         ::close(fds[fd].host_fd);
         fds[fd].host_fd = -1;
         return WASI_ESUCCESS;
      }

      // fd_fdstat_get: return file descriptor attributes
      uint32_t fd_fdstat_get(uint32_t fd, uint32_t stat_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         // fdstat layout: u8 filetype, u16 flags, u64 rights_base, u64 rights_inheriting
         // total: 24 bytes
         memset(mem(stat_ptr), 0, 24);
         mem<uint8_t>(stat_ptr)[0] = fds[fd].filetype;
         // Grant all rights
         write_u64(stat_ptr + 8, WASI_RIGHTS_ALL);
         write_u64(stat_ptr + 16, WASI_RIGHTS_ALL);
         return WASI_ESUCCESS;
      }

      // fd_fdstat_set_flags
      uint32_t fd_fdstat_set_flags(uint32_t fd, uint32_t /*flags*/) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         return WASI_ESUCCESS; // no-op
      }

      // fd_filestat_get
      uint32_t fd_filestat_get(uint32_t fd, uint32_t buf_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         struct stat st;
         if (fstat(fds[fd].host_fd, &st) != 0)
            return errno_to_wasi(errno);
         // filestat layout: u64 dev, u64 ino, u8 filetype, u8 pad[7],
         //                  u64 nlink, u64 size, u64 atim, u64 mtim, u64 ctim
         memset(mem(buf_ptr), 0, 64);
         write_u64(buf_ptr + 0, st.st_dev);
         write_u64(buf_ptr + 8, st.st_ino);
         mem<uint8_t>(buf_ptr + 16)[0] = fds[fd].filetype;
         write_u64(buf_ptr + 24, st.st_nlink);
         write_u64(buf_ptr + 32, st.st_size);
         return WASI_ESUCCESS;
      }

      // fd_prestat_get: return preopen info
      uint32_t fd_prestat_get(uint32_t fd, uint32_t buf_ptr) {
         if (fd < 3 || fd >= fds.size()) return WASI_EBADF;
         if (!fds[fd].is_preopen) return WASI_EBADF;
         // prestat: u8 tag (0 = dir), u8 pad[3], u32 dir_name_len
         write_u32(buf_ptr, 0); // tag = dir
         write_u32(buf_ptr + 4, static_cast<uint32_t>(fds[fd].preopen_path.size()));
         return WASI_ESUCCESS;
      }

      // fd_prestat_dir_name
      uint32_t fd_prestat_dir_name(uint32_t fd, uint32_t path_ptr, uint32_t path_len) {
         if (fd < 3 || fd >= fds.size()) return WASI_EBADF;
         if (!fds[fd].is_preopen) return WASI_EBADF;
         uint32_t copy_len = std::min(path_len, static_cast<uint32_t>(fds[fd].preopen_path.size()));
         memcpy(mem(path_ptr), fds[fd].preopen_path.c_str(), copy_len);
         return WASI_ESUCCESS;
      }

      // fd_read: scatter read
      uint32_t fd_read(uint32_t fd, uint32_t iovs_ptr, uint32_t iovs_len, uint32_t nread_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         check_iov_bounds(iovs_ptr, iovs_len);
         uint32_t total = 0;
         for (uint32_t i = 0; i < iovs_len; i++) {
            uint32_t buf = read_u32(iovs_ptr + i * 8);
            uint32_t len = read_u32(iovs_ptr + i * 8 + 4);
            ssize_t n = ::read(fds[fd].host_fd, mem(buf), len);
            if (n < 0) {
               if (total > 0) break;
               return errno_to_wasi(errno);
            }
            total += n;
            if (static_cast<uint32_t>(n) < len) break;
         }
         write_u32(nread_ptr, total);
         return WASI_ESUCCESS;
      }

      // fd_pread: scatter read at offset
      uint32_t fd_pread(uint32_t fd, uint32_t iovs_ptr, uint32_t iovs_len,
                        uint64_t offset, uint32_t nread_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         check_iov_bounds(iovs_ptr, iovs_len);
         uint32_t total = 0;
         for (uint32_t i = 0; i < iovs_len; i++) {
            uint32_t buf = read_u32(iovs_ptr + i * 8);
            uint32_t len = read_u32(iovs_ptr + i * 8 + 4);
            ssize_t n = ::pread(fds[fd].host_fd, mem(buf), len, offset + total);
            if (n < 0) {
               if (total > 0) break;
               return errno_to_wasi(errno);
            }
            total += n;
            if (static_cast<uint32_t>(n) < len) break;
         }
         write_u32(nread_ptr, total);
         return WASI_ESUCCESS;
      }

      // fd_write: gather write
      uint32_t fd_write(uint32_t fd, uint32_t iovs_ptr, uint32_t iovs_len, uint32_t nwritten_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         check_iov_bounds(iovs_ptr, iovs_len);
         uint32_t total = 0;
         for (uint32_t i = 0; i < iovs_len; i++) {
            uint32_t buf = read_u32(iovs_ptr + i * 8);
            uint32_t len = read_u32(iovs_ptr + i * 8 + 4);
            ssize_t n = ::write(fds[fd].host_fd, mem(buf), len);
            if (n < 0) {
               if (total > 0) break;
               return errno_to_wasi(errno);
            }
            total += n;
            if (static_cast<uint32_t>(n) < len) break;
         }
         write_u32(nwritten_ptr, total);
         return WASI_ESUCCESS;
      }

      // fd_seek
      uint32_t fd_seek(uint32_t fd, int64_t offset, uint32_t whence, uint32_t newoffset_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         int host_whence;
         switch (whence) {
            case WASI_WHENCE_SET: host_whence = SEEK_SET; break;
            case WASI_WHENCE_CUR: host_whence = SEEK_CUR; break;
            case WASI_WHENCE_END: host_whence = SEEK_END; break;
            default: return WASI_EINVAL;
         }
         off_t result = ::lseek(fds[fd].host_fd, offset, host_whence);
         if (result < 0) return errno_to_wasi(errno);
         write_u64(newoffset_ptr, static_cast<uint64_t>(result));
         return WASI_ESUCCESS;
      }

      // fd_tell — returns current file offset
      uint32_t fd_tell(uint32_t fd, uint32_t offset_ptr) {
         return fd_seek(fd, 0, WASI_WHENCE_CUR, offset_ptr);
      }

      // fd_readdir: read directory entries
      uint32_t fd_readdir(uint32_t fd, uint32_t buf, uint32_t buf_len,
                          uint64_t cookie, uint32_t bufused_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         int hfd = fds[fd].host_fd;
         // Open a DIR* from the host fd (dup to avoid closing the original)
         int dfd = ::dup(hfd);
         if (dfd < 0) return errno_to_wasi(errno);
         DIR* dir = ::fdopendir(dfd);
         if (!dir) { ::close(dfd); return errno_to_wasi(errno); }

         // Skip to the cookie position
         if (cookie > 0) ::seekdir(dir, static_cast<long>(cookie));

         uint32_t used = 0;
         struct dirent* ent;
         while ((ent = ::readdir(dir)) != nullptr) {
            uint32_t name_len = static_cast<uint32_t>(strlen(ent->d_name));
            // dirent layout: u64 d_next, u64 d_ino, u32 d_namlen, u8 d_type
            uint32_t header_size = 24;
            uint32_t entry_size = header_size + name_len;

            if (used + header_size > buf_len) break; // no room for header

            // Write header
            uint64_t next_cookie = static_cast<uint64_t>(::telldir(dir));
            memcpy(mem(buf + used), &next_cookie, 8);
            write_u64(buf + used + 8, ent->d_ino);
            write_u32(buf + used + 16, name_len);
            uint8_t ftype = WASI_FILETYPE_UNKNOWN;
            switch (ent->d_type) {
               case DT_REG: ftype = WASI_FILETYPE_REGULAR_FILE; break;
               case DT_DIR: ftype = WASI_FILETYPE_DIRECTORY; break;
               case DT_LNK: ftype = WASI_FILETYPE_SYMBOLIC_LINK; break;
               case DT_BLK: ftype = WASI_FILETYPE_BLOCK_DEVICE; break;
               case DT_CHR: ftype = WASI_FILETYPE_CHARACTER_DEVICE; break;
               default: break;
            }
            *mem<uint8_t>(buf + used + 20) = ftype;
            used += header_size;

            // Write name (may be truncated if buf runs out)
            uint32_t name_copy = std::min(name_len, buf_len - used);
            memcpy(mem(buf + used), ent->d_name, name_copy);
            used += name_copy;
            if (name_copy < name_len) break; // truncated
         }
         ::closedir(dir); // also closes dfd
         write_u32(bufused_ptr, used);
         return WASI_ESUCCESS;
      }

      // path_open: open a file relative to a pre-opened directory
      uint32_t path_open(uint32_t dir_fd, uint32_t /*dirflags*/,
                         uint32_t path_ptr, uint32_t path_len,
                         uint32_t oflags, uint64_t /*rights_base*/,
                         uint64_t /*rights_inheriting*/, uint32_t fdflags,
                         uint32_t fd_ptr) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;

         int flags = 0;
         if (oflags & WASI_OFLAG_CREAT)  flags |= O_CREAT;
         if (oflags & WASI_OFLAG_EXCL)   flags |= O_EXCL;
         if (oflags & WASI_OFLAG_TRUNC)  flags |= O_TRUNC;

         // Determine read/write mode from fdflags (simplified)
         if (fdflags & 1) // append
            flags |= O_WRONLY | O_APPEND;
         else if (oflags & (WASI_OFLAG_CREAT | WASI_OFLAG_TRUNC))
            flags |= O_RDWR;
         else
            flags |= O_RDONLY;

         // Check if it's a directory
         struct stat st;
         if (oflags & WASI_OFLAG_DIRECTORY) {
            flags = O_RDONLY | O_DIRECTORY;
         } else if (::stat(host_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            flags = O_RDONLY | O_DIRECTORY;
         }

         // Prevent symlink-following sandbox escapes
         flags |= O_NOFOLLOW;
         int hfd = ::open(host_path.c_str(), flags, 0666);
         if (hfd < 0) return errno_to_wasi(errno);

         uint32_t wasi_fd = static_cast<uint32_t>(fds.size());
         uint8_t ft = WASI_FILETYPE_REGULAR_FILE;
         if (fstat(hfd, &st) == 0 && S_ISDIR(st.st_mode))
            ft = WASI_FILETYPE_DIRECTORY;
         fds.push_back({hfd, ft, false, ""});
         write_u32(fd_ptr, wasi_fd);
         return WASI_ESUCCESS;
      }

      // path_filestat_get
      uint32_t path_filestat_get(uint32_t dir_fd, uint32_t /*flags*/,
                                 uint32_t path_ptr, uint32_t path_len,
                                 uint32_t buf_ptr) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         struct stat st;
         if (::stat(host_path.c_str(), &st) != 0)
            return errno_to_wasi(errno);
         memset(mem(buf_ptr), 0, 64);
         write_u64(buf_ptr + 0, st.st_dev);
         write_u64(buf_ptr + 8, st.st_ino);
         uint8_t ft = WASI_FILETYPE_REGULAR_FILE;
         if (S_ISDIR(st.st_mode)) ft = WASI_FILETYPE_DIRECTORY;
         else if (S_ISLNK(st.st_mode)) ft = WASI_FILETYPE_SYMBOLIC_LINK;
         mem<uint8_t>(buf_ptr + 16)[0] = ft;
         write_u64(buf_ptr + 24, st.st_nlink);
         write_u64(buf_ptr + 32, st.st_size);
         return WASI_ESUCCESS;
      }

      // path_readlink
      uint32_t path_readlink(uint32_t dir_fd, uint32_t path_ptr, uint32_t path_len,
                             uint32_t buf_ptr, uint32_t buf_len, uint32_t bufused_ptr) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         ssize_t n = ::readlink(host_path.c_str(), mem(buf_ptr), buf_len);
         if (n < 0) return errno_to_wasi(errno);
         write_u32(bufused_ptr, static_cast<uint32_t>(n));
         return WASI_ESUCCESS;
      }

      // path_remove_directory
      uint32_t path_remove_directory(uint32_t dir_fd, uint32_t path_ptr, uint32_t path_len) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         if (::rmdir(host_path.c_str()) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // path_unlink_file
      uint32_t path_unlink_file(uint32_t dir_fd, uint32_t path_ptr, uint32_t path_len) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         if (::unlink(host_path.c_str()) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // random_get
      uint32_t random_get(uint32_t buf_ptr, uint32_t buf_len) {
         // Use /dev/urandom or arc4random
         std::random_device rd;
         auto* p = mem<uint8_t>(buf_ptr);
         for (uint32_t i = 0; i < buf_len; i++)
            p[i] = static_cast<uint8_t>(rd());
         return WASI_ESUCCESS;
      }

      // proc_exit — terminates execution by throwing
      struct wasi_exit_exception {
         int code;
      };

      void proc_exit(uint32_t code) {
         exit_code = code;
         throw wasi_exit_exception{static_cast<int>(code)};
      }

      // clock_res_get: return clock resolution
      uint32_t clock_res_get(uint32_t clock_id, uint32_t resolution_ptr) {
         uint64_t res;
         switch (clock_id) {
            case WASI_CLOCK_REALTIME:
            case WASI_CLOCK_MONOTONIC:
            case WASI_CLOCK_PROCESS_CPUTIME_ID:
            case WASI_CLOCK_THREAD_CPUTIME_ID: {
               struct timespec ts;
               clockid_t cid = (clock_id == WASI_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
               if (clock_getres(cid, &ts) != 0) return errno_to_wasi(errno);
               res = static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
               break;
            }
            default: return WASI_EINVAL;
         }
         write_u64(resolution_ptr, res);
         return WASI_ESUCCESS;
      }

      // fd_advise: hint about file access pattern (no-op)
      uint32_t fd_advise(uint32_t fd, uint64_t /*offset*/, uint64_t /*len*/, uint32_t /*advice*/) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         return WASI_ESUCCESS;
      }

      // fd_allocate: preallocate file space
      uint32_t fd_allocate(uint32_t fd, uint64_t offset, uint64_t len) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
#ifdef __APPLE__
         // macOS: use ftruncate if extending
         struct stat st;
         if (fstat(fds[fd].host_fd, &st) != 0) return errno_to_wasi(errno);
         off_t needed = static_cast<off_t>(offset + len);
         if (needed > st.st_size) {
            if (ftruncate(fds[fd].host_fd, needed) != 0) return errno_to_wasi(errno);
         }
#else
         if (posix_fallocate(fds[fd].host_fd, offset, len) != 0)
            return errno_to_wasi(errno);
#endif
         return WASI_ESUCCESS;
      }

      // fd_datasync: synchronize file data to disk
      uint32_t fd_datasync(uint32_t fd) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
#ifdef __APPLE__
         if (::fsync(fds[fd].host_fd) != 0) return errno_to_wasi(errno);
#else
         if (::fdatasync(fds[fd].host_fd) != 0) return errno_to_wasi(errno);
#endif
         return WASI_ESUCCESS;
      }

      // fd_sync: synchronize file data and metadata
      uint32_t fd_sync(uint32_t fd) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         if (::fsync(fds[fd].host_fd) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // fd_fdstat_set_rights: set fd rights (no-op — we grant all rights)
      uint32_t fd_fdstat_set_rights(uint32_t fd, uint64_t /*rights_base*/, uint64_t /*rights_inheriting*/) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         return WASI_ESUCCESS;
      }

      // fd_filestat_set_size: truncate file
      uint32_t fd_filestat_set_size(uint32_t fd, uint64_t size) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         if (::ftruncate(fds[fd].host_fd, static_cast<off_t>(size)) != 0)
            return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // fd_filestat_set_times: set file timestamps
      uint32_t fd_filestat_set_times(uint32_t fd, uint64_t atim, uint64_t mtim, uint32_t fst_flags) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         struct timespec times[2];
         // fst_flags: bit 0 = set atim, bit 1 = set atim to now, bit 2 = set mtim, bit 3 = set mtim to now
         if (fst_flags & 2) {
            times[0].tv_nsec = UTIME_NOW;
            times[0].tv_sec = 0;
         } else if (fst_flags & 1) {
            times[0].tv_sec = static_cast<time_t>(atim / 1000000000ULL);
            times[0].tv_nsec = static_cast<long>(atim % 1000000000ULL);
         } else {
            times[0].tv_nsec = UTIME_OMIT;
            times[0].tv_sec = 0;
         }
         if (fst_flags & 8) {
            times[1].tv_nsec = UTIME_NOW;
            times[1].tv_sec = 0;
         } else if (fst_flags & 4) {
            times[1].tv_sec = static_cast<time_t>(mtim / 1000000000ULL);
            times[1].tv_nsec = static_cast<long>(mtim % 1000000000ULL);
         } else {
            times[1].tv_nsec = UTIME_OMIT;
            times[1].tv_sec = 0;
         }
         if (futimens(fds[fd].host_fd, times) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // fd_pwrite: scatter write at offset
      uint32_t fd_pwrite(uint32_t fd, uint32_t iovs_ptr, uint32_t iovs_len,
                         uint64_t offset, uint32_t nwritten_ptr) {
         if (fd >= fds.size() || fds[fd].host_fd < 0) return WASI_EBADF;
         check_iov_bounds(iovs_ptr, iovs_len);
         uint32_t total = 0;
         for (uint32_t i = 0; i < iovs_len; i++) {
            uint32_t buf = read_u32(iovs_ptr + i * 8);
            uint32_t len = read_u32(iovs_ptr + i * 8 + 4);
            ssize_t n = ::pwrite(fds[fd].host_fd, mem(buf), len, offset + total);
            if (n < 0) {
               if (total > 0) break;
               return errno_to_wasi(errno);
            }
            total += n;
            if (static_cast<uint32_t>(n) < len) break;
         }
         write_u32(nwritten_ptr, total);
         return WASI_ESUCCESS;
      }

      // fd_renumber: renumber file descriptor
      uint32_t fd_renumber(uint32_t from, uint32_t to) {
         if (from >= fds.size() || fds[from].host_fd < 0) return WASI_EBADF;
         // Cap target fd to prevent DoS via massive vector allocation
         static constexpr uint32_t max_fds = 65536;
         if (to >= max_fds) return WASI_EBADF;
         if (to >= fds.size()) {
            fds.resize(to + 1);
         }
         if (fds[to].host_fd >= 0 && to >= 3) {
            ::close(fds[to].host_fd);
         }
         fds[to] = fds[from];
         fds[from].host_fd = -1;
         return WASI_ESUCCESS;
      }

      // path_create_directory
      uint32_t path_create_directory(uint32_t dir_fd, uint32_t path_ptr, uint32_t path_len) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         if (::mkdir(host_path.c_str(), 0777) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // path_filestat_set_times
      uint32_t path_filestat_set_times(uint32_t dir_fd, uint32_t /*flags*/,
                                       uint32_t path_ptr, uint32_t path_len,
                                       uint64_t atim, uint64_t mtim, uint32_t fst_flags) {
         std::string host_path = resolve_path(dir_fd, mem(path_ptr), path_len);
         if (host_path.empty()) return WASI_ENOENT;
         struct timespec times[2];
         if (fst_flags & 2) {
            times[0].tv_nsec = UTIME_NOW; times[0].tv_sec = 0;
         } else if (fst_flags & 1) {
            times[0].tv_sec = static_cast<time_t>(atim / 1000000000ULL);
            times[0].tv_nsec = static_cast<long>(atim % 1000000000ULL);
         } else {
            times[0].tv_nsec = UTIME_OMIT; times[0].tv_sec = 0;
         }
         if (fst_flags & 8) {
            times[1].tv_nsec = UTIME_NOW; times[1].tv_sec = 0;
         } else if (fst_flags & 4) {
            times[1].tv_sec = static_cast<time_t>(mtim / 1000000000ULL);
            times[1].tv_nsec = static_cast<long>(mtim % 1000000000ULL);
         } else {
            times[1].tv_nsec = UTIME_OMIT; times[1].tv_sec = 0;
         }
         if (utimensat(AT_FDCWD, host_path.c_str(), times, 0) != 0)
            return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // path_link: create hard link
      uint32_t path_link(uint32_t old_fd, uint32_t /*old_flags*/,
                         uint32_t old_path_ptr, uint32_t old_path_len,
                         uint32_t new_fd,
                         uint32_t new_path_ptr, uint32_t new_path_len) {
         std::string old_host = resolve_path(old_fd, mem(old_path_ptr), old_path_len);
         std::string new_host = resolve_path(new_fd, mem(new_path_ptr), new_path_len);
         if (old_host.empty() || new_host.empty()) return WASI_ENOENT;
         if (::link(old_host.c_str(), new_host.c_str()) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // path_rename
      uint32_t path_rename(uint32_t old_fd, uint32_t old_path_ptr, uint32_t old_path_len,
                           uint32_t new_fd, uint32_t new_path_ptr, uint32_t new_path_len) {
         std::string old_host = resolve_path(old_fd, mem(old_path_ptr), old_path_len);
         std::string new_host = resolve_path(new_fd, mem(new_path_ptr), new_path_len);
         if (old_host.empty() || new_host.empty()) return WASI_ENOENT;
         if (::rename(old_host.c_str(), new_host.c_str()) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // path_symlink: create symbolic link
      uint32_t path_symlink(uint32_t old_path_ptr, uint32_t old_path_len,
                            uint32_t dir_fd,
                            uint32_t new_path_ptr, uint32_t new_path_len) {
         std::string old_path(mem(old_path_ptr), old_path_len);
         std::string new_host = resolve_path(dir_fd, mem(new_path_ptr), new_path_len);
         if (new_host.empty()) return WASI_ENOENT;
         if (::symlink(old_path.c_str(), new_host.c_str()) != 0) return errno_to_wasi(errno);
         return WASI_ESUCCESS;
      }

      // poll_oneoff: event polling
      uint32_t poll_oneoff(uint32_t in_ptr, uint32_t out_ptr, uint32_t nsubscriptions,
                           uint32_t nevents_ptr) {
         // Each subscription is 48 bytes, each event is 32 bytes
         uint32_t events_written = 0;
         for (uint32_t i = 0; i < nsubscriptions; i++) {
            uint32_t sub_offset = in_ptr + i * 48;
            uint64_t userdata;
            memcpy(&userdata, mem(sub_offset), 8);
            uint8_t type = *mem<uint8_t>(sub_offset + 8);

            uint32_t evt_offset = out_ptr + events_written * 32;
            memset(mem(evt_offset), 0, 32);
            memcpy(mem(evt_offset), &userdata, 8);  // userdata
            // error = 0 (success)
            *mem<uint8_t>(evt_offset + 10) = type;  // event type

            if (type == 0) {
               // clock: extract timeout and sleep
               uint64_t timeout;
               memcpy(&timeout, mem(sub_offset + 24), 8);
               uint16_t flags;
               memcpy(&flags, mem(sub_offset + 40), 2);
               if (!(flags & 1)) { // not absolute
                  struct timespec ts;
                  ts.tv_sec = timeout / 1000000000ULL;
                  ts.tv_nsec = timeout % 1000000000ULL;
                  nanosleep(&ts, nullptr);
               }
            } else {
               // fd_read (1) or fd_write (2): report ready immediately
               write_u64(evt_offset + 16, 1); // nbytes available
            }
            events_written++;
         }
         write_u32(nevents_ptr, events_written);
         return WASI_ESUCCESS;
      }

      // proc_raise: raise signal (no-op in sandbox)
      uint32_t proc_raise(uint32_t /*sig*/) {
         return WASI_ENOSYS;
      }

      // sched_yield: yield CPU
      uint32_t sched_yield() {
         return WASI_ESUCCESS;
      }

   };

   /// WASI trampoline: sets host->memory before dispatching to the member function.
   /// All WASI functions take scalar types, so we use the fast trampoline path.
   template<auto Func>
   native_value wasi_trampoline(void* host, native_value* args, char* memory) {
      using args_t = flatten_parameters_t<AUTO_PARAM_WORKAROUND(Func)>;
      using ret_t  = return_type_t<AUTO_PARAM_WORKAROUND(Func)>;
      static_cast<wasi_host*>(host)->memory = memory;
      return fast_trampoline_fwd_impl<Func, wasi_host, ret_t, args_t>(
         static_cast<wasi_host*>(host), args, memory,
         std::make_index_sequence<std::tuple_size_v<args_t>>{});
   }

   /// Reverse-order WASI trampoline: args[0] = last WASM param (matches jit/jit2
   /// zero-copy stack pass). backend::construct() picks this one for backends
   /// with reverse_host_args=true.
   template<auto Func>
   native_value wasi_trampoline_rev(void* host, native_value* args, char* memory) {
      using args_t = flatten_parameters_t<AUTO_PARAM_WORKAROUND(Func)>;
      using ret_t  = return_type_t<AUTO_PARAM_WORKAROUND(Func)>;
      static_cast<wasi_host*>(host)->memory = memory;
      return fast_trampoline_rev_impl<Func, wasi_host, ret_t, args_t>(
         static_cast<wasi_host*>(host), args, memory,
         std::make_index_sequence<std::tuple_size_v<args_t>>{});
   }

   /// Register a single WASI function on a host_function_table.
   template<auto Func>
   void add_wasi_func(host_function_table& table, const std::string& name) {
      using args_t = flatten_parameters_t<AUTO_PARAM_WORKAROUND(Func)>;
      using ret_t  = return_type_t<AUTO_PARAM_WORKAROUND(Func)>;
      using TC     = psizam::type_converter<wasi_host>;

      host_function_table::entry e;
      e.module_name     = "wasi_snapshot_preview1";
      e.func_name       = name;
      e.signature       = function_types_provider<TC, ret_t, args_t>(
         std::make_index_sequence<std::tuple_size_v<args_t>>{});
      e.trampoline      = &wasi_trampoline<Func>;
      e.rev_trampoline  = &wasi_trampoline_rev<Func>;
      table.add_entry(std::move(e));
   }

   /// Register all WASI Preview1 functions on a host_function_table.
   inline void register_wasi(host_function_table& table) {
      #define WASI_REG(name) \
         add_wasi_func<&wasi_host::name>(table, #name)

      WASI_REG(args_get);
      WASI_REG(args_sizes_get);
      WASI_REG(environ_get);
      WASI_REG(environ_sizes_get);
      WASI_REG(clock_res_get);
      WASI_REG(clock_time_get);
      WASI_REG(fd_advise);
      WASI_REG(fd_allocate);
      WASI_REG(fd_close);
      WASI_REG(fd_datasync);
      WASI_REG(fd_fdstat_get);
      WASI_REG(fd_fdstat_set_flags);
      WASI_REG(fd_fdstat_set_rights);
      WASI_REG(fd_filestat_get);
      WASI_REG(fd_filestat_set_size);
      WASI_REG(fd_filestat_set_times);
      WASI_REG(fd_pread);
      WASI_REG(fd_prestat_get);
      WASI_REG(fd_prestat_dir_name);
      WASI_REG(fd_pwrite);
      WASI_REG(fd_read);
      WASI_REG(fd_readdir);
      WASI_REG(fd_renumber);
      WASI_REG(fd_seek);
      WASI_REG(fd_sync);
      WASI_REG(fd_tell);
      WASI_REG(fd_write);
      WASI_REG(path_create_directory);
      WASI_REG(path_filestat_get);
      WASI_REG(path_filestat_set_times);
      WASI_REG(path_link);
      WASI_REG(path_open);
      WASI_REG(path_readlink);
      WASI_REG(path_remove_directory);
      WASI_REG(path_rename);
      WASI_REG(path_symlink);
      WASI_REG(path_unlink_file);
      WASI_REG(poll_oneoff);
      WASI_REG(proc_exit);
      WASI_REG(proc_raise);
      WASI_REG(random_get);
      WASI_REG(sched_yield);

      #undef WASI_REG
   }

} // namespace psizam::detail
