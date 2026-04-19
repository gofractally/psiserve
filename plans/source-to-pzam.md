# source-to-pzam: Deterministic C++ → .pzam Toolchain

## Goal

A reproducible build target that produces three artifacts which, together with psizam, turn C++ source into an executable `.pzam` — with both compilation stages running *inside* psizam so the output is a deterministic function of inputs.

```
hello.cc ──[clang-wasi.wasm, in psizam]──► hello.wasm ──[pzam-compile.wasm, in psizam]──► hello.pzam ──[pzam-run]──► "hello world"
```

**Artifacts:**

| Artifact | Role | Status |
|---|---|---|
| `clang-wasi.wasm` (+ embedded `wasm-ld`) | C++ → `.wasm` | NEW |
| `wasi-sysroot.tar` | headers/libs clang needs as *input* target | NEW (pinned, content-addressed) |
| `pzam-compile.wasm` | `.wasm` → `.pzam` | EXISTS (`plans/wasi-llvm-build-guide.md`) |
| `source-to-pzam` (host CLI) | orchestrates the two stages under psizam | NEW |

**Final user commands (what "done" looks like):**

```bash
# One-time toolchain build (produces the three artifacts above)
scripts/build-source-to-pzam.sh

# Compile a C++ program to .pzam (both stages run inside psizam)
cat > hello.cc <<'EOF'
#include <cstdio>
int main() { printf("hello world\n"); return 0; }
EOF

build/Release/bin/source-to-pzam hello.cc -o hello.pzam

# Execute the .pzam — prints "hello world"
build/Release/bin/pzam-run hello.pzam
# hello world
```

---

## Phasing

Each phase has a **completion signal** — a concrete, testable check that tells us to stop and move on. Phases are ordered so each one de-risks the next.

### Phase 0 — Bootstrap Validation (½–1 day)

**Purpose:** Prove the *tail* of the pipeline works today, before we build the head. If we can't run a native-wasi-sdk-compiled `hello.wasm` through `pzam-compile` and then `pzam-run` to get "hello world", nothing downstream matters.

**Tasks:**
1. Install wasi-sdk (or use Homebrew `wasi-runtimes` + `llvm`) as a *host-side* C++ compiler.
2. Compile `hello.cc` with it to `hello.wasm`.
3. Compile `hello.wasm` to `hello.pzam` using existing `pzam-compile` (native host build).
4. Run `pzam-run hello.pzam` and confirm stdout is `hello world`.
5. If it fails: fix `libraries/psizam/tools/pzam_run.cpp` + `libraries/psizam/include/psizam/detail/wasi_host.hpp` until it works.

**Completion signal:** `pzam-run hello.pzam` prints `hello world` on both macOS and Linux.

**Files touched (if any):** `libraries/psizam/include/psizam/detail/wasi_host.hpp`, `libraries/psizam/tools/pzam_run.cpp`.

---

### Phase 1 — WASI Surface Audit for Clang (1–3 days)

**Purpose:** `pzam-compile.wasm` exercises a narrow WASI surface (args/environ/`fd_read`/`fd_write`/`proc_exit`). Clang walks directories, reads many small files, and uses clocks. Find the gaps before the long LLVM build so Phase 2 doesn't stall on WASI fixes.

**Tasks:**
1. Enumerate WASI imports from a native wasi-sdk clang: `wasm-objdump -x $(brew --prefix llvm)/bin/clang-22 | grep "import"` (or `wasm-tools dump`). Record the list.
2. Diff against what `libraries/psizam/include/psizam/detail/wasi_host.hpp` + `libraries/wasi/cpp/` implement.
3. Likely gaps (budget them now): `fd_readdir`, `path_filestat_get`, `path_readlink`, `path_open` flag coverage (O_DIRECTORY, O_TRUNC), `clock_time_get` monotonic/realtime, `fd_fdstat_get`, `fd_prestat_get`, `fd_prestat_dir_name`, `random_get`.
4. Implement missing functions against the host filesystem (through `--dir=guest:host` preopens already handled by `pzam-run`).
5. Integration test: run a small native-wasi-sdk test program that stress-tests each added syscall under `pzam-run`.

**Completion signal:** Running a stock wasi-sdk clang (as a `.wasm`) under `pzam-run --dir=/:/` compiling `int main(){}` succeeds and produces a valid `.wasm`. This is the dress rehearsal for Phase 2's output — if it works here, our own clang-wasi.wasm will work too.

**Files touched:** `libraries/psizam/include/psizam/detail/wasi_host.hpp`, `libraries/wasi/cpp/*`, new integration tests under `libraries/psizam/tests/`.

---

### Phase 2 — Build `clang-wasi.wasm` + `wasm-ld.wasm` (~1 week wall-clock, mostly waiting on LLVM)

**Purpose:** Produce the C++ → `.wasm` front end as WASM executables.

**Tasks:**

**2a. Extend `cmake/llvm-wasi/CMakeLists.txt`:**
- Add an option `LLVM_WASI_BUILD_CLANG=OFF` (default off so existing AOT-library build path is unaffected).
- When set, add a second `ExternalProject_Add(clang-wasi ...)` (or extend the existing one) with:
  ```cmake
  -DLLVM_ENABLE_PROJECTS=clang;lld
  -DLLVM_BUILD_TOOLS=ON
  -DCLANG_BUILD_TOOLS=ON
  -DCLANG_ENABLE_STATIC_ANALYZER=OFF
  -DCLANG_ENABLE_ARCMT=OFF
  -DCLANG_PLUGIN_SUPPORT=OFF
  -DLLVM_TARGETS_TO_BUILD=WebAssembly
  ```
  (note: `LLVM_TARGETS_TO_BUILD=WebAssembly` for the *output* target — we're building a C++-to-WASM compiler, not a cross-compiler to x86/arm)
- Build targets: `clang`, `wasm-ld`.

**2b. Single-binary packaging:** clang + lld as two separate `.wasm`s is simpler; merging into one (clang-driver dispatches based on argv[0]) is a later optimization. Ship two files.

**2c. Size pruning:** Follow the same approach as `plans/wasi-llvm-build-guide.md` (manual analysis registration, no tools we don't call). Budget: clang-wasi.wasm ~60–100 MB, wasm-ld.wasm ~15–25 MB.

**2d. Smoke test:** Under `wasmtime` first (bypasses our engine):
```bash
wasmtime run --dir=. -- build/wasi-*/bin/clang-wasi.wasm \
    --target=wasm32-wasip1 --sysroot=./wasi-sysroot -c hello.cc -o hello.o
wasmtime run --dir=. -- build/wasi-*/bin/wasm-ld.wasm hello.o -o hello.wasm -L./wasi-sysroot/lib/wasm32-wasip1 -lc
```
Then under `pzam-run` with LLVM AOT backend (not JIT2 — `TODO.md` notes the 5.7 MB-offset crash on complex modules).

**Completion signal:** `pzam-run clang-wasi.pzam -- -c hello.cc -o hello.o` produces an object file identical to native wasi-sdk clang's output (content-hash compared).

**Files touched:** `cmake/llvm-wasi/CMakeLists.txt`, `scripts/build-llvm-wasi.sh` (add `--with-clang` flag).

---

### Phase 3 — Pinned, Reproducible WASI Sysroot (2–3 days)

**Purpose:** Today `cmake/wasi-toolchain.cmake` reads from Homebrew `wasi-runtimes` — a moving target. For reproducibility, the sysroot must be pinned and content-addressed.

**Tasks:**
1. New `cmake/wasi-sysroot/CMakeLists.txt`:
   - `ExternalProject_Add` for pinned wasi-libc (git SHA).
   - `ExternalProject_Add` for pinned LLVM libc++/libcxxabi built against that wasi-libc (same LLVM 22.1.2 tree as `cmake/llvm-wasi/`).
   - Install layout matching the standard wasi-sysroot tree: `include/`, `lib/wasm32-wasip1/`.
2. `scripts/package-wasi-sysroot.sh`: emits `wasi-sysroot.tar` using `tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner`.
3. Record SHA-256 in `cmake/wasi-sysroot/sysroot.sha256`; CI enforces.
4. Replace the Homebrew path in `cmake/wasi-toolchain.cmake` with a lookup of `${WASI_SYSROOT_TAR}` or a pointed-to extracted directory.

**Completion signal:** `sha256sum wasi-sysroot.tar` is byte-identical across macOS+Linux CI runners.

**Files touched:** `cmake/wasi-sysroot/` (new), `cmake/wasi-toolchain.cmake`, `scripts/package-wasi-sysroot.sh` (new).

---

### Phase 4 — `source-to-pzam` Driver (2 days)

**Purpose:** The single command the user runs. Hides the two-stage pipeline, runs both stages under psizam, writes the `.pzam` + a manifest.

**Tasks:**
1. New `programs/source-to-pzam/source_to_pzam.cpp`:
   - Accepts: input `.cc` files, `-o out.pzam`, `-O<N>`, `--target=wasm32-wasip1` (default), optional `--backend=llvm|jit2` for the `.wasm→.pzam` stage.
   - Stage A: loads `clang-wasi.pzam` into a psizam instance, preopens temp working dir, invokes with `argv` = `["clang-wasi", "--target=wasm32-wasip1", "--sysroot=...", hello.cc, "-o", "/tmp/stage/hello.wasm"]`.
   - Stage A': loads `wasm-ld.pzam`, invokes similarly to link to `hello.wasm`.
   - Stage B: loads `pzam-compile.pzam`, invokes with `["pzam-compile", "--backend=llvm", "--target=x86_64", hello.wasm, "-o", out]`.
   - Writes a `.manifest.json` next to the output containing SHA-256 of each stage tool, sysroot, input, each intermediate, and final output.
2. CMake target `source-to-pzam` links `psizam-llvm` for Stage B path; embeds tool paths via configure-time substitution (`-DSOURCE_TO_PZAM_TOOLDIR`).
3. Integration test `libraries/psizam/tests/source_to_pzam_hello_world.cpp`:
   - Compiles the hello.cc string, executes the resulting .pzam, captures stdout, asserts `"hello world\n"`.

**Completion signal:** `ctest -R source_to_pzam_hello_world` passes.

**Files touched:** `programs/source-to-pzam/` (new), `CMakeLists.txt` (top-level), new test.

---

### Phase 5 — Reproducibility CI + Hardening (1 day + ongoing)

**Purpose:** Prove and keep proving the build is reproducible.

**Tasks:**
1. CI matrix: macOS-14, ubuntu-22.04. Build all three artifacts on each. Diff SHA-256 — fail on mismatch.
2. Deterministic build flag set (already partial; consolidate in `cmake/wasi-toolchain.cmake`):
   - `SOURCE_DATE_EPOCH=0` exported in build scripts.
   - `-ffile-prefix-map=${CMAKE_SOURCE_DIR}=`, `-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=`.
   - `-fno-ident`.
   - `-Wl,--no-timestamp` on wasm-ld invocations (already respects `SOURCE_DATE_EPOCH`, belt-and-suspenders).
3. Pin host `llvm-tblgen`: in CI, always build from source (don't trust system tblgen). `scripts/build-llvm-wasi.sh` already supports this via `LLVM_HOST_TBLGEN` unset.
4. Manifest verification in `source-to-pzam`: on `--verify`, recomputes hashes and errors on mismatch.

**Completion signal:** Two clean CI runs on different runners produce byte-identical `clang-wasi.wasm`, `wasm-ld.wasm`, `wasi-sysroot.tar`, and (for a fixed input) byte-identical `hello.pzam`.

**Files touched:** `.github/workflows/reproducibility.yml` (new), `cmake/wasi-toolchain.cmake`, `programs/source-to-pzam/`.

---

## Known Sharp Edges (to track, not block)

| Risk | Mitigation |
|---|---|
| JIT2 crash on complex modules at ~5.7 MB offset (`TODO.md`) | Stage A runs under LLVM AOT backend only, not JIT2, until fix lands. |
| Clang threading | `LLVM_ENABLE_THREADS=OFF` — already set in `cmake/llvm-wasi/CMakeLists.txt`. |
| Homebrew `wasi-runtimes` is a hidden unpinned input today | Replaced in Phase 3. |
| `LLVM softfloat integration` open (Phase 4c in `plans/llvm-backend-design.md`) | Orthogonal to this plan — toolchain is deterministic regardless; consensus-grade `.pzam` output is a separate gate. |
| clang-wasi.wasm size (60–100 MB) | Acceptable; same pruning playbook as `plans/wasi-llvm-build-guide.md`. |
| Cache keying | Manifest hashes in Phase 4 make incremental rebuilds content-addressable — add a build cache later if wall-clock hurts. |

---

## Effort Summary

| Phase | Wall-clock | Active work |
|---|---|---|
| 0. Bootstrap validation | ½–1 day | all active |
| 1. WASI surface audit | 1–3 days | all active |
| 2. Build clang-wasi.wasm | ~1 week | ~2 days active, rest LLVM build |
| 3. Sysroot pinning | 2–3 days | all active |
| 4. source-to-pzam driver | 2 days | all active |
| 5. Reproducibility CI | 1 day + ongoing | 1 day setup |
| **Total** | **~2.5–3 weeks** | **~8–10 active days** |

---

## Working Protocol Across Sessions

This plan persists across sessions. Each session:

1. Read this file first.
2. Identify the current phase from "Completion signal" state.
3. Work one phase at a time; don't skip forward.
4. When a phase's completion signal is met, update `TODO.md` and mark the phase **Done** inline in this file with the commit SHA.
5. If a phase uncovers a new blocker, add it to "Known Sharp Edges" rather than stalling.

**Phase status (update as we go):**

- [x] Phase 0 — Bootstrap validation *(verified: native wasi-sdk hello.cc → hello.wasm → hello.pzam (aarch64/jit2) → "hello world")*
- [x] Phase 1 — WASI surface audit *(psizam implements all 42 WASI P1 non-socket/non-threads functions; dress rehearsal fs_stress.wasm passed under pzam-run)*
- [x] Phase 2 — clang-wasi.wasm + wasm-ld.wasm *(both cross-compiled to wasm32-wasip1 via cmake/llvm-wasi/; end-to-end verified under psizam-wasi JIT runner)*
- [ ] Phase 3 — Pinned wasi-sysroot
- [x] Phase 4 — source-to-pzam driver *(driver compiles hello.cc → hello.pzam using clang.wasm + wasm-ld.wasm under psizam-wasi, then pzam-compile; pzam-run hello.pzam prints "hello world"; multi-sysroot preopen support via --cxxsysroot/--resource-dir/--clang-rt-dir)*
- [ ] Phase 5 — Reproducibility CI

**Deviations / notes:**
- Stage A/A' use the **JIT** runner (`psizam-wasi`) rather than **AOT** (`pzam-run clang.pzam`) because pzam-compile's jit2 backend currently fails AOT codegen on LLVM-heavy modules with a duplicate `cl::opt` registration error. Tracked separately; source-to-pzam defaults to `--runner=psizam-wasi`.
- pzam-compile output is bit-reproducible for simple programs (e.g. hello world) as of `canonicalize_reloc_sites` in `jit_reloc.hpp`: every recorded relocation site is zeroed before serialization, so ASLR-dependent compile-time pointer values stop leaking into the code blob. Verified by a second `cmp` step in the hello-world CTest.
- Modules that exercise jit2 codegen paths for softfloat helpers, trapping float→int conversions, SIMD softfloat, and EH runtime calls are still non-reproducible. Those paths call `emit_mov_imm64` directly with runtime function pointers rather than going through `emit_reloc_mov_imm64`, so the reloc recorder never sees them and canonicalization has nothing to zero. Fix: thread the appropriate `reloc_symbol` through `emit_sf_unop/binop/cmp`, the trap-convert helper, `simd_v128_*_softfloat`, and `emit_eh_runtime_call_a64` (adding new symbols for `__psizam_setjmp`, `__psizam_eh_throw`, `__psizam_eh_enter/leave`, `__psizam_eh_get_match/payload/exnref`, `__psizam_eh_throw_ref`). Same class of bug exists in `x86_64.hpp` EH paths (`emit_mov(&__psizam_eh_*, rax)`). Only surfaces as a reproducibility gap today; it would also block loading these code paths under different ASLR if they were actually reached at runtime.
