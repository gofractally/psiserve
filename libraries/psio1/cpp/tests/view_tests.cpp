// Unified view<T, Fmt> tests — exercises the same API across fb and frac formats,
// including container views, iterators, and structured bindings.

#include <catch2/catch.hpp>
#include <psio1/flatbuf.hpp>
#include <psio1/fracpack.hpp>
#include <psio1/frac_ref.hpp>

#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct VPoint
{
   int32_t x;
   int32_t y;
};
PSIO1_REFLECT(VPoint, definitionWillNotChange(), x, y)

struct VCustomer
{
   uint64_t    id;
   std::string name;
   std::string email;
};
PSIO1_REFLECT(VCustomer, id, name, email)

struct VLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO1_REFLECT(VLineItem, product, qty, unit_price)

struct VOrder
{
   uint64_t                   id;
   VCustomer                  customer;
   std::vector<VLineItem>     items;
   double                     total;
   std::optional<std::string> note;
};
PSIO1_REFLECT(VOrder, id, customer, items, total, note)

struct VTagged
{
   uint32_t           id;
   std::set<int32_t>  tags;
};
PSIO1_REFLECT(VTagged, id, tags)

struct VConfig
{
   uint32_t                        version;
   std::map<std::string, uint32_t> limits;
};
PSIO1_REFLECT(VConfig, version, limits)

// ── Fracpack helpers ────────────────────────────────────────────────────────

template <typename T>
std::vector<char> pack_frac(const T& val)
{
   return psio1::to_frac(val);
}

// ── FlatBuffer helpers ──────────────────────────────────────────────────────

template <typename T>
std::vector<uint8_t> pack_fb(const T& val)
{
   psio1::fb_builder bld;
   bld.pack(val);
   return {bld.data(), bld.data() + bld.size()};
}

// ============================================================================
//  FRAC VIEW TESTS
// ============================================================================

TEST_CASE("frac view: fixed-size struct", "[view][frac]")
{
   VPoint pt{42, -7};
   auto   packed = pack_frac(pt);
   auto   v      = psio1::view<VPoint, psio1::frac>::from_buffer(packed.data());

   REQUIRE(v);
   REQUIRE(v.x() == 42);
   REQUIRE(v.y() == -7);
}

TEST_CASE("frac view: struct with strings", "[view][frac]")
{
   VCustomer c{99, "Alice", "alice@test.com"};
   auto      packed = pack_frac(c);
   auto      v      = psio1::frac_view<VCustomer>::from_buffer(packed.data());

   REQUIRE(v.id() == 99);
   REQUIRE(v.name() == "Alice");
   REQUIRE(v.email() == "alice@test.com");
}

TEST_CASE("frac view: nested struct with vector", "[view][frac]")
{
   VOrder o{1, {10, "Bob", "bob@x.com"}, {{"A", 2, 5.0}, {"B", 1, 10.0}}, 20.0, "rush"};
   auto   packed = pack_frac(o);
   auto   v      = psio1::frac_view<VOrder>::from_buffer(packed.data());

   REQUIRE(v.id() == 1);
   REQUIRE(v.total() == 20.0);
   REQUIRE(v.customer().name() == "Bob");

   auto items = v.items();
   REQUIRE(items.size() == 2);
   REQUIRE(items[0].product() == "A");
   REQUIRE(items[0].qty() == 2);
   REQUIRE(items[1].product() == "B");
   REQUIRE(items[1].unit_price() == 10.0);
}

TEST_CASE("frac view: optional present and absent", "[view][frac]")
{
   VOrder with_note{1, {1, "A", "a@a"}, {}, 0, "hello"};
   VOrder no_note{2, {2, "B", "b@b"}, {}, 0, std::nullopt};

   auto p1 = pack_frac(with_note);
   auto p2 = pack_frac(no_note);

   auto v1 = psio1::frac_view<VOrder>::from_buffer(p1.data());
   auto v2 = psio1::frac_view<VOrder>::from_buffer(p2.data());

   REQUIRE(v1.note() == "hello");
   REQUIRE(v2.note() == std::string_view{});
}

TEST_CASE("frac view: set_view contains and iteration", "[view][frac]")
{
   VTagged t{7, {10, 20, 30, 40, 50}};
   auto    packed = pack_frac(t);
   auto    v      = psio1::frac_view<VTagged>::from_buffer(packed.data());

   REQUIRE(v.id() == 7);
   auto tags = v.tags();
   REQUIRE(tags.size() == 5);
   REQUIRE(tags.contains(10));
   REQUIRE(tags.contains(30));
   REQUIRE(!tags.contains(15));

   // Iteration
   std::vector<int32_t> keys;
   for (auto k : tags)
      keys.push_back(k);
   REQUIRE(keys == std::vector<int32_t>{10, 20, 30, 40, 50});
}

TEST_CASE("frac view: map_view lookup and iteration", "[view][frac]")
{
   VConfig cfg{1, {{"alpha", 10}, {"beta", 20}, {"gamma", 30}}};
   auto    packed = pack_frac(cfg);
   auto    v      = psio1::frac_view<VConfig>::from_buffer(packed.data());

   REQUIRE(v.version() == 1);
   auto lim = v.limits();
   REQUIRE(lim.size() == 3);
   REQUIRE(lim.contains("beta"));
   REQUIRE(!lim.contains("delta"));
   REQUIRE(lim.value_or("alpha", 0u) == 10u);
   REQUIRE(lim.value_or("gamma", 0u) == 30u);
   REQUIRE(lim.value_or("missing", 999u) == 999u);

   // Structured binding iteration
   std::vector<std::pair<std::string, uint32_t>> entries;
   for (auto [k, val] : lim)
      entries.push_back({std::string(k), val});
   REQUIRE(entries.size() == 3);
   REQUIRE(entries[0].first == "alpha");
   REQUIRE(entries[0].second == 10);
   REQUIRE(entries[2].first == "gamma");
   REQUIRE(entries[2].second == 30);
}

// ============================================================================
//  FLATBUFFER VIEW TESTS
// ============================================================================

TEST_CASE("fb view: fixed-size struct", "[view][fb]")
{
   VPoint pt{42, -7};
   auto   packed = pack_fb(pt);
   auto   v      = psio1::view<VPoint, psio1::fb>::from_buffer(packed.data());

   REQUIRE(v);
   REQUIRE(v.x() == 42);
   REQUIRE(v.y() == -7);
}

TEST_CASE("fb view: struct with strings", "[view][fb]")
{
   VCustomer c{99, "Alice", "alice@test.com"};
   auto      packed = pack_fb(c);
   auto      v      = psio1::fb_view<VCustomer>::from_buffer(packed.data());

   REQUIRE(v.id() == 99);
   REQUIRE(v.name() == "Alice");
   REQUIRE(v.email() == "alice@test.com");
}

TEST_CASE("fb view: nested struct with vector", "[view][fb]")
{
   VOrder o{1, {10, "Bob", "bob@x.com"}, {{"A", 2, 5.0}, {"B", 1, 10.0}}, 20.0, "rush"};
   auto   packed = pack_fb(o);
   auto   v      = psio1::fb_view<VOrder>::from_buffer(packed.data());

   REQUIRE(v.id() == 1);
   REQUIRE(v.total() == 20.0);
   REQUIRE(v.customer().name() == "Bob");

   auto items = v.items();
   REQUIRE(items.size() == 2);
   REQUIRE(items[0].product() == "A");
   REQUIRE(items[0].qty() == 2);
   REQUIRE(items[1].product() == "B");
   REQUIRE(items[1].unit_price() == 10.0);
}

TEST_CASE("fb view: vec_view iteration", "[view][fb]")
{
   VOrder o{1, {1, "X", "x@x"}, {{"P1", 1, 1.0}, {"P2", 2, 2.0}, {"P3", 3, 3.0}}, 0, std::nullopt};
   auto   packed = pack_fb(o);
   auto   v      = psio1::fb_view<VOrder>::from_buffer(packed.data());

   int count = 0;
   for (auto item : v.items())
   {
      REQUIRE(item.qty() == static_cast<uint32_t>(count + 1));
      ++count;
   }
   REQUIRE(count == 3);
}

TEST_CASE("fb view: set_view contains and iteration", "[view][fb]")
{
   VTagged t{7, {10, 20, 30, 40, 50}};
   auto    packed = pack_fb(t);
   auto    v      = psio1::fb_view<VTagged>::from_buffer(packed.data());

   auto tags = v.tags();
   REQUIRE(tags.size() == 5);
   REQUIRE(tags.contains(10));
   REQUIRE(tags.contains(50));
   REQUIRE(!tags.contains(15));

   std::vector<int32_t> keys;
   for (auto k : tags)
      keys.push_back(k);
   REQUIRE(keys == std::vector<int32_t>{10, 20, 30, 40, 50});
}

TEST_CASE("fb view: map_view lookup and iteration", "[view][fb]")
{
   VConfig cfg{1, {{"alpha", 10}, {"beta", 20}, {"gamma", 30}}};
   auto    packed = pack_fb(cfg);
   auto    v      = psio1::fb_view<VConfig>::from_buffer(packed.data());

   auto lim = v.limits();
   REQUIRE(lim.size() == 3);
   REQUIRE(lim.contains("beta"));
   REQUIRE(!lim.contains("delta"));
   REQUIRE(lim.value_or("alpha", 0u) == 10u);
   REQUIRE(lim.value_or("gamma", 0u) == 30u);
   REQUIRE(lim.value_or("missing", 999u) == 999u);

   // Structured binding iteration
   std::vector<std::pair<std::string, uint32_t>> entries;
   for (auto [k, val] : lim)
      entries.push_back({std::string(k), val});
   REQUIRE(entries.size() == 3);
   // FB maps are sorted by key
   REQUIRE(entries[0].first == "alpha");
   REQUIRE(entries[0].second == 10);
   REQUIRE(entries[2].first == "gamma");
   REQUIRE(entries[2].second == 30);
}

// ============================================================================
//  CROSS-FORMAT VIEW TESTS — same type, same API, different wire format
// ============================================================================

TEST_CASE("cross-format: same view API, different formats", "[view][cross]")
{
   VCustomer c{42, "Eve", "eve@example.com"};

   auto frac_packed = pack_frac(c);
   auto fb_packed   = pack_fb(c);

   auto fv = psio1::view<VCustomer, psio1::frac>::from_buffer(frac_packed.data());
   auto bv = psio1::view<VCustomer, psio1::fb>::from_buffer(fb_packed.data());

   // Identical API, identical results
   REQUIRE(fv.id() == bv.id());
   REQUIRE(fv.name() == bv.name());
   REQUIRE(fv.email() == bv.email());
}

TEST_CASE("cross-format: nested struct same results", "[view][cross]")
{
   VOrder o{99, {1, "Dan", "dan@d.com"}, {{"X", 5, 3.0}}, 15.0, "note"};

   auto frac_packed = pack_frac(o);
   auto fb_packed   = pack_fb(o);

   auto fv = psio1::frac_view<VOrder>::from_buffer(frac_packed.data());
   auto bv = psio1::fb_view<VOrder>::from_buffer(fb_packed.data());

   REQUIRE(fv.id() == bv.id());
   REQUIRE(fv.total() == bv.total());
   REQUIRE(fv.customer().name() == bv.customer().name());
   REQUIRE(fv.items().size() == bv.items().size());
   REQUIRE(fv.items()[0].product() == bv.items()[0].product());
   REQUIRE(fv.items()[0].qty() == bv.items()[0].qty());
}

// ============================================================================
//  ITERATOR CONCEPT TESTS
// ============================================================================

TEST_CASE("iterator concepts", "[view][iterator]")
{
   // Verify that container view iterators satisfy std concepts
   using frac_vec_iter = psio1::vec_view<VLineItem, psio1::frac>::iterator;
   using fb_vec_iter   = psio1::vec_view<VLineItem, psio1::fb>::iterator;
   using frac_set_iter = psio1::set_view<int32_t, psio1::frac>::iterator;
   using fb_set_iter   = psio1::set_view<int32_t, psio1::fb>::iterator;
   using frac_map_iter = psio1::map_view<std::string, uint32_t, psio1::frac>::iterator;
   using fb_map_iter   = psio1::map_view<std::string, uint32_t, psio1::fb>::iterator;

   STATIC_REQUIRE(std::random_access_iterator<frac_vec_iter>);
   STATIC_REQUIRE(std::random_access_iterator<fb_vec_iter>);
   STATIC_REQUIRE(std::random_access_iterator<frac_set_iter>);
   STATIC_REQUIRE(std::random_access_iterator<fb_set_iter>);
   STATIC_REQUIRE(std::random_access_iterator<frac_map_iter>);
   STATIC_REQUIRE(std::random_access_iterator<fb_map_iter>);
}

// ============================================================================
//  TYPE IDENTITY TESTS
// ============================================================================

TEST_CASE("type aliases are correct", "[view]")
{
   STATIC_REQUIRE(std::is_same_v<psio1::frac_view<VOrder>, psio1::view<VOrder, psio1::frac>>);
   STATIC_REQUIRE(std::is_same_v<psio1::fb_view<VOrder>, psio1::view<VOrder, psio1::fb>>);
   STATIC_REQUIRE(std::is_same_v<psio1::frac_sorted_set<int>, psio1::set_view<int, psio1::frac>>);
   STATIC_REQUIRE(std::is_same_v<psio1::fb_sorted_vec<int>, psio1::set_view<int, psio1::fb>>);
}
