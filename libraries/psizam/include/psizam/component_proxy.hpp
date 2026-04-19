#pragma once

// Canonical-ABI dispatch primitives: flat_val slot type, the
// export_lift/lower policies that satisfy {Lift,Lower}Policy, and
// ComponentProxy<T> — the reflective invoker that routes 16 flat
// values into a member function and lowers the result.
//
// Extracted from psizam/component.hpp so guest-only translation units
// can include it without dragging in <psio/wit_gen.hpp> /
// <psio/wit_encode.hpp>. Those pull in typeid and std::runtime_error
// throws (for WIT binary emission), which are both incompatible with
// the guest's `-fno-exceptions -fno-rtti` default.
//
// component.hpp still re-exports everything here and additionally
// provides generate_component_wit*; new code targeting guest thunks
// should include this header directly.

#include <psizam/canonical_dispatch.hpp>
#include <psio/wit_owned.hpp>

#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __wasm__
extern "C" void* cabi_realloc(void* old_ptr, std::size_t old_size,
                              std::size_t align, std::size_t new_size);
#endif

namespace psizam {

   // ── Flat ABI slot type ────────────────────────────────────────────────────
   // All flat ABI values are passed as int64_t (widest scalar) in our convention.
   // The canonical ABI specifies i32/i64/f32/f64, but since we control both
   // host and guest, we use int64_t as a universal envelope.
   using flat_val = int64_t;

   // ── Lift policy for component export args ─────────────────────────────────
   // Reads canonical ABI flat values from an int64_t array (the 16 export params).
   // Satisfies LiftPolicy. For memory access (strings, vectors), resolves offsets
   // through a configurable base pointer:
   //   - mem_base != nullptr: offsets are relative to mem_base (native testing)
   //   - mem_base == nullptr: offsets treated as native pointers (WASM context)

   struct export_lift_policy {
      const int64_t* slots;
      size_t         idx = 0;
      const uint8_t* mem_base;

      explicit export_lift_policy(const int64_t* s, const uint8_t* mem = nullptr)
         : slots(s), mem_base(mem) {}

      uint32_t next_i32() { return static_cast<uint32_t>(slots[idx++]); }
      uint64_t next_i64() { return static_cast<uint64_t>(slots[idx++]); }

      float next_f32() {
         union { int32_t i; float f; } u;
         u.i = static_cast<int32_t>(slots[idx++]);
         return u.f;
      }

      double next_f64() {
         union { int64_t i; double f; } u;
         u.i = slots[idx++];
         return u.f;
      }

      const uint8_t* resolve(uint32_t off) const {
         return mem_base ? (mem_base + off)
                         : reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(off));
      }

      uint8_t  load_u8(uint32_t off)  { return resolve(off)[0]; }
      uint16_t load_u16(uint32_t off) { uint16_t v; std::memcpy(&v, resolve(off), 2); return v; }
      uint32_t load_u32(uint32_t off) { uint32_t v; std::memcpy(&v, resolve(off), 4); return v; }
      uint64_t load_u64(uint32_t off) { uint64_t v; std::memcpy(&v, resolve(off), 8); return v; }
      float    load_f32(uint32_t off) { float v; std::memcpy(&v, resolve(off), 4); return v; }
      double   load_f64(uint32_t off) { double v; std::memcpy(&v, resolve(off), 8); return v; }
      const char* load_bytes(uint32_t off, uint32_t) {
         return reinterpret_cast<const char*>(resolve(off));
      }
   };

   // ── Lower policy for component export results ─────────────────────────────
   // Writes canonical ABI flat values to a psio::native_value array.
   // Satisfies LowerPolicy. For memory allocation (string/vector data in results),
   // uses a bump allocator into an internal buffer.

   struct export_lower_policy {
      psio::native_value results[16] = {};
      size_t       result_count = 0;

      // Bump allocator for result data (string/vector contents)
      std::vector<uint8_t> result_buf;
      uint32_t             bump = 0;

      uint32_t alloc(uint32_t align, uint32_t size) {
         bump = (bump + align - 1) & ~(align - 1);
         uint32_t ptr = bump;
         bump += size;
         if (result_buf.size() < bump)
            result_buf.resize(bump);
         return ptr;
      }

      void store_u8(uint32_t off, uint8_t v)   { result_buf[off] = v; }
      void store_u16(uint32_t off, uint16_t v)  { std::memcpy(&result_buf[off], &v, 2); }
      void store_u32(uint32_t off, uint32_t v)  { std::memcpy(&result_buf[off], &v, 4); }
      void store_u64(uint32_t off, uint64_t v)  { std::memcpy(&result_buf[off], &v, 8); }
      void store_f32(uint32_t off, float v)     { std::memcpy(&result_buf[off], &v, 4); }
      void store_f64(uint32_t off, double v)    { std::memcpy(&result_buf[off], &v, 8); }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (len > 0) std::memcpy(&result_buf[off], data, len);
      }

      void emit_i32(uint32_t v) { psio::native_value nv; nv.i64 = 0; nv.i32 = v; results[result_count++] = nv; }
      void emit_i64(uint64_t v) { psio::native_value nv; nv.i64 = v; results[result_count++] = nv; }
      void emit_f32(float v)    { psio::native_value nv; nv.i64 = 0; nv.f32 = v; results[result_count++] = nv; }
      void emit_f64(double v)   { psio::native_value nv; nv.i64 = 0; nv.f64 = v; results[result_count++] = nv; }
   };

   // ── Return-area StorePolicy for multi-slot export results ─────────────────
   // A record/vector/optional return whose flat count > 1 must spill to a
   // return area: the callee cabi_realloc's a chunk of canonical_size<T> bytes,
   // writes the root record at offset 0, and returns the pointer. Nested
   // data (list contents, string bytes) is allocated via cabi_realloc too,
   // with the embedded pointers written as i32 offsets into linear memory.
   //
   // On wasm32 a pointer and a linear-memory offset are interchangeable
   // 32-bit values, so we just `reinterpret_cast<uintptr_t>` the result of
   // cabi_realloc. On the host-side native test path, the policy never
   // allocates (it's a guest-only code path in practice).

   struct guest_return_area_policy {
      uint8_t* base;

      uint32_t alloc(uint32_t align, uint32_t size) {
#ifdef __wasm__
         void* p = ::cabi_realloc(nullptr, 0, align, size);
         return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
#else
         (void)align; (void)size;
         return 0;
#endif
      }

      void store_u8(uint32_t off, uint8_t v)   { base[off] = v; }
      void store_u16(uint32_t off, uint16_t v) { std::memcpy(base + off, &v, 2); }
      void store_u32(uint32_t off, uint32_t v) { std::memcpy(base + off, &v, 4); }
      void store_u64(uint32_t off, uint64_t v) { std::memcpy(base + off, &v, 8); }
      void store_f32(uint32_t off, float v)    { std::memcpy(base + off, &v, 4); }
      void store_f64(uint32_t off, double v)   { std::memcpy(base + off, &v, 8); }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (len > 0) std::memcpy(base + off, data, len);
      }
   };

   // ── Method flat count computation ─────────────────────────────────────────

   namespace detail_component {

      template <typename... Args>
      constexpr size_t param_flat_count(psio::TypeList<Args...>) {
         return (0 + ... + psizam::flat_count_v<Args>);
      }

      template <typename T>
      constexpr size_t result_flat_count() {
         if constexpr (std::is_void_v<T>)
            return 0;
         else
            return psizam::flat_count_v<T>;
      }

   } // namespace detail_component

   // ── ComponentProxy<T> — canonical ABI dispatch ────────────────────────────
   //
   // Dispatches a flat-arg call to a specific method on T using canonical
   // lift/lower. The template parameter MemPtr carries all type information.

   template <typename T>
   struct ComponentProxy {

      /// Call a method given 16 flat args (no memory context — scalar methods only).
      template <auto MemPtr>
      static flat_val call(T* impl,
                           flat_val a0,  flat_val a1,  flat_val a2,  flat_val a3,
                           flat_val a4,  flat_val a5,  flat_val a6,  flat_val a7,
                           flat_val a8,  flat_val a9,  flat_val a10, flat_val a11,
                           flat_val a12, flat_val a13, flat_val a14, flat_val a15)
      {
         flat_val slots[16] = {a0, a1, a2, a3, a4, a5, a6, a7,
                               a8, a9, a10, a11, a12, a13, a14, a15};
         return call_with_memory<MemPtr>(impl, slots, nullptr);
      }

      /// Call with explicit memory base (for dispatching methods with complex types).
      /// The memory parameter provides the base for resolving i32 offsets to string/
      /// vector data (as produced by buffer_lower_policy).
      template <auto MemPtr>
      static flat_val call_with_memory(T* impl, const flat_val* slots, const uint8_t* memory) {
         using MType    = psio::MemberPtrType<decltype(MemPtr)>;
         using ArgTypes = typename MType::SimplifiedArgTypes;

         constexpr size_t pcnt = detail_component::param_flat_count(ArgTypes{});
         static_assert(pcnt <= psio::MAX_FLAT_PARAMS,
            "Method exceeds psio::MAX_FLAT_PARAMS (16). Spilled args not yet supported.");

         // Lift all args from flat values using canonical ABI rules
         export_lift_policy lift_p(slots, memory);
         auto arg_tuple = lift_args(lift_p, ArgTypes{});

         // Call method and lower the result
         return invoke_and_lower<MemPtr>(impl, arg_tuple,
            std::make_index_sequence<std::tuple_size_v<decltype(arg_tuple)>>{});
      }

   private:
      template <psizam::LiftPolicy Policy, typename... Args>
      static auto lift_args(Policy& p, psio::TypeList<Args...>) {
         return std::tuple{psizam::canonical_lift_flat<std::remove_cvref_t<Args>>(p)...};
      }

      template <auto MemPtr, typename Tuple, size_t... Is>
      static flat_val invoke_and_lower(T* impl, Tuple& args, std::index_sequence<Is...>) {
         using MType      = psio::MemberPtrType<decltype(MemPtr)>;
         using ReturnType = typename MType::ReturnType;

         if constexpr (std::is_void_v<ReturnType>) {
            (impl->*MemPtr)(std::get<Is>(args)...);
            return 0;
         } else if constexpr (std::is_same_v<ReturnType, psio::owned<std::string, psio::wit>>
                              || detail_dispatch::is_wit_vector<ReturnType>::value) {
            // wit::string / wit::vector<T> — the payload buffer already
            // lives in linear memory (cabi_realloc-owned), so we just
            // spill an 8-byte return area with (ptr, len) and return
            // the pointer to it. No recursive canonical_lower_fields.
            auto result = (impl->*MemPtr)(std::get<Is>(args)...);
            auto [s_ptr, s_len] = result.release();
#ifdef __wasm__
            uint32_t* ret_area =
               static_cast<uint32_t*>(::cabi_realloc(nullptr, 0, 4, 8));
#else
            static uint32_t fallback[2];
            uint32_t* ret_area = fallback;
#endif
            ret_area[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_ptr));
            ret_area[1] = static_cast<uint32_t>(s_len);
            return static_cast<flat_val>(reinterpret_cast<uintptr_t>(ret_area));
         } else {
            constexpr size_t rflat = psizam::flat_count_v<ReturnType>;
            auto result = (impl->*MemPtr)(std::get<Is>(args)...);
            if constexpr (rflat <= psio::MAX_FLAT_RESULTS) {
               // Single-slot flat return: lower directly into slot 0.
               export_lower_policy lower_p;
               psizam::canonical_lower_flat(result, lower_p);
               return static_cast<flat_val>(lower_p.results[0].i64);
            } else {
               // Multi-slot return: spill to a cabi_realloc'd return area and
               // return an i32 pointer to it. Nested data (list contents,
               // string bytes) each get their own cabi_realloc allocation;
               // the embedded i32 "pointers" we write are wasm linear-memory
               // addresses, which the host reads through its memory view.
               constexpr uint32_t align = psio::canonical_align_v<ReturnType>;
               constexpr uint32_t size  = psio::canonical_size_v<ReturnType>;
#ifdef __wasm__
               uint8_t* ret_area =
                  static_cast<uint8_t*>(::cabi_realloc(nullptr, 0, align, size));
#else
               static uint8_t fallback_ret[64];
               uint8_t* ret_area = fallback_ret;
#endif
               guest_return_area_policy ra{ret_area};
               psio::canonical_lower_fields(result, ra, 0);
               return static_cast<flat_val>(reinterpret_cast<uintptr_t>(ret_area));
            }
         }
      }
   };

   // ── ImportProxy — guest-side import thunk dispatch ─────────────────────
   //
   // The mirror of ComponentProxy: takes C++ args, lowers to flat_vals,
   // calls a raw 16-wide import, lifts the return. Handles return-area
   // protocol for multi-slot returns (strings, records, lists).
   //
   // Usage (in guest code):
   //   extern "C" flat_val _raw_import(flat_val, ..., flat_val);
   //   ReturnType Interface::method(args...) {
   //      return ImportProxy::call<&Interface::method, &_raw_import>(args...);
   //   }

   using raw_import_fn = flat_val(*)(
      flat_val, flat_val, flat_val, flat_val,
      flat_val, flat_val, flat_val, flat_val,
      flat_val, flat_val, flat_val, flat_val,
      flat_val, flat_val, flat_val, flat_val);

   namespace detail_component {
      template <typename F> struct fn_traits;
      template <typename R, typename... Args>
      struct fn_traits<R(*)(Args...)> {
         using ReturnType = R;
         using ArgTuple = std::tuple<Args...>;
      };
   }

   // Forward declaration — defined in module.hpp
   template <typename T>
   void guest_import_lower(flat_val* slots, size_t& idx, const T& v);

   struct ImportProxy {

      template <typename FnPtr, typename... CppArgs>
      static auto call_impl(raw_import_fn raw, const CppArgs&... args)
      {
         using Traits = detail_component::fn_traits<FnPtr>;
         using Ret = typename Traits::ReturnType;

         // Lower C++ args to flat_vals. Data is already in guest linear
         // memory — just emit pointers and sizes, no copy needed.
         flat_val slots[16] = {};
         std::size_t idx = 0;
         (guest_import_lower(slots, idx, args), ...);

         constexpr size_t rflat = detail_component::result_flat_count<Ret>();

         if constexpr (std::is_void_v<Ret>) {
            raw(slots[0],  slots[1],  slots[2],  slots[3],
                slots[4],  slots[5],  slots[6],  slots[7],
                slots[8],  slots[9],  slots[10], slots[11],
                slots[12], slots[13], slots[14], slots[15]);
         }
         else if constexpr (rflat <= psio::MAX_FLAT_RESULTS) {
            // Scalar return — direct cast from flat_val
            flat_val r = raw(
                slots[0],  slots[1],  slots[2],  slots[3],
                slots[4],  slots[5],  slots[6],  slots[7],
                slots[8],  slots[9],  slots[10], slots[11],
                slots[12], slots[13], slots[14], slots[15]);
            if constexpr (std::is_integral_v<Ret>)
               return static_cast<Ret>(r);
            else if constexpr (std::is_same_v<Ret, float>) {
               union { int32_t i; float f; } u;
               u.i = static_cast<int32_t>(r);
               return u.f;
            } else if constexpr (std::is_same_v<Ret, double>) {
               union { int64_t i; double f; } u;
               u.i = r;
               return u.f;
            } else if constexpr (psizam::wasm_type_traits<std::decay_t<Ret>, void>::is_wasm_type) {
               using wt = psizam::wasm_type_traits<std::decay_t<Ret>, void>;
               return wt::wrap(static_cast<typename wt::wasm_type>(r));
            } else {
               return static_cast<Ret>(r);
            }
         }
         else {
            // Multi-slot return — return area protocol.
            // Allocate a return area, pass its pointer as an extra arg
            // after the regular args. The bridge writes the canonical
            // result into it and returns the pointer.
            // String and list returns are always {i32 ptr, i32 len} = 8 bytes.
            // Records use their canonical size from reflection.
            constexpr uint32_t ret_align = []() -> uint32_t {
               using U = std::remove_cvref_t<Ret>;
               if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>> ||
                             psio::detail::is_std_string_ct<U>::value)
                  return 4;
               else if constexpr (detail_dispatch::is_wit_vector<U>::value)
                  return 4;
               else
                  return psio::canonical_align_v<U>;
            }();
            constexpr uint32_t ret_size = []() -> uint32_t {
               using U = std::remove_cvref_t<Ret>;
               if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>> ||
                             psio::detail::is_std_string_ct<U>::value)
                  return 8;
               else if constexpr (detail_dispatch::is_wit_vector<U>::value)
                  return 8;
               else
                  return psio::canonical_size_v<U>;
            }();
#ifdef __wasm__
            uint8_t* ret_area = static_cast<uint8_t*>(
               ::cabi_realloc(nullptr, 0, ret_align, ret_size));
#else
            static uint8_t fallback[256];
            uint8_t* ret_area = fallback;
#endif
            slots[idx] = static_cast<flat_val>(
               reinterpret_cast<uintptr_t>(ret_area));

            raw(slots[0],  slots[1],  slots[2],  slots[3],
                slots[4],  slots[5],  slots[6],  slots[7],
                slots[8],  slots[9],  slots[10], slots[11],
                slots[12], slots[13], slots[14], slots[15]);

            // Lift the result from the return area
            return lift_from_retarea<Ret>(ret_area);
         }
      }

   private:
      template <typename Ret>
      static Ret lift_from_retarea(const uint8_t* ret_area) {
         using U = std::remove_cvref_t<Ret>;
         if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
            uint32_t ptr, len;
            std::memcpy(&ptr, ret_area, 4);
            std::memcpy(&len, ret_area + 4, 4);
            return psio::owned<std::string, psio::wit>::adopt(
               reinterpret_cast<char*>(static_cast<uintptr_t>(ptr)), len);
         }
         else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
            using E = typename detail_dispatch::is_wit_vector<U>::element_type;
            uint32_t ptr, len;
            std::memcpy(&ptr, ret_area, 4);
            std::memcpy(&len, ret_area + 4, 4);
            return U::adopt(
               reinterpret_cast<E*>(static_cast<uintptr_t>(ptr)), len);
         }
         else if constexpr (psio::detail::is_std_string_ct<U>::value) {
            // std::string return — read {ptr, len} from return area,
            // construct string from linear memory
            uint32_t ptr, len;
            std::memcpy(&ptr, ret_area, 4);
            std::memcpy(&len, ret_area + 4, 4);
            return std::string(
               reinterpret_cast<const char*>(static_cast<uintptr_t>(ptr)), len);
         }
         else if constexpr (psio::Reflected<U>) {
            // Record return — lift from canonical layout in return area
            export_lift_policy lift(nullptr, ret_area);
            // The return area IS the base; fields are at canonical offsets
            // relative to ret_area. Use psio::canonical_lift_fields.
            struct retarea_load_policy {
               const uint8_t* base;
               uint8_t  load_u8(uint32_t off)  { return base[off]; }
               uint16_t load_u16(uint32_t off) { uint16_t v; std::memcpy(&v, base+off, 2); return v; }
               uint32_t load_u32(uint32_t off) { uint32_t v; std::memcpy(&v, base+off, 4); return v; }
               uint64_t load_u64(uint32_t off) { uint64_t v; std::memcpy(&v, base+off, 8); return v; }
               float    load_f32(uint32_t off) { float v;    std::memcpy(&v, base+off, 4); return v; }
               double   load_f64(uint32_t off) { double v;   std::memcpy(&v, base+off, 8); return v; }
               const char* load_bytes(uint32_t off, uint32_t) {
                  return reinterpret_cast<const char*>(base + off);
               }
            };
            retarea_load_policy lp{ret_area};
            return psio::canonical_lift_fields<U>(lp, 0);
         }
         else {
            static_assert(sizeof(Ret) == 0, "ImportProxy: unsupported multi-slot return type");
         }
      }
   };

} // namespace psizam
