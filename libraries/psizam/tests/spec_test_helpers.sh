#!/bin/bash
# Helper script for spec test build steps. Invoked from CMake to allow build
# to continue when wast2json doesn't support a particular .wast file's features.
#
# Usage:
#   spec_test_helpers.sh wast2json <wast2json> <flags> <wast> <out_json>
#       Run wast2json. On failure, touch the output json (empty) so downstream
#       targets can detect "regen unavailable" and skip cleanly.
#
#   spec_test_helpers.sh gen_tests <generator> <json> <wasm_dir> <out_cpp>
#       Run spec_test_generator only when <json> is non-empty. On empty json,
#       leave the existing checked-in <out_cpp> alone (just touch it so ninja
#       sees it as up-to-date).

set -e

cmd="$1"
shift

case "$cmd" in
   wast2json)
      wast2json_bin="$1"; shift
      out_json="${@: -1}"   # last arg is output path
      # Drop the last arg from the wast2json argv:
      argv=("${@:1:$#-1}")
      # Preprocess the .wast (last arg of argv) to strip constructs newer
      # wabt can't parse: `(module definition ...)` forms and any assert_*
      # block containing `0x1_0000_0000` (post-Memory64 oversize-validation
      # tests). These strip cleanly from memory.wast without affecting the
      # rest of the module sequence.
      wast_path="${argv[${#argv[@]}-1]}"
      tmp_wast=$(mktemp -t wast_filtered.XXXXXX).wast
      trap 'rm -f "$tmp_wast"' EXIT
      awk '
      BEGIN { buf = ""; depth = 0; in_block = 0 }
      function flush() {
        if (buf != "") {
          if (buf ~ /0x1_0000_0000[^_0-9]/ || buf ~ /module[ \t]+definition/) {
            # drop
          } else {
            printf "%s", buf
          }
          buf = ""
        }
      }
      {
        line = $0 "\n"
        scratch = line
        n_open = gsub(/\(/, "(", scratch)
        scratch = line
        n_close = gsub(/\)/, ")", scratch)
        if (in_block) {
          buf = buf line
          depth += n_open - n_close
          if (depth <= 0) { flush(); in_block = 0; depth = 0 }
        } else {
          if (n_open > n_close) {
            in_block = 1; buf = line; depth = n_open - n_close
          } else {
            buf = line; flush()
          }
        }
      }
      END { flush() }
      ' "$wast_path" > "$tmp_wast"
      # Substitute the filtered wast into argv:
      argv[${#argv[@]}-1]="$tmp_wast"
      # wast2json emits .wasm side-effect files in the same directory as its
      # -o argument. Tests read .wasm from ${CMAKE_BINARY_DIR}/wasms/, which
      # cmake sets as our CWD — so emit with a bare basename (→ files land in
      # CWD) and then move just the .json to its final location.
      json_name=$(basename "$out_json")
      if "$wast2json_bin" "${argv[@]}" -o "$json_name" 2>/dev/null; then
         [ "$(pwd)/$json_name" != "$out_json" ] && mv "$json_name" "$out_json"
         exit 0
      else
         echo "wast2json failed for $out_json — keeping checked-in tests cpp" 1>&2
         rm -f "$json_name"
         : > "$out_json"
         exit 0
      fi
      ;;
   gen_tests)
      gen="$1"; json="$2"; wasm_dir="$3"; out_cpp="$4"
      if [ -s "$json" ]; then
         "$gen" "$json" "$wasm_dir" > "$out_cpp"
      else
         touch "$out_cpp"
      fi
      ;;
   *)
      echo "Unknown command: $cmd" 1>&2
      exit 1
      ;;
esac
