// Phase 5 — CPO dispatch + format_tag_base scoped sugar.
//
// Uses a toy format `test_fmt` that implements every CPO for `int`
// via hidden-friend tag_invoke overloads. The test asserts that both
// call forms dispatch to the same body:
//
//   psio3::encode(test_fmt{}, 42, sink);     // generic CPO
//   test_fmt::encode(42, sink);              // scoped sugar

#include <psio3/cpo.hpp>
#include <psio3/error.hpp>
#include <psio3/format_tag_base.hpp>

#include <catch.hpp>

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

   // ── Toy format ─────────────────────────────────────────────────────────

   struct test_fmt : psio3::format_tag_base<test_fmt>
   {
      // encode int → little-endian bytes into a vector<char> sink.
      friend void tag_invoke(decltype(psio3::encode), test_fmt,
                             int v, std::vector<char>& sink)
      {
         for (int i = 0; i < 4; ++i)
            sink.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
      }

      // encode int → fresh vector<char>.
      friend std::vector<char> tag_invoke(decltype(psio3::encode), test_fmt,
                                          int v)
      {
         std::vector<char> out;
         tag_invoke(psio3::encode, test_fmt{}, v, out);
         return out;
      }

      // decode bytes → int.
      friend int tag_invoke(decltype(psio3::decode<int>), test_fmt, int*,
                            std::span<const char> b)
      {
         int out = 0;
         for (int i = 0; i < 4 && i < static_cast<int>(b.size()); ++i)
            out |= static_cast<unsigned char>(b[i]) << (i * 8);
         return out;
      }

      // size of an int encoding is always 4 bytes.
      friend std::size_t tag_invoke(decltype(psio3::size_of), test_fmt,
                                    const int&)
      {
         return 4;
      }

      // validate: accept any 4+ byte buffer.
      friend psio3::codec_status tag_invoke(decltype(psio3::validate<int>),
                                            test_fmt, int*,
                                            std::span<const char> b)
      {
         return b.size() >= 4
                  ? psio3::codec_ok()
                  : psio3::codec_fail("buffer too small", 0, "test_fmt");
      }

      // validate_strict: structural only for the toy format (no
      // semantic specs attached).
      friend psio3::codec_status tag_invoke(decltype(psio3::validate_strict<int>),
                                            test_fmt, int*,
                                            std::span<const char> b)
      {
         return tag_invoke(psio3::validate<int>, test_fmt{}, static_cast<int*>(nullptr), b);
      }

      // make_boxed: decode + std::make_unique.
      friend std::unique_ptr<int> tag_invoke(decltype(psio3::make_boxed<int>),
                                             test_fmt, int*,
                                             std::span<const char> b)
      {
         return std::make_unique<int>(
            tag_invoke(psio3::decode<int>, test_fmt{}, static_cast<int*>(nullptr), b));
      }
   };

}  // namespace

TEST_CASE("psio3::encode CPO dispatches via ADL on the format tag",
          "[cpo]")
{
   std::vector<char> sink;
   psio3::encode(test_fmt{}, 0x04030201, sink);
   REQUIRE(sink.size() == 4);
   REQUIRE(static_cast<unsigned char>(sink[0]) == 0x01);
   REQUIRE(static_cast<unsigned char>(sink[3]) == 0x04);
}

TEST_CASE("psio3::encode return-value overload", "[cpo]")
{
   auto out = psio3::encode(test_fmt{}, 42);
   REQUIRE(out.size() == 4);
   REQUIRE(static_cast<unsigned char>(out[0]) == 42);
}

TEST_CASE("psio3::decode round-trips with encode", "[cpo]")
{
   auto out = psio3::encode(test_fmt{}, 0x12345678);
   int  v   = psio3::decode<int>(test_fmt{}, std::span<const char>{out});
   REQUIRE(v == 0x12345678);
}

TEST_CASE("psio3::size_of queries the encoded size", "[cpo]")
{
   REQUIRE(psio3::size_of(test_fmt{}, 0) == 4);
   REQUIRE(psio3::size_of(test_fmt{}, 0x7FFFFFFF) == 4);
}

TEST_CASE("psio3::validate accepts well-sized bytes, rejects small",
          "[cpo]")
{
   char good[4]{};
   char tiny[2]{};

   auto ok = psio3::validate<int>(test_fmt{}, std::span<const char>{good, 4});
   REQUIRE(ok.ok());

   auto bad = psio3::validate<int>(test_fmt{}, std::span<const char>{tiny, 2});
   REQUIRE(!bad.ok());
   REQUIRE(bad.error().format_name == "test_fmt");
}

TEST_CASE("psio3::validate_strict delegates to structural for the toy format",
          "[cpo]")
{
   char good[4]{};
   auto s = psio3::validate_strict<int>(test_fmt{}, std::span<const char>{good, 4});
   REQUIRE(s.ok());
}

TEST_CASE("psio3::make_boxed returns a heap-allocated unique_ptr<T>",
          "[cpo]")
{
   auto out = psio3::encode(test_fmt{}, 99);
   auto box = psio3::make_boxed<int>(test_fmt{}, std::span<const char>{out});
   REQUIRE(box != nullptr);
   REQUIRE(*box == 99);
}

// ── format_tag_base scoped sugar ─────────────────────────────────────────

TEST_CASE("test_fmt::encode(v, sink) scoped sugar matches generic CPO",
          "[cpo][format_tag_base]")
{
   std::vector<char> a, b;
   psio3::encode(test_fmt{}, 0xABCDEF, a);
   test_fmt::encode(0xABCDEF, b);
   REQUIRE(a == b);
}

TEST_CASE("test_fmt::encode(v) scoped sugar matches return-value CPO",
          "[cpo][format_tag_base]")
{
   auto a = psio3::encode(test_fmt{}, 777);
   auto b = test_fmt::encode(777);
   REQUIRE(a == b);
}

TEST_CASE("test_fmt::decode<T>(bytes) scoped sugar matches generic",
          "[cpo][format_tag_base]")
{
   auto out = test_fmt::encode(12345);
   int  v1  = psio3::decode<int>(test_fmt{}, std::span<const char>{out});
   int  v2  = test_fmt::decode<int>(std::span<const char>{out});
   REQUIRE(v1 == 12345);
   REQUIRE(v2 == 12345);
}

TEST_CASE("test_fmt::validate<T>(bytes) scoped sugar matches generic",
          "[cpo][format_tag_base]")
{
   char good[4]{};
   auto s1 = psio3::validate<int>(test_fmt{}, std::span<const char>{good, 4});
   auto s2 = test_fmt::validate<int>(std::span<const char>{good, 4});
   REQUIRE(s1.ok());
   REQUIRE(s2.ok());
}

TEST_CASE("test_fmt::make_boxed<T>(bytes) scoped sugar matches generic",
          "[cpo][format_tag_base]")
{
   auto out = test_fmt::encode(8675309);
   auto box = test_fmt::make_boxed<int>(std::span<const char>{out});
   REQUIRE(*box == 8675309);
}

TEST_CASE("scoped sugar's [[nodiscard]] on validate is preserved",
          "[cpo][format_tag_base][nodiscard]")
{
   char good[4]{};
   auto s = test_fmt::validate<int>(std::span<const char>{good, 4});
   REQUIRE(s.ok());
}
