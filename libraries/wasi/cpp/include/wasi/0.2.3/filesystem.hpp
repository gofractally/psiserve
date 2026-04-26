#pragma once

// wasi:filesystem@0.2.3 — types and preopens interfaces.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/filesystem/types.wit
//   libraries/wasi/wit/0.2.3/filesystem/preopens.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime — psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <psio1/reflect.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_resource.hpp>

#include <wasi/0.2.3/io.hpp>
#include <wasi/0.2.3/clocks.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// =====================================================================
// wasi:filesystem/types — type aliases and enums
// =====================================================================

/// File size or length of a region within a file.
using filesize = uint64_t;

/// Number of hard links to an inode.
using link_count = uint64_t;

/// The type of a filesystem object referenced by a descriptor.
enum class descriptor_type : uint8_t
{
   unknown,
   block_device,
   character_device,
   directory,
   fifo,
   symbolic_link,
   regular_file,
   socket,
};
PSIO1_REFLECT(descriptor_type)

/// Descriptor flags (bitfield).
struct descriptor_flags
{
   bool read                 = false;
   bool write                = false;
   bool file_integrity_sync  = false;
   bool data_integrity_sync  = false;
   bool requested_write_sync = false;
   bool mutate_directory     = false;
};
PSIO1_REFLECT(descriptor_flags,
             read, write, file_integrity_sync, data_integrity_sync,
             requested_write_sync, mutate_directory)

/// File attributes.
struct descriptor_stat
{
   descriptor_type           type                        = descriptor_type::unknown;
   link_count                link_count_val              = 0;
   filesize                  size                        = 0;
   std::optional<datetime>   data_access_timestamp       = std::nullopt;
   std::optional<datetime>   data_modification_timestamp = std::nullopt;
   std::optional<datetime>   status_change_timestamp     = std::nullopt;
};
PSIO1_REFLECT(descriptor_stat,
             type, link_count_val, size,
             data_access_timestamp, data_modification_timestamp,
             status_change_timestamp)

/// Flags determining the method of how paths are resolved.
struct path_flags
{
   bool symlink_follow = false;
};
PSIO1_REFLECT(path_flags, symlink_follow)

/// Open flags used by open-at.
struct open_flags
{
   bool create    = false;
   bool directory = false;
   bool exclusive = false;
   bool truncate  = false;
};
PSIO1_REFLECT(open_flags, create, directory, exclusive, truncate)

/// When setting a timestamp, this gives the value to set it to.
struct new_timestamp
{
   enum tag_t : uint8_t
   {
      no_change,
      now,
      timestamp
   };
   tag_t    tag  = no_change;
   datetime value{};
};
PSIO1_REFLECT(new_timestamp, tag, value)

/// A directory entry.
struct directory_entry
{
   descriptor_type type = descriptor_type::unknown;
   std::string     name;
};
PSIO1_REFLECT(directory_entry, type, name)

/// Error codes returned by functions, similar to errno in POSIX.
enum class filesystem_error_code : uint8_t
{
   access,
   would_block,
   already,
   bad_descriptor,
   busy,
   deadlock,
   quota,
   exist,
   file_too_large,
   illegal_byte_sequence,
   in_progress,
   interrupted,
   invalid,
   io,
   is_directory,
   loop,
   too_many_links,
   message_size,
   name_too_long,
   no_device,
   no_entry,
   no_lock,
   insufficient_memory,
   insufficient_space,
   not_directory,
   not_empty,
   not_recoverable,
   unsupported,
   no_tty,
   no_such_device,
   overflow,
   not_permitted,
   pipe,
   read_only,
   invalid_seek,
   text_file_busy,
   cross_device,
};
PSIO1_REFLECT(filesystem_error_code)

/// File or memory access pattern advisory information.
enum class advice : uint8_t
{
   normal,
   sequential,
   random,
   will_need,
   dont_need,
   no_reuse,
};
PSIO1_REFLECT(advice)

/// A 128-bit hash value, split into parts.
struct metadata_hash_value
{
   uint64_t lower = 0;
   uint64_t upper = 0;
};
PSIO1_REFLECT(metadata_hash_value, lower, upper)

// ── WIT result<T, error-code> as std::variant ───────────────────────
// index 0 = ok, index 1 = err (filesystem_error_code)

template <typename T>
using fs_result = std::variant<T, filesystem_error_code>;

using fs_result_void = std::variant<std::monostate, filesystem_error_code>;

namespace fs_detail {
   template <typename T>
   fs_result<T> ok(T value) { return fs_result<T>{std::in_place_index<0>, std::move(value)}; }

   inline fs_result_void ok() { return fs_result_void{std::in_place_index<0>}; }

   struct fs_err_proxy
   {
      filesystem_error_code ec;
      template <typename T>
      operator fs_result<T>() const { return fs_result<T>{std::in_place_index<1>, ec}; }
      operator fs_result_void() const { return fs_result_void{std::in_place_index<1>, ec}; }
   };

   inline fs_err_proxy err(filesystem_error_code e) { return {e}; }
}

// =====================================================================
// wasi:filesystem/types — resource descriptor
// =====================================================================

struct descriptor : psio1::wit_resource {};
PSIO1_REFLECT(descriptor)

// =====================================================================
// wasi:filesystem/types — resource directory-entry-stream
// =====================================================================

struct directory_entry_stream : psio1::wit_resource {};
PSIO1_REFLECT(directory_entry_stream)

// =====================================================================
// Interface: wasi:filesystem/types
// =====================================================================

struct wasi_filesystem_types
{
   // ── descriptor methods ────────────────────────────────────────────

   // [method] descriptor.read-via-stream: func(offset: filesize) -> result<input-stream, error-code>
   static inline fs_result<psio1::own<input_stream>> descriptor_read_via_stream(
       psio1::borrow<descriptor> /*self*/,
       filesize /*offset*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.write-via-stream: func(offset: filesize) -> result<output-stream, error-code>
   static inline fs_result<psio1::own<output_stream>> descriptor_write_via_stream(
       psio1::borrow<descriptor> /*self*/,
       filesize /*offset*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.append-via-stream: func() -> result<output-stream, error-code>
   static inline fs_result<psio1::own<output_stream>> descriptor_append_via_stream(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.advise: func(offset: filesize, length: filesize, advice: advice) -> result<_, error-code>
   static inline fs_result_void descriptor_advise(
       psio1::borrow<descriptor> /*self*/,
       filesize /*offset*/,
       filesize /*length*/,
       advice /*advice*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.sync-data: func() -> result<_, error-code>
   static inline fs_result_void descriptor_sync_data(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.get-flags: func() -> result<descriptor-flags, error-code>
   static inline fs_result<descriptor_flags> descriptor_get_flags(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.get-type: func() -> result<descriptor-type, error-code>
   static inline fs_result<descriptor_type> descriptor_get_type(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.set-size: func(size: filesize) -> result<_, error-code>
   static inline fs_result_void descriptor_set_size(
       psio1::borrow<descriptor> /*self*/,
       filesize /*size*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.set-times: func(data-access-timestamp: new-timestamp, data-modification-timestamp: new-timestamp) -> result<_, error-code>
   static inline fs_result_void descriptor_set_times(
       psio1::borrow<descriptor> /*self*/,
       new_timestamp /*data_access_timestamp*/,
       new_timestamp /*data_modification_timestamp*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.read: func(length: filesize, offset: filesize) -> result<tuple<list<u8>, bool>, error-code>
   static inline fs_result<std::tuple<std::vector<uint8_t>, bool>> descriptor_read(
       psio1::borrow<descriptor> /*self*/,
       filesize /*length*/,
       filesize /*offset*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.write: func(buffer: list<u8>, offset: filesize) -> result<filesize, error-code>
   static inline fs_result<filesize> descriptor_write(
       psio1::borrow<descriptor> /*self*/,
       std::vector<uint8_t> /*buffer*/,
       filesize /*offset*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.read-directory: func() -> result<directory-entry-stream, error-code>
   static inline fs_result<psio1::own<directory_entry_stream>> descriptor_read_directory(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.sync: func() -> result<_, error-code>
   static inline fs_result_void descriptor_sync(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.create-directory-at: func(path: string) -> result<_, error-code>
   static inline fs_result_void descriptor_create_directory_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.stat: func() -> result<descriptor-stat, error-code>
   static inline fs_result<descriptor_stat> descriptor_stat_fn(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.stat-at: func(path-flags: path-flags, path: string) -> result<descriptor-stat, error-code>
   static inline fs_result<descriptor_stat> descriptor_stat_at(
       psio1::borrow<descriptor> /*self*/,
       path_flags /*path_flags*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.set-times-at: func(path-flags, path, data-access-timestamp, data-modification-timestamp) -> result<_, error-code>
   static inline fs_result_void descriptor_set_times_at(
       psio1::borrow<descriptor> /*self*/,
       path_flags /*path_flags*/,
       std::string /*path*/,
       new_timestamp /*data_access_timestamp*/,
       new_timestamp /*data_modification_timestamp*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.link-at: func(old-path-flags, old-path, new-descriptor, new-path) -> result<_, error-code>
   static inline fs_result_void descriptor_link_at(
       psio1::borrow<descriptor> /*self*/,
       path_flags /*old_path_flags*/,
       std::string /*old_path*/,
       psio1::borrow<descriptor> /*new_descriptor*/,
       std::string /*new_path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.open-at: func(path-flags, path, open-flags, flags) -> result<descriptor, error-code>
   static inline fs_result<psio1::own<descriptor>> descriptor_open_at(
       psio1::borrow<descriptor> /*self*/,
       path_flags /*path_flags*/,
       std::string /*path*/,
       open_flags /*open_flags*/,
       descriptor_flags /*flags*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.readlink-at: func(path: string) -> result<string, error-code>
   static inline fs_result<std::string> descriptor_readlink_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.remove-directory-at: func(path: string) -> result<_, error-code>
   static inline fs_result_void descriptor_remove_directory_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.rename-at: func(old-path, new-descriptor, new-path) -> result<_, error-code>
   static inline fs_result_void descriptor_rename_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*old_path*/,
       psio1::borrow<descriptor> /*new_descriptor*/,
       std::string /*new_path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.symlink-at: func(old-path, new-path) -> result<_, error-code>
   static inline fs_result_void descriptor_symlink_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*old_path*/,
       std::string /*new_path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.unlink-file-at: func(path: string) -> result<_, error-code>
   static inline fs_result_void descriptor_unlink_file_at(
       psio1::borrow<descriptor> /*self*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.is-same-object: func(other: borrow<descriptor>) -> bool
   static inline bool descriptor_is_same_object(
       psio1::borrow<descriptor> /*self*/,
       psio1::borrow<descriptor> /*other*/)
   {
      return false;
   }

   // [method] descriptor.metadata-hash: func() -> result<metadata-hash-value, error-code>
   static inline fs_result<metadata_hash_value> descriptor_metadata_hash(
       psio1::borrow<descriptor> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // [method] descriptor.metadata-hash-at: func(path-flags, path) -> result<metadata-hash-value, error-code>
   static inline fs_result<metadata_hash_value> descriptor_metadata_hash_at(
       psio1::borrow<descriptor> /*self*/,
       path_flags /*path_flags*/,
       std::string /*path*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // ── directory-entry-stream methods ────────────────────────────────

   // [method] directory-entry-stream.read-directory-entry: func() -> result<option<directory-entry>, error-code>
   static inline fs_result<std::optional<directory_entry>> directory_entry_stream_read_directory_entry(
       psio1::borrow<directory_entry_stream> /*self*/)
   {
      return fs_detail::err(filesystem_error_code::bad_descriptor);
   }

   // ── free functions ────────────────────────────────────────────────

   // filesystem-error-code: func(err: borrow<error>) -> option<error-code>
   static inline std::optional<filesystem_error_code> filesystem_error_code_fn(
       psio1::borrow<io_error> /*err*/)
   {
      return std::nullopt;
   }
};

// =====================================================================
// Interface: wasi:filesystem/preopens
// =====================================================================

struct wasi_filesystem_preopens
{
   // get-directories: func() -> list<tuple<descriptor, string>>
   static inline std::vector<std::tuple<psio1::own<descriptor>, std::string>> get_directories()
   {
      return {};
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO1_PACKAGE(wasi_filesystem, "0.2.3");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(wasi_filesystem)

PSIO1_INTERFACE(wasi_filesystem_types,
               types(descriptor_type, descriptor_flags, descriptor_stat,
                     path_flags, open_flags, new_timestamp, directory_entry,
                     filesystem_error_code, advice, metadata_hash_value,
                     descriptor, directory_entry_stream),
               funcs(func(descriptor_read_via_stream, self, offset),
                     func(descriptor_write_via_stream, self, offset),
                     func(descriptor_append_via_stream, self),
                     func(descriptor_advise, self, offset, length, advice),
                     func(descriptor_sync_data, self),
                     func(descriptor_get_flags, self),
                     func(descriptor_get_type, self),
                     func(descriptor_set_size, self, size),
                     func(descriptor_set_times, self, data_access_timestamp, data_modification_timestamp),
                     func(descriptor_read, self, length, offset),
                     func(descriptor_write, self, buffer, offset),
                     func(descriptor_read_directory, self),
                     func(descriptor_sync, self),
                     func(descriptor_create_directory_at, self, path),
                     func(descriptor_stat_fn, self),
                     func(descriptor_stat_at, self, path_flags, path),
                     func(descriptor_set_times_at, self, path_flags, path, data_access_timestamp, data_modification_timestamp),
                     func(descriptor_link_at, self, old_path_flags, old_path, new_descriptor, new_path),
                     func(descriptor_open_at, self, path_flags, path, open_flags, flags),
                     func(descriptor_readlink_at, self, path),
                     func(descriptor_remove_directory_at, self, path),
                     func(descriptor_rename_at, self, old_path, new_descriptor, new_path),
                     func(descriptor_symlink_at, self, old_path, new_path),
                     func(descriptor_unlink_file_at, self, path),
                     func(descriptor_is_same_object, self, other),
                     func(descriptor_metadata_hash, self),
                     func(descriptor_metadata_hash_at, self, path_flags, path),
                     func(directory_entry_stream_read_directory_entry, self),
                     func(filesystem_error_code_fn, err)))

PSIO1_INTERFACE(wasi_filesystem_preopens,
               types(),
               funcs(func(get_directories)))
