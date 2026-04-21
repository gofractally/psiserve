// token.cpp — Token smart contract for the blockchain PoC.
//
// Clean psibase-style service. Contract calls get_database() to get
// its scoped subtree, get_table() for tables. No transactions —
// blockchain.wasm manages the tx lifecycle.

#include <psi/service.hpp>
#include <psio/name.hpp>
#include "blockchain_api_impl.hpp"

using psi::db::bytes;
using psio::name_id;

static bytes to_key(name_id id)
{
   return {reinterpret_cast<const uint8_t*>(&id.value),
           reinterpret_cast<const uint8_t*>(&id.value) + sizeof(id.value)};
}

static uint64_t read_u64(const bytes& b)
{
   uint64_t v = 0;
   if (b.size() >= sizeof(v))
      __builtin_memcpy(&v, b.data(), sizeof(v));
   return v;
}

static bytes to_val(uint64_t v)
{
   return {reinterpret_cast<const uint8_t*>(&v),
           reinterpret_cast<const uint8_t*>(&v) + sizeof(v)};
}

struct TokenService
{
   void init()
   {
      using namespace psio::literals;

      auto db       = psi::blockchain::api::get_database();
      auto balances = db.get_table("balances");

      balances.upsert(to_key("bank"_n), to_val(1'000'000));
   }

   void transfer(name_id from, name_id to, uint64_t amount, std::string memo)
   {
      auto db       = psi::blockchain::api::get_database();
      auto balances = db.get_table("balances");

      auto from_val = balances.get(to_key(from));
      psibase::check(from_val.has_value(), "sender not found");

      uint64_t from_bal = read_u64(*from_val);
      psibase::check(from_bal >= amount, "insufficient funds");

      balances.upsert(to_key(from), to_val(from_bal - amount));

      uint64_t to_bal = 0;
      auto to_val_r = balances.get(to_key(to));
      if (to_val_r)
         to_bal = read_u64(*to_val_r);

      balances.upsert(to_key(to), to_val(to_bal + amount));
   }

   uint64_t balance(name_id account)
   {
      auto db       = psi::blockchain::api::get_database();
      auto balances = db.get_table("balances");

      auto val = balances.get(to_key(account));
      return val ? read_u64(*val) : 0;
   }
};

PSIO_REFLECT(TokenService,
             method(init),
             method(transfer, from, to, amount, memo),
             method(balance, account))

PSIBASE_DISPATCH(TokenService)
