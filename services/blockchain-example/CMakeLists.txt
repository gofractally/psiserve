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

set(_local_flags -I${CMAKE_CURRENT_SOURCE_DIR})
set(_psi_sdk_inc -I${CMAKE_SOURCE_DIR}/libraries/psi-sdk/include)

# ── blockchain.wasm (Phase 1: monolithic server) ─────────────────────────────
psi_add_wasm_module(blockchain_example
   SOURCES blockchain.cpp
   OUTPUT  blockchain.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_flags} ${_psi_sdk_inc})

# ── token.wasm ───────────────────────────────────────────────────────────────
psi_add_wasm_module(token_contract
   SOURCES token.cpp
   OUTPUT  token.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_flags})

# ── token_ui.wasm ────────────────────────────────────────────────────────────
psi_add_wasm_module(token_ui
   SOURCES token_ui.cpp
   OUTPUT  token_ui.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_flags})

# ── bc_connection.wasm (Phase 2) ─────────────────────────────────────────────
psi_add_wasm_module(bc_connection
   SOURCES bc_connection.cpp
   OUTPUT  bc_connection.wasm
   DEPS    psio psizam psi-api
   FLAGS   ${_local_flags} ${_psi_sdk_inc})
