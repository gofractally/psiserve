#pragma once

// hosted.hpp — high-level driver that collapses the PSIO-reflected
// host↔guest wiring into a few lines at the call site.
//
// Example:
//
//   struct env     { static void log_u64(std::uint64_t); };
//   struct greeter { static void run(std::uint64_t); };
//
//   struct Host { void log_u64(std::uint64_t); };
//   PSIO_HOST_MODULE(Host, interface(env, log_u64))
//
//   Host host;
//   psizam::hosted<Host, psizam::interpreter> vm{wasm_bytes, host};
//   vm.as<greeter>().run(5);
//
// What it does:
//   • Constructor walks psio::detail::impl_info<Impl>::interfaces and
//     registers every (member pointer, name) pair with
//     `registered_host_functions::add<MemberPtr>` under the WASM module
//     name matching the interface's reflected name.
//   • `as<Tag>()` looks up the PSIO interface_info for Tag, constructs
//     its reflected proxy bound to the backend, and returns it ready to
//     call. Method calls on the proxy dispatch through the backend by
//     resolving the function name in interface_info::func_names.

#include <psizam/backend.hpp>
#include <psizam/canonical_dispatch.hpp>
#include <psizam/host_function.hpp>

#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psizam
{

namespace detail
{

// ── is_scalar_wasm_type — can this C++ type be passed as a single WASM value?
template <typename T>
constexpr bool is_scalar_wasm_type_v = [] {
   if constexpr (std::is_void_v<T>)
      return true;
   else if constexpr (std::is_integral_v<T>)
      return sizeof(T) <= 8;
   else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>)
      return true;
   else if constexpr (wasm_type_traits<std::decay_t<T>>::is_wasm_type)
      return true;
   else
      return false;
}();

// Member-pointer decomposition for checking method signatures
template <typename T>
struct member_fn_types;

template <typename Host, typename R, typename... Args>
struct member_fn_types<R (Host::*)(Args...)>
{
   using ReturnType = R;
   static constexpr bool all_scalar =
      is_scalar_wasm_type_v<R> && (is_scalar_wasm_type_v<std::remove_cvref_t<Args>> && ...);
};
template <typename Host, typename R, typename... Args>
struct member_fn_types<R (Host::*)(Args...) const> : member_fn_types<R (Host::*)(Args...)> {};

// ── Thread-local active backend for re-entrant cabi_realloc ─────────
// Set by invoke_canonical_export before calling an export, used by
// canonical host handlers to call cabi_realloc in the guest.
inline thread_local void* tl_active_backend = nullptr;
inline thread_local void* tl_active_host    = nullptr;

// Helper: call cabi_realloc on the active backend
inline uint32_t host_cabi_realloc(uint32_t align, uint32_t size) {
   if (!tl_active_backend) return 0;
   using be_t = backend<std::nullptr_t, interpreter>;
   auto* be = static_cast<be_t*>(tl_active_backend);
   auto r = be->call_with_return(tl_active_host,
      std::string_view{"cabi_realloc"},
      uint32_t{0}, uint32_t{0}, align, size);
   return r ? r->to_ui32() : 0u;
}

// ── host_lift_policy ─────────────────────────────────────────────────
// Reads flat slots + linear memory to reconstruct a host C++ value from
// a guest export's return (or a guest import's args on the host side).
// slots[] is pre-filled by the caller; mem_base is linear memory.
struct host_lift_policy
{
   const ::psio::native_value* slots;
   size_t idx = 0;
   const uint8_t* mem_base;

   host_lift_policy(const ::psio::native_value* s, const uint8_t* m) : slots(s), mem_base(m) {}

   uint32_t next_i32() { return slots[idx++].i32; }
   uint64_t next_i64() { return slots[idx++].i64; }
   float    next_f32() { return slots[idx++].f32; }
   double   next_f64() { return slots[idx++].f64; }

   const uint8_t* resolve(uint32_t off) const { return mem_base + off; }
   uint8_t  load_u8(uint32_t off)  { return resolve(off)[0]; }
   uint16_t load_u16(uint32_t off) { uint16_t v; std::memcpy(&v, resolve(off), 2); return v; }
   uint32_t load_u32(uint32_t off) { uint32_t v; std::memcpy(&v, resolve(off), 4); return v; }
   uint64_t load_u64(uint32_t off) { uint64_t v; std::memcpy(&v, resolve(off), 8); return v; }
   float    load_f32(uint32_t off) { float v;    std::memcpy(&v, resolve(off), 4); return v; }
   double   load_f64(uint32_t off) { double v;   std::memcpy(&v, resolve(off), 8); return v; }
   const char* load_bytes(uint32_t off, uint32_t) { return reinterpret_cast<const char*>(resolve(off)); }
};

// ── Canonical host function handler ─────────────────────────────────
// Creates a std::function<void(Host*, TC&)> that reads 16 i64 flat vals
// from the operand stack, lifts them into C++ types via canonical ABI,
// calls the real host method, lowers the return, and pushes the result.
template <auto MemPtr, typename Policy, typename... Args>
auto lift_canonical_args(Policy& p, ::psio::TypeList<Args...>)
{
   return std::tuple{psizam::canonical_lift_flat<std::remove_cvref_t<Args>>(p)...};
}

template <auto MemPtr, typename Host, typename TC>
auto make_canonical_host_handler()
{
   return std::function<void(Host*, TC&)>{[](Host* host, TC& tc) {
      using MType      = member_fn_types<decltype(MemPtr)>;
      using ReturnType = typename MType::ReturnType;

      // Read 16 i64 values from operand stack
      ::psio::native_value slots[16];
      for (int i = 0; i < 16; ++i)
         slots[i].i64 = tc.get_interface()
                            .operand_from_back(15 - i)
                            .template get<::psizam::detail::i64_const_t>()
                            .data.ui;

      // Linear memory for resolving string/list pointers
      const uint8_t* mem =
         static_cast<const uint8_t*>(tc.get_interface().get_memory());

      // Lift args from flat vals
      host_lift_policy lift{slots, mem};
      using fn_types = member_fn_types<decltype(MemPtr)>;
      // Extract arg types from the member pointer
      auto args = [&]<typename H, typename R, typename... As>(R (H::*)(As...)) {
         return lift_canonical_args<MemPtr>(
            lift, ::psio::TypeList<std::remove_cvref_t<As>...>{});
      }(MemPtr);

      // Trim operand stack (pop the 16 args)
      tc.get_interface().trim_operands(16);

      // Call the real method and push result
      if constexpr (std::is_void_v<ReturnType>) {
         std::apply([host](auto&&... a) {
            (host->*MemPtr)(std::forward<decltype(a)>(a)...);
         }, args);
         tc.get_interface().push_operand(
            ::psizam::detail::i64_const_t{uint64_t{0}});
      } else {
         auto result = std::apply([host](auto&&... a) {
            return (host->*MemPtr)(std::forward<decltype(a)>(a)...);
         }, args);

         using U = std::remove_cvref_t<ReturnType>;
         uint64_t rv = 0;

         if constexpr (std::is_integral_v<U>)
            rv = static_cast<uint64_t>(result);
         else if constexpr (std::is_floating_point_v<U>) {
            if constexpr (sizeof(U) == 4) {
               union { float f; uint32_t u; } cvt{result};
               rv = cvt.u;
            } else {
               union { double f; uint64_t u; } cvt{result};
               rv = cvt.u;
            }
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view> ||
                            std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
            // String return: allocate in guest memory via cabi_realloc,
            // copy the string data, write {ptr, len} into a return area,
            // return the pointer to the return area.
            std::string_view sv;
            if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>)
               sv = result.view();
            else
               sv = result;

            char* guest_mem = static_cast<char*>(tc.get_interface().get_memory());

            // The guest passed a retptr as the next flat arg after the
            // real args. It's in slots[num_consumed_slots].
            uint32_t retptr = static_cast<uint32_t>(slots[lift.idx].i64);

            // Allocate space for the string data in guest memory via
            // cabi_realloc (called re-entrantly through the backend).
            uint32_t str_ptr = 0;
            if (!sv.empty()) {
               str_ptr = host_cabi_realloc(1, static_cast<uint32_t>(sv.size()));
               // Re-read guest memory (cabi_realloc may have grown it)
               guest_mem = static_cast<char*>(tc.get_interface().get_memory());
               std::memcpy(guest_mem + str_ptr, sv.data(), sv.size());
            }

            // Write {ptr, len} into the return area
            uint32_t str_len = static_cast<uint32_t>(sv.size());
            std::memcpy(guest_mem + retptr, &str_ptr, 4);
            std::memcpy(guest_mem + retptr + 4, &str_len, 4);

            rv = static_cast<uint64_t>(retptr);
         }
         else if constexpr (psio::Reflected<U> ||
                            psio::is_std_variant_v<U> ||
                            psio::detail::is_std_vector_ct<U>::value ||
                            psio::detail::is_std_optional_ct<U>::value ||
                            psio::is_std_tuple<U>::value) {
            // Complex return: lower fields into guest return area.
            // Handles records, variants (WIT result<T,E>), vectors, optionals.
            uint32_t retptr = static_cast<uint32_t>(slots[lift.idx].i64);

            // Store policy that writes to guest memory, re-reading the
            // base pointer after each alloc (cabi_realloc may grow memory).
            struct guest_store {
               TC* tc;
               char* mem() { return static_cast<char*>(tc->get_interface().get_memory()); }
               uint32_t alloc(uint32_t align, uint32_t size) {
                  return host_cabi_realloc(align, size);
               }
               void store_u8(uint32_t off, uint8_t v)   { std::memcpy(mem()+off, &v, 1); }
               void store_u16(uint32_t off, uint16_t v) { std::memcpy(mem()+off, &v, 2); }
               void store_u32(uint32_t off, uint32_t v) { std::memcpy(mem()+off, &v, 4); }
               void store_u64(uint32_t off, uint64_t v) { std::memcpy(mem()+off, &v, 8); }
               void store_f32(uint32_t off, float v)    { std::memcpy(mem()+off, &v, 4); }
               void store_f64(uint32_t off, double v)   { std::memcpy(mem()+off, &v, 8); }
               void store_bytes(uint32_t off, const char* data, uint32_t len) {
                  if (len > 0) std::memcpy(mem()+off, data, len);
               }
            };
            guest_store sp{&tc};
            psio::canonical_lower_fields(result, sp, retptr);
            rv = static_cast<uint64_t>(retptr);
         }

         tc.get_interface().push_operand(
            ::psizam::detail::i64_const_t{rv});
      }
   }};
}

// ── register_canonical_host_fn ──────────────────────────────────────
// Pushes a canonical handler + (i64×16)→i64 WASM signature into the
// registered_host_functions mappings, bypassing the legacy add<> path
// which can't handle non-scalar C++ types.
template <auto MemPtr, typename Rhf>
void register_canonical_host_fn(const std::string& mod, const std::string& name)
{
   using Host = typename Rhf::host_type_t;
   using TC   = typename Rhf::type_converter_t;
   auto& m    = Rhf::mappings::get();

   m.named_mapping[{mod, name}] = m.current_index++;
   m.functions.push_back(make_canonical_host_handler<MemPtr, Host, TC>());

   host_function hf;
   hf.params.assign(16, types::i64);
   hf.ret = {types::i64};
   m.host_functions.push_back(std::move(hf));

   m.fast_fwd.push_back(nullptr);
   m.fast_rev.push_back(nullptr);
   m.raw_ptrs.push_back(nullptr);
}

// Register one (Impl, Tag) interface: iterate iface_impl<Impl,Tag>::methods.
// Scalar-only methods go through the legacy add<> path; methods with
// canonical types (strings, records, lists) go through add_canonical.
template <typename Rhf, typename IfaceImpl, std::size_t... Is>
inline void register_iface_methods(std::string_view mod, std::index_sequence<Is...>)
{
   auto register_one = [&]<std::size_t I>() {
      constexpr auto method_ptr = std::get<I>(IfaceImpl::methods);
      using MTypes = member_fn_types<std::remove_const_t<decltype(method_ptr)>>;

      if constexpr (MTypes::all_scalar) {
         Rhf::template add<method_ptr>(
            std::string{mod}, std::string{IfaceImpl::names[I]});
      } else {
         register_canonical_host_fn<method_ptr, Rhf>(
            std::string{mod}, std::string{IfaceImpl::names[I]});
      }
   };
   (register_one.template operator()<Is>(), ...);
}

template <typename Rhf, typename IfaceImpl>
inline void register_one_interface()
{
   using iface_tag  = typename IfaceImpl::tag;
   using iface_info = ::psio::detail::interface_info<iface_tag>;
   constexpr auto n =
       std::tuple_size_v<std::remove_cvref_t<decltype(IfaceImpl::methods)>>;
   register_iface_methods<Rhf, IfaceImpl>(
       std::string_view{iface_info::name},
       std::make_index_sequence<n>{});
}

template <typename Rhf, typename Impl>
inline void register_all_impls()
{
   if constexpr (requires { typename ::psio::detail::impl_info<Impl>::interfaces; })
   {
      using interfaces = typename ::psio::detail::impl_info<Impl>::interfaces;
      [&]<typename... IfaceImpls>(std::tuple<IfaceImpls...>*) {
         (register_one_interface<Rhf, IfaceImpls>(), ...);
      }(static_cast<interfaces*>(nullptr));
   }
}

// ── Fn trait for free-function pointers ──────────────────────────────
// PSIO anchors declare methods as `static R method(Args...)`, so
// `decltype(&Tag::method)` is `R (*)(Args...)` — not covered by
// psio::MemberPtrType's member-pointer specializations. We extract R
// and Args locally from that free-pointer type.
template <typename F>
struct free_fn_traits;

template <typename R, typename... Args>
struct free_fn_traits<R (*)(Args...)>
{
   using ReturnType = R;
   using ArgTypes   = ::psio::TypeList<std::remove_cvref_t<Args>...>;
};

// ── host_lower_policy ────────────────────────────────────────────────
// Lowers host C++ values into a pending argument array that will feed
// the export call.  alloc() calls into the guest's `cabi_realloc` to
// reserve linear-memory regions; store_* / store_bytes copy directly
// into that memory via the context's linear_memory() base pointer.
template <typename Backend, typename Host>
struct host_lower_policy
{
   Backend* be;
   Host*    host;
   std::vector<::psio::native_value> flat_values;

   host_lower_policy(Backend& b, Host& h) : be(&b), host(&h) {}

   uint32_t alloc(uint32_t align, uint32_t size)
   {
      if (size == 0) return 0;
      auto ret = be->call_with_return(
         *host, std::string_view{}, std::string_view{"cabi_realloc"},
         uint32_t{0}, uint32_t{0}, uint32_t{align}, uint32_t{size});
      return ret ? ret->to_ui32() : uint32_t{0};
   }

   char* linear_memory() { return be->get_context().linear_memory(); }

   void store_u8(uint32_t off, uint8_t v)   { std::memcpy(linear_memory() + off, &v, 1); }
   void store_u16(uint32_t off, uint16_t v) { std::memcpy(linear_memory() + off, &v, 2); }
   void store_u32(uint32_t off, uint32_t v) { std::memcpy(linear_memory() + off, &v, 4); }
   void store_u64(uint32_t off, uint64_t v) { std::memcpy(linear_memory() + off, &v, 8); }
   void store_f32(uint32_t off, float v)    { std::memcpy(linear_memory() + off, &v, 4); }
   void store_f64(uint32_t off, double v)   { std::memcpy(linear_memory() + off, &v, 8); }
   void store_bytes(uint32_t off, const char* data, uint32_t len)
   {
      if (len > 0) std::memcpy(linear_memory() + off, data, len);
   }

   void emit_i32(uint32_t v) { ::psio::native_value nv; nv.i64 = 0; nv.i32 = v; flat_values.push_back(nv); }
   void emit_i64(uint64_t v) { ::psio::native_value nv; nv.i64 = v;              flat_values.push_back(nv); }
   void emit_f32(float v)    { ::psio::native_value nv; nv.i64 = 0; nv.f32 = v; flat_values.push_back(nv); }
   void emit_f64(double v)   { ::psio::native_value nv; nv.i64 = 0; nv.f64 = v; flat_values.push_back(nv); }
};

// ── canonical_result — deferred copy/view proxy ─────────────────────
// Returned from host→guest calls for string and list types. Holds a
// raw {ptr, len} into guest linear memory. The caller decides whether
// to borrow (zero-copy view) or copy (owning type) at the assignment
// site via implicit conversion operators.
//
//   std::string_view sv = proxy.concat("a", "b");  // zero-copy
//   wit::string      ws = proxy.concat("a", "b");  // malloc + copy
//   auto           result = proxy.concat("a", "b"); // proxy — use .view()

template <typename WireType>
struct canonical_result;

template <>
struct canonical_result<psio::owned<std::string, psio::wit>>
{
   uint32_t       ptr;
   uint32_t       len;
   const uint8_t* mem;

   std::string_view view() const noexcept {
      return {reinterpret_cast<const char*>(mem + ptr), len};
   }
   const char* data() const noexcept { return reinterpret_cast<const char*>(mem + ptr); }
   std::size_t size() const noexcept { return len; }
   bool        empty() const noexcept { return len == 0; }

   operator std::string_view() const noexcept { return view(); }
   operator psio::owned<std::string, psio::wit>() const {
      return psio::owned<std::string, psio::wit>{view()};
   }
   operator std::string() const { return std::string{view()}; }
};

template <typename E>
struct canonical_result<psio::owned<std::vector<E>, psio::wit>>
{
   uint32_t       ptr;
   uint32_t       len;
   const uint8_t* mem;

   std::span<const E> view() const noexcept {
      return {reinterpret_cast<const E*>(mem + ptr), len};
   }
   const E*    data() const noexcept { return reinterpret_cast<const E*>(mem + ptr); }
   std::size_t size() const noexcept { return len; }
   bool        empty() const noexcept { return len == 0; }
   const E& operator[](std::size_t i) const noexcept { return data()[i]; }
   const E* begin() const noexcept { return data(); }
   const E* end() const noexcept { return data() + len; }

   operator std::span<const E>() const noexcept { return view(); }
   operator psio::owned<std::vector<E>, psio::wit>() const {
      return psio::owned<std::vector<E>, psio::wit>{view()};
   }
   operator std::vector<E>() const {
      auto v = view();
      return std::vector<E>(v.begin(), v.end());
   }
};

// ── canonical_lower_flat for canonical_result ───────────────────────
// When a canonical_result from module X is passed as an arg to module Y,
// lower it as a string_view / span — this triggers a single copy from
// X's linear memory into Y's linear memory via cabi_realloc. No
// host-heap intermediate.
template <LowerPolicy Policy>
void canonical_lower_flat(const canonical_result<psio::owned<std::string, psio::wit>>& value, Policy& p)
{
   canonical_lower_flat(value.view(), p);
}

template <typename E, LowerPolicy Policy>
void canonical_lower_flat(const canonical_result<psio::owned<std::vector<E>, psio::wit>>& value, Policy& p)
{
   // Lower as std::vector-style: alloc array in target, copy elements
   using namespace psio::detail;
   constexpr uint32_t es = psio::canonical_size_v<E>;
   constexpr uint32_t ea = psio::canonical_align_v<E>;
   uint32_t count = static_cast<uint32_t>(value.size());
   uint32_t arr = p.alloc(ea, count * es);
   for (uint32_t i = 0; i < count; i++)
      psio::detail_canonical::store_field(value[i], p, arr + i * es);
   p.emit_i32(arr);
   p.emit_i32(count);
}

// ── invoke_canonical_export ─────────────────────────────────────────
// The full host-side canonical-ABI dance for a single export call.
// For string/list returns, produces a canonical_result proxy that
// defers the copy/view decision to the assignment site.
template <typename Ret, typename Backend, typename Host, typename... Args>
auto invoke_canonical_export(Backend& be, Host& host, std::string_view name, Args&&... args)
{
   host_lower_policy<Backend, Host> lp{be, host};
   (canonical_lower_flat(args, lp), ...);

   // PSIO_MODULE thunks are always declared `(flat_val, ..., flat_val)`
   // with 16 slots — the `flat_val` envelope is i64. So we read each
   // lowered slot as i64 (emit_i32/emit_f32 left the high 32 bits zero)
   // and pad the tail with zeros to reach the required 16-wide sig.
   auto& fv = lp.flat_values;
   auto slot_u64 = [&](size_t i) -> uint64_t {
      return i < fv.size() ? fv[i].i64 : uint64_t{0};
   };
   auto r = be.call_with_return(
      host, std::string_view{}, name,
      slot_u64(0),  slot_u64(1),  slot_u64(2),  slot_u64(3),
      slot_u64(4),  slot_u64(5),  slot_u64(6),  slot_u64(7),
      slot_u64(8),  slot_u64(9),  slot_u64(10), slot_u64(11),
      slot_u64(12), slot_u64(13), slot_u64(14), slot_u64(15));

   if constexpr (std::is_void_v<Ret>) {
      return;
   } else if constexpr (std::is_same_v<std::remove_cvref_t<Ret>, psio::owned<std::string, psio::wit>>) {
      // Return a deferred-conversion proxy — caller decides copy vs view.
      uint32_t ret_ptr = r ? r->to_ui32() : 0;
      const uint8_t* mem = reinterpret_cast<const uint8_t*>(be.get_context().linear_memory());
      uint32_t s_ptr, s_len;
      std::memcpy(&s_ptr, mem + ret_ptr, 4);
      std::memcpy(&s_len, mem + ret_ptr + 4, 4);
      return canonical_result<psio::owned<std::string, psio::wit>>{s_ptr, s_len, mem};
   } else if constexpr (detail_dispatch::is_wit_vector<std::remove_cvref_t<Ret>>::value) {
      using E = typename detail_dispatch::is_wit_vector<std::remove_cvref_t<Ret>>::element_type;
      uint32_t ret_ptr = r ? r->to_ui32() : 0;
      const uint8_t* mem = reinterpret_cast<const uint8_t*>(be.get_context().linear_memory());
      uint32_t e_ptr, e_len;
      std::memcpy(&e_ptr, mem + ret_ptr, 4);
      std::memcpy(&e_len, mem + ret_ptr + 4, 4);
      return canonical_result<psio::owned<std::vector<E>, psio::wit>>{e_ptr, e_len, mem};
   } else {
      using U = std::remove_cvref_t<Ret>;
      constexpr size_t rflat = psizam::flat_count_v<U>;
      host_lift_policy lift{nullptr, reinterpret_cast<const uint8_t*>(be.get_context().linear_memory())};
      if constexpr (rflat <= psio::MAX_FLAT_RESULTS) {
         // Single-slot flat return — scalar.
         ::psio::native_value slot;
         slot.i64 = 0;
         if (r) slot.i64 = r->to_ui64();
         host_lift_policy s_lift{&slot, reinterpret_cast<const uint8_t*>(be.get_context().linear_memory())};
         return canonical_lift_flat<U>(s_lift);
      } else {
         // Multi-slot return: the call result is an i32 pointer to a
         // return area in linear memory; read the record from there via
         // psio's canonical_lift_fields.
         uint32_t ret_ptr = r ? r->to_ui32() : 0;
         return psio::canonical_lift_fields<U>(lift, ret_ptr);
      }
   }
}

// Adapter the PSIO-emitted interface proxy plugs into. The proxy's
// per-function methods call `.template call<Index, Fn>(args...)`; we
// look the WASM export name up in `Info::func_names[Index]` and choose
// between a plain scalar pass-through and the canonical-ABI path.
template <typename Backend, typename Host, typename Info>
struct proxy_adapter
{
   Backend* _backend;
   Host*    _host;

   proxy_adapter(Backend& b, Host& h) : _backend(&b), _host(&h) {}

   // Fn is the pointer-to-function TYPE of the interface method
   // (decltype(&Tag::method), which for a static method resolves to a
   // free-function pointer). PSIO_MODULE thunks are always 16-wide
   // flat_val signatures regardless of the logical param types, so every
   // call routes through the canonical-ABI lower/lift path — scalars
   // just lower to a single i32/i64/f32/f64 slot with the tail zero-
   // padded up to 16.
   template <std::size_t Index, typename Fn, typename... Args>
   auto call(Args&&... args)
   {
      using Traits = free_fn_traits<Fn>;
      using Ret    = typename Traits::ReturnType;
      std::string_view name = Info::func_names[Index];

      // Set thread-locals so canonical host handlers can call
      // cabi_realloc re-entrantly for string/record returns
      tl_active_backend = static_cast<void*>(_backend);
      tl_active_host    = static_cast<void*>(_host);

      if constexpr (std::is_void_v<Ret>) {
         invoke_canonical_export<Ret>(*_backend, *_host, name,
                                     std::forward<Args>(args)...);
         tl_active_backend = nullptr;
         tl_active_host    = nullptr;
      } else {
         auto result = invoke_canonical_export<Ret>(*_backend, *_host, name,
                                             std::forward<Args>(args)...);
         tl_active_backend = nullptr;
         tl_active_host    = nullptr;
         return result;
      }
   }
};

// ── void* host variants ─────────────────────────────────────────────
// Versions of host_lower_policy and invoke_canonical_export that accept
// void* host pointers, for use with composition (multi-module) backends
// where the host type is erased.

template <typename Backend>
struct void_host_lower_policy
{
   Backend* be;
   void*    host;
   std::vector<::psio::native_value> flat_values;

   void_host_lower_policy(Backend& b, void* h) : be(&b), host(h) {}

   uint32_t alloc(uint32_t align, uint32_t size)
   {
      if (size == 0) return 0;
      auto ret = be->call_with_return(
         host, std::string_view{"cabi_realloc"},
         uint32_t{0}, uint32_t{0}, uint32_t{align}, uint32_t{size});
      return ret ? ret->to_ui32() : uint32_t{0};
   }

   char* linear_memory() { return be->get_context().linear_memory(); }

   void store_u8(uint32_t off, uint8_t v)   { std::memcpy(linear_memory() + off, &v, 1); }
   void store_u16(uint32_t off, uint16_t v) { std::memcpy(linear_memory() + off, &v, 2); }
   void store_u32(uint32_t off, uint32_t v) { std::memcpy(linear_memory() + off, &v, 4); }
   void store_u64(uint32_t off, uint64_t v) { std::memcpy(linear_memory() + off, &v, 8); }
   void store_f32(uint32_t off, float v)    { std::memcpy(linear_memory() + off, &v, 4); }
   void store_f64(uint32_t off, double v)   { std::memcpy(linear_memory() + off, &v, 8); }
   void store_bytes(uint32_t off, const char* data, uint32_t len)
   {
      if (len > 0) std::memcpy(linear_memory() + off, data, len);
   }

   void emit_i32(uint32_t v) { ::psio::native_value nv; nv.i64 = 0; nv.i32 = v; flat_values.push_back(nv); }
   void emit_i64(uint64_t v) { ::psio::native_value nv; nv.i64 = v;              flat_values.push_back(nv); }
   void emit_f32(float v)    { ::psio::native_value nv; nv.i64 = 0; nv.f32 = v; flat_values.push_back(nv); }
   void emit_f64(double v)   { ::psio::native_value nv; nv.i64 = 0; nv.f64 = v; flat_values.push_back(nv); }
};

template <typename Ret, typename Backend, typename... Args>
auto invoke_canonical_export_void(Backend& be, void* host, std::string_view name, Args&&... args)
{
   void_host_lower_policy<Backend> lp{be, host};
   (canonical_lower_flat(args, lp), ...);

   auto& fv = lp.flat_values;
   auto slot_u64 = [&](size_t i) -> uint64_t {
      return i < fv.size() ? fv[i].i64 : uint64_t{0};
   };
   auto r = be.call_with_return(
      host, name,
      slot_u64(0),  slot_u64(1),  slot_u64(2),  slot_u64(3),
      slot_u64(4),  slot_u64(5),  slot_u64(6),  slot_u64(7),
      slot_u64(8),  slot_u64(9),  slot_u64(10), slot_u64(11),
      slot_u64(12), slot_u64(13), slot_u64(14), slot_u64(15));

   if constexpr (std::is_void_v<Ret>) {
      return;
   } else if constexpr (std::is_same_v<std::remove_cvref_t<Ret>, psio::owned<std::string, psio::wit>>) {
      uint32_t ret_ptr = r ? r->to_ui32() : 0;
      const uint8_t* mem = reinterpret_cast<const uint8_t*>(be.get_context().linear_memory());
      uint32_t s_ptr, s_len;
      std::memcpy(&s_ptr, mem + ret_ptr, 4);
      std::memcpy(&s_len, mem + ret_ptr + 4, 4);
      return canonical_result<psio::owned<std::string, psio::wit>>{s_ptr, s_len, mem};
   } else if constexpr (detail_dispatch::is_wit_vector<std::remove_cvref_t<Ret>>::value) {
      using E = typename detail_dispatch::is_wit_vector<std::remove_cvref_t<Ret>>::element_type;
      uint32_t ret_ptr = r ? r->to_ui32() : 0;
      const uint8_t* mem = reinterpret_cast<const uint8_t*>(be.get_context().linear_memory());
      uint32_t e_ptr, e_len;
      std::memcpy(&e_ptr, mem + ret_ptr, 4);
      std::memcpy(&e_len, mem + ret_ptr + 4, 4);
      return canonical_result<psio::owned<std::vector<E>, psio::wit>>{e_ptr, e_len, mem};
   } else {
      using U = std::remove_cvref_t<Ret>;
      constexpr size_t rflat = psizam::flat_count_v<U>;
      host_lift_policy lift{nullptr, reinterpret_cast<const uint8_t*>(be.get_context().linear_memory())};
      if constexpr (rflat <= psio::MAX_FLAT_RESULTS) {
         ::psio::native_value slot;
         slot.i64 = 0;
         if (r) slot.i64 = r->to_ui64();
         host_lift_policy s_lift{&slot, reinterpret_cast<const uint8_t*>(be.get_context().linear_memory())};
         return canonical_lift_flat<U>(s_lift);
      } else {
         uint32_t ret_ptr = r ? r->to_ui32() : 0;
         return psio::canonical_lift_fields<U>(lift, ret_ptr);
      }
   }
}

// Proxy adapter for void* host — used by composition modules
template <typename Backend, typename Info>
struct void_proxy_adapter
{
   Backend* _backend;
   void*    _host;

   void_proxy_adapter(Backend& b, void* h) : _backend(&b), _host(h) {}

   template <std::size_t Index, typename Fn, typename... Args>
   auto call(Args&&... args)
   {
      using Traits = free_fn_traits<Fn>;
      using Ret    = typename Traits::ReturnType;
      std::string_view name = Info::func_names[Index];

      tl_active_backend = static_cast<void*>(_backend);
      tl_active_host    = _host;

      if constexpr (std::is_void_v<Ret>) {
         invoke_canonical_export_void<Ret>(*_backend, _host, name,
                                          std::forward<Args>(args)...);
         tl_active_backend = nullptr;
         tl_active_host    = nullptr;
      } else {
         auto result = invoke_canonical_export_void<Ret>(*_backend, _host, name,
                                                  std::forward<Args>(args)...);
         tl_active_backend = nullptr;
         tl_active_host    = nullptr;
         return result;
      }
   }
};

}  // namespace detail

// hosted<Impl, BackendKind> — owns a backend instance wired to Impl.
//
// Impl must have one or more PSIO_HOST_MODULE(Impl, interface(…), …) blocks
// declaring which interfaces it fulfills; the constructor registers
// each of those methods with the backend's host-function table keyed
// by the interface's WIT name.
template <typename Impl,
          typename BackendKind         = interpreter,
          typename Execution_Interface = execution_interface,
          typename Type_Converter      = type_converter<Impl>>
struct hosted
{
   using host_t    = Impl;
   using rhf_t     = registered_host_functions<Impl, Execution_Interface, Type_Converter>;
   using backend_t = backend<rhf_t, BackendKind>;

   wasm_allocator            alloc;
   std::vector<std::uint8_t> wasm_copy;
   backend_t                 be;
   Impl*                     host_ptr;

   template <typename Bytes>
   hosted(const Bytes& wasm_bytes, Impl& host)
      : wasm_copy(std::begin(wasm_bytes), std::end(wasm_bytes)),
        be((detail::register_all_impls<rhf_t, Impl>(), wasm_copy), host, &alloc),
        host_ptr(&host)
   {
   }

   backend_t& backend_ref() { return be; }
   Impl&      host() { return *host_ptr; }

   // Return a proxy for a guest-exported interface Tag. Usage:
   //
   //   struct greeter { static void run(std::uint64_t); };
   //   PSIO_INTERFACE(greeter, types(), funcs(func(run, count)))
   //   vm.as<greeter>().run(5);
   template <typename Tag>
   auto as()
   {
      using info    = ::psio::detail::interface_info<Tag>;
      using adapter = detail::proxy_adapter<backend_t, Impl, info>;
      using proxy_t = typename info::template proxy<adapter>;
      return proxy_t{be, *host_ptr};
   }
};

}  // namespace psizam
