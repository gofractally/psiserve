#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <pfs/cid.hpp>
#include <pfs/config.hpp>
#include <pfs/keys.hpp>
#include <pfs/schema.hpp>
#include <pfs/store.hpp>
#include <pfs/unixfs.hpp>

#include <pcrypt/sha256.hpp>
#include <psio/fracpack.hpp>

#include <psitri/database.hpp>
#include <psitri/database_impl.hpp>

#include <cstring>
#include <filesystem>

namespace
{
   std::filesystem::path test_db_path()
   {
      auto p = std::filesystem::temp_directory_path() / "pfs_test_db";
      std::filesystem::remove_all(p);
      return p;
   }

   std::vector<uint8_t> bytes(std::initializer_list<uint8_t> il) { return {il}; }
}  // namespace

// ════════════════════════════════════════════════════════════════════
// CID tests
// ════════════════════════════════════════════════════════════════════

TEST_CASE("CID from digest round-trips", "[cid]")
{
   std::array<uint8_t, 32> digest = {};
   for (uint8_t i = 0; i < 32; ++i)
      digest[i] = i;

   auto c = pfs::cid::from_digest(digest);

   // Header bytes
   CHECK(c.bytes[0] == 0x01);
   CHECK(c.bytes[1] == 0x70);
   CHECK(c.bytes[2] == 0x12);
   CHECK(c.bytes[3] == 0x20);

   // Digest round-trip
   auto d = c.digest();
   CHECK(std::memcmp(d.data(), digest.data(), 32) == 0);
}

TEST_CASE("CID string round-trip", "[cid]")
{
   std::array<uint8_t, 32> digest = {};
   for (uint8_t i = 0; i < 32; ++i)
      digest[i] = i * 7 + 3;

   auto c   = pfs::cid::from_digest(digest);
   auto str = c.to_string();

   // Must start with 'b' (base32lower multibase)
   REQUIRE(str[0] == 'b');

   // Round-trip
   auto c2 = pfs::cid::from_string(str);
   CHECK(c == c2);
}

TEST_CASE("CID from_bytes round-trip", "[cid]")
{
   std::array<uint8_t, 36> raw = {};
   raw[0] = 0x01;
   raw[1] = 0x70;
   raw[2] = 0x12;
   raw[3] = 0x20;
   for (uint8_t i = 0; i < 32; ++i)
      raw[4 + i] = 0xAA ^ i;

   auto c = pfs::cid::from_bytes(raw);
   CHECK(std::memcmp(c.bytes.data(), raw.data(), 36) == 0);
}

TEST_CASE("compute_cid is deterministic", "[cid]")
{
   std::vector<uint8_t> data(1000);
   for (size_t i = 0; i < data.size(); ++i)
      data[i] = static_cast<uint8_t>(i & 0xFF);

   auto c1 = pfs::compute_cid(data);
   auto c2 = pfs::compute_cid(data);
   CHECK(c1 == c2);
}

TEST_CASE("compute_cid differs for different data", "[cid]")
{
   std::vector<uint8_t> a = {1, 2, 3};
   std::vector<uint8_t> b = {1, 2, 4};

   auto ca = pfs::compute_cid(a);
   auto cb = pfs::compute_cid(b);
   CHECK(ca != cb);
}

// ════════════════════════════════════════════════════════════════════
// Key tests
// ════════════════════════════════════════════════════════════════════

TEST_CASE("block_key encoding", "[keys]")
{
   auto k = pfs::block_key(0x0102030405060708ULL);
   REQUIRE(k.size() == 9);
   CHECK(k[0] == 'B');
   CHECK(static_cast<uint8_t>(k[1]) == 0x01);
   CHECK(static_cast<uint8_t>(k[8]) == 0x08);
}

TEST_CASE("cas_key encoding", "[keys]")
{
   pfs::cid c;
   auto     k = pfs::cas_key(c);
   REQUIRE(k.size() == 37);
   CHECK(k[0] == 'C');
}

TEST_CASE("fs_key encoding", "[keys]")
{
   auto k = pfs::fs_key(psio::name_id(42), "photos/cat.jpg");
   REQUIRE(k.size() == 9 + 14);
   CHECK(k[0] == 'F');
}

TEST_CASE("fs_key sort order preserves path order", "[keys]")
{
   using namespace psio::literals;
   auto k1 = pfs::fs_key("alice"_n, "a.txt");
   auto k2 = pfs::fs_key("alice"_n, "b.txt");
   CHECK(k1 < k2);
}

TEST_CASE("shard_of is consistent", "[keys]")
{
   auto s1 = pfs::shard_of(12345, 256);
   auto s2 = pfs::shard_of(12345, 256);
   CHECK(s1 == s2);
   CHECK(s1 < 256);
}

// ════════════════════════════════════════════════════════════════════
// Schema fracpack round-trip tests
// ════════════════════════════════════════════════════════════════════

TEST_CASE("cas_entry fracpack round-trip", "[schema]")
{
   pfs::cas_entry e;
   e.refcount   = 3;
   e.total_size = 1024 * 1024;
   e.inline_data = {1, 2, 3, 4, 5};

   auto packed   = psio::to_frac(e);
   auto unpacked = psio::from_frac<pfs::cas_entry>(packed);

   CHECK(unpacked.refcount == 3);
   CHECK(unpacked.total_size == 1024 * 1024);
   CHECK(unpacked.inline_data == std::vector<uint8_t>{1, 2, 3, 4, 5});
   CHECK(unpacked.chunks.empty());
}

TEST_CASE("fs_entry fracpack round-trip", "[schema]")
{
   pfs::cid c = pfs::cid::from_digest(pcrypt::sha256(psio::bytes_view{}));

   pfs::fs_entry e;
   e.type        = pfs::entry_type::file;
   e.mode        = 0755;
   e.owner       = 42;
   e.mtime_ns    = 1000000000ULL;
   e.size        = 999;
   e.content_cid = c;

   auto packed   = psio::to_frac(e);
   auto unpacked = psio::from_frac<pfs::fs_entry>(packed);

   CHECK(unpacked.type == pfs::entry_type::file);
   CHECK(unpacked.mode == 0755);
   CHECK(unpacked.owner == 42);
   CHECK(unpacked.size == 999);
   REQUIRE(unpacked.content_cid.has_value());
   CHECK(*unpacked.content_cid == c);
}

TEST_CASE("fs_quota fracpack round-trip", "[schema]")
{
   pfs::fs_quota q;
   q.limit = 1024 * 1024;
   q.used  = 512;

   auto packed   = psio::to_frac(q);
   auto unpacked = psio::from_frac<pfs::fs_quota>(packed);

   CHECK(unpacked.limit == q.limit);
   CHECK(unpacked.used == q.used);
}

// ════════════════════════════════════════════════════════════════════
// UnixFS tests
// ════════════════════════════════════════════════════════════════════

TEST_CASE("UnixFS encode_file_block produces valid protobuf", "[unixfs]")
{
   std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
   auto                 block = pfs::unixfs::encode_file_block(data);

   // Should produce non-empty output
   CHECK(!block.empty());
   // Block should be larger than input (protobuf overhead)
   CHECK(block.size() > data.size());
}

TEST_CASE("UnixFS deterministic encoding", "[unixfs]")
{
   std::vector<uint8_t> data(1000);
   for (size_t i = 0; i < data.size(); ++i)
      data[i] = static_cast<uint8_t>(i & 0xFF);

   auto b1 = pfs::unixfs::encode_file_block(data);
   auto b2 = pfs::unixfs::encode_file_block(data);
   CHECK(b1 == b2);
}

// ════════════════════════════════════════════════════════════════════
// Integration: store write/read/ls
// ════════════════════════════════════════════════════════════════════

TEST_CASE("store write and read small file", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
   auto c = fs.write("alice"_n, "hello.txt", data);

   // Read back via file handle
   auto h = fs.open("alice"_n, "hello.txt");
   CHECK(h.size() == 10);
   CHECK(h.content_cid() == c);

   // Read content
   std::vector<uint8_t> read_data;
   h.read([&](psio::bytes_view chunk) {
      read_data.insert(read_data.end(), chunk.begin(), chunk.end());
   });
   CHECK(read_data == data);
}

TEST_CASE("store stat returns entry", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   auto data = bytes({42});
   fs.write("bob"_n, "file.dat", data);

   auto s = fs.stat("bob"_n, "file.dat");
   REQUIRE(s.has_value());
   CHECK(s->type == pfs::entry_type::file);
   CHECK(s->size == 1);
}

TEST_CASE("store stat returns nullopt for missing", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   auto s = fs.stat("nobody"_n, "nope.txt");
   CHECK(!s.has_value());
}

TEST_CASE("store mkdir and ls", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   fs.mkdir("alice"_n, "photos");
   fs.write("alice"_n, "photos/a.jpg", bytes({1, 2, 3}));
   fs.write("alice"_n, "photos/b.jpg", bytes({4, 5, 6}));

   std::vector<std::string> names;
   auto cursor = fs.ls("alice"_n, "photos");
   while (cursor.valid())
   {
      names.push_back(cursor.entry().name);
      cursor.next();
   }

   CHECK(names.size() == 2);
   // Keys are sorted, so a.jpg < b.jpg
   CHECK(names[0] == "a.jpg");
   CHECK(names[1] == "b.jpg");
}

TEST_CASE("store remove deletes file", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   fs.write("alice"_n, "temp.txt", bytes({1, 2, 3}));
   CHECK(fs.stat("alice"_n, "temp.txt").has_value());

   fs.remove("alice"_n, "temp.txt");
   CHECK(!fs.stat("alice"_n, "temp.txt").has_value());
}

TEST_CASE("store dedup: same content same CID", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   std::vector<uint8_t> data = {10, 20, 30};
   auto c1 = fs.write("alice"_n, "file1.txt", data);
   auto c2 = fs.write("alice"_n, "file2.txt", data);

   CHECK(c1 == c2);
}

TEST_CASE("store quota enforcement", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   fs.set_quota("alice"_n, 10);

   std::vector<uint8_t> data(5);
   fs.write("alice"_n, "small.txt", data);

   std::vector<uint8_t> big(20);
   CHECK_THROWS(fs.write("alice"_n, "big.txt", big));

   auto q = fs.quota("alice"_n);
   CHECK(q.limit == 10);
   CHECK(q.used == 5);
}

TEST_CASE("store share creates new ref", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   std::vector<uint8_t> data = {1, 2, 3, 4, 5};
   fs.write("alice"_n, "doc.txt", data);

   fs.share("alice"_n, "doc.txt", "bob"_n, "shared.txt");

   auto h = fs.open("bob"_n, "shared.txt");
   CHECK(h.size() == 5);

   // Content should match
   std::vector<uint8_t> read_data;
   h.read([&](psio::bytes_view chunk) {
      read_data.insert(read_data.end(), chunk.begin(), chunk.end());
   });
   CHECK(read_data == data);
}

TEST_CASE("store chmod updates mode", "[store]")
{
   auto db_path = test_db_path();
   auto db      = psitri::database::open(db_path);
   pfs::store fs(db);

   using namespace psio::literals;
   fs.write("alice"_n, "script.sh", bytes({1}));
   fs.chmod("alice"_n, "script.sh", 0755);

   auto s = fs.stat("alice"_n, "script.sh");
   REQUIRE(s.has_value());
   CHECK(s->mode == 0755);
}
