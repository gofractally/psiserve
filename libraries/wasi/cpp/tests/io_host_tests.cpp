#include <wasi/0.2.3/io_host.hpp>

#include <catch2/catch.hpp>

#include <fcntl.h>
#include <unistd.h>

using namespace wasi_host;
using psiber::RealFd;
using psiber::Scheduler;

TEST_CASE("WasiIoHost input_stream read from pipe", "[wasi][io]")
{
   WasiIoHost host;

   int pipefd[2];
   REQUIRE(::pipe(pipefd) == 0);

   // Set read end non-blocking
   int flags = ::fcntl(pipefd[0], F_GETFL, 0);
   ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

   auto is = host.create_input_stream(RealFd{pipefd[0]});
   REQUIRE(is.handle != psizam::handle_table<stream_data, 256>::invalid_handle);

   // Write some data to the pipe
   const char* msg = "hello wasi";
   ::write(pipefd[1], msg, 10);

   // Read via WASI API
   auto result = host.input_stream_read(psio1::borrow<input_stream>{is.handle}, 64);
   REQUIRE(result.index() == 0);  // ok
   auto& data = std::get<0>(result);
   REQUIRE(data.size() == 10);
   REQUIRE(std::string(data.begin(), data.end()) == "hello wasi");

   ::close(pipefd[0]);
   ::close(pipefd[1]);
}

TEST_CASE("WasiIoHost output_stream write to pipe", "[wasi][io]")
{
   WasiIoHost host;

   int pipefd[2];
   REQUIRE(::pipe(pipefd) == 0);

   // Set write end non-blocking
   int flags = ::fcntl(pipefd[1], F_GETFL, 0);
   ::fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK);

   auto os = host.create_output_stream(RealFd{pipefd[1]});

   std::vector<uint8_t> content{'W', 'A', 'S', 'I'};
   auto result = host.output_stream_write(
       psio1::borrow<output_stream>{os.handle}, std::move(content));
   REQUIRE(result.index() == 0);  // ok

   // Verify data arrived
   char buf[16] = {};
   ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
   REQUIRE(n == 4);
   REQUIRE(std::string(buf, 4) == "WASI");

   ::close(pipefd[0]);
   ::close(pipefd[1]);
}

TEST_CASE("WasiIoHost check_write returns capacity", "[wasi][io]")
{
   WasiIoHost host;

   int pipefd[2];
   REQUIRE(::pipe(pipefd) == 0);

   auto os = host.create_output_stream(RealFd{pipefd[1]});

   auto result = host.output_stream_check_write(
       psio1::borrow<output_stream>{os.handle});
   REQUIRE(result.index() == 0);
   REQUIRE(std::get<0>(result) == 65536);

   ::close(pipefd[0]);
   ::close(pipefd[1]);
}

TEST_CASE("WasiIoHost pollable_ready on readable pipe", "[wasi][io]")
{
   WasiIoHost host;

   int pipefd[2];
   REQUIRE(::pipe(pipefd) == 0);

   int flags = ::fcntl(pipefd[0], F_GETFL, 0);
   ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

   auto is = host.create_input_stream(RealFd{pipefd[0]});
   auto p = host.input_stream_subscribe(psio1::borrow<input_stream>{is.handle});

   // Not ready yet (nothing written)
   REQUIRE_FALSE(host.pollable_ready(psio1::borrow<pollable>{p.handle}));

   // Write data
   ::write(pipefd[1], "x", 1);

   // Now should be ready
   REQUIRE(host.pollable_ready(psio1::borrow<pollable>{p.handle}));

   ::close(pipefd[0]);
   ::close(pipefd[1]);
}

TEST_CASE("WasiIoHost error_to_debug_string", "[wasi][io]")
{
   WasiIoHost host;

   auto handle = host.errors.create(io_error_data{"test error message"});
   auto result = host.error_to_debug_string(psio1::borrow<io_error>{handle});
   REQUIRE(std::string(result.begin(), result.end()) == "test error message");
}

TEST_CASE("WasiIoHost closed stream returns error", "[wasi][io]")
{
   WasiIoHost host;

   // Invalid handle
   auto result = host.input_stream_read(
       psio1::borrow<input_stream>{psizam::handle_table<stream_data, 256>::invalid_handle}, 64);
   REQUIRE(result.index() == 1);  // err
   REQUIRE(std::get<1>(result).tag == stream_error::closed);
}
