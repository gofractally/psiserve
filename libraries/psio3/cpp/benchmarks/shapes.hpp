#pragma once
//
// libraries/psio3/cpp/benchmarks/shapes.hpp — unified shape library.
//
// Parallel v1/v3 reflected types at each size tier. Types live at global
// scope with V1-/V3- prefixes because v1's PSIO_REFLECT macro generates
// reflection at the translation-unit level using the unqualified type
// name, and doesn't round-trip fully-qualified names from a namespace.
// Matches the pattern already used by v1_oracle_tests.cpp.

#include <psio/reflect.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/reflect.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Tier 1: Point — 8 B fixed, DWNC ───────────────────────────────────
struct V1Point
{
   std::int32_t x = 0, y = 0;
   friend bool operator==(const V1Point&, const V1Point&) = default;
};
PSIO_REFLECT(V1Point, definitionWillNotChange(), x, y)

struct V3Point
{
   std::int32_t x = 0, y = 0;
   friend bool operator==(const V3Point&, const V3Point&) = default;
};
PSIO3_REFLECT(V3Point, x, y)
PSIO3_TYPE_ATTRS(V3Point, psio3::definition_will_not_change{})

// ── Tier 2: NameRecord — 16 B fixed, DWNC ─────────────────────────────
struct V1NameRecord
{
   std::uint64_t account = 0, limit = 0;
   friend bool operator==(const V1NameRecord&,
                           const V1NameRecord&) = default;
};
PSIO_REFLECT(V1NameRecord, definitionWillNotChange(), account, limit)

struct V3NameRecord
{
   std::uint64_t account = 0, limit = 0;
   friend bool operator==(const V3NameRecord&,
                           const V3NameRecord&) = default;
};
PSIO3_REFLECT(V3NameRecord, account, limit)
PSIO3_TYPE_ATTRS(V3NameRecord, psio3::definition_will_not_change{})

// ── Tier 3: FlatRecord — non-DWNC, variable ───────────────────────────
struct V1FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const V1FlatRecord&,
                           const V1FlatRecord&) = default;
};
PSIO_REFLECT(V1FlatRecord, id, label, values)

struct V3FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const V3FlatRecord&,
                           const V3FlatRecord&) = default;
};
PSIO3_REFLECT(V3FlatRecord, id, label, values)

// ── Tier 4: Record — non-DWNC, variable + optional ────────────────────
struct V1Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const V1Record&, const V1Record&) = default;
};
PSIO_REFLECT(V1Record, id, label, values, score)

struct V3Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const V3Record&, const V3Record&) = default;
};
PSIO3_REFLECT(V3Record, id, label, values, score)

// ── Tier 5: Validator — 65 B fixed, DWNC ─────────────────────────────
//
// Packed with __attribute__((packed)) so sum(field_sizes) == sizeof(T).
// Matches the canonical Ethereum Validator memory layout for wire
// correctness with v1's pssz/bin/ssz DWNC memcpy fast paths.
struct __attribute__((packed)) V1Validator
{
   std::uint64_t pubkey_lo          = 0;
   std::uint64_t pubkey_hi          = 0;
   std::uint64_t withdrawal_lo      = 0;
   std::uint64_t withdrawal_hi      = 0;
   std::uint64_t effective_balance  = 0;
   bool          slashed            = false;
   std::uint64_t activation_epoch   = 0;
   std::uint64_t exit_epoch         = 0;
   std::uint64_t withdrawable_epoch = 0;
   friend bool operator==(const V1Validator&,
                           const V1Validator&) = default;
};
PSIO_REFLECT(V1Validator, definitionWillNotChange(), pubkey_lo, pubkey_hi,
             withdrawal_lo, withdrawal_hi, effective_balance, slashed,
             activation_epoch, exit_epoch, withdrawable_epoch)

struct __attribute__((packed)) V3Validator
{
   std::uint64_t pubkey_lo          = 0;
   std::uint64_t pubkey_hi          = 0;
   std::uint64_t withdrawal_lo      = 0;
   std::uint64_t withdrawal_hi      = 0;
   std::uint64_t effective_balance  = 0;
   bool          slashed            = false;
   std::uint64_t activation_epoch   = 0;
   std::uint64_t exit_epoch         = 0;
   std::uint64_t withdrawable_epoch = 0;
   friend bool operator==(const V3Validator&,
                           const V3Validator&) = default;
};
PSIO3_REFLECT(V3Validator, pubkey_lo, pubkey_hi, withdrawal_lo,
              withdrawal_hi, effective_balance, slashed,
              activation_epoch, exit_epoch, withdrawable_epoch)
PSIO3_TYPE_ATTRS(V3Validator, psio3::definition_will_not_change{})

// ── Tier 6: Order — nested records + vector + optional ────────────────
struct V1LineItem
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const V1LineItem&,
                           const V1LineItem&) = default;
};
PSIO_REFLECT(V1LineItem, product, qty, unit_price)

struct V1UserProfile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const V1UserProfile&,
                           const V1UserProfile&) = default;
};
PSIO_REFLECT(V1UserProfile, id, name, email, age, verified)

struct V1Order
{
   std::uint64_t              id = 0;
   V1UserProfile              customer;
   std::vector<V1LineItem>    items;
   double                     total = 0.0;
   std::optional<std::string> note;
   friend bool operator==(const V1Order&, const V1Order&) = default;
};
PSIO_REFLECT(V1Order, id, customer, items, total, note)

struct V3LineItem
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const V3LineItem&,
                           const V3LineItem&) = default;
};
PSIO3_REFLECT(V3LineItem, product, qty, unit_price)

struct V3UserProfile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const V3UserProfile&,
                           const V3UserProfile&) = default;
};
PSIO3_REFLECT(V3UserProfile, id, name, email, age, verified)

struct V3Order
{
   std::uint64_t              id = 0;
   V3UserProfile              customer;
   std::vector<V3LineItem>    items;
   double                     total = 0.0;
   std::optional<std::string> note;
   friend bool operator==(const V3Order&, const V3Order&) = default;
};
PSIO3_REFLECT(V3Order, id, customer, items, total, note)

// ── Tier 7: ValidatorList — 100 Validators ~= 12.1 KB ────────────────
struct V1ValidatorList
{
   std::uint64_t            epoch = 0;
   std::vector<V1Validator> validators;
   friend bool operator==(const V1ValidatorList&,
                           const V1ValidatorList&) = default;
};
PSIO_REFLECT(V1ValidatorList, epoch, validators)

struct V3ValidatorList
{
   std::uint64_t            epoch = 0;
   std::vector<V3Validator> validators;
   friend bool operator==(const V3ValidatorList&,
                           const V3ValidatorList&) = default;
};
PSIO3_REFLECT(V3ValidatorList, epoch, validators)

// ── Sample factories (identical values on both sides) ─────────────────
namespace psio3_bench {

   inline V1Point v1_point() { return {.x = -42, .y = 77}; }
   inline V3Point v3_point() { return {.x = -42, .y = 77}; }

   inline V1NameRecord v1_namerec()
   {
      return {.account = 0x0123'4567'89AB'CDEFull, .limit = 1'000'000};
   }
   inline V3NameRecord v3_namerec()
   {
      return {.account = 0x0123'4567'89AB'CDEFull, .limit = 1'000'000};
   }

   inline V1FlatRecord v1_flatrec()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }
   inline V3FlatRecord v3_flatrec()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }

   inline V1Record v1_record()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }
   inline V3Record v3_record()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }

   template <typename V>
   inline V make_validator(std::uint64_t i)
   {
      V v;
      v.pubkey_lo          = i * 7;
      v.pubkey_hi          = i * 11;
      v.withdrawal_lo      = i * 13;
      v.withdrawal_hi      = i * 17;
      v.effective_balance  = 32'000'000'000ull;
      v.slashed            = (i % 50) == 0;
      v.activation_epoch   = 100;
      v.exit_epoch         = 0xFFFFFFFFull;
      v.withdrawable_epoch = 0xFFFFFFFFull;
      return v;
   }
   inline V1Validator v1_validator() { return make_validator<V1Validator>(1); }
   inline V3Validator v3_validator() { return make_validator<V3Validator>(1); }

   template <typename Order, typename User, typename Line>
   inline Order make_order()
   {
      Order o;
      o.id       = 10042;
      o.customer = User{.id = 77, .name = "Alice Stone",
                         .email = "alice@example.com",
                         .age = 34, .verified = true};
      o.items    = {
         Line{.product = "Widget",      .qty = 2, .unit_price = 9.99},
         Line{.product = "Sprocket",    .qty = 1, .unit_price = 14.50},
         Line{.product = "Gizmo-Mk.II", .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }
   inline V1Order v1_order()
   {
      return make_order<V1Order, V1UserProfile, V1LineItem>();
   }
   inline V3Order v3_order()
   {
      return make_order<V3Order, V3UserProfile, V3LineItem>();
   }

   template <typename List, typename Validator>
   inline List make_vlist(std::uint32_t n)
   {
      List l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator<Validator>(i));
      return l;
   }
   inline V1ValidatorList v1_vlist(std::uint32_t n = 100)
   {
      return make_vlist<V1ValidatorList, V1Validator>(n);
   }
   inline V3ValidatorList v3_vlist(std::uint32_t n = 100)
   {
      return make_vlist<V3ValidatorList, V3Validator>(n);
   }

}  // namespace psio3_bench
