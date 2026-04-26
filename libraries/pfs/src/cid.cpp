#include <pfs/cid.hpp>
#include <pfs/config.hpp>
#include <pfs/unixfs.hpp>

#include <pcrypt/sha256.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pfs
{

cid cid::from_digest(std::span<const uint8_t, digest_size> digest)
{
   cid c;
   std::memcpy(c.bytes.data() + header_size, digest.data(), digest_size);
   return c;
}

cid cid::from_bytes(std::span<const uint8_t, size> raw)
{
   cid c;
   std::memcpy(c.bytes.data(), raw.data(), size);
   return c;
}

// RFC 4648 base32 (lowercase, no padding)
namespace
{
   constexpr char b32_alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";

   std::string base32_encode(psio1::bytes_view data)
   {
      std::string out;
      out.reserve((data.size() * 8 + 4) / 5);

      uint64_t buf  = 0;
      int      bits = 0;
      for (auto b : data)
      {
         buf = (buf << 8) | b;
         bits += 8;
         while (bits >= 5)
         {
            bits -= 5;
            out += b32_alphabet[(buf >> bits) & 0x1F];
         }
      }
      if (bits > 0)
         out += b32_alphabet[(buf << (5 - bits)) & 0x1F];

      return out;
   }

   int b32_val(char c)
   {
      if (c >= 'a' && c <= 'z')
         return c - 'a';
      if (c >= '2' && c <= '7')
         return c - '2' + 26;
      if (c >= 'A' && c <= 'Z')
         return c - 'A';
      return -1;
   }

   std::vector<uint8_t> base32_decode(std::string_view s)
   {
      std::vector<uint8_t> out;
      out.reserve(s.size() * 5 / 8);

      uint64_t buf  = 0;
      int      bits = 0;
      for (char c : s)
      {
         int v = b32_val(c);
         if (v < 0)
            throw std::runtime_error("pfs::cid: invalid base32 character");
         buf = (buf << 5) | v;
         bits += 5;
         if (bits >= 8)
         {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
         }
      }
      return out;
   }
}  // namespace

std::string cid::to_string() const
{
   return "b" + base32_encode({bytes.data(), size});
}

cid cid::from_string(std::string_view s)
{
   if (s.empty())
      throw std::runtime_error("pfs::cid: empty string");
   if (s[0] != 'b' && s[0] != 'B')
      throw std::runtime_error("pfs::cid: expected base32lower multibase prefix 'b'");

   auto decoded = base32_decode(s.substr(1));
   if (decoded.size() != size)
      throw std::runtime_error("pfs::cid: decoded size mismatch, expected 36 bytes");

   cid c;
   std::memcpy(c.bytes.data(), decoded.data(), size);
   return c;
}

cid compute_cid(psio1::bytes_view data)
{
   if (data.size() <= chunk_size)
   {
      auto block  = unixfs::encode_file_block(data);
      auto digest = pcrypt::sha256(block);
      return cid::from_digest(std::span<const uint8_t, 32>(digest));
   }

   std::vector<unixfs::dag_link> links;
   std::vector<uint64_t>         block_sizes;

   for (size_t off = 0; off < data.size(); off += chunk_size)
   {
      size_t len   = std::min(static_cast<size_t>(chunk_size), data.size() - off);
      auto   chunk = data.subspan(off, len);
      auto   block = unixfs::encode_file_block(chunk);

      auto digest = pcrypt::sha256(block);
      auto c      = cid::from_digest(std::span<const uint8_t, 32>(digest));

      unixfs::dag_link link;
      link.cid_bytes.assign(c.bytes.begin(), c.bytes.end());
      link.tsize = block.size();
      links.push_back(std::move(link));
      block_sizes.push_back(len);
   }

   auto root_block  = unixfs::encode_file_root(links, data.size(), block_sizes);
   auto root_digest = pcrypt::sha256(root_block);
   return cid::from_digest(std::span<const uint8_t, 32>(root_digest));
}

}  // namespace pfs
