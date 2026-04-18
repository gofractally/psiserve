// Phase A canary: round-trip the hand-written wasi:cli@0.2.3 bindings
// through PSIO reflection and emit_wit, then check the output against
// the structural expectations set by the vendored .wit.
//
// Byte-equal diff is still out of reach — emit_wit currently renders
// packages as `name@version;` and the PSIO_PACKAGE macro only accepts
// C++ identifiers (no colon), so `wasi:cli` can't round-trip yet.
// This test pins the invariants we DO expect to hold so any accidental
// regression in either side (the header or emit_wit) is surfaced
// immediately.

#include <catch2/catch.hpp>

#include <psio/emit_wit.hpp>
#include <psio/schema.hpp>

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
      std::ifstream in{path};
      std::ostringstream buf;
      buf << in.rdbuf();
      return buf.str();
   }
}  // namespace

// Wrap the vendored interface in a minimal world so SchemaBuilder's
// public entry point (insert_world) can walk it. Exists only for the
// test — production composition happens inside psiserve's Linker.
PSIO_WORLD(cli_env_canary,
           imports(),
           exports(::environment))

TEST_CASE("wasi:cli/environment — header reflects into a Schema",
          "[wasi][cli][round_trip]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio::detail::cli_env_canary_world_tag>()
                     .build();

   REQUIRE(schema.package.name == "wasi_cli");
   REQUIRE(schema.package.version == "0.2.3");

   REQUIRE(schema.interfaces.size() == 1);
   const auto& iface = schema.interfaces[0];
   REQUIRE(iface.name == "environment");
   REQUIRE(iface.type_names.empty());
   REQUIRE(iface.funcs.size() == 3);
   REQUIRE(iface.funcs[0].name == "get_environment");
   REQUIRE(iface.funcs[1].name == "get_arguments");
   REQUIRE(iface.funcs[2].name == "initial_cwd");
}

TEST_CASE("wasi:cli/environment — emit_wit matches vendored shape",
          "[wasi][cli][round_trip][emit_wit]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio::detail::cli_env_canary_world_tag>()
                     .build();
   auto text = S::emit_wit(schema);

   // Package header. Drift from canonical `wasi:cli` noted at the top
   // of this file — pinning the current output protects against
   // silent regressions until PSIO_PACKAGE can carry a namespace.
   REQUIRE(contains(text, "package wasi_cli@0.2.3;"));

   // Interface + function spellings (kebab-cased).
   REQUIRE(contains(text, "interface environment {"));
   REQUIRE(contains(text, "get-environment: func()"));
   REQUIRE(contains(text, "get-arguments: func()"));
   REQUIRE(contains(text, "initial-cwd: func()"));

   // Return types, inline.
   REQUIRE(contains(text, "list<tuple<string, string>>"));
   REQUIRE(contains(text, "list<string>"));
   REQUIRE(contains(text, "option<string>"));

   // World envelope (the canary wrapper).
   REQUIRE(contains(text, "world cli-env-canary {"));
   REQUIRE(contains(text, "export environment;"));
}

TEST_CASE("wasi:cli/environment — vendored .wit declares the same shape",
          "[wasi][cli][round_trip][vendored]")
{
   // The vendored files are truth — assert our expectations about
   // what the reflected output should approximate, against the
   // source we pretend to mirror. If a regenerated upstream drops
   // one of these strings the header is out-of-date and needs an
   // update. Upstream keeps the package header in world-level files
   // (imports.wit / command.wit) and gates the interface with
   // `@since(version = 0.2.0)` feature attributes.
   auto env = read_file(std::string{WASI_WIT_DIR} + "/0.2.3/cli/environment.wit");
   REQUIRE_FALSE(env.empty());

   REQUIRE(contains(env, "interface environment {"));
   REQUIRE(contains(env, "get-environment: func() -> list<tuple<string, string>>;"));
   REQUIRE(contains(env, "get-arguments: func() -> list<string>;"));
   REQUIRE(contains(env, "initial-cwd: func() -> option<string>;"));

   auto imports = read_file(std::string{WASI_WIT_DIR} + "/0.2.3/cli/imports.wit");
   REQUIRE_FALSE(imports.empty());
   REQUIRE(contains(imports, "package wasi:cli@0.2.3;"));
}
