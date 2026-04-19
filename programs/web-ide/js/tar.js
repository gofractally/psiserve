// Minimal POSIX ustar reader. Handles regular files and directories.
// Returns entries in order. Symlinks, hard links, and extended headers
// are skipped but acknowledged.
//
// Usage:
//   const entries = parseTar(arrayBuffer);
//   for (const e of entries) {
//     if (e.type === 'file') ... e.path, e.data (Uint8Array)
//     if (e.type === 'dir')  ... e.path
//   }

export function parseTar(buffer) {
   const bytes = new Uint8Array(buffer);
   const entries = [];
   let offset = 0;
   const decoder = new TextDecoder('utf-8');

   while (offset + 512 <= bytes.length) {
      const header = bytes.subarray(offset, offset + 512);
      if (isZero(header)) break; // end marker

      const name = cstr(header, 0, 100);
      if (!name) { offset += 512; continue; }

      const sizeOctal = cstr(header, 124, 12);
      const size = parseInt(sizeOctal, 8) || 0;
      const typeflag = String.fromCharCode(header[156] || 0);

      // ustar prefix extends the path
      const prefix = cstr(header, 345, 155);
      let path = prefix ? prefix + '/' + name : name;

      // Normalize: strip leading "./"
      if (path.startsWith('./')) path = path.slice(2);
      // Strip trailing '/' for dirs (we set type explicitly)
      const isDir = path.endsWith('/') || typeflag === '5';
      if (isDir && path.endsWith('/')) path = path.slice(0, -1);

      const dataStart = offset + 512;
      const dataEnd = dataStart + size;
      const padded = (dataEnd + 511) & ~511;

      if (typeflag === '0' || typeflag === '' || typeflag === '\0') {
         // Regular file
         entries.push({
            type: 'file',
            path,
            data: bytes.subarray(dataStart, dataEnd),
         });
      } else if (typeflag === '5' || isDir) {
         if (path) entries.push({ type: 'dir', path });
      }
      // Skip symlinks (2), hard links (1), PAX extended headers (x, g), etc.

      offset = padded;
   }
   return entries;

   function cstr(buf, start, len) {
      let end = start;
      while (end < start + len && buf[end] !== 0) end++;
      return decoder.decode(buf.subarray(start, end));
   }
   function isZero(buf) {
      for (let i = 0; i < 512; i++) if (buf[i] !== 0) return false;
      return true;
   }
}
