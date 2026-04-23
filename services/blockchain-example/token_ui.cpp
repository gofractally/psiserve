// token_ui.cpp — UI rendering for the token example.
//
// Implements the seam declared in
// <blockchain-example/token_ui.hpp>. Owns the HTML shell and the JSON
// responses bc_connection ships back to the browser. Knows nothing
// about transactions, tables, or the wire — it is given chain values
// by the caller and returns bytes.
//
// Phase 2 splits this module into its own WASM instance. Nothing in
// the surface below assumes it runs in-process with the chain.

#include <blockchain-example/token_ui.hpp>

#include <cstdio>
#include <string>

namespace blockchain_example::token_ui
{
   namespace
   {
      // NOTE: raw string literal; keep it straightforward HTML so a
      // future build can hash this byte-for-byte as a content-addressed
      // asset if we want.
      constexpr std::string_view kIndexHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Token Blockchain Explorer</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: system-ui, -apple-system, sans-serif; background: #0f172a; color: #e2e8f0; padding: 2rem; }
  h1 { color: #38bdf8; margin-bottom: 1.5rem; }
  .panel { background: #1e293b; border-radius: 8px; padding: 1.5rem; margin-bottom: 1rem; }
  .panel h2 { color: #94a3b8; font-size: 0.875rem; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 1rem; }
  input, button { padding: 0.5rem 1rem; border-radius: 4px; border: 1px solid #334155; background: #0f172a; color: #e2e8f0; font-size: 1rem; }
  button { background: #2563eb; border-color: #2563eb; cursor: pointer; font-weight: 600; }
  button:hover { background: #1d4ed8; }
  .form-row { display: flex; gap: 0.5rem; margin-bottom: 0.75rem; }
  .form-row input { flex: 1; }
  .balance-display { font-size: 2rem; font-weight: 700; color: #22c55e; }
  .status { padding: 0.75rem; border-radius: 4px; margin-top: 0.5rem; display: none; }
  .status.success { display: block; background: #052e16; color: #4ade80; }
  .status.error { display: block; background: #450a0a; color: #f87171; }
  .block { background: #0f172a; border-radius: 4px; padding: 0.75rem; margin-bottom: 0.5rem; }
  .block-num { color: #38bdf8; font-weight: 600; }
  .block-time { color: #64748b; font-size: 0.875rem; }
  .block-txs { color: #94a3b8; font-size: 0.875rem; }
  #blocks { max-height: 400px; overflow-y: auto; }
</style>
</head>
<body>
<h1>Token Blockchain Explorer</h1>

<div class="panel">
  <h2>Check Balance</h2>
  <div class="form-row">
    <input type="text" id="bal-account" placeholder="Account name" value="bank">
    <button onclick="checkBalance()">Check</button>
  </div>
  <div class="balance-display" id="bal-display">--</div>
</div>

<div class="panel">
  <h2>Transfer Tokens</h2>
  <div class="form-row">
    <input type="text" id="tx-from" placeholder="From">
    <input type="text" id="tx-to" placeholder="To">
    <input type="number" id="tx-amount" placeholder="Amount">
    <button onclick="doTransfer()">Send</button>
  </div>
  <div class="status" id="tx-status"></div>
</div>

<div class="panel">
  <h2>Recent Blocks</h2>
  <div id="blocks"></div>
</div>

<script>
async function checkBalance() {
  const account = document.getElementById('bal-account').value;
  try {
    const r = await fetch('/api/balance?account=' + encodeURIComponent(account));
    const j = await r.json();
    document.getElementById('bal-display').textContent =
      j.balance !== undefined ? j.balance.toLocaleString() : 'Not found';
  } catch(e) {
    document.getElementById('bal-display').textContent = 'Error';
  }
}

async function doTransfer() {
  const from = document.getElementById('tx-from').value;
  const to = document.getElementById('tx-to').value;
  const amount = parseInt(document.getElementById('tx-amount').value);
  const status = document.getElementById('tx-status');
  try {
    const r = await fetch('/api/transfer', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({from, to, amount})
    });
    const j = await r.json();
    status.className = 'status ' + (j.success ? 'success' : 'error');
    status.textContent = j.message;
  } catch(e) {
    status.className = 'status error';
    status.textContent = 'Network error: ' + e.message;
  }
}

async function refreshBlocks() {
  try {
    const r = await fetch('/api/blocks');
    const blocks = await r.json();
    const el = document.getElementById('blocks');
    el.innerHTML = blocks.map(b =>
      '<div class="block">' +
      '<span class="block-num">Block #' + b.number + '</span> ' +
      '<span class="block-time">' + new Date(b.timestamp_ns / 1000000).toLocaleTimeString() + '</span> ' +
      '<span class="block-txs">' + b.tx_count + ' tx</span>' +
      '</div>'
    ).join('');
  } catch(e) {}
}

checkBalance();
refreshBlocks();
setInterval(refreshBlocks, 3000);
</script>
</body>
</html>
)html";

      void append_uint(std::string& s, uint64_t v)
      {
         char buf[21];
         int  n = 0;
         if (v == 0) buf[n++] = '0';
         else while (v) { buf[n++] = char('0' + v % 10); v /= 10; }
         for (int i = n - 1; i >= 0; --i) s.push_back(buf[i]);
      }
   }  // namespace

   std::string_view index_html() { return kIndexHtml; }

   std::string render_balance(std::string_view account, uint64_t balance,
                              bool found)
   {
      // ISSUE json-escape: account names come from URL query and flow
      // straight into the response. A real build would JSON-escape
      // the value; for the PoC we trust the sender (and name_id's
      // character set happens to be safe).
      std::string json = "{\"account\":\"";
      if (found) json.append(account.data(), account.size());
      else       json.append("not_found");
      json.append("\",\"balance\":");
      append_uint(json, found ? balance : 0);
      json.push_back('}');
      return json;
   }

   std::string render_transfer_result(bool             success,
                                      std::string_view message)
   {
      std::string json = "{\"success\":";
      json.append(success ? "true" : "false");
      json.append(",\"message\":\"");
      json.append(message.data(), message.size());
      json.append("\"}");
      return json;
   }

   std::string render_blocks(std::span<const chain::BlockHeader> blocks)
   {
      std::string json = "[";
      bool first = true;
      for (const auto& b : blocks)
      {
         if (!first) json.push_back(',');
         first = false;
         json.append("{\"number\":");       append_uint(json, b.number);
         json.append(",\"timestamp_ns\":"); append_uint(json, b.timestamp_ns);
         json.append(",\"tx_count\":");     append_uint(json, b.tx_count);
         json.push_back('}');
      }
      json.push_back(']');
      return json;
   }

}  // namespace blockchain_example::token_ui
