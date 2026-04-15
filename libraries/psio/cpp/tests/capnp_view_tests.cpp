// capnp_view_tests.cpp — Catch2 tests for view<T, cp> (Cap'n Proto format)
//
// Uses a minimal inline builder to construct capnp wire data without
// depending on the official capnp library.

#include <catch2/catch.hpp>
#include <psio/fracpack.hpp>  // for struct_tuple_t
#include <psio/capnp_view.hpp>

#include <cstring>
#include <string>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct CpPoint
{
   double x;
   double y;
};
PSIO_REFLECT(CpPoint, definitionWillNotChange(), x, y)

struct CpToken
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO_REFLECT(CpToken, kind, offset, length, text)

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
PSIO_REFLECT(CpUser, id, name, email, bio, age, score, tags, verified)

struct CpLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO_REFLECT(CpLineItem, product, qty, unit_price)

struct CpOrder
{
   uint64_t                 id;
   CpUser                   customer;
   std::vector<CpLineItem>  items;
   double                   total;
   std::string              note;
};
PSIO_REFLECT(CpOrder, id, customer, items, total, note)

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
   using L = psio::capnp_layout<CpPoint>;
   STATIC_REQUIRE(L::data_words == 2);
   STATIC_REQUIRE(L::ptr_count == 0);
   STATIC_REQUIRE(!L::loc(0).is_ptr);
   STATIC_REQUIRE(L::loc(0).offset == 0);
   STATIC_REQUIRE(!L::loc(1).is_ptr);
   STATIC_REQUIRE(L::loc(1).offset == 8);
}

TEST_CASE("cp layout: CpToken mixed data+pointer", "[view][cp]")
{
   using L = psio::capnp_layout<CpToken>;
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
   using L = psio::capnp_layout<CpUser>;
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
   auto v   = psio::capnp_view<CpPoint>::from_buffer(buf.data());

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
   auto v   = psio::capnp_view<CpToken>::from_buffer(buf.data());

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
   auto v   = psio::capnp_view<CpUser>::from_buffer(buf.data());

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
   auto v   = psio::capnp_view<CpUser>::from_buffer(buf.data());

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

   using OrderLayout = psio::capnp_layout<CpOrder>;
   using ItemLayout  = psio::capnp_layout<CpLineItem>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(OrderLayout::data_words);
   uint32_t ptrs_start = b.alloc(OrderLayout::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, OrderLayout::data_words, OrderLayout::ptr_count);

   b.set_field<uint64_t>(data_start, 0, 999);   // id
   b.set_field<double>(data_start, 8, 54.47);    // total

   // customer (ptr 0) — minimal, just set id and name
   {
      using UL = psio::capnp_layout<CpUser>;
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
   auto v   = psio::capnp_view<CpOrder>::from_buffer(buf.data());

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
   using OrderLayout = psio::capnp_layout<CpOrder>;
   using ItemLayout  = psio::capnp_layout<CpLineItem>;
   using UL          = psio::capnp_layout<CpUser>;

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
   auto v     = psio::capnp_view<CpOrder>::from_buffer(buf.data());
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
PSIO_REFLECT(CpConfig, version, port, timeout, verbose)

TEST_CASE("cp view: default value XOR", "[view][cp]")
{
   // CpConfig layout: version(u32)@0, port(u16)@4, timeout(f64)@8, verbose(bool)@6.bit0
   // (port is 2 bytes at offset 4, verbose gets packed into the gap at byte 6)
   using L = psio::capnp_layout<CpConfig>;

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
   auto v   = psio::capnp_view<CpConfig>::from_buffer(buf.data());

   REQUIRE(v.version() == 5);
   REQUIRE(v.port() == 9090);
   REQUIRE(v.timeout() == 60.0);
   REQUIRE(v.verbose() == false);
}

TEST_CASE("cp view: zero wire with non-zero defaults", "[view][cp]")
{
   // When wire data is all zeros, the actual values should be the defaults
   using L = psio::capnp_layout<CpConfig>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   // All zeros — should read back as defaults

   auto buf = b.finish();
   auto v   = psio::capnp_view<CpConfig>::from_buffer(buf.data());

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
PSIO_REFLECT(CpMessage, id, payload, tag)

TEST_CASE("cp view: Data (vector<uint8_t>)", "[view][cp]")
{
   // CpMessage layout: id@0 (u64) → data offset 0
   // payload → ptr 0 (List(UInt8))
   // tag → ptr 1 (Text)
   using L = psio::capnp_layout<CpMessage>;

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
   auto v   = psio::capnp_view<CpMessage>::from_buffer(buf.data());

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
PSIO_REFLECT(CpShape, area, shape)

TEST_CASE("cp layout: union discriminant placement", "[view][cp]")
{
   // Verified against official capnp compiler:
   // data_words=3, ptr_count=1
   // area @0: Float64 at data offset 0
   // circle @1: Float64 at data offset 8
   // rectangle @2: ptr 0
   // nothing @3: void (no space)
   // discriminant: uint16_t at data offset 16
   using L = psio::capnp_layout<CpShape>;
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
   using L = psio::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 100.0);    // area
   b.set_field<double>(data_start, 8, 5.0);      // circle (alt 0 data)
   b.set_field<uint16_t>(data_start, 16, 0);     // discriminant = 0 (circle)

   auto buf = b.finish();
   auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

   REQUIRE(v.area() == 100.0);
   auto shape = v.shape();
   REQUIRE(shape.index() == 0);
   REQUIRE(std::get<0>(shape) == 5.0);
}

TEST_CASE("cp view: union rectangle variant", "[view][cp]")
{
   using L = psio::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 200.0);    // area
   b.set_field<uint16_t>(data_start, 16, 1);     // discriminant = 1 (rectangle)
   b.write_text(ptrs_start + 0, "4x3");           // rectangle text at ptr 0

   auto buf = b.finish();
   auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

   REQUIRE(v.area() == 200.0);
   auto shape = v.shape();
   REQUIRE(shape.index() == 1);
   REQUIRE(std::get<1>(shape) == "4x3");
}

TEST_CASE("cp view: union nothing (void) variant", "[view][cp]")
{
   using L = psio::capnp_layout<CpShape>;

   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 0.0);      // area
   b.set_field<uint16_t>(data_start, 16, 2);     // discriminant = 2 (nothing)

   auto buf = b.finish();
   auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

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
PSIO_REFLECT(CpMatrix, id, rows)

TEST_CASE("cp view: nested list (vector<vector<uint32_t>>)", "[view][cp]")
{
   // CpMatrix layout: id @0 u32 → data offset 0, rows → ptr 0
   // rows is List(List(UInt32)): outer list is a list of pointers (elem_sz=6),
   // each pointer points to a List(UInt32) with elem_sz=4.
   using L = psio::capnp_layout<CpMatrix>;

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
   auto v   = psio::capnp_view<CpMatrix>::from_buffer(buf.data());

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
   using L = psio::capnp_layout<CpPoint>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   b.set_field<double>(data_start, 0, 3.14);
   b.set_field<double>(data_start, 8, 2.72);

   auto    buf = b.finish();
   CpPoint pt  = psio::capnp_unpack<CpPoint>(buf.data());
   REQUIRE(pt.x == 3.14);
   REQUIRE(pt.y == 2.72);
}

TEST_CASE("cp unpack: struct with strings and vectors", "[view][cp]")
{
   // Build CpMessage wire data, unpack
   using L = psio::capnp_layout<CpMessage>;
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
   auto msg = psio::capnp_unpack<CpMessage>(buf.data());

   REQUIRE(msg.id == 42);
   REQUIRE(msg.tag == "hello");
   REQUIRE(msg.payload.size() == 2);
   REQUIRE(msg.payload[0] == 0xDE);
   REQUIRE(msg.payload[1] == 0xAD);
}

TEST_CASE("cp unpack: union variant", "[view][cp]")
{
   using L = psio::capnp_layout<CpShape>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   uint32_t ptrs_start = b.alloc(L::ptr_count);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

   b.set_field<double>(data_start, 0, 50.0);
   b.set_field<uint16_t>(data_start, 16, 1);  // discriminant = rectangle
   b.write_text(ptrs_start + 0, "5x10");

   auto buf = b.finish();
   auto s   = psio::capnp_unpack<CpShape>(buf.data());

   REQUIRE(s.area == 50.0);
   REQUIRE(s.shape.index() == 1);
   REQUIRE(std::get<1>(s.shape) == "5x10");
}

TEST_CASE("cp unpack: default values restored", "[view][cp]")
{
   using L = psio::capnp_layout<CpConfig>;
   capnp_builder b;
   uint32_t root_ptr   = b.alloc(1);
   uint32_t data_start = b.alloc(L::data_words);
   b.set_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);
   // All zeros → defaults

   auto buf = b.finish();
   auto cfg = psio::capnp_unpack<CpConfig>(buf.data());

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
   auto    buf = psio::capnp_pack(pt);

   // Read back through view
   auto v = psio::capnp_view<CpPoint>::from_buffer(buf.data());
   REQUIRE(v.x() == 3.14);
   REQUIRE(v.y() == 2.72);

   // And unpack round-trip
   auto pt2 = psio::capnp_unpack<CpPoint>(buf.data());
   REQUIRE(pt2.x == 3.14);
   REQUIRE(pt2.y == 2.72);
}

TEST_CASE("cp pack: struct with string and data", "[view][cp]")
{
   CpMessage msg;
   msg.id      = 99;
   msg.payload = {0x01, 0x02, 0x03};
   msg.tag     = "packed";

   auto buf = psio::capnp_pack(msg);
   auto v   = psio::capnp_view<CpMessage>::from_buffer(buf.data());

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

   auto buf = psio::capnp_pack(user);
   auto v   = psio::capnp_view<CpUser>::from_buffer(buf.data());

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

   auto buf = psio::capnp_pack(order);
   auto v   = psio::capnp_view<CpOrder>::from_buffer(buf.data());

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

      auto buf = psio::capnp_pack(shape);
      auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

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

      auto buf = psio::capnp_pack(shape);
      auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

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

      auto buf = psio::capnp_pack(shape);
      auto v   = psio::capnp_view<CpShape>::from_buffer(buf.data());

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

   auto buf = psio::capnp_pack(cfg);
   auto v   = psio::capnp_view<CpConfig>::from_buffer(buf.data());

   REQUIRE(v.version() == 5);
   REQUIRE(v.port() == 9090);
   REQUIRE(v.timeout() == 60.0);
   REQUIRE(v.verbose() == false);

   // All-default values should also round-trip
   CpConfig def{};
   auto buf2 = psio::capnp_pack(def);
   auto v2   = psio::capnp_view<CpConfig>::from_buffer(buf2.data());

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

   auto buf = psio::capnp_pack(m);
   auto v   = psio::capnp_view<CpMatrix>::from_buffer(buf.data());

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

   auto    buf  = psio::capnp_pack(order);
   CpOrder out  = psio::capnp_unpack<CpOrder>(buf.data());

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
      auto    buf = psio::capnp_pack(pt);
      REQUIRE(psio::capnp_validate(buf.data(), buf.size()));
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

      auto buf = psio::capnp_pack(order);
      REQUIRE(psio::capnp_validate(buf.data(), buf.size()));
   }
}

TEST_CASE("cp validate: too-short buffer", "[view][cp]")
{
   REQUIRE_FALSE(psio::capnp_validate(nullptr, 0));

   uint8_t small[4] = {};
   REQUIRE_FALSE(psio::capnp_validate(small, 4));
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
   REQUIRE_FALSE(psio::capnp_validate(buf.data(), buf.size()));
}

TEST_CASE("cp validate: truncated segment", "[view][cp]")
{
   CpPoint pt{1.0, 2.0};
   auto    buf = psio::capnp_pack(pt);
   // Truncate: claim full size but only provide half
   REQUIRE_FALSE(psio::capnp_validate(buf.data(), buf.size() / 2));
}

// ============================================================================
//  TYPE IDENTITY & ITERATOR CONCEPT TESTS
// ============================================================================

TEST_CASE("cp type alias identity", "[view][cp]")
{
   STATIC_REQUIRE(std::is_same_v<psio::capnp_view<CpOrder>, psio::view<CpOrder, psio::cp>>);
}

TEST_CASE("cp vec_view iterator is random access", "[view][cp]")
{
   using iter = psio::vec_view<CpLineItem, psio::cp>::iterator;
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
PSIO_REFLECT(CpDefaults, count, ratio)

TEST_CASE("cp ref: read scalar fields", "[ref][cp]")
{
   CpToken tok{3, 100, 42, "hello"};
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
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
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
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
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
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
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
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
   auto data = psio::capnp_pack(user);

   psio::capnp_ref<CpUser> ref(std::move(data));
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
   auto data = psio::capnp_pack(user);

   psio::capnp_ref<CpUser> ref(std::move(data));
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
   auto data = psio::capnp_pack(order);

   psio::capnp_ref<CpOrder> ref(std::move(data));
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
   auto data = psio::capnp_pack(shape);

   psio::capnp_ref<CpShape> ref(std::move(data));
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
   auto data = psio::capnp_pack(shape);

   psio::capnp_ref<CpShape> ref(std::move(data));
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
   auto data = psio::capnp_pack(order);

   psio::capnp_ref<CpOrder> ref(std::move(data));
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
   auto data = psio::capnp_pack(mat);

   psio::capnp_ref<CpMatrix> ref(std::move(data));

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
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
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
   auto    data = psio::capnp_pack(tok);

   psio::capnp_ref<CpToken> ref(std::move(data));
   auto                     v = ref.as_view();

   REQUIRE(v.kind() == 3);
   REQUIRE(v.offset() == 100);
   REQUIRE(v.text() == "hello");
}

TEST_CASE("cp ref: bool mutation", "[ref][cp]")
{
   CpUser user{};
   user.verified = false;
   auto data = psio::capnp_pack(user);

   psio::capnp_ref<CpUser> ref(std::move(data));
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
   auto data = psio::capnp_pack(d);

   psio::capnp_ref<CpDefaults> ref(std::move(data));
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
