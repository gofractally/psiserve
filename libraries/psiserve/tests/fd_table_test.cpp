#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/fd_table.hpp>

#include <array>
#include <variant>

using namespace psiserve;

// Focused lifecycle coverage for psiserve::FdTable.  The table is the
// pivot point for every cross-instance resource transfer and its
// invariants — lowest-free-slot allocation, close/extract asymmetry,
// bounds checks — underpin sandboxing, so they deserve dedicated
// unit coverage independent of InstanceContext.

// ── alloc ────────────────────────────────────────────────────────────

TEST_CASE("alloc assigns consecutive low fds starting from 0", "[fd_table]")
{
   FdTable fds;

   auto a = fds.alloc(SocketFd{RealFd{10}, nullptr, nullptr});
   auto b = fds.alloc(SocketFd{RealFd{11}, nullptr, nullptr});
   auto c = fds.alloc(SocketFd{RealFd{12}, nullptr, nullptr});

   CHECK(*a == 0);
   CHECK(*b == 1);
   CHECK(*c == 2);
}

TEST_CASE("alloc reuses the lowest freed slot", "[fd_table]")
{
   FdTable fds;

   auto a = fds.alloc(SocketFd{RealFd{10}, nullptr, nullptr});
   auto b = fds.alloc(SocketFd{RealFd{11}, nullptr, nullptr});
   auto c = fds.alloc(SocketFd{RealFd{12}, nullptr, nullptr});

   REQUIRE(fds.close(b));

   auto d = fds.alloc(SocketFd{RealFd{99}, nullptr, nullptr});
   CHECK(*d == *b);
   (void)a; (void)c;
}

TEST_CASE("alloc returns invalid_virtual_fd when the table is saturated",
          "[fd_table]")
{
   FdTable fds;

   for (int i = 0; i < max_fds; ++i)
      REQUIRE(*fds.alloc(SocketFd{RealFd{i}, nullptr, nullptr}) == i);

   auto overflow = fds.alloc(SocketFd{RealFd{999}, nullptr, nullptr});
   CHECK(*overflow == *invalid_virtual_fd);
}

// ── get ──────────────────────────────────────────────────────────────

TEST_CASE("get returns nullptr for out-of-range or closed slots",
          "[fd_table]")
{
   FdTable fds;

   CHECK(fds.get(VirtualFd{-1})         == nullptr);
   CHECK(fds.get(VirtualFd{max_fds})    == nullptr);
   CHECK(fds.get(VirtualFd{max_fds+10}) == nullptr);

   auto v = fds.alloc(SocketFd{RealFd{7}, nullptr, nullptr});
   REQUIRE(fds.close(v));
   CHECK(fds.get(v) == nullptr);
}

TEST_CASE("get yields the variant stored by alloc", "[fd_table]")
{
   FdTable fds;

   auto u = fds.alloc(UdpFd{RealFd{55}});
   auto f = fds.alloc(FileFd{RealFd{56}});
   auto d = fds.alloc(DirFd{RealFd{57}});

   REQUIRE(fds.get(u) != nullptr);
   REQUIRE(fds.get(f) != nullptr);
   REQUIRE(fds.get(d) != nullptr);

   CHECK(std::holds_alternative<UdpFd>(*fds.get(u)));
   CHECK(std::holds_alternative<FileFd>(*fds.get(f)));
   CHECK(std::holds_alternative<DirFd>(*fds.get(d)));
}

// ── close ────────────────────────────────────────────────────────────

TEST_CASE("close returns false for unknown fds", "[fd_table]")
{
   FdTable fds;
   CHECK_FALSE(fds.close(VirtualFd{-1}));
   CHECK_FALSE(fds.close(VirtualFd{7}));
   CHECK_FALSE(fds.close(VirtualFd{max_fds}));
}

TEST_CASE("close is idempotent after first success", "[fd_table]")
{
   FdTable fds;
   auto v = fds.alloc(SocketFd{RealFd{8}, nullptr, nullptr});
   CHECK(fds.close(v));
   CHECK_FALSE(fds.close(v));
   CHECK_FALSE(fds.close(v));
}

// ── extract ──────────────────────────────────────────────────────────

TEST_CASE("extract on closed fd after close returns nullopt",
          "[fd_table]")
{
   FdTable fds;
   auto v = fds.alloc(SocketFd{RealFd{20}, nullptr, nullptr});
   REQUIRE(fds.close(v));
   CHECK_FALSE(fds.extract(v).has_value());
}

TEST_CASE("close after extract is a no-op", "[fd_table]")
{
   FdTable fds;
   auto v = fds.alloc(SocketFd{RealFd{21}, nullptr, nullptr});
   auto e = fds.extract(v);
   REQUIRE(e.has_value());
   CHECK_FALSE(fds.close(v));
}

TEST_CASE("double extract returns nullopt the second time",
          "[fd_table]")
{
   FdTable fds;
   auto v = fds.alloc(SocketFd{RealFd{22}, nullptr, nullptr});

   auto first = fds.extract(v);
   auto again = fds.extract(v);

   REQUIRE(first.has_value());
   CHECK_FALSE(again.has_value());
}

TEST_CASE("extract preserves per-variant state", "[fd_table]")
{
   FdTable fds;

   SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(0xBADF00D);
   auto sock = fds.alloc(SocketFd{RealFd{30}, ctx, nullptr});
   auto udp  = fds.alloc(UdpFd   {RealFd{31}});
   auto dir  = fds.alloc(DirFd   {RealFd{32}});

   auto s = fds.extract(sock);
   auto u = fds.extract(udp);
   auto d = fds.extract(dir);

   REQUIRE(s.has_value());
   REQUIRE(u.has_value());
   REQUIRE(d.has_value());

   CHECK(*std::get<SocketFd>(*s).real_fd == 30);
   CHECK( std::get<SocketFd>(*s).ssl_ctx == ctx);
   CHECK(*std::get<UdpFd>   (*u).real_fd == 31);
   CHECK(*std::get<DirFd>   (*d).real_fd == 32);
}

TEST_CASE("extract allows the slot to be immediately reallocated",
          "[fd_table]")
{
   FdTable fds;
   auto a = fds.alloc(SocketFd{RealFd{40}, nullptr, nullptr});

   auto e = fds.extract(a);
   REQUIRE(e.has_value());

   auto b = fds.alloc(UdpFd{RealFd{41}});
   CHECK(*b == *a);
   CHECK(std::holds_alternative<UdpFd>(*fds.get(b)));
}
