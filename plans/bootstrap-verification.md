# Bootstrap Verification Matrix

Cross-compilation verification plan for pzam-compile self-hosting. Every cell must pass before we can claim the bootstrapping loop is sound.

## Compiler Forms

| ID | Description | Backends Available |
|----|-------------|-------------------|
| **N** | Native `pzam-compile` binary | JIT, LLVM |
| **P-L** | `pzam-run` + `compiler-llvm.pzam` (LLVM-compiled pzam-compile.wasm) | JIT only |
| **P-J** | `pzam-run` + `compiler-jit.pzam` (JIT-compiled pzam-compile.wasm) | JIT only |
| **W** | `wasmtime` running `pzam-compile.wasm` | JIT only |

P-L, P-J, and W all execute the same WASM module — only the execution method differs.

## Stage 0: Bootstrap — Native Compiles the Compiler

Compile `pzam-compile.wasm` to native `.pzam` using the native compiler.

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| S0-L | `N --backend=llvm --target=aarch64 pzam-compile.wasm → compiler-llvm.pzam` | 🟢 | 48MB, 4.5min |
| S0-J | `N --backend=jit --target=aarch64 pzam-compile.wasm → compiler-jit.pzam` | 🔴 | "branch out of range" — JIT codegen limit on large functions |
| S0-Lx | `N --backend=llvm --target=x86_64 pzam-compile.wasm → compiler-llvm-x64.pzam` | 🟡 | Not yet tested |
| S0-Jx | `N --backend=jit --target=x86_64 pzam-compile.wasm → compiler-jit-x64.pzam` | 🟡 | Not yet tested (jit2 x86_64) |

## Stage 1: Sandboxed Compiler — Compile Test Programs

Run the compiled compiler to compile simple test programs (`fib_simple.wasm`).

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| 1a | `P-L --backend=jit --target=aarch64 fib.wasm → fib-PL.pzam` | 🔴 | "undefined element" — call_indirect bug |
| 1b | `P-J --backend=jit --target=aarch64 fib.wasm → fib-PJ.pzam` | 🔴 | Blocked by S0-J |
| 1c | `W --backend=jit --target=aarch64 fib.wasm → fib-W.pzam` | 🟡 | Not yet tested |
| 1d | `N --backend=jit --target=aarch64 fib.wasm → fib-N.pzam` | 🔴 | "branch out of range" on aarch64 JIT |

### Expected equalities
- `fib-PL.pzam == fib-PJ.pzam == fib-W.pzam == fib-N.pzam` (same backend, same input → identical output)

## Stage 2: Self-Compilation — Compiled Compiler Compiles Itself

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| 2a | `P-L --backend=jit pzam-compile.wasm → compiler-s2a.pzam` | 🔴 | Blocked by 1a |
| 2b | `P-J --backend=jit pzam-compile.wasm → compiler-s2b.pzam` | 🔴 | Blocked by S0-J |
| 2c | `W --backend=jit pzam-compile.wasm → compiler-s2c.pzam` | 🟡 | Not yet tested |

### Expected equalities
- `compiler-s2a.pzam == compiler-s2b.pzam == compiler-s2c.pzam == compiler from S0-J` (same WASM, same backend)

## Stage 3: Fixed Point — Stage-2 Compiler Compiles Itself

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| 3a | `pzam-run compiler-s2a.pzam pzam-compile.wasm --backend=jit pzam-compile.wasm → compiler-s3a.pzam` | 🔴 | Blocked by 2a |

### Expected equality (fixed point)
- `compiler-s3a.pzam == compiler-s2a.pzam` — self-compilation is a fixed point

## Stage 4: Output Execution — Run Programs Compiled by Sandboxed Compiler

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| 4a | `pzam-run fib.wasm fib-PL.pzam` → outputs 6765, exit 0 | 🔴 | Blocked by 1a |
| 4b | `pzam-run fib.wasm fib-W.pzam` → outputs 6765, exit 0 | 🟡 | Blocked by 1c |
| 4c | `pzam-run fib.wasm fib-N-llvm.pzam` → outputs 6765, exit 0 | 🟢 | Native LLVM compile + pzam-run works |

## Future: LLVM-in-WASM (after cross-compiling LLVM to WASI)

| Test | Command | Status | Notes |
|------|---------|--------|-------|
| F0 | `N --backend=llvm test.wasm → test-N-llvm.pzam` | 🟢 | Works today |
| F1a | `P-L --backend=llvm test.wasm → test-PL-llvm.pzam` | ⬜ | Requires LLVM-in-WASM |
| F1b | `W --backend=llvm test.wasm → test-W-llvm.pzam` | ⬜ | Requires LLVM-in-WASM |

### Expected equality (the ultimate goal)
- `test-N-llvm.pzam == test-PL-llvm.pzam == test-W-llvm.pzam` — bit-identical deterministic compilation

## Blockers

| Issue | Affects | Fix |
|-------|---------|-----|
| call_indirect "undefined element" in pzam-run for large modules | 1a, 2a, 3a, 4a | 🟢 Fixed — stale %mem pointer after calls; reload via __psizam_get_memory |
| JIT "branch out of range" for large functions on aarch64 | S0-J, 1b, 1d | Need long-branch support in JIT codegen |
| LLVM `select` type mismatch | S0-L (was blocking) | 🟢 Fixed — promote operands to matching types |
| Allocator 16GB bump limit for large modules | S0-L (was blocking) | 🟢 Fixed — increased to 16GB (should reset per-function) |
| Veneer alignment on aarch64 | S0-L, 4a (was blocking) | 🟢 Fixed — align veneer_start to 4 bytes |
| Unresolved BL for libc/compiler-rt symbols | S0-L, 4a (was blocking) | 🟢 Fixed — map memset/memmove/memcpy to reloc_symbols |
| ARM64 I-cache not invalidated after code load | S0-L (was blocking) | 🟢 Fixed — __builtin___clear_cache after mprotect |
| LLVM-in-WASM not built | F1a, F1b | Implement cross-compilation plan |

## Legend
- 🟢 Passing
- 🔴 Failing / blocked
- 🟡 Not yet tested
- ⬜ Not yet implemented
