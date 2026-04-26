#pragma once

#include <wasi/0.2.3/sockets.hpp>
#include <wasi/0.2.3/io_host.hpp>

#include <psio1/structural.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wasi_host {

struct network_data {};

enum class tcp_state : uint8_t
{
   created,
   bind_started,
   bound,
   connect_started,
   connected,
   listen_started,
   listening,
   closed,
};

struct tcp_socket_data
{
   RealFd            fd;
   ip_address_family family;
   tcp_state         state   = tcp_state::created;
   uint32_t          backlog = 128;
};

struct udp_socket_data
{
   RealFd            fd;
   ip_address_family family;
   bool              bound = false;
};

// ── address conversion helpers ───────────────────────────────────────

inline socklen_t to_sockaddr(const ip_socket_address& addr,
                             struct sockaddr_storage&  ss)
{
   std::memset(&ss, 0, sizeof(ss));
   if (auto* v4 = std::get_if<ipv4_socket_address>(&addr))
   {
      auto& sa       = reinterpret_cast<struct sockaddr_in&>(ss);
      sa.sin_family  = AF_INET;
      sa.sin_port    = htons(v4->port);
      std::memcpy(&sa.sin_addr, v4->address.octets, 4);
      return sizeof(struct sockaddr_in);
   }
   auto& v6          = std::get<ipv6_socket_address>(addr);
   auto& sa          = reinterpret_cast<struct sockaddr_in6&>(ss);
   sa.sin6_family    = AF_INET6;
   sa.sin6_port      = htons(v6.port);
   sa.sin6_flowinfo  = htonl(v6.flow_info);
   for (int i = 0; i < 8; ++i)
   {
      sa.sin6_addr.s6_addr[i * 2]     = static_cast<uint8_t>(v6.address.segments[i] >> 8);
      sa.sin6_addr.s6_addr[i * 2 + 1] = static_cast<uint8_t>(v6.address.segments[i]);
   }
   sa.sin6_scope_id = v6.scope_id;
   return sizeof(struct sockaddr_in6);
}

inline ip_socket_address from_sockaddr(const struct sockaddr_storage& ss)
{
   if (ss.ss_family == AF_INET)
   {
      auto& sa = reinterpret_cast<const struct sockaddr_in&>(ss);
      ipv4_socket_address result;
      result.port = ntohs(sa.sin_port);
      std::memcpy(result.address.octets, &sa.sin_addr, 4);
      return result;
   }
   auto& sa = reinterpret_cast<const struct sockaddr_in6&>(ss);
   ipv6_socket_address result;
   result.port      = ntohs(sa.sin6_port);
   result.flow_info = ntohl(sa.sin6_flowinfo);
   for (int i = 0; i < 8; ++i)
   {
      result.address.segments[i] =
          (uint16_t(sa.sin6_addr.s6_addr[i * 2]) << 8) |
          uint16_t(sa.sin6_addr.s6_addr[i * 2 + 1]);
   }
   result.scope_id = sa.sin6_scope_id;
   return result;
}

inline ::error_code errno_to_network_error()
{
   switch (errno)
   {
      case EACCES:
      case EPERM:        return ::error_code::access_denied;
      case EADDRINUSE:   return ::error_code::address_in_use;
      case EADDRNOTAVAIL:return ::error_code::address_not_bindable;
      case ECONNREFUSED: return ::error_code::connection_refused;
      case ECONNRESET:   return ::error_code::connection_reset;
      case ECONNABORTED: return ::error_code::connection_aborted;
      case EHOSTUNREACH:
      case ENETUNREACH:  return ::error_code::remote_unreachable;
      case ETIMEDOUT:    return ::error_code::timeout;
      case EINVAL:       return ::error_code::invalid_argument;
      case ENOMEM:       return ::error_code::out_of_memory;
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
                         return ::error_code::would_block;
      case EINPROGRESS:  return ::error_code::would_block;
      default:           return ::error_code::unknown;
   }
}

inline int make_nonblocking_socket(int af, int type)
{
   int fd = ::socket(af, type, 0);
   if (fd < 0)
      return -1;
   ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
   ::fcntl(fd, F_SETFD, FD_CLOEXEC);
   return fd;
}

// ── WasiSocketsHost ──────────────────────────────────────────────────

struct WasiSocketsHost
{
   using neterr = ::error_code;

   WasiIoHost& io;

   psizam::handle_table<network_data, 4>     networks{4};
   psizam::handle_table<tcp_socket_data, 256> tcp_sockets{256};
   psizam::handle_table<udp_socket_data, 64>  udp_sockets{64};

   explicit WasiSocketsHost(WasiIoHost& io_host) : io(io_host) {}

   // ── wasi:sockets/instance-network ─────────────────────────────────

   psio1::own<network> instance_network()
   {
      return psio1::own<network>{networks.create(network_data{})};
   }

   // ── wasi:sockets/network ─────────────────────────────────────────

   std::optional<::error_code> network_error_code(
       psio1::borrow<io_error> /*err*/)
   {
      return std::nullopt;
   }

   // ── wasi:sockets/tcp-create-socket ────────────────────────────────

   socket_result<psio1::own<tcp_socket>> create_tcp_socket(
       ip_address_family family)
   {
      int af = (family == ip_address_family::ipv4) ? AF_INET : AF_INET6;
      int fd = make_nonblocking_socket(af, SOCK_STREAM);
      if (fd < 0)
         return sock_detail::err(errno_to_network_error());

      int optval = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

      auto handle = tcp_sockets.create(tcp_socket_data{RealFd{fd}, family});
      return sock_detail::ok(psio1::own<tcp_socket>{handle});
   }

   // ── wasi:sockets/tcp — lifecycle ──────────────────────────────────

   socket_result_void tcp_socket_start_bind(
       psio1::borrow<tcp_socket>  self,
       psio1::borrow<network>     /*net*/,
       ip_socket_address         local_address)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s || s->state != tcp_state::created)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage ss;
      socklen_t len = to_sockaddr(local_address, ss);
      if (::bind(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), len) < 0)
         return sock_detail::err(errno_to_network_error());

      s->state = tcp_state::bind_started;
      return sock_detail::ok();
   }

   socket_result_void tcp_socket_finish_bind(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      if (s->state != tcp_state::bind_started)
         return sock_detail::err(neterr::not_in_progress);
      s->state = tcp_state::bound;
      return sock_detail::ok();
   }

   socket_result_void tcp_socket_start_connect(
       psio1::borrow<tcp_socket>  self,
       psio1::borrow<network>     /*net*/,
       ip_socket_address         remote_address)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      if (s->state != tcp_state::created && s->state != tcp_state::bound)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage ss;
      socklen_t len = to_sockaddr(remote_address, ss);
      int r = ::connect(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), len);
      if (r == 0)
      {
         s->state = tcp_state::connected;
         return sock_detail::ok();
      }
      if (errno == EINPROGRESS)
      {
         s->state = tcp_state::connect_started;
         return sock_detail::ok();
      }
      return sock_detail::err(errno_to_network_error());
   }

   socket_result<std::tuple<psio1::own<input_stream>, psio1::own<output_stream>>>
   tcp_socket_finish_connect(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);

      if (s->state == tcp_state::connect_started)
      {
         int       err = 0;
         socklen_t len = sizeof(err);
         ::getsockopt(*s->fd, SOL_SOCKET, SO_ERROR, &err, &len);
         if (err != 0)
         {
            errno = err;
            return sock_detail::err(errno_to_network_error());
         }
         s->state = tcp_state::connected;
      }
      else if (s->state != tcp_state::connected)
      {
         return sock_detail::err(neterr::not_in_progress);
      }

      int fd2 = ::dup(*s->fd);
      if (fd2 < 0)
         return sock_detail::err(errno_to_network_error());
      ::fcntl(fd2, F_SETFL, ::fcntl(fd2, F_GETFL, 0) | O_NONBLOCK);

      auto is = io.create_input_stream(s->fd);
      auto os = io.create_output_stream(RealFd{fd2});
      return sock_detail::ok(std::make_tuple(std::move(is), std::move(os)));
   }

   socket_result_void tcp_socket_start_listen(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s || s->state != tcp_state::bound)
         return sock_detail::err(neterr::invalid_state);

      if (::listen(*s->fd, static_cast<int>(s->backlog)) < 0)
         return sock_detail::err(errno_to_network_error());

      s->state = tcp_state::listen_started;
      return sock_detail::ok();
   }

   socket_result_void tcp_socket_finish_listen(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      if (s->state != tcp_state::listen_started)
         return sock_detail::err(neterr::not_in_progress);
      s->state = tcp_state::listening;
      return sock_detail::ok();
   }

   socket_result<std::tuple<psio1::own<tcp_socket>,
                             psio1::own<input_stream>,
                             psio1::own<output_stream>>>
   tcp_socket_accept(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s || s->state != tcp_state::listening)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage peer;
      socklen_t               peer_len = sizeof(peer);
      int                     client_fd;

      for (;;)
      {
         client_fd = ::accept(*s->fd,
                              reinterpret_cast<struct sockaddr*>(&peer),
                              &peer_len);
         if (client_fd >= 0)
            break;
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            Scheduler::current().yield(s->fd, Readable);
            continue;
         }
         return sock_detail::err(errno_to_network_error());
      }

      ::fcntl(client_fd, F_SETFL, ::fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
      ::fcntl(client_fd, F_SETFD, FD_CLOEXEC);

      auto client_handle = tcp_sockets.create(
          tcp_socket_data{RealFd{client_fd}, s->family, tcp_state::connected});

      int fd2 = ::dup(client_fd);
      if (fd2 < 0)
         return sock_detail::err(errno_to_network_error());
      ::fcntl(fd2, F_SETFL, ::fcntl(fd2, F_GETFL, 0) | O_NONBLOCK);

      auto is = io.create_input_stream(RealFd{client_fd});
      auto os = io.create_output_stream(RealFd{fd2});

      return sock_detail::ok(std::make_tuple(
          psio1::own<tcp_socket>{client_handle},
          std::move(is), std::move(os)));
   }

   // ── tcp address queries ───────────────────────────────────────────

   socket_result<ip_socket_address> tcp_socket_local_address(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage ss;
      socklen_t               len = sizeof(ss);
      if (::getsockname(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(from_sockaddr(ss));
   }

   socket_result<ip_socket_address> tcp_socket_remote_address(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage ss;
      socklen_t               len = sizeof(ss);
      if (::getpeername(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(from_sockaddr(ss));
   }

   bool tcp_socket_is_listening(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      return s && s->state == tcp_state::listening;
   }

   ip_address_family tcp_socket_address_family(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      return s ? s->family : ip_address_family::ipv4;
   }

   socket_result_void tcp_socket_set_listen_backlog_size(
       psio1::borrow<tcp_socket> self, uint64_t value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      s->backlog = static_cast<uint32_t>(value > 4096 ? 4096 : value);
      return sock_detail::ok();
   }

   // ── tcp socket options ────────────────────────────────────────────

   socket_result<bool> tcp_socket_keep_alive_enabled(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val = 0;
      socklen_t len = sizeof(val);
      ::getsockopt(*s->fd, SOL_SOCKET, SO_KEEPALIVE, &val, &len);
      return sock_detail::ok(val != 0);
   }

   socket_result_void tcp_socket_set_keep_alive_enabled(
       psio1::borrow<tcp_socket> self, bool value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val = value ? 1 : 0;
      if (::setsockopt(*s->fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result<wasi_duration> tcp_socket_keep_alive_idle_time(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
#ifdef TCP_KEEPIDLE
      int       val = 0;
      socklen_t len = sizeof(val);
      ::getsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, &len);
      return sock_detail::ok(static_cast<wasi_duration>(val) * wasi_duration{1'000'000'000});
#elif defined(TCP_KEEPALIVE)
      int       val = 0;
      socklen_t len = sizeof(val);
      ::getsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, &len);
      return sock_detail::ok(static_cast<wasi_duration>(val) * wasi_duration{1'000'000'000});
#else
      return sock_detail::err(neterr::not_supported);
#endif
   }

   socket_result_void tcp_socket_set_keep_alive_idle_time(
       psio1::borrow<tcp_socket> self, wasi_duration value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int secs = static_cast<int>(value / wasi_duration{1'000'000'000});
      if (secs < 1)
         secs = 1;
#ifdef TCP_KEEPIDLE
      if (::setsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPIDLE, &secs, sizeof(secs)) < 0)
         return sock_detail::err(errno_to_network_error());
#elif defined(TCP_KEEPALIVE)
      if (::setsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPALIVE, &secs, sizeof(secs)) < 0)
         return sock_detail::err(errno_to_network_error());
#else
      (void)secs;
      return sock_detail::err(neterr::not_supported);
#endif
      return sock_detail::ok();
   }

   socket_result<wasi_duration> tcp_socket_keep_alive_interval(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
#ifdef TCP_KEEPINTVL
      int       val = 0;
      socklen_t len = sizeof(val);
      ::getsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, &len);
      return sock_detail::ok(static_cast<wasi_duration>(val) * wasi_duration{1'000'000'000});
#else
      return sock_detail::err(neterr::not_supported);
#endif
   }

   socket_result_void tcp_socket_set_keep_alive_interval(
       psio1::borrow<tcp_socket> self, wasi_duration value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
#ifdef TCP_KEEPINTVL
      int secs = static_cast<int>(value / wasi_duration{1'000'000'000});
      if (secs < 1)
         secs = 1;
      if (::setsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPINTVL, &secs, sizeof(secs)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
#else
      return sock_detail::err(neterr::not_supported);
#endif
   }

   socket_result<uint32_t> tcp_socket_keep_alive_count(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
#ifdef TCP_KEEPCNT
      int       val = 0;
      socklen_t len = sizeof(val);
      ::getsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPCNT, &val, &len);
      return sock_detail::ok(static_cast<uint32_t>(val));
#else
      return sock_detail::err(neterr::not_supported);
#endif
   }

   socket_result_void tcp_socket_set_keep_alive_count(
       psio1::borrow<tcp_socket> self, uint32_t value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
#ifdef TCP_KEEPCNT
      int val = static_cast<int>(value);
      if (::setsockopt(*s->fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
#else
      return sock_detail::err(neterr::not_supported);
#endif
   }

   socket_result<uint8_t> tcp_socket_hop_limit(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val   = 0;
      socklen_t len   = sizeof(val);
      int       level = (s->family == ip_address_family::ipv4) ? IPPROTO_IP : IPPROTO_IPV6;
      int       opt   = (s->family == ip_address_family::ipv4) ? IP_TTL : IPV6_UNICAST_HOPS;
      if (::getsockopt(*s->fd, level, opt, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint8_t>(val));
   }

   socket_result_void tcp_socket_set_hop_limit(
       psio1::borrow<tcp_socket> self, uint8_t value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val   = value;
      int level = (s->family == ip_address_family::ipv4) ? IPPROTO_IP : IPPROTO_IPV6;
      int opt   = (s->family == ip_address_family::ipv4) ? IP_TTL : IPV6_UNICAST_HOPS;
      if (::setsockopt(*s->fd, level, opt, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result<uint64_t> tcp_socket_receive_buffer_size(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val = 0;
      socklen_t len = sizeof(val);
      if (::getsockopt(*s->fd, SOL_SOCKET, SO_RCVBUF, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint64_t>(val));
   }

   socket_result_void tcp_socket_set_receive_buffer_size(
       psio1::borrow<tcp_socket> self, uint64_t value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val = static_cast<int>(value > INT_MAX ? INT_MAX : value);
      if (::setsockopt(*s->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result<uint64_t> tcp_socket_send_buffer_size(
       psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val = 0;
      socklen_t len = sizeof(val);
      if (::getsockopt(*s->fd, SOL_SOCKET, SO_SNDBUF, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint64_t>(val));
   }

   socket_result_void tcp_socket_set_send_buffer_size(
       psio1::borrow<tcp_socket> self, uint64_t value)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val = static_cast<int>(value > INT_MAX ? INT_MAX : value);
      if (::setsockopt(*s->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   psio1::own<pollable> tcp_socket_subscribe(psio1::borrow<tcp_socket> self)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return psio1::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
      EventKind events = Readable;
      if (s->state == tcp_state::connect_started)
         events = Writable;
      return psio1::own<pollable>{io.pollables.create(pollable_data{s->fd, events})};
   }

   socket_result_void tcp_socket_shutdown(
       psio1::borrow<tcp_socket> self, shutdown_type how)
   {
      auto* s = tcp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int sh = SHUT_RDWR;
      switch (how)
      {
         case shutdown_type::receive: sh = SHUT_RD; break;
         case shutdown_type::send:    sh = SHUT_WR; break;
         case shutdown_type::both:    sh = SHUT_RDWR; break;
      }
      if (::shutdown(*s->fd, sh) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   // ── wasi:sockets/udp-create-socket ────────────────────────────────

   socket_result<psio1::own<udp_socket>> create_udp_socket(
       ip_address_family family)
   {
      int af = (family == ip_address_family::ipv4) ? AF_INET : AF_INET6;
      int fd = make_nonblocking_socket(af, SOCK_DGRAM);
      if (fd < 0)
         return sock_detail::err(errno_to_network_error());
      auto handle = udp_sockets.create(udp_socket_data{RealFd{fd}, family});
      return sock_detail::ok(psio1::own<udp_socket>{handle});
   }

   // ── wasi:sockets/udp ─────────────────────────────────────────────

   socket_result_void udp_socket_start_bind(
       psio1::borrow<udp_socket> self,
       psio1::borrow<network>    /*net*/,
       ip_socket_address        local_address)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);

      struct sockaddr_storage ss;
      socklen_t len = to_sockaddr(local_address, ss);
      if (::bind(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result_void udp_socket_finish_bind(psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      s->bound = true;
      return sock_detail::ok();
   }

   socket_result<std::tuple<psio1::own<incoming_datagram_stream>,
                             psio1::own<outgoing_datagram_stream>>>
   udp_socket_stream(
       psio1::borrow<udp_socket>             self,
       std::optional<ip_socket_address>     remote_address)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);

      if (remote_address)
      {
         struct sockaddr_storage ss;
         socklen_t len = to_sockaddr(*remote_address, ss);
         if (::connect(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), len) < 0)
            return sock_detail::err(errno_to_network_error());
      }

      return sock_detail::err(neterr::not_supported);
   }

   socket_result<ip_socket_address> udp_socket_local_address(
       psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      struct sockaddr_storage ss;
      socklen_t               len = sizeof(ss);
      if (::getsockname(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(from_sockaddr(ss));
   }

   socket_result<ip_socket_address> udp_socket_remote_address(
       psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      struct sockaddr_storage ss;
      socklen_t               len = sizeof(ss);
      if (::getpeername(*s->fd, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(from_sockaddr(ss));
   }

   ip_address_family udp_socket_address_family(psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      return s ? s->family : ip_address_family::ipv4;
   }

   socket_result<uint8_t> udp_socket_unicast_hop_limit(
       psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val   = 0;
      socklen_t len   = sizeof(val);
      int       level = (s->family == ip_address_family::ipv4) ? IPPROTO_IP : IPPROTO_IPV6;
      int       opt   = (s->family == ip_address_family::ipv4) ? IP_TTL : IPV6_UNICAST_HOPS;
      if (::getsockopt(*s->fd, level, opt, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint8_t>(val));
   }

   socket_result_void udp_socket_set_unicast_hop_limit(
       psio1::borrow<udp_socket> self, uint8_t value)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val   = value;
      int level = (s->family == ip_address_family::ipv4) ? IPPROTO_IP : IPPROTO_IPV6;
      int opt   = (s->family == ip_address_family::ipv4) ? IP_TTL : IPV6_UNICAST_HOPS;
      if (::setsockopt(*s->fd, level, opt, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result<uint64_t> udp_socket_receive_buffer_size(
       psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val = 0;
      socklen_t len = sizeof(val);
      if (::getsockopt(*s->fd, SOL_SOCKET, SO_RCVBUF, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint64_t>(val));
   }

   socket_result_void udp_socket_set_receive_buffer_size(
       psio1::borrow<udp_socket> self, uint64_t value)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val = static_cast<int>(value > INT_MAX ? INT_MAX : value);
      if (::setsockopt(*s->fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   socket_result<uint64_t> udp_socket_send_buffer_size(
       psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int       val = 0;
      socklen_t len = sizeof(val);
      if (::getsockopt(*s->fd, SOL_SOCKET, SO_SNDBUF, &val, &len) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok(static_cast<uint64_t>(val));
   }

   socket_result_void udp_socket_set_send_buffer_size(
       psio1::borrow<udp_socket> self, uint64_t value)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return sock_detail::err(neterr::invalid_state);
      int val = static_cast<int>(value > INT_MAX ? INT_MAX : value);
      if (::setsockopt(*s->fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)) < 0)
         return sock_detail::err(errno_to_network_error());
      return sock_detail::ok();
   }

   psio1::own<pollable> udp_socket_subscribe(psio1::borrow<udp_socket> self)
   {
      auto* s = udp_sockets.get(self.handle);
      if (!s)
         return psio1::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
      return psio1::own<pollable>{io.pollables.create(pollable_data{s->fd, Readable})};
   }

   // ── datagram stream stubs ─────────────────────────────────────────

   socket_result<std::vector<incoming_datagram>>
   incoming_datagram_stream_receive(
       psio1::borrow<incoming_datagram_stream> /*self*/, uint64_t /*max*/)
   {
      return sock_detail::ok(std::vector<incoming_datagram>{});
   }

   psio1::own<pollable> incoming_datagram_stream_subscribe(
       psio1::borrow<incoming_datagram_stream> /*self*/)
   {
      return psio1::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
   }

   socket_result<uint64_t> outgoing_datagram_stream_check_send(
       psio1::borrow<outgoing_datagram_stream> /*self*/)
   {
      return sock_detail::err(neterr::not_supported);
   }

   socket_result<uint64_t> outgoing_datagram_stream_send(
       psio1::borrow<outgoing_datagram_stream> /*self*/,
       std::vector<outgoing_datagram>         /*datagrams*/)
   {
      return sock_detail::err(neterr::not_supported);
   }

   psio1::own<pollable> outgoing_datagram_stream_subscribe(
       psio1::borrow<outgoing_datagram_stream> /*self*/)
   {
      return psio1::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
   }

   // ── wasi:sockets/ip-name-lookup ───────────────────────────────────

   socket_result<psio1::own<resolve_address_stream>> resolve_addresses(
       psio1::borrow<network> /*net*/, std::string /*name*/)
   {
      return sock_detail::err(neterr::not_supported);
   }

   socket_result<std::optional<ip_address>>
   resolve_address_stream_resolve_next_address(
       psio1::borrow<resolve_address_stream> /*self*/)
   {
      return sock_detail::ok<std::optional<ip_address>>(std::nullopt);
   }

   psio1::own<pollable> resolve_address_stream_subscribe(
       psio1::borrow<resolve_address_stream> /*self*/)
   {
      return psio1::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
   }
};

}  // namespace wasi_host

PSIO1_HOST_MODULE(wasi_host::WasiSocketsHost,
   interface(wasi_sockets_instance_network, instance_network),
   interface(wasi_sockets_network, network_error_code),
   interface(wasi_sockets_tcp_create_socket, create_tcp_socket),
   interface(wasi_sockets_tcp,
      tcp_socket_start_bind, tcp_socket_finish_bind,
      tcp_socket_start_connect, tcp_socket_finish_connect,
      tcp_socket_start_listen, tcp_socket_finish_listen,
      tcp_socket_accept,
      tcp_socket_local_address, tcp_socket_remote_address,
      tcp_socket_is_listening, tcp_socket_address_family,
      tcp_socket_set_listen_backlog_size,
      tcp_socket_keep_alive_enabled, tcp_socket_set_keep_alive_enabled,
      tcp_socket_keep_alive_idle_time, tcp_socket_set_keep_alive_idle_time,
      tcp_socket_keep_alive_interval, tcp_socket_set_keep_alive_interval,
      tcp_socket_keep_alive_count, tcp_socket_set_keep_alive_count,
      tcp_socket_hop_limit, tcp_socket_set_hop_limit,
      tcp_socket_receive_buffer_size, tcp_socket_set_receive_buffer_size,
      tcp_socket_send_buffer_size, tcp_socket_set_send_buffer_size,
      tcp_socket_subscribe, tcp_socket_shutdown),
   interface(wasi_sockets_udp_create_socket, create_udp_socket),
   interface(wasi_sockets_udp,
      udp_socket_start_bind, udp_socket_finish_bind,
      udp_socket_stream,
      udp_socket_local_address, udp_socket_remote_address,
      udp_socket_address_family,
      udp_socket_unicast_hop_limit, udp_socket_set_unicast_hop_limit,
      udp_socket_receive_buffer_size, udp_socket_set_receive_buffer_size,
      udp_socket_send_buffer_size, udp_socket_set_send_buffer_size,
      udp_socket_subscribe,
      incoming_datagram_stream_receive, incoming_datagram_stream_subscribe,
      outgoing_datagram_stream_check_send, outgoing_datagram_stream_send,
      outgoing_datagram_stream_subscribe),
   interface(wasi_sockets_ip_name_lookup,
      resolve_addresses,
      resolve_address_stream_resolve_next_address,
      resolve_address_stream_subscribe))
