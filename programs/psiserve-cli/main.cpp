#include <psiserve/runtime.hpp>

#include <boost/program_options.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace po = boost::program_options;

int main(int argc, char** argv)
{
   po::options_description desc("psiserve - WASM TCP server");
   desc.add_options()
      ("help,h", "Show help")
      ("port,p", po::value<uint16_t>()->default_value(8080), "Listen port")
      ("webroot,w", po::value<std::string>(), "Directory to serve files from (fd 1)")
      ("wasm",   po::value<std::string>(), "WASM module file");

   po::positional_options_description pos;
   pos.add("wasm", 1);

   po::variables_map vm;
   try
   {
      po::store(po::command_line_parser(argc, argv)
                   .options(desc)
                   .positional(pos)
                   .run(),
                vm);
      po::notify(vm);
   }
   catch (const po::error& e)
   {
      std::cerr << "Error: " << e.what() << "\n\n" << desc << "\n";
      return 1;
   }

   if (vm.count("help") || !vm.count("wasm"))
   {
      std::cout << "Usage: psiserve [options] <wasm-file>\n\n" << desc << "\n";
      return vm.count("help") ? 0 : 1;
   }

   psiserve::Runtime::Config cfg;
   cfg.wasm_path = vm["wasm"].as<std::string>();
   cfg.port      = psiserve::Port{vm["port"].as<uint16_t>()};
   if (vm.count("webroot"))
      cfg.webroot = vm["webroot"].as<std::string>();

   try
   {
      psiserve::Runtime rt(cfg);
      rt.run();
   }
   catch (const std::exception& e)
   {
      std::cerr << "Fatal: " << e.what() << "\n";
      return 1;
   }

   return 0;
}
