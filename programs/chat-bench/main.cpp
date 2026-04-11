/*
 * chat-bench — Latency benchmark for the psiserve chat server
 *
 * Starts psiserve with chat.wasm, then connects two TCP clients:
 *
 *   Ping client (A): sends "PING <T1>\n" where T1 is a nanosecond timestamp
 *   Echo client (B): receives the ping, sends "ECHO <T1> <T2> <T3>\n"
 *                     where T2 = receive time, T3 = send time
 *   Ping client (A): receives the echo at T4, computes:
 *                     A→B  = T2 - T1
 *                     B→A  = T4 - T3
 *                     trip = T4 - T1
 *
 * Usage: chat-bench <psiserve-binary> <chat.wasm> [--port PORT] [--count N]
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

// ── Timing ──────────────────────────────────────────────────────────────────────

static uint64_t now_ns()
{
   auto t = std::chrono::steady_clock::now().time_since_epoch();
   return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count());
}

// ── Socket helpers ──────────────────────────────────────────────────────────────

static int connect_to(uint16_t port)
{
   int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
   {
      perror("socket");
      return -1;
   }

   int flag = 1;
   ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

   struct sockaddr_in addr = {};
   addr.sin_family         = AF_INET;
   addr.sin_port           = htons(port);
   addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);

   if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
   {
      ::close(fd);
      return -1;
   }
   return fd;
}

/// Read until we see a newline.  Returns the line (without newline), or empty on error.
static std::string read_line(int fd, char* residual_buf, int& residual_len)
{
   std::string line;

   // Check residual buffer first
   for (int i = 0; i < residual_len; ++i)
   {
      if (residual_buf[i] == '\n')
      {
         line.append(residual_buf, i);
         int remaining = residual_len - i - 1;
         if (remaining > 0)
            std::memmove(residual_buf, residual_buf + i + 1, remaining);
         residual_len = remaining;
         return line;
      }
   }

   // Consume what's in residual
   line.append(residual_buf, residual_len);
   residual_len = 0;

   // Read more
   char buf[1024];
   for (;;)
   {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0)
         return {};

      for (ssize_t i = 0; i < n; ++i)
      {
         if (buf[i] == '\n')
         {
            line.append(buf, i);
            int remaining = static_cast<int>(n - i - 1);
            if (remaining > 0)
            {
               std::memcpy(residual_buf, buf + i + 1, remaining);
               residual_len = remaining;
            }
            return line;
         }
      }
      line.append(buf, n);
   }
}

static bool write_all(int fd, const char* buf, int len)
{
   while (len > 0)
   {
      ssize_t n = ::write(fd, buf, len);
      if (n <= 0)
         return false;
      buf += n;
      len -= static_cast<int>(n);
   }
   return true;
}

// ── Latency record ──────────────────────────────────────────────────────────────

struct LatencySample
{
   uint64_t a_to_b_ns;  // T2 - T1
   uint64_t b_to_a_ns;  // T4 - T3
   uint64_t trip_ns;     // T4 - T1
};

// ── Echo client thread ──────────────────────────────────────────────────────────

static void echo_thread(int fd, std::atomic<bool>& done)
{
   char residual[1024];
   int  residual_len = 0;

   // Read and discard welcome message
   read_line(fd, residual, residual_len);

   while (!done.load(std::memory_order_relaxed))
   {
      std::string line = read_line(fd, residual, residual_len);
      if (line.empty())
         break;

      // Parse: "PING <T1>"
      if (line.rfind("PING ", 0) != 0)
         continue;

      uint64_t t2 = now_ns();

      const char* t1_str = line.c_str() + 5;

      // Build response: "ECHO <T1> <T2> <T3>\n"
      uint64_t t3  = now_ns();
      char     msg[256];
      int      len = snprintf(msg, sizeof(msg), "ECHO %s %llu %llu\n", t1_str, (unsigned long long)t2, (unsigned long long)t3);
      if (!write_all(fd, msg, len))
         break;
   }
}

// ── Statistics ──────────────────────────────────────────────────────────────────

static void print_stats(const char* label, std::vector<uint64_t>& samples)
{
   if (samples.empty())
      return;

   std::sort(samples.begin(), samples.end());

   auto   n      = samples.size();
   double mean   = std::accumulate(samples.begin(), samples.end(), 0.0) / n;
   double median = samples[n / 2];
   double p99    = samples[std::min(n - 1, (size_t)(n * 0.99))];
   double p999   = samples[std::min(n - 1, (size_t)(n * 0.999))];
   double mn     = samples.front();
   double mx     = samples.back();

   printf("  %-12s  min=%7.1f  mean=%7.1f  median=%7.1f  p99=%7.1f  p99.9=%7.1f  max=%7.1f µs\n",
          label,
          mn / 1000.0, mean / 1000.0, median / 1000.0,
          p99 / 1000.0, p999 / 1000.0, mx / 1000.0);
}

// ── Main ────────────────────────────────────────────────────────────────────────

static void usage(const char* argv0)
{
   fprintf(stderr, "Usage: %s <psiserve-binary> <chat.wasm> [--port PORT] [--count N] [--warmup W]\n", argv0);
}

int main(int argc, char* argv[])
{
   if (argc < 3)
   {
      usage(argv[0]);
      return 1;
   }

   const char* psiserve_bin = argv[1];
   const char* chat_wasm    = argv[2];
   uint16_t    port         = 9990;
   int         count        = 10000;
   int         warmup       = 100;

   for (int i = 3; i < argc; i += 2)
   {
      if (i + 1 >= argc)
      {
         usage(argv[0]);
         return 1;
      }
      if (strcmp(argv[i], "--port") == 0)
         port = static_cast<uint16_t>(atoi(argv[i + 1]));
      else if (strcmp(argv[i], "--count") == 0)
         count = atoi(argv[i + 1]);
      else if (strcmp(argv[i], "--warmup") == 0)
         warmup = atoi(argv[i + 1]);
   }

   // Start psiserve as child process
   printf("Starting psiserve on port %d...\n", port);

   pid_t child = fork();
   if (child < 0)
   {
      perror("fork");
      return 1;
   }

   if (child == 0)
   {
      // Child: exec psiserve
      char port_str[16];
      snprintf(port_str, sizeof(port_str), "%d", port);
      execl(psiserve_bin, psiserve_bin, "-p", port_str, chat_wasm, nullptr);
      perror("execl");
      _exit(1);
   }

   // Parent: wait for server to be ready
   int server_fd = -1;
   for (int attempt = 0; attempt < 50; ++attempt)
   {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      server_fd = connect_to(port);
      if (server_fd >= 0)
      {
         ::close(server_fd);
         break;
      }
   }
   if (server_fd < 0)
   {
      fprintf(stderr, "Could not connect to server after 5 seconds\n");
      kill(child, SIGTERM);
      waitpid(child, nullptr, 0);
      return 1;
   }

   printf("Server ready. Connecting clients...\n");

   // Connect ping client (A) and echo client (B)
   int ping_fd = connect_to(port);
   int echo_fd = connect_to(port);
   if (ping_fd < 0 || echo_fd < 0)
   {
      fprintf(stderr, "Failed to connect clients\n");
      kill(child, SIGTERM);
      waitpid(child, nullptr, 0);
      return 1;
   }

   // Give server a moment to accept both and send welcome messages
   std::this_thread::sleep_for(std::chrono::milliseconds(100));

   // Start echo client thread
   std::atomic<bool> done{false};
   std::thread       echo(echo_thread, echo_fd, std::ref(done));

   // Read and discard ping client's welcome message
   {
      char residual[1024];
      int  residual_len = 0;
      read_line(ping_fd, residual, residual_len);
      // If there's residual after the welcome line, we'd lose it —
      // but the welcome is the only unsolicited message, so this is fine.
   }

   // Give echo thread time to read its welcome
   std::this_thread::sleep_for(std::chrono::milliseconds(50));

   // Run benchmark
   printf("Running %d warmup + %d measured pings...\n", warmup, count);

   std::vector<LatencySample> samples;
   samples.reserve(count);

   char residual[1024];
   int  residual_len = 0;

   int total = warmup + count;
   for (int i = 0; i < total; ++i)
   {
      // Send PING with timestamp
      uint64_t t1 = now_ns();
      char     msg[128];
      int      len = snprintf(msg, sizeof(msg), "PING %llu\n", (unsigned long long)t1);

      if (!write_all(ping_fd, msg, len))
      {
         fprintf(stderr, "Write failed at iteration %d\n", i);
         break;
      }

      // Read ECHO response
      std::string response = read_line(ping_fd, residual, residual_len);
      uint64_t    t4       = now_ns();

      if (response.empty() || response.rfind("ECHO ", 0) != 0)
      {
         fprintf(stderr, "Bad response at iteration %d: '%s'\n", i, response.c_str());
         break;
      }

      // Parse: "ECHO <T1> <T2> <T3>"
      unsigned long long r_t1, r_t2, r_t3;
      if (sscanf(response.c_str(), "ECHO %llu %llu %llu", &r_t1, &r_t2, &r_t3) != 3)
      {
         fprintf(stderr, "Parse failed at iteration %d: '%s'\n", i, response.c_str());
         break;
      }

      if (i >= warmup)
      {
         samples.push_back({
            r_t2 - r_t1,   // A→B
            t4 - r_t3,     // B→A
            t4 - r_t1,     // total round trip
         });
      }
   }

   // Shut down
   done.store(true, std::memory_order_relaxed);
   ::close(ping_fd);
   ::close(echo_fd);
   echo.join();

   kill(child, SIGTERM);
   waitpid(child, nullptr, 0);

   // Report results
   if (samples.empty())
   {
      printf("No samples collected.\n");
      return 1;
   }

   printf("\nResults (%zu samples):\n", samples.size());

   std::vector<uint64_t> a_to_b, b_to_a, trip;
   a_to_b.reserve(samples.size());
   b_to_a.reserve(samples.size());
   trip.reserve(samples.size());

   for (auto& s : samples)
   {
      a_to_b.push_back(s.a_to_b_ns);
      b_to_a.push_back(s.b_to_a_ns);
      trip.push_back(s.trip_ns);
   }

   print_stats("A→B:", a_to_b);
   print_stats("B→A:", b_to_a);
   print_stats("round-trip:", trip);

   return 0;
}
