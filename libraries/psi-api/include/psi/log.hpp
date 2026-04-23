#pragma once

// psi/log.hpp — Guest-side console logging façade.
//
// Wraps the raw `psi.log` host import (declared in <psi/host.h>) with
// typed C++ entry points so services can do:
//
//     psi::log::info("booting {}", name);
//     psi::log::warn("unexpected path: {}", path);
//
// The host routes each level to its matching quill severity and tags
// the source with the process name, so the call site is visible in
// the server log without any extra plumbing.

#include <cstdint>
#include <string>
#include <string_view>

#ifdef __wasm__
extern "C" __attribute__((import_module("psi"), import_name("log")))
void psi_log(int level, const void* msg, int len);
#else
inline void psi_log(int, const void*, int) {}
#endif

namespace psi::log
{
   enum class level : int32_t
   {
      debug = 0,
      info  = 1,
      warn  = 2,
      error = 3,
   };

   /// Send a pre-rendered message to the host logger at the given level.
   inline void write(level lvl, std::string_view msg)
   {
      ::psi_log(static_cast<int>(lvl), msg.data(),
                static_cast<int>(msg.size()));
   }

   inline void debug(std::string_view msg) { write(level::debug, msg); }
   inline void info (std::string_view msg) { write(level::info , msg); }
   inline void warn (std::string_view msg) { write(level::warn , msg); }
   inline void error(std::string_view msg) { write(level::error, msg); }

   // ── Simple {} formatter ─────────────────────────────────────────
   //
   // Avoids dragging <format> (libc++'s WASI std::format is heavy and
   // triggers weak-symbol issues) and gives guests a tiny, allocation-
   // light formatter that covers the common bring-up use cases:
   // strings, string_views, signed/unsigned ints.
   namespace detail
   {
      inline void append_uint(std::string& out, uint64_t v)
      {
         char buf[21];
         int  n = 0;
         if (v == 0)
            buf[n++] = '0';
         else
            while (v) { buf[n++] = char('0' + v % 10); v /= 10; }
         for (int i = n - 1; i >= 0; --i) out.push_back(buf[i]);
      }

      inline void append_int(std::string& out, int64_t v)
      {
         if (v < 0) { out.push_back('-'); v = -v; }
         append_uint(out, static_cast<uint64_t>(v));
      }

      template <typename T>
      void append_arg(std::string& out, const T& a)
      {
         if constexpr (std::is_same_v<T, std::string_view> ||
                       std::is_same_v<T, std::string>)
            out.append(a.data(), a.size());
         else if constexpr (std::is_convertible_v<T, const char*>)
            out.append(static_cast<const char*>(a));
         else if constexpr (std::is_signed_v<T>)
            append_int(out, static_cast<int64_t>(a));
         else if constexpr (std::is_unsigned_v<T>)
            append_uint(out, static_cast<uint64_t>(a));
         else
            static_assert(sizeof(T) == 0,
                          "psi::log supports strings and integers only");
      }

      inline void format_into(std::string& out, std::string_view fmt)
      {
         out.append(fmt.data(), fmt.size());
      }

      template <typename Arg, typename... Rest>
      void format_into(std::string& out, std::string_view fmt,
                       const Arg& a, const Rest&... rest)
      {
         auto pos = fmt.find("{}");
         if (pos == std::string_view::npos)
         {
            out.append(fmt.data(), fmt.size());
            return;
         }
         out.append(fmt.data(), pos);
         append_arg(out, a);
         format_into(out, fmt.substr(pos + 2), rest...);
      }
   }  // namespace detail

   template <typename... Args>
   void info(std::string_view fmt, const Args&... args)
   {
      std::string s;
      detail::format_into(s, fmt, args...);
      write(level::info, s);
   }

   template <typename... Args>
   void warn(std::string_view fmt, const Args&... args)
   {
      std::string s;
      detail::format_into(s, fmt, args...);
      write(level::warn, s);
   }

   template <typename... Args>
   void error(std::string_view fmt, const Args&... args)
   {
      std::string s;
      detail::format_into(s, fmt, args...);
      write(level::error, s);
   }

   template <typename... Args>
   void debug(std::string_view fmt, const Args&... args)
   {
      std::string s;
      detail::format_into(s, fmt, args...);
      write(level::debug, s);
   }

}  // namespace psi::log
