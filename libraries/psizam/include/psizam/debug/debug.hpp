#pragma once
#include <psizam/debug/dwarf.hpp>
#include <psizam/backend.hpp>

namespace psizam::debug
{
   struct debug_instr_map
   {
      using builder = debug_instr_map;

      const void* code_begin = nullptr;
      const void* wasm_begin = nullptr;
      size_t      wasm_size  = 0;
      size_t      code_size  = 0;

      jit_info             locs;
      const jit_instr_loc* offset_to_addr     = nullptr;
      std::size_t                 offset_to_addr_len = 0;

      uint32_t code_offset(const void* p)
      {
         return reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(code_begin);
      }

      uint32_t wasm_offset(const void* p)
      {
         return reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(wasm_begin);
      }

      void on_code_start(const void* code_addr, const void* wasm_addr)
      {
         code_begin = code_addr;
         wasm_begin = wasm_addr;
      }

      void on_function_start(const void* code_addr, const void* wasm_addr)
      {
         locs.fn_locs.emplace_back();
         locs.fn_locs.back().code_prologue = code_offset(code_addr);
         locs.fn_locs.back().wasm_begin    = wasm_offset(wasm_addr);
         on_instr_start(code_addr, wasm_addr);
      }

      void on_function_end(const void* code_addr, const void* wasm_addr)
      {
         locs.fn_locs.back().code_end = code_offset(code_addr);
         locs.fn_locs.back().wasm_end = wasm_offset(wasm_addr);
      }

      void on_instr_start(const void* code_addr, const void* wasm_addr)
      {
         locs.instr_locs.push_back({code_offset(code_addr), wasm_offset(wasm_addr)});
      }

      void on_code_end(const void* code_addr, const void* wasm_addr)
      {
         code_size = (const char*)code_addr - (const char*)code_begin;
         wasm_size = (const char*)wasm_addr - (const char*)wasm_begin;
         on_instr_start(code_addr, wasm_addr);
      }

      void set(builder&& b)
      {
         *this = std::move(b);

         {
            uint32_t code = 0;
            uint32_t wasm = 0;
            for (auto& fn : locs.fn_locs)
            {
               PSIZAM_ASSERT(code <= fn.code_prologue &&  //
                                 fn.code_prologue <= fn.code_end,
                             profile_exception, "function parts are out of order");
               PSIZAM_ASSERT(wasm <= fn.wasm_begin && fn.wasm_begin <= fn.wasm_end,
                             profile_exception, "function wasm is out of order");
               code = fn.code_end;
               wasm = fn.wasm_end;
            }
         }

         {
            uint32_t code = 0;
            uint32_t wasm = 0;
            for (auto& instr : locs.instr_locs)
            {
#ifndef __aarch64__
               PSIZAM_ASSERT(code <= instr.code_offset, profile_exception,
                             "jit instructions are out of order");
               PSIZAM_ASSERT(wasm <= instr.wasm_addr, profile_exception,
                             "jit instructions are out of order");
#endif
               code = instr.code_offset;
               wasm = instr.wasm_addr;
            }
         }

         offset_to_addr     = locs.instr_locs.data();
         offset_to_addr_len = locs.instr_locs.size();
      }

      void relocate(const void* new_base) { code_begin = new_base; }

      std::uint32_t translate(const void* pc) const
      {
         std::size_t diff = (reinterpret_cast<const char*>(pc) -
                             reinterpret_cast<const char*>(code_begin));  // negative values wrap
         if (diff >= code_size || diff < offset_to_addr[0].code_offset)
            return 0xFFFFFFFFu;
         std::uint32_t code_offset = diff;

         // Loop invariant: offset_to_addr[lower].code_offset <= code_offset < offset_to_addr[upper].code_offset
         std::size_t lower = 0, upper = offset_to_addr_len;
         while (upper - lower > 1)
         {
            std::size_t mid = lower + (upper - lower) / 2;
            if (offset_to_addr[mid].code_offset <= code_offset)
               lower = mid;
            else
               upper = mid;
         }

         return offset_to_addr[lower].wasm_addr;
      }
   };  // debug_instr_map

   // Post-codegen fix-up for backends that emit native code in a pass
   // after WASM parsing (jit2, jit_llvm). The parser-level imap hooks
   // fire during parse with pre-codegen addresses, so fn_locs can't be
   // used for PC→wasm translation. After the final codegen pass
   // completes, call this to rebuild fn_locs from the module's recorded
   // function bounds.
   //
   // wasm_base: start of the original WASM file bytes.
   // native_code_base: base pointer against which `jit_code_offset` is
   //                   interpreted. For the native x86/arm JIT (jit2)
   //                   pass `mod.allocator.get_code_start()` — offsets
   //                   are relative. For jit_llvm pass nullptr — offsets
   //                   are already absolute native addresses.
   inline void rebuild_from_module(debug_instr_map& map,
                                   const psizam::module& mod,
                                   const void* wasm_base,
                                   const void* native_code_base = nullptr)
   {
      auto native_ptr_for = [&](std::size_t jit_off) -> const void* {
         if (native_code_base == nullptr)
            return reinterpret_cast<const void*>(jit_off);
         return reinterpret_cast<const char*>(native_code_base) + jit_off;
      };
      std::vector<jit_fn_loc> raw;
      raw.reserve(mod.code.size());
      for (const auto& fb : mod.code)
      {
         if (fb.body_start == nullptr) continue;
         if (native_code_base == nullptr && fb.jit_code_offset == 0) continue;
         jit_fn_loc fl;
         fl.code_prologue = 0;
         fl.code_end      = 0;
         fl.wasm_begin    = static_cast<uint32_t>(
             fb.body_start - reinterpret_cast<const uint8_t*>(wasm_base));
         fl.wasm_end      = fl.wasm_begin + fb.wasm_body_bytes;
         raw.push_back(fl);
      }
      struct indexed { const void* ptr; jit_fn_loc fl; };
      std::vector<indexed> tmp;
      tmp.reserve(raw.size());
      std::size_t i = 0;
      for (const auto& fb : mod.code)
      {
         if (fb.body_start == nullptr) continue;
         if (native_code_base == nullptr && fb.jit_code_offset == 0) continue;
         tmp.push_back({native_ptr_for(fb.jit_code_offset), raw[i++]});
      }
      std::sort(tmp.begin(), tmp.end(),
                [](const indexed& a, const indexed& b) { return a.ptr < b.ptr; });
      if (tmp.empty()) { map.code_size = 0; return; }
      const char* lo = reinterpret_cast<const char*>(tmp.front().ptr);
      map.code_begin = lo;
      debug_instr_map::builder b;
      b.code_begin = lo;
      b.wasm_begin = wasm_base;
      for (std::size_t k = 0; k < tmp.size(); ++k)
      {
         const char* fn_native = reinterpret_cast<const char*>(tmp[k].ptr);
         const char* fn_end    = (k + 1 < tmp.size())
                                    ? reinterpret_cast<const char*>(tmp[k + 1].ptr)
                                    : fn_native + 0x10000;  // best-effort upper bound
         jit_fn_loc fl = tmp[k].fl;
         fl.code_prologue = static_cast<uint32_t>(fn_native - lo);
         fl.code_end      = static_cast<uint32_t>(fn_end    - lo);
         b.locs.fn_locs.push_back(fl);
         b.locs.instr_locs.push_back({fl.code_prologue, fl.wasm_begin});
      }
      // Trailing sentinel so translate()'s binary search bounds are sane.
      b.locs.instr_locs.push_back(
          {static_cast<uint32_t>(b.locs.fn_locs.back().code_end),
           b.locs.fn_locs.back().wasm_end});
      // debug_instr_map uses code_size (computed from a final on_code_end
      // call), not a stored end pointer. Emit a synthetic on_code_end
      // event at the last function's exit so `set()` computes code_size.
      const char* final_end = reinterpret_cast<const char*>(lo) +
                              b.locs.fn_locs.back().code_end;
      b.on_code_end(final_end,
                    reinterpret_cast<const char*>(wasm_base) +
                        b.locs.fn_locs.back().wasm_end);
      map.set(std::move(b));
   }

   template <typename Backend>
   std::shared_ptr<debugger_registration> enable_debug(std::vector<uint8_t>& code,
                                                              Backend&              backend,
                                                              info&                 dwarf_info,
                                                              psio::input_stream    wasm_source)
   {
      auto& module = backend.get_module();
      auto& alloc  = module.allocator;
      auto& dbg    = backend.get_debug();
      return register_with_debugger(dwarf_info, dbg.locs, module, wasm_source);
   }
}  // namespace psizam::debug
