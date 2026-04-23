# psibase — command-style wasm service.
#
# wasi-libc's crt1-command supplies _start, which calls main(). No
# reactor exports, no hand-rolled init. This is the minimum shape
# for a psi "program".
psi_add_wasm_module(psibase
   SOURCES    main.cpp
   OUTPUT     psibase.wasm
   EXEC_MODEL command)
