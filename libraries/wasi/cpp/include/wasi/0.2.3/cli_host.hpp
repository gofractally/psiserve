#pragma once

#include <wasi/0.2.3/cli.hpp>
#include <wasi/0.2.3/io_host.hpp>

#include <psio/structural.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

extern "C" char** environ;

namespace wasi_host {

struct WasiCliHost
{
   WasiIoHost& io;

   std::vector<std::string>                          args;
   std::vector<std::tuple<std::string, std::string>> env_vars;
   std::optional<std::string>                        cwd;

   explicit WasiCliHost(WasiIoHost& io_host) : io(io_host)
   {
      for (char** e = environ; e && *e; ++e)
      {
         std::string entry(*e);
         auto pos = entry.find('=');
         if (pos != std::string::npos)
            env_vars.emplace_back(entry.substr(0, pos), entry.substr(pos + 1));
      }

      char buf[4096];
      if (::getcwd(buf, sizeof(buf)))
         cwd = std::string(buf);
   }

   std::vector<std::tuple<std::string, std::string>> get_environment()
   {
      return env_vars;
   }

   std::vector<std::string> get_arguments()
   {
      return args;
   }

   std::optional<std::string> initial_cwd()
   {
      return cwd;
   }

   // ── wasi:cli/stdin, stdout, stderr ────────────────────────────────

   psio::own<input_stream> get_stdin()
   {
      int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
      ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
      return io.create_input_stream(RealFd{STDIN_FILENO});
   }

   psio::own<output_stream> get_stdout()
   {
      return io.create_output_stream(RealFd{STDOUT_FILENO});
   }

   psio::own<output_stream> get_stderr()
   {
      return io.create_output_stream(RealFd{STDERR_FILENO});
   }
};

}  // namespace wasi_host

PSIO_HOST_MODULE(wasi_host::WasiCliHost,
   interface(environment, get_environment, get_arguments, initial_cwd))
