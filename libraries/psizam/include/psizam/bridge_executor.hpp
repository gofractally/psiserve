#pragma once

// bridge_executor.hpp — Threaded-dispatch bridge executor for the composition
// system.
//
// Replaces the compile-time template-based bridge_call<FnPtr> with a compact
// runtime data-driven bytecode program. Each bridge_program describes how to
// marshal arguments from consumer to provider and return values back, using a
// small instruction set that covers scalars, strings, lists, and records.
//
// The executor uses GCC computed-goto for zero-overhead dispatch: each opcode
// handler increments the instruction pointer and jumps directly to the next
// handler's address via a dispatch table indexed by opcode.
//
// Bridge programs are compiled once at link time from the function's C++
// type signature (via compile_bridge<FnPtr>). Later, these will be compiled
// from WIT-parsed type descriptors instead.

#include <psio/canonical_abi.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

#include <psizam/canonical_dispatch.hpp>
#include <psizam/host_function_table.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace psizam {

// Forward declaration — the actual template is in composition.hpp
template <typename Host, typename BackendKind>
struct module_instance;

namespace bridge {

// Bridge function trait decomposition — mirrors detail::bridge_fn_traits
// but lives here so bridge_executor.hpp is self-contained.
template <typename FnPtr>
struct bridge_fn_traits;

template <typename R, typename... Args>
struct bridge_fn_traits<R (*)(Args...)>
{
   using ReturnType = R;
   using ArgTypes   = ::psio::TypeList<std::remove_cvref_t<Args>...>;
   static constexpr std::size_t arg_count = sizeof...(Args);
};

// ── Bridge instruction opcodes ──────────────────────────────────────────────
enum op : uint8_t {
   fwd_i32,            // provider_slots[dst] = (uint32_t)consumer_args[src]
   fwd_i64,            // provider_slots[dst] = consumer_args[src] (64-bit)
   fwd_f32,            // provider_slots[dst] = consumer_args[src] (float bits)
   fwd_f64,            // provider_slots[dst] = consumer_args[src] (double bits)
   copy_string_arg,    // read {ptr,len} from consumer args, cabi_realloc in provider, memcpy
   copy_list_arg,      // same for list: uses element_size and element_align from instruction
   copy_record_arg,    // memcpy canonical_size bytes from consumer mem to provider mem via realloc
   set_retptr,         // alloc return area in consumer, pass ptr as extra arg to provider
   call_export,        // invoke the provider's export by name
   return_scalar,      // return raw i64 from call result
   return_string,      // read {ptr,len} from provider retarea, alloc+copy to consumer
   return_list,        // same for list with element_size
   return_record,      // memcpy record from provider retarea to consumer retarea
   done,               // end of program
};

// ── Bridge instruction ──────────────────────────────────────────────────────
struct instruction {
   op       opcode;
   uint8_t  src;       // source slot index in consumer args
   uint8_t  dst;       // dest slot index in provider args
   uint8_t  align;     // element alignment (for lists/records)
   uint32_t size;      // element_size / record_size
};

// ── Bridge program ──────────────────────────────────────────────────────────
// A compact description of how to bridge one function call between two modules.
// Compiled once from the function's type signature at link time.
static constexpr int MAX_BRIDGE_INSTRUCTIONS = 48;

struct bridge_program {
   instruction code[MAX_BRIDGE_INSTRUCTIONS];
   uint8_t     len = 0;
   std::string export_name;

   void emit(op o, uint8_t src = 0, uint8_t dst = 0, uint8_t align = 0, uint32_t size = 0) {
      code[len++] = {o, src, dst, align, size};
   }
};

// ── Bridge program compiler ─────────────────────────────────────────────────
// Walks the function's C++ arg types and return type at compile time and emits
// the right opcodes. Later, this will be replaced by a WIT-parsed version.

namespace detail_bridge {

// Emit arg-forwarding instructions for one parameter type.
// Returns the number of consumer slots consumed.
template <typename T>
struct arg_emitter {
   static uint8_t emit(bridge_program& prog, uint8_t src_slot, uint8_t dst_slot) {
      using U = std::remove_cvref_t<T>;
      if constexpr (std::is_same_v<U, bool> ||
                    std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t> ||
                    std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t> ||
                    std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t>) {
         prog.emit(fwd_i32, src_slot, dst_slot);
         return 1;
      }
      else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t>) {
         prog.emit(fwd_i64, src_slot, dst_slot);
         return 1;
      }
      else if constexpr (std::is_same_v<U, float>) {
         prog.emit(fwd_f32, src_slot, dst_slot);
         return 1;
      }
      else if constexpr (std::is_same_v<U, double>) {
         prog.emit(fwd_f64, src_slot, dst_slot);
         return 1;
      }
      else if constexpr (std::is_same_v<U, std::string_view> ||
                         psio::detail::is_std_string_ct<U>::value) {
         // String arg: consumer has {ptr, len} in two slots
         prog.emit(copy_string_arg, src_slot, dst_slot);
         return 2;  // consumes 2 consumer slots
      }
      else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
         using E = typename detail_dispatch::is_wit_vector<U>::element_type;
         prog.emit(copy_list_arg, src_slot, dst_slot,
                   static_cast<uint8_t>(alignof(E)),
                   static_cast<uint32_t>(psio::canonical_size_v<E>));
         return 2;  // consumes 2 consumer slots (ptr, len)
      }
      else if constexpr (psio::detail::is_std_vector_ct<U>::value) {
         using E = typename psio::detail::vector_elem_ct<U>::type;
         prog.emit(copy_list_arg, src_slot, dst_slot,
                   static_cast<uint8_t>(psio::canonical_align_v<E>),
                   static_cast<uint32_t>(psio::canonical_size_v<E>));
         return 2;  // consumes 2 consumer slots (ptr, len)
      }
      else if constexpr (detail_dispatch::is_std_span<U>::value) {
         using E = typename detail_dispatch::is_std_span<U>::element_type;
         prog.emit(copy_list_arg, src_slot, dst_slot,
                   static_cast<uint8_t>(alignof(E)),
                   static_cast<uint32_t>(sizeof(E)));
         return 2;  // consumes 2 consumer slots
      }
      else if constexpr (psio::Reflected<U>) {
         // Record: recursively emit field-by-field forwarding instructions.
         // Each field becomes its own forward/copy instruction(s).
         constexpr size_t fc = flat_count_v<U>;
         if constexpr (fc <= psio::MAX_FLAT_PARAMS) {
            // Fits in flat slots — expand each field
            uint8_t field_src = src_slot;
            uint8_t field_dst = dst_slot;
            psio::apply_members(
               (typename psio::reflect<U>::data_members*)nullptr,
               [&](auto... ptrs) {
                  ((([&]<typename MP>(MP) {
                     using FieldType = std::remove_cvref_t<
                        typename psio::MemberPtrType<MP>::ValueType>;
                     uint8_t consumed = arg_emitter<FieldType>::emit(
                        prog, field_src, field_dst);
                     constexpr size_t ffc = flat_count_v<FieldType>;
                     field_src += consumed;
                     field_dst += ffc;
                  })(ptrs)), ...);
               }
            );
            return static_cast<uint8_t>(fc);
         } else {
            // Too many flat slots — spilled to memory as a pointer
            prog.emit(copy_record_arg, src_slot, dst_slot,
                      static_cast<uint8_t>(psio::canonical_align_v<U>),
                      static_cast<uint32_t>(psio::canonical_size_v<U>));
            return 1;  // single pointer slot
         }
      }
      else {
         static_assert(sizeof(U) == 0, "compile_bridge: unsupported arg type");
         return 0;
      }
   }
};

// Number of flat slots a type occupies in the actual (possibly spilled)
// calling convention. For types that fit, this equals flat_count_v<T>.
// For records that would exceed MAX_FLAT_PARAMS, the record is passed
// as a single i32 pointer (1 slot). Strings, lists, and vectors always
// occupy 2 slots regardless.
template <typename T>
constexpr uint8_t call_slot_count() {
   using U = std::remove_cvref_t<T>;
   constexpr size_t fc = flat_count_v<U>;
   if constexpr (std::is_same_v<U, std::string_view> ||
                 psio::detail::is_std_string_ct<U>::value ||
                 detail_dispatch::is_wit_vector<U>::value ||
                 psio::detail::is_std_vector_ct<U>::value ||
                 detail_dispatch::is_std_span<U>::value) {
      return 2;
   } else if constexpr (psio::Reflected<U> &&
                        !std::is_integral_v<U> && !std::is_floating_point_v<U> &&
                        !std::is_same_v<U, bool>) {
      return (fc <= psio::MAX_FLAT_PARAMS) ? static_cast<uint8_t>(fc) : 1;
   } else {
      return static_cast<uint8_t>(fc);
   }
}

// Walk all arg types and emit instructions
template <typename... Args>
void emit_args(bridge_program& prog, ::psio::TypeList<Args...>) {
   uint8_t src = 0;
   uint8_t dst = 0;
   auto emit_one = [&]<typename T>() {
      uint8_t consumed = arg_emitter<T>::emit(prog, src, dst);
      dst += call_slot_count<T>();
      src += consumed;
   };
   (emit_one.template operator()<Args>(), ...);
}

// Emit return handling instructions
template <typename Ret, typename ArgTypes>
void emit_return(bridge_program& prog) {
   using U = std::remove_cvref_t<Ret>;

   if constexpr (std::is_void_v<U>) {
      // No return value — just done
   }
   else if constexpr (flat_count_v<U> <= psio::MAX_FLAT_RESULTS) {
      // Single-slot scalar return
      prog.emit(return_scalar);
   }
   else if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
      // String return: the set_retptr was already emitted before call
      constexpr size_t num_arg_flats = []<typename... As>(::psio::TypeList<As...>) {
         return (flat_count_v<As> + ... + size_t{0});
      }(ArgTypes{});
      prog.emit(return_string, static_cast<uint8_t>(num_arg_flats));
   }
   else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
      using E = typename detail_dispatch::is_wit_vector<U>::element_type;
      constexpr size_t num_arg_flats = []<typename... As>(::psio::TypeList<As...>) {
         return (flat_count_v<As> + ... + size_t{0});
      }(ArgTypes{});
      prog.emit(return_list, static_cast<uint8_t>(num_arg_flats), 0,
                static_cast<uint8_t>(alignof(E)),
                static_cast<uint32_t>(sizeof(E)));
   }
   else {
      // Record/optional return via retarea
      constexpr size_t num_arg_flats = []<typename... As>(::psio::TypeList<As...>) {
         return (flat_count_v<As> + ... + size_t{0});
      }(ArgTypes{});
      prog.emit(return_record, static_cast<uint8_t>(num_arg_flats), 0,
                static_cast<uint8_t>(psio::canonical_align_v<U>),
                static_cast<uint32_t>(psio::canonical_size_v<U>));
   }
}

}  // namespace detail_bridge

// ── compile_bridge ──────────────────────────────────────────────────────────
// Compile a bridge program from a function pointer type's signature.
template <typename FnPtr>
bridge_program compile_bridge(std::string_view export_name) {
   using Traits = bridge_fn_traits<FnPtr>;
   using Ret = typename Traits::ReturnType;
   using ArgTypes = typename Traits::ArgTypes;

   bridge_program prog;
   prog.export_name = std::string(export_name);

   // 1. Emit arg forwarding/copying instructions
   [&]<typename... As>(::psio::TypeList<As...>) {
      detail_bridge::emit_args(prog, ::psio::TypeList<As...>{});
   }(ArgTypes{});

   // 2. If return needs a retarea pointer, emit set_retptr before the call
   using U = std::remove_cvref_t<Ret>;
   if constexpr (!std::is_void_v<U> && flat_count_v<U> > psio::MAX_FLAT_RESULTS) {
      // Need a retptr — figure out which provider slot it goes into
      // The retptr slot is always the next slot after all arg flats in
      // the provider's arg array.
      constexpr size_t num_arg_flats = []<typename... As>(::psio::TypeList<As...>) {
         return (flat_count_v<As> + ... + size_t{0});
      }(ArgTypes{});

      // Compute return area size and alignment.
      // wit-owned strings and vectors have a {ptr, len} return area (8 bytes, align 4).
      // Plain records use their canonical size/align.
      constexpr uint32_t ret_size = [] {
         if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>)
            return uint32_t{8};
         else if constexpr (detail_dispatch::is_wit_vector<U>::value)
            return uint32_t{8};
         else
            return psio::canonical_size_v<U>;
      }();
      constexpr uint32_t ret_align = [] {
         if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>)
            return uint32_t{4};
         else if constexpr (detail_dispatch::is_wit_vector<U>::value)
            return uint32_t{4};
         else
            return psio::canonical_align_v<U>;
      }();

      prog.emit(set_retptr, 0,
                static_cast<uint8_t>(num_arg_flats), // dst = provider slot for retptr
                static_cast<uint8_t>(ret_align),
                ret_size);
   }

   // 3. Call the provider's export
   prog.emit(call_export);

   // 4. Return handling
   detail_bridge::emit_return<Ret, ArgTypes>(prog);

   // 5. Done
   prog.emit(done);

   return prog;
}

// ── execute_bridge ──────────────────────────────────────────────────────────
// Threaded-dispatch executor. Uses computed-goto (GCC extension) for
// zero-overhead opcode dispatch.
//
// Template parameter BackendKind is needed to resolve module_instance type.
template <typename Host, typename BackendKind>
native_value execute_bridge(
   const bridge_program& prog,
   module_instance<Host, BackendKind>* consumer,
   module_instance<Host, BackendKind>* provider,
   void* host,
   native_value* consumer_args,
   char* consumer_memory)
{
   // Provider arg slots (16-wide flat calling convention)
   uint64_t provider_slots[16] = {};

   // Return value
   native_value result;
   result.i64 = 0;

   // Provider retptr (allocated in provider's memory for multi-slot returns)
   uint32_t provider_retptr = 0;
   // Consumer retptr (passed by consumer for multi-slot returns)
   uint32_t consumer_retptr = 0;

   const instruction* ip = prog.code;

   // Computed-goto dispatch table
   static constexpr void* dispatch_table[] = {
      &&do_fwd_i32,
      &&do_fwd_i64,
      &&do_fwd_f32,
      &&do_fwd_f64,
      &&do_copy_string_arg,
      &&do_copy_list_arg,
      &&do_copy_record_arg,
      &&do_set_retptr,
      &&do_call_export,
      &&do_return_scalar,
      &&do_return_string,
      &&do_return_list,
      &&do_return_record,
      &&do_done,
   };

   #define DISPATCH() goto *dispatch_table[ip->opcode]
   #define NEXT() do { ++ip; DISPATCH(); } while(0)

   DISPATCH();

do_fwd_i32:
   provider_slots[ip->dst] = static_cast<uint64_t>(
      static_cast<uint32_t>(consumer_args[ip->src].i64));
   NEXT();

do_fwd_i64:
   provider_slots[ip->dst] = consumer_args[ip->src].i64;
   NEXT();

do_fwd_f32:
   provider_slots[ip->dst] = consumer_args[ip->src].i64;
   NEXT();

do_fwd_f64:
   provider_slots[ip->dst] = consumer_args[ip->src].i64;
   NEXT();

do_copy_string_arg:
   {
      // Read ptr+len from consumer args
      uint32_t src_ptr = static_cast<uint32_t>(consumer_args[ip->src].i64);
      uint32_t src_len = static_cast<uint32_t>(consumer_args[ip->src + 1].i64);

      uint32_t dst_ptr = 0;
      if (src_len > 0) {
         // Allocate in provider's memory
         auto alloc_ret = provider->be->call_with_return(
            host, std::string_view{"cabi_realloc"},
            uint32_t{0}, uint32_t{0}, uint32_t{1}, src_len);
         dst_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;

         // Copy bytes from consumer to provider
         char* provider_mem = provider->be->get_context().linear_memory();
         // Re-read consumer memory in case it moved during the alloc
         char* cons_mem = consumer->be->get_context().linear_memory();
         std::memcpy(provider_mem + dst_ptr, cons_mem + src_ptr, src_len);
      }

      provider_slots[ip->dst]     = static_cast<uint64_t>(dst_ptr);
      provider_slots[ip->dst + 1] = static_cast<uint64_t>(src_len);
   }
   NEXT();

do_copy_list_arg:
   {
      uint32_t src_ptr = static_cast<uint32_t>(consumer_args[ip->src].i64);
      uint32_t src_len = static_cast<uint32_t>(consumer_args[ip->src + 1].i64);
      uint32_t elem_size = ip->size;
      uint32_t elem_align = ip->align;

      uint32_t byte_len = src_len * elem_size;
      uint32_t dst_ptr = 0;
      if (byte_len > 0) {
         auto alloc_ret = provider->be->call_with_return(
            host, std::string_view{"cabi_realloc"},
            uint32_t{0}, uint32_t{0}, elem_align, byte_len);
         dst_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;

         char* provider_mem = provider->be->get_context().linear_memory();
         char* cons_mem = consumer->be->get_context().linear_memory();
         std::memcpy(provider_mem + dst_ptr, cons_mem + src_ptr, byte_len);
      }

      provider_slots[ip->dst]     = static_cast<uint64_t>(dst_ptr);
      provider_slots[ip->dst + 1] = static_cast<uint64_t>(src_len);
   }
   NEXT();

do_copy_record_arg:
   {
      // Spilled record arg (flat_count > MAX_FLAT_PARAMS): consumer passes a
      // single i32 pointer to the record in its memory. Alloc in provider,
      // memcpy the canonical layout.
      uint32_t src_ptr = static_cast<uint32_t>(consumer_args[ip->src].i64);

      auto alloc_ret = provider->be->call_with_return(
         host, std::string_view{"cabi_realloc"},
         uint32_t{0}, uint32_t{0}, static_cast<uint32_t>(ip->align), ip->size);
      uint32_t dst_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;

      char* provider_mem = provider->be->get_context().linear_memory();
      char* cons_mem = consumer->be->get_context().linear_memory();
      std::memcpy(provider_mem + dst_ptr, cons_mem + src_ptr, ip->size);

      provider_slots[ip->dst] = static_cast<uint64_t>(dst_ptr);
   }
   NEXT();

do_set_retptr:
   {
      // Allocate return area in provider's memory.
      // ip->dst = provider arg slot for retptr, ip->size/align = retarea dimensions.
      auto alloc_ret = provider->be->call_with_return(
         host, std::string_view{"cabi_realloc"},
         uint32_t{0}, uint32_t{0}, static_cast<uint32_t>(ip->align), ip->size);
      provider_retptr = alloc_ret ? alloc_ret->to_ui32() : 0u;
      provider_slots[ip->dst] = static_cast<uint64_t>(provider_retptr);
   }
   NEXT();

do_call_export:
   {
      auto r = provider->be->call_with_return(
         host, std::string_view{prog.export_name},
         provider_slots[0],  provider_slots[1],
         provider_slots[2],  provider_slots[3],
         provider_slots[4],  provider_slots[5],
         provider_slots[6],  provider_slots[7],
         provider_slots[8],  provider_slots[9],
         provider_slots[10], provider_slots[11],
         provider_slots[12], provider_slots[13],
         provider_slots[14], provider_slots[15]);
      result.i64 = r ? r->to_ui64() : 0;
   }
   NEXT();

do_return_scalar:
   // result already holds the raw return value
   goto do_done;  // skip to end

do_return_string:
   {
      // Provider wrote string into its return area (retptr from the call result).
      // result.i64 holds the provider retptr.
      uint32_t prov_retptr = static_cast<uint32_t>(result.i64);
      const uint8_t* provider_mem =
         reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

      uint32_t s_ptr, s_len;
      std::memcpy(&s_ptr, provider_mem + prov_retptr, 4);
      std::memcpy(&s_len, provider_mem + prov_retptr + 4, 4);

      // Consumer's retptr is at consumer_args[num_arg_flats]
      // ip->src holds num_arg_flats
      consumer_retptr = static_cast<uint32_t>(consumer_args[ip->src].i64);

      // Allocate in consumer's memory for the string data
      uint32_t consumer_str_ptr = 0;
      if (s_len > 0) {
         auto alloc_ret = consumer->be->call_with_return(
            host, std::string_view{"cabi_realloc"},
            uint32_t{0}, uint32_t{0}, uint32_t{1}, s_len);
         consumer_str_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;
         // Re-read provider_mem — it could be invalidated by consumer alloc
         // (but they're separate modules with separate memories, so it's fine)
         char* consumer_mem_w = consumer->be->get_context().linear_memory();
         std::memcpy(consumer_mem_w + consumer_str_ptr,
                     provider_mem + s_ptr, s_len);
      }

      // Write {ptr, len} into consumer's return area
      char* consumer_mem_w = consumer->be->get_context().linear_memory();
      std::memcpy(consumer_mem_w + consumer_retptr, &consumer_str_ptr, 4);
      std::memcpy(consumer_mem_w + consumer_retptr + 4, &s_len, 4);

      result.i64 = static_cast<uint64_t>(consumer_retptr);
   }
   goto do_done;

do_return_list:
   {
      uint32_t prov_retptr = static_cast<uint32_t>(result.i64);
      const uint8_t* provider_mem =
         reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

      uint32_t e_ptr, e_len;
      std::memcpy(&e_ptr, provider_mem + prov_retptr, 4);
      std::memcpy(&e_len, provider_mem + prov_retptr + 4, 4);

      // ip->src = num_arg_flats, ip->size = sizeof(element), ip->align = alignof(element)
      consumer_retptr = static_cast<uint32_t>(consumer_args[ip->src].i64);

      uint32_t byte_len = e_len * ip->size;
      uint32_t consumer_e_ptr = 0;
      if (byte_len > 0) {
         auto alloc_ret = consumer->be->call_with_return(
            host, std::string_view{"cabi_realloc"},
            uint32_t{0}, uint32_t{0},
            static_cast<uint32_t>(ip->align), byte_len);
         consumer_e_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;
         char* consumer_mem_w = consumer->be->get_context().linear_memory();
         std::memcpy(consumer_mem_w + consumer_e_ptr,
                     provider_mem + e_ptr, byte_len);
      }

      char* consumer_mem_w = consumer->be->get_context().linear_memory();
      std::memcpy(consumer_mem_w + consumer_retptr, &consumer_e_ptr, 4);
      std::memcpy(consumer_mem_w + consumer_retptr + 4, &e_len, 4);

      result.i64 = static_cast<uint64_t>(consumer_retptr);
   }
   goto do_done;

do_return_record:
   {
      uint32_t prov_retptr = static_cast<uint32_t>(result.i64);
      const uint8_t* provider_mem =
         reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

      // ip->src = num_arg_flats, ip->size = record byte size
      consumer_retptr = static_cast<uint32_t>(consumer_args[ip->src].i64);

      // Simple deep-copy: memcpy the entire record from provider's retarea
      // to consumer's retarea. This works for flat (no-pointer) records.
      // For records with embedded pointers (strings, lists), we'd need a
      // recursive copy — but that's a future extension.
      char* consumer_mem_w = consumer->be->get_context().linear_memory();
      std::memcpy(consumer_mem_w + consumer_retptr,
                  provider_mem + prov_retptr, ip->size);

      result.i64 = static_cast<uint64_t>(consumer_retptr);
   }
   goto do_done;

do_done:
   #undef DISPATCH
   #undef NEXT
   return result;
}

}  // namespace bridge
}  // namespace psizam
