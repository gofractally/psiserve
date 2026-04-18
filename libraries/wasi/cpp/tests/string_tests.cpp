// Host-side tests for wasi::string. On native builds the allocator
// routes through malloc/free, so these tests exercise the ownership
// and conversion semantics without a live wasm guest. On guest builds
// the same API maps through cabi_realloc.

#include <catch2/catch.hpp>

#include <wasi/string.hpp>

#include <string>
#include <string_view>
#include <utility>

TEST_CASE("wasi::string default-constructs empty", "[wasi][string]")
{
   wasi::string s;
   REQUIRE(s.empty());
   REQUIRE(s.size() == 0);
   REQUIRE(s.data() == nullptr);
}

TEST_CASE("wasi::string deep-copies from string_view", "[wasi][string]")
{
   std::string   src = "hello world";
   wasi::string  s{std::string_view{src}};

   REQUIRE(s.size() == src.size());
   REQUIRE(std::string_view{s} == src);

   // Mutating the source must not affect the copy.
   src[0] = 'X';
   REQUIRE(std::string_view{s} == "hello world");
}

TEST_CASE("wasi::string is move-only", "[wasi][string]")
{
   STATIC_REQUIRE(!std::is_copy_constructible_v<wasi::string>);
   STATIC_REQUIRE(!std::is_copy_assignable_v<wasi::string>);
   STATIC_REQUIRE(std::is_nothrow_move_constructible_v<wasi::string>);
   STATIC_REQUIRE(std::is_nothrow_move_assignable_v<wasi::string>);
}

TEST_CASE("wasi::string move transfers ownership", "[wasi][string]")
{
   wasi::string a{std::string_view{"payload"}};
   const char*  raw = a.data();
   auto         n   = a.size();

   wasi::string b = std::move(a);
   REQUIRE(b.data() == raw);
   REQUIRE(b.size() == n);
   REQUIRE(a.empty());
   REQUIRE(a.data() == nullptr);
}

TEST_CASE("wasi::string move-assigns and frees prior buffer", "[wasi][string]")
{
   wasi::string a{std::string_view{"first"}};
   wasi::string b{std::string_view{"second"}};
   b = std::move(a);
   REQUIRE(std::string_view{b} == "first");
   REQUIRE(a.empty());
}

TEST_CASE("wasi::string implicitly converts to string_view", "[wasi][string]")
{
   wasi::string     s{std::string_view{"borrow me"}};
   std::string_view v = s;
   REQUIRE(v == "borrow me");
   REQUIRE(v.data() == s.data());  // same storage, no copy
}

TEST_CASE("wasi::string release + adopt round-trips without copy", "[wasi][string]")
{
   // Simulates the canonical-ABI handoff: release() surrenders the
   // buffer to the two flat slots; adopt() reconstructs the owner on
   // the receiving side. No allocation happens in between.
   wasi::string orig{std::string_view{"handoff"}};
   const char*  raw_ptr = orig.data();
   auto         raw_len = orig.size();

   auto handoff = orig.release();
   REQUIRE(orig.empty());
   REQUIRE(handoff.ptr == raw_ptr);
   REQUIRE(handoff.len == raw_len);

   wasi::string adopted = wasi::string::adopt(handoff.ptr, handoff.len);
   REQUIRE(adopted.data() == raw_ptr);
   REQUIRE(std::string_view{adopted} == "handoff");
}

TEST_CASE("wasi::string layout matches canonical ABI {ptr, len}",
          "[wasi][string]")
{
   // The lifted form on the wire is two i32/i64 slots in order. Any
   // reordering or padding here would break zero-copy lift/lower.
   STATIC_REQUIRE(sizeof(wasi::string)
                  == sizeof(const char*) + sizeof(std::size_t));
   STATIC_REQUIRE(std::is_standard_layout_v<wasi::string>);
}
