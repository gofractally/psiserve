# psizam — High-Performance WebAssembly Engine

psizam is a C++23 WebAssembly engine designed for deterministic, high-performance execution. It provides five execution backends, ahead-of-time compilation to native code via the `.pzam` format, and a zero-boilerplate typed C++ API using reflection.

Originally forked from EOS VM, psizam has been substantially rewritten with a new two-pass JIT, LLVM AOT backend, self-contained compiled module format, and library architecture split for flexible deployment.

## Quick Start

### Building

```bash
# macOS requires Homebrew Clang
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++

# Configure and build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSIZAM_ENABLE_TOOLS=ON
cmake --build build

# With tests
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSIZAM_ENABLE_TESTS=ON
cmake --build build
cd build && ctest -j$(nproc)
```

### Running a WASM Module

```bash
# Compile .wasm to native .pzam
./build/bin/pzam compile --target=aarch64 --backend=jit2 -o module.pzam module.wasm

# Run the compiled module
./build/bin/pzam run module.pzam

# Inspect metadata
./build/bin/pzam inspect module.pzam

# Validate files
./build/bin/pzam validate module.wasm
./build/bin/pzam validate module.pzam
```

### Embedding in C++

#### Step 1: Compile .wasm to .pzam

```bash
pzam compile module.wasm -o module.pzam
```

Or from C++:

```cpp
#include <psizam/pzam_compile.hpp>

int main() {
   psizam::pzam_compile_file("module.wasm", "module.pzam");
}
```

#### Step 2: Load and call from C++

```cpp
#include <psizam/pzam_typed.hpp>

// Declare host functions the WASM module can call
struct my_host {
   uint32_t get_value() { return 42; }
   void log(uint32_t ptr, uint32_t len) { /* ... */ }
};
PSIO_REFLECT(my_host, method(get_value), method(log, ptr, len))

// Declare WASM exports you want to call (must match the module's exports)
struct my_exports {
   uint32_t add(uint32_t a, uint32_t b);
   void     on_request(uint32_t path_ptr, uint32_t path_len);
};
PSIO_REFLECT(my_exports, method(add, a, b), method(on_request, path_ptr, path_len))

int main() {
   my_host host;
   auto instance = psizam::pzam_load_file<my_host, my_exports>("module.pzam", host);

   // Call WASM exports like native C++ methods
   uint32_t result = instance.exports().add(1, 2);
   instance.exports().on_request(ptr, len);
}
```

#### Advanced: JIT from .wasm source

For development workflows where you want to skip the compile step, or when you need control over the allocator and engine:

```cpp
#include <psizam/backend.hpp>
#include <psizam/pzam_typed.hpp>

int main() {
   my_host host;
   auto code  = psizam::read_wasm("module.wasm");
   auto table = psizam::make_host_table<my_host>("env");

   psizam::wasm_allocator alloc;
   psizam::backend<std::nullptr_t, psizam::jit> be(code, table, &host, &alloc);
   be.initialize();
   be.call(&host, "_start");
}
```

## Architecture

### Execution Backends

| Backend | Platforms | Description |
|---------|-----------|-------------|
| `interpreter` | All | Pure interpreted execution, portable across all platforms |
| `jit` | x86_64, aarch64 | Single-pass JIT, fastest compilation |
| `jit2` | x86_64, aarch64 | Two-pass JIT with IR optimization and register allocation |
| `jit_llvm` | x86_64, aarch64 | LLVM-based compilation (AOT and JIT), highest execution speed |
| `null_backend` | All | Validation only, no code generation |

### Library Targets

psizam is split into focused CMake targets:

| Target | Type | Description |
|--------|------|-------------|
| `psizam` | INTERFACE | Core header-only library (types, allocators, host functions) |
| `psizam-exec` | STATIC | Runtime execution (native only). Links runtime helpers + LLVM runtime symbols |
| `psizam-jit1-x86` | INTERFACE | JIT1 compilation targeting x86_64 |
| `psizam-jit1-arm` | INTERFACE | JIT1 compilation targeting aarch64 |
| `psizam-jit2-x86` | INTERFACE | JIT2 compilation targeting x86_64 |
| `psizam-jit2-arm` | INTERFACE | JIT2 compilation targeting aarch64 |
| `psizam-llvm-x86` | STATIC | LLVM AOT compilation targeting x86_64 |
| `psizam-llvm-arm` | STATIC | LLVM AOT compilation targeting aarch64 |
| `psizam-llvm-jit` | STATIC | LLVM ORC JIT runtime (native live-JIT) |
| `psizam-llvm` | INTERFACE | Umbrella linking all enabled LLVM targets |

Consumers link what they need. A compile-only WASI binary links `psizam` + a compile target.
An execution-only loader links `psizam` + `psizam-exec`. A full tool links both.

## .pzam File Format (v3)

A `.pzam` file is a self-contained compiled WebAssembly module. It includes all module metadata (types, imports, exports, data segments, element segments, globals) and one or more compiled code sections. No `.wasm` file is needed at runtime.

### Structure

```
pzam_file
  magic: 0x4d415a50 ("PZAM")
  format_version: 3
  input_hash: SHA-256 of original .wasm
  metadata: pzam_module_metadata
    types[]          — function signatures
    imports[]        — imported functions, tables, memories, globals
    functions[]      — type index per local function
    tables[]         — table types and limits
    memories[]       — memory limits
    globals[]        — global types and init expressions
    exports[]        — exported items
    elements[]       — element segments (for indirect calls)
    data[]           — data segments
    tags[]           — exception tags
    start_function   — start function index (or UINT32_MAX)
    features_required — WASM features used (SIMD, bulk memory, etc.)
  code_sections[]: pzam_code_section
    arch             — target architecture (x86_64 or aarch64)
    opt_tier         — optimization level (jit1, jit2, llvm_O1-O3)
    instrumentation  — softfloat, gas metering, backtrace, etc.
    compiler         — compiler name, version, identity hash
    attestations[]   — signatures vouching for correctness
    functions[]      — per-function offset, size, stack depth
    relocations[]    — position-independent code fixups
    code_blob        — native machine code
```

Serialized with [fracpack](../psio/) for zero-copy access via `psio::view<pzam_file>`.

### Multi-Architecture Support

A single `.pzam` file can contain multiple code sections for different architectures:

```bash
# Compile for both architectures
pzam compile module.wasm --target=x86_64 -o module.pzam
pzam compile module.wasm --target=aarch64 --append module.pzam   # (future)
```

At load time, the loader picks the code section matching the host architecture.

## Host Function Registration

### Reflection-Based (Recommended)

With `PSIO_REFLECT`, a single call registers all methods:

```cpp
struct my_host {
   uint32_t get_value() { return 42; }
   void log(uint32_t ptr, uint32_t len) { /* ... */ }
};
PSIO_REFLECT(my_host, method(get_value), method(log, ptr, len))

auto table = psizam::make_host_table<my_host>("env");
```

### Manual Registration

For cases where you need per-function control (custom module names, selective registration):

```cpp
psizam::host_function_table table;
table.add<&my_host::get_value>("env", "get_value");
table.add<&my_host::log>("env", "log");
```

The registration system uses template metaprogramming to generate optimized trampolines. For functions with only WASM-primitive parameters (i32, i64, f32, f64), it generates a "fast trampoline" that avoids the type conversion pipeline entirely.

### Custom Type Converters

For complex parameter types (pointers, spans, strings), implement a custom `type_converter`:

```cpp
template<>
struct psizam::type_converter<my_host> : psizam::type_converter<> {
   using type_converter<>::type_converter;

   // Convert WASM i32 offset to host pointer
   char* from_wasm(psizam::wasm_ptr_t ptr) {
      return get_interface().linear_memory() + ptr.offset;
   }
};
```

## Attestation and Trust Policy

Code sections in `.pzam` files can carry cryptographic attestations — signatures from trusted parties (compilers, validators, auditors) vouching for the code's correctness.

### Signing

```cpp
#include <psizam/pzam_attestation.hpp>

// Implement pzam_signer with your crypto library
struct my_signer : psizam::pzam_signer {
   std::array<uint8_t, 32> pubkey_hash() const override { /* ... */ }
   std::vector<uint8_t> sign(std::span<const uint8_t, 32> digest) const override { /* ... */ }
};

// Sign a code section
my_signer signer(private_key);
auto attestation = psizam::pzam_sign(code_section, signer);
code_section.attestations.push_back(attestation);
```

### Verification and Trust Policy

```cpp
// Implement pzam_verifier with your crypto library
struct my_verifier : psizam::pzam_verifier {
   bool verify(std::span<const uint8_t, 32> digest,
               std::span<const uint8_t, 32> pubkey_hash,
               std::span<const uint8_t> signature) const override { /* ... */ }
};

// Create a trust policy
auto policy = psizam::pzam_trust_policy::require_trusted(2);
policy.add_trusted_key(compiler_pubkey_hash);
policy.add_trusted_key(auditor_pubkey_hash);

// Check before loading
my_verifier verifier;
if (!policy.check(code_section, verifier)) {
   // Reject: insufficient attestations
}
```

Trust policy modes:

| Mode | Description |
|------|-------------|
| `trust_all` | Accept any code (development/testing) |
| `require_any(N)` | At least N valid signatures from any signer |
| `require_trusted(N)` | At least N valid signatures from trusted keys |
| `custom(fn)` | User-provided predicate function |

## CLI Reference

### `pzam compile`

Compile a `.wasm` file to a native `.pzam` module.

```
pzam compile <input.wasm> [options]

Options:
  --target=x86_64|aarch64    Target architecture (required)
  --backend=jit2|llvm        Compilation backend (default: jit2)
  --softfloat                Use deterministic software floating point (requires --backend=llvm)
  --backtrace                Enable async backtrace support
  -o <output.pzam>           Output file (default: input with .pzam extension)
```

### `pzam run`

Execute a pre-compiled `.pzam` module with WASI support.

```
pzam run <module.pzam> [options] [-- args...]

Options:
  --dir=guest:host           Pre-open a directory (can be repeated)
```

### `pzam inspect`

Display metadata and code section details from a `.pzam` file.

```
pzam inspect <module.pzam>
```

Shows: format version, input hash, types, imports, exports, code section details (architecture, optimization tier, compiler info, function count, code size, attestations).

### `pzam validate`

Validate a `.wasm` or `.pzam` file for structural correctness.

```
pzam validate <module.wasm|module.pzam>
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `PSIZAM_ENABLE_SOFTFLOAT` | ON | Deterministic IEEE-754 via software floating point |
| `PSIZAM_ENABLE_TOOLS` | ON* | Build CLI tools (pzam, psizam-interp, etc.) |
| `PSIZAM_ENABLE_TESTS` | OFF | Build unit tests and spec tests |
| `PSIZAM_ENABLE_SPEC_TESTS` | ON** | WebAssembly spec compliance tests |
| `PSIZAM_ENABLE_FUZZ_TESTS` | OFF | Fuzz testing harness |
| `PSIZAM_ENABLE_BENCHMARKS` | OFF | Comparative benchmark suite |
| `PSIZAM_ENABLE_LLVM` | OFF | LLVM AOT/JIT backend |
| `PSIZAM_LLVM_TARGETS` | X86;AArch64 | LLVM target architectures |
| `PSIZAM_FULL_DEBUG` | OFF | Stack dumps and instruction tracing |

\* When built standalone. OFF when used as a subdirectory.
\** Requires `PSIZAM_ENABLE_TESTS`.

## Memory Safety

psizam uses guard-page-based memory isolation rather than per-access bounds checks:

- Linear memory is bounded by OS-level guard pages
- Out-of-bounds access triggers a hardware fault caught by the signal handler
- JIT-compiled code runs with zero per-access overhead
- The `wasm_allocator` manages page-aligned memory regions with guard pages

## Deterministic Execution

For consensus and blockchain applications, psizam provides deterministic execution guarantees:

- **Software floating point**: When `PSIZAM_ENABLE_SOFTFLOAT` is ON, all floating-point operations use Berkeley SoftFloat-3, producing bit-identical results across platforms
- **WASM sandbox compilation**: The compiler itself can run inside a WASM sandbox (via the WASI build of `pzam-compile`), ensuring deterministic compilation output
- **Attestation**: Validators can independently recompile and verify that a `.pzam` file matches the expected output

## Dependencies

- **C++23 compiler** (Clang recommended, GCC may work)
- **psio** — Reflection and fracpack serialization (included in psiserve)
- **softfloat** — Berkeley SoftFloat-3 (vendored in `external/softfloat/`)
- **Catch2 v2** — Test framework (vendored in `external/Catch2/`)
- **LLVM** (optional) — For LLVM AOT/JIT backend
