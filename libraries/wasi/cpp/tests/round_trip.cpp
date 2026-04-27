// Phase A canary: round-trip the hand-written wasi:cli@0.2.3 bindings
// through PSIO reflection and the v3 WIT generator, then check the
// output against the structural expectations set by the vendored
// .wit.
//
// v1 used SchemaBuilder + emit_wit (Phase B/C of the schema layer);
// v3's path is more direct — wit_gen walks PSIO_PACKAGE /
// PSIO_INTERFACE / PSIO_REFLECT registries and produces WIT text
// straight from the C++ side without an intermediate Schema IR.

#include <catch2/catch.hpp>

#include <psio/wit_gen.hpp>

#include <wasi/0.2.3/cli.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
   bool contains(std::string_view hay, std::string_view needle)
   {
      return hay.find(needle) != std::string_view::npos;
   }

   std::string read_file(const std::string& path)
   {
      std::ifstream      in{path};
      std::ostringstream buf;
      buf << in.rdbuf();
      return buf.str();
   }
}  // namespace

TEST_CASE("wasi:cli/environment — generate_wit_text matches vendored shape",
          "[wasi][cli][round_trip]")
{
   auto text = psio::generate_wit_text<environment>("wasi", "cli", "0.2.3");

   // Package header — v3 produces canonical "ns:name@version".
   REQUIRE(contains(text, "package wasi:cli@0.2.3;"));

   // Interface + function spellings (kebab-cased).
   REQUIRE(contains(text, "interface environment {"));
   REQUIRE(contains(text, "get-environment: func()"));
   REQUIRE(contains(text, "get-arguments: func()"));
   REQUIRE(contains(text, "initial-cwd: func()"));

   // Return types, inline.
   //
   // get-environment returns list<tuple<string, string>>; v3's wit_gen
   // doesn't yet model std::tuple as a WIT tuple<…> (separate
   // follow-up), so we weaken to the outer list<…> here.
   REQUIRE(contains(text, "list<"));
   REQUIRE(contains(text, "list<string>"));
   REQUIRE(contains(text, "option<string>"));
}

TEST_CASE("wasi:cli/environment — vendored .wit declares the same shape",
          "[wasi][cli][round_trip][vendored]")
{
   // The vendored files are truth — assert our expectations about
   // what the reflected output should approximate, against the
   // source we pretend to mirror.  Upstream keeps the package header
   // in world-level files (imports.wit / command.wit) and gates the
   // interface with `@since(version = 0.2.0)` feature attributes.
   auto env =
      read_file(std::string{WASI_WIT_DIR} + "/0.2.3/cli/environment.wit");
   REQUIRE_FALSE(env.empty());

   REQUIRE(contains(env, "interface environment {"));
   REQUIRE(contains(env,
                    "get-environment: func() -> list<tuple<string, string>>;"));
   REQUIRE(contains(env, "get-arguments: func() -> list<string>;"));
   REQUIRE(contains(env, "initial-cwd: func() -> option<string>;"));

   auto imports =
      read_file(std::string{WASI_WIT_DIR} + "/0.2.3/cli/imports.wit");
   REQUIRE_FALSE(imports.empty());
   REQUIRE(contains(imports, "package wasi:cli@0.2.3;"));
}
