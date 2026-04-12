#include <psiber/dns.hpp>
#include <psiber/scheduler.hpp>

#include <ares.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace psiber
{
   // ── One-time c-ares library init ──────────────────────────────────────────

   namespace
   {
      struct ares_lib_guard
      {
         ares_lib_guard() { ares_library_init(ARES_LIB_INIT_ALL); }
         ~ares_lib_guard() { ares_library_cleanup(); }
      };
      static ares_lib_guard s_ares_lib;

      // ── Per-resolution state ───────────────────────────────────────────

      struct sock_watch
      {
         int  fd;
         bool want_read;
         bool want_write;
      };

      struct resolve_ctx
      {
         bool                                 done   = false;
         int                                  status = ARES_SUCCESS;
         std::vector<struct sockaddr_storage>  addrs;
         std::vector<sock_watch>              watches;
      };

      void sock_state_cb(void* data, ares_socket_t fd, int readable, int writable)
      {
         auto* ctx = static_cast<resolve_ctx*>(data);

         if (!readable && !writable)
         {
            ctx->watches.erase(
                std::remove_if(ctx->watches.begin(), ctx->watches.end(),
                               [fd](const sock_watch& w) {
                                  return w.fd == static_cast<int>(fd);
                               }),
                ctx->watches.end());
            return;
         }

         for (auto& w : ctx->watches)
         {
            if (w.fd == static_cast<int>(fd))
            {
               w.want_read  = readable;
               w.want_write = writable;
               return;
            }
         }
         ctx->watches.push_back({static_cast<int>(fd), bool(readable), bool(writable)});
      }

      void addrinfo_cb(void* arg, int status, int /*timeouts*/, struct ares_addrinfo* ai)
      {
         auto* ctx   = static_cast<resolve_ctx*>(arg);
         ctx->status = status;
         ctx->done   = true;

         if (status == ARES_SUCCESS && ai)
         {
            for (auto* node = ai->nodes; node; node = node->ai_next)
            {
               struct sockaddr_storage addr{};
               std::memcpy(&addr, node->ai_addr, node->ai_addrlen);
               ctx->addrs.push_back(addr);
            }
            ares_freeaddrinfo(ai);
         }
      }
   }  // namespace

   // ── dns_resolver ──────────────────────────────────────────────────────────

   dns_resolver::dns_resolver()  = default;
   dns_resolver::~dns_resolver() = default;

   std::vector<struct sockaddr_storage> dns_resolver::resolve(Scheduler&       sched,
                                                              std::string_view host,
                                                              uint16_t         port)
   {
      resolve_ctx ctx;

      // Create a channel with socket state callback for fiber-aware polling
      ares_channel_t* ch = nullptr;
      struct ares_options opts{};
      opts.sock_state_cb      = sock_state_cb;
      opts.sock_state_cb_data = &ctx;
      int rc = ares_init_options(&ch, &opts, ARES_OPT_SOCK_STATE_CB);
      if (rc != ARES_SUCCESS)
         throw std::runtime_error(std::string("ares_init_options failed: ") +
                                  ares_strerror(rc));

      struct ares_addrinfo_hints hints{};
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_family   = AF_UNSPEC;

      std::string host_str(host);
      char        port_str[8];
      std::snprintf(port_str, sizeof(port_str), "%u", port);

      ares_getaddrinfo(ch, host_str.c_str(),
                       port > 0 ? port_str : nullptr,
                       &hints, addrinfo_cb, &ctx);

      // Drive c-ares I/O, yielding the fiber on each fd wait
      while (!ctx.done)
      {
         if (ctx.watches.empty())
         {
            sched.yieldCurrentFiber();
            continue;
         }

         auto& w = ctx.watches.front();
         EventKind ev = w.want_read ? Readable : Writable;
         sched.yield(RealFd{w.fd}, ev);

         ares_socket_t rfd = w.want_read  ? static_cast<ares_socket_t>(w.fd) : ARES_SOCKET_BAD;
         ares_socket_t wfd = w.want_write ? static_cast<ares_socket_t>(w.fd) : ARES_SOCKET_BAD;
         ares_process_fd(ch, rfd, wfd);
      }

      ares_destroy(ch);

      if (ctx.status != ARES_SUCCESS)
         throw std::runtime_error(std::string("DNS resolution failed for '") +
                                  host_str + "': " + ares_strerror(ctx.status));

      if (ctx.addrs.empty())
         throw std::runtime_error("DNS resolution returned no addresses for '" +
                                  host_str + "'");

      return std::move(ctx.addrs);
   }

}  // namespace psiber
