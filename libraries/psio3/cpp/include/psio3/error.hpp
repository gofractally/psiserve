#pragma once
//
// psio3/error.hpp — error model (design doc § 5.4).
//
// `psio3::validate` is the only operation that can report an error.
// Every other operation (encode, decode, size, make_boxed, view
// accessors) is `noexcept` and assumes its input has been validated.
//
// Return type: `[[nodiscard]] psio3::codec_status` — a lightweight
// result type the compiler refuses to let users silently drop. Under
// `-Werror=unused-result` (enabled on the psio3 target) this makes
// "did you validate?" a compile-time invariant. The same API works
// with `-fno-exceptions`; only the opt-in `.or_throw()` helper
// disappears.

#include <cstdint>
#include <string_view>

#if defined(PSIO3_EXCEPTIONS_ENABLED) && PSIO3_EXCEPTIONS_ENABLED
#include <stdexcept>
#include <string>
#endif

namespace psio3 {

   // Structured error payload. No allocation — static strings only.
   struct codec_error
   {
      std::string_view what{};          // static message, e.g. "offset out of range"
      std::uint32_t    byte_offset = 0; // where in the buffer the problem was
      std::string_view format_name{};   // "ssz", "frac", etc.

      constexpr codec_error() = default;
      constexpr codec_error(std::string_view w, std::uint32_t off,
                            std::string_view fmt) noexcept
         : what(w), byte_offset(off), format_name(fmt) {}
   };

#if defined(PSIO3_EXCEPTIONS_ENABLED) && PSIO3_EXCEPTIONS_ENABLED
   // Exception type thrown by .or_throw(). Derives from runtime_error
   // so generic catch(std::exception&) works.
   class codec_exception : public std::runtime_error
   {
      codec_error err_;

    public:
      explicit codec_exception(codec_error e)
         : std::runtime_error(std::string{e.what}),
           err_(e) {}

      const codec_error& error() const noexcept { return err_; }
   };
#endif

   // ── codec_status ──────────────────────────────────────────────────────
   //
   // Semantics:
   //   - ok() / operator bool() → true if no error
   //   - error()                 → access the codec_error (only when !ok)
   //   - or_throw()              → throw codec_exception if error (only
   //                               under PSIO3_EXCEPTIONS_ENABLED)
   //
   // [[nodiscard]] at the class level + -Werror=unused-result in CI
   // means unchecked statuses fail to compile.
   class [[nodiscard]] codec_status
   {
      codec_error err_{};
      bool        has_err_ = false;

    public:
      constexpr codec_status() noexcept = default;

      // Error constructor. Use the out-of-class `codec_fail()` factory
      // for readability.
      constexpr explicit codec_status(codec_error e) noexcept
         : err_(e), has_err_(true) {}

      constexpr bool ok() const noexcept { return !has_err_; }
      constexpr explicit operator bool() const noexcept { return ok(); }

      // Valid only when !ok(). UB to call otherwise — callers gate on
      // ok() / operator bool first.
      constexpr const codec_error& error() const noexcept { return err_; }

#if defined(PSIO3_EXCEPTIONS_ENABLED) && PSIO3_EXCEPTIONS_ENABLED
      // Opt-in exception path. Users who WANT exceptions call .or_throw()
      // explicitly; nothing in the library throws without being asked.
      void or_throw() const
      {
         if (has_err_) throw codec_exception{err_};
      }
#endif
   };

   // Factory for the error path — reads cleaner than
   // `codec_status{codec_error{...}}`.
   constexpr codec_status codec_fail(std::string_view what,
                                     std::uint32_t    byte_offset,
                                     std::string_view format_name) noexcept
   {
      return codec_status{codec_error{what, byte_offset, format_name}};
   }

   // Factory for success.
   constexpr codec_status codec_ok() noexcept { return codec_status{}; }

}  // namespace psio3
