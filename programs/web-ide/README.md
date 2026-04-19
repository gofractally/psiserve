# wasm-ide

Browser-native editor + runner for C targeting WebAssembly. Everything is
static HTML/CSS/JS — no build step, no backend.

## Features

- **File explorer** — create / rename / delete files; contents persist in
  localStorage (`wasm-ide:vfs:v1`).
- **Monaco editor** with C/C++/Markdown/JSON/JS syntax, loaded from jsDelivr.
- **Compile** — real in-browser `clang → wasm-ld → .wasm` using the
  toolchain staged under `./toolchain/`.
- **Run** a `.wasm` under a WASI preview1 sandbox. stdout/stderr stream to
  the terminal; stdin input box sends lines when the guest reads.
- **Download .wasm** — save whatever's currently compiled or loaded.
- **Load .wasm** — upload a prebuilt `.wasm` to run without compiling.

## Quick start

```sh
cd programs/web-ide
python3 -m http.server 8080
# open http://localhost:8080
```

Edit `hello.cc` (or create a new `.c` file), hit **Compile** (`Cmd/Ctrl+B`),
then **Run** (`Cmd/Ctrl+Enter`). First compile downloads ~96 MB of toolchain
artifacts (cached via the browser Cache API; subsequent compiles are instant).

## Language support

**C:** fully supported. The bundled sysroot is `wasi-libc` — `stdio`,
`string`, `stdlib`, `math`, and friends all work.

**C++:** not yet. Would need to bundle `libc++` / `libc++abi` and the C++
header set (`<cstdio>`, `<string>`, STL). Rename your file to `.c` for now.

## Toolchain layout

`./toolchain/` holds:

| File | Size | Purpose |
|---|---:|---|
| `clang.wasm` | 43 MiB | wasm32-wasip1 clang 22 from `cmake/llvm-wasi/` |
| `wasm-ld.wasm` | 24 MiB | wasm32-wasip1 linker from `cmake/llvm-wasi/` |
| `sysroot.tar` | 28 MiB | `sysroot/` (wasi-libc) + `resource/` (clang builtin headers) + `clang-rt/` (libclang_rt.builtins-wasm32) |

To refresh the toolchain from the psiserve build tree:

```sh
TC=programs/web-ide/toolchain
mkdir -p $TC
cp build/llvm-wasi/install/bin/clang.wasm   $TC/
cp build/llvm-wasi/install/bin/wasm-ld.wasm $TC/

STAGING=$(mktemp -d)
mkdir -p $STAGING/sysroot $STAGING/resource $STAGING/clang-rt
( cd "$(brew --prefix wasi-libc)/share/wasi-sysroot" && tar -cf - . ) \
   | ( cd $STAGING/sysroot && tar -xf - )
cp -R "$(brew --prefix llvm)/lib/clang/22/include" $STAGING/resource/
cp "$(brew --prefix wasi-runtimes)/share/wasi-runtimes/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a" \
   $STAGING/clang-rt/
( cd $STAGING && tar -cf - . ) > $TC/sysroot.tar
```

## Architecture

- `js/vfs.js` — in-memory workspace VFS (Map + localStorage).
- `js/tar.js` — POSIX ustar reader.
- `js/fs.js` — virtual filesystem used by the WASI shim.
- `js/wasi.js` — WASI preview1 shim: args/environ, stdio, fd ops on real
  files, `path_open/filestat_get/readdir/unlink/rename/create_directory`,
  `fd_pread/pwrite/seek/tell/filestat_set_size`, clock_time, random, and
  proc_exit (as `ExitError`). Unique inodes per path — clang dedupes include
  dirs by `(dev, ino)`, so constant inodes cause all header paths to collapse
  into one.
- `js/runner.js` — instantiates a `.wasm` and calls `_start` with the shim.
- `js/compiler.js` — orchestrates `clang -c` then `wasm-ld`:
  - clang: `clang --target=wasm32-wasip1 --sysroot=/sysroot -resource-dir=/resource -O2 -c <src> -o /work/main.o`
  - wasm-ld: `wasm-ld /sysroot/lib/wasm32-wasip1/crt1-command.o /work/main.o -L/sysroot/lib/wasm32-wasip1 -L/clang-rt -lc -lclang_rt.builtins -o /work/main.wasm`
- `js/app.js` — UI glue (toolbar, file list, editor, terminal).

Fetches of the toolchain go through `caches.open('wasm-ide-toolchain-v1')`
so the 96 MB first-load becomes free afterwards.

## Limits

- Host process memory is per-tab; a real Clang invocation uses ~500 MB –
  1 GB of browser memory. On a 4 GB tab it's fine for single files; deeply
  recursive templates might OOM.
- `fd_readdir`, `path_symlink`, `path_link`, `poll_oneoff`, and sockets are
  stubbed (`E.NOSYS`) — good enough for compilation, not for network
  programs.
- Everything lives in one thread — no pthreads support.
