# ── Blockchain PoC Service ────────────────────────────────────────────────────
#
# Builds 4 WASM modules via psi_add_wasm_module:
#   blockchain.wasm   — orchestrator (HTTP server + block production)
#   bc_connection.wasm — per-connection handler (Phase 2)
#   token.wasm        — smart contract
#   token_ui.wasm     — UI query handler
#
# Phase 1: blockchain.wasm handles everything inline (sequential accept).
# Phase 2: blockchain.wasm delegates to bc_connection + token + token_ui
#           via cross-instance dispatch.

# Service-local public headers live under include/blockchain-example/ so
# sources can pull them in as <blockchain-example/foo.hpp>. psi-api and
# psi-sdk supply <psi/*> and <psio/*> headers via their own targets.
set(_local_inc  -I${CMAKE_CURRENT_SOURCE_DIR}/include)
set(_psi_sdk_inc -I${CMAKE_SOURCE_DIR}/libraries/psi-sdk/include)

# ── blockchain.wasm — Phase 1 monolithic build ───────────────────────────────
#
# All four services share one wasm binary in Phase 1. Their sources
# are the same ones used for the per-service Phase 2 wasms below; only
# the linkage changes. `_start` comes from bc_connection.cpp (the HTTP
# front end); the chain/token/ui functions are plain C++ calls.
#
# token.cpp's PSIBASE_DISPATCH(TokenService) emits an `apply` export
# here too. The host never calls it in Phase 1, but its presence
# keeps the Phase-1 artifact byte-compatible with what a future
# orchestrator would load if it ever wanted to reach into this binary
# via the action ABI.
psi_add_wasm_module(blockchain_example
   SOURCES bc_connection.cpp
           blockchain.cpp
           token.cpp
           token_ui.cpp
           guest_alloc.cpp
   OUTPUT  blockchain.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_inc} ${_psi_sdk_inc})

# ── Phase 2 per-service wasms ────────────────────────────────────────────────
#
# These build the same sources in isolation so that once the runtime
# infrastructure is ready (cross-instance dispatch + socket transfer),
# we can swap the monolithic binary above for the multi-module layout
# below without touching any service code.

psi_add_wasm_module(token_contract
   SOURCES token.cpp guest_alloc.cpp
   OUTPUT  token.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_inc})

psi_add_wasm_module(token_ui
   SOURCES token_ui.cpp guest_alloc.cpp
   OUTPUT  token_ui.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_inc})

# ISSUE phase-2-bc-connection: bc_connection.wasm can't be built as a
# standalone module yet because its _start opens the chain in-process.
# Phase 2 reshapes it into a `handle_connection(sock_fd)` entry driven
# by blockchain.wasm's listener fiber. Until then, bc_connection.cpp
# ships only through blockchain.wasm above.
