#pragma once

// wasi:io@0.2.3 host implementation — real I/O backed by psiber scheduler.
//
// WasiIoHost manages handle tables for io_error, pollable, input_stream,
// and output_stream resources.  Blocking operations yield the calling
// fiber via the psiber scheduler.
//
// Other WASI host modules (sockets, filesystem) create streams via
// the public create_input_stream / create_output_stream methods.

#include <wasi/0.2.3/io.hpp>

#include <psiber/scheduler.hpp>
#include <psiber/types.hpp>
#include <psizam/handle_table.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace wasi_host {

using psiber::EventKind;
using psiber::Readable;
using psiber::RealFd;
using psiber::Scheduler;
using psiber::Writable;
using psiber::invalid_real_fd;

struct io_error_data
{
   std::string message;
};

struct pollable_data
{
   RealFd    fd;
   EventKind events;
};

struct stream_data
{
   RealFd fd;
   bool   closed = false;
};

static constexpr stream_error closed_err{stream_error::closed};

struct WasiIoHost
{
   psizam::handle_table<io_error_data, 64>  errors{64};
   psizam::handle_table<pollable_data, 256> pollables{256};
   psizam::handle_table<stream_data, 256>   istreams{256};
   psizam::handle_table<stream_data, 256>   ostreams{256};

   // ── Factory methods (called by sockets/filesystem hosts) ──────────

   psio::own<input_stream> create_input_stream(RealFd fd)
   {
      return psio::own<input_stream>{istreams.create(stream_data{fd})};
   }

   psio::own<output_stream> create_output_stream(RealFd fd)
   {
      return psio::own<output_stream>{ostreams.create(stream_data{fd})};
   }

   void close_stream_fd(RealFd fd)
   {
      // Mark all streams backed by this fd as closed
      // (Called when the owning socket is dropped)
   }

   // ── wasi:io/error ─────────────────────────────────────────────────

   std::vector<uint8_t> error_to_debug_string(psio::borrow<io_error> self)
   {
      auto* e = errors.get(self.handle);
      if (!e)
         return {};
      return std::vector<uint8_t>(e->message.begin(), e->message.end());
   }

   // ── wasi:io/poll ──────────────────────────────────────────────────

   bool pollable_ready(psio::borrow<pollable> self)
   {
      auto* p = pollables.get(self.handle);
      if (!p)
         return true;

      struct pollfd pfd;
      pfd.fd = *p->fd;
      pfd.events = 0;
      if (p->events & Readable)
         pfd.events |= POLLIN;
      if (p->events & Writable)
         pfd.events |= POLLOUT;
      pfd.revents = 0;
      int r = ::poll(&pfd, 1, 0);
      return r > 0;
   }

   void pollable_block(psio::borrow<pollable> self)
   {
      auto* p = pollables.get(self.handle);
      if (!p)
         return;
      Scheduler::current().yield(p->fd, p->events);
   }

   std::vector<uint32_t> poll(std::vector<psio::borrow<pollable>> in)
   {
      auto check_ready = [&](uint32_t idx) -> bool {
         auto* p = pollables.get(in[idx].handle);
         if (!p)
            return true;
         struct pollfd pfd;
         pfd.fd = *p->fd;
         pfd.events = 0;
         if (p->events & Readable)
            pfd.events |= POLLIN;
         if (p->events & Writable)
            pfd.events |= POLLOUT;
         pfd.revents = 0;
         return ::poll(&pfd, 1, 0) > 0;
      };

      std::vector<uint32_t> ready;
      for (uint32_t i = 0; i < in.size(); ++i)
      {
         if (check_ready(i))
            ready.push_back(i);
      }
      if (!ready.empty())
         return ready;

      // None ready — block on the first valid pollable
      for (uint32_t i = 0; i < in.size(); ++i)
      {
         auto* p = pollables.get(in[i].handle);
         if (p)
         {
            Scheduler::current().yield(p->fd, p->events);
            break;
         }
      }

      // Re-check after waking
      for (uint32_t i = 0; i < in.size(); ++i)
      {
         if (check_ready(i))
            ready.push_back(i);
      }
      if (ready.empty())
         ready.push_back(0);
      return ready;
   }

   // ── wasi:io/streams — input-stream ────────────────────────────────

   wasi_result<std::vector<uint8_t>> input_stream_read(
       psio::borrow<input_stream> self, uint64_t len)
   {
      auto* s = istreams.get(self.handle);
      if (!s || s->closed)
         return wasi_io_detail::err<std::vector<uint8_t>>(closed_err);

      uint64_t cap = len > 65536 ? 65536 : len;
      std::vector<uint8_t> buf(cap);

      ssize_t n = ::read(*s->fd, buf.data(), cap);
      if (n > 0)
      {
         buf.resize(static_cast<size_t>(n));
         return wasi_io_detail::ok(std::move(buf));
      }
      if (n == 0)
         return wasi_io_detail::err<std::vector<uint8_t>>(closed_err);
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
         buf.clear();
         return wasi_io_detail::ok(std::move(buf));
      }
      return wasi_io_detail::err<std::vector<uint8_t>>(
          {stream_error::last_operation_failed, errors.create(io_error_data{strerror(errno)})});
   }

   wasi_result<std::vector<uint8_t>> input_stream_blocking_read(
       psio::borrow<input_stream> self, uint64_t len)
   {
      auto* s = istreams.get(self.handle);
      if (!s || s->closed)
         return wasi_io_detail::err<std::vector<uint8_t>>(closed_err);

      uint64_t cap = len > 65536 ? 65536 : len;
      std::vector<uint8_t> buf(cap);

      for (;;)
      {
         ssize_t n = ::read(*s->fd, buf.data(), cap);
         if (n > 0)
         {
            buf.resize(static_cast<size_t>(n));
            return wasi_io_detail::ok(std::move(buf));
         }
         if (n == 0)
            return wasi_io_detail::err<std::vector<uint8_t>>(closed_err);
         if (errno != EAGAIN && errno != EWOULDBLOCK)
            return wasi_io_detail::err<std::vector<uint8_t>>(
                {stream_error::last_operation_failed,
                 errors.create(io_error_data{strerror(errno)})});
         Scheduler::current().yield(s->fd, Readable);
      }
   }

   wasi_result<uint64_t> input_stream_skip(
       psio::borrow<input_stream> self, uint64_t len)
   {
      auto result = input_stream_read(self, len);
      if (result.index() == 0)
         return wasi_io_detail::ok(static_cast<uint64_t>(std::get<0>(result).size()));
      return wasi_io_detail::err<uint64_t>(std::get<1>(result));
   }

   wasi_result<uint64_t> input_stream_blocking_skip(
       psio::borrow<input_stream> self, uint64_t len)
   {
      auto result = input_stream_blocking_read(self, len);
      if (result.index() == 0)
         return wasi_io_detail::ok(static_cast<uint64_t>(std::get<0>(result).size()));
      return wasi_io_detail::err<uint64_t>(std::get<1>(result));
   }

   psio::own<pollable> input_stream_subscribe(psio::borrow<input_stream> self)
   {
      auto* s = istreams.get(self.handle);
      if (!s || s->closed)
         return psio::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
      return psio::own<pollable>{pollables.create(pollable_data{s->fd, Readable})};
   }

   // ── wasi:io/streams — output-stream ───────────────────────────────

   wasi_result<uint64_t> output_stream_check_write(psio::borrow<output_stream> self)
   {
      auto* s = ostreams.get(self.handle);
      if (!s || s->closed)
         return wasi_io_detail::err<uint64_t>(closed_err);
      // Report available buffer space — 64KB is a reasonable default
      return wasi_io_detail::ok(uint64_t{65536});
   }

   wasi_result_void output_stream_write(
       psio::borrow<output_stream> self, std::vector<uint8_t> contents)
   {
      auto* s = ostreams.get(self.handle);
      if (!s || s->closed)
         return wasi_io_detail::err(closed_err);

      const uint8_t* data = contents.data();
      size_t         remaining = contents.size();

      while (remaining > 0)
      {
         ssize_t n = ::write(*s->fd, data, remaining);
         if (n > 0)
         {
            data += n;
            remaining -= static_cast<size_t>(n);
            continue;
         }
         if (n == 0)
            return wasi_io_detail::err(closed_err);
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            Scheduler::current().yield(s->fd, Writable);
            continue;
         }
         return wasi_io_detail::err(
             {stream_error::last_operation_failed,
              errors.create(io_error_data{strerror(errno)})});
      }
      return wasi_io_detail::ok();
   }

   wasi_result_void output_stream_blocking_write_and_flush(
       psio::borrow<output_stream> self, std::vector<uint8_t> contents)
   {
      return output_stream_write(self, std::move(contents));
   }

   wasi_result_void output_stream_flush(psio::borrow<output_stream> self)
   {
      auto* s = ostreams.get(self.handle);
      if (!s || s->closed)
         return wasi_io_detail::err(closed_err);
      return wasi_io_detail::ok();
   }

   wasi_result_void output_stream_blocking_flush(psio::borrow<output_stream> self)
   {
      return output_stream_flush(self);
   }

   psio::own<pollable> output_stream_subscribe(psio::borrow<output_stream> self)
   {
      auto* s = ostreams.get(self.handle);
      if (!s || s->closed)
         return psio::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
      return psio::own<pollable>{pollables.create(pollable_data{s->fd, Writable})};
   }

   wasi_result_void output_stream_write_zeroes(
       psio::borrow<output_stream> self, uint64_t len)
   {
      uint64_t cap = len > 65536 ? 65536 : len;
      std::vector<uint8_t> zeroes(cap, 0);
      return output_stream_write(self, std::move(zeroes));
   }

   wasi_result_void output_stream_blocking_write_zeroes_and_flush(
       psio::borrow<output_stream> self, uint64_t len)
   {
      return output_stream_write_zeroes(self, len);
   }

   wasi_result<uint64_t> output_stream_splice(
       psio::borrow<output_stream> self, psio::borrow<input_stream> src, uint64_t len)
   {
      auto read_result = input_stream_read(src, len);
      if (read_result.index() == 1)
         return wasi_io_detail::err<uint64_t>(std::get<1>(read_result));

      auto& data = std::get<0>(read_result);
      uint64_t count = data.size();
      auto write_result = output_stream_write(self, std::move(data));
      if (write_result.index() == 1)
         return wasi_io_detail::err<uint64_t>(std::get<1>(write_result));

      return wasi_io_detail::ok(count);
   }

   wasi_result<uint64_t> output_stream_blocking_splice(
       psio::borrow<output_stream> self, psio::borrow<input_stream> src, uint64_t len)
   {
      auto read_result = input_stream_blocking_read(src, len);
      if (read_result.index() == 1)
         return wasi_io_detail::err<uint64_t>(std::get<1>(read_result));

      auto& data = std::get<0>(read_result);
      uint64_t count = data.size();
      auto write_result = output_stream_write(self, std::move(data));
      if (write_result.index() == 1)
         return wasi_io_detail::err<uint64_t>(std::get<1>(write_result));

      return wasi_io_detail::ok(count);
   }
};

}  // namespace wasi_host

PSIO_HOST_MODULE(wasi_host::WasiIoHost,
   interface(wasi_io_error, error_to_debug_string),
   interface(wasi_io_poll, pollable_ready, pollable_block, poll),
   interface(wasi_io_streams,
      input_stream_read, input_stream_blocking_read,
      input_stream_skip, input_stream_blocking_skip,
      input_stream_subscribe,
      output_stream_check_write,
      output_stream_write, output_stream_blocking_write_and_flush,
      output_stream_flush, output_stream_blocking_flush,
      output_stream_subscribe,
      output_stream_write_zeroes, output_stream_blocking_write_zeroes_and_flush,
      output_stream_splice, output_stream_blocking_splice))
