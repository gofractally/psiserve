#pragma once

// blockchain-example/token.hpp — TokenService interface.
//
// The declaration + reflection that makes up the public face of the
// token contract. `token.cpp` implements the methods and instantiates
// PSIBASE_DISPATCH; other services include this header to obtain the
// proxy that packs fracpack actions for cross-contract calls.
//
// Mirrors the psibase convention of a header-only interface under
// <services/user/Foo.hpp> with reflection metadata declared next to
// the class.

#include <psio/name.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <string_view>

namespace blockchain_example
{

   /// The token smart contract.
   ///
   /// Owns a single `balances` table keyed by account `name_id` with
   /// `uint64_t` values. The first call to `init()` seeds the bank
   /// account with the genesis supply.
   struct TokenService
   {
      /// One-time setup: create the balances table and mint the
      /// genesis supply into the `bank` account. Idempotent — calling
      /// `init` twice is a no-op.
      void init();

      /// Move `amount` tokens from `from` to `to`. Creates the
      /// recipient account if needed. Aborts the transaction with
      /// `psibase::check` on missing source or insufficient funds.
      void transfer(psio::name_id    from,
                    psio::name_id    to,
                    uint64_t         amount,
                    std::string_view memo);

      /// Read-only balance lookup. Returns 0 for unknown accounts.
      uint64_t balance(psio::name_id account);
   };

   // clang-format off
   PSIO_REFLECT(TokenService,
      method(init),
      method(transfer, from, to, amount, memo),
      method(balance, account))
   // clang-format on

}  // namespace blockchain_example
