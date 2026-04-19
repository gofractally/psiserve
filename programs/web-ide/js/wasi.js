// WASI preview1 shim with filesystem support, backed by an in-memory VFS.
//
// This shim is enough to host clang.wasm + wasm-ld.wasm for single-TU C
// compilation. Coverage:
//
//   args_*, environ_*, clock_*, random_get, proc_exit, sched_yield, poll_oneoff (stub)
//   fd_write/fd_read on stdout/stderr/stdin (line-buffered to callbacks)
//   fd_write/fd_read/fd_seek/fd_tell/fd_close on regular files
//   fd_filestat_get, fd_fdstat_get, fd_fdstat_set_flags (no-op)
//   fd_prestat_get, fd_prestat_dir_name (preopen discovery)
//   path_open, path_filestat_get, path_unlink_file, path_remove_directory,
//     path_rename, path_create_directory, path_readlink (ENOENT)
//   fd_readdir
//
// Paths are resolved against the VFS as absolute paths — the `dirfd` passed
// to path_open is used to figure out a base directory from the fd's prestat
// name, then joined with the relative path.
//
// `stdout`/`stderr` callbacks are line-buffered (flush on newline / proc_exit).

import { MemFS, normalize, dirname } from './fs.js';

// WASI errno values
const E = {
   SUCCESS: 0, BADF: 8, EXIST: 20, INVAL: 28, ISDIR: 31,
   NOENT: 44, NOSYS: 52, PERM: 63, NOTDIR: 54, NOTEMPTY: 55,
};

// File types
const FT = { UNKNOWN: 0, BLOCK: 1, CHAR: 2, DIR: 3, REG: 4, SOCK_DGRAM: 5, SOCK_STREAM: 6, SYMLINK: 7 };

// path_open oflags
const O = {
   CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8,
};

export class ExitError extends Error {
   constructor(code) { super(`WASI exit ${code}`); this.code = code; }
}

export class WASI {
   constructor(opts = {}) {
      this.args = opts.args ?? ['wasm'];
      this.env  = opts.env  ?? [];
      this.fs   = opts.fs   ?? new MemFS();
      this.memory = null;
      this._stdoutCb = opts.stdout ?? (s => console.log(s));
      this._stderrCb = opts.stderr ?? (s => console.error(s));
      this._stdinFn  = opts.stdin  ?? (() => null);
      this._outBuf   = '';
      this._errBuf   = '';
      this._debug    = !!opts.debug;

      // fd table — keyed by integer fd
      this.fds = new Map();
      this.fds.set(0, { kind: 'stdin'  });
      this.fds.set(1, { kind: 'stdout' });
      this.fds.set(2, { kind: 'stderr' });
      this._nextFd = 5;

      // Preopens: { fd: {path}, ... }
      this._preopenFds = [];
      const preopens = opts.preopens ?? [];
      let nextPre = 3;
      for (const p of preopens) {
         this.fds.set(nextPre, { kind: 'preopen_dir', path: normalize(p), position: 0n });
         this._preopenFds.push(nextPre);
         nextPre++;
      }
      this._nextFd = Math.max(this._nextFd, nextPre);
   }

   bind(instance) { this.memory = instance.exports.memory; }

   imports() {
      const w = this;
      const W = (name, fn) => {
         if (w._debug) return (...a) => { const r = fn(...a); console.log(`[wasi] ${name}(${a.join(',')}) → ${r}`); return r; };
         return fn;
      };
      return {
         wasi_snapshot_preview1: {
            args_sizes_get:     W('args_sizes_get',     (argc, bufsize) => w._args_sizes_get(argc, bufsize)),
            args_get:           W('args_get',           (argv, buf)     => w._args_get(argv, buf)),
            environ_sizes_get:  W('environ_sizes_get',  (cnt, sz)       => w._environ_sizes_get(cnt, sz)),
            environ_get:        W('environ_get',        (ep, buf)       => w._environ_get(ep, buf)),

            fd_close:           W('fd_close',           (fd)             => w._fd_close(fd)),
            fd_fdstat_get:      W('fd_fdstat_get',      (fd, ptr)        => w._fd_fdstat_get(fd, ptr)),
            fd_fdstat_set_flags:W('fd_fdstat_set_flags',(fd, flags)      => E.SUCCESS),
            fd_filestat_get:    W('fd_filestat_get',    (fd, ptr)        => w._fd_filestat_get(fd, ptr)),
            fd_prestat_get:     W('fd_prestat_get',     (fd, ptr)        => w._fd_prestat_get(fd, ptr)),
            fd_prestat_dir_name:W('fd_prestat_dir_name',(fd, buf, len)   => w._fd_prestat_dir_name(fd, buf, len)),
            fd_read:            W('fd_read',            (fd, iovs, n, np) => w._fd_read(fd, iovs, n, np)),
            fd_write:           W('fd_write',           (fd, iovs, n, np) => w._fd_write(fd, iovs, n, np)),
            fd_seek:            W('fd_seek',            (fd, off, w2, newp) => w._fd_seek(fd, off, w2, newp)),
            fd_tell:            W('fd_tell',            (fd, ptr)        => w._fd_tell(fd, ptr)),
            fd_sync:            (fd) => E.SUCCESS,
            fd_readdir:         W('fd_readdir',         (fd, buf, sz, cookie_lo, cookie_hi, nread) => w._fd_readdir(fd, buf, sz, cookie_lo, cookie_hi, nread)),
            fd_renumber:        (from, to) => E.SUCCESS,
            fd_advise:          (fd, off, len, advice) => E.SUCCESS,
            fd_allocate:        (fd, off, len) => E.SUCCESS,
            fd_datasync:        (fd) => E.SUCCESS,
            fd_filestat_set_size: W('fd_filestat_set_size', (fd, size) => w._fd_filestat_set_size(fd, size)),
            fd_filestat_set_times: (fd, atim, mtim, flags) => E.SUCCESS,
            fd_pread:           W('fd_pread',  (fd, iovs, n, offset_bigint, nread_ptr) => w._fd_pread(fd, iovs, n, offset_bigint, nread_ptr)),
            fd_pwrite:          W('fd_pwrite', (fd, iovs, n, offset_bigint, nwritten_ptr) => w._fd_pwrite(fd, iovs, n, offset_bigint, nwritten_ptr)),

            path_open:          W('path_open',          (dirfd, df, pp, pl, of, rb, ri, fdf, newfdp) =>
                                                        w._path_open(dirfd, df, pp, pl, of, rb, ri, fdf, newfdp)),
            path_filestat_get:  W('path_filestat_get',  (dirfd, df, pp, pl, ptr) => w._path_filestat_get(dirfd, df, pp, pl, ptr)),
            path_create_directory: W('path_create_directory', (dirfd, pp, pl) => w._path_create_directory(dirfd, pp, pl)),
            path_unlink_file:   W('path_unlink_file',   (dirfd, pp, pl) => w._path_unlink_file(dirfd, pp, pl)),
            path_remove_directory: W('path_remove_directory', (dirfd, pp, pl) => w._path_remove_directory(dirfd, pp, pl)),
            path_rename:        W('path_rename',        (f_dirfd, f_pp, f_pl, t_dirfd, t_pp, t_pl) => w._path_rename(f_dirfd, f_pp, f_pl, t_dirfd, t_pp, t_pl)),
            path_readlink:      (...a) => E.NOENT,
            path_filestat_set_times: (...a) => E.SUCCESS,
            path_symlink:       (...a) => E.NOSYS,
            path_link:          (...a) => E.NOSYS,

            proc_exit:          (code) => { w._flush(); throw new ExitError(code); },
            proc_raise:         (sig)  => E.NOSYS,

            clock_res_get:      (id, ptr) => { w._writeU64(ptr, 1_000_000n); return E.SUCCESS; },
            clock_time_get:     (id, prec, ptr) => {
               const ns = BigInt(Math.floor(performance.timeOrigin * 1e6))
                        + BigInt(Math.floor(performance.now() * 1e6));
               w._writeU64(ptr, ns);
               return E.SUCCESS;
            },
            random_get:         (buf, len) => {
               crypto.getRandomValues(w._bytes().subarray(buf, buf + len));
               return E.SUCCESS;
            },

            sched_yield:        () => E.SUCCESS,
            poll_oneoff:        (...a) => E.NOSYS,
            sock_recv:          (...a) => E.NOSYS,
            sock_send:          (...a) => E.NOSYS,
            sock_shutdown:      (...a) => E.NOSYS,
            sock_accept:        (...a) => E.NOSYS,
         },
      };
   }

   // ── Memory helpers ───────────────────────────────────────────
   _dv() { return new DataView(this.memory.buffer); }
   _bytes() { return new Uint8Array(this.memory.buffer); }
   _writeU8(p, v)  { this._dv().setUint8(p, v); }
   _writeU16(p, v) { this._dv().setUint16(p, v, true); }
   _writeU32(p, v) { this._dv().setUint32(p, v, true); }
   _writeU64(p, v) { this._dv().setBigUint64(p, typeof v === 'bigint' ? v : BigInt(v), true); }
   _readStr(p, len) { return new TextDecoder('utf-8', { fatal: false }).decode(this._bytes().subarray(p, p + len)); }
   _writeBytes(p, b) { this._bytes().set(b, p); }

   _readIovs(ptr, len) {
      const dv = this._dv();
      const out = [];
      for (let i = 0; i < len; i++) {
         out.push({ buf: dv.getUint32(ptr + i * 8, true), len: dv.getUint32(ptr + i * 8 + 4, true) });
      }
      return out;
   }

   // ── Stdio ────────────────────────────────────────────────────
   _args_sizes_get(argc_ptr, buf_size_ptr) {
      const enc = new TextEncoder();
      this._writeU32(argc_ptr, this.args.length);
      this._writeU32(buf_size_ptr, this.args.reduce((s, a) => s + enc.encode(a).length + 1, 0));
      return E.SUCCESS;
   }
   _args_get(argv_ptr, buf_ptr) {
      const enc = new TextEncoder();
      let cur = buf_ptr;
      for (let i = 0; i < this.args.length; i++) {
         this._writeU32(argv_ptr + i * 4, cur);
         const bytes = enc.encode(this.args[i] + '\0');
         this._writeBytes(cur, bytes);
         cur += bytes.length;
      }
      return E.SUCCESS;
   }
   _environ_sizes_get(cnt_ptr, sz_ptr) {
      const enc = new TextEncoder();
      this._writeU32(cnt_ptr, this.env.length);
      this._writeU32(sz_ptr, this.env.reduce((s, e) => s + enc.encode(e).length + 1, 0));
      return E.SUCCESS;
   }
   _environ_get(ep_ptr, buf_ptr) {
      const enc = new TextEncoder();
      let cur = buf_ptr;
      for (let i = 0; i < this.env.length; i++) {
         this._writeU32(ep_ptr + i * 4, cur);
         const bytes = enc.encode(this.env[i] + '\0');
         this._writeBytes(cur, bytes);
         cur += bytes.length;
      }
      return E.SUCCESS;
   }

   // ── fd management ────────────────────────────────────────────
   _allocFd() { return this._nextFd++; }

   _fd_close(fd) {
      if (!this.fds.has(fd)) return E.BADF;
      this.fds.delete(fd);
      return E.SUCCESS;
   }

   _fd_fdstat_get(fd, ptr) {
      const slot = this.fds.get(fd);
      if (!slot) return E.BADF;
      const dv = this._dv();
      let filetype = FT.CHAR;
      if (slot.kind === 'preopen_dir' || slot.kind === 'dir') filetype = FT.DIR;
      else if (slot.kind === 'file') filetype = FT.REG;
      dv.setUint8(ptr + 0, filetype);
      dv.setUint16(ptr + 2, 0, true);
      dv.setBigUint64(ptr + 8, 0xffffffffffffffffn, true);
      dv.setBigUint64(ptr + 16, 0xffffffffffffffffn, true);
      return E.SUCCESS;
   }

   _fd_filestat_get(fd, ptr) {
      const slot = this.fds.get(fd);
      if (!slot) return E.BADF;
      return this._writeFilestat(ptr, slot.path, slot.kind);
   }

   _writeFilestat(ptr, path, kind) {
      const dv = this._dv();
      let size = 0n, filetype = FT.CHAR, mtime = 0n, ino = 0n;
      if (path != null) {
         const entry = this.fs.get(path);
         if (!entry) return E.NOENT;
         if (entry.kind === 'file') {
            size = BigInt(entry.data.byteLength);
            filetype = FT.REG;
            mtime = entry.mtime_ns;
         } else if (entry.kind === 'dir') {
            filetype = FT.DIR;
         }
         // Unique inode per path. Clang dedupes include-search dirs by
         // (dev, ino) — returning ino=0 for every dir makes clang collapse
         // /sysroot/include, /sysroot/include/wasm32-wasip1, /resource/include
         // to a single entry. FNV-1a 64-bit is fine here.
         ino = fnv1a64(path);
      }
      dv.setBigUint64(ptr + 0, 1n, true);   // dev (non-zero so dedup works)
      dv.setBigUint64(ptr + 8, ino, true);
      dv.setUint8(ptr + 16, filetype);
      // pad 7
      dv.setBigUint64(ptr + 24, 1n, true);  // nlink
      dv.setBigUint64(ptr + 32, size, true);
      dv.setBigUint64(ptr + 40, mtime, true); // atim
      dv.setBigUint64(ptr + 48, mtime, true); // mtim
      dv.setBigUint64(ptr + 56, mtime, true); // ctim
      return E.SUCCESS;
   }

   _fd_prestat_get(fd, ptr) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'preopen_dir') return E.BADF;
      const dv = this._dv();
      dv.setUint8(ptr + 0, 0); // preopentype: dir
      // pr_name_len is at offset 4 (after 4-byte tag + padding)
      dv.setUint32(ptr + 4, new TextEncoder().encode(slot.path).length, true);
      return E.SUCCESS;
   }

   _fd_prestat_dir_name(fd, buf, len) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'preopen_dir') return E.BADF;
      const bytes = new TextEncoder().encode(slot.path);
      if (bytes.length > len) return E.INVAL;
      this._writeBytes(buf, bytes);
      return E.SUCCESS;
   }

   // ── File I/O ─────────────────────────────────────────────────
   _fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr) {
      const slot = this.fds.get(fd);
      if (!slot) return E.BADF;
      const iovs = this._readIovs(iovs_ptr, iovs_len);
      let total = 0;

      if (slot.kind === 'stdout' || slot.kind === 'stderr') {
         const chunks = [];
         for (const { buf, len } of iovs) {
            if (len === 0) continue;
            chunks.push(this._bytes().slice(buf, buf + len));
            total += len;
         }
         const text = new TextDecoder('utf-8', { fatal: false }).decode(concat(chunks));
         if (slot.kind === 'stdout') this._bufOut('_outBuf', text, this._stdoutCb);
         else                        this._bufOut('_errBuf', text, this._stderrCb);
         this._writeU32(nwritten_ptr, total);
         return E.SUCCESS;
      }

      if (slot.kind !== 'file') return E.BADF;
      if (!slot.writable) return E.PERM;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;

      // Concat the iovecs, place at slot.position
      const chunks = [];
      for (const { buf, len } of iovs) {
         if (len === 0) continue;
         chunks.push(this._bytes().slice(buf, buf + len));
         total += len;
      }
      const src = concat(chunks);
      const newSize = Math.max(entry.data.byteLength, slot.position + src.byteLength);
      if (newSize > entry.data.byteLength) {
         const grown = new Uint8Array(newSize);
         grown.set(entry.data, 0);
         entry.data = grown;
      }
      entry.data.set(src, slot.position);
      slot.position += src.byteLength;
      entry.mtime_ns = BigInt(Date.now()) * 1_000_000n;
      this._writeU32(nwritten_ptr, total);
      return E.SUCCESS;
   }

   _fd_read(fd, iovs_ptr, iovs_len, nread_ptr) {
      const slot = this.fds.get(fd);
      if (!slot) return E.BADF;

      if (slot.kind === 'stdin') {
         const line = this._stdinFn();
         if (line == null) { this._writeU32(nread_ptr, 0); return E.SUCCESS; }
         const bytes = new TextEncoder().encode(line);
         return this._readIntoIovs(bytes, 0, iovs_ptr, iovs_len, nread_ptr);
      }

      if (slot.kind !== 'file') { this._writeU32(nread_ptr, 0); return E.SUCCESS; }
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;
      const read = this._readIntoIovs(entry.data, slot.position, iovs_ptr, iovs_len, nread_ptr);
      // Advance position by bytes actually read
      const dv = this._dv();
      slot.position += dv.getUint32(nread_ptr, true);
      return read;
   }

   _readIntoIovs(srcBytes, srcOffset, iovs_ptr, iovs_len, nread_ptr) {
      const iovs = this._readIovs(iovs_ptr, iovs_len);
      let total = 0;
      for (const { buf, len } of iovs) {
         if (srcOffset >= srcBytes.byteLength) break;
         const toCopy = Math.min(len, srcBytes.byteLength - srcOffset);
         this._writeBytes(buf, srcBytes.subarray(srcOffset, srcOffset + toCopy));
         srcOffset += toCopy;
         total += toCopy;
      }
      this._writeU32(nread_ptr, total);
      return E.SUCCESS;
   }

   _fd_seek(fd, offset_bigint_lo_hi_as_single_i64, whence, newoffset_ptr) {
      // WASI passes a 64-bit signed offset as a single i64 param. JS here gets a BigInt.
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'file') return E.INVAL;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;

      const offset = typeof offset_bigint_lo_hi_as_single_i64 === 'bigint'
                       ? Number(offset_bigint_lo_hi_as_single_i64)
                       : offset_bigint_lo_hi_as_single_i64;
      let newPos;
      switch (whence) {
         case 0: newPos = offset; break;                               // SET
         case 1: newPos = slot.position + offset; break;               // CUR
         case 2: newPos = entry.data.byteLength + offset; break;       // END
         default: return E.INVAL;
      }
      if (newPos < 0) return E.INVAL;
      slot.position = newPos;
      this._writeU64(newoffset_ptr, BigInt(newPos));
      return E.SUCCESS;
   }

   _fd_pread(fd, iovs_ptr, iovs_len, offset_bigint, nread_ptr) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'file') return E.BADF;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;
      const offset = Number(typeof offset_bigint === 'bigint' ? offset_bigint : BigInt(offset_bigint));
      return this._readIntoIovs(entry.data, offset, iovs_ptr, iovs_len, nread_ptr);
   }

   _fd_pwrite(fd, iovs_ptr, iovs_len, offset_bigint, nwritten_ptr) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'file') return E.BADF;
      if (!slot.writable) return E.PERM;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;
      const offset = Number(typeof offset_bigint === 'bigint' ? offset_bigint : BigInt(offset_bigint));
      const iovs = this._readIovs(iovs_ptr, iovs_len);
      const chunks = [];
      let total = 0;
      for (const { buf, len } of iovs) {
         if (len === 0) continue;
         chunks.push(this._bytes().slice(buf, buf + len));
         total += len;
      }
      const src = concat(chunks);
      const newSize = Math.max(entry.data.byteLength, offset + src.byteLength);
      if (newSize > entry.data.byteLength) {
         const grown = new Uint8Array(newSize);
         grown.set(entry.data, 0);
         entry.data = grown;
      }
      entry.data.set(src, offset);
      entry.mtime_ns = BigInt(Date.now()) * 1_000_000n;
      this._writeU32(nwritten_ptr, total);
      return E.SUCCESS;
   }

   _fd_filestat_set_size(fd, size_bigint) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'file') return E.BADF;
      if (!slot.writable) return E.PERM;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'file') return E.BADF;
      const newSize = Number(typeof size_bigint === 'bigint' ? size_bigint : BigInt(size_bigint));
      if (newSize === entry.data.byteLength) return E.SUCCESS;
      const next = new Uint8Array(newSize);
      next.set(entry.data.subarray(0, Math.min(newSize, entry.data.byteLength)));
      entry.data = next;
      entry.mtime_ns = BigInt(Date.now()) * 1_000_000n;
      return E.SUCCESS;
   }

   _fd_tell(fd, ptr) {
      const slot = this.fds.get(fd);
      if (!slot || slot.kind !== 'file') return E.INVAL;
      this._writeU64(ptr, BigInt(slot.position));
      return E.SUCCESS;
   }

   _fd_readdir(fd, buf, buf_len, cookie_bigint, nread_ptr) {
      const slot = this.fds.get(fd);
      if (!slot || (slot.kind !== 'preopen_dir' && slot.kind !== 'dir')) return E.BADF;
      const entry = this.fs.get(slot.path);
      if (!entry || entry.kind !== 'dir') return E.BADF;

      const children = Array.from(entry.children).sort();
      const enc = new TextEncoder();
      let cookie = typeof cookie_bigint === 'bigint' ? Number(cookie_bigint) : cookie_bigint;
      let written = 0;
      const dv = this._dv();

      for (let i = cookie; i < children.length; i++) {
         const name = children[i];
         const childPath = slot.path === '/' ? '/' + name : slot.path + '/' + name;
         const child = this.fs.get(childPath);
         const nameBytes = enc.encode(name);
         const entrySize = 24 + nameBytes.length; // dirent header is 24 bytes
         if (written + entrySize > buf_len) break;

         const entryPtr = buf + written;
         dv.setBigUint64(entryPtr + 0, BigInt(i + 1), true);  // d_next (cookie of next entry)
         dv.setBigUint64(entryPtr + 8, 0n, true);             // d_ino
         dv.setUint32(entryPtr + 16, nameBytes.length, true); // d_namlen
         let ft = FT.UNKNOWN;
         if (child?.kind === 'file') ft = FT.REG;
         else if (child?.kind === 'dir') ft = FT.DIR;
         dv.setUint8(entryPtr + 20, ft);                      // d_type
         this._writeBytes(entryPtr + 24, nameBytes);
         written += entrySize;
      }
      this._writeU32(nread_ptr, written);
      return E.SUCCESS;
   }

   // ── Path operations ──────────────────────────────────────────
   _resolvePath(dirfd, path_ptr, path_len) {
      const slot = this.fds.get(dirfd);
      if (!slot) return null;
      const rel = this._readStr(path_ptr, path_len);
      const baseDir = (slot.kind === 'preopen_dir' || slot.kind === 'dir') ? slot.path : '/';
      const joined = rel.startsWith('/') ? rel : (baseDir === '/' ? '/' + rel : baseDir + '/' + rel);
      return normalize(joined);
   }

   _path_open(dirfd, dirflags, path_ptr, path_len,
              oflags, rights_base, rights_inheriting, fdflags, newfd_ptr) {
      const path = this._resolvePath(dirfd, path_ptr, path_len);
      if (path == null) return E.BADF;

      const creat = (oflags & O.CREAT) !== 0;
      const excl  = (oflags & O.EXCL)  !== 0;
      const trunc = (oflags & O.TRUNC) !== 0;
      const mustDir = (oflags & O.DIRECTORY) !== 0;

      let entry = this.fs.get(path);
      if (!entry) {
         if (!creat) return E.NOENT;
         this.fs.writeFile(path, new Uint8Array(0));
         entry = this.fs.get(path);
      } else if (creat && excl) {
         return E.EXIST;
      }

      if (mustDir && entry.kind !== 'dir') return E.NOTDIR;

      if (entry.kind === 'dir') {
         const fd = this._allocFd();
         this.fds.set(fd, { kind: 'dir', path, position: 0 });
         this._writeU32(newfd_ptr, fd);
         return E.SUCCESS;
      }

      if (trunc && entry.kind === 'file') {
         entry.data = new Uint8Array(0);
         entry.mtime_ns = BigInt(Date.now()) * 1_000_000n;
      }

      // rights_base bit 6 is FD_READ, bit 7 is FD_WRITE — but many guests
      // pass maxed-out rights masks. Use fdflags + oflags to infer.
      //
      // Simple heuristic: if any write-oflag was set OR rights_base
      // has FD_WRITE bit, allow write.
      const FD_WRITE = 1n << 6n;
      const writable = creat || trunc
                     || (typeof rights_base === 'bigint' && (rights_base & FD_WRITE) !== 0n)
                     || (typeof rights_base === 'number' && (BigInt(rights_base) & FD_WRITE) !== 0n);

      const fd = this._allocFd();
      this.fds.set(fd, { kind: 'file', path, position: 0, writable });
      this._writeU32(newfd_ptr, fd);
      return E.SUCCESS;
   }

   _path_filestat_get(dirfd, dirflags, path_ptr, path_len, ptr) {
      const path = this._resolvePath(dirfd, path_ptr, path_len);
      if (path == null) return E.BADF;
      const entry = this.fs.get(path);
      if (!entry) return E.NOENT;
      return this._writeFilestat(ptr, path, entry.kind);
   }

   _path_create_directory(dirfd, path_ptr, path_len) {
      const path = this._resolvePath(dirfd, path_ptr, path_len);
      if (path == null) return E.BADF;
      if (this.fs.exists(path)) return E.EXIST;
      this.fs.mkdir(path);
      return E.SUCCESS;
   }

   _path_unlink_file(dirfd, path_ptr, path_len) {
      const path = this._resolvePath(dirfd, path_ptr, path_len);
      if (path == null) return E.BADF;
      return this.fs.unlink(path) ? E.SUCCESS : E.NOENT;
   }

   _path_remove_directory(dirfd, path_ptr, path_len) {
      const path = this._resolvePath(dirfd, path_ptr, path_len);
      if (path == null) return E.BADF;
      const e = this.fs.get(path);
      if (!e) return E.NOENT;
      if (e.kind !== 'dir') return E.NOTDIR;
      if (e.children && e.children.size > 0) return E.NOTEMPTY;
      return this.fs.unlink(path) ? E.SUCCESS : E.NOENT;
   }

   _path_rename(fromFd, fp, fl, toFd, tp, tl) {
      const from = this._resolvePath(fromFd, fp, fl);
      const to   = this._resolvePath(toFd, tp, tl);
      if (from == null || to == null) return E.BADF;
      return this.fs.rename(from, to) ? E.SUCCESS : E.NOENT;
   }

   // ── Buffering ────────────────────────────────────────────────
   _bufOut(key, chunk, cb) {
      this[key] += chunk;
      let idx;
      while ((idx = this[key].indexOf('\n')) !== -1) {
         cb(this[key].slice(0, idx + 1));
         this[key] = this[key].slice(idx + 1);
      }
   }

   _flush() {
      if (this._outBuf) { this._stdoutCb(this._outBuf); this._outBuf = ''; }
      if (this._errBuf) { this._stderrCb(this._errBuf); this._errBuf = ''; }
   }
}

// FNV-1a 64-bit hash of a string — used for pseudo-inodes so clang can
// dedupe directories by (dev, ino) pairs.
function fnv1a64(str) {
   let h = 0xcbf29ce484222325n;
   const bytes = new TextEncoder().encode(str);
   for (const b of bytes) {
      h ^= BigInt(b);
      h = BigInt.asUintN(64, h * 0x100000001b3n);
   }
   // Avoid returning 0 (reserved).
   return h === 0n ? 1n : h;
}

function concat(chunks) {
   let total = 0; for (const c of chunks) total += c.length;
   const out = new Uint8Array(total);
   let p = 0; for (const c of chunks) { out.set(c, p); p += c.length; }
   return out;
}
