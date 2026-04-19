// Simple in-memory virtual filesystem, persisted to localStorage.
//
// API:
//   vfs.list()                -> [{path, size}]
//   vfs.read(path)            -> string | null
//   vfs.write(path, content)  -> void
//   vfs.rename(from, to)      -> bool
//   vfs.remove(path)          -> void
//   vfs.onChange(cb)          -> subscribe to any mutation
//
// Files are keyed by forward-slash path; no directories — flat namespace is
// enough for a single-translation-unit IDE. Contents are UTF-8 strings.

const KEY = 'wasm-ide:vfs:v1';

function loadFromStorage() {
   try {
      const raw = localStorage.getItem(KEY);
      if (!raw) return null;
      const obj = JSON.parse(raw);
      if (typeof obj !== 'object' || obj === null) return null;
      return obj;
   } catch {
      return null;
   }
}

function saveToStorage(files) {
   try {
      localStorage.setItem(KEY, JSON.stringify(files));
   } catch (e) {
      console.warn('vfs: failed to persist', e);
   }
}

const DEFAULT_FILES = {
   'hello.cc': `// Hello, WebAssembly!
// Compile with the Compile button, then Run.
// stdout and stderr are routed to the terminal below.

#include <cstdio>
#include <string>

int main() {
    std::string greeting = "Hello, WebAssembly!";
    std::printf("%s\\n", greeting.c_str());

    // Simple arithmetic demo
    int sum = 0;
    for (int i = 1; i <= 10; i++) sum += i;
    std::printf("sum(1..10) = %d\\n", sum);

    return 0;
}
`,
   'README.md': `# wasm-ide scratch

This is your workspace. Files live in your browser's localStorage — nothing
is uploaded anywhere.

- Click **Compile** to build the current file to WebAssembly.
- Click **Run** to execute the last compiled .wasm in a WASI sandbox.
- Click **Download .wasm** to save the compiled artifact.

The toolchain (clang.wasm + wasm-ld.wasm + wasi-sysroot) must be served
alongside this page under /toolchain/. If it isn't, **Compile** will show
instructions; you can still Run a prebuilt .wasm via **Load .wasm**.
`,
};

class VFS {
   constructor() {
      this._files = loadFromStorage() ?? { ...DEFAULT_FILES };
      this._listeners = new Set();
      if (Object.keys(this._files).length === 0) {
         this._files = { ...DEFAULT_FILES };
         saveToStorage(this._files);
      }
   }

   list() {
      return Object.keys(this._files).sort().map(path => ({
         path,
         size: this._files[path].length,
      }));
   }

   read(path) {
      return path in this._files ? this._files[path] : null;
   }

   write(path, content) {
      this._files[path] = content;
      saveToStorage(this._files);
      this._notify();
   }

   rename(from, to) {
      if (!(from in this._files) || to in this._files) return false;
      this._files[to] = this._files[from];
      delete this._files[from];
      saveToStorage(this._files);
      this._notify();
      return true;
   }

   remove(path) {
      delete this._files[path];
      saveToStorage(this._files);
      this._notify();
   }

   onChange(cb) {
      this._listeners.add(cb);
      return () => this._listeners.delete(cb);
   }

   _notify() {
      for (const cb of this._listeners) {
         try { cb(); } catch (e) { console.error(e); }
      }
   }

   // Reset to defaults (wipes user files). Used only in dev.
   resetToDefaults() {
      this._files = { ...DEFAULT_FILES };
      saveToStorage(this._files);
      this._notify();
   }
}

export const vfs = new VFS();
