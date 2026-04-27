#pragma once
//
// adapters/capnp_adapter.hpp — drive libcapnp (Sandstorm's Cap'n Proto
// C++ library) on the unified bench shapes via the generated
// bench_schemas.capnp.h.
//
// capnp is zero-copy: encode = MallocMessageBuilder + initRoot<T>() +
// setters + messageToFlatArray; decode = FlatArrayMessageReader +
// getRoot<T>() (just pointer arithmetic).  For a fair comparison
// against the streaming codecs we measure a "decode + walk" path
// that touches at least one field, since GetRoot alone optimises
// to nearly zero.

#include "bench_schemas.capnp.h"

#include "../shapes.hpp"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/array.h>

namespace cp_bench {

   //  build_<Shape>(builder, shape) — populate the capnp Builder
   //  using the schema's setX/initY methods.

   inline void build(CpPoint::Builder b, const Point& v)
   {
      b.setX(v.x);
      b.setY(v.y);
   }

   inline void build(CpNameRecord::Builder b, const NameRecord& v)
   {
      b.setAccount(v.account);
      b.setLimit(v.limit);
   }

   inline void build(CpFlatRecord::Builder b, const FlatRecord& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, v.values[i]);
   }
   inline void build(CpFlatRecord::Builder b, const FlatRecordBounded& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, static_cast<uint16_t>(v.values[i]));
   }

   inline void build(CpRecord::Builder b, const Record& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, v.values[i]);
      b.setScore(v.score ? *v.score : 0u);
   }
   inline void build(CpRecord::Builder b, const RecordBounded& v)
   {
      b.setId(v.id);
      b.setLabel(v.label);
      auto vs = b.initValues(static_cast<unsigned>(v.values.size()));
      for (unsigned i = 0; i < v.values.size(); ++i)
         vs.set(i, static_cast<uint16_t>(v.values[i]));
      b.setScore(v.score ? *v.score : 0u);
   }

   inline void build(CpValidator::Builder b, const Validator& v)
   {
      b.setPubkeyLo(v.pubkey_lo);
      b.setPubkeyHi(v.pubkey_hi);
      b.setWithdrawalLo(v.withdrawal_lo);
      b.setWithdrawalHi(v.withdrawal_hi);
      b.setEffectiveBalance(v.effective_balance);
      b.setSlashed(v.slashed);
      b.setActivationEpoch(v.activation_epoch);
      b.setExitEpoch(v.exit_epoch);
      b.setWithdrawableEpoch(v.withdrawable_epoch);
   }

   inline void build(CpLineItem::Builder b, const LineItem& v)
   {
      b.setProduct(v.product);
      b.setQty(v.qty);
      b.setUnitPrice(v.unit_price);
   }
   inline void build(CpLineItem::Builder b, const LineItemBounded& v)
   {
      b.setProduct(v.product);
      b.setQty(v.qty);
      b.setUnitPrice(v.unit_price);
   }

   inline void build(CpUserProfile::Builder b, const UserProfile& v)
   {
      b.setId(v.id);
      b.setName(v.name);
      b.setEmail(v.email);
      b.setAge(v.age);
      b.setVerified(v.verified);
   }
   inline void build(CpUserProfile::Builder b,
                     const UserProfileBounded& v)
   {
      b.setId(v.id);
      b.setName(v.name);
      b.setEmail(v.email);
      b.setAge(v.age);
      b.setVerified(v.verified);
   }

   inline void build(CpOrder::Builder b, const Order& v)
   {
      b.setId(v.id);
      build(b.initCustomer(), v.customer);
      auto its = b.initItems(static_cast<unsigned>(v.items.size()));
      for (unsigned i = 0; i < v.items.size(); ++i)
         build(its[i], v.items[i]);
      b.setTotal(v.total);
      if (v.note)
         b.setNote(*v.note);
   }
   inline void build(CpOrder::Builder b, const OrderBounded& v)
   {
      b.setId(v.id);
      build(b.initCustomer(), v.customer);
      auto its = b.initItems(static_cast<unsigned>(v.items.size()));
      for (unsigned i = 0; i < v.items.size(); ++i)
         build(its[i], v.items[i]);
      b.setTotal(v.total);
      if (v.note)
         b.setNote(*v.note);
   }

   inline void build(CpValidatorList::Builder b, const ValidatorList& v)
   {
      b.setEpoch(v.epoch);
      auto vs = b.initValidators(
         static_cast<unsigned>(v.validators.size()));
      for (unsigned i = 0; i < v.validators.size(); ++i)
         build(vs[i], v.validators[i]);
   }
   inline void build(CpValidatorList::Builder b,
                     const ValidatorListBounded& v)
   {
      b.setEpoch(v.epoch);
      auto vs = b.initValidators(
         static_cast<unsigned>(v.validators.size()));
      for (unsigned i = 0; i < v.validators.size(); ++i)
         build(vs[i], v.validators[i]);
   }

   //  Map each psio shape to its capnp struct type.
   template <typename Shape>
   struct cp_struct_for;

   template <> struct cp_struct_for<Point>                  { using type = CpPoint;          };
   template <> struct cp_struct_for<NameRecord>             { using type = CpNameRecord;     };
   template <> struct cp_struct_for<FlatRecord>             { using type = CpFlatRecord;     };
   template <> struct cp_struct_for<FlatRecordBounded>      { using type = CpFlatRecord;     };
   template <> struct cp_struct_for<Record>                 { using type = CpRecord;         };
   template <> struct cp_struct_for<RecordBounded>          { using type = CpRecord;         };
   template <> struct cp_struct_for<Validator>              { using type = CpValidator;      };
   template <> struct cp_struct_for<Order>                  { using type = CpOrder;          };
   template <> struct cp_struct_for<OrderBounded>           { using type = CpOrder;          };
   template <> struct cp_struct_for<ValidatorList>          { using type = CpValidatorList;  };
   template <> struct cp_struct_for<ValidatorListBounded>   { using type = CpValidatorList;  };

   template <typename Shape>
   using cp_struct_t = typename cp_struct_for<Shape>::type;

}  // namespace cp_bench
