#include <pfs/unixfs.hpp>

#include <cstring>

// Hand-rolled protobuf encoder for dag-pb + UnixFS.
//
// dag-pb schema (from IPLD):
//   message PBLink { optional bytes Hash = 2; optional uint64 Tsize = 3; }
//   message PBNode { repeated PBLink Links = 2; optional bytes Data = 1; }
//
// UnixFS Data schema:
//   message Data { required Type DataType = 1; optional bytes Data = 2;
//                  optional uint64 filesize = 3; repeated uint64 blocksizes = 4; }
//   enum Type { Raw=0; Directory=1; File=2; ... }

namespace pfs::unixfs
{

namespace
{
   // Protobuf varint encoding
   void write_varint(std::vector<uint8_t>& buf, uint64_t v)
   {
      while (v >= 0x80)
      {
         buf.push_back(static_cast<uint8_t>(v | 0x80));
         v >>= 7;
      }
      buf.push_back(static_cast<uint8_t>(v));
   }

   // Protobuf field tag
   void write_tag(std::vector<uint8_t>& buf, uint32_t field, uint32_t wire_type)
   {
      write_varint(buf, (field << 3) | wire_type);
   }

   // Protobuf length-delimited field
   void write_bytes_field(std::vector<uint8_t>& buf,
                          uint32_t              field,
                          psio1::bytes_view data)
   {
      write_tag(buf, field, 2);  // wire type 2 = length-delimited
      write_varint(buf, data.size());
      buf.insert(buf.end(), data.begin(), data.end());
   }

   void write_varint_field(std::vector<uint8_t>& buf, uint32_t field, uint64_t v)
   {
      write_tag(buf, field, 0);  // wire type 0 = varint
      write_varint(buf, v);
   }

   // Build UnixFS Data message for a file leaf (type=File, with inline data)
   std::vector<uint8_t> make_unixfs_data(psio1::bytes_view file_data)
   {
      std::vector<uint8_t> buf;
      buf.reserve(file_data.size() + 16);

      // field 1: DataType = 2 (File)
      write_varint_field(buf, 1, 2);

      // field 2: Data (the file bytes)
      write_bytes_field(buf, 2, file_data);

      // field 3: filesize
      write_varint_field(buf, 3, file_data.size());

      return buf;
   }

   // Build UnixFS Data message for a file root node (type=File, no inline data)
   std::vector<uint8_t> make_unixfs_root_data(uint64_t                  total_size,
                                               std::span<const uint64_t> block_sizes)
   {
      std::vector<uint8_t> buf;
      buf.reserve(32 + block_sizes.size() * 10);

      // field 1: DataType = 2 (File)
      write_varint_field(buf, 1, 2);

      // field 3: filesize
      write_varint_field(buf, 3, total_size);

      // field 4: blocksizes (repeated)
      for (auto bs : block_sizes)
         write_varint_field(buf, 4, bs);

      return buf;
   }
}  // namespace

std::vector<uint8_t> encode_file_block(psio1::bytes_view data)
{
   // Build the UnixFS Data message
   auto unixfs_data = make_unixfs_data(data);

   // Wrap in PBNode: field 1 = Data (the UnixFS bytes)
   std::vector<uint8_t> node;
   node.reserve(unixfs_data.size() + 8);
   write_bytes_field(node, 1, unixfs_data);

   return node;
}

std::vector<uint8_t> encode_file_root(std::span<const dag_link> links,
                                      uint64_t                  total_size,
                                      std::span<const uint64_t> block_sizes)
{
   std::vector<uint8_t> node;
   node.reserve(links.size() * 64 + 32);

   // PBNode.Links (field 2, repeated) — must come BEFORE Data in dag-pb
   for (auto& link : links)
   {
      // Build PBLink message
      std::vector<uint8_t> pb_link;
      pb_link.reserve(link.cid_bytes.size() + 16);

      // PBLink.Hash (field 2) = CID bytes
      write_bytes_field(pb_link, 2, link.cid_bytes);

      // PBLink.Tsize (field 3) = total size
      write_varint_field(pb_link, 3, link.tsize);

      // Embed PBLink as field 2 of PBNode
      write_bytes_field(node, 2, pb_link);
   }

   // PBNode.Data (field 1) = UnixFS root data
   auto unixfs_data = make_unixfs_root_data(total_size, block_sizes);
   write_bytes_field(node, 1, unixfs_data);

   return node;
}

}  // namespace pfs::unixfs
