// token.cpp — Implementation of the token smart contract.
//
// The declaration and reflection metadata live in
// <blockchain-example/token.hpp>; that is the face other services
// include to build proxy calls. Here we implement the methods and
// emit PSIBASE_DISPATCH so the same translation unit produces the
// canonical `apply` export when compiled into token.wasm.
//
// The same source is also linked into blockchain.wasm so that the
// Phase-1 orchestrator can invoke TokenService in-process (no
// cross-instance call). Phase 2 will dispatch via the runtime
// instead, and this file will not change.
//
// Nothing in this file touches transactions. The orchestrator is
// expected to publish a write transaction via
// psi::blockchain::detail::active_tx_scope before calling into a
// method — see blockchain.cpp::with_write_tx.

#include <blockchain-example/token.hpp>

#include <psi/blockchain_guest.hpp>
#include <psi/service.hpp>

#include <cstdint>
#include <cstring>

namespace blockchain_example
{
   namespace
   {
      using psi::db::bytes;
      using psio::name_id;

      bytes to_key(name_id id)
      {
         auto* p = reinterpret_cast<const uint8_t*>(&id.value);
         return {p, p + sizeof(id.value)};
      }

      bytes to_val(uint64_t v)
      {
         auto* p = reinterpret_cast<const uint8_t*>(&v);
         return {p, p + sizeof(v)};
      }

      uint64_t read_u64(const bytes& b)
      {
         uint64_t v = 0;
         if (b.size() >= sizeof(v))
            std::memcpy(&v, b.data(), sizeof(v));
         return v;
      }

      constexpr uint64_t BANK_INITIAL = 1'000'000;
   }  // namespace

   void TokenService::init()
   {
      using namespace psio::literals;

      auto tbl = psi::blockchain::current_tx().get_table("balances");
      if (!tbl)
         return;  // Orchestrator didn't install a tx, or table-ensure
                  // failed; nothing we can do from the service layer.

      if (!tbl.get(to_key("bank"_n)))
         tbl.upsert(to_key("bank"_n), to_val(BANK_INITIAL));
   }

   void TokenService::transfer(name_id          from,
                               name_id          to,
                               uint64_t         amount,
                               std::string_view /*memo*/)
   {
      auto tbl = psi::blockchain::current_tx().get_table("balances");

      // ISSUE psibase-check-surface-errors: psibase::check traps on
      // failure, which unwinds the whole wasm. In a real PoC we want
      // the orchestrator to catch the trap and translate it into a
      // 4xx response. Currently any bad transfer kills the worker.
      psibase::check(bool(tbl), "no balances table");

      auto from_val = tbl.get(to_key(from));
      psibase::check(from_val.has_value(), "sender not found");

      uint64_t from_bal = read_u64(*from_val);
      psibase::check(from_bal >= amount, "insufficient funds");

      tbl.upsert(to_key(from), to_val(from_bal - amount));

      uint64_t to_bal = 0;
      if (auto to_rec = tbl.get(to_key(to)))
         to_bal = read_u64(*to_rec);
      tbl.upsert(to_key(to), to_val(to_bal + amount));
   }

   uint64_t TokenService::balance(name_id account)
   {
      auto tbl = psi::blockchain::current_tx().get_table("balances");
      if (!tbl)
         return 0;
      auto v = tbl.get(to_key(account));
      return v ? read_u64(*v) : 0;
   }

}  // namespace blockchain_example

PSIBASE_DISPATCH(blockchain_example::TokenService)
