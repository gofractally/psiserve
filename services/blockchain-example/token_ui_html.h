// Auto-generated inline HTML for the token UI.
// In Phase 2 this is served by token_ui.wasm instead.

static const char TOKEN_UI_HTML[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Token Blockchain Explorer</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body { font-family: system-ui, -apple-system, sans-serif; background: #0f172a; color: #e2e8f0; padding: 2rem; max-width: 700px; margin: 0 auto; }
  h1 { color: #38bdf8; margin-bottom: 1.5rem; }
  .panel { background: #1e293b; border-radius: 8px; padding: 1.5rem; margin-bottom: 1rem; }
  .panel h2 { color: #94a3b8; font-size: 0.875rem; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 1rem; }
  input, button { padding: 0.5rem 1rem; border-radius: 4px; border: 1px solid #334155; background: #0f172a; color: #e2e8f0; font-size: 1rem; }
  button { background: #2563eb; border-color: #2563eb; cursor: pointer; font-weight: 600; }
  button:hover { background: #1d4ed8; }
  .form-row { display: flex; gap: 0.5rem; margin-bottom: 0.75rem; flex-wrap: wrap; }
  .form-row input { flex: 1; min-width: 100px; }
  .balance-display { font-size: 2rem; font-weight: 700; color: #22c55e; margin-top: 0.5rem; }
  .status { padding: 0.75rem; border-radius: 4px; margin-top: 0.5rem; display: none; }
  .status.success { display: block; background: #052e16; color: #4ade80; }
  .status.error { display: block; background: #450a0a; color: #f87171; }
  .block { background: #0f172a; border-radius: 4px; padding: 0.75rem; margin-bottom: 0.5rem; display: flex; justify-content: space-between; }
  .block-num { color: #38bdf8; font-weight: 600; }
  .block-time { color: #64748b; font-size: 0.875rem; }
  .block-txs { color: #94a3b8; font-size: 0.875rem; }
  #blocks { max-height: 300px; overflow-y: auto; }
  .empty { color: #475569; font-style: italic; }
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
    <input type="text" id="tx-from" placeholder="From" value="bank">
    <input type="text" id="tx-to" placeholder="To">
    <input type="number" id="tx-amount" placeholder="Amount">
    <button onclick="doTransfer()">Send</button>
  </div>
  <div class="status" id="tx-status"></div>
</div>

<div class="panel">
  <h2>Recent Blocks</h2>
  <div id="blocks"><div class="empty">Waiting for blocks...</div></div>
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
    if (j.success) checkBalance();
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
    if (blocks.length === 0) {
      el.innerHTML = '<div class="empty">Waiting for blocks...</div>';
      return;
    }
    el.innerHTML = blocks.map(b =>
      '<div class="block">' +
      '<span class="block-num">Block #' + b.number + '</span>' +
      '<span class="block-time">' + new Date(b.timestamp_ns / 1000000).toLocaleTimeString() + '</span>' +
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

static const int TOKEN_UI_HTML_LEN = sizeof(TOKEN_UI_HTML) - 1;
