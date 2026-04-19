// source-to-pzam: host-side driver for the deterministic C++ → .pzam toolchain.
//
// Orchestrates three stages, all producing intermediates in a shared temp dir:
//   A. clang.pzam     — C++ → .o          (runs under pzam-run inside psizam)
//   A'. wasm-ld.pzam  — .o + libs → .wasm (runs under pzam-run inside psizam)
//   B. pzam-compile   — .wasm → .pzam     (native host binary; JIT2 or LLVM AOT)
//
// All three stages are invoked as child processes of this driver. Stages A/A'
// share a single preopened /work directory (the temp dir) and a read-only
// /sysroot preopen pointing at the pinned wasi-sysroot. Stage B runs natively
// and reads the final .wasm directly.
//
// Usage:
//   source-to-pzam [options] <input.cc>... -o <out.pzam>
//
// Options:
//   -o <file>            Output .pzam path (required).
//   -O<N>                Optimization level passed to clang (default -O2).
//   --target=<triple>    WASM target triple (default wasm32-wasip1).
//   --backend=llvm|jit2  Backend for stage B (default llvm).
//   --arch=x86_64|aarch64  Native target for the .pzam (default: host arch).
//   --keep-temp          Don't delete the working dir on success.
//   --verify             After producing .pzam, recompute hashes and check manifest.
//   --manifest=<file>    Write manifest JSON to <file> (default: <out>.manifest.json).
//   --tooldir=<dir>      Override directory containing pzam-run / clang.pzam / etc.
//   --sysroot=<dir>      Override wasi-sysroot directory (C libs, crt1-command.o).
//   --cxxsysroot=<dir>   C++ sysroot (libc++, libc++abi, C++ headers). Optional.
//   --resource-dir=<dir> LLVM clang resource dir (builtin headers like stddef.h). Optional.
//   --clang-rt-dir=<dir> Directory containing libclang_rt.builtins.a. Optional.
//   -v, --verbose        Print stage invocations.
//   -h, --help           This message.
//
// Environment overrides (take precedence over compiled-in defaults, not over flags):
//   SOURCE_TO_PZAM_TOOLDIR, SOURCE_TO_PZAM_SYSROOT,
//   SOURCE_TO_PZAM_CXXSYSROOT, SOURCE_TO_PZAM_RESOURCE_DIR, SOURCE_TO_PZAM_CLANG_RT_DIR

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

#ifndef SOURCE_TO_PZAM_DEFAULT_TOOLDIR
#define SOURCE_TO_PZAM_DEFAULT_TOOLDIR ""
#endif
#ifndef SOURCE_TO_PZAM_DEFAULT_SYSROOT
#define SOURCE_TO_PZAM_DEFAULT_SYSROOT ""
#endif
#ifndef SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT
#define SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT ""
#endif
#ifndef SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR
#define SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR ""
#endif
#ifndef SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR
#define SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR ""
#endif

struct Config {
   std::vector<std::string> inputs;
   std::string output;
   std::string opt_flag = "-O2";
   std::string target = "wasm32-wasip1";
   // jit2 is the default because it works in every build config; llvm AOT
   // requires -DPSIZAM_ENABLE_LLVM=ON at psizam build time.
   std::string backend = "jit2";
   std::string arch;  // auto-detected below if empty
   std::string tooldir = SOURCE_TO_PZAM_DEFAULT_TOOLDIR;
   std::string sysroot = SOURCE_TO_PZAM_DEFAULT_SYSROOT;
   std::string cxxsysroot = SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT;
   std::string resource_dir = SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR;
   std::string clang_rt_dir = SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR;
   std::string manifest;
   // Runner for stages A/A' (how to execute the clang & wasm-ld modules).
   //   psizam-wasi — load the .wasm under the JIT backend (works today).
   //   pzam-run    — load the pre-compiled .pzam under the AOT backend.
   // AOT path is currently blocked by a jit2-backend codegen issue with
   // LLVM-heavy C++ static initializers (clang crashes at startup in
   // cl::opt registration). Default to psizam-wasi until fixed.
   std::string runner = "psizam-wasi";
   bool keep_temp = false;
   bool verify = false;
   bool verbose = false;
};

void usage(const char* prog) {
   std::cerr
      << "Usage: " << prog << " [options] <input.cc>... -o <out.pzam>\n\n"
      << "Options:\n"
      << "  -o <file>              Output .pzam path (required)\n"
      << "  -O<N>                  clang optimization level (default -O2)\n"
      << "  --target=<triple>      WASM triple (default wasm32-wasip1)\n"
      << "  --backend=llvm|jit2    Stage B backend (default jit2; llvm requires -DPSIZAM_ENABLE_LLVM=ON)\n"
      << "  --runner=psizam-wasi|pzam-run  Stage A/A' executor (default psizam-wasi)\n"
      << "  --arch=x86_64|aarch64  Native arch for .pzam (default: host)\n"
      << "  --tooldir=<dir>        Directory with the runner, pzam-compile, clang.(wasm|pzam), wasm-ld.(wasm|pzam)\n"
      << "  --sysroot=<dir>        wasi-sysroot (C libs, crt1-command.o)\n"
      << "  --cxxsysroot=<dir>     C++ sysroot (libc++, libc++abi, C++ headers) [optional]\n"
      << "  --resource-dir=<dir>   LLVM clang resource dir (builtin headers) [optional]\n"
      << "  --clang-rt-dir=<dir>   Dir containing libclang_rt.builtins.a [optional]\n"
      << "  --manifest=<file>      Manifest path (default <out>.manifest.json)\n"
      << "  --keep-temp            Keep working dir on success\n"
      << "  --verify               Recompute hashes after build\n"
      << "  -v, --verbose          Print stage invocations\n"
      << "  -h, --help             This message\n";
}

[[noreturn]] void die(const std::string& msg) {
   std::cerr << "source-to-pzam: " << msg << "\n";
   std::exit(1);
}

bool starts_with(const std::string& s, std::string_view p) {
   return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

std::string basename_no_ext(const std::string& path) {
   auto slash = path.find_last_of('/');
   std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
   auto dot = name.find_last_of('.');
   if (dot != std::string::npos) name = name.substr(0, dot);
   return name;
}

bool file_exists(const std::string& p) {
   struct stat st;
   return ::stat(p.c_str(), &st) == 0;
}

// Minimal SHA-256 (public domain–style). Deterministic output only used for
// manifest hashing; not security-critical.
struct Sha256 {
   uint32_t s[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
   uint8_t buf[64] = {};
   uint64_t len = 0;
   size_t nbuf = 0;

   static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32-n)); }

   void compress(const uint8_t* p) {
      static const uint32_t K[64] = {
         0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
         0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
         0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
         0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
         0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
         0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
         0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
         0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
      uint32_t w[64];
      for (int i = 0; i < 16; i++)
         w[i] = (uint32_t(p[4*i])<<24)|(uint32_t(p[4*i+1])<<16)|(uint32_t(p[4*i+2])<<8)|uint32_t(p[4*i+3]);
      for (int i = 16; i < 64; i++) {
         uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
         uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >>10);
         w[i] = w[i-16] + s0 + w[i-7] + s1;
      }
      uint32_t a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7];
      for (int i = 0; i < 64; i++) {
         uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
         uint32_t ch = (e & f) ^ (~e & g);
         uint32_t t1 = h + S1 + ch + K[i] + w[i];
         uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
         uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
         uint32_t t2 = S0 + mj;
         h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
      }
      s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
   }

   void update(const uint8_t* data, size_t n) {
      len += n;
      while (n) {
         size_t take = std::min<size_t>(n, 64 - nbuf);
         std::memcpy(buf + nbuf, data, take);
         nbuf += take; data += take; n -= take;
         if (nbuf == 64) { compress(buf); nbuf = 0; }
      }
   }

   std::string finish() {
      uint64_t bits = len * 8;
      uint8_t pad = 0x80;
      update(&pad, 1);
      uint8_t z = 0;
      while (nbuf != 56) update(&z, 1);
      uint8_t lenbuf[8];
      for (int i = 0; i < 8; i++) lenbuf[7-i] = uint8_t(bits >> (8*i));
      update(lenbuf, 8);
      char out[65];
      for (int i = 0; i < 8; i++)
         std::snprintf(out + i*8, 9, "%08x", s[i]);
      out[64] = 0;
      return std::string(out);
   }
};

std::string sha256_file(const std::string& path) {
   std::ifstream f(path, std::ios::binary);
   if (!f) die("cannot hash: " + path);
   Sha256 h;
   std::vector<uint8_t> buf(64 * 1024);
   while (f) {
      f.read(reinterpret_cast<char*>(buf.data()), buf.size());
      auto n = f.gcount();
      if (n > 0) h.update(buf.data(), size_t(n));
   }
   return h.finish();
}

std::string detect_arch() {
#if defined(__x86_64__)
   return "x86_64";
#elif defined(__aarch64__)
   return "aarch64";
#else
   return "";
#endif
}

int run_child(const std::vector<std::string>& argv, bool verbose) {
   if (verbose) {
      std::cerr << "[source-to-pzam] exec:";
      for (auto& a : argv) std::cerr << ' ' << a;
      std::cerr << "\n";
   }
   std::vector<char*> cargv;
   cargv.reserve(argv.size() + 1);
   for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
   cargv.push_back(nullptr);

   pid_t pid = ::fork();
   if (pid < 0) die(std::string("fork: ") + std::strerror(errno));
   if (pid == 0) {
      ::execv(cargv[0], cargv.data());
      std::fprintf(stderr, "source-to-pzam: exec %s: %s\n", cargv[0], std::strerror(errno));
      std::_Exit(127);
   }
   int status = 0;
   if (::waitpid(pid, &status, 0) < 0) die(std::string("waitpid: ") + std::strerror(errno));
   if (WIFEXITED(status)) return WEXITSTATUS(status);
   if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
   return 1;
}

void remove_tree(const std::string& path) {
   // Simple recursive rm via system(). Only called on paths we created under
   // mkdtemp, so the blast radius is bounded.
   std::string cmd = "rm -rf '";
   for (char c : path) {
      if (c == '\'') cmd += "'\\''";
      else cmd.push_back(c);
   }
   cmd += "'";
   ::system(cmd.c_str());
}

std::string make_tempdir() {
   const char* tmp = std::getenv("TMPDIR");
   std::string tpl = (tmp && *tmp) ? tmp : "/tmp";
   if (tpl.back() == '/') tpl.pop_back();
   tpl += "/source-to-pzam.XXXXXX";
   std::vector<char> buf(tpl.begin(), tpl.end());
   buf.push_back('\0');
   if (!::mkdtemp(buf.data())) die(std::string("mkdtemp: ") + std::strerror(errno));
   return std::string(buf.data());
}

Config parse_args(int argc, char** argv) {
   Config cfg;

   if (const char* e = std::getenv("SOURCE_TO_PZAM_TOOLDIR"); e && *e) cfg.tooldir = e;
   if (const char* e = std::getenv("SOURCE_TO_PZAM_SYSROOT"); e && *e) cfg.sysroot = e;
   if (const char* e = std::getenv("SOURCE_TO_PZAM_CXXSYSROOT"); e && *e) cfg.cxxsysroot = e;
   if (const char* e = std::getenv("SOURCE_TO_PZAM_RESOURCE_DIR"); e && *e) cfg.resource_dir = e;
   if (const char* e = std::getenv("SOURCE_TO_PZAM_CLANG_RT_DIR"); e && *e) cfg.clang_rt_dir = e;

   for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
      else if (a == "-v" || a == "--verbose") cfg.verbose = true;
      else if (a == "--keep-temp") cfg.keep_temp = true;
      else if (a == "--verify") cfg.verify = true;
      else if (a == "-o") {
         if (++i >= argc) die("-o requires an argument");
         cfg.output = argv[i];
      }
      else if (starts_with(a, "-o"))            cfg.output   = a.substr(2);
      else if (starts_with(a, "-O"))            cfg.opt_flag = a;
      else if (starts_with(a, "--target="))     cfg.target   = a.substr(9);
      else if (starts_with(a, "--backend="))    cfg.backend  = a.substr(10);
      else if (starts_with(a, "--runner="))     cfg.runner   = a.substr(9);
      else if (starts_with(a, "--arch="))       cfg.arch     = a.substr(7);
      else if (starts_with(a, "--tooldir="))    cfg.tooldir  = a.substr(10);
      else if (starts_with(a, "--sysroot="))    cfg.sysroot  = a.substr(10);
      else if (starts_with(a, "--cxxsysroot=")) cfg.cxxsysroot  = a.substr(13);
      else if (starts_with(a, "--resource-dir=")) cfg.resource_dir = a.substr(15);
      else if (starts_with(a, "--clang-rt-dir=")) cfg.clang_rt_dir = a.substr(15);
      else if (starts_with(a, "--manifest="))   cfg.manifest = a.substr(11);
      else if (!a.empty() && a[0] == '-')       die("unknown option: " + a);
      else                                      cfg.inputs.push_back(a);
   }

   if (cfg.inputs.empty()) die("no input files");
   if (cfg.output.empty()) die("missing -o <output.pzam>");
   if (cfg.tooldir.empty()) die("no tooldir (set --tooldir or SOURCE_TO_PZAM_TOOLDIR)");
   if (cfg.sysroot.empty()) die("no sysroot (set --sysroot or SOURCE_TO_PZAM_SYSROOT)");
   if (!cfg.cxxsysroot.empty() && !file_exists(cfg.cxxsysroot))
      die("cxxsysroot not found: " + cfg.cxxsysroot);
   if (!cfg.resource_dir.empty() && !file_exists(cfg.resource_dir))
      die("resource-dir not found: " + cfg.resource_dir);
   if (!cfg.clang_rt_dir.empty() && !file_exists(cfg.clang_rt_dir))
      die("clang-rt-dir not found: " + cfg.clang_rt_dir);
   if (cfg.arch.empty()) {
      cfg.arch = detect_arch();
      if (cfg.arch.empty()) die("unsupported host arch; pass --arch=x86_64|aarch64");
   }
   if (cfg.backend != "llvm" && cfg.backend != "jit2") die("--backend must be llvm or jit2");
   if (cfg.runner != "psizam-wasi" && cfg.runner != "pzam-run")
      die("--runner must be psizam-wasi or pzam-run");
   if (cfg.manifest.empty()) cfg.manifest = cfg.output + ".manifest.json";
   return cfg;
}

struct StageHash {
   std::string label;
   std::string path;
   std::string sha256;
};

void write_manifest(const Config& cfg, const std::vector<StageHash>& items) {
   std::ofstream f(cfg.manifest);
   if (!f) die("cannot write manifest: " + cfg.manifest);
   f << "{\n";
   f << "  \"tool\": \"source-to-pzam\",\n";
   f << "  \"target\": \"" << cfg.target << "\",\n";
   f << "  \"arch\": \"" << cfg.arch << "\",\n";
   f << "  \"backend\": \"" << cfg.backend << "\",\n";
   f << "  \"runner\": \"" << cfg.runner << "\",\n";
   f << "  \"opt\": \"" << cfg.opt_flag << "\",\n";
   f << "  \"artifacts\": [\n";
   for (size_t i = 0; i < items.size(); i++) {
      f << "    { \"label\": \"" << items[i].label
        << "\", \"path\": \"" << items[i].path
        << "\", \"sha256\": \"" << items[i].sha256 << "\" }";
      if (i + 1 < items.size()) f << ",";
      f << "\n";
   }
   f << "  ]\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
   Config cfg = parse_args(argc, argv);

   // Runner-dependent tool paths. psizam-wasi loads .wasm directly under the
   // JIT backend; pzam-run loads pre-compiled .pzam under the AOT backend.
   const bool use_wasi = (cfg.runner == "psizam-wasi");
   const std::string runner_bin  = cfg.tooldir + (use_wasi ? "/psizam-wasi" : "/pzam-run");
   const std::string pzam_compile = cfg.tooldir + "/pzam-compile";
   const std::string clang_mod   = cfg.tooldir + (use_wasi ? "/clang.wasm"   : "/clang.pzam");
   const std::string wasm_ld_mod = cfg.tooldir + (use_wasi ? "/wasm-ld.wasm" : "/wasm-ld.pzam");

   for (auto& f : {runner_bin, pzam_compile, clang_mod, wasm_ld_mod})
      if (!file_exists(f)) die("missing tool: " + f);
   if (!file_exists(cfg.sysroot)) die("missing sysroot: " + cfg.sysroot);

   std::string workdir = make_tempdir();
   if (cfg.verbose) std::cerr << "[source-to-pzam] workdir: " << workdir << "\n";

   // Guest paths (inside runner preopens). Clang and wasm-ld see:
   //   /work         — the tempdir (rw)
   //   /sysroot      — wasi-sysroot (C libs, crt1)
   //   /cxxsysroot   — optional: C++ headers + libc++/libc++abi
   //   /rtclang      — optional: LLVM clang resource dir (builtin headers)
   //   /rtbuiltins   — optional: dir holding libclang_rt.builtins.a
   const std::string guest_work        = "/work";
   const std::string guest_sysroot     = "/sysroot";
   const std::string guest_cxxsysroot  = "/cxxsysroot";
   const std::string guest_rtclang     = "/rtclang";
   const std::string guest_rtbuiltins  = "/rtbuiltins";

   auto add_preopens = [&](std::vector<std::string>& av) {
      av.push_back("--dir=" + guest_work    + ":" + workdir);
      av.push_back("--dir=" + guest_sysroot + ":" + cfg.sysroot);
      if (!cfg.cxxsysroot.empty())
         av.push_back("--dir=" + guest_cxxsysroot + ":" + cfg.cxxsysroot);
      if (!cfg.resource_dir.empty())
         av.push_back("--dir=" + guest_rtclang + ":" + cfg.resource_dir);
      if (!cfg.clang_rt_dir.empty())
         av.push_back("--dir=" + guest_rtbuiltins + ":" + cfg.clang_rt_dir);
   };

   // Stage A: compile each input to a .o in /work.
   std::vector<std::string> objs;
   objs.reserve(cfg.inputs.size());

   for (const auto& src : cfg.inputs) {
      if (!file_exists(src)) die("input not found: " + src);

      // Copy source into /work so the guest can see it by a stable guest path.
      // Close the stream explicitly before fork(), otherwise buffered output
      // may not reach disk and the child sees an empty source file.
      std::string src_guest_name = basename_no_ext(src) + ".cc";
      std::string src_host_path  = workdir + "/" + src_guest_name;
      {
         std::ifstream in(src, std::ios::binary);
         std::ofstream out(src_host_path, std::ios::binary);
         out << in.rdbuf();
         if (!out) die("cannot copy source to workdir");
      }

      std::string obj_name = basename_no_ext(src) + ".o";
      std::string obj_host = workdir + "/" + obj_name;

      // Runner-specific invocation shape:
      //   psizam-wasi [--dir=...] [--backend=jit] <module.wasm> <guest-args>...
      //   pzam-run    [--dir=...] <module.pzam> -- <guest-args>...
      std::vector<std::string> ax = { runner_bin };
      add_preopens(ax);
      if (use_wasi) ax.push_back("--backend=jit");
      ax.push_back(clang_mod);
      if (!use_wasi) ax.push_back("--");
      // argv[0] is auto-populated by the runner from the module filename
      // (clang.wasm / clang.pzam). Don't prepend an extra "clang".
      std::vector<std::string> clang_args = {
         std::string("--target=") + cfg.target,
         "--sysroot=" + guest_sysroot,
      };
      if (cfg.verbose) clang_args.push_back("-v");
      if (!cfg.resource_dir.empty()) {
         clang_args.push_back("-resource-dir");
         clang_args.push_back(guest_rtclang);
      }
      if (!cfg.cxxsysroot.empty()) {
         clang_args.push_back("-isystem");
         clang_args.push_back(guest_cxxsysroot + "/include/" + cfg.target + "/c++/v1");
         clang_args.push_back("-isystem");
         clang_args.push_back(guest_cxxsysroot + "/include/c++/v1");
      }
      clang_args.push_back(cfg.opt_flag);
      clang_args.push_back("-fno-exceptions");
      clang_args.push_back("-fno-rtti");
      clang_args.push_back("-c");
      clang_args.push_back(guest_work + "/" + src_guest_name);
      clang_args.push_back("-o");
      clang_args.push_back(guest_work + "/" + obj_name);
      ax.insert(ax.end(), clang_args.begin(), clang_args.end());

      int rc = run_child(ax, cfg.verbose);
      if (rc != 0) { if (!cfg.keep_temp) remove_tree(workdir); die("clang failed (exit " + std::to_string(rc) + ")"); }
      if (!file_exists(obj_host)) { if (!cfg.keep_temp) remove_tree(workdir); die("clang produced no .o for " + src); }
      objs.push_back(obj_name);
   }

   // Stage A': link .o files into hello.wasm.
   const std::string wasm_name = "linked.wasm";
   const std::string wasm_host = workdir + "/" + wasm_name;

   std::vector<std::string> ld = { runner_bin };
   add_preopens(ld);
   if (use_wasi) ld.push_back("--backend=jit");
   ld.push_back(wasm_ld_mod);
   if (!use_wasi) ld.push_back("--");
   // lld dispatches by argv[0]; the runner sets argv[0] to the module
   // filename (wasm-ld.wasm / wasm-ld.pzam), so no extra "wasm-ld" needed.
   ld.push_back("-L" + guest_sysroot + "/lib/" + cfg.target);
   if (!cfg.cxxsysroot.empty()) {
      ld.push_back("-L" + guest_cxxsysroot + "/lib/" + cfg.target);
      // Homebrew's wasi-runtimes layout also uses the unknown vendor triple.
      ld.push_back("-L" + guest_cxxsysroot + "/lib/wasm32-unknown-wasip1");
   }
   if (!cfg.clang_rt_dir.empty()) {
      ld.push_back("-L" + guest_rtbuiltins);
      ld.push_back("-L" + guest_rtbuiltins + "/" + cfg.target);
      ld.push_back("-L" + guest_rtbuiltins + "/wasm32-unknown-wasip1");
   }
   ld.push_back(guest_sysroot + "/lib/" + cfg.target + "/crt1-command.o");
   for (auto& o : objs) ld.push_back(guest_work + "/" + o);
   for (auto& lib : {"-lc", "-lc++", "-lc++abi", "-lclang_rt.builtins"})
      ld.push_back(lib);
   ld.insert(ld.end(), {"-o", guest_work + "/" + wasm_name});
   // Match the canonical clang+wasm-ld invocation's feature preservation.
   ld.push_back("--keep-section=target_features");

   int rc = run_child(ld, cfg.verbose);
   if (rc != 0) { if (!cfg.keep_temp) remove_tree(workdir); die("wasm-ld.pzam failed (exit " + std::to_string(rc) + ")"); }
   if (!file_exists(wasm_host)) { if (!cfg.keep_temp) remove_tree(workdir); die("wasm-ld produced no .wasm"); }

   // Stage B: pzam-compile the final .wasm to .pzam (native host binary).
   std::vector<std::string> pc = {
      pzam_compile,
      "--target=" + cfg.arch,
      "--backend=" + cfg.backend,
      "-o", cfg.output,
      wasm_host,
   };
   rc = run_child(pc, cfg.verbose);
   if (rc != 0) { if (!cfg.keep_temp) remove_tree(workdir); die("pzam-compile failed (exit " + std::to_string(rc) + ")"); }
   if (!file_exists(cfg.output)) { if (!cfg.keep_temp) remove_tree(workdir); die("pzam-compile produced no output"); }

   // Manifest.
   std::vector<StageHash> hashes;
   hashes.push_back({use_wasi ? "clang.wasm"   : "clang.pzam",   clang_mod,   sha256_file(clang_mod)});
   hashes.push_back({use_wasi ? "wasm-ld.wasm" : "wasm-ld.pzam", wasm_ld_mod, sha256_file(wasm_ld_mod)});
   hashes.push_back({"pzam-compile", pzam_compile, sha256_file(pzam_compile)});
   for (const auto& src : cfg.inputs)
      hashes.push_back({"input", src, sha256_file(src)});
   hashes.push_back({"linked.wasm", wasm_host, sha256_file(wasm_host)});
   hashes.push_back({"output.pzam", cfg.output, sha256_file(cfg.output)});
   write_manifest(cfg, hashes);

   if (cfg.verify) {
      auto h = sha256_file(cfg.output);
      if (h != hashes.back().sha256) die("verify: output hash changed after manifest write");
   }

   if (!cfg.keep_temp) remove_tree(workdir);
   else std::cerr << "[source-to-pzam] kept workdir: " << workdir << "\n";

   return 0;
}
