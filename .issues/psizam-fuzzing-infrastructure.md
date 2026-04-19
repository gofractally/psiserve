---
id: psizam-fuzzing-infrastructure
title: psizam differential fuzzing infrastructure (libFuzzer)
status: in-progress
priority: high
area: psizam
agent: psiserve-agent3
branch: main
created: 2026-04-19
depends_on: []
blocks: [psizam-eh-v2-fuzzing]
---

## Description
Build coverage-guided differential fuzzer using libFuzzer to catch
cross-backend correctness divergences and crashes in psizam.

## Acceptance Criteria
- [x] libFuzzer coverage-guided differential fuzzer added
- [x] .gitignore for libFuzzer corpus directory
- [x] Fuzz regression harness for known crash cases
- [ ] Corpus and reproducers committed/organized (large untracked set in agent3)
- [ ] CI integration for fuzzer

## Dirty Files (agent3 working set)
- `libraries/psizam/tests/fuzz/` — crash_*.wasm, mismatch_*.wasm corpus files
- `libraries/psizam/tests/fuzz/run-locked.py`
- `libraries/psizam/tests/fuzz/wasm-gen/` — generated test cases
- `libraries/psizam/tests/fuzz/wasm-gen/reproducers/`
