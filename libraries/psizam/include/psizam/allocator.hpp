#pragma once

#include <psizam/constants.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/detail/span.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#if !defined(PSIZAM_COMPILE_ONLY) || defined(__LP64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// .eh_frame registration for JIT exception unwinding
#ifndef PSIZAM_COMPILE_ONLY
extern "C" void __register_frame(const void*);
extern "C" void __deregister_frame(const void*);
#endif

namespace psizam {

   class bounded_allocator {
    public:
      bounded_allocator(size_t size) {
         mem_size = size;
         raw      = std::unique_ptr<uint8_t[]>(new uint8_t[mem_size]);
      }
      template <typename T>
      T* alloc(size_t size = 1) {
         PSIZAM_ASSERT((sizeof(T) * size) + index <= mem_size, wasm_bad_alloc, "wasm failed to allocate native");
         T* ret = (T*)(raw.get() + index);
         index += sizeof(T) * size;
         return ret;
      }

      template <typename T>
      void reclaim(const T* ptr, size_t size=0) { /* noop for now */ }

      void free() {
         PSIZAM_ASSERT(index > 0, wasm_double_free, "double free");
         index = 0;
      }
      void                       reset() { index = 0; }
      size_t                     mem_size;
      std::unique_ptr<uint8_t[]> raw;
      size_t                     index = 0;
   };

#ifdef PSIZAM_COMPILE_ONLY
   // Compile-only stubs — types exist but don't allocate
   class stack_allocator {
    public:
      explicit stack_allocator(std::size_t, std::size_t = 4*1024*1024) {}
      void* top() const { return nullptr; }
      void* guard_base() const { return nullptr; }
      std::size_t guard_size() const { return 0; }
   };
#else
   // Conditionally allocates a new stack leaving enough room
   // for host function and signal handler execution.  If
   // the required stack size is small enough to fit in the
   // regular program stack, does nothing and returns nullptr.
   class stack_allocator {
    public:
      explicit stack_allocator(std::size_t min_size, std::size_t available = 4*1024*1024) {
         if(min_size > available) {
            std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            // Add one guard page at the bottom for stack overflow detection
            _guard_size = pagesize;
            _size = ((min_size + pagesize - 1) & ~(pagesize - 1)) + 4*1024*1024 + _guard_size;
            int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_STACK
            flags |= MAP_STACK;
#endif
            _ptr = ::mmap(nullptr, _size, PROT_READ | PROT_WRITE, flags, -1, 0);
            PSIZAM_ASSERT(_ptr != MAP_FAILED, wasm_bad_alloc, "Failed to allocate stack");
            // Protect the bottom page as a guard
            ::mprotect(_ptr, _guard_size, PROT_NONE);
         }
      }
      ~stack_allocator() {
         if(_ptr) {
            ::munmap(_ptr, _size);
         }
      }
      void* top() const {
         if(_ptr) {
            return static_cast<char*>(_ptr) + _size;
         } else {
            return nullptr;
         }
      }
      // Returns the guard page region (for signal handler registration)
      void* guard_base() const { return _ptr; }
      std::size_t guard_size() const { return _guard_size; }
   private:
      void* _ptr = nullptr;
      std::size_t _size = 0;
      std::size_t _guard_size = 0;
   };
#endif

#ifdef PSIZAM_COMPILE_ONLY
   class contiguous_allocator {
      public:
         template<std::size_t align_amt>
         static constexpr size_t align_offset(size_t offset) { return (offset + align_amt - 1) & ~(align_amt - 1); }
         contiguous_allocator(size_t size) : _size(size) {
            _base = (char*)std::malloc(size);
         }
         ~contiguous_allocator() { std::free(_base); }
         template <typename T>
         T* alloc(size_t size = 0) {
            _offset = align_offset<alignof(T)>(_offset);
            size_t aligned = (sizeof(T) * size) + _offset;
            if (aligned > _size) {
               _base = (char*)std::realloc(_base, aligned);
               _size = aligned;
            }
            T* ptr = (T*)(_base + _offset);
            _offset = aligned;
            return ptr;
         }
         template <typename T>
         void reclaim(const T*, size_t = 0) {}
         void free() {}
      private:
         size_t _offset = 0;
         size_t _size   = 0;
         char*  _base;
   };
#else
   class contiguous_allocator {
      public:
         template<std::size_t align_amt>
         static constexpr size_t align_offset(size_t offset) { return (offset + align_amt - 1) & ~(align_amt - 1); }

         static std::size_t align_to_page(std::size_t offset) {
            std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            return (offset + pagesize - 1) & ~(pagesize - 1);
         }

         contiguous_allocator(size_t size) {
            _size = align_to_page(size);
            _base = (char*)mmap(NULL, _size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            PSIZAM_ASSERT(_base != MAP_FAILED, wasm_bad_alloc, "mmap failed.");
         }
         ~contiguous_allocator() { munmap(_base, align_to_page(_size)); }

         template <typename T>
         T* alloc(size_t size = 0) {
            _offset = align_offset<alignof(T)>(_offset);
            size_t aligned = (sizeof(T) * size) + _offset;
            if (aligned > _size) {
               size_t new_size = align_to_page(aligned);
               char* new_base = (char*)mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
               PSIZAM_ASSERT(new_base != MAP_FAILED, wasm_bad_alloc, "mmap failed.");
               memcpy(new_base, _base, _size);
               munmap(_base, _size);
               _size = new_size;
               _base = new_base;
            }
            T* ptr = (T*)(_base + _offset);
            _offset = aligned;
            return ptr;
         }
         template <typename T>
         void reclaim(const T* ptr, size_t size=0) { /* noop for now */ }
         void free() { /* noop for now */ }

      private:
         size_t _offset = 0;
         size_t _size   = 0;
         char*  _base;
   };
#endif

#ifndef PSIZAM_COMPILE_ONLY
   class jit_allocator {
       static constexpr std::size_t segment_size = std::size_t{1024u} * 1024u * 1024u;
   public:
      // allocates page aligned memory with executable permission
      void * alloc(std::size_t size) {
         std::lock_guard l{_mutex};
         size = round_to_page(size);
         auto best = free_blocks_by_size.lower_bound(size);
         if(best == free_blocks_by_size.end()) {
            best = allocate_segment(size);
         }
         if (best->first > size) {
            best = split_block(best, size);
         }
         transfer_node(free_blocks, allocated_blocks, best->second);
         best = transfer_node(free_blocks_by_size, allocated_blocks_by_size, *best);
         return best->second;
      }
      // ptr must be previously allocated by a call to alloc
      void free(void* ptr) noexcept {
         std::lock_guard l{_mutex};
         auto pos = transfer_node(allocated_blocks, free_blocks, ptr);
         transfer_node(allocated_blocks_by_size, free_blocks_by_size, {pos->second, pos->first});

         // merge the freed block with adjacent free blocks
         if(pos != free_blocks.begin()) {
            auto prev = pos;
            --prev;
            pos = maybe_consolidate_blocks(prev, pos);
         }
         auto next = pos;
         ++next;
         if (next != free_blocks.end()) {
            maybe_consolidate_blocks(pos, next);
         }
      }
      std::vector<std::span<const char>> span() const
      {
         std::vector<std::span<const char>> result;
         {
            std::lock_guard l{_mutex};
            for (const auto& [base, size] : _segments)
            {
               result.push_back({reinterpret_cast<const char*>(base), size});
            }
         }
         return result;
      }
      static jit_allocator& instance() {
         static jit_allocator the_jit_allocator;
         return the_jit_allocator;
      }
   private:
      struct segment {
         segment(void * base, std::size_t size) : base(base), size(size) {}
         segment(segment&& other) : base(other.base), size(other.size) {
            other.base = nullptr;
            other.size = 0;
         }
         segment& operator=(const segment& other) = delete;
         ~segment() {
            if(base) {
               ::munmap(base, size);
            }
         }
         void * base;
         std::size_t size;
      };
      using block = std::pair<std::size_t, void*>;
      struct by_size {
         using is_transparent = void;
         bool operator()(const block& lhs, const block& rhs) const {
            return lhs.first < rhs.first || (lhs.first == rhs.first && std::less<void*>{}(lhs.second, rhs.second));
         }
         bool operator()(const block& lhs, std::size_t rhs) const {
            return lhs.first < rhs;
         }
         bool operator()(std::size_t lhs, const block& rhs) const {
            return lhs < rhs.first;
         }
      };
      std::vector<segment> _segments;
      std::set<block, by_size> free_blocks_by_size;
      std::set<block, by_size> allocated_blocks_by_size;
      std::map<void*, std::size_t> free_blocks;
      std::map<void*, std::size_t> allocated_blocks;
      mutable std::mutex _mutex;
      using blocks_by_size_t = std::set<block, by_size>;
      using blocks_t = std::map<void*, size_t>;

      // moves an element from one associative container to another
      // @pre key must be present in from, but not in to
      template<typename C>
      static typename C::iterator transfer_node(C& from, C& to, typename C::key_type key) noexcept {
         auto node = from.extract(key);
         assert(node);
         auto [pos, inserted, _] = to.insert(std::move(node));
         assert(inserted);
         return pos;
      }

      blocks_by_size_t::iterator allocate_segment(std::size_t min_size) {
         std::size_t size = std::max(min_size, segment_size);
         // To avoid additional memory mappings being created during permission changes of
         // from PROT_EXEC to PROT_READ | PROT_WRITE, and back to PROT_EXEC,
         // set permisions to PROT_READ | PROT_WRITE initially.
         // The permission will be changed to PROT_EXEC after executible code is copied.
         void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         PSIZAM_ASSERT(base != MAP_FAILED, wasm_bad_alloc, "failed to allocate jit segment");
         segment s{base, size};
         _segments.emplace_back(std::move(s));
         bool success = false;
         auto guard_1 = scope_guard{[&] { if(!success) { _segments.pop_back(); } }};
         auto pos2 = free_blocks_by_size.insert({size, base}).first;
         auto guard_2 = scope_guard{[&] { if(!success) { free_blocks_by_size.erase(pos2); }}};
         free_blocks.insert({base, size});
         success = true;
         return pos2;
      }

      blocks_by_size_t::iterator split_block(blocks_by_size_t::iterator pos, std::size_t size) {
         bool success = false;
         auto new1 = free_blocks_by_size.insert({size, pos->second}).first;
         auto guard1 = scope_guard{[&]{ if(!success) { free_blocks_by_size.erase(new1); } }};
         auto new2 = free_blocks_by_size.insert({pos->first - size, static_cast<char*>(pos->second) + size}).first;
         auto guard2 = scope_guard{[&]{ if(!success) { free_blocks_by_size.erase(new2); } }};
         free_blocks.insert({new2->second, new2->first});
         // the rest is nothrow
         free_blocks_by_size.erase(pos);
         free_blocks[new1->second] = new1->first;
         success = true;
         return new1;
      }

      blocks_t::iterator maybe_consolidate_blocks(blocks_t::iterator lhs, blocks_t::iterator rhs) noexcept {
         if(static_cast<char*>(lhs->first) + lhs->second == rhs->first) {
            // merge blocks in free_blocks_by_size
            auto node = free_blocks_by_size.extract({lhs->second, lhs->first});
            assert(node);
            node.value().first += rhs->second;
            free_blocks_by_size.insert(std::move(node));
            free_blocks_by_size.erase({rhs->second, rhs->first});
            // merge the blocks in free_blocks
            lhs->second += rhs->second;
            free_blocks.erase(rhs);
            return lhs;
         } else {
            return rhs;
         }
      }

      static std::size_t round_to_page(std::size_t offset) {
         std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
         return (offset + pagesize - 1) & ~(pagesize - 1);
      }
   };
#endif // !PSIZAM_COMPILE_ONLY

   class growable_allocator {
    public:
      static constexpr size_t max_memory_size = sizeof(size_t) >= 8
         ? size_t(16) * 1024 * 1024 * 1024   // 16GB on 64-bit
         : size_t(1) * 1024 * 1024 * 1024;   // 1GB on 32-bit (wasm32)
      template<std::size_t align_amt>
      static constexpr size_t align_offset(size_t offset) { return (offset + align_amt - 1) & ~(align_amt - 1); }

      static std::size_t align_to_page(std::size_t offset) {
#ifdef PSIZAM_COMPILE_ONLY
         constexpr std::size_t pagesize = 4096;
#else
         std::size_t pagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#endif
         assert(max_memory_size % pagesize == 0);
         return (offset + pagesize - 1) & ~(pagesize - 1);
      }

      growable_allocator() {}

      // size in bytes
      explicit growable_allocator(size_t size) {
         PSIZAM_ASSERT(size <= max_memory_size, wasm_bad_alloc, "Too large initial memory size");
         use_default_memory();
      }

      void use_default_memory() {
         PSIZAM_ASSERT(_base == nullptr, wasm_bad_alloc, "default memory already allocated");
#if defined(PSIZAM_COMPILE_ONLY) && !defined(__LP64__)
         // On wasm32, IR data uses a separate per-function allocator so the main
         // allocator only holds parsed module data + native code.  256MB is enough
         // for modules up to ~23k functions producing ~104MB of native code.
         constexpr size_t alloc_size = 256 * 1024 * 1024;
         _base = (char*)std::malloc(alloc_size);
         PSIZAM_ASSERT(_base != nullptr, wasm_bad_alloc, "failed to allocate default memory.");
         _capacity = alloc_size;
#else
         // 64-bit (native or COMPILE_ONLY): mmap demand-paged virtual memory
         _base = (char*)mmap(NULL, max_memory_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         PSIZAM_ASSERT(_base != MAP_FAILED, wasm_bad_alloc, "failed to mmap for default memory.");
         _capacity = max_memory_size;
#endif
      }

      // size in bytes
      void use_fixed_memory(size_t size) {
         PSIZAM_ASSERT(0 < size && size <= max_memory_size, wasm_bad_alloc, "Too large or 0 fixed memory size");
         PSIZAM_ASSERT(_base == nullptr, wasm_bad_alloc, "Fixed memory already allocated");
#if defined(PSIZAM_COMPILE_ONLY) && !defined(__LP64__)
         // 32-bit COMPILE_ONLY (WASM32): use malloc
         _base = (char*)std::malloc(size);
         PSIZAM_ASSERT(_base != nullptr, wasm_bad_alloc, "malloc in use_fixed_memory failed.");
#else
         // 64-bit (native or COMPILE_ONLY): mmap demand-paged
         _base = (char*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         PSIZAM_ASSERT(_base != MAP_FAILED, wasm_bad_alloc, "mmap in use_fixed_memory failed.");
#endif
         _capacity = size;
      }

      ~growable_allocator() {
         if (_base != nullptr) {
#if defined(PSIZAM_COMPILE_ONLY) && !defined(__LP64__)
            // 32-bit COMPILE_ONLY (WASM32): free malloc'd memory
            std::free(_base);
#else
            munmap(_base, _capacity);
#endif
         }
#ifndef PSIZAM_COMPILE_ONLY
         if (_eh_frame_data) {
#ifdef __APPLE__
            __deregister_frame(static_cast<char*>(_eh_frame_data) + 24); // FDE pointer
#else
            __deregister_frame(_eh_frame_data);
#endif
            std::free(_eh_frame_data);
            _eh_frame_data = nullptr;
         }
         if (_is_jit && _code_base) {
            jit_allocator::instance().free(_code_base);
         }
#endif
      }

      // TODO use Outcome library
      template <typename T>
      T* alloc(size_t size = 0) {
         static_assert(max_memory_size % alignof(T) == 0, "alignment must divide max_memory_size.");
         PSIZAM_ASSERT(_capacity % alignof(T) == 0, wasm_bad_alloc, "alignment must divide _capacity.");
         _offset = align_offset<alignof(T)>(_offset);
         // Evaluating the inequality in this form cannot cause integer overflow.
         // Once this assertion passes, the rest of the function is safe.
         PSIZAM_ASSERT ((_capacity - _offset) / sizeof(T) >= size, wasm_bad_alloc, "Allocated too much memory");
         size_t aligned = (sizeof(T) * size) + _offset;
         PSIZAM_ASSERT (aligned <= _capacity, wasm_bad_alloc, "Allocated too much memory after aligned");

         T* ptr  = (T*)(_base + _offset);
         _offset = aligned;
         if (_offset > _largest_offset) {
            _largest_offset = _offset;
         }
         return ptr;
      }

      void * start_code() {
         _offset = align_to_page(_offset);
         return _base + _offset;
      }
      template<bool IsJit>
      void end_code(void * code_base) {
         assert((char*)code_base >= _base);
         assert((char*)code_base <= (_base+_offset));
         _code_base = (char*)code_base;
         _actual_code_size = _offset - ((char*)code_base - _base);
         _offset = align_to_page(_offset);
         _code_size = _offset - ((char*)code_base - _base);
#ifndef PSIZAM_COMPILE_ONLY
         if constexpr (IsJit) {
            auto & jit_alloc = jit_allocator::instance();
            void * executable_code = jit_alloc.alloc(_code_size);
            int err = mprotect(executable_code, _code_size, PROT_READ | PROT_WRITE);
            PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
            std::memcpy(executable_code, _code_base, _code_size);
            _code_base = (char*)executable_code;
            enable_code(IsJit);
            _is_jit = true;
            _offset = (char*)code_base - _base;
            // Register .eh_frame so C++ exceptions can unwind through JIT frames.
            // Only x86_64: macOS aarch64 uses compact unwind (__unwind_info),
            // not DWARF .eh_frame, so __register_frame doesn't work there.
            // aarch64 uses longjmp_on_exception in call_host_function instead.
#if defined(__x86_64__) || (defined(__aarch64__) && defined(__linux__))
            register_jit_eh_frame();
#endif
         }
#endif
      }

      // Sets protection on code pages to allow them to be executed.
      void enable_code([[maybe_unused]] bool is_jit) {
#ifndef PSIZAM_COMPILE_ONLY
         mprotect(_code_base, _code_size, is_jit?PROT_EXEC:(PROT_READ|PROT_WRITE));
#endif
      }
      // Make code pages unexecutable so deadline timer can kill an
      // execution (in both JIT and Interpreter)
      void disable_code() {
#ifndef PSIZAM_COMPILE_ONLY
         mprotect(_code_base, _code_size, PROT_NONE);
#endif
      }

      const void* get_code_start() const { return _code_base; }

      detail::span<std::byte> get_code_span() const {return {(std::byte*)_code_base, _code_size};}
      /// Returns the actual code size before page alignment padding.
      size_t get_actual_code_size() const { return _actual_code_size; }

      /* different semantics than free,
       * the memory must be at the end of the most recently allocated block.
       */
      template <typename T>
      void reclaim(const T* ptr, size_t size=0) {
         PSIZAM_ASSERT( _offset / sizeof(T) >= size, wasm_bad_alloc, "reclaimed too much memory" );
         PSIZAM_ASSERT( size == 0 || (char*)(ptr + size) == (_base + _offset), wasm_bad_alloc, "reclaiming memory must be strictly LIFO");
         if ( size != 0 )
            _offset = ((char*)ptr - _base);
      }

      size_t largest_used_size() {
         return align_to_page(_largest_offset);
      }

      /*
       * Finalize the memory by unmapping any excess pages, this means that the allocator will no longer grow
       */
      void finalize() {
#ifdef PSIZAM_COMPILE_ONLY
         // In compile-only mode, just update the offset — no munmap
         _offset = align_to_page(_offset);
#else
         if(_capacity != _offset) {
            std::size_t final_size = align_to_page(_offset);
            if (final_size < _capacity) { // final_size can grow to _capacity after align_to_page.
                                          // make sure no 0 size passed to munmap
               PSIZAM_ASSERT(munmap(_base + final_size, _capacity - final_size) == 0, wasm_bad_alloc, "failed to finalize growable_allocator");
               _capacity = _offset = final_size;
               if (final_size == 0) {
                  // _base became invalid after munmap if final_size is 0 so
                  // set it to nullptr to avoid being used
                  _base = nullptr;
               }
            }
         }
#endif
      }

      void free() { PSIZAM_ASSERT(false, wasm_bad_alloc, "unimplemented"); }

      void release_base_memory()
      {
         if (_base != nullptr) {
#ifdef PSIZAM_COMPILE_ONLY
            std::free(_base);
#else
            PSIZAM_ASSERT(munmap(_base, _capacity) == 0, wasm_bad_alloc, "failed to release base memory");
#endif
            _base = nullptr;
         }
      }

      void reset() { _offset = 0; }

#ifndef PSIZAM_COMPILE_ONLY
      // Generate and register minimal .eh_frame data covering the JIT code region.
      // This enables C++ exception unwinding through JIT-generated frames.
      // All JIT functions use standard frame pointer convention (CFA = FP + 16),
      // so a single CIE + FDE covers the entire code region.
      void register_jit_eh_frame() {
         if (!_code_base || _code_size == 0) return;
         // 64 bytes: CIE(24) + FDE(32) + terminator(4) + 4 padding
         auto* buf = static_cast<uint8_t*>(std::calloc(64, 1));
         if (!buf) return;
         uint8_t* p = buf;

#ifdef __x86_64__
         // --- CIE (24 bytes) ---
         // length = 20
         p[0] = 20; p[1] = 0; p[2] = 0; p[3] = 0;
         // CIE_id = 0 (already zero from calloc)
         // version = 1
         p[8] = 1;
         // augmentation = "zR"
         p[9] = 'z'; p[10] = 'R'; // p[11] = 0 (null terminator, already zero)
         // code alignment factor = 1
         p[12] = 1;
         // data alignment factor = -8 (SLEB128)
         p[13] = 0x78;
         // return address register = 16
         p[14] = 16;
         // augmentation data length = 1
         p[15] = 1;
         // FDE encoding = DW_EH_PE_absptr (0x00, already zero)
         // Initial instructions:
         // DW_CFA_def_cfa rsp(7), 8
         p[17] = 0x0C; p[18] = 7; p[19] = 8;
         // DW_CFA_offset RA(16), 1 → saved at CFA-8
         p[20] = 0x90; p[21] = 1;
         // p[22], p[23] = padding (zero)

         // --- FDE (32 bytes) ---
         uint8_t* fde = buf + 24;
         // length = 28
         fde[0] = 28; fde[1] = 0; fde[2] = 0; fde[3] = 0;
         // CIE pointer = 28 (offset back to CIE)
         fde[4] = 28; fde[5] = 0; fde[6] = 0; fde[7] = 0;
         // PC begin (64-bit absolute)
         std::memcpy(fde + 8, &_code_base, 8);
         // PC range (64-bit)
         uint64_t range = _code_size;
         std::memcpy(fde + 16, &range, 8);
         // augmentation data length = 0 (already zero)
         // Instructions:
         // DW_CFA_def_cfa rbp(6), 16
         fde[25] = 0x0C; fde[26] = 6; fde[27] = 16;
         // DW_CFA_offset rbp(6), 2 → saved at CFA-16
         fde[28] = 0x86; fde[29] = 2;
         // fde[30], fde[31] = padding (zero)

#elif defined(__aarch64__)
         // --- CIE (24 bytes) ---
         p[0] = 20; p[1] = 0; p[2] = 0; p[3] = 0;
         p[8] = 1;
         p[9] = 'z'; p[10] = 'R';
         p[12] = 4;    // code alignment factor = 4 (ARM64 instructions)
         p[13] = 0x78; // data alignment factor = -8
         p[14] = 30;   // return address register = X30 (LR)
         p[15] = 1;    // augmentation data length = 1
         // FDE encoding = absptr (already zero)
         // Initial: DW_CFA_def_cfa SP(31), 0
         p[17] = 0x0C; p[18] = 31; p[19] = 0;

         // --- FDE (32 bytes) ---
         uint8_t* fde = buf + 24;
         fde[0] = 28; fde[1] = 0; fde[2] = 0; fde[3] = 0;
         fde[4] = 28; fde[5] = 0; fde[6] = 0; fde[7] = 0;
         std::memcpy(fde + 8, &_code_base, 8);
         uint64_t range = _code_size;
         std::memcpy(fde + 16, &range, 8);
         // Instructions:
         // DW_CFA_def_cfa X29(29), 16
         fde[25] = 0x0C; fde[26] = 29; fde[27] = 16;
         // DW_CFA_offset X29(29), 2 → saved at CFA-16
         fde[28] = 0x9D; fde[29] = 2;
         // DW_CFA_offset X30(30), 1 → saved at CFA-8
         fde[30] = 0x9E; fde[31] = 1;
#endif

         // Terminator (4 zero bytes at offset 56, already zero from calloc)
         _eh_frame_data = buf;

#ifdef __APPLE__
         __register_frame(buf + 24); // macOS: pointer to single FDE
#else
         __register_frame(buf);      // Linux: pointer to .eh_frame section
#endif
      }
#endif

      size_t   _offset                = 0;
      size_t   _largest_offset        = 0;
      size_t   _capacity              = 0;
      char*    _base                  = nullptr;
      char*    _code_base             = nullptr;
      size_t   _code_size             = 0;
      size_t   _actual_code_size     = 0;
      bool     _is_jit                = false;
      void*    _eh_frame_data         = nullptr;
   };

#ifdef PSIZAM_COMPILE_ONLY
   // Compile-only stubs for runtime-only types
   template <typename T>
   class fixed_stack_allocator {
    public:
      fixed_stack_allocator(size_t) {}
      template <typename U>
      void free() {}
      inline T* get_base_ptr() const { return nullptr; }
   };

   class wasm_allocator {
    public:
      static constexpr std::size_t page_size_val = 4096;
      static std::int32_t table_offset() { return -static_cast<std::int32_t>(2 * page_size_val); }
      static std::size_t table_size() { return page_size_val; }
      static std::int32_t globals_end() { return -static_cast<std::int32_t>(page_size_val); }
      // Runtime table 0 size, stored at end of table page (before globals pointer)
      static std::int32_t table0_size_offset() { return globals_end() - 12; }
      template <typename T>
      void alloc(size_t = 1) {}
      template <typename T>
      void free(std::size_t = 0) {}
      void free() {}
      wasm_allocator() {}
      void reset(uint32_t) {}
      void reset() {}
      template <typename T>
      inline T* get_base_ptr() const { return nullptr; }
      template <typename T>
      inline T* create_pointer(uint32_t) { return nullptr; }
      inline int32_t get_current_page() const { return 0; }
      bool is_in_region(char*) { return false; }
      inline void set_wasm_allocator(wasm_allocator*) {}
   };
#else
   template <typename T>
   class fixed_stack_allocator {
    private:
      T*     raw      = nullptr;
      size_t max_size = 0;

    public:
      template <typename U>
      void free() {
         munmap(raw, max_memory);
      }
      fixed_stack_allocator(size_t max_size) : max_size(max_size) {
         raw = (T*)mmap(NULL, max_memory, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         PSIZAM_ASSERT( raw != MAP_FAILED, wasm_bad_alloc, "mmap failed to alloca pages" );
         mprotect(raw, max_size * sizeof(T), PROT_READ | PROT_WRITE);
      }
      inline T* get_base_ptr() const { return raw; }
   };

   class wasm_allocator {

    private:
      char*   raw       = nullptr;
      int32_t page      = 0;
      static std::size_t syspagesize() {
         static const std::size_t result = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
         return result;
      }
      static std::size_t prefix_size() {
         return table_size() + syspagesize();
      }
      // Safety invariant: for 32-bit WASM, the maximum effective address is
      // addr(u32_max) + offset(u32_max) + access_size(16 for v128) = 8GB + 14.
      // Since max_memory = 8GB and suffix_size >= 4096, all OOB accesses land
      // in PROT_NONE guard pages. This holds because max_memory >= 2 * max_useable_memory.
      static std::size_t suffix_size() {
         return syspagesize();
      }

    public:
      static std::int32_t table_offset() {
         return -static_cast<std::int32_t>(prefix_size());
      }
      static std::size_t table_size() {
         return syspagesize();
      }
      static std::int32_t globals_end() {
         return -static_cast<std::int32_t>(syspagesize());
      }
      // Runtime table 0 size, stored at end of table page (before globals pointer).
      // Used by JIT to bounds-check call_indirect after table.grow.
      static std::int32_t table0_size_offset() {
         return globals_end() - 12;
      }
      template <typename T>
      void alloc(size_t size = 1 /*in pages*/) {
         if (size == 0) return;
         PSIZAM_ASSERT(page >= 0, wasm_bad_alloc, "require memory to allocate");
         PSIZAM_ASSERT(size + page <= max_pages, wasm_bad_alloc, "exceeded max number of pages");
         int err = mprotect(raw + (page_size * page), (page_size * size), PROT_READ | PROT_WRITE);
         PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
         T* ptr    = (T*)(raw + (page_size * page));
         memset(ptr, 0, page_size * size);
         page += size;
      }
      template <typename T>
      void free(std::size_t size) {
         if (size == 0) return;
         PSIZAM_ASSERT(page >= 0, wasm_bad_alloc, "require memory to deallocate");
         PSIZAM_ASSERT(size <= static_cast<uint32_t>(page), wasm_bad_alloc, "freed too many pages");
         page -= size;
         int err = mprotect(raw + (page_size * page), (page_size * size), PROT_NONE);
         PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
      }
      void free() {
         ::munmap(raw - prefix_size(), max_memory + prefix_size() + suffix_size());
      }
      wasm_allocator() {
         raw  = (char*)::mmap(NULL, max_memory + prefix_size() + suffix_size(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         PSIZAM_ASSERT( raw != MAP_FAILED, wasm_bad_alloc, "mmap failed to alloca pages" );
         int err = ::mprotect(raw, table_size(), PROT_READ | PROT_WRITE);
         PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
         raw += prefix_size();
         page = -1;
      }
      // Initializes the memory controlled by the allocator.
      //
      // \post get_current_page() == new_pages
      // \post all allocated pages are zero-filled.
      void reset(uint32_t new_pages) {
         if (page >= 0) {
            memset(raw, '\0', page_size * page); // zero the memory
         } else {
            int err = ::mprotect(raw - syspagesize(), syspagesize(), PROT_READ);
            PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
            page = 0;
         }
         if(new_pages > static_cast<uint32_t>(page)) {
            alloc<char>(new_pages - page);
         } else if(new_pages < static_cast<uint32_t>(page)) {
            free<char>(page - new_pages);
         }
      }

      // Signal no memory defined
      void reset() {
         if (page >= 0) {
            memset(raw, '\0', page_size * page); // zero the memory
            int err = ::mprotect(raw - syspagesize(), page_size * page + syspagesize(), PROT_NONE);
            PSIZAM_ASSERT(err == 0, wasm_bad_alloc, "mprotect failed");
         }
         page = -1;
      }

      template <typename T>
      inline T* get_base_ptr() const {
         return reinterpret_cast<T*>(raw);
      }
      template <typename T>
      inline T* create_pointer(uint32_t offset) { return reinterpret_cast<T*>(raw + offset); }
      inline int32_t get_current_page() const { return page; }
      bool is_in_region(char* p) { return p >= raw && p < raw + max_memory; }
      std::span<const char> span() const
      {
         return { raw - prefix_size(), max_memory + prefix_size() + suffix_size() };
      }

      std::span<std::byte> get_span() const {
         const std::size_t syspagesize = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
         return {(std::byte*)raw - syspagesize, max_memory + 2*syspagesize};
      }
   };
#endif // !PSIZAM_COMPILE_ONLY
} // namespace psizam
