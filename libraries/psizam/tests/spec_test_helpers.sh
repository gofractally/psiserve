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
      if "$wast2json_bin" "${argv[@]}" -o "$out_json" 2>/dev/null; then
         exit 0
      else
         echo "wast2json failed for $out_json — keeping checked-in tests cpp" 1>&2
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
