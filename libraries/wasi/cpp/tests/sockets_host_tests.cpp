#include <wasi/0.2.3/sockets_host.hpp>

#include <catch2/catch.hpp>

#include <cstring>
#include <thread>

using namespace wasi_host;
using psiber::Scheduler;

TEST_CASE("WasiSocketsHost create TCP socket", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(result.index() == 0);
   auto& sock = std::get<0>(result);
   REQUIRE(sock.handle != psizam::handle_table<tcp_socket_data, 256>::invalid_handle);
}

TEST_CASE("WasiSocketsHost create UDP socket", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto result = sockets.create_udp_socket(ip_address_family::ipv4);
   REQUIRE(result.index() == 0);
   auto& sock = std::get<0>(result);
   REQUIRE(sock.handle != psizam::handle_table<udp_socket_data, 64>::invalid_handle);
}

TEST_CASE("WasiSocketsHost instance_network", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto net = sockets.instance_network();
   REQUIRE(net.handle != psizam::handle_table<network_data, 4>::invalid_handle);
}

TEST_CASE("WasiSocketsHost TCP bind and local address", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto net = sockets.instance_network();

   auto create_result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(create_result.index() == 0);
   auto tcp = std::get<0>(create_result);

   ip_socket_address addr = ipv4_socket_address{0, ipv4_address{{127, 0, 0, 1}}};

   auto bind_start = sockets.tcp_socket_start_bind(
       psio::borrow<tcp_socket>{tcp.handle},
       psio::borrow<network>{net.handle},
       addr);
   REQUIRE(bind_start.index() == 0);

   auto bind_finish = sockets.tcp_socket_finish_bind(
       psio::borrow<tcp_socket>{tcp.handle});
   REQUIRE(bind_finish.index() == 0);

   auto local = sockets.tcp_socket_local_address(
       psio::borrow<tcp_socket>{tcp.handle});
   REQUIRE(local.index() == 0);
   auto& local_addr = std::get<0>(local);
   REQUIRE(std::holds_alternative<ipv4_socket_address>(local_addr));
   REQUIRE(std::get<ipv4_socket_address>(local_addr).port != 0);
}

TEST_CASE("WasiSocketsHost TCP listen lifecycle", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto net = sockets.instance_network();
   auto tcp_result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(tcp_result.index() == 0);
   auto tcp = std::get<0>(tcp_result);

   ip_socket_address addr = ipv4_socket_address{0, ipv4_address{{127, 0, 0, 1}}};

   REQUIRE(sockets.tcp_socket_start_bind(
       psio::borrow<tcp_socket>{tcp.handle},
       psio::borrow<network>{net.handle}, addr).index() == 0);
   REQUIRE(sockets.tcp_socket_finish_bind(
       psio::borrow<tcp_socket>{tcp.handle}).index() == 0);

   REQUIRE_FALSE(sockets.tcp_socket_is_listening(
       psio::borrow<tcp_socket>{tcp.handle}));

   REQUIRE(sockets.tcp_socket_start_listen(
       psio::borrow<tcp_socket>{tcp.handle}).index() == 0);
   REQUIRE(sockets.tcp_socket_finish_listen(
       psio::borrow<tcp_socket>{tcp.handle}).index() == 0);

   REQUIRE(sockets.tcp_socket_is_listening(
       psio::borrow<tcp_socket>{tcp.handle}));
}

TEST_CASE("WasiSocketsHost TCP socket options", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto tcp_result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(tcp_result.index() == 0);
   auto tcp = std::get<0>(tcp_result);
   auto b   = psio::borrow<tcp_socket>{tcp.handle};

   SECTION("keepalive")
   {
      REQUIRE(sockets.tcp_socket_set_keep_alive_enabled(b, true).index() == 0);
      auto ka = sockets.tcp_socket_keep_alive_enabled(b);
      REQUIRE(ka.index() == 0);
      REQUIRE(std::get<0>(ka) == true);
   }

   SECTION("receive buffer size")
   {
      REQUIRE(sockets.tcp_socket_set_receive_buffer_size(b, 32768).index() == 0);
      auto sz = sockets.tcp_socket_receive_buffer_size(b);
      REQUIRE(sz.index() == 0);
      REQUIRE(std::get<0>(sz) > 0);
   }

   SECTION("send buffer size")
   {
      REQUIRE(sockets.tcp_socket_set_send_buffer_size(b, 32768).index() == 0);
      auto sz = sockets.tcp_socket_send_buffer_size(b);
      REQUIRE(sz.index() == 0);
      REQUIRE(std::get<0>(sz) > 0);
   }

   SECTION("address family")
   {
      REQUIRE(sockets.tcp_socket_address_family(b) == ip_address_family::ipv4);
   }

   SECTION("hop limit")
   {
      auto hl = sockets.tcp_socket_hop_limit(b);
      REQUIRE(hl.index() == 0);
      REQUIRE(std::get<0>(hl) > 0);
   }
}

TEST_CASE("WasiSocketsHost TCP state machine rejects invalid transitions", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto tcp_result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(tcp_result.index() == 0);
   auto tcp = std::get<0>(tcp_result);
   auto b   = psio::borrow<tcp_socket>{tcp.handle};

   REQUIRE(sockets.tcp_socket_finish_bind(b).index() == 1);
   REQUIRE(sockets.tcp_socket_start_listen(b).index() == 1);
   REQUIRE(sockets.tcp_socket_finish_listen(b).index() == 1);
   REQUIRE(sockets.tcp_socket_finish_connect(b).index() == 1);
}

TEST_CASE("WasiSocketsHost UDP bind and local address", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto net = sockets.instance_network();
   auto udp_result = sockets.create_udp_socket(ip_address_family::ipv4);
   REQUIRE(udp_result.index() == 0);
   auto udp = std::get<0>(udp_result);

   ip_socket_address addr = ipv4_socket_address{0, ipv4_address{{127, 0, 0, 1}}};

   REQUIRE(sockets.udp_socket_start_bind(
       psio::borrow<udp_socket>{udp.handle},
       psio::borrow<network>{net.handle}, addr).index() == 0);
   REQUIRE(sockets.udp_socket_finish_bind(
       psio::borrow<udp_socket>{udp.handle}).index() == 0);

   auto local = sockets.udp_socket_local_address(
       psio::borrow<udp_socket>{udp.handle});
   REQUIRE(local.index() == 0);
   REQUIRE(std::get<ipv4_socket_address>(std::get<0>(local)).port != 0);
}

TEST_CASE("WasiSocketsHost TCP subscribe returns pollable", "[wasi][sockets]")
{
   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   auto tcp_result = sockets.create_tcp_socket(ip_address_family::ipv4);
   REQUIRE(tcp_result.index() == 0);
   auto tcp = std::get<0>(tcp_result);

   auto p = sockets.tcp_socket_subscribe(psio::borrow<tcp_socket>{tcp.handle});
   REQUIRE(p.handle != psizam::handle_table<pollable_data, 256>::invalid_handle);
}
