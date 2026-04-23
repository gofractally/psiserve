#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/fd_table.hpp>
#include <psiserve/instance_context.hpp>

using namespace psiserve;

// ═══════════════════════════════════════════════════════════════════
// FdTable::extract
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("FdTable extract returns entry and clears slot", "[fd_table]")
{
   FdTable fds;
   auto vfd = fds.alloc(SocketFd{RealFd{42}, nullptr, nullptr});
   REQUIRE(*vfd >= 0);

   auto entry = fds.extract(vfd);
   REQUIRE(entry.has_value());
   CHECK(std::holds_alternative<SocketFd>(*entry));
   CHECK(*std::get<SocketFd>(*entry).real_fd == 42);

   CHECK(fds.get(vfd) == nullptr);
}

TEST_CASE("FdTable extract on closed fd returns nullopt", "[fd_table]")
{
   FdTable fds;
   auto r = fds.extract(VirtualFd{5});
   CHECK(!r.has_value());
}

TEST_CASE("FdTable extract on invalid fd returns nullopt", "[fd_table]")
{
   FdTable fds;
   CHECK(!fds.extract(VirtualFd{-1}).has_value());
   CHECK(!fds.extract(VirtualFd{300}).has_value());
}

TEST_CASE("FdTable extract allows re-alloc of same slot", "[fd_table]")
{
   FdTable fds;
   auto vfd1 = fds.alloc(SocketFd{RealFd{10}, nullptr, nullptr});
   fds.extract(vfd1);

   auto vfd2 = fds.alloc(SocketFd{RealFd{20}, nullptr, nullptr});
   CHECK(*vfd2 == *vfd1);

   auto* entry = fds.get(vfd2);
   REQUIRE(entry != nullptr);
   CHECK(*std::get<SocketFd>(*entry).real_fd == 20);
}

// ═══════════════════════════════════════════════════════════════════
// InstanceContext::transfer_socket
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("transfer_socket moves fd between instances", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   auto src_vfd = src.process.fds.alloc(SocketFd{RealFd{77}, nullptr, nullptr});
   REQUIRE(*src_vfd >= 0);

   auto dst_vfd = src.transfer_socket(src_vfd, dst);
   CHECK(*dst_vfd >= 0);

   CHECK(src.process.fds.get(src_vfd) == nullptr);

   auto* entry = dst.process.fds.get(dst_vfd);
   REQUIRE(entry != nullptr);
   CHECK(*std::get<SocketFd>(*entry).real_fd == 77);
}

TEST_CASE("transfer_socket fails for invalid source vfd", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   auto dst_vfd = src.transfer_socket(VirtualFd{99}, dst);
   CHECK(*dst_vfd == *invalid_virtual_fd);
}

TEST_CASE("transfer_socket preserves TLS state", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(0xDEAD);
   auto src_vfd = src.process.fds.alloc(SocketFd{RealFd{33}, ctx, nullptr});

   auto dst_vfd = src.transfer_socket(src_vfd, dst);
   REQUIRE(*dst_vfd >= 0);

   auto* entry = dst.process.fds.get(dst_vfd);
   REQUIRE(entry != nullptr);
   auto& sock = std::get<SocketFd>(*entry);
   CHECK(*sock.real_fd == 33);
   CHECK(sock.ssl_ctx == ctx);
}

TEST_CASE("multiple transfers between instances", "[instance_context]")
{
   InstanceContext blockchain;
   InstanceContext conn1;
   InstanceContext conn2;

   auto vfd1 = blockchain.process.fds.alloc(SocketFd{RealFd{10}, nullptr, nullptr});
   auto vfd2 = blockchain.process.fds.alloc(SocketFd{RealFd{11}, nullptr, nullptr});

   auto c1_vfd = blockchain.transfer_socket(vfd1, conn1);
   auto c2_vfd = blockchain.transfer_socket(vfd2, conn2);

   CHECK(*c1_vfd >= 0);
   CHECK(*c2_vfd >= 0);

   CHECK(blockchain.process.fds.get(vfd1) == nullptr);
   CHECK(blockchain.process.fds.get(vfd2) == nullptr);

   CHECK(*std::get<SocketFd>(*conn1.process.fds.get(c1_vfd)).real_fd == 10);
   CHECK(*std::get<SocketFd>(*conn2.process.fds.get(c2_vfd)).real_fd == 11);
}

// ═══════════════════════════════════════════════════════════════════
// Resource-transfer edge cases
//
// The socket variant was the motivating case, but the same machinery
// must work uniformly for the other fd variants — UDP, files,
// preopened dirs.  These tests keep the uniform story honest.
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("transfer_socket moves UDP variants", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   auto vfd = src.process.fds.alloc(UdpFd{RealFd{55}});
   REQUIRE(*vfd >= 0);

   auto d = src.transfer_socket(vfd, dst);
   REQUIRE(*d >= 0);
   CHECK(src.process.fds.get(vfd) == nullptr);

   auto* entry = dst.process.fds.get(d);
   REQUIRE(entry != nullptr);
   CHECK(std::holds_alternative<UdpFd>(*entry));
   CHECK(*std::get<UdpFd>(*entry).real_fd == 55);
}

TEST_CASE("transfer_socket moves FileFd variants", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   auto vfd = src.process.fds.alloc(FileFd{RealFd{66}});
   auto d   = src.transfer_socket(vfd, dst);
   REQUIRE(*d >= 0);

   auto* entry = dst.process.fds.get(d);
   REQUIRE(entry != nullptr);
   CHECK(std::holds_alternative<FileFd>(*entry));
}

TEST_CASE("transfer_socket moves DirFd variants", "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   auto vfd = src.process.fds.alloc(DirFd{RealFd{77}});
   auto d   = src.transfer_socket(vfd, dst);
   REQUIRE(*d >= 0);

   auto* entry = dst.process.fds.get(d);
   REQUIRE(entry != nullptr);
   CHECK(std::holds_alternative<DirFd>(*entry));
}

TEST_CASE("transfer_socket fails into a saturated destination",
          "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst;

   // Fill dst completely so there is no free slot to land in.
   for (int i = 0; i < max_fds; ++i)
      REQUIRE(*dst.process.fds.alloc(SocketFd{RealFd{i}, nullptr, nullptr}) >= 0);

   auto src_vfd = src.process.fds.alloc(SocketFd{RealFd{100}, nullptr, nullptr});
   REQUIRE(*src_vfd >= 0);

   auto d = src.transfer_socket(src_vfd, dst);

   // Destination is full → no slot allocated; but note that the source
   // slot has already been extracted (by design — extract precedes the
   // alloc in transfer_socket).  This asymmetry is the motivator for
   // the TODO in the comment below.
   CHECK(*d == *invalid_virtual_fd);
   CHECK(src.process.fds.get(src_vfd) == nullptr);
}

TEST_CASE("double transfer of the same vfd fails on second attempt",
          "[instance_context]")
{
   InstanceContext src;
   InstanceContext dst1;
   InstanceContext dst2;

   auto vfd = src.process.fds.alloc(SocketFd{RealFd{88}, nullptr, nullptr});

   auto first  = src.transfer_socket(vfd, dst1);
   auto second = src.transfer_socket(vfd, dst2);

   CHECK(*first  >= 0);
   CHECK(*second == *invalid_virtual_fd);

   CHECK(dst2.process.fds.get(VirtualFd{0}) == nullptr);
}

TEST_CASE("round-trip transfer restores ownership to the original",
          "[instance_context]")
{
   InstanceContext a;
   InstanceContext b;

   auto vfd = a.process.fds.alloc(SocketFd{RealFd{11}, nullptr, nullptr});
   REQUIRE(*vfd >= 0);

   auto in_b  = a.transfer_socket(vfd, b);
   REQUIRE(*in_b >= 0);
   CHECK(a.process.fds.get(vfd) == nullptr);

   auto in_a2 = b.transfer_socket(in_b, a);
   REQUIRE(*in_a2 >= 0);
   CHECK(b.process.fds.get(in_b) == nullptr);

   auto* entry = a.process.fds.get(in_a2);
   REQUIRE(entry != nullptr);
   CHECK(*std::get<SocketFd>(*entry).real_fd == 11);
}

TEST_CASE("transfer_socket does not duplicate the resource into source",
          "[instance_context]")
{
   // Sanity: after a successful transfer the source must not still
   // observe the resource, even if its internal index matches the
   // destination's new vfd.
   InstanceContext src;
   InstanceContext dst;

   auto vfd = src.process.fds.alloc(SocketFd{RealFd{200}, nullptr, nullptr});
   auto d   = src.transfer_socket(vfd, dst);
   REQUIRE(*d >= 0);

   CHECK(src.process.fds.get(vfd) == nullptr);
   // The destination slot is its own index, which will generally be 0.
   CHECK(src.process.fds.get(d) == nullptr);
}
