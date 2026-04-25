#pragma once
//
// psio3/validate_strict_walker.hpp — semantic-spec validation walker.
//
// Implements the validate_strict half of design §5.3.3 / §5.4: after a
// format's structural validate succeeds, decode the value and walk the
// reflected fields invoking each spec's `validate(span<const char>)`
// member on the field's payload bytes. Returns the first failure.
//
// Per design §5.3.3: spec types may optionally define
//   static codec_status validate(std::span<const char>) noexcept;
// Specs without this member are ignored — the walker is open to
// third-party spec types in user code (no library modification needed).
//
// Format integration: each format's `tag_invoke(validate_strict<T>, …)`
// is a thin wrapper:
//   1. structural validate (precondition for sound decode)
//   2. decode<T>
//   3. validate_specs_on_value<T>(decoded)
// The walker is format-agnostic — it works on the typed value,
// extracting field byte ranges from the C++ representation rather
// than from the wire (the spec interpretation only depends on the
// payload bytes, not framing).
//
// Scope of v1:
//   - Top-level fields of T whose type is std::string (the common
//     ByteString shape carrier). Spec validate receives the string
//     bytes via std::span<const char>{s.data(), s.size()}.
//   - Specs whose validate signature is `(span<const char>) noexcept`.
// Out of scope (follow-ups):
//   - Recursing into nested records, vectors of strings, optionals.
//   - sorted_spec / length_bound semantic validation (these benefit
//     from typed access, which the (span) signature doesn't allow).

#include <psio3/error.hpp>
#include <psio3/reflect.hpp>
#include <psio3/wrappers.hpp>  // effective_annotations_for

#include <cstddef>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio3 {

   namespace detail::vstrict {

      // SFINAE: does S have a static validate(span<const char>) member
      // returning codec_status?
      template <typename S, typename = void>
      struct has_span_validate : std::false_type {};

      template <typename S>
      struct has_span_validate<
         S,
         std::void_t<decltype(S::validate(std::span<const char>{}))>>
         : std::is_same<decltype(S::validate(std::span<const char>{})),
                         codec_status>
      {
      };

      template <typename S>
      inline constexpr bool has_span_validate_v = has_span_validate<S>::value;

      // Run every spec's validate(span) on `bytes`. Returns first failure.
      template <typename Tuple>
      codec_status run_specs(std::span<const char> bytes,
                              const Tuple& tup) noexcept
      {
         codec_status err = codec_ok();
         std::apply(
            [&](const auto&... specs) {
               (
                  ([&]() {
                     using S = std::remove_cvref_t<decltype(specs)>;
                     if (!err.ok())
                        return;
                     if constexpr (has_span_validate_v<S>)
                     {
                        codec_status s = S::validate(bytes);
                        if (!s.ok())
                           err = s;
                     }
                  }()),
                  ...);
            },
            tup);
         return err;
      }

      // Per-field handler: extracts the field's payload bytes for the
      // shapes we currently support, then runs the spec set against them.
      template <typename T, std::size_t I>
      codec_status validate_field(const T& value) noexcept
      {
         using R       = ::psio3::reflect<T>;
         using F       = typename R::template member_type<I>;
         constexpr auto MemPtr = R::template member_pointer<I>;
         using EA = ::psio3::effective_annotations_for<T, F, MemPtr>;
         constexpr auto annotations = EA::value;

         const F& field_val = value.*MemPtr;

         if constexpr (std::is_same_v<F, std::string>)
         {
            std::span<const char> bytes{field_val.data(), field_val.size()};
            return run_specs(bytes, annotations);
         }
         else
         {
            // Other shapes (vec<u8>, nested records, vec<string>) are
            // not yet covered — design §5.3.3 v1 scope is ByteString
            // payloads, which all encode the same way regardless of
            // format. Returning ok keeps validate_strict total.
            (void)field_val;
            return codec_ok();
         }
      }

   }  // namespace detail::vstrict

   // Public entry point: walk every reflected field of `value` and run
   // each spec's `validate(span)` against the field's payload bytes.
   // Returns the first failure, or codec_ok() if everything passes.
   template <typename T>
   [[nodiscard]] inline codec_status
   validate_specs_on_value(const T& value) noexcept
   {
      using R                 = ::psio3::reflect<T>;
      constexpr std::size_t N = R::member_count;
      return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         codec_status err = codec_ok();
         ((err.ok()
              ? (err = detail::vstrict::validate_field<T, Is>(value),
                 void())
              : void()),
          ...);
         return err;
      }(std::make_index_sequence<N>{});
   }

}  // namespace psio3
