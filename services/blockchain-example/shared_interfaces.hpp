#pragma once

// shared_interfaces.hpp — WIT-style interface declarations for the
// blockchain PoC. Each struct declares the functions an interface
// provides; PSIO_INTERFACE gives the reflection machinery the
// function list and parameter names for canonical-ABI wiring.
//
// Interfaces:
//   blockchain_api  — what contracts see (get_database, push_transaction, etc.)
//   bc_connection   — per-connection handler (handle_connection)
//   token_contract  — smart contract entry point (apply)
//   token_ui        — UI query handler (handle_query)
//   env             — debug logging

#include <cstdint>
#include <string_view>
#include <vector>

#include <psio/guest_attrs.hpp>
#include <psio/name.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>
#include <psio/wit_resource.hpp>

#include "types.hpp"

PSIO_PACKAGE(blockchain_example, "0.1.0");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(blockchain_example)

// ── blockchain_api ──────────────────────────────────────────────────
//
// The interface that blockchain.wasm exports to contracts and UI
// modules. Contracts call get_database() to get a scoped subtree;
// connections call push_transaction() and query_ui().

struct blockchain_api
{
   static uint32_t           push_transaction(std::vector<uint8_t> tx_bytes);
   static wit::vector<uint8_t> query_ui(std::string_view service,
                                         std::string_view path,
                                         std::string_view query);
   static wit::vector<uint8_t> query_blocks(uint32_t count);
   static uint64_t           get_head_block_num();
};

PSIO_INTERFACE(blockchain_api, types(),
   funcs(func(push_transaction, tx_bytes),
         func(query_ui,         service, path, query),
         func(query_blocks,     count),
         func(get_head_block_num)))

// ── bc_connection ───────────────────────────────────────────────────
//
// Exported by bc_connection.wasm. blockchain.wasm calls
// handle_connection via async_call with thread::fresh.
// The socket fd is passed as a scalar (ownership transferred by
// the host's fd-table move).

struct bc_connection_iface
{
   static void handle_connection(uint32_t blockchain_handle, uint32_t sock_fd);
};

PSIO_INTERFACE(bc_connection_iface, types(),
   funcs(func(handle_connection, blockchain_handle, sock_fd)))

// ── token_contract ──────────────────────────────────────────────────
//
// Exported by token.wasm. blockchain.wasm calls apply() with
// fracpack-encoded action bytes.

struct token_contract
{
   static uint32_t apply(std::vector<uint8_t> action_data);
};

PSIO_INTERFACE(token_contract, types(),
   funcs(func(apply, action_data)))

// ── token_ui ────────────────────────────────────────────────────────
//
// Exported by token_ui.wasm. Handles GET queries and returns
// response bytes (HTML or JSON).

struct token_ui_iface
{
   static wit::vector<uint8_t> handle_query(std::string_view path,
                                             std::string_view query);
};

PSIO_INTERFACE(token_ui_iface, types(),
   funcs(func(handle_query, path, query)))

// ── env ─────────────────────────────────────────────────────────────
//
// Debug logging. Available to all modules.

struct bc_env
{
   PSIO_IMPORT(bc_env, log)
   static void log(std::string_view msg);
};

PSIO_INTERFACE(bc_env, types(),
   funcs(func(log, msg)))
