#include <wasi/0.2.3/filesystem_host.hpp>

#include <catch2/catch.hpp>

#include <cstdio>
#include <fstream>

using namespace wasi_host;
using psiber::Scheduler;

TEST_CASE("WasiFilesystemHost stat on cwd", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   int fd = ::open(".", O_RDONLY | O_DIRECTORY);
   REQUIRE(fd >= 0);
   auto h = fs.descriptors.create(descriptor_data{
       RealFd{fd}, descriptor_type::directory, "."});

   auto result = fs.descriptor_stat_fn(psio::borrow<descriptor>{h});
   REQUIRE(result.index() == 0);
   auto& st = std::get<0>(result);
   REQUIRE(st.type == descriptor_type::directory);
}

TEST_CASE("WasiFilesystemHost get_type", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   int fd = ::open(".", O_RDONLY | O_DIRECTORY);
   REQUIRE(fd >= 0);
   auto h = fs.descriptors.create(descriptor_data{
       RealFd{fd}, descriptor_type::directory, "."});

   auto result = fs.descriptor_get_type(psio::borrow<descriptor>{h});
   REQUIRE(result.index() == 0);
   REQUIRE(std::get<0>(result) == descriptor_type::directory);
}

TEST_CASE("WasiFilesystemHost read_directory", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   int fd = ::open(".", O_RDONLY | O_DIRECTORY);
   REQUIRE(fd >= 0);
   auto h = fs.descriptors.create(descriptor_data{
       RealFd{fd}, descriptor_type::directory, "."});

   auto stream_result = fs.descriptor_read_directory(psio::borrow<descriptor>{h});
   REQUIRE(stream_result.index() == 0);
   auto& stream = std::get<0>(stream_result);

   int count = 0;
   for (;;)
   {
      auto entry = fs.directory_entry_stream_read_directory_entry(
          psio::borrow<directory_entry_stream>{stream.handle});
      REQUIRE(entry.index() == 0);
      auto& opt = std::get<0>(entry);
      if (!opt)
         break;
      ++count;
   }
   REQUIRE(count > 0);
}

TEST_CASE("WasiFilesystemHost open_at and read", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   int fd = ::open(".", O_RDONLY | O_DIRECTORY);
   REQUIRE(fd >= 0);
   auto dir_h = fs.descriptors.create(descriptor_data{
       RealFd{fd}, descriptor_type::directory, "."});

   // Create a temp file
   std::string tmpfile = "wasi_fs_test_tmp.txt";
   {
      std::ofstream out(tmpfile);
      out << "hello filesystem";
   }

   path_flags pf{.symlink_follow = true};
   open_flags of{};
   descriptor_flags df{.read = true};

   auto open_result = fs.descriptor_open_at(
       psio::borrow<descriptor>{dir_h}, pf, tmpfile, of, df);
   REQUIRE(open_result.index() == 0);
   auto& file_desc = std::get<0>(open_result);

   auto read_result = fs.descriptor_read(
       psio::borrow<descriptor>{file_desc.handle}, 256, 0);
   REQUIRE(read_result.index() == 0);
   auto& [data, eof] = std::get<0>(read_result);
   REQUIRE(std::string(data.begin(), data.end()) == "hello filesystem");
   REQUIRE(eof == true);

   ::unlink(tmpfile.c_str());
}

TEST_CASE("WasiFilesystemHost stat_at", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   int fd = ::open(".", O_RDONLY | O_DIRECTORY);
   REQUIRE(fd >= 0);
   auto dir_h = fs.descriptors.create(descriptor_data{
       RealFd{fd}, descriptor_type::directory, "."});

   std::string tmpfile = "wasi_fs_stat_test.txt";
   { std::ofstream(tmpfile) << "data"; }

   path_flags pf{.symlink_follow = true};
   auto result = fs.descriptor_stat_at(
       psio::borrow<descriptor>{dir_h}, pf, tmpfile);
   REQUIRE(result.index() == 0);
   REQUIRE(std::get<0>(result).type == descriptor_type::regular_file);
   REQUIRE(std::get<0>(result).size == 4);

   ::unlink(tmpfile.c_str());
}

TEST_CASE("WasiFilesystemHost invalid handle returns bad_descriptor", "[wasi][filesystem]")
{
   WasiIoHost          io;
   WasiFilesystemHost  fs(io);

   auto result = fs.descriptor_stat_fn(
       psio::borrow<descriptor>{psizam::handle_table<descriptor_data, 256>::invalid_handle});
   REQUIRE(result.index() == 1);
   REQUIRE(std::get<1>(result) == filesystem_error_code::bad_descriptor);
}
