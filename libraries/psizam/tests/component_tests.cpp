#include <catch2/catch.hpp>
#include <psio/ctype.hpp>
#include <psio/wit_encode.hpp>
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

// ── is_viewable tests ────────────────────────────────────────────────────────

TEST_CASE("is_viewable — scalar struct", "[ctype]") {
   STATIC_REQUIRE(psio::is_viewable<Point>);
}

TEST_CASE("is_viewable — struct with string", "[ctype]") {
   STATIC_REQUIRE(psio::is_viewable<Person>);
}

TEST_CASE("is_viewable — struct with vector<scalar>", "[ctype]") {
   // Has vector<string> which needs descriptor alloc → NOT viewable
   STATIC_REQUIRE_FALSE(psio::is_viewable<Order>);
}

TEST_CASE("is_viewable — nested struct with vector<compound>", "[ctype]") {
   // Has vector<Point> (compound) → NOT viewable
   STATIC_REQUIRE_FALSE(psio::is_viewable<Nested>);
}

// ── CView construction and promotion ─────────────────────────────────────────

TEST_CASE("CView<Point> — scalar struct round-trip", "[ctype]") {
   Point p{42, 99};
   psio::CView<Point> view(p);

   CHECK(view.get<0>() == 42);
   CHECK(view.get<1>() == 99);

   Point promoted = view.promote();
   CHECK(promoted.x == 42);
   CHECK(promoted.y == 99);
}

TEST_CASE("CView<Person> — struct with string", "[ctype]") {
   Person p{"Alice", 30};
   psio::CView<Person> view(p);

   CHECK(view.get<0>() == "Alice");
   CHECK(view.get<1>() == 30);

   Person promoted = view.promote();
   CHECK(promoted.name == "Alice");
   CHECK(promoted.age == 30);
}

TEST_CASE("CView<Order> — struct with vector<string>", "[ctype]") {
   Order o{"Bob", {1, 2, 3}, {"apple", "banana", "cherry"}};
   psio::CView<Order> view(o);

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

TEST_CASE("CView<Nested> — nested struct with optional", "[ctype]") {
   Nested n{{"Carol", 25}, {{1, 2}, {3, 4}}, Point{10, 20}};
   psio::CView<Nested> view(n);

   // person field
   CHECK(view.get<0>().get<0>() == "Carol");
   CHECK(view.get<0>().get<1>() == 25);

   // path field (vector<Point> → span<CView<Point>>)
   CHECK(view.get<1>().size() == 2);
   CHECK(view.get<1>()[0].get<0>() == 1);
   CHECK(view.get<1>()[1].get<1>() == 4);

   // origin field (optional<Point> → const CView<Point>*)
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

TEST_CASE("CView<Nested> — optional is null", "[ctype]") {
   Nested n{{"Dave", 40}, {}, std::nullopt};
   psio::CView<Nested> view(n);

   CHECK(view.get<2>() == nullptr);

   Nested promoted = view.promote();
   CHECK_FALSE(promoted.origin.has_value());
}

// ── COwned tests ─────────────────────────────────────────────────────────────

TEST_CASE("COwned — wraps CView and tracks allocations", "[ctype]") {
   Person p{"Eve", 28};
   psio::CView<Person> view(p);
   psio::COwned<Person> owned(std::move(view));

   CHECK(owned.get<0>() == "Eve");
   CHECK(owned.get<1>() == 28);

   Person promoted = owned.promote();
   CHECK(promoted.name == "Eve");
   CHECK(promoted.age == 28);
}

// ── ComponentProxy tests ─────────────────────────────────────────────────────

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
