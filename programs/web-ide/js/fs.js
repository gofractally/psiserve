// Tiny virtual filesystem for the in-browser WASI shim.
//
// Tree is a plain JS Map keyed by absolute path. Each value is either:
//   { kind: 'dir', children: Set<string> }
//   { kind: 'file', data: Uint8Array, mtime_ns: bigint }
//
// Paths are always absolute, normalized, no trailing slash (except '/').
// We merge multiple sources (tar, user VFS) into a single namespace.
//
// The shim will treat the VFS as a single filesystem; WASI preopens map
// "preopen dir fds" to subtrees. pathOpen("/some/path") from any fd just
// resolves against the absolute path.

export class MemFS {
   constructor() {
      this.entries = new Map();
      this._mkdirp('/');
   }

   _mkdirp(absPath) {
      const parts = absPath === '/' ? [''] : absPath.split('/');
      let cur = '';
      for (let i = 0; i < parts.length; i++) {
         const seg = parts[i];
         cur = (i === 0) ? '/' : (cur === '/' ? '/' + seg : cur + '/' + seg);
         if (!this.entries.has(cur)) {
            this.entries.set(cur, { kind: 'dir', children: new Set() });
         }
         if (i > 0) {
            const parent = cur.lastIndexOf('/') === 0 ? '/' : cur.slice(0, cur.lastIndexOf('/'));
            const parentEntry = this.entries.get(parent);
            if (parentEntry && parentEntry.kind === 'dir') parentEntry.children.add(seg);
         }
      }
   }

   exists(path) { return this.entries.has(normalize(path)); }
   get(path)    { return this.entries.get(normalize(path)); }

   writeFile(path, data) {
      path = normalize(path);
      const parent = dirname(path);
      this._mkdirp(parent);
      const bytes = data instanceof Uint8Array ? data : new TextEncoder().encode(data);
      this.entries.set(path, {
         kind: 'file',
         data: bytes,
         mtime_ns: BigInt(Date.now()) * 1_000_000n,
      });
      this.entries.get(parent).children.add(basename(path));
   }

   mkdir(path) {
      this._mkdirp(normalize(path));
   }

   unlink(path) {
      path = normalize(path);
      if (!this.entries.has(path)) return false;
      this.entries.delete(path);
      const parent = dirname(path);
      this.entries.get(parent)?.children.delete(basename(path));
      return true;
   }

   rename(from, to) {
      from = normalize(from); to = normalize(to);
      const entry = this.entries.get(from);
      if (!entry) return false;
      this.entries.delete(from);
      this.entries.get(dirname(from))?.children.delete(basename(from));
      this.entries.set(to, entry);
      this._mkdirp(dirname(to));
      this.entries.get(dirname(to)).children.add(basename(to));
      return true;
   }

   readFile(path) {
      const e = this.entries.get(normalize(path));
      return e?.kind === 'file' ? e.data : null;
   }

   // For debugging: list all paths under a prefix
   listUnder(prefix) {
      prefix = normalize(prefix);
      const out = [];
      for (const p of this.entries.keys()) {
         if (p === prefix || p.startsWith(prefix === '/' ? '/' : prefix + '/')) {
            out.push(p);
         }
      }
      return out.sort();
   }
}

export function normalize(path) {
   if (!path) return '/';
   if (!path.startsWith('/')) path = '/' + path;
   // Collapse '//' and resolve '.' / '..'
   const parts = path.split('/');
   const stack = [];
   for (const p of parts) {
      if (p === '' || p === '.') continue;
      if (p === '..') { stack.pop(); continue; }
      stack.push(p);
   }
   return '/' + stack.join('/');
}

export function dirname(path) {
   path = normalize(path);
   if (path === '/') return '/';
   const i = path.lastIndexOf('/');
   return i <= 0 ? '/' : path.slice(0, i);
}

export function basename(path) {
   path = normalize(path);
   const i = path.lastIndexOf('/');
   return path.slice(i + 1);
}
