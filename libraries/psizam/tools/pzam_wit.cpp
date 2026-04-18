// pzam wit: Manage WIT custom sections in WASM binaries.
//
// Subcommands:
//   pzam wit embed <module.wasm> [-o output.wasm]
//       Scan the data section for constexpr-generated WIT blobs (magic
//       prefix PSIO_WIT\x01), promote each to a component-wit:NAME
//       custom section, and excise the blobs from the data section
//       (splitting segments to leave no gap in linear memory init).
//
//   pzam wit show <module.wasm>
//       Display all component-wit and component-type custom sections.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ── LEB128 ──────────────────────────────────────────────────────────

static std::vector<uint8_t> leb128(uint32_t n) {
   std::vector<uint8_t> out;
   do {
      uint8_t b = n & 0x7f;
      n >>= 7;
      if (n) b |= 0x80;
      out.push_back(b);
   } while (n);
   return out;
}

static uint32_t read_leb128(const uint8_t*& p) {
   uint32_t r = 0;
   unsigned s = 0;
   while (true) {
      uint8_t b = *p++;
      r |= (uint32_t)(b & 0x7f) << s;
      s += 7;
      if (!(b & 0x80)) break;
   }
   return r;
}

static int32_t read_sleb128(const uint8_t*& p) {
   int32_t r = 0;
   unsigned s = 0;
   uint8_t b;
   do {
      b = *p++;
      r |= (int32_t)(b & 0x7f) << s;
      s += 7;
   } while (b & 0x80);
   if (s < 32 && (b & 0x40)) r |= -(1 << s);
   return r;
}

static std::vector<uint8_t> sleb128(int32_t n) {
   std::vector<uint8_t> out;
   while (true) {
      uint8_t b = n & 0x7f;
      n >>= 7;
      if ((n == 0 && !(b & 0x40)) || (n == -1 && (b & 0x40))) {
         out.push_back(b);
         break;
      }
      out.push_back(b | 0x80);
   }
   return out;
}

// ── Constants ───────────────────────────────────────────────────────

static constexpr char    WIT_MAGIC[]   = "PSIO_WIT\x01";
static constexpr size_t  WIT_MAGIC_LEN = sizeof(WIT_MAGIC) - 1;
static constexpr uint8_t SECTION_DATA  = 11;

// ── Data segment representation ─────────────────────────────────────

struct data_segment {
   uint32_t             flags;
   int32_t              mem_offset;  // linear memory start address
   std::vector<uint8_t> bytes;
};

// ── Parse and rebuild the WASM binary ───────────────────────────────

struct wasm_binary {
   std::vector<uint8_t> raw;

   // Section boundaries (offset into raw, payload size)
   struct section_loc {
      uint8_t  id;
      size_t   header_start;  // offset of section id byte
      size_t   payload_start; // offset of first payload byte
      size_t   payload_size;
   };

   std::vector<section_loc> sections;

   void parse() {
      const uint8_t* p = raw.data() + 8;
      const uint8_t* end = raw.data() + raw.size();
      while (p < end) {
         section_loc sec;
         sec.header_start = p - raw.data();
         sec.id = *p++;
         sec.payload_size = read_leb128(p);
         sec.payload_start = p - raw.data();
         p += sec.payload_size;
         sections.push_back(sec);
      }
   }

   // Parse data segments from the data section
   std::vector<data_segment> parse_data_segments() const {
      std::vector<data_segment> segs;
      for (auto& sec : sections) {
         if (sec.id != SECTION_DATA) continue;
         const uint8_t* p = raw.data() + sec.payload_start;
         uint32_t count = read_leb128(p);
         for (uint32_t i = 0; i < count; ++i) {
            data_segment ds;
            ds.flags = read_leb128(p);
            if (ds.flags == 0) {
               // Active, memory 0: i32.const offset, end
               if (*p != 0x41) break;  // expect i32.const
               ++p;
               ds.mem_offset = read_sleb128(p);
               if (*p != 0x0b) break;  // expect end
               ++p;
            }
            uint32_t size = read_leb128(p);
            ds.bytes.assign(p, p + size);
            p += size;
            segs.push_back(std::move(ds));
         }
      }
      return segs;
   }

   // Encode data segments back to binary
   static std::vector<uint8_t> encode_data_section(const std::vector<data_segment>& segs) {
      std::vector<uint8_t> payload;
      auto count = leb128(static_cast<uint32_t>(segs.size()));
      payload.insert(payload.end(), count.begin(), count.end());
      for (auto& seg : segs) {
         auto flags = leb128(seg.flags);
         payload.insert(payload.end(), flags.begin(), flags.end());
         if (seg.flags == 0) {
            payload.push_back(0x41);  // i32.const
            auto off = sleb128(seg.mem_offset);
            payload.insert(payload.end(), off.begin(), off.end());
            payload.push_back(0x0b);  // end
         }
         auto sz = leb128(static_cast<uint32_t>(seg.bytes.size()));
         payload.insert(payload.end(), sz.begin(), sz.end());
         payload.insert(payload.end(), seg.bytes.begin(), seg.bytes.end());
      }
      return payload;
   }

   // Rebuild the binary replacing the data section
   std::vector<uint8_t> rebuild(const std::vector<uint8_t>& new_data_payload) const {
      std::vector<uint8_t> out;
      // Copy header
      out.insert(out.end(), raw.begin(), raw.begin() + 8);
      for (auto& sec : sections) {
         if (sec.id == SECTION_DATA) {
            // Replace with new data section
            out.push_back(SECTION_DATA);
            auto sz = leb128(static_cast<uint32_t>(new_data_payload.size()));
            out.insert(out.end(), sz.begin(), sz.end());
            out.insert(out.end(), new_data_payload.begin(), new_data_payload.end());
         } else {
            // Copy original section verbatim
            size_t sec_end = sec.payload_start + sec.payload_size;
            out.insert(out.end(), raw.begin() + sec.header_start,
                       raw.begin() + sec_end);
         }
      }
      return out;
   }
};

// ── Find and extract WIT blobs from data segments ───────────────────

struct wit_blob {
   std::string interface_name;
   std::string wit_text;
   size_t      seg_index;     // which data segment
   size_t      offset_in_seg; // byte offset within that segment
   size_t      blob_size;     // total bytes (magic + len + text)
};

static std::vector<wit_blob> find_blobs_in_segments(
   const std::vector<data_segment>& segs)
{
   std::vector<wit_blob> blobs;
   for (size_t si = 0; si < segs.size(); ++si) {
      auto& seg = segs[si];
      for (size_t i = 0; i + WIT_MAGIC_LEN + 4 <= seg.bytes.size(); ++i) {
         if (std::memcmp(&seg.bytes[i], WIT_MAGIC, WIT_MAGIC_LEN) != 0)
            continue;

         size_t hdr = i + WIT_MAGIC_LEN;
         uint32_t text_len = seg.bytes[hdr] | (seg.bytes[hdr+1] << 8) |
                             (seg.bytes[hdr+2] << 16) | (seg.bytes[hdr+3] << 24);
         size_t total = WIT_MAGIC_LEN + 4 + text_len;
         if (i + total > seg.bytes.size()) continue;

         std::string text(reinterpret_cast<const char*>(&seg.bytes[hdr + 4]), text_len);

         std::string_view sv = text;
         auto pos = sv.find("interface ");
         if (pos == std::string_view::npos) continue;
         sv = sv.substr(pos + 10);
         auto end_pos = sv.find_first_of(" {");
         if (end_pos == std::string_view::npos) continue;

         wit_blob blob;
         blob.interface_name = std::string(sv.substr(0, end_pos));
         blob.wit_text = std::move(text);
         blob.seg_index = si;
         blob.offset_in_seg = i;
         blob.blob_size = total;
         blobs.push_back(std::move(blob));
         i += total - 1;
      }
   }
   return blobs;
}

// Excise blobs from segments by splitting
static std::vector<data_segment> excise_blobs(
   const std::vector<data_segment>& orig_segs,
   const std::vector<wit_blob>& blobs)
{
   // Group blobs by segment index
   std::vector<std::vector<const wit_blob*>> blobs_per_seg(orig_segs.size());
   for (auto& b : blobs)
      blobs_per_seg[b.seg_index].push_back(&b);

   std::vector<data_segment> result;
   for (size_t si = 0; si < orig_segs.size(); ++si) {
      auto& seg = orig_segs[si];
      auto& seg_blobs = blobs_per_seg[si];

      if (seg_blobs.empty()) {
         result.push_back(seg);
         continue;
      }

      // Sort blobs by offset
      std::sort(seg_blobs.begin(), seg_blobs.end(),
                [](auto* a, auto* b) { return a->offset_in_seg < b->offset_in_seg; });

      // Split the segment around each blob
      size_t cursor = 0;
      for (auto* blob : seg_blobs) {
         // Piece before the blob
         if (blob->offset_in_seg > cursor) {
            data_segment before;
            before.flags = seg.flags;
            before.mem_offset = seg.mem_offset + static_cast<int32_t>(cursor);
            before.bytes.assign(seg.bytes.begin() + cursor,
                                seg.bytes.begin() + blob->offset_in_seg);
            result.push_back(std::move(before));
         }
         // Skip the blob
         cursor = blob->offset_in_seg + blob->blob_size;
      }
      // Piece after the last blob
      if (cursor < seg.bytes.size()) {
         data_segment after;
         after.flags = seg.flags;
         after.mem_offset = seg.mem_offset + static_cast<int32_t>(cursor);
         after.bytes.assign(seg.bytes.begin() + cursor, seg.bytes.end());
         result.push_back(std::move(after));
      }
   }

   // Remove empty segments
   result.erase(
      std::remove_if(result.begin(), result.end(),
                     [](const data_segment& s) { return s.bytes.empty(); }),
      result.end());

   return result;
}

// Append a custom section
static void append_custom_section(std::vector<uint8_t>& wasm,
                                  const std::string& name,
                                  const uint8_t* payload, size_t payload_len) {
   auto name_leb = leb128(static_cast<uint32_t>(name.size()));
   size_t sec_size = name_leb.size() + name.size() + payload_len;
   auto size_leb = leb128(static_cast<uint32_t>(sec_size));
   wasm.push_back(0x00);
   wasm.insert(wasm.end(), size_leb.begin(), size_leb.end());
   wasm.insert(wasm.end(), name_leb.begin(), name_leb.end());
   wasm.insert(wasm.end(), name.begin(), name.end());
   wasm.insert(wasm.end(), payload, payload + payload_len);
}

// Also need to update datacount section if present
static void update_datacount(std::vector<uint8_t>& wasm, uint32_t new_count) {
   // Find the datacount section (id=12) and update it
   const uint8_t* p = wasm.data() + 8;
   const uint8_t* end = wasm.data() + wasm.size();
   while (p < end) {
      size_t offset = p - wasm.data();
      uint8_t id = *p++;
      const uint8_t* size_start = p;
      uint32_t size = read_leb128(p);
      if (id == 12) { // datacount
         // Overwrite in place — datacount payload is a single varuint32
         auto new_payload = leb128(new_count);
         auto new_size = leb128(static_cast<uint32_t>(new_payload.size()));
         // If the new encoding fits in the same space, overwrite
         size_t old_total = (p + size) - (wasm.data() + offset);
         size_t new_total = 1 + new_size.size() + new_payload.size();
         // For simplicity, only handle same-size or smaller
         if (new_total <= old_total) {
            size_t pos = offset;
            wasm[pos++] = 12;
            for (auto b : new_size) wasm[pos++] = b;
            for (auto b : new_payload) wasm[pos++] = b;
            // Pad remainder with zeros if smaller (won't happen for small counts)
         }
         return;
      }
      p += size;
   }
}

// ── Subcommands ─────────────────────────────────────────────────────

static int wit_embed(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam wit embed <module.wasm> [-o output.wasm]\n";
      return 1;
   }
   std::string input = argv[1];
   std::string output = input;
   for (int i = 2; i < argc; ++i) {
      if (std::string_view(argv[i]) == "-o" && i + 1 < argc)
         output = argv[++i];
   }

   std::ifstream ifs(input, std::ios::binary);
   if (!ifs) { std::cerr << "Cannot open " << input << "\n"; return 1; }
   std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
   ifs.close();

   wasm_binary wasm;
   wasm.raw = std::move(raw);
   wasm.parse();

   auto segments = wasm.parse_data_segments();
   auto blobs = find_blobs_in_segments(segments);
   if (blobs.empty()) {
      // No blobs — just copy input to output
      std::ofstream ofs(output, std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(wasm.raw.data()), wasm.raw.size());
      std::cerr << "No PSIO_WIT blobs found in " << input << "\n";
      return 0;
   }

   // Excise blobs from data segments
   auto new_segments = excise_blobs(segments, blobs);
   auto new_data_payload = wasm_binary::encode_data_section(new_segments);

   // Rebuild binary with new data section
   auto result = wasm.rebuild(new_data_payload);

   // Update datacount section if segment count changed
   if (new_segments.size() != segments.size())
      update_datacount(result, static_cast<uint32_t>(new_segments.size()));

   // Append component-wit custom sections
   for (auto& blob : blobs) {
      std::cerr << "  " << blob.interface_name
                << " (" << blob.wit_text.size() << " bytes)\n";
      std::string section_name = "component-wit:" + blob.interface_name;
      append_custom_section(result, section_name,
         reinterpret_cast<const uint8_t*>(blob.wit_text.data()),
         blob.wit_text.size());
   }

   std::ofstream ofs(output, std::ios::binary);
   if (!ofs) { std::cerr << "Cannot write " << output << "\n"; return 1; }
   ofs.write(reinterpret_cast<const char*>(result.data()), result.size());

   size_t saved = wasm.raw.size() - result.size()
                  + blobs.size() * 50; // approximate custom section overhead
   std::cerr << "Embedded " << blobs.size() << " WIT section(s), "
             << "data section: " << segments.size() << " → "
             << new_segments.size() << " segments\n";
   return 0;
}

static int wit_show(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam wit show <module.wasm>\n";
      return 1;
   }

   std::ifstream ifs(argv[1], std::ios::binary);
   if (!ifs) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }
   std::vector<uint8_t> raw((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

   wasm_binary wasm;
   wasm.raw = std::move(raw);
   wasm.parse();

   for (auto& sec : wasm.sections) {
      if (sec.id != 0) continue;
      const uint8_t* p = wasm.raw.data() + sec.payload_start;
      uint32_t name_len = read_leb128(p);
      std::string name(reinterpret_cast<const char*>(p), name_len);
      p += name_len;
      size_t payload_len = sec.payload_size - (p - (wasm.raw.data() + sec.payload_start));

      if (name.find("component-wit:") == 0 || name.find("component-type:") == 0) {
         std::cout << "── " << name << " (" << payload_len << " bytes) ──\n";
         if (name.find("component-wit:") == 0) {
            std::cout << std::string_view(reinterpret_cast<const char*>(p), payload_len);
            if (payload_len > 0 && p[payload_len - 1] != '\n')
               std::cout << '\n';
         } else {
            std::cout << "(binary, " << payload_len << " bytes)\n";
         }
         std::cout << '\n';
      }
   }

   // Check for un-promoted blobs in data section
   auto segments = wasm.parse_data_segments();
   auto blobs = find_blobs_in_segments(segments);
   if (!blobs.empty()) {
      std::cout << "── Un-promoted PSIO_WIT blobs in data section ──\n";
      for (auto& b : blobs)
         std::cout << "  " << b.interface_name
                   << " (" << b.wit_text.size() << " bytes in segment "
                   << b.seg_index << ")\n";
   }

   return 0;
}

// ── Entry point ─────────────────────────────────────────────────────

int pzam_wit_main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam wit <embed|show> [options]\n";
      return 1;
   }
   std::string_view cmd = argv[1];
   if (cmd == "embed") return wit_embed(argc - 1, argv + 1);
   if (cmd == "show")  return wit_show(argc - 1, argv + 1);
   std::cerr << "Unknown wit subcommand: " << cmd << "\n";
   return 1;
}

#ifdef PZAM_STANDALONE_WIT
int main(int argc, char** argv) { return pzam_wit_main(argc, argv); }
#endif
