#!/bin/bash
# =============================================================================
# Cross-Compile / Host / Self-Compile Test Matrix
# =============================================================================
# Tests every combination of:
#   Compiler:  native (arm64), wasmtime (WASI), wasmer (WASI)
#   Backend:   jit2, llvm
#   Target:    x86_64, aarch64
#   Input:     fib_simple.wasm (14 funcs), eosio.system.wasm (208 funcs),
#              pzam-compile-nollvm.wasm (843 funcs)
#
# Then: execution, self-compilation, and determinism tests.
# =============================================================================

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$ROOT/build/test-matrix-results"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"/{pzam,logs}

# ── Tool paths ──
NATIVE_COMPILE="$ROOT/build/Debug/bin/pzam-compile"
NATIVE_RUN="$ROOT/build/Debug/bin/pzam-run"
WASI_COMPILE_JIT2="$ROOT/build/wasi-nollvm/bin/pzam-compile"
WASI_COMPILE_LLVM="$ROOT/build/wasi/bin/pzam-compile"
WASMTIME="$(command -v wasmtime)"
WASMER="$(command -v wasmer)"

# ── Test inputs (absolute paths) ──
WASM_FIB="$ROOT/fib_simple.wasm"
WASM_EOSIO="$ROOT/libraries/psizam/eosio.system.wasm"
WASM_PZAM_NOLLVM="$ROOT/build/wasi-nollvm/bin/pzam-compile"

# ── Report ──
CSV="$RESULTS_DIR/results.csv"
echo "test_id,input,compiler,backend,target,status,time_s,output_size,error" > "$CSV"

PASS=0; FAIL=0; SKIP=0; TOTAL=0

numfmt_size() {
   local b=$1
   if [ "$b" -eq 0 ]; then echo "—"; return; fi
   if [ "$b" -gt $((1024*1024)) ]; then echo "$((b/1024/1024))MB"
   elif [ "$b" -gt 1024 ]; then echo "$((b/1024))KB"
   else echo "${b}B"; fi
}

run_compile() {
   local tid="$1" label="$2" compiler="$3" backend="$4" target="$5"
   shift 5
   local cmd="$*"
   local out="$RESULTS_DIR/pzam/${tid}.pzam"
   local logfile="$RESULTS_DIR/logs/${tid}.log"
   TOTAL=$((TOTAL + 1))
   printf "  %-55s " "$tid"
   local t0 t1 elapsed
   t0=$(python3 -c 'import time; print(time.time())')
   if eval "$cmd" > "$logfile" 2>&1; then
      t1=$(python3 -c 'import time; print(time.time())')
      elapsed=$(python3 -c "print(f'{$t1-$t0:.2f}')")
      local sz=$(stat -f%z "$out" 2>/dev/null || echo 0)
      printf "✅ %7ss  %s\n" "$elapsed" "$(numfmt_size $sz)"
      PASS=$((PASS + 1))
      echo "${tid},${label},${compiler},${backend},${target},PASS,${elapsed},${sz}," >> "$CSV"
   else
      t1=$(python3 -c 'import time; print(time.time())')
      elapsed=$(python3 -c "print(f'{$t1-$t0:.2f}')")
      local err=$(tail -1 "$logfile" | tr ',' ';' | head -c 200)
      printf "❌ %7ss  %s\n" "$elapsed" "$err"
      FAIL=$((FAIL + 1))
      echo "${tid},${label},${compiler},${backend},${target},FAIL,${elapsed},0,\"${err}\"" >> "$CSV"
   fi
}

echo "╔══════════════════════════════════════════════════════════════════════════════╗"
echo "║              psizam Cross-Compile / Host / Self-Compile Matrix              ║"
echo "║                            $(date '+%Y-%m-%d %H:%M:%S')                               ║"
echo "╚══════════════════════════════════════════════════════════════════════════════╝"
echo ""

# =============================================================================
# SECTION 1: COMPILATION MATRIX
# =============================================================================
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "SECTION 1: COMPILATION MATRIX"
echo "═══════════════════════════════════════════════════════════════════════════════"

declare -a INPUTS=("$WASM_FIB" "$WASM_EOSIO" "$WASM_PZAM_NOLLVM")
declare -a LABELS=("fib" "eosio" "pzam-nollvm")

for idx in 0 1 2; do
   INPUT="${INPUTS[$idx]}"
   LABEL="${LABELS[$idx]}"
   echo ""
   echo "── Input: $LABEL ($(wc -c < "$INPUT" | tr -d ' ') bytes) ──"

   # Native compiler
   for backend in jit2 llvm; do
      for target in x86_64 aarch64; do
         tid="${LABEL}_native_${backend}_${target}"
         run_compile "$tid" "$LABEL" "native" "$backend" "$target" \
            "'$NATIVE_COMPILE' --target=$target --backend=$backend '$INPUT' -o '$RESULTS_DIR/pzam/${tid}.pzam'"
      done
   done

   # Wasmtime + jit2 (nollvm binary)
   for target in x86_64 aarch64; do
      tid="${LABEL}_wasmtime_jit2_${target}"
      run_compile "$tid" "$LABEL" "wasmtime" "jit2" "$target" \
         "'$WASMTIME' run --dir=/ '$WASI_COMPILE_JIT2' --target=$target --backend=jit2 '$INPUT' -o '$RESULTS_DIR/pzam/${tid}.pzam'"
   done

   # Wasmtime + llvm (full binary)
   for target in x86_64 aarch64; do
      tid="${LABEL}_wasmtime_llvm_${target}"
      run_compile "$tid" "$LABEL" "wasmtime" "llvm" "$target" \
         "'$WASMTIME' run --dir=/ '$WASI_COMPILE_LLVM' --target=$target --backend=llvm '$INPUT' -o '$RESULTS_DIR/pzam/${tid}.pzam'"
   done

   # Wasmer + jit2
   for target in x86_64 aarch64; do
      tid="${LABEL}_wasmer_jit2_${target}"
      run_compile "$tid" "$LABEL" "wasmer" "jit2" "$target" \
         "'$WASMER' run --volume '$ROOT:$ROOT' --volume '$RESULTS_DIR:$RESULTS_DIR' '$WASI_COMPILE_JIT2' -- --target=$target --backend=jit2 '$INPUT' -o '$RESULTS_DIR/pzam/${tid}.pzam'"
   done

   # Wasmer + llvm
   for target in x86_64 aarch64; do
      tid="${LABEL}_wasmer_llvm_${target}"
      run_compile "$tid" "$LABEL" "wasmer" "llvm" "$target" \
         "'$WASMER' run --volume '$ROOT:$ROOT' --volume '$RESULTS_DIR:$RESULTS_DIR' '$WASI_COMPILE_LLVM' -- --target=$target --backend=llvm '$INPUT' -o '$RESULTS_DIR/pzam/${tid}.pzam'"
   done
done

# =============================================================================
# SECTION 2: EXECUTION TESTS (aarch64 .pzam via pzam-run)
# =============================================================================
echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "SECTION 2: EXECUTION TESTS (fib_simple → pzam-run, expect 'fib(10) = 55')"
echo "═══════════════════════════════════════════════════════════════════════════════"

for compiler in native wasmtime wasmer; do
   for backend in jit2 llvm; do
      tid="exec_fib_${compiler}_${backend}_aarch64"
      pzam="$RESULTS_DIR/pzam/fib_${compiler}_${backend}_aarch64.pzam"
      TOTAL=$((TOTAL + 1))
      printf "  %-55s " "$tid"
      if [ ! -f "$pzam" ]; then
         printf "⏭️  (no .pzam)\n"
         SKIP=$((SKIP + 1))
         echo "${tid},exec,pzam-run,,aarch64,SKIP,0,0,\"no pzam\"" >> "$CSV"
         continue
      fi
      t0=$(python3 -c 'import time; print(time.time())')
      actual=$("$NATIVE_RUN" "$WASM_FIB" "$pzam" 2>"$RESULTS_DIR/logs/${tid}.log" || true)
      t1=$(python3 -c 'import time; print(time.time())')
      elapsed=$(python3 -c "print(f'{$t1-$t0:.2f}')")
      if echo "$actual" | grep -qF "fib(10) = 55"; then
         printf "✅ %7ss  output OK\n" "$elapsed"
         PASS=$((PASS + 1))
         echo "${tid},exec,pzam-run,,aarch64,PASS,${elapsed},0," >> "$CSV"
      else
         printf "❌ %7ss  got: '%s'\n" "$elapsed" "$(echo "$actual" | head -1)"
         FAIL=$((FAIL + 1))
         echo "${tid},exec,pzam-run,,aarch64,FAIL,${elapsed},0,\"output mismatch\"" >> "$CSV"
      fi
   done
done

# =============================================================================
# SECTION 3: SELF-COMPILATION
# (compile pzam-compile.wasm → .pzam, run it to compile fib, run fib)
# =============================================================================
echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "SECTION 3: SELF-COMPILATION (pzam-compile.wasm → .pzam → compile fib → run)"
echo "═══════════════════════════════════════════════════════════════════════════════"

for compiler in native wasmtime wasmer; do
   for backend in jit2 llvm; do
      pzam_compiler="$RESULTS_DIR/pzam/pzam-nollvm_${compiler}_${backend}_aarch64.pzam"
      tid="selfcomp_${compiler}_${backend}"
      TOTAL=$((TOTAL + 1))
      printf "  %-55s " "$tid"
      if [ ! -f "$pzam_compiler" ]; then
         printf "⏭️  (no compiler .pzam)\n"
         SKIP=$((SKIP + 1))
         echo "${tid},selfcomp,,,aarch64,SKIP,0,0,\"no pzam\"" >> "$CSV"
         continue
      fi
      logfile="$RESULTS_DIR/logs/${tid}.log"
      out_pzam="$RESULTS_DIR/pzam/${tid}_fib.pzam"
      t0=$(python3 -c 'import time; print(time.time())')
      if "$NATIVE_RUN" --dir="$ROOT" --dir="$RESULTS_DIR" \
         "$WASM_PZAM_NOLLVM" "$pzam_compiler" \
         --target=aarch64 --backend=jit2 "$WASM_FIB" -o "$out_pzam" \
         > "$logfile" 2>&1; then
         actual=$("$NATIVE_RUN" "$WASM_FIB" "$out_pzam" 2>>"$logfile" || true)
         t1=$(python3 -c 'import time; print(time.time())')
         elapsed=$(python3 -c "print(f'{$t1-$t0:.2f}')")
         if echo "$actual" | grep -qF "fib(10) = 55"; then
            printf "✅ %7ss  compiled & ran OK\n" "$elapsed"
            PASS=$((PASS + 1))
            echo "${tid},selfcomp,pzam-run,,aarch64,PASS,${elapsed},$(stat -f%z "$out_pzam")," >> "$CSV"
         else
            printf "❌ %7ss  output: '%s'\n" "$elapsed" "$(echo "$actual" | head -1)"
            FAIL=$((FAIL + 1))
            echo "${tid},selfcomp,pzam-run,,aarch64,FAIL,${elapsed},0,\"output mismatch\"" >> "$CSV"
         fi
      else
         t1=$(python3 -c 'import time; print(time.time())')
         elapsed=$(python3 -c "print(f'{$t1-$t0:.2f}')")
         err=$(tail -1 "$logfile" | tr ',' ';' | head -c 200)
         printf "❌ %7ss  %s\n" "$elapsed" "$err"
         FAIL=$((FAIL + 1))
         echo "${tid},selfcomp,pzam-run,,aarch64,FAIL,${elapsed},0,\"${err}\"" >> "$CSV"
      fi
   done
done

# =============================================================================
# SECTION 4: DETERMINISM (LLVM outputs should be bit-identical)
# =============================================================================
echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "SECTION 4: DETERMINISM (LLVM outputs: native vs wasmtime vs wasmer)"
echo "═══════════════════════════════════════════════════════════════════════════════"

for label in fib eosio; do
   for target in x86_64 aarch64; do
      native_pzam="$RESULTS_DIR/pzam/${label}_native_llvm_${target}.pzam"
      for other in wasmtime wasmer; do
         other_pzam="$RESULTS_DIR/pzam/${label}_${other}_llvm_${target}.pzam"
         tid="determ_${label}_${target}_native_vs_${other}"
         TOTAL=$((TOTAL + 1))
         printf "  %-55s " "$tid"
         if [ ! -f "$native_pzam" ] || [ ! -f "$other_pzam" ]; then
            printf "⏭️  (missing file)\n"
            SKIP=$((SKIP + 1))
            echo "${tid},determ,diff,,${target},SKIP,0,0,\"missing pzam\"" >> "$CSV"
            continue
         fi
         if diff "$native_pzam" "$other_pzam" > /dev/null 2>&1; then
            printf "✅ bit-identical\n"
            PASS=$((PASS + 1))
            echo "${tid},determ,diff,,${target},PASS,0,0," >> "$CSV"
         else
            sz1=$(stat -f%z "$native_pzam")
            sz2=$(stat -f%z "$other_pzam")
            printf "❌ differ (native=%s vs %s=%s)\n" "$(numfmt_size $sz1)" "$other" "$(numfmt_size $sz2)"
            FAIL=$((FAIL + 1))
            echo "${tid},determ,diff,,${target},FAIL,0,0,\"files differ: $sz1 vs $sz2\"" >> "$CSV"
         fi
      done
   done
done

# =============================================================================
# SUMMARY
# =============================================================================
echo ""
echo "═══════════════════════════════════════════════════════════════════════════════"
echo "SUMMARY: $PASS ✅  $FAIL ❌  $SKIP ⏭️   of $TOTAL total"
echo "═══════════════════════════════════════════════════════════════════════════════"
echo ""
echo "CSV:       $CSV"
echo "Logs:      $RESULTS_DIR/logs/"
echo "Artifacts: $RESULTS_DIR/pzam/"
