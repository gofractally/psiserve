#pragma once

// types.hpp — shared data types for the blockchain PoC.
//
// All structs use PSIO_REFLECT for fracpack serialization and
// canonical-ABI lift/lower across the host/guest boundary.

#include <cstdint>
#include <string>
#include <vector>

#include <psio/reflect.hpp>

namespace bc
{

   struct Account
   {
      std::string name;
      uint64_t    balance = 0;
   };
   PSIO_REFLECT(Account, name, balance)

   struct TransferArgs
   {
      std::string from;
      std::string to;
      uint64_t    amount = 0;
   };
   PSIO_REFLECT(TransferArgs, from, to, amount)

   struct Action
   {
      uint64_t              method = 0;
      std::vector<uint8_t>  args;
   };
   PSIO_REFLECT(Action, method, args)

   struct Transaction
   {
      std::string from;
      std::string to;
      uint64_t    amount = 0;
      std::string status;
   };
   PSIO_REFLECT(Transaction, from, to, amount, status)

   struct Block
   {
      uint64_t                  number       = 0;
      uint64_t                  timestamp_ns = 0;
      uint32_t                  tx_count     = 0;
      std::vector<Transaction>  transactions;
   };
   PSIO_REFLECT(Block, number, timestamp_ns, tx_count, transactions)

   struct QueryRequest
   {
      std::string service;
      std::string path;
      std::string query;
   };
   PSIO_REFLECT(QueryRequest, service, path, query)

   struct TransactionResult
   {
      bool        success = false;
      std::string message;
   };
   PSIO_REFLECT(TransactionResult, success, message)

}  // namespace bc
