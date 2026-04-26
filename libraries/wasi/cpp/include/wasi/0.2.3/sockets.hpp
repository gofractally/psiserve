#pragma once

// wasi:sockets@0.2.3 — network, TCP, UDP, and name-lookup interfaces.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/sockets/network.wit
//   libraries/wasi/wit/0.2.3/sockets/tcp.wit
//   libraries/wasi/wit/0.2.3/sockets/tcp-create-socket.wit
//   libraries/wasi/wit/0.2.3/sockets/udp.wit
//   libraries/wasi/wit/0.2.3/sockets/udp-create-socket.wit
//   libraries/wasi/wit/0.2.3/sockets/ip-name-lookup.wit
//   libraries/wasi/wit/0.2.3/sockets/instance-network.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime -- psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <wasi/0.2.3/io.hpp>

#include <psio1/reflect.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_resource.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// =====================================================================
// wasi:clocks/monotonic-clock — duration type alias
// =====================================================================
// duration is defined as u64 (nanoseconds) in wasi:clocks/monotonic-clock.
// When a full clocks binding is added, this should move there.

using wasi_duration = uint64_t;

// =====================================================================
// wasi:sockets/network — types and resources
// =====================================================================

// ── error-code enum ──────────────────────────────────────────────────

enum class error_code : uint8_t
{
   unknown,
   access_denied,
   not_supported,
   invalid_argument,
   out_of_memory,
   timeout,
   concurrency_conflict,
   not_in_progress,
   would_block,
   invalid_state,
   new_socket_limit,
   address_not_bindable,
   address_in_use,
   remote_unreachable,
   connection_refused,
   connection_reset,
   connection_aborted,
   datagram_too_large,
   name_unresolvable,
   temporary_resolver_failure,
   permanent_resolver_failure,
};
PSIO1_REFLECT(error_code)

// ── WIT result<T, error-code> as std::variant ───────────────────────
// index 0 = ok (success payload), index 1 = err (error_code)

template <typename T>
using socket_result = std::variant<T, error_code>;

using socket_result_void = std::variant<std::monostate, error_code>;

namespace sock_detail {
   template <typename T>
   socket_result<T> ok(T value) { return socket_result<T>{std::in_place_index<0>, std::move(value)}; }

   inline socket_result_void ok() { return socket_result_void{std::in_place_index<0>}; }

   struct sock_err_proxy
   {
      error_code ec;
      template <typename T>
      operator socket_result<T>() const { return socket_result<T>{std::in_place_index<1>, ec}; }
      operator socket_result_void() const { return socket_result_void{std::in_place_index<1>, ec}; }
   };

   inline sock_err_proxy err(error_code e) { return {e}; }
}

// ── ip-address-family enum ───────────────────────────────────────────

enum class ip_address_family : uint8_t
{
   ipv4,
   ipv6,
};
PSIO1_REFLECT(ip_address_family)

// ── IP address types ─────────────────────────────────────────────────

struct ipv4_address
{
   uint8_t octets[4];
};
PSIO1_REFLECT(ipv4_address, octets)

struct ipv6_address
{
   uint16_t segments[8];
};
PSIO1_REFLECT(ipv6_address, segments)

using ip_address = std::variant<ipv4_address, ipv6_address>;

namespace ip_address_tag {
   inline constexpr size_t ipv4 = 0;
   inline constexpr size_t ipv6 = 1;
}

// ── socket address types ─────────────────────────────────────────────

struct ipv4_socket_address
{
   uint16_t     port;
   ipv4_address address;
};
PSIO1_REFLECT(ipv4_socket_address, port, address)

struct ipv6_socket_address
{
   uint16_t     port;
   uint32_t     flow_info;
   ipv6_address address;
   uint32_t     scope_id;
};
PSIO1_REFLECT(ipv6_socket_address, port, flow_info, address, scope_id)

using ip_socket_address = std::variant<ipv4_socket_address, ipv6_socket_address>;

namespace ip_socket_address_tag {
   inline constexpr size_t ipv4 = 0;
   inline constexpr size_t ipv6 = 1;
}

// ── network resource ─────────────────────────────────────────────────

struct network : psio1::wit_resource {};
PSIO1_REFLECT(network)

// ── shutdown-type enum (tcp) ─────────────────────────────────────────

enum class shutdown_type : uint8_t
{
   receive,
   send,
   both,
};
PSIO1_REFLECT(shutdown_type)

// ── tcp-socket resource ──────────────────────────────────────────────

struct tcp_socket : psio1::wit_resource {};
PSIO1_REFLECT(tcp_socket)

// ── udp-socket resource ──────────────────────────────────────────────

struct udp_socket : psio1::wit_resource {};
PSIO1_REFLECT(udp_socket)

// ── udp datagram stream resources ────────────────────────────────────

struct incoming_datagram_stream : psio1::wit_resource {};
PSIO1_REFLECT(incoming_datagram_stream)

struct outgoing_datagram_stream : psio1::wit_resource {};
PSIO1_REFLECT(outgoing_datagram_stream)

// ── resolve-address-stream resource ──────────────────────────────────

struct resolve_address_stream : psio1::wit_resource {};
PSIO1_REFLECT(resolve_address_stream)

// ── UDP datagram records ─────────────────────────────────────────────

struct incoming_datagram
{
   std::vector<uint8_t> data;
   ip_socket_address    remote_address;
};
PSIO1_REFLECT(incoming_datagram, data, remote_address)

struct outgoing_datagram
{
   std::vector<uint8_t>             data;
   std::optional<ip_socket_address> remote_address;
};
PSIO1_REFLECT(outgoing_datagram, data, remote_address)

// =====================================================================
// Interface: wasi:sockets/network
// =====================================================================

struct wasi_sockets_network
{
   // network-error-code: func(err: borrow<error>) -> option<error-code>
   static inline std::optional<error_code> network_error_code(
       psio1::borrow<io_error> /*err*/)
   {
      return std::nullopt;
   }
};

// =====================================================================
// Interface: wasi:sockets/tcp
// =====================================================================

struct wasi_sockets_tcp
{
   // ── tcp-socket methods ────────────────────────────────────────────

   // [method] tcp-socket.start-bind: func(network: borrow<network>, local-address: ip-socket-address) -> result<_, error-code>
   static inline socket_result_void tcp_socket_start_bind(
       psio1::borrow<tcp_socket> /*self*/,
       psio1::borrow<network> /*network*/,
       ip_socket_address /*local_address*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.finish-bind: func() -> result<_, error-code>
   static inline socket_result_void tcp_socket_finish_bind(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.start-connect: func(network: borrow<network>, remote-address: ip-socket-address) -> result<_, error-code>
   static inline socket_result_void tcp_socket_start_connect(
       psio1::borrow<tcp_socket> /*self*/,
       psio1::borrow<network> /*network*/,
       ip_socket_address /*remote_address*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.finish-connect: func() -> result<tuple<input-stream, output-stream>, error-code>
   static inline socket_result<std::tuple<psio1::own<input_stream>, psio1::own<output_stream>>>
   tcp_socket_finish_connect(psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.start-listen: func() -> result<_, error-code>
   static inline socket_result_void tcp_socket_start_listen(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.finish-listen: func() -> result<_, error-code>
   static inline socket_result_void tcp_socket_finish_listen(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.accept: func() -> result<tuple<tcp-socket, input-stream, output-stream>, error-code>
   static inline socket_result<
       std::tuple<psio1::own<tcp_socket>, psio1::own<input_stream>, psio1::own<output_stream>>>
   tcp_socket_accept(psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.local-address: func() -> result<ip-socket-address, error-code>
   static inline socket_result<ip_socket_address> tcp_socket_local_address(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::invalid_state);
   }

   // [method] tcp-socket.remote-address: func() -> result<ip-socket-address, error-code>
   static inline socket_result<ip_socket_address> tcp_socket_remote_address(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::invalid_state);
   }

   // [method] tcp-socket.is-listening: func() -> bool
   static inline bool tcp_socket_is_listening(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return false;
   }

   // [method] tcp-socket.address-family: func() -> ip-address-family
   static inline ip_address_family tcp_socket_address_family(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return ip_address_family::ipv4;
   }

   // [method] tcp-socket.set-listen-backlog-size: func(value: u64) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_listen_backlog_size(
       psio1::borrow<tcp_socket> /*self*/,
       uint64_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.keep-alive-enabled: func() -> result<bool, error-code>
   static inline socket_result<bool> tcp_socket_keep_alive_enabled(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-keep-alive-enabled: func(value: bool) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_keep_alive_enabled(
       psio1::borrow<tcp_socket> /*self*/,
       bool /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.keep-alive-idle-time: func() -> result<duration, error-code>
   static inline socket_result<wasi_duration> tcp_socket_keep_alive_idle_time(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-keep-alive-idle-time: func(value: duration) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_keep_alive_idle_time(
       psio1::borrow<tcp_socket> /*self*/,
       wasi_duration /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.keep-alive-interval: func() -> result<duration, error-code>
   static inline socket_result<wasi_duration> tcp_socket_keep_alive_interval(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-keep-alive-interval: func(value: duration) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_keep_alive_interval(
       psio1::borrow<tcp_socket> /*self*/,
       wasi_duration /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.keep-alive-count: func() -> result<u32, error-code>
   static inline socket_result<uint32_t> tcp_socket_keep_alive_count(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-keep-alive-count: func(value: u32) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_keep_alive_count(
       psio1::borrow<tcp_socket> /*self*/,
       uint32_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.hop-limit: func() -> result<u8, error-code>
   static inline socket_result<uint8_t> tcp_socket_hop_limit(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-hop-limit: func(value: u8) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_hop_limit(
       psio1::borrow<tcp_socket> /*self*/,
       uint8_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.receive-buffer-size: func() -> result<u64, error-code>
   static inline socket_result<uint64_t> tcp_socket_receive_buffer_size(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-receive-buffer-size: func(value: u64) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_receive_buffer_size(
       psio1::borrow<tcp_socket> /*self*/,
       uint64_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.send-buffer-size: func() -> result<u64, error-code>
   static inline socket_result<uint64_t> tcp_socket_send_buffer_size(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.set-send-buffer-size: func(value: u64) -> result<_, error-code>
   static inline socket_result_void tcp_socket_set_send_buffer_size(
       psio1::borrow<tcp_socket> /*self*/,
       uint64_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] tcp-socket.subscribe: func() -> pollable
   static inline psio1::own<pollable> tcp_socket_subscribe(
       psio1::borrow<tcp_socket> /*self*/)
   {
      return psio1::own<pollable>{0};
   }

   // [method] tcp-socket.shutdown: func(shutdown-type: shutdown-type) -> result<_, error-code>
   static inline socket_result_void tcp_socket_shutdown(
       psio1::borrow<tcp_socket> /*self*/,
       shutdown_type /*shutdown_type*/)
   {
      return sock_detail::err(error_code::not_supported);
   }
};

// =====================================================================
// Interface: wasi:sockets/tcp-create-socket
// =====================================================================

struct wasi_sockets_tcp_create_socket
{
   // create-tcp-socket: func(address-family: ip-address-family) -> result<tcp-socket, error-code>
   static inline socket_result<psio1::own<tcp_socket>> create_tcp_socket(
       ip_address_family /*address_family*/)
   {
      return sock_detail::err(error_code::not_supported);
   }
};

// =====================================================================
// Interface: wasi:sockets/udp
// =====================================================================

struct wasi_sockets_udp
{
   // ── udp-socket methods ────────────────────────────────────────────

   // [method] udp-socket.start-bind: func(network: borrow<network>, local-address: ip-socket-address) -> result<_, error-code>
   static inline socket_result_void udp_socket_start_bind(
       psio1::borrow<udp_socket> /*self*/,
       psio1::borrow<network> /*network*/,
       ip_socket_address /*local_address*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.finish-bind: func() -> result<_, error-code>
   static inline socket_result_void udp_socket_finish_bind(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.stream: func(remote-address: option<ip-socket-address>) -> result<tuple<incoming-datagram-stream, outgoing-datagram-stream>, error-code>
   static inline socket_result<
       std::tuple<psio1::own<incoming_datagram_stream>, psio1::own<outgoing_datagram_stream>>>
   udp_socket_stream(
       psio1::borrow<udp_socket> /*self*/,
       std::optional<ip_socket_address> /*remote_address*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.local-address: func() -> result<ip-socket-address, error-code>
   static inline socket_result<ip_socket_address> udp_socket_local_address(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::invalid_state);
   }

   // [method] udp-socket.remote-address: func() -> result<ip-socket-address, error-code>
   static inline socket_result<ip_socket_address> udp_socket_remote_address(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::invalid_state);
   }

   // [method] udp-socket.address-family: func() -> ip-address-family
   static inline ip_address_family udp_socket_address_family(
       psio1::borrow<udp_socket> /*self*/)
   {
      return ip_address_family::ipv4;
   }

   // [method] udp-socket.unicast-hop-limit: func() -> result<u8, error-code>
   static inline socket_result<uint8_t> udp_socket_unicast_hop_limit(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.set-unicast-hop-limit: func(value: u8) -> result<_, error-code>
   static inline socket_result_void udp_socket_set_unicast_hop_limit(
       psio1::borrow<udp_socket> /*self*/,
       uint8_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.receive-buffer-size: func() -> result<u64, error-code>
   static inline socket_result<uint64_t> udp_socket_receive_buffer_size(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.set-receive-buffer-size: func(value: u64) -> result<_, error-code>
   static inline socket_result_void udp_socket_set_receive_buffer_size(
       psio1::borrow<udp_socket> /*self*/,
       uint64_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.send-buffer-size: func() -> result<u64, error-code>
   static inline socket_result<uint64_t> udp_socket_send_buffer_size(
       psio1::borrow<udp_socket> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.set-send-buffer-size: func(value: u64) -> result<_, error-code>
   static inline socket_result_void udp_socket_set_send_buffer_size(
       psio1::borrow<udp_socket> /*self*/,
       uint64_t /*value*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] udp-socket.subscribe: func() -> pollable
   static inline psio1::own<pollable> udp_socket_subscribe(
       psio1::borrow<udp_socket> /*self*/)
   {
      return psio1::own<pollable>{0};
   }

   // ── incoming-datagram-stream methods ──────────────────────────────

   // [method] incoming-datagram-stream.receive: func(max-results: u64) -> result<list<incoming-datagram>, error-code>
   static inline socket_result<std::vector<incoming_datagram>>
   incoming_datagram_stream_receive(
       psio1::borrow<incoming_datagram_stream> /*self*/,
       uint64_t /*max_results*/)
   {
      return sock_detail::ok(std::vector<incoming_datagram>{});
   }

   // [method] incoming-datagram-stream.subscribe: func() -> pollable
   static inline psio1::own<pollable> incoming_datagram_stream_subscribe(
       psio1::borrow<incoming_datagram_stream> /*self*/)
   {
      return psio1::own<pollable>{0};
   }

   // ── outgoing-datagram-stream methods ──────────────────────────────

   // [method] outgoing-datagram-stream.check-send: func() -> result<u64, error-code>
   static inline socket_result<uint64_t> outgoing_datagram_stream_check_send(
       psio1::borrow<outgoing_datagram_stream> /*self*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] outgoing-datagram-stream.send: func(datagrams: list<outgoing-datagram>) -> result<u64, error-code>
   static inline socket_result<uint64_t> outgoing_datagram_stream_send(
       psio1::borrow<outgoing_datagram_stream> /*self*/,
       std::vector<outgoing_datagram> /*datagrams*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] outgoing-datagram-stream.subscribe: func() -> pollable
   static inline psio1::own<pollable> outgoing_datagram_stream_subscribe(
       psio1::borrow<outgoing_datagram_stream> /*self*/)
   {
      return psio1::own<pollable>{0};
   }
};

// =====================================================================
// Interface: wasi:sockets/udp-create-socket
// =====================================================================

struct wasi_sockets_udp_create_socket
{
   // create-udp-socket: func(address-family: ip-address-family) -> result<udp-socket, error-code>
   static inline socket_result<psio1::own<udp_socket>> create_udp_socket(
       ip_address_family /*address_family*/)
   {
      return sock_detail::err(error_code::not_supported);
   }
};

// =====================================================================
// Interface: wasi:sockets/ip-name-lookup
// =====================================================================

struct wasi_sockets_ip_name_lookup
{
   // resolve-addresses: func(network: borrow<network>, name: string) -> result<resolve-address-stream, error-code>
   static inline socket_result<psio1::own<resolve_address_stream>> resolve_addresses(
       psio1::borrow<network> /*network*/,
       std::string /*name*/)
   {
      return sock_detail::err(error_code::not_supported);
   }

   // [method] resolve-address-stream.resolve-next-address: func() -> result<option<ip-address>, error-code>
   static inline socket_result<std::optional<ip_address>>
   resolve_address_stream_resolve_next_address(
       psio1::borrow<resolve_address_stream> /*self*/)
   {
      return sock_detail::ok<std::optional<ip_address>>(std::nullopt);
   }

   // [method] resolve-address-stream.subscribe: func() -> pollable
   static inline psio1::own<pollable> resolve_address_stream_subscribe(
       psio1::borrow<resolve_address_stream> /*self*/)
   {
      return psio1::own<pollable>{0};
   }
};

// =====================================================================
// Interface: wasi:sockets/instance-network
// =====================================================================

struct wasi_sockets_instance_network
{
   // instance-network: func() -> network
   static inline psio1::own<network> instance_network()
   {
      return psio1::own<network>{0};
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO1_PACKAGE(wasi_sockets, "0.2.3");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(wasi_sockets)

PSIO1_INTERFACE(wasi_sockets_network,
               types(error_code,
                     ip_address_family,
                     ipv4_address,
                     ipv6_address,
                     ipv4_socket_address,
                     ipv6_socket_address,
                     network),
               funcs(func(network_error_code, err)))

PSIO1_INTERFACE(wasi_sockets_tcp,
               types(shutdown_type, tcp_socket),
               funcs(func(tcp_socket_start_bind, self, network, local_address),
                     func(tcp_socket_finish_bind, self),
                     func(tcp_socket_start_connect, self, network, remote_address),
                     func(tcp_socket_finish_connect, self),
                     func(tcp_socket_start_listen, self),
                     func(tcp_socket_finish_listen, self),
                     func(tcp_socket_accept, self),
                     func(tcp_socket_local_address, self),
                     func(tcp_socket_remote_address, self),
                     func(tcp_socket_is_listening, self),
                     func(tcp_socket_address_family, self),
                     func(tcp_socket_set_listen_backlog_size, self, value),
                     func(tcp_socket_keep_alive_enabled, self),
                     func(tcp_socket_set_keep_alive_enabled, self, value),
                     func(tcp_socket_keep_alive_idle_time, self),
                     func(tcp_socket_set_keep_alive_idle_time, self, value),
                     func(tcp_socket_keep_alive_interval, self),
                     func(tcp_socket_set_keep_alive_interval, self, value),
                     func(tcp_socket_keep_alive_count, self),
                     func(tcp_socket_set_keep_alive_count, self, value),
                     func(tcp_socket_hop_limit, self),
                     func(tcp_socket_set_hop_limit, self, value),
                     func(tcp_socket_receive_buffer_size, self),
                     func(tcp_socket_set_receive_buffer_size, self, value),
                     func(tcp_socket_send_buffer_size, self),
                     func(tcp_socket_set_send_buffer_size, self, value),
                     func(tcp_socket_subscribe, self),
                     func(tcp_socket_shutdown, self, shutdown_type)))

PSIO1_INTERFACE(wasi_sockets_tcp_create_socket,
               types(),
               funcs(func(create_tcp_socket, address_family)))

PSIO1_INTERFACE(wasi_sockets_udp,
               types(incoming_datagram,
                     outgoing_datagram,
                     udp_socket,
                     incoming_datagram_stream,
                     outgoing_datagram_stream),
               funcs(func(udp_socket_start_bind, self, network, local_address),
                     func(udp_socket_finish_bind, self),
                     func(udp_socket_stream, self, remote_address),
                     func(udp_socket_local_address, self),
                     func(udp_socket_remote_address, self),
                     func(udp_socket_address_family, self),
                     func(udp_socket_unicast_hop_limit, self),
                     func(udp_socket_set_unicast_hop_limit, self, value),
                     func(udp_socket_receive_buffer_size, self),
                     func(udp_socket_set_receive_buffer_size, self, value),
                     func(udp_socket_send_buffer_size, self),
                     func(udp_socket_set_send_buffer_size, self, value),
                     func(udp_socket_subscribe, self),
                     func(incoming_datagram_stream_receive, self, max_results),
                     func(incoming_datagram_stream_subscribe, self),
                     func(outgoing_datagram_stream_check_send, self),
                     func(outgoing_datagram_stream_send, self, datagrams),
                     func(outgoing_datagram_stream_subscribe, self)))

PSIO1_INTERFACE(wasi_sockets_udp_create_socket,
               types(),
               funcs(func(create_udp_socket, address_family)))

PSIO1_INTERFACE(wasi_sockets_ip_name_lookup,
               types(resolve_address_stream),
               funcs(func(resolve_addresses, network, name),
                     func(resolve_address_stream_resolve_next_address, self),
                     func(resolve_address_stream_subscribe, self)))

PSIO1_INTERFACE(wasi_sockets_instance_network,
               types(),
               funcs(func(instance_network)))

// end of wasi_sockets interface registrations
