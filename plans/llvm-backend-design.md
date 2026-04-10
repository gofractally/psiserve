# psizam Architecture Evolution: Decoupled Compilation, LLVM Backend, and Sandboxed Execution

## Overview

This document captures the full design for evolving psizam from a monolithic template-heavy engine into a cleanly separated compile/execute architecture with:

1. **Decoupled host functions** — unified trampoline, runtime engine selection
2. **Three compilers** — jit1, jit2, jit_llvm (runtime-selectable)
3. **Two executors** — native and interpreter (compiler-agnostic)
4. **Two execution modes** — objective (deterministic/consensus) and subjective (native float/max perf)
5. **Sandboxed compilation** — compiler-in-WASM via psizam itself
6. **Code caching** — signed, position-independent native code artifacts

---

## 1. Architectural Separation: Compile vs Execute

### Current Architecture (Monolithic)

Everything is coupled through the `Host` template parameter:

```
backend<Host, Impl, Options>
  → Impl::context<Host>              (jit_execution_context<Host>)
  → Impl::parser<Host, Opts>         (binary_parser<writer<context<Host>>>)
    → machine_code_writer<context<Host>>
      → static call_host_function(context<Host>*, ...)
```

Changing the backend requires recompiling the entire application. Host function types are baked into every layer at compile time.

### New Architecture (Decoupled)

```
┌─────────────────────────────────┐
│  Host Function Table            │  ← Thin header (only template piece)
│  table.add<&fn>("mod","name")   │     Generates trampolines at compile time
│  Produces runtime table:        │     Stores function pointers + WASM signatures
│    { trampoline, signature }[]  │
└──────────┬──────────────────────┘
           │ runtime table pointer
    ┌──────┴──────┐
    │             │
    v             v
┌────────┐   ┌──────────┐
│Compiler│   │ Executor  │
│        │   │           │
│ jit1   │   │ native    │  ← Same native code blob,
│ jit2   │   │ interp    │     same dispatch, same table
│jit_llvm│   │           │
└───┬────┘   └─────┬─────┘
    │              │
    v              v
┌────────────────────┐
│ compiled_module    │  ← native code blob + metadata
│  code_blob[]       │     position-independent
│  jit_code_offset[] │     serializable to .pzam cache
│  func_type_index[] │
└────────────────────┘
```

### What Changes

| Component | Before | After | Lines changed |
|-----------|--------|-------|---------------|
| `host_function.hpp` | `registered_host_functions<Host>` template | + `host_function_table` runtime class | ~150 new lines |
| `execution_context.hpp` | `execution_context<Host>`, `jit_execution_context<Host>` | Non-templated on Host; holds `host_function_table*` + `void* host` | ~100 modified |
| `backend.hpp` | `backend<Host, Impl, Options>` | `backend<Impl, Options>` or runtime `compile()` function | ~50 modified |
| `x86_64.hpp` | `machine_code_writer<Context<Host>>` | `machine_code_writer` (concrete) | ~5 (remove template param) |
| `jit_codegen.hpp` | `jit_codegen<Context<Host>>` | `jit_codegen` (concrete) | ~5 |
| `jit_codegen_a64.hpp` | Same | Same | ~5 |
| `aarch64.hpp` | Same | Same | ~5 |
| `ir_writer.hpp` | `ir_writer<Context<Host>>` | `ir_writer` (concrete) | ~5 |
| `parser.hpp` | No change (already Host-agnostic through writer) | No change | 0 |
| `jit_ir.hpp`, `jit_optimize.hpp`, `jit_regalloc.hpp` | No change | No change | 0 |
| Tools/benchmarks | `backend<rhf_t, jit2>` | `backend<jit2>` + table, or `compile(bytes, table, engine)` | ~5 per file |

**Total: ~3 files with real logic changes, ~5 files with mechanical de-templating, ~0 performance loss.**

---

## 2. Unified Host Function Trampoline

### The Problem Today

Two different calling conventions for host functions:

- **JIT path**: Args as `native_value[]` in reverse stack order → `fast_trampoline_rev`
- **Interpreter path**: Args on `operand_stack` (variant) → `std::function` + `Type_Converter` (slow)

Plus two trampoline variants (`fast_trampoline_fwd`, `fast_trampoline_rev`) and a slow `std::function` fallback.

### Unified Design

One trampoline signature, one arg order, used by ALL executors:

```cpp
// The ONE trampoline type — generated at compile time, called at runtime
using host_trampoline_t = native_value(*)(void* host, native_value* args,
                                          uint32_t num_args, char* memory);
```

`args[]` is always in **forward order** (args[0] = first WASM parameter).

```cpp
struct host_function_table {
    struct entry {
        host_trampoline_t  trampoline;     // compile-time generated, runtime called
        host_function      signature;      // WASM-level param/return types
        std::string        module_name;
        std::string        func_name;
    };
    std::vector<entry>     entries;
    std::unordered_map<host_func_pair, uint32_t>  name_map;

    // ── This ONE method stays templated ──
    template<auto Func>
    void add(const std::string& mod, const std::string& name) {
        // Template metaprogramming extracts C++ types, generates trampoline
        // Stores {trampoline_ptr, wasm_signature, names} in entries[]
    }

    void resolve(module& mod);   // Runtime: maps WASM imports → entry indices
    native_value call(void* host, uint32_t mapped_idx,
                      native_value* args, uint32_t num_args, char* memory);
};
```

### How Each Executor Calls Host Functions

**JIT (native executor):**
```
Generated code pushes args as native_value[] (forward order)
  → call_host_function(ctx, stack, func_idx, depth)
    → ctx->table->call(ctx->host, mapped_idx, stack, num_args, mem)
      → table->entries[idx].trampoline(host, args, num_args, mem)
        → actual C++ function
```

**Interpreter:**
```
Pop operand_stack into native_value args[N]  (trivial: N assignments)
  → ctx->table->call(ctx->host, mapped_idx, args, num_args, mem)
    → same trampoline
      → same C++ function
```

### Performance

Same as today. The current fast path does: `mappings::get().fast_rev[idx](host, stack, mem)`. The new path does: `table->entries[idx].trampoline(host, args, num_args, mem)`. Same number of indirections, same function pointer call, same compile-time-generated trampoline code.

### What Gets Deleted

- `fast_trampoline_rev` (merged into single trampoline)
- `fast_trampoline_fwd` (merged into single trampoline)
- `std::function` slow path (unnecessary with unified trampoline)
- `Type_Converter` from host call hot path (only needed at registration time)
- `_rhf` member and `host_invoker_t` type trait
- `if constexpr (!std::is_same_v<Host, std::nullptr_t>)` branches

---

## 3. Compilers and Executors

### Three Compilers

All produce the same output: a position-independent native code blob with per-function `jit_code_offset`.

| Compiler | Input | Technique | Speed |
|----------|-------|-----------|-------|
| **jit1** | WASM bytes | Single-pass direct x86_64/aarch64 emission during parsing | ~10 us |
| **jit2** | WASM bytes | Two-pass: WASM → psizam IR → optimize → regalloc → codegen | ~100 us |
| **jit_llvm** | WASM bytes | Two-pass: WASM → psizam IR → LLVM IR → LLVM opt → ORC JIT | ~10-100 ms |

All three take the same inputs: `(wasm_bytes, host_function_table)` and produce the same output format. The host_function_table provides WASM-level type signatures for import validation — no C++ type information needed.

### Executors

| Executor | Input | Dispatch |
|----------|-------|----------|
| **native** | Native code blob | `jit_code_offset + _code_base` → function pointer call |
| **interpreter** | Bitcode | `interpret_visitor` walks opcodes |
| **wamr_interp** | WASM bytes | WAMR's register-based fast interpreter (optional, external) |

The native executor doesn't know or care which compiler produced the code. jit1, jit2, jit_llvm, or a cache file — it's all the same `jit_code_offset + _code_base` dispatch.

#### External Interpreter Integration (WAMR)

WAMR's register-based fast interpreter is significantly faster than a traditional stack-based interpreter. It can be integrated as an alternative executor without rewriting it — WAMR takes raw WASM bytes and manages its own parsing/compilation to internal register bytecode.

**Integration levels:**

| Feature | Shallow (~200 lines) | Medium (~500 lines) | Deep (~1000+ lines, fork) |
|---------|---------------------|---------------------|--------------------------|
| Host functions | Bridge: WAMR calling convention → our trampoline | Same | Same |
| Linear memory | WAMR-managed | Our mmap via `Alloc_With_Pool` | Our guard-paged allocator |
| Guard pages | Lost | Present but redundant (WAMR still bounds-checks; interpreter overhead dominates so no perf cost) | Full (patch out WAMR bounds checks) |
| COW snapshots | Lost | Works (memory pool is in our mmap region) | Full |
| Watchdog | Lost | `wasm_runtime_terminate()` from timer thread | Same |

**Recommended: Medium integration.** Point WAMR's memory pool at our guard-paged mmap region. WAMR's bounds checks become redundant but harmless — interpretation overhead dominates, so the extra checks cost nothing. COW works because the linear memory lives in our controlled mmap. Watchdog uses WAMR's own terminate API from a timer thread.

**Host function bridge:**
```
WAMR native call → adapter(wamr_exec_env, wamr_args)
    → extract args as native_value[]
    → table.entries[idx].trampoline(host, args, n, mem)
    → pack return value back to WAMR format
```

**Limitations — WAMR is subjective-only:**

| Feature | psizam engines (interp, jit1, jit2, jit_llvm) | wamr_interp |
|---------|-----------------------------------------------|-------------|
| Objective mode | Yes — softfloat, bit-identical results | **No** — WAMR uses its own float impl, can't swap in our softfloat |
| Softfloat | Yes — Berkeley SoftFloat-3 calls | **No** — would require patching WAMR's interpreter loop |
| Yield | Yes — compiler inserts `test/jnz` at backedges | **Coarse only** — `wasm_runtime_terminate()` from timer, not per-backedge |
| Debug metadata | Yes — compiler emits stack recovery data | **No** — WAMR has its own debug format |
| Consensus participation | Yes | **No** |

WAMR is a fast interpreter for **subjective, non-consensus paths** only: HTTP serving, local dev, debugging, cases where you want interpreter-level safety without JIT warmup and don't need determinism.

### Runtime Selection

```cpp
host_function_table table;
table.add<&my_identity>("env", "identity");
table.add<&my_mem_op>("env", "mem_op");

// Compile options — all runtime, any combination valid
struct compile_options {
    engine   engine    = engine::jit2;
    mode     mode      = mode::objective;
    bool     softfloat = true;    // objective forces true; subjective defaults false
    bool     debug     = false;   // stack recovery metadata in code stream
    bool     yield     = false;   // yield checks at loop backedges
};

// engine::jit1, jit2, jit_llvm  → compile to native, run with native executor
// engine::interpreter            → compile to bitcode, run with interpret_visitor
// engine::wamr_interp            → delegate to WAMR (optional external dep)

// Validate only — reject bad modules without paying compile cost
psizam::validate(wasm_bytes, table);   // throws on invalid WASM

// Compile (includes validation)
auto mod = psizam::compile(wasm_bytes, table, {
    .engine = engine::jit_llvm,
    .mode   = mode::subjective,
    .yield  = true,
});
// or: auto mod = psizam::load_cached("module.pzam", table);

// Executor is compiler-agnostic
execution_context ctx(mod, table, &my_host);

// Resolve export name → typed function handle ONCE at setup
// Template generates a compile-time trampoline (symmetric with host function trampolines):
//   host add<&fn>():  native_value[] → C++ types (inbound)
//   get_func<Sig>():  C++ types → WASM ABI registers (outbound)
auto handle_request = ctx.get_func<int32_t(int32_t, int32_t)>("handle_request");

// Call many times — zero overhead: args → registers → jit_code_offset[idx] + _code_base
auto result = handle_request(arg1, arg2);
handle_request(arg3, arg4);
```

### Combinatorics

Five options sounds like a combinatorial explosion, but it's manageable because **each flag is a local, independent decision** in the compiler:

| Flag | Where checked | What changes |
|------|---------------|-------------|
| engine | Top-level dispatch | Which compiler runs (separate code paths already) |
| softfloat | Each float opcode | `call softfloat_fn` vs native `fadd` — one if/else |
| debug | Function entry/exit | Emit extra metadata — additive |
| yield | Loop backedges | Emit `test/jnz` — additive |
| mode | Validation only | objective implies softfloat=true |

No flag interacts with another in the codegen. The compiler is one code path with independent branch points, not 2^N specializations. The only combinatorial cost is in **cache files** (each unique combo = different `.pzam`), which is fine — you only cache what you actually use.

---

## 4. Objective and Subjective Modes

### Objective Mode (Deterministic / Consensus)

- All engines (interpreter, jit1, jit2, jit_llvm) MUST produce **bit-identical execution results** for the same WASM program and inputs
- Float operations → softfloat calls (Berkeley SoftFloat-3, deterministic IEEE-754)
- Required for consensus-critical computation (blockchain transactions)
- Different compilers CAN produce different native code — but execution results must be identical

### Subjective Mode (Non-Deterministic / Max Performance)

- Native hardware float instructions allowed
- Results may differ across platforms, engines, compiler versions
- Used for non-consensus work (HTTP serving, local computation)
- Full LLVM float optimization: constant folding, vectorization, reassociation

### Softfloat as Independent Flag

Softfloat is decoupled from mode so it can be used for **cross-engine validation** in subjective mode:

- `mode::objective` → forces `softfloat = true` (error if explicitly set false)
- `mode::subjective` → defaults `softfloat = false`, but can be set `true` for validation

```cpp
// Consensus: objective always uses softfloat
auto mod = psizam::compile(wasm_bytes, table, { .mode = mode::objective });

// Max perf: subjective with native float
auto mod = psizam::compile(wasm_bytes, table, { .mode = mode::subjective, .softfloat = false });

// Validation: subjective with softfloat (compare results across engines)
auto mod = psizam::compile(wasm_bytes, table, { .mode = mode::subjective, .softfloat = true });
```

When `softfloat = true`, the compiler emits `call @_psizam_f32_add(a, b)` for every float add.
When `softfloat = false`, the compiler emits native `fadd` / `addss`.

All options are recorded in the compiled module metadata (and in the `.pzam` cache file).

### SIMD Float Operations

Same split applies to SIMD float ops:
- Objective: `call @_psizam_f32x4_add(a, b)` (softfloat vector wrappers)
- Subjective: LLVM `<4 x float> fadd` → native SSE/AVX/NEON

---

## 5. The LLVM Backend (jit_llvm)

### Where LLVM Plugs In

```
WASM binary
    │
    │ binary_parser calls ir_writer::emit_*()  [IDENTICAL to jit2]
    ▼
ir_writer (Pass 1)          ── WASM stack machine → virtual-register IR
    │
    │ SKIP jit_optimizer, jit_regalloc, jit_codegen entirely
    │ (psizam's optimizations would limit LLVM's freedom — see below)
    ▼
llvm_ir_translator          ── psizam ir_inst[] → LLVM IR (llvm::Module)
    │                           Mechanical switch over ~150 ir_op opcodes
    ▼
LLVM PassManager            ── O0/O1/O2/O3 optimization
    │
    ▼
ORC JIT v2 (LLJIT)          ── LLVM IR → native code
    │                           Custom memory manager → growable_allocator
    ▼
Native code blob            ── Same format as jit1/jit2 output
```

### Why LLVM Gets Raw IR (No Pre-Optimization)

The ir_writer output is ideal LLVM input: virtual registers (unlimited supply, no physical assignment), basic blocks with loop/branch structure, and no optimization decisions made. Three approaches were considered:

| Approach | What LLVM gets | LLVM's freedom | Cost |
|----------|---------------|----------------|------|
| **A. ir_writer → LLVM** ✓ | Virtual-register IR, unoptimized | Full — LLVM does all opt + regalloc | Translate ~150 ir_ops |
| **B. WASM → LLVM directly** | Raw WASM stack machine | Maximum — sees original structured control flow | Reimplement ~1100 lines of ir_writer |
| **C. ir_writer + jit_optimizer → LLVM** | Pre-optimized IR | **Reduced** — patterns already lowered | Same as A, worse output |

**A is the right choice:**
- ir_writer's output is essentially SSA-like — exactly what LLVM expects
- No physical register constraints — LLVM's register allocator has full freedom (critical for SIMD: XMM/YMM allocation)
- No premature optimization — LLVM's constant folding, DCE, strength reduction, loop-invariant code motion, and vectorization are all strictly superior to psizam's simple passes
- psizam's `jit_optimizer` does strength reduction that could transform patterns LLVM would have optimized differently/better — pre-optimization can actively hurt

**B is marginally better** (LLVM gets WASM's structured control flow directly for loop analysis) but costs ~1100 lines of reimplementation. Not worth it — LLVM reconstructs loop structure from basic blocks routinely.

**Key invariant:** The `jit_optimizer`, `jit_regalloc`, and `jit_codegen` are ONLY used by the jit2 path. The jit_llvm path completely bypasses them.

### IR Translation (psizam IR → LLVM IR)

```
psizam IR                          LLVM IR
─────────                          ───────
const_i32 dest=v0 imm=42          %v0 = i32 42
i32_add   dest=v2 s1=v0 s2=v1    %v2 = add i32 %v0, %v1
i32_load  dest=v3 s1=v2 off=8    %ptr = gep i8, ptr %mem, i64 (zext %v2 + 8)
                                   %v3 = load i32, ptr %ptr
br_if     target=3 s1=v4          br i1 (trunc %v4), label %block3, label %next
call      index=7                  call @wasm_func_7(...)
v128_op(i32x4_add) s1 s2          %r = add <4 x i32> %s1, %s2
```

### LLVM Function Signatures

Clean SysV-compatible signatures for LLVM optimization:

```llvm
define i64 @wasm_func_N(ptr %ctx, ptr %mem, i32 %p0, i64 %p1) {
```

Per-function trampolines bridge between psizam's JIT ABI (`rdi`=ctx, `rsi`=mem, args on stack) and LLVM's SysV CC. Internal WASM-to-WASM calls use direct SysV calls — no trampoline overhead within a module.

### SIMD: The Biggest Win

jit2 has no SIMD register allocation — falls back to stack-based codegen. LLVM provides:
- Full register allocation for vector values (XMM/YMM/NEON)
- Optimal instruction selection (SSE4.2, AVX2, NEON)
- Auto-vectorization of scalar loops
- Multiply-add fusion

### Environment Compatibility

| Concern | Status |
|---------|--------|
| Guard page memory model | Preserved — don't mark loads as `dereferenceable` |
| Signal handler | Compatible — code lives in jit_allocator via custom memory manager |
| Watchdog (mprotect PROT_NONE) | Compatible — code in standard jit_allocator region |
| COW snapshots (fork) | Compatible — ORC session destroyed after compilation |
| Function dispatch | Compatible — same `jit_code_offset + _code_base` |
| Host function calls | Compatible — same `call_host_function` through unified table |

### File Organization

```
libraries/psizam/
  include/psizam/
    host_function_table.hpp      # NEW: Runtime table + templated add<F>()
    backend.hpp                  # Simplified: no Host template
    llvm_ir_writer.hpp           # NEW: Thin wrapper — ir_writer + LLVM Pass 2
    llvm_ir_translator.hpp       # NEW: psizam IR → LLVM IR (header)
    llvm_jit_compiler.hpp        # NEW: ORC JIT setup (header)
  src/                           # NEW directory
    llvm_ir_translator.cpp       # Translation implementation (~800-1200 lines)
    llvm_jit_compiler.cpp        # ORC JIT + memory manager (~300-500 lines)
  CMakeLists.txt                 # + PSIZAM_ENABLE_LLVM, psizam-llvm target
```

### CMake Integration

```cmake
option(PSIZAM_ENABLE_LLVM "Enable LLVM JIT backend (jit_llvm)" OFF)
if(PSIZAM_ENABLE_LLVM)
   find_package(LLVM REQUIRED CONFIG)
   # Minimum LLVM 15 for stable ORC JIT v2 API
   llvm_map_components_to_libnames(LLVM_LIBS
      core support orcjit native passes target)
   add_library(psizam-llvm STATIC
      src/llvm_ir_translator.cpp
      src/llvm_jit_compiler.cpp)
   target_link_libraries(psizam-llvm PUBLIC psizam ${LLVM_LIBS})
   target_compile_definitions(psizam-llvm PUBLIC PSIZAM_ENABLE_LLVM)
endif()
```

---

## 6. Sandboxed Compilation: Compiler-in-WASM

### Vision

Compile the entire compilation pipeline (psizam + optionally LLVM) to WASM. Run the compiler inside psizam itself — psizam executes psizam-in-WASM. No external runtime dependency (not Wasmer).

### Threat Model

| Threat | Native compiler | Compiler-in-WASM (via psizam) |
|--------|----------------|-------------------------------|
| Malicious input crashes compiler | Host process dies | WASM trap → graceful error |
| "Temple bomb" (unbounded memory) | OOM-kill | `memory.grow` fails at limit → clean error |
| "Temple bomb" (unbounded CPU) | Hang | Watchdog / fuel kills execution |
| Non-determinism (ASLR, allocator) | Platform-dependent output | Deterministic (WASM semantics) |
| Different compiler versions | Depends on system install | Versioned `.wasm` artifact |

### Pipeline

```
Untrusted WASM module (bytes)
    │
    │ Host writes bytes into psizam sandbox's linear memory
    ▼
psizam-in-WASM (running inside psizam, native jit2)
    │  Memory: hard cap (e.g., 512MB WASM linear memory)
    │  CPU: bounded by psizam's watchdog
    │  Crash: WASM trap → graceful error
    │  Contains: parser + ir_writer + jit_codegen (or + LLVM)
    │  Cross-compiles: emits x86_64/aarch64 bytes into linear memory
    ▼
Host reads native code blob from sandbox's linear memory
    │
    ▼
Sign → save to .pzam cache   ──or──   Load into jit_allocator
```

### Why It Works

psizam's codegen writes x86_64/aarch64 bytes to a buffer. It doesn't execute them. When compiled to WASM:
- Replace `growable_allocator` (mmap) with simple buffer allocator
- Strip `mprotect` and signal handler setup (not needed during compilation)
- Export: `compile(wasm_ptr, len, opt_level) → (code_ptr, code_len)`
- Generated bytes are identical regardless of where the compiler runs

### Determinism

Same compiler `.wasm` + same input = bit-identical native code. WASM execution is deterministic: no ASLR, deterministic heap allocator, no pointer-value-dependent hash ordering. The compiler `.wasm` is a versioned artifact.

### Compilation Modes

| Mode | Where compiler runs | Isolation | Speed |
|------|-------------------|-----------|-------|
| `jit1` native | Host CPU | None | ~10 us |
| `jit2` native | Host CPU | None | ~100 us |
| `jit1`-in-WASM | psizam sandbox | Full | ~20-50 us |
| `jit2`-in-WASM | psizam sandbox | Full | ~150-500 us |
| `jit_llvm`-in-WASM | psizam sandbox | Full | ~100-200 ms |

---

## 7. Native Code Caching (AOT Serialization)

### Position-Independence

For native code to be saveable/loadable at any base address, it must be PIC.

**Already PIC:**
- WASM-to-WASM calls: relative `E8` + 32-bit offset (contiguous blob)
- All branches: relative jumps
- `br_table`: sequential `cmp/je` (no jump tables)
- Memory access: via register (mem_base in `rsi`/`x20`)
- Global access: via register (context in `rdi`/`x19`)

**Requires fix:**
- Host function stubs: embed absolute address of `call_host_function`
- Fix: indirect call through context: `call [rdi + host_dispatch_offset]` — fully PIC

### Cache File Format

```
┌──────────────────────────────────────────┐
│ Header                                   │
│   magic: "PZAM"                          │
│   format_version: u16                    │
│   target: u8 (x86_64 / aarch64)         │
│   mode: u8 (objective / subjective)      │
│   debug: u8 (0 or 1)                    │
│   yield: u8 (0 or 1)                    │
│   opt_level: u8 (O0-O3)                 │
│   compiler_hash: [32]u8                  │
│   input_hash: [32]u8                     │
│   num_functions: u32                     │
│   code_size: u32                         │
├──────────────────────────────────────────┤
│ Function table                           │
│   [jit_code_offset, func_type_idx] × N   │
├──────────────────────────────────────────┤
│ Native code blob (PIC)                   │
│   Offset 0: Entry trampoline             │
│   Offset N: Error handlers               │
│   Offset M+: Function bodies             │
├──────────────────────────────────────────┤
│ Signature (64 bytes, Ed25519)            │
│   Signs: all preceding bytes             │
│   Key: operator-held, not in sandbox     │
└──────────────────────────────────────────┘
```

### Load Path

```
1. Read .pzam file
2. Verify signature (Ed25519) BEFORE mapping as executable
3. Verify: target arch, mode, compiler_hash, input_hash
4. If invalid → reject, recompile, re-sign, save
5. Allocate from jit_allocator
6. Copy code blob, set _code_base + jit_code_offset per function
7. mprotect(PROT_EXEC)
8. Ready for fast dispatch
```

### Cache Invalidation

Automatic when any of these change:
- Input WASM module (`input_hash`)
- Compiler version (`compiler_hash` — SHA-256 of compiler `.wasm` or git commit)
- Target architecture
- Any compile option (mode, debug, yield, optimization level)

### Signing

The host signs native code after extraction from the sandbox. The signing key is held by the host (or HSM/keychain), never inside the WASM sandbox. Options:
- Per-installation key pair (protects against local cache tampering)
- Operator-provided key (supply chain protection)

---

## 8. Expected Performance

| Metric | jit1 | jit2 | jit_llvm | Notes |
|--------|------|------|----------|-------|
| Compile time | ~10 us | ~100 us | ~10-100 ms | |
| Host call overhead | Baseline | Same | Same | Unified trampoline |
| Compute (SHA-256) | 2-3x native | 1.5-2x native | 1.1-1.5x native | LLVM loop opts |
| SIMD throughput | N/A | Poor | Near-native | LLVM regalloc + ISA selection |
| Compile memory | ~50 KB | ~100 KB | ~10-50 MB | |

Cached code: zero compilation cost on subsequent loads.

---

## 9. Cooperative Yield

When `compile_options::yield = true`, the compiler inserts lightweight flag checks at loop backedges:

```asm
; ~1 cycle branch-predicted, no function call on the fast path
test byte [rdi + yield_flag_offset], 1
jnz  yield_handler
```

### How It Works

- The yield flag lives in the context struct (register-indirect via `rdi`) — fully PIC
- The scheduler sets the flag from outside; generated code polls it
- Only when the flag is set does execution jump to the yield handler (cold path)
- The exact yield handler mechanism (host call, longjmp, fiber switch) is a later design decision

### Cost

~1 cycle per backedge (branch-predicted not-taken). Much cheaper than a host call trampoline on every iteration.

### Interactions

- All compilers (jit1, jit2, jit_llvm) emit yield checks when `yield = true`
- Cached `.pzam` files include yield checks — no re-instrumentation on load
- LLVM can optimize: prove bounded loops don't need checks, elide from inner loops with known trip counts
- Orthogonal to debug/mode — any combination valid
- When `yield = false`, no checks emitted (blockchain path)

---

## 10. Implementation Plan

### Role of the Interpreter

The interpreter is a **validation oracle**, not a production executor. Its purpose is to provide a trusted reference implementation that all compiled outputs are tested against. Every phase validates compiled execution results against the interpreter. The interpreter itself requires minimal changes — it already works.

### Dependency Graph

```
Phase 1 (EOS cleanup)
    │
    v
Phase 2 (Public API + de-template)
    │
    ├──────────────────┬──────────────────┐
    v                  v                  v
Phase 3 (PIC +    Phase 4 (jit_llvm)  Phase 5 (WASM 3.0)
 code caching)        │                  │
    │                  │              [feeds into all
    └──────┬───────────┘               backends as
           v                           they evolve]
Phase 6 (Yield)
           │
           v
Phase 7 (Sandboxed compilation)
```

Phases 3, 4, and 5 are **independent** after Phase 2 and can be worked in parallel. WASM 3.0 features are additive — each sub-feature (exceptions, ref types, etc.) is a self-contained addition to parser + backends.

### Phase 1: EOS Cleanup — *~1–2 days, pure deletion*

Remove spec-deviating EOS backward compatibility. No behavior changes for conforming WASM.

**Delete — spec deviations:**

| Item | Files | What to do |
|------|-------|------------|
| `eosio_options` struct | `options.hpp` | Delete struct |
| `psizam_fp` (buggy float) | `interpret_visitor.hpp` (~14), `x86_64.hpp` (~13), `aarch64.hpp` (~13) | Delete `if (psizam_fp)` branches, keep spec path |
| `allow_code_after_function_end` | `parser.hpp` (5 uses) | Delete flag, keep spec behavior |
| `allow_u32_limits_flags` | `parser.hpp` (1 use) | Delete flag, keep spec behavior |
| `allow_invalid_empty_local_set` | `parser.hpp` (1 use) | Delete flag, keep spec behavior |
| `allow_zero_blocktype` | `parser.hpp` (3 uses) | Delete flag, keep spec behavior |
| `forbid_export_mutable_globals` | `options.hpp` | Delete flag |
| 4 EOS-compat test files | `tests/allow_*.cpp` | Delete |

**Keep:** Resource limits (`max_pages`, `max_call_depth`, etc.) and feature gates (`enable_simd`, etc.).

**Validation:** All spec tests pass. EOS-compat tests deleted.
**Output:** Cleaner codebase, ~40 fewer branches in codegen.

### Phase 2: Public API + Architecture Decoupling — *~1–2 weeks*

The core refactor. Expose the clean public API and decouple Host from the template chain.

**2a. `host_function_table` + unified trampoline**
- New `host_function_table` class: runtime table with `add<&fn>(mod, name)` as the only template
- Single trampoline signature: `native_value(*)(void* host, native_value* args, uint32_t n, char* mem)`
- `resolve(module&)` maps WASM imports → table indices at load time
- Delete: `fast_trampoline_rev`, `fast_trampoline_fwd`, `std::function` slow path, `Type_Converter` from hot path

**2b. De-template the engine**
- Remove `Host` template from `execution_context`, `jit_execution_context`
- Context holds `host_function_table*` + `void* host` instead of `Host*`
- De-template `machine_code_writer`, `jit_codegen`, `ir_writer` (remove `Context<Host>` param)
- Host call stubs: `table->entries[idx].trampoline(host, args, n, mem)` — same indirection count

**2c. `compile_options` + public API**
```cpp
psizam::validate(wasm_bytes, table);
auto mod = psizam::compile(wasm_bytes, table, { .engine = engine::jit2 });
auto mod = psizam::load_cached("module.pzam", table);
execution_context ctx(mod, table, &my_host);
ctx.execute("entry", args...);
```

**2d. Update all consumers**
- Tools (`psizam-interp`, etc.), benchmarks, tests use new API
- Old `backend<Host, Impl>` becomes a thin compatibility wrapper or is deleted

**Validation:** Full spec test suite passes with all existing engines (interpreter, jit1, jit2) through new API. Interpreter output matches jit1/jit2 output for all tests.
**Output:** Clean public API, runtime engine selection, no Host template propagation.

### Phase 2.5: Cross-Engine Compliance Harness — *~3-5 days, then ongoing*

Build the automated cross-implementation testing infrastructure. Runs **continuously alongside all subsequent phases**.

- Extend existing benchmark harness (wasmtime, wasmer, WAMR already integrated) into a compliance comparison tool
- Add the WASM spec interpreter (OCaml reference impl from the spec repo) as ground truth
- Add V8 via d8 or Node.js as browser-grade reference
- Generate edge-case WASM modules targeting known ambiguities: NaN bit patterns, signed zero, trap boundaries, alignment, unreachable code paths
- CI job: run all test cases through all engines, report disagreements automatically
- Output: `plans/spec-compliance/` directory with per-section compliance reports

**Categories of results:**
- **All agree, psizam matches** → conforming ✓
- **All agree, psizam disagrees** → BUG — fix immediately
- **Industry split** → document both sides, pick and justify for consensus
- **Only spec interpreter differs from all production engines** → likely spec bug or intentional production deviation — investigate

**2.5b. Structured fuzzing infrastructure**
- Replace random-byte fuzzer with wasm-smith as primary generator (valid WASM that exercises compiler + executor, not just parser)
- Add wasm-mutate for mutation-based coverage expansion from seed corpus
- Wire into differential oracle: same module through all psizam backends + external engines
- Fuzzing targets: parse, compile, execute, serialize round-trip, cross-option determinism
- Consider OSS-Fuzz registration for continuous 24/7 campaigns

**Output:** Living compliance matrix + continuous fuzzing. Every subsequent phase (LLVM, WASM 3.0, etc.) feeds test results into this harness automatically.

### Phase 3: PIC + Code Caching — *~1 week*

Position-independent code and signed cache files. Can proceed **in parallel** with Phase 4.

**3a. PIC fixes**
- Host function stubs: absolute address → `call [rdi + host_dispatch_offset]`
- Audit remaining absolute references (should be minimal — br_table is already `cmp/je`)
- Add PIC verification test: compile, relocate to different base, execute

**3b. `.pzam` file format**
- Header: magic, version, target arch, compile_options (mode, softfloat, debug, yield), compiler_hash, input_hash
- Body: function table (offsets + type indices) + native code blob
- Footer: Ed25519 signature (signs all preceding bytes)

**3c. `psizam::load_cached()` implementation**
- Verify signature before mapping as executable
- Verify: arch, options, input_hash, compiler_hash
- On mismatch → reject (caller recompiles + re-signs)
- Copy into jit_allocator, mprotect, ready for dispatch

**Validation:** Compile with jit1/jit2, save to `.pzam`, load from cache, execute — results match interpreter.
**Output:** Zero-cost startup for cached modules. Tamper-evident cache files.

### Phase 4: LLVM Backend — *~3–4 weeks*

The main performance win. Can proceed **in parallel** with Phase 3.

**4a. Scaffolding (~2 days)**
- CMake: `PSIZAM_ENABLE_LLVM`, `find_package(LLVM)`, `psizam-llvm` target
- `llvm_jit_compiler.cpp`: ORC JIT v2 setup, custom memory manager → growable_allocator
- Stub `llvm_ir_translator.cpp` that handles `unreachable` only

**4b. Integer + control flow (~1 week)**
- Translate psizam IR → LLVM IR: `const_i32/i64`, `i32_add/sub/mul/...`, `i32_load/store`
- Control flow: blocks, loops, br, br_if, br_table, return
- Locals, globals, function calls (internal WASM-to-WASM)
- **Validate:** Integer spec tests pass, output matches interpreter

**4c. Float + softfloat (~1 week)**
- Float ops: when `softfloat = true` → `call @_psizam_f32_add(a, b)`; when `false` → native `fadd`
- Conversions, min/max with NaN canonicalization
- **Validate:** Float spec tests pass in both softfloat modes, output matches interpreter

**4d. Host calls + memory (~3 days)**
- Host call emission through unified `host_function_table`
- mem_base reload after any host call (host may grow memory)
- Call depth tracking
- **Validate:** Host function tests pass

**4e. SIMD (~1 week)**
- `v128_op` → LLVM `<4 x i32>`, `<4 x float>`, etc.
- Softfloat SIMD wrappers for objective mode
- LLVM handles register allocation (XMM/YMM) — the biggest win over jit2
- **Validate:** SIMD spec tests pass

**4f. Benchmarks + tuning**
- Add jit_llvm to `bench_compare.cpp`
- Measure: compile time, execution time, memory usage
- LLVM pass pipeline tuning for WASM patterns (small functions, many indirect calls)
- **Validate:** Compute benchmarks show improvement over jit2

**Output:** Full WASM support via LLVM with near-native performance, especially for SIMD.

### Phase 5: WASM 3.0 Proposals — *ongoing, parallelizable*

**Depends on:** Phase 2 (public API). Independent of Phases 3/4.

Each sub-feature is self-contained: parser changes + interpreter support + jit1/jit2 codegen + (later) jit_llvm codegen. Ordered by priority to psiserve and blockchain use cases.

**5a. Reference types + multi-value — *~1 week***
- `externref` / `funcref` as first-class values (locals, globals, table ops)
- Multi-value block/function returns (multiple values on stack)
- Parser: new value types, multi-return function signatures
- All backends: ref-typed locals/globals, multi-value control flow
- **Validate:** Reference types + multi-value spec tests pass

**5b. Exception handling — *~2 weeks***
- `try` / `catch` / `catch_all` / `throw` / `rethrow` / `delegate`
- New control flow in parser: exception tags, try blocks
- Interpreter: exception propagation through call stack
- JIT backends: landing pads, unwind tables or setjmp/longjmp lowering
- LLVM backend (if ready): maps naturally to LLVM's exception handling IR
- **Validate:** Exception handling spec tests pass

**5c. Tail calls — *~3 days***
- `return_call` / `return_call_indirect`
- Interpreter: reuse current frame instead of pushing new one
- JIT backends: emit jump instead of call (reuse stack frame)
- Important for functional languages and recursive WASM patterns
- **Validate:** Tail call spec tests pass

**5d. Relaxed SIMD — *~3 days***
- Non-deterministic SIMD variants (e.g., `relaxed_madd`, `relaxed_swizzle`)
- **Subjective mode only** — these are explicitly non-deterministic by spec
- Objective mode: reject or lower to deterministic equivalents
- Perfect fit for the objective/subjective architecture
- **Validate:** Relaxed SIMD spec tests pass in subjective mode

**5e. Threads + atomics — *~2 weeks***
- `memory.atomic.*` ops, `wait` / `notify`
- Shared linear memory (multiple instances referencing same memory)
- Requires careful interaction with guard page allocator and COW snapshots
- Atomic ops in interpreter: `std::atomic` operations
- JIT backends: emit `lock` prefix instructions (x86) / `ldaxr`/`stlxr` (aarch64)
- **Validate:** Threads spec tests pass

**5f. Component Model — *~3–4 weeks***
- Interface types: canonical ABI for cross-module calls
- Module linking: import/export of module instances
- Resource types and handles
- Critical for blockchain (composable smart contracts) and psiserve (composable services)
- Largest single feature — design doc needed before implementation
- **Validate:** Component model spec tests pass

**5g. Stack switching — *~2 weeks***
- `cont.new` / `resume` / `suspend` / `cont.bind`
- Native coroutine/fiber support inside WASM
- Could complement or replace our external yield mechanism
- Interaction with JIT backends: stack management, register save/restore
- **Validate:** Stack switching spec tests pass

**5h. Other (lower priority)**
- Extended const expressions (~1 day — minor parser change)
- Memory64 (~1 week — 64-bit addresses, >4GB memory)
- Multiple memories (~1 week — more than one linear memory)
- Custom page sizes (~1 day)
- Branch hinting (~1 day — `br.hint` annotations for codegen)

**Output:** Spec-compliant modern WASM engine. Each sub-feature lands independently.

### Phase 6: Cooperative Yield — *~3 days*

**Depends on:** Phase 2 (compile_options), Phase 3 or 4 (at least one compiler updated).

- Add yield flag offset to context struct
- When `compile_options::yield = true`, emit `test byte [rdi + offset], 1; jnz handler` at loop backedges
- All compilers (jit1, jit2, jit_llvm) emit yield checks
- Yield handler mechanism: design TBD (fiber switch, longjmp, host callback)
- Cached `.pzam` files include yield checks — flag recorded in header
- Note: Phase 5g (stack switching) may provide a WASM-native alternative

**Validation:** Execute tight loop with yield enabled, verify handler fires. Cached code with yield still works.
**Output:** Cooperative multitasking support for psiserve fiber scheduler.

### Phase 7: Sandboxed Compilation — *~2–3 weeks*

**Depends on:** Phase 3 (code caching — sandboxed output needs to be cached/signed).

- Compile psizam (parser + ir_writer + jit_codegen) to WASM via wasi-sdk
- Abstract allocator: `mmap` → linear buffer allocator for WASM build
- Strip `mprotect`/signal handler setup (not needed during compilation)
- Export: `compile(wasm_ptr, len, options) → (code_ptr, code_len)`
- Host side: load compiler `.wasm`, feed untrusted module bytes in, extract native code blob out
- Resource limits: WASM linear memory cap (e.g., 512 MB), psizam watchdog on the compiler execution
- Sign extracted code with host-held key, save to `.pzam`

**Validation:** Sandboxed compilation produces **bit-identical** native code to native compilation. Verify with full spec test suite.
**Output:** Safe compilation of untrusted WASM — crash isolation, resource bounding, deterministic output.

### Future / Optional

| Item | Effort | Value |
|------|--------|-------|
| WAMR integration (subjective-only fast interpreter) | ~1 week (medium integration) | Fast interpreter for non-consensus paths; requires maintaining fork |
| LLVM-in-WASM (sandboxed LLVM compilation) | ~2–3 weeks beyond Phase 7 | LLVM optimization with full sandboxing; depends on LLVM compiling to WASM cleanly |
| Debug mode (stack recovery metadata) | ~1 week | Debuggable WASM execution for development |
| GC proposal (structs, arrays, type hierarchies) | ~4+ weeks | Java/Kotlin/Dart targeting — low priority for now |

---

## 11. Code Quality: Correct by Construction

The goal is to make wrong code fail to compile rather than silently produce wrong results. This is especially important when AI generates code — a type error caught at compile time saves hours of debugging a wrong-output-at-runtime bug.

### Strong Index Types

The codebase currently uses raw `uint32_t` for all indices (function, type, local, global, table, memory, label). Mixing these up compiles fine and produces silently wrong results.

```cpp
// Current: nothing stops you from passing a type_index where a func_index is expected
uint32_t fidx = ...;
uint32_t tidx = ...;
mod.get_function_type(tidx);  // BUG: passed type index, wanted function index — compiles fine

// Fixed: distinct types, wrong usage won't compile
struct func_index  { uint32_t value; };
struct type_index  { uint32_t value; };
struct local_index { uint32_t value; };
struct global_index{ uint32_t value; };
struct table_index { uint32_t value; };
struct mem_index   { uint32_t value; };
struct label_index { uint32_t value; };

auto& get_function_type(func_index idx) const;
// mod.get_function_type(tidx);  // COMPILE ERROR: type_index ≠ func_index
```

Zero runtime cost (same representation), but the compiler catches mix-ups. Apply during Phase 2 as part of the type cleanup.

**Implementation:** Use psitri's `ucc::typed_int<T, Tag>` (`external/psitri/libraries/ucc/include/ucc/typed_int.hpp`). psitri is a submodule dependency of psiserve. `typed_int` provides explicit construction, `*val` for unwrapping, full comparison/arithmetic, `std::numeric_limits` specialization, and zero runtime cost. Usage:

```cpp
struct func_index_tag {};
struct type_index_tag {};
using func_index  = ucc::typed_int<uint32_t, func_index_tag>;
using type_index  = ucc::typed_int<uint32_t, type_index_tag>;
// func_index and type_index are incompatible types — can't mix them
```

### Scoped Enums for Value Types

```cpp
// Current: unscoped enum, typedef to uint8_t — can accidentally mix with raw integers
enum types { i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, ... };
typedef uint8_t value_type;  // can hold anything

// Fixed: scoped enum, exhaustive switch required
enum class value_type : uint8_t { i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, v128 = 0x7b, ... };
```

With `-Wswitch-enum`, any `switch` on `value_type` that misses a case produces a compiler warning. This catches "added `externref` to the enum but forgot to handle it in codegen" — a classic AI error.

### IR Validation Pass

Insert a validation pass between IR transformations that checks structural invariants:

```cpp
void validate_ir(const ir_function& func) {
    for (auto& inst : func.insts) {
        // Every use refers to a valid def
        // Every def has correct type
        // Every block ends with a terminator
        // No use-after-def violations
        // Operand types match opcode expectations
    }
}
```

Run after ir_writer (Pass 1), after jit_optimizer, and before codegen. In debug builds, this catches IR corruption immediately at the source rather than manifesting as a wrong-result or crash deep in codegen. Cheap to write (~200 lines), catches entire classes of bugs.

### Exhaustive Opcode Handling

For the ~150 IR opcodes and ~400+ WASM opcodes, use `switch` with no `default` case + `-Wswitch-enum`:

```cpp
// BAD: default silently swallows new opcodes
switch (op) {
    case ir_op::i32_add: ...
    case ir_op::i32_sub: ...
    default: unreachable();  // silently wrong if a new op is added
}

// GOOD: compiler warns when a case is missing
switch (op) {
    case ir_op::i32_add: ...
    case ir_op::i32_sub: ...
    // Adding ir_op::i32_rotl to the enum → compiler warning here
}
```

This is critical for LLVM IR translation — if an opcode is added to `ir_op` but not handled in `llvm_ir_translator.cpp`, the compiler tells you.

### Differential Testing as CI Gate

The interpreter is the validation oracle. Every test should run against **all** backends and compare results:

```cpp
// Test harness pseudocode
for (auto engine : {engine::interpreter, engine::jit1, engine::jit2, engine::jit_llvm}) {
    for (auto sf : {true, false}) {
        auto result = run(wasm_bytes, engine, {.softfloat = sf});
        REQUIRE(result == reference_result);
    }
}
```

This catches "jit_llvm produces 0x8000'0000 where interpreter produces 0x7FFF'FFFF" immediately. Make it a CI gate — no merge if any backend disagrees.

### Span-Based APIs

Replace pointer+length pairs with `std::span`:

```cpp
// Current: easy to get length wrong
void parse(const uint8_t* code, size_t size);

// Fixed: single object, can't mismatch
void parse(std::span<const uint8_t> code);
```

### Const by Default

Mark everything `const` that doesn't need mutation. For IR:
- `ir_function` is mutable during construction (ir_writer), then **frozen** before optimization
- Optimizer takes `const ir_function&` input, returns new `ir_function` output (functional transformation)
- Codegen takes `const ir_function&` — can't accidentally mutate IR during emission

This prevents a class of bugs where codegen accidentally modifies the IR it's reading.

### Compile-Time Option Validation

Use `static_assert` and `requires` to enforce option constraints:

```cpp
auto compile(std::span<const uint8_t> wasm, const host_function_table& table, compile_options opts) {
    if (opts.mode == mode::objective) {
        PSIZAM_ASSERT(opts.softfloat, config_exception,
            "objective mode requires softfloat — cannot produce deterministic results with native float");
    }
    // ...
}
```

### Spec Compliance Audit

Tests prove we handle cases we thought of. A spec audit proves we handle everything the spec requires. For a consensus system, a subtle spec deviation is a consensus-splitting bug.

**Process: Systematic spec-to-code traceability**

1. **Obtain the spec** — The WebAssembly spec (https://webassembly.github.io/spec/) is structured as:
   - §2 Structure (types, instructions, modules)
   - §3 Validation (typing rules for every instruction)
   - §4 Execution (reduction rules — the precise semantics)
   - §5 Binary format (encoding)
   - Appendices for each proposal (SIMD, exceptions, etc.)

2. **Build a compliance matrix** — For each spec item (instruction, validation rule, execution rule), document:

   ```
   | Spec Section | Spec Requirement | Code Location | Status | Notes |
   |--------------|-----------------|---------------|--------|-------|
   | §4.4.1.1 i32.add | wrap result to 2^32 | interpret_visitor.hpp:320 | ✓ | |
   | §4.4.1.2 i32.sub | wrap result to 2^32 | interpret_visitor.hpp:325 | ✓ | |
   | §4.4.3.1 i32.trunc_f32_s | trap if NaN or out of range | interpret_visitor.hpp:795 | ✓ | was psizam_fp bug, fixed Phase 1 |
   | §3.3.1.1 i32.add validation | [i32 i32] → [i32] | parser.hpp:1050 | ✓ | |
   ```

3. **Audit per-backend** — Each backend (interpreter, jit1, jit2, jit_llvm) gets its own audit column. The interpreter is the reference — if it's correct per spec, other backends are validated by differential testing. But new backends (jit_llvm) should be audited independently against the spec too, not just against the interpreter.

4. **Automated cross-implementation differential testing**

   We already have wasmtime, wasmer, wasm3, and WAMR in the benchmark infrastructure. Extend this into a compliance comparison tool:

   ```
   For each test case (spec tests, fuzz-generated, edge cases):
     Run through: spec interpreter, wasmtime, wasmer, WAMR, psizam (all backends)
     Compare results:
       All agree        → ground truth, psizam must match
       psizam disagrees → BUG in psizam
       Industry split   → AMBIGUITY — document, pick side, justify for consensus
       All disagree     → spec underspecified — escalate
   ```

   **Reference implementations (priority order):**
   - **WASM spec interpreter** (OCaml, in the spec repo) — THE canonical answer for what the spec means
   - **Wasmtime** — Bytecode Alliance reference, most spec-rigorous production engine
   - **Wasmer** — second production-grade reference
   - **WAMR** — embedded reference, different implementation approach
   - **V8** (via d8/Node.js) — browser-grade, most battle-tested

   **Automation:** Generate edge-case WASM modules targeting known ambiguities (NaN bit patterns, signed zero, integer overflow wrapping, trap-vs-wrap boundaries, alignment). Run through all engines. CI reports disagreements automatically.

   **For consensus:** When the industry is ambiguous, we must document our choice in the compliance matrix and explain why. This becomes part of the consensus specification — validators must agree on the same interpretation.

5. **AI agent workflow** — Parallel agents for audit:
   - Agent reads a spec section (e.g., §4.4.1 "Numeric Instructions")
   - Agent reads the corresponding implementation code
   - Agent reads the reference implementations' handling of the same section
   - Agent produces a compliance report: conforming, non-conforming, or ambiguous (with cross-engine evidence)
   - Human reviews non-conforming and ambiguous items

6. **Living document** — The compliance matrix lives in `plans/spec-compliance/` (per-section files). Updated as:
   - New WASM 3.0 proposals are implemented (Phase 5)
   - New backends are added (Phase 4)
   - Bugs are found and fixed
   - Cross-engine disagreements are discovered and resolved

**Coverage targets:**

| Area | Items | Priority |
|------|-------|----------|
| §4.4 Numeric instructions (~180 opcodes) | Execution semantics, edge cases (NaN, overflow, div-by-zero) | Critical — consensus |
| §4.4 Memory instructions | Load/store alignment, out-of-bounds trapping | Critical — safety |
| §3.3 Instruction validation | Type checking rules for every opcode | Critical — rejects invalid modules |
| §4.5 Control instructions | Block/loop/br/br_table/call/return semantics | Critical |
| §5.5 Binary format | LEB128, section ordering, limits encoding | High — parser correctness |
| §4.5.5 Table instructions | call_indirect, table.get/set | Medium |
| §2.5 Modules | Import/export resolution, start function | Medium |
| Float edge cases | NaN propagation, signed zero, min/max with NaN | Critical — consensus |

**When to run:**
- After Phase 1 (EOS cleanup) — audit float instructions now that psizam_fp is removed
- After Phase 2 (new API) — full audit as baseline
- After Phase 4 (jit_llvm) — audit new backend against spec independently
- After each Phase 5 sub-feature — audit new proposal implementation

### Fuzzing: Structured WASM Generation + Differential Oracles

Our current fuzzer (`tests/fuzz/fuzz_driver.cpp`) feeds random bytes via libFuzzer. This mostly tests the parser rejection path — random bytes rarely produce valid WASM that exercises the compiler or executor.

**Industry tools to integrate (priority order):**

| Tool | Source | What it does | Integration |
|------|--------|-------------|-------------|
| **wasm-smith** | [bytecodealliance/wasm-tools](https://github.com/bytecodealliance/wasm-tools) | Generates valid, structured WASM modules from seeds | Primary generator — replaces random bytes |
| **wasm-mutate** | Same repo | Semantically mutates existing WASM modules | Expands coverage from seed corpus |
| **Binaryen `--translate-to-fuzz`** | [WebAssembly/binaryen](https://github.com/WebAssembly/binaryen) | Converts arbitrary bytes → valid WASM | Alternative generator, different coverage |
| **wasm-reduce** / **wasm-shrink** | Binaryen / wasm-tools | Minimizes failing test cases | Reduces crash reproductions to minimal WASM |

**Differential fuzzing architecture:**

```
wasm-smith / wasm-mutate → valid WASM module
    │
    ├→ psizam interpreter   ─┐
    ├→ psizam jit1           │
    ├→ psizam jit2           ├→ compare all results
    ├→ psizam jit_llvm       │    any disagreement = BUG
    ├→ wasmtime              │
    ├→ wasmer                │
    └→ spec interpreter     ─┘
```

This combines structured generation (valid WASM that exercises real code paths) with differential oracles (multiple engines as ground truth). A disagreement between any two engines is either a bug or a spec ambiguity — both are valuable findings.

**Fuzzing targets (what to fuzz):**
- **Parse + validate** — malformed WASM (current harness, keep it)
- **Compile** — valid WASM through all compilers (crash = bug)
- **Execute** — valid WASM with host functions, compare results across backends
- **Serialize/deserialize** — `.pzam` round-trip (compile → save → load → execute)
- **Cross-option** — same module compiled with different `compile_options` in objective mode must produce identical execution results

**Infrastructure:** Consider registering with [OSS-Fuzz](https://github.com/google/oss-fuzz) for continuous 24/7 fuzzing (free for open source). Wasmtime runs on this and finds critical bugs regularly.

### Summary: What to Apply When

| Technique | When | Cost | Bugs prevented |
|-----------|------|------|---------------|
| Strong index types | Phase 2 (de-template) | ~1 day | Index mix-ups (func vs type vs local) |
| Scoped enums + `-Wswitch-enum` | Phase 1 (EOS cleanup) | ~1 day | Missing opcode handlers |
| IR validation pass | Phase 2 or Phase 4 | ~1 day | Malformed IR between passes |
| No-default switch | All new code | Free | Silent opcode omissions |
| Differential testing CI | Phase 2 (test harness) | ~2 days | Cross-backend disagreements |
| Spec compliance audit | Phase 1, 2, 4, 5 | ~2-3 days per major section | Spec deviations, consensus bugs |
| Structured fuzzing (wasm-smith) | Phase 2.5 | ~2-3 days setup, then continuous | Compiler/executor crashes, semantic bugs |
| Differential fuzzing | Phase 2.5 | ~2-3 days setup, then continuous | Cross-engine disagreements, edge cases |
| `std::span` APIs | Phase 2 | ~1 day | Buffer length mismatches |
| Const IR after construction | Phase 2 or Phase 4 | ~1 day | Accidental IR mutation |
| Option validation | Phase 2 | ~1 hour | Invalid option combinations |

---

## 12. Risks and Mitigations


| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| LLVM speculates unsafe loads | Low | Don't mark as dereferenceable; Wasmer proves this works |
| Custom ORC memory manager complexity | Medium | Extend SectionMemoryManager; well-documented |
| Decoupling breaks existing tests | Medium | Phase 2 is a pure refactor; run full test suite |
| psizam-in-WASM build complexity | Medium | Start with native LLVM; sandboxing is a later phase |
| LLVM dependency size (~100-200MB) | Certain | Optional; gated behind PSIZAM_ENABLE_LLVM |
| Trampoline ABI bridge overhead | Low | ~5 instructions; negligible vs function body |
| Cache signature verification cost | Low | Ed25519 verify is ~50us; one-time at load |
| WAMR fork maintenance burden | Medium | Medium integration requires maintaining a fork for a subjective-only executor; evaluate if the perf gain over our interpreter justifies the maintenance cost |

---

## 13. Key Files

| File | Role in new architecture |
|------|-------------------------|
| `include/psizam/host_function_table.hpp` | **NEW** — Unified runtime table with templated `add<F>()` |
| `include/psizam/host_function.hpp` | Keep for backwards compat; internally builds table |
| `include/psizam/execution_context.hpp` | Remove Host template; hold `table*` + `void* host` |
| `include/psizam/backend.hpp` | Remove Host template; runtime engine selection |
| `include/psizam/x86_64.hpp` | De-template (remove Context param) |
| `include/psizam/jit_codegen.hpp` | De-template; PIC host stubs |
| `include/psizam/jit_codegen_a64.hpp` | De-template |
| `include/psizam/aarch64.hpp` | De-template |
| `include/psizam/llvm_ir_writer.hpp` | **NEW** — Wraps ir_writer + LLVM Pass 2 |
| `include/psizam/llvm_ir_translator.hpp` | **NEW** — psizam IR → LLVM IR |
| `include/psizam/llvm_jit_compiler.hpp` | **NEW** — ORC JIT + memory manager |
| `src/llvm_ir_translator.cpp` | **NEW** — Translation (~800-1200 lines) |
| `src/llvm_jit_compiler.cpp` | **NEW** — ORC JIT impl (~300-500 lines) |
