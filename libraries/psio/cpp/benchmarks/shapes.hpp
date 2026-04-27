#pragma once
//
// libraries/psio/cpp/benchmarks/shapes.hpp — bench shape library.
//
// Reflected struct definitions used by the benchmark harness, ranging
// from a fixed-size 8-byte Point through a 100-element ValidatorList
// at ~12 KB.  Each tier exists as both an unbounded form (plain
// std::string / std::vector) and a `psio::length_bound`-annotated
// `*Bounded` form so codecs that key off bounds can be measured both
// ways.
//
// Naming policy: types live at global scope so PSIO_REFLECT works
// without namespace gymnastics; factories live in `psio_bench::`.
// (Was previously `psio3_bench::` when the library was called psio3 —
// renamed alongside the psio3 → psio rename.)

#include <psio/annotate.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Tier 1: Point — 8 B fixed, DWNC ───────────────────────────────────
struct Point
{
   std::int32_t x = 0, y = 0;
   friend bool operator==(const Point&, const Point&) = default;
};
PSIO_REFLECT(Point, x, y, definitionWillNotChange())

// ── Tier 2: NameRecord — 16 B fixed, DWNC ─────────────────────────────
struct NameRecord
{
   std::uint64_t account = 0, limit = 0;
   friend bool operator==(const NameRecord&, const NameRecord&) = default;
};
PSIO_REFLECT(NameRecord, account, limit, definitionWillNotChange())

// ── Tier 3: FlatRecord — non-DWNC, variable ───────────────────────────
struct FlatRecord
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint16_t> values;
   friend bool operator==(const FlatRecord&, const FlatRecord&) = default;
};
PSIO_REFLECT(FlatRecord, id, label, values)

// ── Tier 4: Record — non-DWNC, variable + optional ────────────────────
struct Record
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint16_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const Record&, const Record&) = default;
};
PSIO_REFLECT(Record, id, label, values, score)

// ── Tier 5: Validator — 65 B fixed, DWNC ──────────────────────────────
//
// Packed with __attribute__((packed)) so sum(field_sizes) == sizeof(T).
// Matches the canonical Ethereum Validator memory layout for wire
// correctness with pssz/bin/ssz DWNC memcpy fast paths.
struct __attribute__((packed)) Validator
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
   friend bool operator==(const Validator&, const Validator&) = default;
};
PSIO_REFLECT(Validator,
             pubkey_lo, pubkey_hi, withdrawal_lo, withdrawal_hi,
             effective_balance, slashed, activation_epoch, exit_epoch,
             withdrawable_epoch, definitionWillNotChange())

// ── Tier 6: Order — nested records + vector + optional ────────────────
struct LineItem
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const LineItem&, const LineItem&) = default;
};
PSIO_REFLECT(LineItem, product, qty, unit_price)

struct UserProfile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const UserProfile&, const UserProfile&) = default;
};
PSIO_REFLECT(UserProfile, id, name, email, age, verified)

struct Order
{
   std::uint64_t              id = 0;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total = 0.0;
   std::optional<std::string> note;
   friend bool operator==(const Order&, const Order&) = default;
};
PSIO_REFLECT(Order, id, customer, items, total, note)

// ── Tier 7: ValidatorList — N Validators (~12 KB at N=100) ────────────
struct ValidatorList
{
   std::uint64_t          epoch = 0;
   std::vector<Validator> validators;
   friend bool operator==(const ValidatorList&,
                           const ValidatorList&) = default;
};
PSIO_REFLECT(ValidatorList, epoch, validators)

// ── Tier 8: bounded variants of the unbounded shapes ──────────────────
//
// Same data as Tier 3-7, with explicit length_bound annotations
// attached via PSIO_FIELD_ATTRS-style `attr(name, max<N>)`.

struct FlatRecordBounded
{
   std::uint32_t              id = 0;
   std::string                label;
   std::vector<std::uint32_t> values;
   friend bool operator==(const FlatRecordBounded&,
                           const FlatRecordBounded&) = default;
};
PSIO_REFLECT(FlatRecordBounded, id, attr(label, max<63>),
             attr(values, max<255>))

struct RecordBounded
{
   std::uint32_t                id = 0;
   std::string                  label;
   std::vector<std::uint32_t>   values;
   std::optional<std::uint32_t> score;
   friend bool operator==(const RecordBounded&,
                           const RecordBounded&) = default;
};
PSIO_REFLECT(RecordBounded, id, attr(label, max<63>),
             attr(values, max<255>), score)

struct LineItemBounded
{
   std::string   product;
   std::uint32_t qty        = 0;
   double        unit_price = 0.0;
   friend bool operator==(const LineItemBounded&,
                           const LineItemBounded&) = default;
};
PSIO_REFLECT(LineItemBounded, attr(product, max<63>), qty, unit_price)

struct UserProfileBounded
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool operator==(const UserProfileBounded&,
                           const UserProfileBounded&) = default;
};
PSIO_REFLECT(UserProfileBounded, id, attr(name, max<63>),
             attr(email, max<255>), age, verified)

struct OrderBounded
{
   std::uint64_t                  id = 0;
   UserProfileBounded             customer;
   std::vector<LineItemBounded>   items;
   double                         total = 0.0;
   std::optional<std::string>     note;
   friend bool operator==(const OrderBounded&,
                           const OrderBounded&) = default;
};
PSIO_REFLECT(OrderBounded, id, customer, attr(items, max<255>), total,
             attr(note, max<255>))

struct ValidatorListBounded
{
   std::uint64_t          epoch = 0;
   std::vector<Validator> validators;
   friend bool operator==(const ValidatorListBounded&,
                           const ValidatorListBounded&) = default;
};
PSIO_REFLECT(ValidatorListBounded, epoch, attr(validators, max<1024>))

// ── Sample factories ──────────────────────────────────────────────────
namespace psio_bench {

   inline Point      point() { return {.x = -42, .y = 77}; }

   inline NameRecord namerec()
   {
      return {.account = 0x0123'4567'89AB'CDEFull, .limit = 1'000'000};
   }

   inline FlatRecord flatrec()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }

   inline Record record()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }

   inline Validator make_validator(std::uint64_t i)
   {
      Validator v;
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
   inline Validator validator() { return make_validator(1); }

   inline Order order()
   {
      Order o;
      o.id       = 10042;
      o.customer = UserProfile{.id = 77, .name = "Alice Stone",
                                .email = "alice@example.com",
                                .age = 34, .verified = true};
      o.items    = {
         LineItem{.product = "Widget",      .qty = 2, .unit_price = 9.99},
         LineItem{.product = "Sprocket",    .qty = 1, .unit_price = 14.50},
         LineItem{.product = "Gizmo-Mk.II", .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }

   inline ValidatorList vlist(std::uint32_t n = 100)
   {
      ValidatorList l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator(i));
      return l;
   }

   // ── Bounded factories ────────────────────────────────────────────
   inline FlatRecordBounded flatrec_bounded()
   {
      return {.id = 9, .label = "flat-cap",
              .values = {3, 5, 8, 13, 21, 34}};
   }

   inline RecordBounded record_bounded()
   {
      return {.id = 7, .label = "oracle",
              .values = {1, 2, 65535, 4096, 32768, 0}, .score = 99};
   }

   inline OrderBounded order_bounded()
   {
      OrderBounded o;
      o.id        = 10042;
      o.customer  = UserProfileBounded{.id = 77, .name = "Alice Stone",
                                        .email = "alice@example.com",
                                        .age = 34, .verified = true};
      o.items     = {
         LineItemBounded{.product = "Widget",
                          .qty = 2, .unit_price = 9.99},
         LineItemBounded{.product = "Sprocket",
                          .qty = 1, .unit_price = 14.50},
         LineItemBounded{.product = "Gizmo-Mk.II",
                          .qty = 5, .unit_price = 3.25},
      };
      o.total = 2 * 9.99 + 14.50 + 5 * 3.25;
      o.note  = std::string{"gift-wrap please"};
      return o;
   }

   inline ValidatorListBounded vlist_bounded(std::uint32_t n = 100)
   {
      ValidatorListBounded l;
      l.epoch = 42;
      l.validators.reserve(n);
      for (std::uint32_t i = 0; i < n; ++i)
         l.validators.push_back(make_validator(i));
      return l;
   }

}  // namespace psio_bench
