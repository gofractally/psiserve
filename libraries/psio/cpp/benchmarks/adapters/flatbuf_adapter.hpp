#pragma once
//
// adapters/flatbuf_adapter.hpp — drive libflatbuffers (Google's
// canonical FlatBuffers C++ library) on the unified bench shapes via
// the generated bench_schemas_generated.h.
//
// Construction is bottom-up: children (strings, vectors, sub-tables)
// must be created before their parent's CreateXxx() call so the
// builder knows their offsets.  These build_*() helpers encapsulate
// that pattern — caller passes a FlatBufferBuilder and a psio shape,
// gets back the root offset.

#include "bench_schemas_generated.h"

#include "../shapes.hpp"

#include <flatbuffers/flatbuffers.h>

namespace fb_bench_adapter {

   using fbs = flatbuffers::FlatBufferBuilder;

   inline flatbuffers::Offset<fb_bench::fb_Point>
   build(fbs& b, const Point& v)
   {
      return fb_bench::Createfb_Point(b, v.x, v.y);
   }

   inline flatbuffers::Offset<fb_bench::fb_NameRecord>
   build(fbs& b, const NameRecord& v)
   {
      return fb_bench::Createfb_NameRecord(b, v.account, v.limit);
   }

   inline flatbuffers::Offset<fb_bench::fb_FlatRecord>
   build(fbs& b, const FlatRecord& v)
   {
      auto label  = b.CreateString(v.label);
      std::vector<uint32_t> vals_v(v.values.begin(), v.values.end());
      auto values = b.CreateVector(vals_v);
      return fb_bench::Createfb_FlatRecord(b, v.id, label, values);
   }
   inline flatbuffers::Offset<fb_bench::fb_FlatRecord>
   build(fbs& b, const FlatRecordBounded& v)
   {
      auto label  = b.CreateString(v.label);
      auto values = b.CreateVector(v.values);
      return fb_bench::Createfb_FlatRecord(b, v.id, label, values);
   }

   inline flatbuffers::Offset<fb_bench::fb_Record>
   build(fbs& b, const Record& v)
   {
      auto label  = b.CreateString(v.label);
      std::vector<uint32_t> vals_v(v.values.begin(), v.values.end());
      auto values = b.CreateVector(vals_v);
      return fb_bench::Createfb_Record(b, v.id, label, values,
                                        v.score ? *v.score : 0u);
   }
   inline flatbuffers::Offset<fb_bench::fb_Record>
   build(fbs& b, const RecordBounded& v)
   {
      auto label  = b.CreateString(v.label);
      auto values = b.CreateVector(v.values);
      return fb_bench::Createfb_Record(b, v.id, label, values,
                                        v.score ? *v.score : 0u);
   }

   inline flatbuffers::Offset<fb_bench::fb_Validator>
   build(fbs& b, const Validator& v)
   {
      return fb_bench::Createfb_Validator(
         b, v.pubkey_lo, v.pubkey_hi, v.withdrawal_lo, v.withdrawal_hi,
         v.effective_balance, v.slashed, v.activation_epoch,
         v.exit_epoch, v.withdrawable_epoch);
   }

   inline flatbuffers::Offset<fb_bench::fb_LineItem>
   build(fbs& b, const LineItem& v)
   {
      auto product = b.CreateString(v.product);
      return fb_bench::Createfb_LineItem(b, product, v.qty, v.unit_price);
   }
   inline flatbuffers::Offset<fb_bench::fb_LineItem>
   build(fbs& b, const LineItemBounded& v)
   {
      auto product = b.CreateString(v.product);
      return fb_bench::Createfb_LineItem(b, product, v.qty, v.unit_price);
   }

   inline flatbuffers::Offset<fb_bench::fb_UserProfile>
   build(fbs& b, const UserProfile& v)
   {
      auto name  = b.CreateString(v.name);
      auto email = b.CreateString(v.email);
      return fb_bench::Createfb_UserProfile(b, v.id, name, email, v.age,
                                             v.verified);
   }
   inline flatbuffers::Offset<fb_bench::fb_UserProfile>
   build(fbs& b, const UserProfileBounded& v)
   {
      auto name  = b.CreateString(v.name);
      auto email = b.CreateString(v.email);
      return fb_bench::Createfb_UserProfile(b, v.id, name, email, v.age,
                                             v.verified);
   }

   inline flatbuffers::Offset<fb_bench::fb_Order>
   build(fbs& b, const Order& v)
   {
      auto customer = build(b, v.customer);
      std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>> items_v;
      items_v.reserve(v.items.size());
      for (const auto& it : v.items)
         items_v.push_back(build(b, it));
      auto items = b.CreateVector(items_v);
      flatbuffers::Offset<flatbuffers::String> note;
      if (v.note)
         note = b.CreateString(*v.note);
      return fb_bench::Createfb_Order(b, v.id, customer, items, v.total,
                                       note);
   }
   inline flatbuffers::Offset<fb_bench::fb_Order>
   build(fbs& b, const OrderBounded& v)
   {
      auto customer = build(b, v.customer);
      std::vector<flatbuffers::Offset<fb_bench::fb_LineItem>> items_v;
      items_v.reserve(v.items.size());
      for (const auto& it : v.items)
         items_v.push_back(build(b, it));
      auto items = b.CreateVector(items_v);
      flatbuffers::Offset<flatbuffers::String> note;
      if (v.note)
         note = b.CreateString(*v.note);
      return fb_bench::Createfb_Order(b, v.id, customer, items, v.total,
                                       note);
   }

   inline flatbuffers::Offset<fb_bench::fb_ValidatorList>
   build(fbs& b, const ValidatorList& v)
   {
      std::vector<flatbuffers::Offset<fb_bench::fb_Validator>> vs_v;
      vs_v.reserve(v.validators.size());
      for (const auto& vd : v.validators)
         vs_v.push_back(build(b, vd));
      auto validators = b.CreateVector(vs_v);
      return fb_bench::Createfb_ValidatorList(b, v.epoch, validators);
   }
   inline flatbuffers::Offset<fb_bench::fb_ValidatorList>
   build(fbs& b, const ValidatorListBounded& v)
   {
      std::vector<flatbuffers::Offset<fb_bench::fb_Validator>> vs_v;
      vs_v.reserve(v.validators.size());
      for (const auto& vd : v.validators)
         vs_v.push_back(build(b, vd));
      auto validators = b.CreateVector(vs_v);
      return fb_bench::Createfb_ValidatorList(b, v.epoch, validators);
   }

   //  Map each psio shape to its fb_bench root table type.
   template <typename Shape>
   struct fb_table_for;

   template <> struct fb_table_for<Point>                  { using type = fb_bench::fb_Point;          };
   template <> struct fb_table_for<NameRecord>             { using type = fb_bench::fb_NameRecord;     };
   template <> struct fb_table_for<FlatRecord>             { using type = fb_bench::fb_FlatRecord;     };
   template <> struct fb_table_for<FlatRecordBounded>      { using type = fb_bench::fb_FlatRecord;     };
   template <> struct fb_table_for<Record>                 { using type = fb_bench::fb_Record;         };
   template <> struct fb_table_for<RecordBounded>          { using type = fb_bench::fb_Record;         };
   template <> struct fb_table_for<Validator>              { using type = fb_bench::fb_Validator;      };
   template <> struct fb_table_for<Order>                  { using type = fb_bench::fb_Order;          };
   template <> struct fb_table_for<OrderBounded>           { using type = fb_bench::fb_Order;          };
   template <> struct fb_table_for<ValidatorList>          { using type = fb_bench::fb_ValidatorList;  };
   template <> struct fb_table_for<ValidatorListBounded>   { using type = fb_bench::fb_ValidatorList;  };

   template <typename Shape>
   using fb_table_t = typename fb_table_for<Shape>::type;

}  // namespace fb_bench_adapter
