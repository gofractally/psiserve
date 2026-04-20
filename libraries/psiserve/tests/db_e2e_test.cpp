// db_e2e_test.cpp — End-to-end test: WASM guest calls psi::db host imports.

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/db_host.hpp>
#include <psizam/hosted.hpp>

#include "db_guest_wasm.hpp"

#include <filesystem>

PSIO_PACKAGE(test, "0.0.0");

struct db_test_guest
{
   static uint32_t run_test();
};

PSIO_INTERFACE(db_test_guest, types(),
   funcs(func(run_test)))

TEST_CASE("db_host E2E WASM guest")
{
   namespace fs = std::filesystem;
   using namespace psiserve;

   auto dir = fs::temp_directory_path() / "db_e2e_test_segments";
   fs::remove_all(dir);

   db_host host;
   host.db = psitri::database::open(dir);
   host.ws = host.db->start_write_session();
   host.name_to_root["testdb"] = 0;

   using rhf = psizam::registered_host_functions<db_host>;
   rhf::template add<&db_host::fd_write>("wasi_snapshot_preview1", "fd_write");

   psizam::hosted<db_host, psizam::interpreter> vm{db_guest_wasm_bytes, host};

   vm.be.call(host, std::string_view{}, std::string_view{"_initialize"});

   auto result = vm.as<db_test_guest>().run_test();
   REQUIRE(result == 0);

   REQUIRE(host.databases.live_count() == 0);
   REQUIRE(host.transactions.live_count() == 0);
   REQUIRE(host.tables.live_count() == 0);
   REQUIRE(host.cursors.live_count() == 0);

   host.ws.reset();
   host.db.reset();
   fs::remove_all(dir);
}
