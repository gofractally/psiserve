// In-browser C → .wasm pipeline.
//
// Architecture:
//   1. Fetch clang.wasm, wasm-ld.wasm, sysroot.tar on first use. Cached
//      via the browser's Cache API so subsequent compiles are instant.
//   2. Unpack sysroot.tar into an in-memory VFS.
//   3. Drop the user's source file into the same VFS under /work/.
//   4. Instantiate clang.wasm with a WASI shim, run with
//        clang --target=wasm32-wasip1 --sysroot=/sysroot -c /work/main.c -o /work/main.o
//   5. Instantiate wasm-ld.wasm, run with
//        wasm-ld /work/main.o -L/sysroot/lib/wasm32-wasip1 -lc
//                -o /work/main.wasm --no-entry --allow-undefined
//      (We let clang emit an object with _start / main, and linker produces
//      a wasip1 module with _start export. For simple programs this works.)
//   6. Read /work/main.wasm bytes from the VFS and return.
//
// C++ is not yet supported — the wasi-libc sysroot doesn't ship libc++ /
// libc++abi / C++ headers. Adding them means shipping the wasi-runtimes
// sysroot too (~50 MB more) and setting -I/-L properly. Follow-up.

import { vfs } from './vfs.js';
import { parseTar } from './tar.js';
import { MemFS, normalize, basename, dirname } from './fs.js';
import { WASI, ExitError } from './wasi.js';

const TOOLCHAIN_BASE = './toolchain';
const CACHE_NAME = 'wasm-ide-toolchain-v2';  // bump when sysroot.tar changes

let _toolchainCache = null;   // { clangMod, wasmLdMod, sysrootEntries }
let _inflightLoad = null;     // shared promise if multiple compiles arrive together

function formatBytes(n) {
   const mb = n / 1024 / 1024;
   return mb >= 1 ? `${mb.toFixed(1)} MiB` : `${(n / 1024).toFixed(1)} KiB`;
}

async function cachedFetch(url, terminal, label) {
   const cache = await caches.open(CACHE_NAME);
   let resp = await cache.match(url);
   if (!resp) {
      terminal.info(`downloading ${label}…\n`);
      resp = await fetch(url);
      if (!resp.ok) throw new Error(`${url} HTTP ${resp.status}`);
      // Store a fresh copy in the cache
      await cache.put(url, resp.clone());
   } else {
      terminal.info(`cached: ${label}\n`);
   }
   const bytes = new Uint8Array(await resp.arrayBuffer());
   return bytes;
}

async function loadToolchain(terminal) {
   if (_toolchainCache) return _toolchainCache;
   if (_inflightLoad) return _inflightLoad;

   _inflightLoad = (async () => {
      const clangBytes  = await cachedFetch(`${TOOLCHAIN_BASE}/clang.wasm`,   terminal, 'clang.wasm');
      terminal.info(`  clang.wasm: ${formatBytes(clangBytes.byteLength)}\n`);
      const wasmLdBytes = await cachedFetch(`${TOOLCHAIN_BASE}/wasm-ld.wasm`, terminal, 'wasm-ld.wasm');
      terminal.info(`  wasm-ld.wasm: ${formatBytes(wasmLdBytes.byteLength)}\n`);
      const sysrootBytes= await cachedFetch(`${TOOLCHAIN_BASE}/sysroot.tar`,  terminal, 'sysroot.tar');
      terminal.info(`  sysroot.tar: ${formatBytes(sysrootBytes.byteLength)}\n`);

      terminal.info('compiling clang.wasm module (one-time)…\n');
      const t0 = performance.now();
      const clangMod = await WebAssembly.compile(clangBytes);
      terminal.info(`  ${((performance.now() - t0)/1000).toFixed(1)}s\n`);

      terminal.info('compiling wasm-ld.wasm module (one-time)…\n');
      const t1 = performance.now();
      const wasmLdMod = await WebAssembly.compile(wasmLdBytes);
      terminal.info(`  ${((performance.now() - t1)/1000).toFixed(1)}s\n`);

      terminal.info('unpacking sysroot…\n');
      const sysrootEntries = parseTar(sysrootBytes.buffer);
      terminal.info(`  ${sysrootEntries.length} entries\n`);

      _toolchainCache = { clangMod, wasmLdMod, sysrootEntries };
      _inflightLoad = null;
      return _toolchainCache;
   })();

   return _inflightLoad;
}

function buildFs(sysrootEntries, userFiles) {
   const fs = new MemFS();
   // Tarball layout is { sysroot/, resource/, clang-rt/ } — mount at root.
   for (const e of sysrootEntries) {
      const path = '/' + e.path;
      if (e.type === 'dir') fs.mkdir(path);
      else if (e.type === 'file') fs.writeFile(path, e.data);
   }
   // Work directory for user source + compiler output
   fs.mkdir('/work');
   for (const [name, content] of userFiles) {
      fs.writeFile('/work/' + basename(name), content);
   }
   return fs;
}

async function runWasiTool(module, args, fs, terminal, { debug } = {}) {
   const wasi = new WASI({
      args,
      env: ['PWD=/work', 'HOME=/work', 'LANG=C'],
      fs,
      preopens: ['/', '/sysroot', '/resource', '/clang-rt', '/work'],
      stdout: s => terminal.stdout(s),
      stderr: s => terminal.stderr(s),
      stdin:  () => null,
      debug,
   });

   let instance;
   try {
      instance = await WebAssembly.instantiate(module, wasi.imports());
   } catch (e) {
      terminal.stderr(`\ninstantiate ${args[0]} failed: ${e.message}\n`);
      return -1;
   }
   wasi.bind(instance);
   try {
      instance.exports._start();
      wasi._flush();
      return 0;
   } catch (e) {
      wasi._flush();
      if (e instanceof ExitError) return e.code;
      // Runtime trap (e.g. memory OOB, unreachable). Surface it — silently
      // swallowing leaves the UI reporting "compiled" while actually nothing
      // was produced, which is extra confusing.
      terminal.stderr(`\n${args[0]} trap: ${e.message}\n`);
      return -1;
   }
}

export async function compile(filename, terminal) {
   const source = vfs.read(filename);
   if (source == null) { terminal.stderr(`file not found: ${filename}\n`); return null; }
   const sourceBase = basename(filename);
   const isCpp = sourceBase.endsWith('.cpp') || sourceBase.endsWith('.cc') || sourceBase.endsWith('.cxx');

   terminal.info(`compile ${filename} (${source.length} bytes, ${isCpp ? 'C++' : 'C'})…\n`);

   let tc;
   try {
      tc = await loadToolchain(terminal);
   } catch (e) {
      terminal.stderr(`toolchain load failed: ${e.message}\n`);
      terminal.info(
         '\nTo enable Compile, stage clang.wasm + wasm-ld.wasm + sysroot.tar\n' +
         'under programs/web-ide/toolchain/ next to this page.\n');
      return null;
   }

   const fs = buildFs(tc.sysrootEntries, [[sourceBase, source]]);

   // Step 1: clang compile to /work/main.o. --sysroot + -resource-dir are
   // enough for C — clang's wasm32-wasip1 driver adds
   //   -internal-isystem /resource/include
   //   -internal-isystem /sysroot/include/wasm32-wasip1
   //   -internal-isystem /sysroot/include
   // automatically.
   //
   // For C++, we additionally need the libc++ header directory as an
   // -isystem and select -stdlib=libc++. Exceptions are disabled since the
   // wasi-runtimes libc++abi we ship doesn't include the exception-throwing
   // __cxa_* hooks clang generates with -fexceptions.
   terminal.info(`clang -c /work/${sourceBase} -o /work/main.o\n`);
   const clangArgs = [
      isCpp ? 'clang++' : 'clang',
      '--target=wasm32-wasip1',
      `--sysroot=/sysroot`,
      `-resource-dir=/resource`,
      '-O2',
      ...(isCpp
         ? [
              '-stdlib=libc++',
              '-fno-exceptions',
              '-isystem', '/sysroot/include/wasm32-wasip1/c++/v1',
           ]
         : []),
      '-c',
      `/work/${sourceBase}`,
      '-o', '/work/main.o',
   ];
   let rc = await runWasiTool(tc.clangMod, clangArgs, fs, terminal);
   if (rc !== 0) {
      terminal.stderr(`clang exited ${rc}\n`);
      return null;
   }
   if (!fs.exists('/work/main.o')) {
      terminal.stderr('clang succeeded but /work/main.o missing\n');
      return null;
   }
   terminal.ok(`  → /work/main.o (${formatBytes(fs.readFile('/work/main.o').byteLength)})\n`);

   // Step 2: wasm-ld link
   terminal.info(`wasm-ld /work/main.o → /work/main.wasm\n`);
   const linkArgs = [
      'wasm-ld',
      '/sysroot/lib/wasm32-wasip1/crt1-command.o',
      '/work/main.o',
      '-L/sysroot/lib/wasm32-wasip1',
      '-L/clang-rt',
      ...(isCpp ? ['-lc++', '-lc++abi'] : []),
      '-lc',
      '-lclang_rt.builtins',
      '-o', '/work/main.wasm',
   ];
   rc = await runWasiTool(tc.wasmLdMod, linkArgs, fs, terminal);
   if (rc !== 0) {
      terminal.stderr(`wasm-ld exited ${rc}\n`);
      return null;
   }

   const outBytes = fs.readFile('/work/main.wasm');
   if (!outBytes) { terminal.stderr('wasm-ld succeeded but /work/main.wasm missing\n'); return null; }
   terminal.ok(`  → /work/main.wasm (${formatBytes(outBytes.byteLength)})\n`);

   // Return a standalone ArrayBuffer (detach from MemFS view)
   const buf = new Uint8Array(outBytes.byteLength);
   buf.set(outBytes);
   return { bytes: buf.buffer, name: filename.replace(/\.[^.]+$/, '') + '.wasm' };
}
