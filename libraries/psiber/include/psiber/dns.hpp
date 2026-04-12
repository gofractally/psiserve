#pragma once

#include <psiber/types.hpp>

#include <cstdint>
#include <string_view>
#include <vector>
#include <sys/socket.h>

namespace psiber
{
   class Scheduler;

   /// Fiber-aware asynchronous DNS resolver backed by c-ares.
   ///
   /// Integrates with the fiber scheduler: the calling fiber yields
   /// while DNS packets are in flight and resumes when the response
   /// arrives.  No blocking, no threads, no callbacks visible to the
   /// caller — just linear code:
   ///
   ///     dns_resolver resolver;
   ///     auto addrs = resolver.resolve(sched, "api.example.com", 443);
   ///     auto sock  = tcp_socket::connect(sched, addrs[0]);
   ///
   class dns_resolver
   {
     public:
      dns_resolver();
      ~dns_resolver();

      dns_resolver(const dns_resolver&)            = delete;
      dns_resolver& operator=(const dns_resolver&) = delete;

      /// Resolve a hostname to a list of socket addresses.
      /// The returned addresses have the port already set.
      /// Yields the calling fiber during DNS lookup.
      /// Throws std::runtime_error on resolution failure.
      std::vector<struct sockaddr_storage> resolve(Scheduler&       sched,
                                                   std::string_view host,
                                                   uint16_t         port = 0);

   };

}  // namespace psiber
