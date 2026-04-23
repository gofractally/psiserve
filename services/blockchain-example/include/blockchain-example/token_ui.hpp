#pragma once

// blockchain-example/token_ui.hpp — UI rendering seam.
//
// token_ui owns everything presentation: the HTML shell, the JSON
// serialization of query responses. bc_connection calls into these
// functions to turn chain data into bytes for the HTTP body.
//
// Nothing here knows anything about transactions, tables, or fiber
// scheduling. All chain state reaches us through the `chain::` API.

#include <blockchain-example/blockchain.hpp>

#include <span>
#include <string>
#include <string_view>

namespace blockchain_example::token_ui
{

   /// Static HTML page served at GET /. Owned by this module so the
   /// chain never touches markup.
   std::string_view index_html();

   // ── JSON response renderers ────────────────────────────────────────

   /// {"account":"NAME","balance":N}
   std::string render_balance(std::string_view account,
                              uint64_t         balance,
                              bool             found);

   /// {"success":BOOL,"message":"..."}
   std::string render_transfer_result(bool             success,
                                      std::string_view message);

   /// JSON array of block headers, newest first.
   std::string render_blocks(std::span<const chain::BlockHeader> blocks);

}  // namespace blockchain_example::token_ui
