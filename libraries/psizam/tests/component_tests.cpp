#include <catch2/catch.hpp>
#include <psio/wview.hpp>
#include <psio/canonical_abi.hpp>
#include <psio/wit_view.hpp>
#include <psizam/component.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// ── Test types ───────────────────────────────────────────────────────────────

struct Point {
   int32_t x;
   int32_t y;
};
PSIO_REFLECT(Point, x, y)

struct Person {
   std::string name;
   uint32_t    age;
};
PSIO_REFLECT(Person, name, age)

struct Order {
   std::string              customer;
   std::vector<int32_t>     quantities;
   std::vector<std::string> items;
};
PSIO_REFLECT(Order, customer, quantities, items)

struct Nested {
   Person               person;
   std::vector<Point>   path;
   std::optional<Point> origin;
};
PSIO_REFLECT(Nested, person, path, origin)

// ── is_canonical_simple tests ────────────────────────────────────────────────

TEST_CASE("is_canonical_simple — scalar types", "[wview]") {
   STATIC_REQUIRE(psio::is_canonical_simple<int32_t>);
   STATIC_REQUIRE(psio::is_canonical_simple<uint64_t>);
   STATIC_REQUIRE(psio::is_canonical_simple<float>);
   STATIC_REQUIRE(psio::is_canonical_simple<double>);
   STATIC_REQUIRE(psio::is_canonical_simple<bool>);
}

TEST_CASE("is_canonical_simple — flat struct", "[wview]") {
   STATIC_REQUIRE(psio::is_canonical_simple<Point>);
}

TEST_CASE("is_canonical_simple — struct with string is NOT simple", "[wview]") {
   STATIC_REQUIRE_FALSE(psio::is_canonical_simple<Person>);
}

TEST_CASE("is_canonical_simple — struct with vector is NOT simple", "[wview]") {
   STATIC_REQUIRE_FALSE(psio::is_canonical_simple<Order>);
}

// ── WView/WOwned type identity for simple types ─────────────────────────────

TEST_CASE("WView<int32_t> collapses to int32_t", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WView<int32_t>, int32_t>);
}

TEST_CASE("WOwned<int32_t> collapses to int32_t", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WOwned<int32_t>, int32_t>);
}

TEST_CASE("WView<Point> collapses to Point", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WView<Point>, Point>);
}

TEST_CASE("WOwned<Point> collapses to Point", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WOwned<Point>, Point>);
}

TEST_CASE("WView<Person> does NOT collapse", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WView<Person>, psio::WViewImpl<Person>>);
}

TEST_CASE("WOwned<Person> does NOT collapse", "[wview]") {
   STATIC_REQUIRE(std::is_same_v<psio::WOwned<Person>, psio::WOwnedImpl<Person>>);
}

// ── is_wviewable tests ──────────────────────────────────────────────────────

TEST_CASE("is_wviewable — scalar struct", "[wview]") {
   STATIC_REQUIRE(psio::is_wviewable<Point>);
}

TEST_CASE("is_wviewable — struct with string", "[wview]") {
   STATIC_REQUIRE(psio::is_wviewable<Person>);
}

TEST_CASE("is_wviewable — struct with vector<scalar>", "[wview]") {
   // Has vector<string> which needs descriptor alloc → NOT viewable
   STATIC_REQUIRE_FALSE(psio::is_wviewable<Order>);
}

TEST_CASE("is_wviewable — nested struct with vector<compound>", "[wview]") {
   // Has vector<Point> (compound) → NOT viewable
   STATIC_REQUIRE_FALSE(psio::is_wviewable<Nested>);
}

// ── Legacy is_viewable still works ──────────────────────────────────────────

TEST_CASE("is_viewable — legacy alias works", "[wview]") {
   STATIC_REQUIRE(psio::is_viewable<Point>);
   STATIC_REQUIRE(psio::is_viewable<Person>);
   STATIC_REQUIRE_FALSE(psio::is_viewable<Order>);
}

// ── WViewImpl construction and promotion ─────────────────────────────────────

TEST_CASE("WViewImpl<Point> — scalar struct round-trip", "[wview]") {
   Point p{42, 99};
   psio::WViewImpl<Point> view(p);

   CHECK(view.get<0>() == 42);
   CHECK(view.get<1>() == 99);

   Point promoted = view.promote();
   CHECK(promoted.x == 42);
   CHECK(promoted.y == 99);
}

TEST_CASE("WViewImpl<Person> — struct with string", "[wview]") {
   Person p{"Alice", 30};
   psio::WViewImpl<Person> view(p);

   CHECK(view.get<0>() == "Alice");
   CHECK(view.get<1>() == 30);

   Person promoted = view.promote();
   CHECK(promoted.name == "Alice");
   CHECK(promoted.age == 30);
}

TEST_CASE("WViewImpl<Order> — struct with vector<string>", "[wview]") {
   Order o{"Bob", {1, 2, 3}, {"apple", "banana", "cherry"}};
   psio::WViewImpl<Order> view(o);

   CHECK(view.get<0>() == "Bob");  // customer
   CHECK(view.get<1>().size() == 3);  // quantities
   CHECK(view.get<1>()[0] == 1);
   CHECK(view.get<1>()[2] == 3);
   CHECK(view.get<2>().size() == 3);  // items (string_view descriptors)
   CHECK(view.get<2>()[0] == "apple");
   CHECK(view.get<2>()[2] == "cherry");

   Order promoted = view.promote();
   CHECK(promoted.customer == "Bob");
   CHECK(promoted.quantities == std::vector<int32_t>{1, 2, 3});
   CHECK(promoted.items == std::vector<std::string>{"apple", "banana", "cherry"});
}

TEST_CASE("WViewImpl<Nested> — nested struct with optional", "[wview]") {
   Nested n{{"Carol", 25}, {{1, 2}, {3, 4}}, Point{10, 20}};
   psio::WViewImpl<Nested> view(n);

   // person field
   CHECK(view.get<0>().get<0>() == "Carol");
   CHECK(view.get<0>().get<1>() == 25);

   // path field (vector<Point> → span<WViewImpl<Point>>)
   CHECK(view.get<1>().size() == 2);
   CHECK(view.get<1>()[0].get<0>() == 1);
   CHECK(view.get<1>()[1].get<1>() == 4);

   // origin field (optional<Point> → const WViewImpl<Point>*)
   REQUIRE(view.get<2>() != nullptr);
   CHECK(view.get<2>()->get<0>() == 10);
   CHECK(view.get<2>()->get<1>() == 20);

   Nested promoted = view.promote();
   CHECK(promoted.person.name == "Carol");
   CHECK(promoted.person.age == 25);
   CHECK(promoted.path.size() == 2);
   CHECK(promoted.path[0].x == 1);
   REQUIRE(promoted.origin.has_value());
   CHECK(promoted.origin->x == 10);
}

TEST_CASE("WViewImpl<Nested> — optional is null", "[wview]") {
   Nested n{{"Dave", 40}, {}, std::nullopt};
   psio::WViewImpl<Nested> view(n);

   CHECK(view.get<2>() == nullptr);

   Nested promoted = view.promote();
   CHECK_FALSE(promoted.origin.has_value());
}

// ── WOwnedImpl tests ────────────────────────────────────────────────────────

TEST_CASE("WOwnedImpl — wraps WViewImpl and tracks allocations", "[wview]") {
   Person p{"Eve", 28};
   psio::WViewImpl<Person> view(p);
   psio::WOwnedImpl<Person> owned(std::move(view));

   CHECK(owned.get<0>() == "Eve");
   CHECK(owned.get<1>() == 28);

   Person promoted = owned.promote();
   CHECK(promoted.name == "Eve");
   CHECK(promoted.age == 28);
}

// ── Legacy CView/COwned aliases ─────────────────────────────────────────────

TEST_CASE("CView legacy alias works", "[wview]") {
   Person p{"Frank", 35};
   psio::CView<Person> view(p);
   CHECK(view.get<0>() == "Frank");
   CHECK(view.get<1>() == 35);
}

TEST_CASE("COwned legacy alias works", "[wview]") {
   Person p{"Grace", 42};
   psio::CView<Person> view(p);
   psio::COwned<Person> owned(std::move(view));
   CHECK(owned.get<0>() == "Grace");
}

// ── Proxy accessor tests ────────────────────────────────────────────────────

TEST_CASE("WViewImpl<Person> — named proxy accessors", "[wview]") {
   Person p{"Alice", 30};
   psio::WViewImpl<Person> view(p);
   auto px = view.proxy();

   CHECK(px.name() == "Alice");
   CHECK(px.age() == 30);
}

TEST_CASE("WViewImpl<Nested> — nested proxy accessors", "[wview]") {
   Nested n{{"Carol", 25}, {{1, 2}, {3, 4}}, Point{10, 20}};
   psio::WViewImpl<Nested> view(n);
   auto px = view.proxy();

   // Nested person — returns proxy with named accessors
   CHECK(px.person().name() == "Carol");
   CHECK(px.person().age() == 25);
}

TEST_CASE("WOwnedImpl — proxy accessors", "[wview]") {
   Person p{"Bob", 50};
   psio::WViewImpl<Person> view(p);
   psio::WOwnedImpl<Person> owned(std::move(view));
   auto px = owned.proxy();

   CHECK(px.name() == "Bob");
   CHECK(px.age() == 50);
}

// ── ComponentProxy tests — scalar methods ────────────────────────────────────

struct Calculator {
   int32_t add(int32_t a, int32_t b) { return a + b; }
   int32_t mul(int32_t a, int32_t b) { return a * b; }
   double  divide(double a, double b) { return a / b; }
   float   to_float(int32_t x) { return static_cast<float>(x); }
};
PSIO_REFLECT(Calculator,
   method(add, a, b),
   method(mul, a, b),
   method(divide, a, b),
   method(to_float, x)
)

TEST_CASE("ComponentProxy — int32 add", "[component]") {
   Calculator calc;
   auto result = psizam::ComponentProxy<Calculator>::call<&Calculator::add>(
      &calc, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   CHECK(static_cast<int32_t>(result) == 7);
}

TEST_CASE("ComponentProxy — int32 mul", "[component]") {
   Calculator calc;
   auto result = psizam::ComponentProxy<Calculator>::call<&Calculator::mul>(
      &calc, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   CHECK(static_cast<int32_t>(result) == 42);
}

TEST_CASE("ComponentProxy — double divide", "[component]") {
   Calculator calc;

   // Pack doubles as int64_t
   union { double d; int64_t i; } a, b;
   a.d = 10.0;
   b.d = 3.0;

   auto result = psizam::ComponentProxy<Calculator>::call<&Calculator::divide>(
      &calc, a.i, b.i, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

   union { int64_t i; double d; } r;
   r.i = result;
   CHECK(r.d == Approx(3.333333).epsilon(0.001));
}

TEST_CASE("ComponentProxy — float return", "[component]") {
   Calculator calc;
   auto result = psizam::ComponentProxy<Calculator>::call<&Calculator::to_float>(
      &calc, 42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

   union { int32_t i; float f; } r;
   r.i = static_cast<int32_t>(result);
   CHECK(r.f == Approx(42.0f));
}

// ── ComponentProxy — record arg dispatch ─────────────────────────────────────

struct PointService {
   int32_t distance_sq(Point p) { return p.x * p.x + p.y * p.y; }
   Point   scale(Point p, int32_t factor) { return {p.x * factor, p.y * factor}; }
};
PSIO_REFLECT(PointService,
   method(distance_sq, p),
   method(scale, p, factor)
)

TEST_CASE("ComponentProxy — record arg flattened as scalars", "[component]") {
   PointService svc;
   // Point{3, 4} flattens to (i32=3, i32=4)
   auto result = psizam::ComponentProxy<PointService>::call<&PointService::distance_sq>(
      &svc, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   CHECK(static_cast<int32_t>(result) == 25);  // 9 + 16
}

TEST_CASE("ComponentProxy — record arg + scalar arg", "[component]") {
   PointService svc;
   // Point{2, 3} + factor=5 → flattens to (i32=2, i32=3, i32=5)
   // Result Point{10, 15} → flattens to 2 values, but we return only first (x=10)
   auto result = psizam::ComponentProxy<PointService>::call<&PointService::scale>(
      &svc, 2, 3, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   CHECK(static_cast<int32_t>(result) == 10);  // first flat value: x
}

// ── ComponentProxy — string arg dispatch via memory context ──────────────────

struct NameService {
   uint32_t name_length(std::string name) { return static_cast<uint32_t>(name.size()); }
   bool     is_alice(std::string name) { return name == "Alice"; }
};
PSIO_REFLECT(NameService,
   method(name_length, name),
   method(is_alice, name)
)

TEST_CASE("ComponentProxy — string arg via call_with_memory", "[component]") {
   // Lower the string arg into a buffer using canonical ABI
   psizam::buffer_lower_policy lp;
   std::string name = "Hello World";
   psizam::canonical_lower<std::string>(name, lp);

   // Pack flat values into slots: [ptr, len]
   psizam::flat_val slots[16] = {};
   for (size_t i = 0; i < lp.flat_values.size() && i < 16; i++)
      slots[i] = static_cast<psizam::flat_val>(lp.flat_values[i].i32);

   // Call with memory context pointing to the lower buffer
   NameService svc;
   auto result = psizam::ComponentProxy<NameService>::call_with_memory<&NameService::name_length>(
      &svc, slots, lp.buf.data());

   CHECK(static_cast<uint32_t>(result) == 11);
}

TEST_CASE("ComponentProxy — string comparison via call_with_memory", "[component]") {
   psizam::buffer_lower_policy lp;
   std::string name = "Alice";
   psizam::canonical_lower<std::string>(name, lp);

   psizam::flat_val slots[16] = {};
   for (size_t i = 0; i < lp.flat_values.size() && i < 16; i++)
      slots[i] = static_cast<psizam::flat_val>(lp.flat_values[i].i32);

   NameService svc;
   auto result = psizam::ComponentProxy<NameService>::call_with_memory<&NameService::is_alice>(
      &svc, slots, lp.buf.data());

   CHECK(static_cast<uint32_t>(result) == 1);  // true
}

// ── ComponentProxy — Person arg dispatch (record with string) ────────────────

struct PersonService {
   uint32_t    get_age(Person p) { return p.age; }
   std::string get_name(Person p) { return p.name; }
};
PSIO_REFLECT(PersonService,
   method(get_age, p),
   method(get_name, p)
)

TEST_CASE("ComponentProxy — Person arg (string+scalar record)", "[component]") {
   // Lower Person into buffer: flattens to [name_ptr, name_len, age]
   psizam::buffer_lower_policy lp;
   Person p{"Alice", 30};
   psizam::canonical_lower<Person>(p, lp);
   CHECK(lp.flat_values.size() == 3);

   psizam::flat_val slots[16] = {};
   for (size_t i = 0; i < lp.flat_values.size() && i < 16; i++)
      slots[i] = static_cast<psizam::flat_val>(lp.flat_values[i].i32);

   PersonService svc;
   auto result = psizam::ComponentProxy<PersonService>::call_with_memory<&PersonService::get_age>(
      &svc, slots, lp.buf.data());

   CHECK(static_cast<uint32_t>(result) == 30);
}

// ── Method flat count tests ──────────────────────────────────────────────────

TEST_CASE("method param flat counts", "[component]") {
   using psio::canonical_flat_count_v;

   // add(i32, i32) → 2 flat params
   using AddM = psio::MemberPtrType<decltype(&Calculator::add)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(AddM::SimplifiedArgTypes{}) == 2);

   // divide(f64, f64) → 2 flat params
   using DivM = psio::MemberPtrType<decltype(&Calculator::divide)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(DivM::SimplifiedArgTypes{}) == 2);

   // name_length(string) → 2 flat params (ptr + len)
   using NlM = psio::MemberPtrType<decltype(&NameService::name_length)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(NlM::SimplifiedArgTypes{}) == 2);

   // get_age(Person) → 3 flat params (name_ptr + name_len + age)
   using GaM = psio::MemberPtrType<decltype(&PersonService::get_age)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(GaM::SimplifiedArgTypes{}) == 3);

   // distance_sq(Point) → 2 flat params (x + y)
   using DsM = psio::MemberPtrType<decltype(&PointService::distance_sq)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(DsM::SimplifiedArgTypes{}) == 2);

   // scale(Point, i32) → 3 flat params (x + y + factor)
   using ScM = psio::MemberPtrType<decltype(&PointService::scale)>;
   STATIC_REQUIRE(psizam::detail_component::param_flat_count(ScM::SimplifiedArgTypes{}) == 3);
}

TEST_CASE("method result flat counts", "[component]") {
   // void → 0
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<void>() == 0);

   // i32 → 1
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<int32_t>() == 1);

   // f64 → 1
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<double>() == 1);

   // string → 2 (ptr + len)
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<std::string>() == 2);

   // Point → 2 (x + y)
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<Point>() == 2);

   // Person → 3 (name_ptr + name_len + age)
   STATIC_REQUIRE(psizam::detail_component::result_flat_count<Person>() == 3);
}

// ── WIT generation tests ─────────────────────────────────────────────────────

TEST_CASE("generate_component_wit — scalar calculator", "[component]") {
   auto wit = psizam::generate_component_wit<Calculator>("test:calc@1.0.0");
   CHECK(wit.find("package test:calc@1.0.0;") != std::string::npos);
   CHECK(wit.find("interface calculator") != std::string::npos);
   CHECK(wit.find("add: func") != std::string::npos);
   CHECK(wit.find("mul: func") != std::string::npos);
   CHECK(wit.find("divide: func") != std::string::npos);
   CHECK(wit.find("s32") != std::string::npos);
   CHECK(wit.find("f64") != std::string::npos);
   CHECK(wit.find("world calculator-world") != std::string::npos);
   CHECK(wit.find("export calculator;") != std::string::npos);
}

// ── WIT binary encoding tests ────────────────────────────────────────────────

struct Item {
   uint64_t    id;
   std::string name;
   std::string category;
   uint32_t    price_cents;
   bool        in_stock;
   std::vector<std::string> tags;
   std::optional<uint32_t>  weight_grams;
};
PSIO_REFLECT(Item, id, name, category, price_cents, in_stock, tags, weight_grams)

struct StockResult {
   uint32_t item_id;
   uint32_t old_quantity;
   uint32_t new_quantity;
};
PSIO_REFLECT(StockResult, item_id, old_quantity, new_quantity)

struct SearchQuery {
   std::string text;
   uint32_t    max_results;
   std::optional<uint32_t> min_price;
   std::optional<uint32_t> max_price;
   std::vector<std::string> categories;
};
PSIO_REFLECT(SearchQuery, text, max_results, min_price, max_price, categories)

struct SearchResponse {
   std::vector<Item> items;
   uint64_t          total_count;
   bool              has_more;
};
PSIO_REFLECT(SearchResponse, items, total_count, has_more)

struct BulkResult {
   uint32_t inserted;
   uint32_t failed;
   std::vector<std::string> errors;
};
PSIO_REFLECT(BulkResult, inserted, failed, errors)

struct InventoryApi {
   std::optional<Item> get_item(uint32_t item_id) { return std::nullopt; }
   std::vector<Item>   list_items(std::string category) { return {}; }
   uint64_t            add_item(Item item) { return 0; }
   StockResult         update_stock(uint32_t item_id, int32_t delta) { return {}; }
   SearchResponse      search(SearchQuery query) { return {}; }
   BulkResult          bulk_import(std::vector<Item> items) { return {}; }
   void                ping() {}
};
PSIO_REFLECT(InventoryApi,
   method(get_item, item_id),
   method(list_items, category),
   method(add_item, item),
   method(update_stock, item_id, delta),
   method(search, query),
   method(bulk_import, items),
   method(ping)
)

TEST_CASE("generate_wit_binary — produces valid Component Model binary", "[component]") {
   auto binary = psio::generate_wit_binary<InventoryApi>("test:inventory@1.0.0");

   // Must start with Component Model header
   REQUIRE(binary.size() >= 8);
   CHECK(binary[0] == 0x00);  // \0
   CHECK(binary[1] == 0x61);  // a
   CHECK(binary[2] == 0x73);  // s
   CHECK(binary[3] == 0x6d);  // m
   CHECK(binary[4] == 0x0d);  // version 13
   CHECK(binary[5] == 0x00);
   CHECK(binary[6] == 0x01);
   CHECK(binary[7] == 0x00);

   // Must contain wit-component-encoding custom section
   std::string as_str(binary.begin(), binary.end());
   CHECK(as_str.find("wit-component-encoding") != std::string::npos);

   // Must contain the interface export name
   CHECK(as_str.find("test:inventory/inventory-api@1.0.0") != std::string::npos);

   // Must contain record field names (kebab-case)
   CHECK(as_str.find("price-cents") != std::string::npos);
   CHECK(as_str.find("weight-grams") != std::string::npos);
   CHECK(as_str.find("item-id") != std::string::npos);

   // Must contain function names
   CHECK(as_str.find("get-item") != std::string::npos);
   CHECK(as_str.find("list-items") != std::string::npos);
   CHECK(as_str.find("bulk-import") != std::string::npos);
   CHECK(as_str.find("ping") != std::string::npos);

   // Write to a temp file for wasm-tools validation
   auto path = std::string("/tmp/cpp_wit_binary_test.wasm");
   {
      std::ofstream out(path, std::ios::binary);
      out.write(reinterpret_cast<const char*>(binary.data()),
                static_cast<std::streamsize>(binary.size()));
   }
   INFO("Binary written to " << path << " (" << binary.size() << " bytes)");
   INFO("Validate with: wasm-tools component wit " << path);
}

// ── PZAM_COMPONENT macro test ────────────────────────────────────────────────
// We can't use PZAM_COMPONENT in a test file that already has PSIO_REFLECT
// for the same types. So we test with a fresh type.

struct Adder {
   int32_t add(int32_t a, int32_t b) { return a + b; }
};
PZAM_COMPONENT(Adder, method(add, a, b))

TEST_CASE("PZAM_COMPONENT — generates callable export", "[component]") {
   // The macro generated:
   // - PSIO_REFLECT(Adder, method(add, a, b))
   // - static Adder _pzam_impl;
   // - extern "C" flat_val add(flat_val a0, ..., flat_val a15)
   //
   // We can call the generated 'add' function directly
   auto result = add(3, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
   CHECK(static_cast<int32_t>(result) == 8);
}

// ── Canonical ABI compile-time layout tests ──────────────────────────────────

TEST_CASE("canonical_size — scalars", "[canonical]") {
   STATIC_REQUIRE(psio::canonical_size_v<bool> == 1);
   STATIC_REQUIRE(psio::canonical_size_v<uint8_t> == 1);
   STATIC_REQUIRE(psio::canonical_size_v<int16_t> == 2);
   STATIC_REQUIRE(psio::canonical_size_v<uint32_t> == 4);
   STATIC_REQUIRE(psio::canonical_size_v<int32_t> == 4);
   STATIC_REQUIRE(psio::canonical_size_v<float> == 4);
   STATIC_REQUIRE(psio::canonical_size_v<uint64_t> == 8);
   STATIC_REQUIRE(psio::canonical_size_v<double> == 8);
}

TEST_CASE("canonical_align — scalars", "[canonical]") {
   STATIC_REQUIRE(psio::canonical_align_v<bool> == 1);
   STATIC_REQUIRE(psio::canonical_align_v<uint8_t> == 1);
   STATIC_REQUIRE(psio::canonical_align_v<int16_t> == 2);
   STATIC_REQUIRE(psio::canonical_align_v<uint32_t> == 4);
   STATIC_REQUIRE(psio::canonical_align_v<float> == 4);
   STATIC_REQUIRE(psio::canonical_align_v<uint64_t> == 8);
   STATIC_REQUIRE(psio::canonical_align_v<double> == 8);
}

TEST_CASE("canonical_size — string and vector", "[canonical]") {
   STATIC_REQUIRE(psio::canonical_size_v<std::string> == 8);
   STATIC_REQUIRE(psio::canonical_size_v<std::vector<int32_t>> == 8);
   STATIC_REQUIRE(psio::canonical_align_v<std::string> == 4);
   STATIC_REQUIRE(psio::canonical_align_v<std::vector<int32_t>> == 4);
}

TEST_CASE("canonical_size — Point (two i32s)", "[canonical]") {
   // Point = { i32 x, i32 y } → size=8, align=4
   STATIC_REQUIRE(psio::canonical_size_v<Point> == 8);
   STATIC_REQUIRE(psio::canonical_align_v<Point> == 4);
   STATIC_REQUIRE(psio::canonical_field_offset_v<Point, 0> == 0);
   STATIC_REQUIRE(psio::canonical_field_offset_v<Point, 1> == 4);
}

TEST_CASE("canonical_size — Person (string + u32)", "[canonical]") {
   // Person = { string name (8 bytes, align 4), u32 age (4 bytes, align 4) }
   // Layout: name at 0, age at 8 → size=12, align=4
   STATIC_REQUIRE(psio::canonical_size_v<Person> == 12);
   STATIC_REQUIRE(psio::canonical_align_v<Person> == 4);
   STATIC_REQUIRE(psio::canonical_field_offset_v<Person, 0> == 0);
   STATIC_REQUIRE(psio::canonical_field_offset_v<Person, 1> == 8);
}

TEST_CASE("canonical_flat_count — various types", "[canonical]") {
   STATIC_REQUIRE(psio::canonical_flat_count_v<int32_t> == 1);
   STATIC_REQUIRE(psio::canonical_flat_count_v<std::string> == 2);
   STATIC_REQUIRE(psio::canonical_flat_count_v<Point> == 2);       // x + y
   STATIC_REQUIRE(psio::canonical_flat_count_v<Person> == 3);      // name(2) + age(1)
}

// ── Lower/lift round-trip tests ──────────────────────────────────────────────

TEST_CASE("lower/lift round-trip — scalar i32", "[canonical]") {
   psizam::buffer_lower_policy lp;
   psizam::canonical_lower<int32_t>(42, lp);
   CHECK(lp.flat_values.size() == 1);
   CHECK(lp.flat_values[0].i32 == 42);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<int32_t>(rp);
   CHECK(result == 42);
}

TEST_CASE("lower/lift round-trip — string", "[canonical]") {
   psizam::buffer_lower_policy lp;
   std::string hello = "hello world";
   psizam::canonical_lower<std::string>(hello, lp);
   CHECK(lp.flat_values.size() == 2);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<std::string>(rp);
   CHECK(result == "hello world");
}

TEST_CASE("lower/lift round-trip — Point", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Point p{100, 200};
   psizam::canonical_lower(p, lp);
   CHECK(lp.flat_values.size() == 2);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<Point>(rp);
   CHECK(result.x == 100);
   CHECK(result.y == 200);
}

TEST_CASE("lower/lift round-trip — Person (string + scalar)", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Person p{"Alice", 30};
   psizam::canonical_lower(p, lp);
   CHECK(lp.flat_values.size() == 3);  // name_ptr, name_len, age

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<Person>(rp);
   CHECK(result.name == "Alice");
   CHECK(result.age == 30);
}

TEST_CASE("lower/lift round-trip — Order (vectors)", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Order o{"Bob", {10, 20, 30}, {"apple", "banana"}};
   psizam::canonical_lower(o, lp);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<Order>(rp);
   CHECK(result.customer == "Bob");
   CHECK(result.quantities == std::vector<int32_t>{10, 20, 30});
   CHECK(result.items == std::vector<std::string>{"apple", "banana"});
}

TEST_CASE("lower/lift round-trip — Nested (recursive)", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Nested n{{"Carol", 25}, {{1, 2}, {3, 4}}, Point{10, 20}};
   psizam::canonical_lower(n, lp);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<Nested>(rp);
   CHECK(result.person.name == "Carol");
   CHECK(result.person.age == 25);
   CHECK(result.path.size() == 2);
   CHECK(result.path[0].x == 1);
   CHECK(result.path[1].y == 4);
   REQUIRE(result.origin.has_value());
   CHECK(result.origin->x == 10);
   CHECK(result.origin->y == 20);
}

TEST_CASE("lower/lift round-trip — optional nullopt", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Nested n{{"Dave", 40}, {}, std::nullopt};
   psizam::canonical_lower(n, lp);

   psizam::buffer_lift_policy rp(lp.buf.data(), lp.buf.size(), lp.flat_values.data());
   auto result = psizam::canonical_lift<Nested>(rp);
   CHECK(result.person.name == "Dave");
   CHECK(result.path.empty());
   CHECK_FALSE(result.origin.has_value());
}

// ── Validate tests ───────────────────────────────────────────────────────────

TEST_CASE("canonical_validate — valid buffer", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Person p{"Eve", 28};
   psio::canonical_lower_fields(p, lp, lp.alloc(psio::canonical_align_v<Person>, psio::canonical_size_v<Person>));

   CHECK(psio::canonical_validate<Person>(lp.buf.data(), lp.buf.size(), 0));
}

TEST_CASE("canonical_validate — truncated buffer fails", "[canonical]") {
   psizam::buffer_lower_policy lp;
   Person p{"Eve", 28};
   psio::canonical_lower_fields(p, lp, lp.alloc(psio::canonical_align_v<Person>, psio::canonical_size_v<Person>));

   // Truncate the buffer
   CHECK_FALSE(psio::canonical_validate<Person>(lp.buf.data(), 4, 0));
}

// ── Rebase tests ─────────────────────────────────────────────────────────────

TEST_CASE("canonical_rebase — shift pointers", "[canonical]") {
   // Lower at base 0
   psizam::buffer_lower_policy lp;
   Person p{"Frank", 35};
   uint32_t record_offset = lp.alloc(psio::canonical_align_v<Person>, psio::canonical_size_v<Person>);
   psio::canonical_lower_fields(p, lp, record_offset);

   // Validate at base 0
   CHECK(psio::canonical_validate<Person>(lp.buf.data(), lp.buf.size(), 0));

   // Rebase by delta=1000
   int32_t delta = 1000;
   psio::canonical_rebase<Person>(lp.buf.data(), 0, delta);

   // Simulate placing into a larger buffer at offset 1000
   std::vector<uint8_t> guest_mem(2000, 0);
   std::memcpy(guest_mem.data() + delta, lp.buf.data(), lp.buf.size());

   // Lift from the rebased position
   psizam::buffer_lift_policy rp(guest_mem.data(), guest_mem.size(), nullptr);
   auto result = psio::canonical_lift_fields<Person>(rp, static_cast<uint32_t>(delta));
   CHECK(result.name == "Frank");
   CHECK(result.age == 35);
}

TEST_CASE("lower with base offset — pre-rebased", "[canonical]") {
   // Lower with base=500, pointers already correct for that offset
   psizam::buffer_lower_policy lp(500);
   Person p{"Grace", 42};
   uint32_t record_offset = lp.alloc(psio::canonical_align_v<Person>, psio::canonical_size_v<Person>);
   psio::canonical_lower_fields(p, lp, record_offset);

   // Place into guest memory at offset 500
   std::vector<uint8_t> guest_mem(1000, 0);
   std::memcpy(guest_mem.data() + 500, lp.buf.data(), lp.buf.size());

   // Lift from offset 500 — pointers should be correct
   psizam::buffer_lift_policy rp(guest_mem.data(), guest_mem.size(), nullptr);
   auto result = psio::canonical_lift_fields<Person>(rp, 500);
   CHECK(result.name == "Grace");
   CHECK(result.age == 42);
}
