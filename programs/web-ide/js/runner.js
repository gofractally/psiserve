// Run a compiled .wasm via the WASI shim.
//
// Usage:
//   await runWasm(bytes, { terminal, stdinProvider, args });
//
// stdinProvider() -> string | null (returns text to feed when guest reads stdin)
// terminal is an object with .stdout(text), .stderr(text), .info(text) methods.

import { WASI, ExitError } from './wasi.js';

export async function runWasm(bytes, opts) {
   const { terminal, stdinProvider, args = ['program'] } = opts;
   terminal.info(`running .wasm (${bytes.byteLength} bytes)…\n`);
   const t0 = performance.now();

   const wasi = new WASI({
      args,
      env: ['PWD=/', 'HOME=/', 'PATH=/', 'LANG=C'],
      stdout: text => terminal.stdout(text),
      stderr: text => terminal.stderr(text),
      stdin: stdinProvider,
   });

   let instance;
   try {
      const result = await WebAssembly.instantiate(bytes, wasi.imports());
      instance = result.instance;
      wasi.bind(instance);
   } catch (e) {
      terminal.stderr(`instantiation failed: ${e.message}\n`);
      return { ok: false, exitCode: 1, elapsed_ms: performance.now() - t0 };
   }

   const startFn = instance.exports._start ?? instance.exports.main;
   if (typeof startFn !== 'function') {
      terminal.stderr(`no _start or main export\n`);
      return { ok: false, exitCode: 1, elapsed_ms: performance.now() - t0 };
   }

   try {
      startFn();
      wasi._flush();
      const elapsed = performance.now() - t0;
      terminal.info(`─ exited 0 (${elapsed.toFixed(1)}ms)\n`);
      return { ok: true, exitCode: 0, elapsed_ms: elapsed };
   } catch (e) {
      wasi._flush();
      if (e instanceof ExitError) {
         const elapsed = performance.now() - t0;
         const status = e.code === 0 ? 'info' : 'stderr';
         terminal[status](`─ exited ${e.code} (${elapsed.toFixed(1)}ms)\n`);
         return { ok: e.code === 0, exitCode: e.code, elapsed_ms: elapsed };
      }
      terminal.stderr(`runtime trap: ${e.message}\n`);
      return { ok: false, exitCode: 1, elapsed_ms: performance.now() - t0 };
   }
}
