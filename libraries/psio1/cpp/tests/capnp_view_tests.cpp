// capnp_view_tests.cpp — Catch2 tests for view<T, cp> (Cap'n Proto format)
//
// Uses a minimal inline builder to construct capnp wire data without
// depending on the official capnp library.

#include <catch2/catch.hpp>
#include <psio1/fracpack.hpp>  // for struct_tuple_t
#include <psio1/capnp_view.hpp>

#include <cstring>
#include <string>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct CpPoint
{
   double x;
   double y;
};
PSIO1_REFLECT(CpPoint, definitionWillNotChange(), x, y)

struct CpToken
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO1_REFLECT(CpToken, kind, offset, length, text)

struct CpUser
{
   uint64_t                 id;
   std::string              name;
   std::string              email;
   std::string              bio;
   uint32_t                 age;
   double                   score;
   std::vector<std::string> tags;
   bool                     verified;
};
PSIO1_REFLECT(CpUser, id, name, email, bio, age, score, tags, verified)

struct CpLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO1_REFLECT(CpLineItem, product, qty, unit_price)

struct CpOrder
{
   uint64_t                 id;
   CpUser                   customer;
   std::vector<CpLineItem>  items;
   double                   total;
   std::string              note;
};
PSIO1_REFLECT(CpOrder, id, customer, items, total, note)

// ── Minimal capnp message builder ──────────────────────────────────────────
//
// Builds a single-segment flat-array message word by word.  Good enough for
// tests; not intended as a general-purpose serializer.

class capnp_builder
{
   std::vector<uint64_t> words_;

  public:
   // Allocate n zero-filled words, return the starting word index.
   uint32_t alloc(uint32_t n)
   {
      uint32_t off = static_cast<uint32_t>(words_.size());
      words_.resize(off + n, 0);
      return off;
   }

   uint8_t* byte_ptr(uint32_t word_idx)
   {
      return reinterpret_cast<uint8_t*>(&words_[word_idx]);
   }

   // Write a struct pointer at word `at`, pointing to `target`.
   void set_struct_ptr(uint32_t at, uint32_t target, uint16_t dw, uint16_t pc)
   {
      int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
      uint64_t word = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                      (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu);
      words_[at] = word;
   }

   // Write a list pointer at word `at`, pointing to `target`.
   // elem_sz: 0=void, 1=bit, 2=byte, 3=2byte, 4=4byte, 5=8byte, 6=pointer, 7=composite
   void set_list_ptr(uint32_t at, uint32_t target, uint8_t elem_sz, uint32_t count)
   {
      int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
      uint64_t word = (uint64_t(count) << 35) | (uint64_t(elem_sz) << 32) |
                      (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu) | 1u;
      words_[at] = word;
   }

   // Write a composite list tag word (goes at the start of composite list data).
   void set_composite_tag(uint32_t at, uint32_t elem_count, uint16_t dw, uint16_t pc)
   {
      words_[at] = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                   (static_cast<uint32_t>(elem_count) << 2);
   }

   // Write a data field at byte offset within a word.
   template <typename T>
   void set_field(uint32_t struct_start, uint32_t byte_offset, T val)
   {
      std::memcpy(byte_ptr(struct_start) + byte_offset, &val, sizeof(val));
   }

   // Set a bool bit at byte_offset.bit_index within the struct.
   void set_bool(uint32_t struct_start, uint32_t byte_offset, uint8_t bit_idx, bool val)
   {
      auto* p = byte_ptr(struct_start) + byte_offset;
      if (val)
         *p |= (1u << bit_idx);
      else
         *p &= ~(1u << bit_idx);
   }

   // Write a Text (NUL-terminated string) and link it to ptr_word.
   // ptr_word is the absolute word index of the pointer slot.
   void write_text(uint32_t ptr_word, const char* text)
   {
      uint32_t len         = static_cast<uint32_t>(std::strlen(text));
      uint32_t total_bytes = len + 1;  // include NUL
      uint32_t n_words     = (total_bytes + 7) / 8;
      uint32_t target      = alloc(n_words);
      set_list_ptr(ptr_word, target, 2, total_bytes);  // elem_sz=2 (byte)
      std::memcpy(byte_ptr(target), text, len);
      byte_ptr(target)[len] = 0;
   }

   // Produce the flat-array message: [segment_table][segment_data]
   std::vector<uint8_t> finish()
   {
      uint32_t             seg_size = static_cast<uint32_t>(words_.size());
      std::vector<uint8_t> result(8 + seg_size * 8);
      uint32_t             zero = 0;
      std::memcpy(result.data(), &zero, 4);
      std::memcpy(result.data() + 4, &seg_size, 4);
      std::memcpy(result.data() + 8, words_.data(), seg_size * 8);
      return result;
   }
};

// ============================================================================
//  LAYOUT TESTS
// ============================================================================

TEST_CASE("cp layout: CpPoint all-data struct", "[view][cp]")
{
   using L = psio1::capnp_layout<CpPoint>;
   STATIC_REQUIRE(L::data_words == 2);
   STATIC_REQUIRE(L::ptr_count == 0);
   STATIC_REQUIRE(!L::loc(0).is_ptr);
   STATIC_REQUIRE(L::loc(0).offset == 0);
   STATIC_REQUIRE(!L::loc(1).is_ptr);
   STATIC_REQUIRE(L::loc(1).offset == 8);
}

TEST_CASE("cp layout: CpToken mixed data+pointer", "[view][cp]")
{
   using L = psio1::capnp_layout<CpToken>;
   // kind(u16)→offset 0, offset(u32)→offset 4, length(u32)→offset 8, text→ptr 0
   STATIC_REQUIRE(!L::loc(0).is_ptr);
   STATIC_REQUIRE(L::loc(0).offset == 0);
   STATIC_REQUIRE(!L::loc(1).is_ptr);
   STATIC_REQUIRE(L::loc(1).offset == 4);
   STATIC_REQUIRE(!L::loc(2).is_ptr);
   STATIC_REQUIRE(L::loc(2).offset == 8);
   STATIC_REQUIRE(L::loc(3).is_ptr);
   STATIC_REQUIRE(L::loc(3).offset == 0);
   STATIC_REQUIRE(L::ptr_count == 1);
}

TEST_CASE("cp layout: CpUser complex layout", "[view][cp]")
{
   using L = psio1::capnp_layout<CpUser>;
   STATIC_REQUIRE(L::data_words == 3);
   STATIC_REQUIRE(L::ptr_count == 4);
   // id at offset 0
   STATIC_REQUIRE(!L::loc(0).is_ptr && L::loc(0).offset == 0);
   // name = ptr 0
   STATIC_REQUIRE(L::loc(1).is_ptr && L::loc(1).offset == 0);
   // email = ptr 1
   STATIC_REQUIRE(L::loc(2).is_ptr && L::loc(2).offset == 1);
   // bio = ptr 2
   STATIC_REQUIRE(L::loc(3).is_ptr && L::loc(3).offset == 2);
   // age at offset 8
   STATIC_REQUIRE(!L::loc(4).is_ptr && L::loc(4).offset == 8);
   // score at offset 16
   STATIC_REQUIRE(!L::loc(5).is_ptr && L::loc(5).offset == 16);
   // tags = ptr 3
   STATIC_REQUIRE(L::loc(6).is_ptr && L::loc(6).offset == 3);
   // verified = bool at byte 12, bit 0
   STATIC_REQUIRE(!L::loc(7).is_ptr && L::loc(7).offset == 12 && L::loc(7).bit_index == 0);
}

// ============================================================================
//  VIEW READ TESTS
// ============================================================================

TEST_CASE("cp view: CpPoint read", "[view][cp]")
{
   // CpPoint layout: data_words=2, ptr_count=0
   // x at offset 0 (float64), y at offset 8 (float64)
   capnp_builder b;
   uint32_t root_ptr  = b.alloc(1);  // root struct pointer
   uint32_t data_start = b.alloc(2); // 2 data words
   b.set_struct_ptr(root_ptr, data_start, 2, 0);
   b.set_field(data_start, 0, 3.14);   // x
   b.set_field(data_start, 8, 2.72);   // y

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpPoint>::from_buffer(buf.data());

   REQUIRE(v);
   REQUIRE(v.x() == 3.14);
   REQUIRE(v.y() == 2.72);
}

TEST_CASE("cp view: CpToken mixed data + text", "[view][cp]")
{
   // CpToken layout: data_words=2, ptr_count=1
   // kind@0 (u16 at byte 0), offset@4 (u32 at byte 4), length@8 (u32 at byte 8)
   // text = ptr 0
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(2); // 2 data words
   uint32_t ptrs_start = b.alloc(1); // 1 pointer
   b.set_struct_ptr(root_ptr, data_start, 2, 1);
   b.set_field<uint16_t>(data_start, 0, 42);    // kind
   b.set_field<uint32_t>(data_start, 4, 1024);  // offset
   b.set_field<uint32_t>(data_start, 8, 15);    // length
   b.write_text(ptrs_start, "identifier_name");

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpToken>::from_buffer(buf.data());

   REQUIRE(v.kind() == 42);
   REQUIRE(v.offset() == 1024);
   REQUIRE(v.length() == 15);
   REQUIRE(v.text() == "identifier_name");
}

TEST_CASE("cp view: CpUser with bool and list of text", "[view][cp]")
{
   // CpUser layout: data_words=3, ptr_count=4
   // id@0, name=ptr0, email=ptr1, bio=ptr2, age@8, score@16, tags=ptr3, verified@byte12.bit0
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(3);  // 3 data words
   uint32_t ptrs_start = b.alloc(4);  // 4 pointers
   b.set_struct_ptr(root_ptr, data_start, 3, 4);

   // Data section
   b.set_field<uint64_t>(data_start, 0, 123456789ULL);  // id
   b.set_field<uint32_t>(data_start, 8, 32);             // age
   b.set_field<double>(data_start, 16, 98.5);            // score
   b.set_bool(data_start, 12, 0, true);                  // verified

   // Pointer section
   b.write_text(ptrs_start + 0, "Alice");       // name = ptr 0
   b.write_text(ptrs_start + 1, "alice@test");   // email = ptr 1
   b.write_text(ptrs_start + 2, "engineer");     // bio = ptr 2

   // tags = List(Text) = list of pointers → ptr 3
   uint32_t tag_list = b.alloc(2);  // 2 pointer entries
   b.set_list_ptr(ptrs_start + 3, tag_list, 6, 2);  // elem_sz=6 (pointer), count=2
   b.write_text(tag_list + 0, "developer");
   b.write_text(tag_list + 1, "wasm");

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpUser>::from_buffer(buf.data());

   REQUIRE(v.id() == 123456789ULL);
   REQUIRE(v.name() == "Alice");
   REQUIRE(v.email() == "alice@test");
   REQUIRE(v.bio() == "engineer");
   REQUIRE(v.age() == 32);
   REQUIRE(v.score() == 98.5);
   REQUIRE(v.verified() == true);

   auto tags = v.tags();
   REQUIRE(tags.size() == 2);
   REQUIRE(tags[0] == "developer");
   REQUIRE(tags[1] == "wasm");
}

TEST_CASE("cp view: iteration over list of text", "[view][cp]")
{
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(3);
   uint32_t ptrs_start = b.alloc(4);
   b.set_struct_ptr(root_ptr, data_start, 3, 4);

   b.set_field<uint64_t>(data_start, 0, 1);  // id

   b.write_text(ptrs_start + 0, "X");   // name
   b.write_text(ptrs_start + 1, "");     // email
   b.write_text(ptrs_start + 2, "");     // bio

   // tags = 3 text entries
   uint32_t tag_list = b.alloc(3);
   b.set_list_ptr(ptrs_start + 3, tag_list, 6, 3);
   b.write_text(tag_list + 0, "a");
   b.write_text(tag_list + 1, "b");
   b.write_text(tag_list + 2, "c");

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpUser>::from_buffer(buf.data());

   std::vector<std::string> collected;
   for (auto t : v.tags())
      collected.emplace_back(t);

   REQUIRE(collected.size() == 3);
   REQUIRE(collected[0] == "a");
   REQUIRE(collected[1] == "b");
   REQUIRE(collected[2] == "c");
}

TEST_CASE("cp view: composite list (list of structs)", "[view][cp]")
{
   // Build a CpOrder with 2 line items
   // CpOrder: data_words=2, ptr_count=3
   //   id@0 (u64), total@8 (f64)
   //   customer=ptr0, items=ptr1, note=ptr2
   //
   // CpLineItem: data_words=2, ptr_count=1
   //   qty@0 (u32), unit_price@8 (f64)
   //   product=ptr0

   using OrderLayout = psio1::capnp_layout<CpOrder>;
   using ItemLayout  = psio1::capnp_layout<CpLineItem>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(OrderLayout::data_words);
   uint32_t ptrs_start = b.alloc(OrderLayout::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, OrderLayout::data_words, OrderLayout::ptr_count);

   b.set_field<uint64_t>(data_start, 0, 999);   // id
   b.set_field<double>(data_start, 8, 54.47);    // total

   // customer (ptr 0) — minimal, just set id and name
   {
      using UL = psio1::capnp_layout<CpUser>;
      uint32_t cust_ptr  = ptrs_start + 0;
      uint32_t cust_data = b.alloc(UL::data_words);
      uint32_t cust_ptrs = b.alloc(UL::ptr_count);
      b.set_struct_ptr(cust_ptr, cust_data, UL::data_words, UL::ptr_count);
      b.set_field<uint64_t>(cust_data, 0, 42);   // id
      b.set_field<uint32_t>(cust_data, 8, 25);    // age
      b.set_field<double>(cust_data, 16, 77.5);   // score
      b.write_text(cust_ptrs + 0, "Bob");          // name
      b.write_text(cust_ptrs + 1, "bob@test.com"); // email
      b.write_text(cust_ptrs + 2, "");             // bio
      // tags: empty list of pointers
      b.set_list_ptr(cust_ptrs + 3, cust_ptrs + 3, 6, 0);  // null-ish empty list pointing at self
   }

   // items (ptr 1) — composite list of 2 CpLineItem
   // IMPORTANT: composite list elements must be contiguous in memory.
   // Allocate all element slots first, then write out-of-band text data.
   {
      uint32_t items_ptr = ptrs_start + 1;

      uint16_t idw       = ItemLayout::data_words;
      uint16_t ipc       = ItemLayout::ptr_count;
      uint32_t words_per = idw + ipc;

      uint32_t tag_word = b.alloc(1);
      b.set_composite_tag(tag_word, 2, idw, ipc);

      // Pre-allocate both elements contiguously
      uint32_t e0 = b.alloc(words_per);
      uint32_t e1 = b.alloc(words_per);

      // Fill data fields
      b.set_field<uint32_t>(e0, 0, 3);       // qty
      b.set_field<double>(e0, 8, 9.99);      // unit_price
      b.set_field<uint32_t>(e1, 0, 1);       // qty
      b.set_field<double>(e1, 8, 24.50);     // unit_price

      // Now write text (allocated after elements)
      b.write_text(e0 + idw, "Widget");
      b.write_text(e1 + idw, "Gadget");

      b.set_list_ptr(items_ptr, tag_word, 7, 2 * words_per);
   }

   // note (ptr 2)
   b.write_text(ptrs_start + 2, "Rush order");

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpOrder>::from_buffer(buf.data());

   REQUIRE(v.id() == 999);
   REQUIRE(v.total() == 54.47);
   REQUIRE(v.note() == "Rush order");

   // Nested customer
   auto vc = v.customer();
   REQUIRE(vc);
   REQUIRE(vc.id() == 42);
   REQUIRE(vc.name() == "Bob");
   REQUIRE(vc.email() == "bob@test.com");
   REQUIRE(vc.age() == 25);
   REQUIRE(vc.score() == 77.5);

   // List of structs
   auto items = v.items();
   REQUIRE(items.size() == 2);
   REQUIRE(items[0].product() == "Widget");
   REQUIRE(items[0].qty() == 3);
   REQUIRE(items[0].unit_price() == 9.99);
   REQUIRE(items[1].product() == "Gadget");
   REQUIRE(items[1].qty() == 1);
   REQUIRE(items[1].unit_price() == 24.50);
}

TEST_CASE("cp view: iteration over composite list", "[view][cp]")
{
   using OrderLayout = psio1::capnp_layout<CpOrder>;
   using ItemLayout  = psio1::capnp_layout<CpLineItem>;
   using UL          = psio1::capnp_layout<CpUser>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(OrderLayout::data_words);
   uint32_t ptrs_start = b.alloc(OrderLayout::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, OrderLayout::data_words, OrderLayout::ptr_count);
   b.set_field<uint64_t>(data_start, 0, 1);
   b.set_field<double>(data_start, 8, 0.0);

   // Minimal customer
   {
      uint32_t cd = b.alloc(UL::data_words);
      uint32_t cp = b.alloc(UL::ptr_count);
      b.set_struct_ptr(ptrs_start + 0, cd, UL::data_words, UL::ptr_count);
      b.write_text(cp + 0, "X");
      b.write_text(cp + 1, "");
      b.write_text(cp + 2, "");
      b.set_list_ptr(cp + 3, cp + 3, 6, 0);
   }

   // 3 items — pre-allocate all elements contiguously, then write text
   {
      uint16_t idw       = ItemLayout::data_words;
      uint16_t ipc       = ItemLayout::ptr_count;
      uint32_t words_per = idw + ipc;

      uint32_t tag = b.alloc(1);
      b.set_composite_tag(tag, 3, idw, ipc);

      // Allocate all 3 elements contiguously
      uint32_t elems[3];
      for (int i = 0; i < 3; ++i)
         elems[i] = b.alloc(words_per);

      // Fill data and write text after all elements are allocated
      for (int i = 0; i < 3; ++i)
      {
         b.set_field<uint32_t>(elems[i], 0, static_cast<uint32_t>(i + 1));
         b.set_field<double>(elems[i], 8, i * 10.0);
         std::string name = "P" + std::to_string(i);
         b.write_text(elems[i] + idw, name.c_str());
      }

      b.set_list_ptr(ptrs_start + 1, tag, 7, 3 * words_per);
   }

   // note
   b.write_text(ptrs_start + 2, "");

   auto buf   = b.finish();
   auto v     = psio1::capnp_view<CpOrder>::from_buffer(buf.data());
   int  count = 0;
   for (auto item : v.items())
   {
      REQUIRE(item.qty() == static_cast<uint32_t>(count + 1));
      ++count;
   }
   REQUIRE(count == 3);
}

// ============================================================================
//  DEFAULT VALUE XOR TESTS
// ============================================================================

// Struct with non-zero defaults — capnp XORs these into wire data
struct CpConfig
{
   uint32_t version  = 1;
   uint16_t port     = 8080;
   double   timeout  = 30.0;
   bool     verbose  = true;
};
PSIO1_REFLECT(CpConfig, version, port, timeout, verbose)

TEST_CASE("cp view: default value XOR", "[view][cp]")
{
   // CpConfig layout: version(u32)@0, port(u16)@4, timeout(f64)@8, verbose(bool)@6.bit0
   // (port is 2 bytes at offset 4, verbose gets packed into the gap at byte 6)
   using L = psio1::capnp_layout<CpConfig>;

   // Wire format stores actual_value XOR default_value.
   // For version=1:  actual=5, wire=5^1=4
   // For port=8080:  actual=9090, wire=9090^8080=0x23C2 (= 9134?)
   // For timeout=30.0: actual=60.0, wire = bitwise_xor(60.0, 30.0)
   // For verbose=true: actual=false, wire = false^true = true (bit set)

   uint32_t wire_version = 5u ^ 1u;             // 4
   uint16_t wire_port    = 9090u ^ 8080u;        // 1154
   bool     wire_verbose = false != true;         // true (bit is set)

   // Float XOR: interpret as uint64, XOR, reinterpret
   uint64_t bits_60, bits_30;
   double   val_60 = 60.0, val_30 = 30.0;
   std::memcpy(&bits_60, &val_60, 8);
   std::memcpy(&bits_30, &val_30, 8);
   uint64_t wire_bits = bits_60 ^ bits_30;
   double   wire_timeout;
   std::memcpy(&wire_timeout, &wire_bits, 8);

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<uint32_t>(data_start, L::loc(0).offset, wire_version);
   b.set_field<uint16_t>(data_start, L::loc(1).offset, wire_port);
   b.set_field<double>(data_start, L::loc(2).offset, wire_timeout);
   b.set_bool(data_start, L::loc(3).offset, L::loc(3).bit_index, wire_verbose);

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpConfig>::from_buffer(buf.data());

   REQUIRE(v.version() == 5);
   REQUIRE(v.port() == 9090);
   REQUIRE(v.timeout() == 60.0);
   REQUIRE(v.verbose() == false);
}

TEST_CASE("cp view: zero wire with non-zero defaults", "[view][cp]")
{
   // When wire data is all zeros, the actual values should be the defaults
   using L = psio1::capnp_layout<CpConfig>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   // All zeros — should read back as defaults

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpConfig>::from_buffer(buf.data());

   REQUIRE(v.version() == 1);
   REQUIRE(v.port() == 8080);
   REQUIRE(v.timeout() == 30.0);
   REQUIRE(v.verbose() == true);
}

// ============================================================================
//  DATA (vector<uint8_t>) TESTS
// ============================================================================

struct CpMessage
{
   uint64_t                 id;
   std::vector<uint8_t>     payload;
   std::string              tag;
};
PSIO1_REFLECT(CpMessage, id, payload, tag)

TEST_CASE("cp view: Data (vector<uint8_t>)", "[view][cp]")
{
   // CpMessage layout: id@0 (u64) → data offset 0
   // payload → ptr 0 (List(UInt8))
   // tag → ptr 1 (Text)
   using L = psio1::capnp_layout<CpMessage>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<uint64_t>(data_start, 0, 42);

   // payload: List(UInt8) with 5 bytes, no NUL terminator
   uint32_t payload_data = b.alloc(1);  // 5 bytes fits in 1 word
   b.set_list_ptr(ptrs_start + 0, payload_data, 2, 5);  // elem_sz=2 (byte), count=5
   auto* pp = b.byte_ptr(payload_data);
   pp[0] = 0xDE; pp[1] = 0xAD; pp[2] = 0xBE; pp[3] = 0xEF; pp[4] = 0x42;

   // tag: Text
   b.write_text(ptrs_start + 1, "test");

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpMessage>::from_buffer(buf.data());

   REQUIRE(v.id() == 42);
   REQUIRE(v.tag() == "test");

   auto p = v.payload();
   REQUIRE(p.size() == 5);
   REQUIRE(p[0] == 0xDE);
   REQUIRE(p[1] == 0xAD);
   REQUIRE(p[2] == 0xBE);
   REQUIRE(p[3] == 0xEF);
   REQUIRE(p[4] == 0x42);

   // Iteration
   std::vector<uint8_t> collected;
   for (auto byte : p)
      collected.push_back(byte);
   REQUIRE(collected == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF, 0x42});
}

// ============================================================================
//  UNION (std::variant) TESTS
// ============================================================================

// Matches the union_test.capnp schema:
//   struct Shape {
//     area @0 :Float64;
//     union { circle @1 :Float64; rectangle @2 :Text; nothing @3 :Void; }
//   }
struct CpShape
{
   double                                                 area;
   std::variant<double, std::string, std::monostate>      shape;
};
PSIO1_REFLECT(CpShape, area, shape)

TEST_CASE("cp layout: union discriminant placement", "[view][cp]")
{
   // Verified against official capnp compiler:
   // data_words=3, ptr_count=1
   // area @0: Float64 at data offset 0
   // circle @1: Float64 at data offset 8
   // rectangle @2: ptr 0
   // nothing @3: void (no space)
   // discriminant: uint16_t at data offset 16
   using L = psio1::capnp_layout<CpShape>;
   STATIC_REQUIRE(L::data_words == 3);
   STATIC_REQUIRE(L::ptr_count == 1);

   // area: regular field at offset 0
   STATIC_REQUIRE(!L::loc(0).is_ptr);
   STATIC_REQUIRE(L::loc(0).offset == 0);

   // shape is variant: loc(1) = discriminant location
   STATIC_REQUIRE(L::is_var(1));
   STATIC_REQUIRE(!L::loc(1).is_ptr);
   STATIC_REQUIRE(L::loc(1).offset == 16);  // discriminant at byte 16

   // Alternative locations
   STATIC_REQUIRE(L::alt_count(1) == 3);
   // circle (alt 0): Float64 at data offset 8
   STATIC_REQUIRE(!L::alt_loc(1, 0).is_ptr);
   STATIC_REQUIRE(L::alt_loc(1, 0).offset == 8);
   // rectangle (alt 1): pointer 0
   STATIC_REQUIRE(L::alt_loc(1, 1).is_ptr);
   STATIC_REQUIRE(L::alt_loc(1, 1).offset == 0);
   // nothing (alt 2): void — no space (offset is 0 but never read)
}

TEST_CASE("cp view: union circle variant", "[view][cp]")
{
   using L = psio1::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 100.0);    // area
   b.set_field<double>(data_start, 8, 5.0);      // circle (alt 0 data)
   b.set_field<uint16_t>(data_start, 16, 0);     // discriminant = 0 (circle)

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

   REQUIRE(v.area() == 100.0);
   auto shape = v.shape();
   REQUIRE(shape.index() == 0);
   REQUIRE(std::get<0>(shape) == 5.0);
}

TEST_CASE("cp view: union rectangle variant", "[view][cp]")
{
   using L = psio1::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 200.0);    // area
   b.set_field<uint16_t>(data_start, 16, 1);     // discriminant = 1 (rectangle)
   b.write_text(ptrs_start + 0, "4x3");           // rectangle text at ptr 0

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

   REQUIRE(v.area() == 200.0);
   auto shape = v.shape();
   REQUIRE(shape.index() == 1);
   REQUIRE(std::get<1>(shape) == "4x3");
}

TEST_CASE("cp view: union nothing (void) variant", "[view][cp]")
{
   using L = psio1::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 0.0);      // area
   b.set_field<uint16_t>(data_start, 16, 2);     // discriminant = 2 (nothing)

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

   auto shape = v.shape();
   REQUIRE(shape.index() == 2);
   REQUIRE(std::holds_alternative<std::monostate>(shape));
}

// ============================================================================
//  NESTED LIST (vector<vector<T>>) TESTS
// ============================================================================

struct CpMatrix
{
   uint32_t                             id;
   std::vector<std::vector<uint32_t>>   rows;
};
PSIO1_REFLECT(CpMatrix, id, rows)

TEST_CASE("cp view: nested list (vector<vector<uint32_t>>)", "[view][cp]")
{
   // CpMatrix layout: id @0 u32 → data offset 0, rows → ptr 0
   // rows is List(List(UInt32)): outer list is a list of pointers (elem_sz=6),
   // each pointer points to a List(UInt32) with elem_sz=4.
   using L = psio1::capnp_layout<CpMatrix>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<uint32_t>(data_start, 0, 7);  // id = 7

   // Outer list: 3 pointers (elem_sz=6 = pointer)
   uint32_t outer = b.alloc(3);  // 3 words, one pointer each
   b.set_list_ptr(ptrs_start + 0, outer, 6, 3);  // elem_sz=6 (pointer), 3 elements

   // Row 0: [10, 20, 30] — 3 uint32_t = 12 bytes = 2 words
   {
      uint32_t inner = b.alloc(2);
      b.set_list_ptr(outer + 0, inner, 4, 3);  // elem_sz=4 (4 byte), 3 elements
      b.set_field<uint32_t>(inner, 0, 10);
      b.set_field<uint32_t>(inner, 4, 20);
      b.set_field<uint32_t>(inner, 8, 30);
   }

   // Row 1: [100, 200] — 2 uint32_t = 8 bytes = 1 word
   {
      uint32_t inner = b.alloc(1);
      b.set_list_ptr(outer + 1, inner, 4, 2);
      b.set_field<uint32_t>(inner, 0, 100);
      b.set_field<uint32_t>(inner, 4, 200);
   }

   // Row 2: [] — empty list (null pointer)
   // outer + 2 is already 0 (null), so empty

   auto buf = b.finish();
   auto v   = psio1::capnp_view<CpMatrix>::from_buffer(buf.data());

   REQUIRE(v.id() == 7);

   auto rows = v.rows();
   REQUIRE(rows.size() == 3);

   // Row 0
   auto r0 = rows[0];
   REQUIRE(r0.size() == 3);
   REQUIRE(r0[0] == 10);
   REQUIRE(r0[1] == 20);
   REQUIRE(r0[2] == 30);

   // Row 1
   auto r1 = rows[1];
   REQUIRE(r1.size() == 2);
   REQUIRE(r1[0] == 100);
   REQUIRE(r1[1] == 200);

   // Row 2 (empty/null)
   auto r2 = rows[2];
   REQUIRE(r2.size() == 0);
   REQUIRE(r2.empty());

   // Iterate outer
   uint32_t total = 0;
   for (auto row : rows)
      for (auto val : row)
         total += val;
   REQUIRE(total == 360);
}

// ============================================================================
//  TYPE IDENTITY & ITERATOR CONCEPT TESTS
// ============================================================================

// ============================================================================
//  UNPACK TESTS
// ============================================================================

TEST_CASE("cp unpack: simple struct round-trip", "[view][cp]")
{
   // Build wire data for CpPoint manually, then unpack
   using L = psio1::capnp_layout<CpPoint>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   b.set_field<double>(data_start, 0, 3.14);
   b.set_field<double>(data_start, 8, 2.72);

   auto    buf = b.finish();
   CpPoint pt  = psio1::capnp_unpack<CpPoint>(buf.data());
   REQUIRE(pt.x == 3.14);
   REQUIRE(pt.y == 2.72);
}

TEST_CASE("cp unpack: struct with strings and vectors", "[view][cp]")
{
   // Build CpMessage wire data, unpack
   using L = psio1::capnp_layout<CpMessage>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<uint64_t>(data_start, 0, 42);

   // payload: [0xDE, 0xAD]
   uint32_t pd = b.alloc(1);
   b.set_list_ptr(ptrs_start + 0, pd, 2, 2);
   b.byte_ptr(pd)[0] = 0xDE;
   b.byte_ptr(pd)[1] = 0xAD;

   b.write_text(ptrs_start + 1, "hello");

   auto buf = b.finish();
   auto msg = psio1::capnp_unpack<CpMessage>(buf.data());

   REQUIRE(msg.id == 42);
   REQUIRE(msg.tag == "hello");
   REQUIRE(msg.payload.size() == 2);
   REQUIRE(msg.payload[0] == 0xDE);
   REQUIRE(msg.payload[1] == 0xAD);
}

TEST_CASE("cp unpack: union variant", "[view][cp]")
{
   using L = psio1::capnp_layout<CpShape>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 50.0);
   b.set_field<uint16_t>(data_start, 16, 1);  // discriminant = rectangle
   b.write_text(ptrs_start + 0, "5x10");

   auto buf = b.finish();
   auto s   = psio1::capnp_unpack<CpShape>(buf.data());

   REQUIRE(s.area == 50.0);
   REQUIRE(s.shape.index() == 1);
   REQUIRE(std::get<1>(s.shape) == "5x10");
}

TEST_CASE("cp unpack: default values restored", "[view][cp]")
{
   using L = psio1::capnp_layout<CpConfig>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   // All zeros → defaults

   auto buf = b.finish();
   auto cfg = psio1::capnp_unpack<CpConfig>(buf.data());

   REQUIRE(cfg.version == 1);
   REQUIRE(cfg.port == 8080);
   REQUIRE(cfg.timeout == 30.0);
   REQUIRE(cfg.verbose == true);
}

// ============================================================================
//  PACK TESTS
// ============================================================================

TEST_CASE("cp pack: simple struct", "[view][cp]")
{
   CpPoint pt{3.14, 2.72};
   auto    buf = psio1::capnp_pack(pt);

   // Read back through view
   auto v = psio1::capnp_view<CpPoint>::from_buffer(buf.data());
   REQUIRE(v.x() == 3.14);
   REQUIRE(v.y() == 2.72);

   // And unpack round-trip
   auto pt2 = psio1::capnp_unpack<CpPoint>(buf.data());
   REQUIRE(pt2.x == 3.14);
   REQUIRE(pt2.y == 2.72);
}

TEST_CASE("cp pack: struct with string and data", "[view][cp]")
{
   CpMessage msg;
   msg.id      = 99;
   msg.payload = {0x01, 0x02, 0x03};
   msg.tag     = "packed";

   auto buf = psio1::capnp_pack(msg);
   auto v   = psio1::capnp_view<CpMessage>::from_buffer(buf.data());

   REQUIRE(v.id() == 99);
   REQUIRE(v.tag() == "packed");
   auto p = v.payload();
   REQUIRE(p.size() == 3);
   REQUIRE(p[0] == 0x01);
   REQUIRE(p[1] == 0x02);
   REQUIRE(p[2] == 0x03);
}

TEST_CASE("cp pack: struct with list of text", "[view][cp]")
{
   CpUser user;
   user.id       = 1;
   user.name     = "Alice";
   user.email    = "alice@test.com";
   user.bio      = "A programmer";
   user.age      = 30;
   user.score    = 95.5;
   user.tags     = {"c++", "rust", "go"};
   user.verified = true;

   auto buf = psio1::capnp_pack(user);
   auto v   = psio1::capnp_view<CpUser>::from_buffer(buf.data());

   REQUIRE(v.id() == 1);
   REQUIRE(v.name() == "Alice");
   REQUIRE(v.email() == "alice@test.com");
   REQUIRE(v.bio() == "A programmer");
   REQUIRE(v.age() == 30);
   REQUIRE(v.score() == 95.5);
   REQUIRE(v.verified() == true);

   auto tags = v.tags();
   REQUIRE(tags.size() == 3);
   REQUIRE(tags[0] == "c++");
   REQUIRE(tags[1] == "rust");
   REQUIRE(tags[2] == "go");
}

TEST_CASE("cp pack: nested struct", "[view][cp]")
{
   CpOrder order;
   order.id   = 100;
   order.total = 29.99;
   order.note  = "test order";
   order.customer.id    = 1;
   order.customer.name  = "Bob";
   order.customer.email = "bob@x.com";
   order.customer.bio   = "";
   order.customer.age   = 40;
   order.customer.score = 80.0;
   order.customer.tags  = {"admin"};
   order.customer.verified = false;
   order.items = {
       {"Widget", 2, 10.00},
       {"Gadget", 1, 9.99}
   };

   auto buf = psio1::capnp_pack(order);
   auto v   = psio1::capnp_view<CpOrder>::from_buffer(buf.data());

   REQUIRE(v.id() == 100);
   REQUIRE(v.total() == 29.99);
   REQUIRE(v.note() == "test order");

   auto cust = v.customer();
   REQUIRE(cust.name() == "Bob");
   REQUIRE(cust.age() == 40);

   auto items = v.items();
   REQUIRE(items.size() == 2);
   REQUIRE(items[0].product() == "Widget");
   REQUIRE(items[0].qty() == 2);
   REQUIRE(items[1].product() == "Gadget");
   REQUIRE(items[1].unit_price() == 9.99);
}

TEST_CASE("cp pack: union variants", "[view][cp]")
{
   // Circle variant
   {
      CpShape shape;
      shape.area  = 100.0;
      shape.shape = 5.0;  // circle (double, index 0)

      auto buf = psio1::capnp_pack(shape);
      auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

      REQUIRE(v.area() == 100.0);
      auto s = v.shape();
      REQUIRE(s.index() == 0);
      REQUIRE(std::get<0>(s) == 5.0);
   }

   // Rectangle variant (string)
   {
      CpShape shape;
      shape.area  = 200.0;
      shape.shape = std::string("4x3");

      auto buf = psio1::capnp_pack(shape);
      auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

      REQUIRE(v.area() == 200.0);
      auto s = v.shape();
      REQUIRE(s.index() == 1);
      REQUIRE(std::get<1>(s) == "4x3");
   }

   // Nothing variant (monostate)
   {
      CpShape shape;
      shape.area  = 0.0;
      shape.shape = std::monostate{};

      auto buf = psio1::capnp_pack(shape);
      auto v   = psio1::capnp_view<CpShape>::from_buffer(buf.data());

      auto s = v.shape();
      REQUIRE(s.index() == 2);
      REQUIRE(std::holds_alternative<std::monostate>(s));
   }
}

TEST_CASE("cp pack: default XOR", "[view][cp]")
{
   CpConfig cfg;
   cfg.version = 5;
   cfg.port    = 9090;
   cfg.timeout = 60.0;
   cfg.verbose = false;

   auto buf = psio1::capnp_pack(cfg);
   auto v   = psio1::capnp_view<CpConfig>::from_buffer(buf.data());

   REQUIRE(v.version() == 5);
   REQUIRE(v.port() == 9090);
   REQUIRE(v.timeout() == 60.0);
   REQUIRE(v.verbose() == false);

   // All-default values should also round-trip
   CpConfig def{};
   auto buf2 = psio1::capnp_pack(def);
   auto v2   = psio1::capnp_view<CpConfig>::from_buffer(buf2.data());

   REQUIRE(v2.version() == 1);
   REQUIRE(v2.port() == 8080);
   REQUIRE(v2.timeout() == 30.0);
   REQUIRE(v2.verbose() == true);
}

TEST_CASE("cp pack: nested list", "[view][cp]")
{
   CpMatrix m;
   m.id   = 42;
   m.rows = {{1, 2, 3}, {10, 20}, {}};

   auto buf = psio1::capnp_pack(m);
   auto v   = psio1::capnp_view<CpMatrix>::from_buffer(buf.data());

   REQUIRE(v.id() == 42);
   auto rows = v.rows();
   REQUIRE(rows.size() == 3);
   REQUIRE(rows[0].size() == 3);
   REQUIRE(rows[0][0] == 1);
   REQUIRE(rows[0][2] == 3);
   REQUIRE(rows[1].size() == 2);
   REQUIRE(rows[1][0] == 10);
   REQUIRE(rows[2].size() == 0);
}

TEST_CASE("cp pack+unpack: full round-trip", "[view][cp]")
{
   CpOrder order;
   order.id   = 777;
   order.total = 123.45;
   order.note  = "round-trip test";
   order.customer.id    = 10;
   order.customer.name  = "Charlie";
   order.customer.email = "charlie@example.com";
   order.customer.bio   = "Loves coding";
   order.customer.age   = 35;
   order.customer.score = 99.9;
   order.customer.tags  = {"dev", "lead"};
   order.customer.verified = true;
   order.items = {
       {"Alpha", 5, 1.50},
       {"Beta",  3, 2.25},
       {"Gamma", 1, 100.00}
   };

   auto    buf  = psio1::capnp_pack(order);
   CpOrder out  = psio1::capnp_unpack<CpOrder>(buf.data());

   REQUIRE(out.id == 777);
   REQUIRE(out.total == 123.45);
   REQUIRE(out.note == "round-trip test");
   REQUIRE(out.customer.name == "Charlie");
   REQUIRE(out.customer.email == "charlie@example.com");
   REQUIRE(out.customer.bio == "Loves coding");
   REQUIRE(out.customer.age == 35);
   REQUIRE(out.customer.score == 99.9);
   REQUIRE(out.customer.tags.size() == 2);
   REQUIRE(out.customer.tags[0] == "dev");
   REQUIRE(out.customer.tags[1] == "lead");
   REQUIRE(out.customer.verified == true);
   REQUIRE(out.items.size() == 3);
   REQUIRE(out.items[0].product == "Alpha");
   REQUIRE(out.items[0].qty == 5);
   REQUIRE(out.items[0].unit_price == 1.50);
   REQUIRE(out.items[2].product == "Gamma");
   REQUIRE(out.items[2].unit_price == 100.00);
}

// ============================================================================
//  VALIDATE TESTS
// ============================================================================

TEST_CASE("cp validate: valid packed messages", "[view][cp]")
{
   // Simple struct
   {
      CpPoint pt{1.0, 2.0};
      auto    buf = psio1::capnp_pack(pt);
      REQUIRE(psio1::validate_capnp(buf.data(), buf.size()));
   }
   // Complex struct with strings, lists, nested structs
   {
      CpOrder order;
      order.id    = 1;
      order.total = 10.0;
      order.note  = "valid";
      order.customer.id   = 1;
      order.customer.name = "X";
      order.customer.email = "";
      order.customer.bio   = "";
      order.customer.age   = 20;
      order.customer.score = 50.0;
      order.customer.tags  = {"a", "b"};
      order.customer.verified = false;
      order.items = {{"P", 1, 5.0}};

      auto buf = psio1::capnp_pack(order);
      REQUIRE(psio1::validate_capnp(buf.data(), buf.size()));
   }
}

TEST_CASE("cp validate: too-short buffer", "[view][cp]")
{
   REQUIRE_FALSE(psio1::validate_capnp(nullptr, 0));

   uint8_t small[4] = {};
   REQUIRE_FALSE(psio1::validate_capnp(small, 4));
}

TEST_CASE("cp validate: cyclic pointer detected", "[view][cp]")
{
   // Craft a message where a struct pointer points back to itself.
   // The word budget should catch this.
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);  // word 0: root pointer
   uint32_t data_start = b.alloc(1);  // word 1: 1 data word
   uint32_t ptr_slot   = b.alloc(1);  // word 2: 1 pointer

   b.set_struct_ptr(root_ptr, data_start, 1, 1);

   // Make the struct's pointer slot point back to itself (the root struct)
   // This creates a cycle: root → root → root → ...
   b.set_struct_ptr(ptr_slot, data_start, 1, 1);

   auto buf = b.finish();
   // The message is small (3 words of segment), budget = 3*8 = 24 words.
   // Each visit costs 2 words (1 data + 1 ptr), so it'll loop ~12 times
   // before the budget runs out.
   REQUIRE_FALSE(psio1::validate_capnp(buf.data(), buf.size()));
}

TEST_CASE("cp validate: truncated segment", "[view][cp]")
{
   CpPoint pt{1.0, 2.0};
   auto    buf = psio1::capnp_pack(pt);
   // Truncate: claim full size but only provide half
   REQUIRE_FALSE(psio1::validate_capnp(buf.data(), buf.size() / 2));
}

// ============================================================================
//  TYPE IDENTITY & ITERATOR CONCEPT TESTS
// ============================================================================

TEST_CASE("cp type alias identity", "[view][cp]")
{
   STATIC_REQUIRE(std::is_same_v<psio1::capnp_view<CpOrder>, psio1::view<CpOrder, psio1::cp>>);
}

TEST_CASE("cp vec_view iterator is random access", "[view][cp]")
{
   using iter = psio1::vec_view<CpLineItem, psio1::cp>::iterator;
   STATIC_REQUIRE(std::random_access_iterator<iter>);
}

// ============================================================================
//  MUTATION TESTS (capnp_ref)
// ============================================================================

struct CpDefaults
{
   int32_t count = 10;
   double  ratio = 2.5;
};
PSIO1_REFLECT(CpDefaults, count, ratio)

TEST_CASE("cp ref: read scalar fields", "[ref][cp]")
{
   CpToken tok{3, 100, 42, "hello"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   uint16_t k = f.kind();
   uint32_t o = f.offset();
   uint32_t l = f.length();

   REQUIRE(k == 3);
   REQUIRE(o == 100);
   REQUIRE(l == 42);

   std::string_view t = f.text();
   REQUIRE(t == "hello");
}

TEST_CASE("cp ref: mutate scalar field", "[ref][cp]")
{
   CpToken tok{3, 100, 42, "hello"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   // Verify original
   REQUIRE(uint16_t(f.kind()) == 3);
   REQUIRE(uint32_t(f.offset()) == 100);

   // Mutate
   f.kind()   = uint16_t(7);
   f.offset() = uint32_t(999);

   // Verify mutation
   REQUIRE(uint16_t(f.kind()) == 7);
   REQUIRE(uint32_t(f.offset()) == 999);

   // Verify via unpack round-trip
   auto unpacked = ref.unpack();
   REQUIRE(unpacked.kind == 7);
   REQUIRE(unpacked.offset == 999);
   REQUIRE(unpacked.length == 42);   // unchanged
   REQUIRE(unpacked.text == "hello");  // unchanged
}

TEST_CASE("cp ref: mutate string field", "[ref][cp]")
{
   CpToken tok{1, 0, 0, "old"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   REQUIRE(std::string_view(f.text()) == "old");

   // Mutate the string
   f.text() = "new and longer string";

   // Verify via view
   REQUIRE(std::string_view(f.text()) == "new and longer string");

   // Verify via unpack
   auto unpacked = ref.unpack();
   REQUIRE(unpacked.text == "new and longer string");
   REQUIRE(unpacked.kind == 1);  // unchanged

   // Validate the mutated message
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate string to empty", "[ref][cp]")
{
   CpToken tok{1, 0, 0, "hello"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   f.text() = "";

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.text.empty());
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate vector field", "[ref][cp]")
{
   CpUser user{};
   user.id   = 1;
   user.name = "test";
   user.tags = {"a", "b"};
   auto data = psio1::capnp_pack(user);

   psio1::capnp_ref<CpUser> ref(std::move(data));
   auto                    f = ref.fields();

   // Read original
   auto tags = f.tags().get();
   REQUIRE(tags.size() == 2);

   // Mutate to larger vector
   f.tags() = std::vector<std::string>{"x", "y", "z", "w"};

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.tags.size() == 4);
   REQUIRE(unpacked.tags[0] == "x");
   REQUIRE(unpacked.tags[1] == "y");
   REQUIRE(unpacked.tags[2] == "z");
   REQUIRE(unpacked.tags[3] == "w");
   REQUIRE(unpacked.id == 1);    // unchanged
   REQUIRE(unpacked.name == "test");  // unchanged
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate vector to empty", "[ref][cp]")
{
   CpUser user{};
   user.tags = {"a", "b"};
   auto data = psio1::capnp_pack(user);

   psio1::capnp_ref<CpUser> ref(std::move(data));
   ref.fields().tags() = std::vector<std::string>{};

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.tags.empty());
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: drill into nested struct", "[ref][cp]")
{
   CpOrder order{};
   order.id            = 42;
   order.customer.id   = 7;
   order.customer.name = "Alice";
   order.customer.age  = 30;
   order.total         = 99.99;
   auto data = psio1::capnp_pack(order);

   psio1::capnp_ref<CpOrder> ref(std::move(data));
   auto                     f = ref.fields();

   // Read through nested proxy
   uint64_t cid = f.customer().id();
   REQUIRE(cid == 7);

   std::string_view cname = f.customer().name();
   REQUIRE(cname == "Alice");

   uint32_t cage = f.customer().age();
   REQUIRE(cage == 30);

   // Mutate nested scalar
   f.customer().age() = uint32_t(31);
   REQUIRE(uint32_t(f.customer().age()) == 31);

   // Mutate nested string
   f.customer().name() = "Bob";
   REQUIRE(std::string_view(f.customer().name()) == "Bob");

   // Verify top-level unchanged
   REQUIRE(uint64_t(f.id()) == 42);

   // Full round-trip
   auto unpacked = ref.unpack();
   REQUIRE(unpacked.id == 42);
   REQUIRE(unpacked.customer.id == 7);
   REQUIRE(unpacked.customer.name == "Bob");
   REQUIRE(unpacked.customer.age == 31);
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate variant field", "[ref][cp]")
{
   // Start with circle variant (double)
   CpShape shape{};
   shape.area  = 10.0;
   shape.shape = 3.14;  // circle radius
   auto data = psio1::capnp_pack(shape);

   psio1::capnp_ref<CpShape> ref(std::move(data));
   auto                     f = ref.fields();

   // Read original
   REQUIRE(double(f.area()) == Approx(10.0));
   auto var = f.shape().get();
   REQUIRE(var.index() == 0);
   REQUIRE(std::get<0>(var) == Approx(3.14));

   // Mutate to string variant (rectangle description)
   f.shape() = std::variant<double, std::string, std::monostate>{
       std::string("wide rectangle")};

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.area == Approx(10.0));
   REQUIRE(unpacked.shape.index() == 1);
   REQUIRE(std::get<1>(unpacked.shape) == "wide rectangle");
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate variant to void", "[ref][cp]")
{
   CpShape shape{};
   shape.area  = 5.0;
   shape.shape = 1.0;  // circle
   auto data = psio1::capnp_pack(shape);

   psio1::capnp_ref<CpShape> ref(std::move(data));
   auto                     f = ref.fields();

   // Mutate to monostate (void)
   f.shape() = std::variant<double, std::string, std::monostate>{
       std::monostate{}};

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.shape.index() == 2);
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate nested struct fields individually", "[ref][cp]")
{
   CpOrder order{};
   order.id            = 1;
   order.customer.id   = 5;
   order.customer.name = "old";
   order.customer.age  = 20;
   auto data = psio1::capnp_pack(order);

   psio1::capnp_ref<CpOrder> ref(std::move(data));
   auto                     f = ref.fields();

   // Replace individual fields via drill-in
   f.customer().id()   = uint64_t(99);
   f.customer().name() = "New Customer";
   f.customer().age()  = uint32_t(25);

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.id == 1);  // unchanged
   REQUIRE(unpacked.customer.id == 99);
   REQUIRE(unpacked.customer.name == "New Customer");
   REQUIRE(unpacked.customer.age == 25);
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: mutate scalar vector field", "[ref][cp]")
{
   CpMatrix mat{};
   mat.id   = 1;
   mat.rows = {{1, 2, 3}, {4, 5, 6}};
   auto data = psio1::capnp_pack(mat);

   psio1::capnp_ref<CpMatrix> ref(std::move(data));

   // Replace with different data
   ref.fields().rows() =
       std::vector<std::vector<uint32_t>>{{10, 20}, {30, 40}, {50, 60}};

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.id == 1);
   REQUIRE(unpacked.rows.size() == 3);
   REQUIRE(unpacked.rows[0] == std::vector<uint32_t>{10, 20});
   REQUIRE(unpacked.rows[1] == std::vector<uint32_t>{30, 40});
   REQUIRE(unpacked.rows[2] == std::vector<uint32_t>{50, 60});
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: multiple mutations compound correctly", "[ref][cp]")
{
   CpToken tok{1, 10, 20, "start"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   // Multiple scalar mutations
   f.kind()   = uint16_t(5);
   f.offset() = uint32_t(50);
   f.length() = uint32_t(100);

   // String mutation
   f.text() = "after first mutation";

   // Another string mutation (creates more dead space)
   f.text() = "final value";

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.kind == 5);
   REQUIRE(unpacked.offset == 50);
   REQUIRE(unpacked.length == 100);
   REQUIRE(unpacked.text == "final value");
   REQUIRE(ref.validate());
}

TEST_CASE("cp ref: as_view returns valid view", "[ref][cp]")
{
   CpToken tok{3, 100, 42, "hello"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     v = ref.as_view();

   REQUIRE(v.kind() == 3);
   REQUIRE(v.offset() == 100);
   REQUIRE(v.text() == "hello");
}

TEST_CASE("cp ref: bool mutation", "[ref][cp]")
{
   CpUser user{};
   user.verified = false;
   auto data = psio1::capnp_pack(user);

   psio1::capnp_ref<CpUser> ref(std::move(data));
   auto                    f = ref.fields();

   REQUIRE(bool(f.verified()) == false);

   f.verified() = true;
   REQUIRE(bool(f.verified()) == true);

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.verified == true);
}

TEST_CASE("cp ref: default-value XOR round-trip", "[ref][cp]")
{
   // CpDefaults has non-zero defaults: count=10, ratio=2.5
   CpDefaults d{};
   d.count = 10;  // equal to default
   d.ratio = 2.5;
   auto data = psio1::capnp_pack(d);

   psio1::capnp_ref<CpDefaults> ref(std::move(data));
   auto                        f = ref.fields();

   // Read back — should get the original values
   REQUIRE(int32_t(f.count()) == 10);
   REQUIRE(double(f.ratio()) == Approx(2.5));

   // Mutate to non-default value
   f.count() = int32_t(42);
   f.ratio()  = 7.77;

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.count == 42);
   REQUIRE(unpacked.ratio == Approx(7.77));
}

// ── Free-list allocator tests ──────────────────────────────────────────────

TEST_CASE("cp ref: repeated string mutation reuses space", "[ref][cp][freelist]")
{
   CpToken tok{42, 100, 5, "hello"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   size_t size_after_pack = ref.size();

   auto f = ref.fields();

   // Mutate string to same-length string many times
   for (int i = 0; i < 100; ++i)
      f.text() = "world";

   // Size should NOT have grown 100x — the free list reclaims the old strings
   // Original "hello" is 6 bytes (incl NUL) → 1 word.
   // "world" is also 1 word. So each mutation frees 1 word and reuses it.
   size_t size_after_mutations = ref.size();
   INFO("size after pack: " << size_after_pack
                            << " after 100 mutations: " << size_after_mutations);
   REQUIRE(size_after_mutations == size_after_pack);

   // Verify the value is correct
   auto unpacked = ref.unpack();
   REQUIRE(unpacked.text == "world");
}

TEST_CASE("cp ref: string mutation to longer string grows once", "[ref][cp][freelist]")
{
   CpToken tok{1, 2, 3, "hi"};
   auto    data = psio1::capnp_pack(tok);

   psio1::capnp_ref<CpToken> ref(std::move(data));
   auto                     f = ref.fields();

   // Mutate to a much longer string — must grow segment
   f.text() = "a longer string value";
   size_t size_after_grow = ref.size();

   // Mutate again with same-length string — should reuse the block
   for (int i = 0; i < 50; ++i)
      f.text() = "another long str value";  // same length as above (21 chars)

   REQUIRE(ref.size() == size_after_grow);
   REQUIRE(ref.unpack().text == "another long str value");
}

TEST_CASE("cp ref: repeated vector mutation reuses space", "[ref][cp][freelist]")
{
   CpUser user{1, "name", "email", "bio", 25, 3.14, {"a", "b", "c"}, false};
   auto   data = psio1::capnp_pack(user);

   psio1::capnp_ref<CpUser> ref(std::move(data));
   auto                    f = ref.fields();

   // Mutate with same-structure vectors repeatedly
   for (int i = 0; i < 50; ++i)
      f.tags() = std::vector<std::string>{"x", "y", "z"};

   // Verify correctness
   auto unpacked = ref.unpack();
   REQUIRE(unpacked.tags.size() == 3);
   REQUIRE(unpacked.tags[0] == "x");
   REQUIRE(unpacked.tags[2] == "z");

   // The free list should have reclaimed space from old vectors
   REQUIRE(ref.free_list().total_free() <= 10);  // bounded internal waste
}

TEST_CASE("cp ref: variant mutation frees old pointer data", "[ref][cp][freelist]")
{
   CpShape shape{99.0, std::string("triangle")};
   auto    data = psio1::capnp_pack(shape);

   psio1::capnp_ref<CpShape> ref(std::move(data));
   size_t initial_size = ref.size();
   auto   f            = ref.fields();

   // Change from string to double (pointer → scalar)
   using shape_var = std::variant<double, std::string, std::monostate>;
   f.shape() = shape_var(3.14);
   // Old string should be freed
   REQUIRE(ref.free_list().total_free() > 0);

   // Change back to string — should reuse freed space
   f.shape() = shape_var(std::string("square"));

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.area == Approx(99.0));
   REQUIRE(std::get<std::string>(unpacked.shape) == "square");
}

TEST_CASE("cp ref: free list coalesces adjacent blocks", "[ref][cp][freelist]")
{
   // Create a struct with multiple string fields to test coalescing
   CpUser user{1, "Alice", "alice@example.com", "", 30, 5.0, {}, false};
   auto   data = psio1::capnp_pack(user);

   psio1::capnp_ref<CpUser> ref(std::move(data));
   auto                    f = ref.fields();

   // Set both name and email to empty — frees both string allocations
   f.name()  = "";
   f.email() = "";

   // Both freed blocks should be tracked (may or may not coalesce depending
   // on whether they were adjacent in the segment)
   REQUIRE(ref.free_list().total_free() > 0);

   // Set them back — should reuse freed space
   f.name()  = "Bob";
   f.email() = "bob@test.com";

   auto unpacked = ref.unpack();
   REQUIRE(unpacked.name == "Bob");
   REQUIRE(unpacked.email == "bob@test.com");
}

TEST_CASE("cp ref: nested struct mutation frees recursively", "[ref][cp][freelist]")
{
   CpLineItem item{"Widget", 10, 4.99};
   CpUser     customer{42, "Customer", "cust@test.com", "bio", 25, 100.0,
                       std::vector<std::string>{"vip"}, true};
   CpOrder    order{1, customer, {item}, 49.90, "rush"};
   auto       data = psio1::capnp_pack(order);

   psio1::capnp_ref<CpOrder> ref(std::move(data));
   size_t initial_size = ref.size();

   // Mutate the note field repeatedly
   auto f = ref.fields();
   for (int i = 0; i < 20; ++i)
      f.note() = "updated note text";

   // Should not have grown much — the free list reclaims old note strings
   INFO("initial: " << initial_size << " final: " << ref.size());
   // At most one extra allocation beyond initial (first mutation grows, rest reuse)
   size_t growth = ref.size() - initial_size;
   REQUIRE(growth <= 24);  // at most 3 words of growth (24 bytes)

   REQUIRE(ref.unpack().note == "updated note text");
}

// ── Dynamic view tests ────────────────────────────────────────────────────

using dv_cp = psio1::dynamic_view<psio1::cp>;
using psio1::operator""_f;

TEST_CASE("dynamic_view: schema lookup and introspection", "[view][cp][dynamic]")
{
   auto* schema = &psio1::cp_schema<CpUser>::schema;
   REQUIRE(schema->field_count == 8);

   auto* id_field = schema->find("id");
   REQUIRE(id_field != nullptr);
   REQUIRE(std::string_view(id_field->name) == "id");
   REQUIRE(id_field->type == psio1::dynamic_type::t_u64);
   REQUIRE(id_field->is_ptr == false);

   auto* name_field = schema->find("name");
   REQUIRE(name_field != nullptr);
   REQUIRE(name_field->type == psio1::dynamic_type::t_text);
   REQUIRE(name_field->is_ptr == true);

   REQUIRE(schema->find("nonexistent") == nullptr);

   CpUser user{1, "a", "b", "c", 2, 3.0, {}, false};
   auto data = psio1::capnp_pack(user);
   psio1::capnp_ref<CpUser> ref(std::move(data));
   dv_cp dv(ref);

   auto names = dv.field_names();
   REQUIRE(names.size() == 8);
   REQUIRE(std::string_view(names[0]) == "id");
   REQUIRE(std::string_view(names[1]) == "name");
   REQUIRE(std::string_view(names[7]) == "verified");

   REQUIRE(dv.field_index("id") == 0);
   REQUIRE(dv.field_index("verified") == 7);
}

TEST_CASE("dynamic_view: implicit scalar conversion", "[view][cp][dynamic]")
{
   CpUser user{42, "Alice", "alice@test.com", "bio", 30, 99.5,
               std::vector<std::string>{"admin", "user"}, true};
   auto data = psio1::capnp_pack(user);
   psio1::capnp_ref<CpUser> ref(std::move(data));
   dv_cp dv(ref);

   // Implicit conversion — assignment just works
   uint64_t id = dv["id"_f];
   REQUIRE(id == 42);

   uint32_t age = dv["age"_f];
   REQUIRE(age == 30);

   double score = dv["score"_f];
   REQUIRE(score == 99.5);

   bool verified = (bool)dv["verified"_f];
   REQUIRE(verified == true);
}

TEST_CASE("dynamic_view: implicit text conversion", "[view][cp][dynamic]")
{
   CpUser user{1, "Bob", "bob@test.com", "A bio", 25, 0.0, {}, false};
   auto data = psio1::capnp_pack(user);
   psio1::capnp_ref<CpUser> ref(std::move(data));
   dv_cp dv(ref);

   std::string_view name = dv["name"_f];
   REQUIRE(name == "Bob");

   std::string_view email = dv["email"_f];
   REQUIRE(email == "bob@test.com");

   std::string_view bio = dv["bio"_f];
   REQUIRE(bio == "A bio");

   REQUIRE(dv["name"_f].size() == 3);
   REQUIRE(dv["email"_f].size() == 12);
}

TEST_CASE("dynamic_view: chained struct navigation", "[view][cp][dynamic]")
{
   CpLineItem item{"Widget", 10, 4.99};
   CpUser     customer{42, "Customer", "c@test.com", "", 25, 100.0,
                        std::vector<std::string>{"vip"}, true};
   CpOrder    order{1, customer, {item}, 49.90, "rush"};
   auto       data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp dv(ref);

   uint64_t id = dv["id"_f];
   REQUIRE(id == 1);

   double total = dv["total"_f];
   REQUIRE(total == Approx(49.90));

   std::string_view note = dv["note"_f];
   REQUIRE(note == "rush");

   // Chained navigation into nested struct
   std::string_view cust_name = dv["customer"_f]["name"_f];
   REQUIRE(cust_name == "Customer");

   uint32_t cust_age = dv["customer"_f]["age"_f];
   REQUIRE(cust_age == 25);

   uint64_t cust_id = dv["customer"_f]["id"_f];
   REQUIRE(cust_id == 42);
}

TEST_CASE("dynamic_view: missing field throws", "[view][cp][dynamic]")
{
   CpPoint pt{1.0, 2.0};
   auto    data = psio1::capnp_pack(pt);
   psio1::capnp_ref<CpPoint> ref(std::move(data));
   dv_cp dv(ref);

   REQUIRE_THROWS_AS(dv["z"_f], std::runtime_error);
}

TEST_CASE("dynamic_view: type mismatch throws", "[view][cp][dynamic]")
{
   CpPoint pt{1.0, 2.0};
   auto    data = psio1::capnp_pack(pt);
   psio1::capnp_ref<CpPoint> ref(std::move(data));
   dv_cp dv(ref);

   REQUIRE_THROWS_AS(static_cast<uint64_t>(dv["x"_f]), std::runtime_error);
   REQUIRE_THROWS_AS(static_cast<std::string_view>(dv["x"_f]), std::runtime_error);
}

TEST_CASE("dynamic_view: comparison operators", "[view][cp][dynamic]")
{
   CpUser user1{10, "Alice", "a@test.com", "", 20, 1.0, {}, false};
   CpUser user2{20, "Bob", "b@test.com", "", 30, 2.0, {}, true};
   auto   data1 = psio1::capnp_pack(user1);
   auto   data2 = psio1::capnp_pack(user2);
   psio1::capnp_ref<CpUser> ref1(std::move(data1));
   psio1::capnp_ref<CpUser> ref2(std::move(data2));
   dv_cp dv1(ref1);
   dv_cp dv2(ref2);

   REQUIRE(dv1["id"_f] < dv2["id"_f]);
   REQUIRE(dv2["id"_f] > dv1["id"_f]);
   REQUIRE(dv1["id"_f] == dv1["id"_f]);

   REQUIRE(dv1["name"_f] < dv2["name"_f]);  // "Alice" < "Bob"

   REQUIRE(dv1["score"_f] < dv2["score"_f]);
}

TEST_CASE("dynamic_view: type introspection", "[view][cp][dynamic]")
{
   CpUser user{1, "a", "b", "c", 2, 3.0, {}, false};
   auto   data = psio1::capnp_pack(user);
   psio1::capnp_ref<CpUser> ref(std::move(data));
   dv_cp dv(ref);

   REQUIRE(dv.type().kind == psio1::dynamic_type::t_struct);
   REQUIRE(dv["id"_f].type().kind == psio1::dynamic_type::t_u64);
   REQUIRE(dv["name"_f].type().kind == psio1::dynamic_type::t_text);
   REQUIRE(dv["verified"_f].type().kind == psio1::dynamic_type::t_bool);
   REQUIRE(dv["score"_f].type().kind == psio1::dynamic_type::t_f64);
   REQUIRE(dv["age"_f].type().kind == psio1::dynamic_type::t_u32);
}

TEST_CASE("dynamic_view: vector access", "[view][cp][dynamic]")
{
   CpLineItem item1{"Widget", 10, 4.99};
   CpLineItem item2{"Gadget", 5, 19.99};
   CpUser     customer{42, "Cust", "c@t.com", "", 25, 0.0, {}, true};
   CpOrder    order{1, customer, {item1, item2}, 74.85, "note"};
   auto       data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp dv(ref);

   REQUIRE(dv["items"_f].size() == 2);

   std::string_view prod0 = dv["items"_f][0]["product"_f];
   REQUIRE(prod0 == "Widget");

   uint32_t qty0 = dv["items"_f][0]["qty"_f];
   REQUIRE(qty0 == 10);

   double price1 = dv["items"_f][1]["unit_price"_f];
   REQUIRE(price1 == 19.99);

   std::string_view prod1 = dv["items"_f][1]["product"_f];
   REQUIRE(prod1 == "Gadget");
}

TEST_CASE("dynamic_view: dynamic_vector iteration", "[view][cp][dynamic]")
{
   CpLineItem item1{"A", 1, 1.0};
   CpLineItem item2{"B", 2, 2.0};
   CpLineItem item3{"C", 3, 3.0};
   CpUser     customer{1, "X", "x@t.com", "", 1, 0.0, {}, false};
   CpOrder    order{1, customer, {item1, item2, item3}, 6.0, ""};
   auto       data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp dv(ref);

   psio1::dynamic_vector<psio1::cp> items(dv["items"_f]);
   REQUIRE(items.size() == 3);

   std::vector<std::string> products;
   for (auto elem : items)
   {
      std::string_view p = elem["product"_f];
      products.push_back(std::string(p));
   }
   REQUIRE(products == std::vector<std::string>{"A", "B", "C"});
}

TEST_CASE("dynamic_view: string vector", "[view][cp][dynamic]")
{
   CpUser user{1, "X", "x@t.com", "", 1, 0.0,
               std::vector<std::string>{"tag1", "tag2", "tag3"}, false};
   auto   data = psio1::capnp_pack(user);
   psio1::capnp_ref<CpUser> ref(std::move(data));
   dv_cp dv(ref);

   REQUIRE(dv["tags"_f].size() == 3);

   std::string_view t0 = dv["tags"_f][0];
   REQUIRE(t0 == "tag1");

   std::string_view t2 = dv["tags"_f][2];
   REQUIRE(t2 == "tag3");
}

TEST_CASE("dynamic_view: construct from view<T,cp>", "[view][cp][dynamic]")
{
   CpPoint pt{3.14, 2.718};
   auto    data = psio1::capnp_pack(pt);
   auto    v    = psio1::view<CpPoint, psio1::cp>::from_buffer(data.data());
   dv_cp   dv(v);

   double x = dv["x"_f];
   REQUIRE(x == 3.14);

   double y = dv["y"_f];
   REQUIRE(y == 2.718);
}

TEST_CASE("dynamic_view: as<T>() duck-typed extraction", "[view][cp][dynamic]")
{
   CpPoint pt{1.5, 2.5};
   auto    data = psio1::capnp_pack(pt);
   psio1::capnp_ref<CpPoint> ref(std::move(data));
   dv_cp dv(ref);

   CpPoint extracted = dv.as<CpPoint>();
   REQUIRE(extracted.x == 1.5);
   REQUIRE(extracted.y == 2.5);
}

TEST_CASE("dynamic: operator/ chaining", "[dynamic]")
{
   CpOrder order;
   order.id       = 42;
   order.customer = CpUser{1, "Alice", "alice@test.com", "hi", 30, 0.95, {}, true};
   order.items    = {{"Widget", 100, 2.0}, {"Gadget", 200, 1.0}};
   order.total    = 400;
   order.note     = "rush";

   auto data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp root(ref);

   // Simple field access
   uint64_t id = root / "id";
   REQUIRE(id == 42);

   // Chained struct navigation
   std::string_view name = root / "customer" / "name";
   REQUIRE(name == "Alice");

   // Mixed field + index
   std::string_view product = root / "items" / size_t(0) / "product";
   REQUIRE(product == "Widget");

   uint32_t qty = root / "items" / size_t(1) / "qty";
   REQUIRE(qty == 200);
}

TEST_CASE("dynamic: .path() string traversal", "[dynamic]")
{
   CpOrder order;
   order.id       = 99;
   order.customer = CpUser{2, "Bob", "bob@test.com", "dev", 25, 0.8, {}, false};
   order.items    = {{"Alpha", 10, 5.0}, {"Beta", 20, 3.0}, {"Gamma", 30, 1.0}};
   order.total    = 110;
   order.note     = "test";

   auto data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp root(ref);

   // Simple field
   uint64_t id = root.path("id");
   REQUIRE(id == 99);

   // Dotted path
   std::string_view name = root.path("customer.name");
   REQUIRE(name == "Bob");

   // Deep dotted path
   double score = root.path("customer.score");
   REQUIRE(score == Approx(0.8));

   // Array index
   std::string_view item0 = root.path("items[0].product");
   REQUIRE(item0 == "Alpha");

   // Array index mid-path
   uint32_t qty = root.path("items[2].qty");
   REQUIRE(qty == 30);

   // Field after array index
   double price = root.path("items[1].unit_price");
   REQUIRE(price == Approx(3.0));

   // Trailing field
   std::string_view last_product = root.path("items[2].product");
   REQUIRE(last_product == "Gamma");
}

TEST_CASE("dynamic: .path() error handling", "[dynamic]")
{
   CpOrder order;
   order.id       = 1;
   order.customer = CpUser{0, "X", "x@x.com", "", 0, 0.0, {}, false};
   order.items    = {};
   order.total    = 0;

   auto data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp root(ref);

   // Missing field throws
   REQUIRE_THROWS(root.path("customer.nonexistent"));

   // Bad bracket syntax throws
   REQUIRE_THROWS(root.path("items[abc"));
}

TEST_CASE("dynamic: compiled_path eval", "[dynamic]")
{
   CpOrder order;
   order.id       = 77;
   order.customer = CpUser{3, "Charlie", "charlie@test.com", "eng", 35, 0.92, {}, true};
   order.items    = {{"Foo", 50, 10.0}, {"Bar", 75, 20.0}};
   order.total    = 250;
   order.note     = "compiled";

   auto data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp root(ref);

   auto& schema = psio1::cp_schema<CpOrder>::schema;

   // Simple field
   psio1::compiled_path<psio1::cp> id_path(schema, "id");
   uint64_t id = id_path.eval(root);
   REQUIRE(id == 77);

   // Dotted path
   psio1::compiled_path<psio1::cp> name_path(schema, "customer.name");
   std::string_view name = name_path.eval(root);
   REQUIRE(name == "Charlie");

   // Deep path
   psio1::compiled_path<psio1::cp> score_path(schema, "customer.score");
   double score = score_path.eval(root);
   REQUIRE(score == Approx(0.92));

   // Path with array index
   psio1::compiled_path<psio1::cp> item_path(schema, "items[1].product");
   std::string_view product = item_path.eval(root);
   REQUIRE(product == "Bar");

   // Reuse compiled path across different data (same schema)
   CpOrder order2;
   order2.id       = 88;
   order2.customer = CpUser{4, "Dana", "dana@test.com", "", 28, 0.5, {}, false};
   order2.items    = {{"Qux", 100, 5.0}};
   order2.total    = 100;
   order2.note     = "reuse";

   auto data2 = psio1::capnp_pack(order2);
   psio1::capnp_ref<CpOrder> ref2(std::move(data2));
   dv_cp root2(ref2);

   // Same compiled paths, different data
   uint64_t id2 = id_path.eval(root2);
   REQUIRE(id2 == 88);

   std::string_view name2 = name_path.eval(root2);
   REQUIRE(name2 == "Dana");

   // Compiled path error: bad field
   REQUIRE_THROWS(psio1::compiled_path<psio1::cp>(schema, "customer.nonexistent"));
}

TEST_CASE("dynamic: hashed_path eval and compile", "[dynamic]")
{
   CpOrder order;
   order.id       = 55;
   order.customer = CpUser{5, "Eve", "eve@test.com", "qa", 40, 0.75, {}, true};
   order.items    = {{"Zap", 60, 7.5}, {"Bop", 80, 3.0}};
   order.total    = 160;
   order.note     = "hashed";

   auto data = psio1::capnp_pack(order);
   psio1::capnp_ref<CpOrder> ref(std::move(data));
   dv_cp root(ref);

   // hashed_path: parse + hash once, eval with tag-byte lookup
   psio1::hashed_path hp_id("id");
   psio1::hashed_path hp_name("customer.name");
   psio1::hashed_path hp_score("customer.score");
   psio1::hashed_path hp_item("items[1].product");

   uint64_t id = hp_id.eval<psio1::cp>(root);
   REQUIRE(id == 55);

   std::string_view name = hp_name.eval<psio1::cp>(root);
   REQUIRE(name == "Eve");

   double score = hp_score.eval<psio1::cp>(root);
   REQUIRE(score == Approx(0.75));

   std::string_view product = hp_item.eval<psio1::cp>(root);
   REQUIRE(product == "Bop");

   // Promote hashed_path → compiled_path for even faster eval
   auto& schema = psio1::cp_schema<CpOrder>::schema;
   auto cp_name = hp_name.compile<psio1::cp>(schema);
   auto cp_score = hp_score.compile<psio1::cp>(schema);

   std::string_view name2 = cp_name.eval(root);
   REQUIRE(name2 == "Eve");

   double score2 = cp_score.eval(root);
   REQUIRE(score2 == Approx(0.75));

   // Verify promotion rejects bad fields
   psio1::hashed_path hp_bad("customer.nonexistent");
   REQUIRE_THROWS(hp_bad.compile<psio1::cp>(schema));
}
