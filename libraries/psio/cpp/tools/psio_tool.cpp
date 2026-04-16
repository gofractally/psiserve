// psio_tool.cpp — Universal serialization format CLI
//
// Validate, inspect, and convert between Cap'n Proto, FlatBuffers, fracpack,
// and WIT formats using dynamic schemas.  No code generation required.
//
// Usage:
//   psio-tool info
//   psio-tool inspect <format> <schema-file> <data-file>
//   psio-tool validate <format> <schema-file> <data-file>
//   psio-tool convert <src-format> <dst-format> <schema-file> <data-file>
//   psio-tool schema <src-format> <schema-file> --to <dst-format>

#include <psio/capnp_parser.hpp>
#include <psio/capnp_view.hpp>
#include <psio/fbs_parser.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// ============================================================================
// Utilities
// ============================================================================

static std::string read_file(const std::string& path)
{
   if (path == "-")
   {
      // Read stdin
      std::ostringstream ss;
      ss << std::cin.rdbuf();
      return ss.str();
   }
   std::ifstream f(path, std::ios::binary);
   if (!f)
   {
      std::cerr << "error: cannot open file: " << path << "\n";
      std::exit(1);
   }
   std::ostringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

static std::vector<uint8_t> read_binary_file(const std::string& path)
{
   if (path == "-")
   {
      std::vector<uint8_t> buf;
      char                 c;
      while (std::cin.get(c))
         buf.push_back(static_cast<uint8_t>(c));
      return buf;
   }
   std::ifstream f(path, std::ios::binary);
   if (!f)
   {
      std::cerr << "error: cannot open file: " << path << "\n";
      std::exit(1);
   }
   return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
}

// ============================================================================
// Build dynamic_schema from capnp_parsed_file
// ============================================================================

// Storage for the runtime schema data (owns the heap allocations)
struct capnp_runtime_schema
{
   // Per-struct storage
   struct struct_schema
   {
      std::vector<psio::dynamic_field_desc> sorted_fields;
      std::vector<std::string>              field_names;
      std::vector<const char*>              ordered_names;
      std::vector<uint8_t>                  tags;
      psio::dynamic_schema                  schema;
   };

   std::vector<std::unique_ptr<struct_schema>> schemas;  // parallel to parsed_file.structs

   // Build schemas for all structs in a parsed capnp file
   void build(const psio::capnp_parsed_file& file)
   {
      schemas.resize(file.structs.size());

      // First pass: build field descriptors for each struct
      for (size_t si = 0; si < file.structs.size(); ++si)
      {
         const auto& ps = file.structs[si];
         auto        ss = std::make_unique<struct_schema>();

         size_t nfields = ps.fields.size();
         ss->sorted_fields.resize(nfields);
         ss->field_names.resize(nfields);
         ss->ordered_names.resize(nfields);
         ss->tags.resize(nfields);

         for (size_t i = 0; i < nfields; ++i)
         {
            const auto& pf = ps.fields[i];
            auto&       df = ss->sorted_fields[i];

            ss->field_names[i] = pf.name;
            df.name_hash       = psio::xxh64_hash(pf.name);
            df.name            = nullptr;  // set after sort
            df.type            = capnp_tag_to_dynamic(pf.type.tag);
            df.is_ptr          = pf.loc.is_ptr;
            df.offset          = pf.loc.offset;
            df.bit_index       = pf.loc.bit_index;
            df.byte_size       = capnp_tag_byte_size(pf.type.tag);
            df.nested          = nullptr;  // set in second pass
         }

         // Sort by name_hash
         std::vector<size_t> order(nfields);
         for (size_t i = 0; i < nfields; ++i)
            order[i] = i;
         std::sort(order.begin(), order.end(),
                   [&](size_t a, size_t b)
                   { return ss->sorted_fields[a].name_hash < ss->sorted_fields[b].name_hash; });

         std::vector<psio::dynamic_field_desc> sorted(nfields);
         for (size_t i = 0; i < nfields; ++i)
            sorted[i] = ss->sorted_fields[order[i]];
         ss->sorted_fields = std::move(sorted);

         // Set ordered_names (declaration order)
         for (size_t i = 0; i < nfields; ++i)
            ss->ordered_names[i] = ss->field_names[i].c_str();

         // Set name pointers in sorted fields and tag bytes
         for (size_t i = 0; i < nfields; ++i)
         {
            size_t orig                = order[i];
            ss->sorted_fields[i].name = ss->field_names[orig].c_str();
            ss->tags[i] = static_cast<uint8_t>(ss->sorted_fields[i].name_hash);
         }

         // Fill in schema struct
         ss->schema.sorted_fields = ss->sorted_fields.data();
         ss->schema.field_count   = nfields;
         ss->schema.ordered_names = ss->ordered_names.data();
         ss->schema.data_words    = ps.data_words;
         ss->schema.ptr_count     = ps.ptr_count;
         ss->schema.tags          = ss->tags.data();

         schemas[si] = std::move(ss);
      }

      // Second pass: set nested schema pointers for struct-typed and list-of-struct fields
      for (size_t si = 0; si < file.structs.size(); ++si)
      {
         const auto& ps = file.structs[si];
         auto&       ss = schemas[si];

         for (size_t i = 0; i < ss->sorted_fields.size(); ++i)
         {
            // Find the original field by name
            std::string_view fname = ss->sorted_fields[i].name;
            for (const auto& pf : ps.fields)
            {
               if (pf.name == fname)
               {
                  if (pf.type.tag == psio::capnp_type_tag::struct_)
                  {
                     int32_t ref_idx = pf.type.referenced_type_idx;
                     if (ref_idx >= 0 && ref_idx < static_cast<int32_t>(schemas.size()))
                        ss->sorted_fields[i].nested = &schemas[ref_idx]->schema;
                  }
                  else if (pf.type.tag == psio::capnp_type_tag::list)
                  {
                     // For List(StructType), element_type_idx >= 0 means struct index
                     int32_t elem_idx = pf.type.element_type_idx;
                     if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(schemas.size()))
                        ss->sorted_fields[i].nested = &schemas[elem_idx]->schema;
                  }
                  break;
               }
            }
         }
      }
   }

   const psio::dynamic_schema* find(const std::string& name,
                                    const psio::capnp_parsed_file& file) const
   {
      for (size_t i = 0; i < file.structs.size(); ++i)
         if (file.structs[i].name == name)
            return &schemas[i]->schema;
      return nullptr;
   }

   // Get the first struct's schema (for simple files with a single type)
   const psio::dynamic_schema* first() const
   {
      if (!schemas.empty())
         return &schemas[0]->schema;
      return nullptr;
   }

   // Get the last struct's schema (often the root type in capnp files)
   const psio::dynamic_schema* last() const
   {
      if (!schemas.empty())
         return &schemas.back()->schema;
      return nullptr;
   }

   static psio::dynamic_type capnp_tag_to_dynamic(psio::capnp_type_tag tag)
   {
      switch (tag)
      {
         case psio::capnp_type_tag::void_:
            return psio::dynamic_type::t_void;
         case psio::capnp_type_tag::bool_:
            return psio::dynamic_type::t_bool;
         case psio::capnp_type_tag::int8:
            return psio::dynamic_type::t_i8;
         case psio::capnp_type_tag::int16:
            return psio::dynamic_type::t_i16;
         case psio::capnp_type_tag::int32:
            return psio::dynamic_type::t_i32;
         case psio::capnp_type_tag::int64:
            return psio::dynamic_type::t_i64;
         case psio::capnp_type_tag::uint8:
            return psio::dynamic_type::t_u8;
         case psio::capnp_type_tag::uint16:
            return psio::dynamic_type::t_u16;
         case psio::capnp_type_tag::uint32:
            return psio::dynamic_type::t_u32;
         case psio::capnp_type_tag::uint64:
            return psio::dynamic_type::t_u64;
         case psio::capnp_type_tag::float32:
            return psio::dynamic_type::t_f32;
         case psio::capnp_type_tag::float64:
            return psio::dynamic_type::t_f64;
         case psio::capnp_type_tag::text:
            return psio::dynamic_type::t_text;
         case psio::capnp_type_tag::data:
            return psio::dynamic_type::t_data;
         case psio::capnp_type_tag::list:
            return psio::dynamic_type::t_vector;
         case psio::capnp_type_tag::struct_:
            return psio::dynamic_type::t_struct;
         case psio::capnp_type_tag::enum_:
            return psio::dynamic_type::t_u16;  // capnp enums are u16
         case psio::capnp_type_tag::any_ptr:
            return psio::dynamic_type::t_void;
         default:
            return psio::dynamic_type::t_void;
      }
   }

   static uint8_t capnp_tag_byte_size(psio::capnp_type_tag tag)
   {
      switch (tag)
      {
         case psio::capnp_type_tag::bool_:
            return 0;
         case psio::capnp_type_tag::int8:
         case psio::capnp_type_tag::uint8:
            return 1;
         case psio::capnp_type_tag::int16:
         case psio::capnp_type_tag::uint16:
         case psio::capnp_type_tag::enum_:
            return 2;
         case psio::capnp_type_tag::int32:
         case psio::capnp_type_tag::uint32:
         case psio::capnp_type_tag::float32:
            return 4;
         case psio::capnp_type_tag::int64:
         case psio::capnp_type_tag::uint64:
         case psio::capnp_type_tag::float64:
            return 8;
         default:
            return 0;
      }
   }
};

// ============================================================================
// JSON output from dynamic_view
// ============================================================================

static void write_json(std::ostream& out, const psio::dynamic_view<psio::cp>& dv, int indent = 0);

static void write_indent(std::ostream& out, int indent)
{
   for (int i = 0; i < indent; ++i)
      out << "  ";
}

static void write_json_string(std::ostream& out, std::string_view s)
{
   out << '"';
   for (char c : s)
   {
      switch (c)
      {
         case '"':
            out << "\\\"";
            break;
         case '\\':
            out << "\\\\";
            break;
         case '\n':
            out << "\\n";
            break;
         case '\r':
            out << "\\r";
            break;
         case '\t':
            out << "\\t";
            break;
         default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
               char buf[8];
               std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
               out << buf;
            }
            else
               out << c;
            break;
      }
   }
   out << '"';
}

static void write_json(std::ostream& out, const psio::dynamic_view<psio::cp>& dv, int indent)
{
   auto info = dv.type();

   psio::dynamic_type kind = info.kind;
   if (kind == psio::dynamic_type::t_variant)
      kind = info.active_kind;

   switch (kind)
   {
      case psio::dynamic_type::t_void:
         out << "null";
         break;

      case psio::dynamic_type::t_bool:
         out << (static_cast<bool>(dv) ? "true" : "false");
         break;

      case psio::dynamic_type::t_i8:
      case psio::dynamic_type::t_i16:
      case psio::dynamic_type::t_i32:
      case psio::dynamic_type::t_i64:
      {
         // Use the appropriate conversion based on type
         int64_t v = 0;
         switch (kind)
         {
            case psio::dynamic_type::t_i8:
               v = static_cast<int8_t>(dv);
               break;
            case psio::dynamic_type::t_i16:
               v = static_cast<int16_t>(dv);
               break;
            case psio::dynamic_type::t_i32:
               v = static_cast<int32_t>(dv);
               break;
            case psio::dynamic_type::t_i64:
               v = static_cast<int64_t>(dv);
               break;
            default:
               break;
         }
         out << v;
         break;
      }

      case psio::dynamic_type::t_u8:
      case psio::dynamic_type::t_u16:
      case psio::dynamic_type::t_u32:
      case psio::dynamic_type::t_u64:
      {
         uint64_t v = 0;
         switch (kind)
         {
            case psio::dynamic_type::t_u8:
               v = static_cast<uint8_t>(dv);
               break;
            case psio::dynamic_type::t_u16:
               v = static_cast<uint16_t>(dv);
               break;
            case psio::dynamic_type::t_u32:
               v = static_cast<uint32_t>(dv);
               break;
            case psio::dynamic_type::t_u64:
               v = static_cast<uint64_t>(dv);
               break;
            default:
               break;
         }
         // JSON doesn't support u64 natively — quote large values
         if (v > 9007199254740991ULL)  // 2^53 - 1
            out << '"' << v << '"';
         else
            out << v;
         break;
      }

      case psio::dynamic_type::t_f32:
      {
         char buf[32];
         std::snprintf(buf, sizeof(buf), "%.9g", static_cast<float>(dv));
         out << buf;
         break;
      }

      case psio::dynamic_type::t_f64:
      {
         char buf[32];
         std::snprintf(buf, sizeof(buf), "%.17g", static_cast<double>(dv));
         out << buf;
         break;
      }

      case psio::dynamic_type::t_text:
         write_json_string(out, static_cast<std::string_view>(dv));
         break;

      case psio::dynamic_type::t_data:
      {
         // Binary data — output as hex string
         size_t len = dv.size();
         out << '"';
         // Access raw bytes via vector indexing
         for (size_t i = 0; i < len; ++i)
         {
            uint8_t b = static_cast<uint8_t>(dv[i]);
            char    hex[4];
            std::snprintf(hex, sizeof(hex), "%02x", b);
            out << hex;
         }
         out << '"';
         break;
      }

      case psio::dynamic_type::t_vector:
      {
         size_t len = dv.size();
         if (len == 0)
         {
            out << "[]";
         }
         else
         {
            out << "[\n";
            for (size_t i = 0; i < len; ++i)
            {
               write_indent(out, indent + 1);
               write_json(out, dv[i], indent + 1);
               if (i + 1 < len)
                  out << ",";
               out << "\n";
            }
            write_indent(out, indent);
            out << "]";
         }
         break;
      }

      case psio::dynamic_type::t_struct:
      {
         auto names = dv.field_names();
         if (names.empty())
         {
            out << "{}";
         }
         else
         {
            out << "{\n";
            for (size_t i = 0; i < names.size(); ++i)
            {
               write_indent(out, indent + 1);
               write_json_string(out, names[i]);
               out << ": ";
               try
               {
                  auto child = dv[psio::field_name{names[i]}];
                  write_json(out, child, indent + 1);
               }
               catch (const std::exception& e)
               {
                  out << "null";  // field access failed
               }
               if (i + 1 < names.size())
                  out << ",";
               out << "\n";
            }
            write_indent(out, indent);
            out << "}";
         }
         break;
      }

      case psio::dynamic_type::t_variant:
         // Should have been resolved via active_kind above
         out << "null";
         break;
   }
}

// ============================================================================
// Schema summary output
// ============================================================================

static void print_capnp_schema_summary(const psio::capnp_parsed_file& file)
{
   std::cout << "Cap'n Proto schema (file ID: 0x" << std::hex << file.file_id << std::dec
             << ")\n";
   std::cout << "  Structs: " << file.structs.size() << "\n";
   for (const auto& s : file.structs)
   {
      std::cout << "    " << s.name << " (" << s.fields.size() << " fields, " << s.data_words
                << " data words, " << s.ptr_count << " pointers)\n";
      for (const auto& f : s.fields)
      {
         const char* type_name = "unknown";
         switch (f.type.tag)
         {
            case psio::capnp_type_tag::void_:
               type_name = "Void";
               break;
            case psio::capnp_type_tag::bool_:
               type_name = "Bool";
               break;
            case psio::capnp_type_tag::int8:
               type_name = "Int8";
               break;
            case psio::capnp_type_tag::int16:
               type_name = "Int16";
               break;
            case psio::capnp_type_tag::int32:
               type_name = "Int32";
               break;
            case psio::capnp_type_tag::int64:
               type_name = "Int64";
               break;
            case psio::capnp_type_tag::uint8:
               type_name = "UInt8";
               break;
            case psio::capnp_type_tag::uint16:
               type_name = "UInt16";
               break;
            case psio::capnp_type_tag::uint32:
               type_name = "UInt32";
               break;
            case psio::capnp_type_tag::uint64:
               type_name = "UInt64";
               break;
            case psio::capnp_type_tag::float32:
               type_name = "Float32";
               break;
            case psio::capnp_type_tag::float64:
               type_name = "Float64";
               break;
            case psio::capnp_type_tag::text:
               type_name = "Text";
               break;
            case psio::capnp_type_tag::data:
               type_name = "Data";
               break;
            case psio::capnp_type_tag::list:
               type_name = "List";
               break;
            case psio::capnp_type_tag::struct_:
            {
               if (f.type.referenced_type_idx >= 0 &&
                   f.type.referenced_type_idx < static_cast<int32_t>(file.structs.size()))
                  type_name = file.structs[f.type.referenced_type_idx].name.c_str();
               else
                  type_name = "Struct";
               break;
            }
            case psio::capnp_type_tag::enum_:
               type_name = "Enum";
               break;
            case psio::capnp_type_tag::any_ptr:
               type_name = "AnyPointer";
               break;
         }
         std::cout << "      @" << f.ordinal << " " << f.name << " :" << type_name;
         if (f.loc.is_ptr)
            std::cout << " [ptr:" << f.loc.offset << "]";
         else
            std::cout << " [data:" << f.loc.offset << "]";
         std::cout << "\n";
      }
   }
   std::cout << "  Enums: " << file.enums.size() << "\n";
   for (const auto& e : file.enums)
   {
      std::cout << "    " << e.name << " (";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         if (i > 0)
            std::cout << ", ";
         std::cout << e.enumerants[i];
      }
      std::cout << ")\n";
   }
}

static void print_fbs_schema_summary(const psio::fbs_schema& schema)
{
   std::cout << "FlatBuffers schema";
   if (!schema.ns.empty())
      std::cout << " (namespace: " << schema.ns << ")";
   std::cout << "\n";
   if (!schema.root_type.empty())
      std::cout << "  root_type: " << schema.root_type << "\n";
   std::cout << "  Types: " << schema.types.size() << "\n";
   for (const auto& t : schema.types)
   {
      const char* kind_str = "???";
      switch (t.kind)
      {
         case psio::fbs_type_kind::table_:
            kind_str = "table";
            break;
         case psio::fbs_type_kind::struct_:
            kind_str = "struct";
            break;
         case psio::fbs_type_kind::enum_:
            kind_str = "enum";
            break;
         case psio::fbs_type_kind::union_:
            kind_str = "union";
            break;
      }
      std::cout << "    " << kind_str << " " << t.name;
      if (t.kind == psio::fbs_type_kind::table_ || t.kind == psio::fbs_type_kind::struct_)
         std::cout << " (" << t.fields.size() << " fields)";
      else if (t.kind == psio::fbs_type_kind::enum_)
         std::cout << " (" << t.enum_values.size() << " values)";
      else if (t.kind == psio::fbs_type_kind::union_)
         std::cout << " (" << t.union_members.size() << " members)";
      std::cout << "\n";

      if (t.kind == psio::fbs_type_kind::table_ || t.kind == psio::fbs_type_kind::struct_)
      {
         for (const auto& f : t.fields)
         {
            const char* type_str = "???";
            switch (f.type)
            {
               case psio::fbs_base_type::bool_:
                  type_str = "bool";
                  break;
               case psio::fbs_base_type::int8_:
                  type_str = "byte";
                  break;
               case psio::fbs_base_type::uint8_:
                  type_str = "ubyte";
                  break;
               case psio::fbs_base_type::int16_:
                  type_str = "short";
                  break;
               case psio::fbs_base_type::uint16_:
                  type_str = "ushort";
                  break;
               case psio::fbs_base_type::int32_:
                  type_str = "int";
                  break;
               case psio::fbs_base_type::uint32_:
                  type_str = "uint";
                  break;
               case psio::fbs_base_type::int64_:
                  type_str = "long";
                  break;
               case psio::fbs_base_type::uint64_:
                  type_str = "ulong";
                  break;
               case psio::fbs_base_type::float32_:
                  type_str = "float";
                  break;
               case psio::fbs_base_type::float64_:
                  type_str = "double";
                  break;
               case psio::fbs_base_type::string_:
                  type_str = "string";
                  break;
               case psio::fbs_base_type::vector_:
                  type_str = "vector";
                  break;
               case psio::fbs_base_type::table_:
                  type_str = "table";
                  break;
               case psio::fbs_base_type::struct_:
                  type_str = "struct";
                  break;
               case psio::fbs_base_type::enum_:
                  type_str = "enum";
                  break;
               case psio::fbs_base_type::union_:
                  type_str = "union";
                  break;
               default:
                  type_str = "none";
                  break;
            }
            std::cout << "      " << f.name << " :" << type_str;
            if (t.kind == psio::fbs_type_kind::table_)
               std::cout << " [slot:" << f.vtable_slot << "]";
            else
               std::cout << " [offset:" << f.struct_offset << "]";
            std::cout << "\n";
         }
      }
   }
}

// ============================================================================
// Format detection
// ============================================================================

enum class format_id
{
   capnp,
   flatbuf,
   fracpack,
   wit,
   unknown,
};

static format_id parse_format(std::string_view s)
{
   if (s == "capnp" || s == "cp" || s == "capnproto")
      return format_id::capnp;
   if (s == "flatbuf" || s == "fbs" || s == "flatbuffers")
      return format_id::flatbuf;
   if (s == "fracpack" || s == "fp")
      return format_id::fracpack;
   if (s == "wit")
      return format_id::wit;
   return format_id::unknown;
}

static const char* format_name(format_id f)
{
   switch (f)
   {
      case format_id::capnp:
         return "capnp";
      case format_id::flatbuf:
         return "flatbuf";
      case format_id::fracpack:
         return "fracpack";
      case format_id::wit:
         return "wit";
      default:
         return "unknown";
   }
}

// Try to detect schema format from file extension
static format_id detect_schema_format(const std::string& path)
{
   auto ext = std::filesystem::path(path).extension().string();
   if (ext == ".capnp")
      return format_id::capnp;
   if (ext == ".fbs")
      return format_id::flatbuf;
   return format_id::unknown;
}

// ============================================================================
// Commands
// ============================================================================

[[noreturn]] static void usage()
{
   std::cerr << R"(psio-tool — universal serialization format utility

Usage:
  psio-tool info
      Print supported formats and version.

  psio-tool inspect <format> <schema-file> <data-file>
      Read binary data and print all fields as JSON.
      format: capnp, flatbuf, fracpack, wit
      data-file: path to binary file, or "-" for stdin

  psio-tool validate <format> <schema-file> <data-file>
      Validate binary data against schema.
      Exit 0 if valid, exit 1 with error details on stderr.

  psio-tool convert <src-format> <dst-format> <schema-file> <data-file>
      Convert between formats (output to stdout).

  psio-tool schema <format> <schema-file>
      Parse and summarize a schema file.

  psio-tool schema <src-format> <schema-file> --to <dst-format>
      Convert schema IDL between formats.

  psio-tool codegen <schema-format> <schema-file> --lang <language> [--output <file>]
      Generate type definitions from a schema file.
      Languages: cpp, rust, go, typescript, python, zig
      C++/Rust output uses PSIO_REFLECT / Pack/Unpack derives.
      Go/TS/Python/Zig output uses native type idioms.

Examples:
  psio-tool schema capnp message.capnp
  psio-tool schema fbs monster.fbs
  psio-tool inspect capnp message.capnp data.bin
  psio-tool validate capnp message.capnp data.bin
  psio-tool codegen capnp schema.capnp --lang cpp
  psio-tool codegen fbs schema.fbs --lang rust --output types.rs
  psio-tool codegen capnp schema.capnp --lang go
  psio-tool codegen capnp schema.capnp --lang typescript
  psio-tool codegen capnp schema.capnp --lang python
  psio-tool codegen fbs schema.fbs --lang zig
)";
   std::exit(1);
}

static int cmd_info()
{
   std::cout << "psio-tool v0.1.0\n\n";
   std::cout << "Supported formats:\n";
   std::cout << "  capnp    (aliases: cp, capnproto)   — Cap'n Proto\n";
   std::cout << "  flatbuf  (aliases: fbs, flatbuffers) — FlatBuffers\n";
   std::cout << "  fracpack (aliases: fp)               — fracpack\n";
   std::cout << "  wit                                  — WebAssembly Interface Types\n";
   std::cout << "\n";
   std::cout << "Capabilities:\n";
   std::cout << "  Schema parsing:      capnp (.capnp), flatbuf (.fbs)\n";
   std::cout << "  Binary inspection:   capnp (via dynamic_view)\n";
   std::cout << "  Binary validation:   capnp\n";
   std::cout << "  Format conversion:   capnp -> json\n";
   return 0;
}

static int cmd_schema(int argc, char** argv)
{
   // psio-tool schema <format> <schema-file> [--to <dst-format>]
   if (argc < 4)
   {
      std::cerr << "error: schema command requires: <format> <schema-file>\n";
      usage();
   }

   auto        fmt       = parse_format(argv[2]);
   std::string schema_path = argv[3];

   // Check for --to flag
   format_id dst_fmt = format_id::unknown;
   if (argc >= 6 && std::string_view(argv[4]) == "--to")
   {
      dst_fmt = parse_format(argv[5]);
      if (dst_fmt == format_id::unknown)
      {
         std::cerr << "error: unknown destination format: " << argv[5] << "\n";
         return 1;
      }
   }

   // Auto-detect format from file extension if not specified
   if (fmt == format_id::unknown)
      fmt = detect_schema_format(schema_path);
   if (fmt == format_id::unknown)
   {
      std::cerr << "error: unknown format '" << argv[2]
                << "'. Use: capnp, flatbuf, fracpack, or wit\n";
      return 1;
   }

   std::string schema_text = read_file(schema_path);

   try
   {
      if (fmt == format_id::capnp)
      {
         auto file = psio::capnp_parse(schema_text);

         if (dst_fmt != format_id::unknown)
         {
            std::cerr << "error: schema conversion from capnp to " << format_name(dst_fmt)
                      << " not yet implemented\n";
            return 1;
         }

         print_capnp_schema_summary(file);
      }
      else if (fmt == format_id::flatbuf)
      {
         auto schema = psio::fbs_parse(schema_text);

         if (dst_fmt != format_id::unknown)
         {
            std::cerr << "error: schema conversion from flatbuf to " << format_name(dst_fmt)
                      << " not yet implemented\n";
            return 1;
         }

         print_fbs_schema_summary(schema);
      }
      else
      {
         std::cerr << "error: schema parsing for " << format_name(fmt) << " not yet implemented\n";
         return 1;
      }
   }
   catch (const psio::capnp_parse_error& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }
   catch (const psio::fbs_parse_error& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }
   catch (const std::exception& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }

   return 0;
}

static int cmd_validate(int argc, char** argv)
{
   // psio-tool validate <format> <schema-file> <data-file>
   if (argc < 5)
   {
      std::cerr << "error: validate command requires: <format> <schema-file> <data-file>\n";
      usage();
   }

   auto        fmt         = parse_format(argv[2]);
   std::string schema_path = argv[3];
   std::string data_path   = argv[4];

   if (fmt == format_id::unknown)
      fmt = detect_schema_format(schema_path);
   if (fmt == format_id::unknown)
   {
      std::cerr << "error: unknown format '" << argv[2] << "'\n";
      return 1;
   }

   std::string          schema_text = read_file(schema_path);
   std::vector<uint8_t> data        = read_binary_file(data_path);

   try
   {
      if (fmt == format_id::capnp)
      {
         // Parse schema to verify it's valid
         auto file = psio::capnp_parse(schema_text);
         if (file.structs.empty())
         {
            std::cerr << "error: schema contains no struct definitions\n";
            return 1;
         }

         // Validate binary data as a capnp message
         if (data.size() < 8)
         {
            std::cerr << "error: data too small for capnp message (need at least 8 bytes, got "
                      << data.size() << ")\n";
            return 1;
         }

         bool valid = psio::capnp_validate(data.data(), data.size());
         if (valid)
         {
            std::cout << "valid\n";
            return 0;
         }
         else
         {
            std::cerr << "error: capnp validation failed — message structure is invalid\n";
            return 1;
         }
      }
      else
      {
         std::cerr << "error: validation for " << format_name(fmt) << " not yet implemented\n";
         return 1;
      }
   }
   catch (const std::exception& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }
}

static int cmd_inspect(int argc, char** argv)
{
   // psio-tool inspect <format> <schema-file> <data-file> [--type <TypeName>]
   if (argc < 5)
   {
      std::cerr << "error: inspect command requires: <format> <schema-file> <data-file>\n";
      usage();
   }

   auto        fmt         = parse_format(argv[2]);
   std::string schema_path = argv[3];
   std::string data_path   = argv[4];

   // Optional --type flag
   std::string type_name;
   for (int i = 5; i < argc - 1; ++i)
   {
      if (std::string_view(argv[i]) == "--type")
         type_name = argv[i + 1];
   }

   if (fmt == format_id::unknown)
      fmt = detect_schema_format(schema_path);
   if (fmt == format_id::unknown)
   {
      std::cerr << "error: unknown format '" << argv[2] << "'\n";
      return 1;
   }

   std::string          schema_text = read_file(schema_path);
   std::vector<uint8_t> data        = read_binary_file(data_path);

   try
   {
      if (fmt == format_id::capnp)
      {
         auto file = psio::capnp_parse(schema_text);
         if (file.structs.empty())
         {
            std::cerr << "error: schema contains no struct definitions\n";
            return 1;
         }

         // Build runtime schemas
         capnp_runtime_schema runtime;
         runtime.build(file);

         // Find the target schema
         const psio::dynamic_schema* schema = nullptr;
         if (!type_name.empty())
         {
            schema = runtime.find(type_name, file);
            if (!schema)
            {
               std::cerr << "error: type '" << type_name << "' not found in schema\n";
               std::cerr << "available types:";
               for (const auto& s : file.structs)
                  std::cerr << " " << s.name;
               std::cerr << "\n";
               return 1;
            }
         }
         else
         {
            // Default: use the last struct (typically the root/main type)
            schema = runtime.last();
         }

         // Validate first
         if (data.size() < 8)
         {
            std::cerr << "error: data too small for capnp message\n";
            return 1;
         }
         if (!psio::capnp_validate(data.data(), data.size()))
         {
            std::cerr << "warning: capnp validation failed, attempting to read anyway\n";
         }

         // Resolve root pointer
         const uint8_t* msg = data.data();
         auto           root =
             psio::capnp_detail::resolve_struct_ptr(msg + 8);  // skip segment table

         // Create dynamic view
         psio::dynamic_view<psio::cp> dv(root, schema);

         // Output JSON
         write_json(std::cout, dv);
         std::cout << "\n";
         return 0;
      }
      else if (fmt == format_id::flatbuf)
      {
         auto fbs = psio::fbs_parse(schema_text);
         if (fbs.types.empty())
         {
            std::cerr << "error: schema contains no type definitions\n";
            return 1;
         }

         // Print schema summary since binary inspection is not yet supported
         std::cerr << "note: FlatBuffer binary inspection requires format-specific "
                      "dynamic_view (not yet implemented).\n";
         std::cerr << "      Showing schema summary instead:\n\n";
         print_fbs_schema_summary(fbs);
         return 1;
      }
      else
      {
         std::cerr << "error: inspect for " << format_name(fmt) << " not yet implemented\n";
         return 1;
      }
   }
   catch (const psio::capnp_parse_error& e)
   {
      std::cerr << "schema parse error: " << e.what() << "\n";
      return 1;
   }
   catch (const psio::fbs_parse_error& e)
   {
      std::cerr << "schema parse error: " << e.what() << "\n";
      return 1;
   }
   catch (const std::exception& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }
}

static int cmd_convert(int argc, char** argv)
{
   // psio-tool convert <src-format> <dst-format> <schema-file> <data-file>
   //   Currently supported: capnp -> json
   if (argc < 6)
   {
      std::cerr
          << "error: convert command requires: <src-format> <dst-format> <schema-file> <data-file>\n";
      usage();
   }

   auto        src_fmt     = parse_format(argv[2]);
   auto        dst_fmt_str = std::string_view(argv[3]);
   std::string schema_path = argv[4];
   std::string data_path   = argv[5];

   // Allow "json" as destination even though it's not a wire format
   bool dst_is_json = (dst_fmt_str == "json" || dst_fmt_str == "JSON");

   if (src_fmt == format_id::unknown)
   {
      src_fmt = detect_schema_format(schema_path);
   }
   if (src_fmt == format_id::unknown)
   {
      std::cerr << "error: unknown source format: " << argv[2] << "\n";
      return 1;
   }
   if (!dst_is_json)
   {
      auto dst_fmt = parse_format(argv[3]);
      if (dst_fmt == format_id::unknown)
      {
         std::cerr << "error: unknown destination format: " << argv[3]
                   << " (currently only 'json' is supported as destination)\n";
         return 1;
      }
      std::cerr << "error: format conversion from " << format_name(src_fmt) << " to "
                << format_name(dst_fmt) << " not yet implemented (use 'json' as destination)\n";
      return 1;
   }

   // Optional --type flag
   std::string type_name;
   for (int i = 6; i < argc - 1; ++i)
   {
      if (std::string_view(argv[i]) == "--type")
         type_name = argv[i + 1];
   }

   std::string          schema_text = read_file(schema_path);
   std::vector<uint8_t> data        = read_binary_file(data_path);

   try
   {
      if (src_fmt == format_id::capnp)
      {
         auto file = psio::capnp_parse(schema_text);
         if (file.structs.empty())
         {
            std::cerr << "error: schema contains no struct definitions\n";
            return 1;
         }

         // Build runtime schemas
         capnp_runtime_schema runtime;
         runtime.build(file);

         // Find the target schema
         const psio::dynamic_schema* schema = nullptr;
         if (!type_name.empty())
         {
            schema = runtime.find(type_name, file);
            if (!schema)
            {
               std::cerr << "error: type '" << type_name << "' not found in schema\n";
               std::cerr << "available types:";
               for (const auto& s : file.structs)
                  std::cerr << " " << s.name;
               std::cerr << "\n";
               return 1;
            }
         }
         else
         {
            // Default: use the last struct (typically the root/main type)
            schema = runtime.last();
         }

         if (data.size() < 8)
         {
            std::cerr << "error: data too small for capnp message (need at least 8 bytes, got "
                      << data.size() << ")\n";
            return 1;
         }
         if (!psio::capnp_validate(data.data(), data.size()))
         {
            std::cerr << "warning: capnp validation failed, attempting to read anyway\n";
         }

         // Resolve root pointer
         const uint8_t* msg  = data.data();
         auto           root = psio::capnp_detail::resolve_struct_ptr(msg + 8);

         // Create dynamic view and write JSON to stdout
         psio::dynamic_view<psio::cp> dv(root, schema);
         write_json(std::cout, dv);
         std::cout << "\n";
         return 0;
      }
      else
      {
         std::cerr << "error: conversion from " << format_name(src_fmt)
                   << " to json not yet implemented\n";
         return 1;
      }
   }
   catch (const psio::capnp_parse_error& e)
   {
      std::cerr << "schema parse error: " << e.what() << "\n";
      return 1;
   }
   catch (const std::exception& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }
}

// ============================================================================
// Code generation — C++ and Rust output from parsed schemas
// ============================================================================

enum class codegen_lang
{
   cpp,
   rust,
   go,
   typescript,
   python,
   zig,
};

static codegen_lang parse_lang(std::string_view s)
{
   if (s == "cpp" || s == "c++" || s == "cxx")
      return codegen_lang::cpp;
   if (s == "rust" || s == "rs")
      return codegen_lang::rust;
   if (s == "go" || s == "golang")
      return codegen_lang::go;
   if (s == "ts" || s == "typescript" || s == "js" || s == "javascript")
      return codegen_lang::typescript;
   if (s == "py" || s == "python")
      return codegen_lang::python;
   if (s == "zig")
      return codegen_lang::zig;
   std::cerr << "error: unknown language '" << s << "'. Use: cpp, rust, go, typescript, python, zig\n";
   std::exit(1);
}

// --- capnp type → C++ type string ---
static std::string capnp_tag_to_cpp(psio::capnp_type_tag tag)
{
   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "void";
      case psio::capnp_type_tag::bool_:
         return "bool";
      case psio::capnp_type_tag::int8:
         return "int8_t";
      case psio::capnp_type_tag::int16:
         return "int16_t";
      case psio::capnp_type_tag::int32:
         return "int32_t";
      case psio::capnp_type_tag::int64:
         return "int64_t";
      case psio::capnp_type_tag::uint8:
         return "uint8_t";
      case psio::capnp_type_tag::uint16:
         return "uint16_t";
      case psio::capnp_type_tag::uint32:
         return "uint32_t";
      case psio::capnp_type_tag::uint64:
         return "uint64_t";
      case psio::capnp_type_tag::float32:
         return "float";
      case psio::capnp_type_tag::float64:
         return "double";
      case psio::capnp_type_tag::text:
         return "std::string";
      case psio::capnp_type_tag::data:
         return "std::vector<uint8_t>";
      case psio::capnp_type_tag::any_ptr:
         return "void*";
      default:
         return "/* unknown */";
   }
}

// --- capnp type → Rust type string ---
static std::string capnp_tag_to_rust(psio::capnp_type_tag tag)
{
   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "()";
      case psio::capnp_type_tag::bool_:
         return "bool";
      case psio::capnp_type_tag::int8:
         return "i8";
      case psio::capnp_type_tag::int16:
         return "i16";
      case psio::capnp_type_tag::int32:
         return "i32";
      case psio::capnp_type_tag::int64:
         return "i64";
      case psio::capnp_type_tag::uint8:
         return "u8";
      case psio::capnp_type_tag::uint16:
         return "u16";
      case psio::capnp_type_tag::uint32:
         return "u32";
      case psio::capnp_type_tag::uint64:
         return "u64";
      case psio::capnp_type_tag::float32:
         return "f32";
      case psio::capnp_type_tag::float64:
         return "f64";
      case psio::capnp_type_tag::text:
         return "String";
      case psio::capnp_type_tag::data:
         return "Vec<u8>";
      case psio::capnp_type_tag::any_ptr:
         return "Vec<u8>";
      default:
         return "/* unknown */";
   }
}

// --- fbs type → C++ type string ---
static std::string fbs_type_to_cpp(psio::fbs_base_type type)
{
   switch (type)
   {
      case psio::fbs_base_type::bool_:
         return "bool";
      case psio::fbs_base_type::int8_:
         return "int8_t";
      case psio::fbs_base_type::uint8_:
         return "uint8_t";
      case psio::fbs_base_type::int16_:
         return "int16_t";
      case psio::fbs_base_type::uint16_:
         return "uint16_t";
      case psio::fbs_base_type::int32_:
         return "int32_t";
      case psio::fbs_base_type::uint32_:
         return "uint32_t";
      case psio::fbs_base_type::int64_:
         return "int64_t";
      case psio::fbs_base_type::uint64_:
         return "uint64_t";
      case psio::fbs_base_type::float32_:
         return "float";
      case psio::fbs_base_type::float64_:
         return "double";
      case psio::fbs_base_type::string_:
         return "std::string";
      default:
         return "/* unknown */";
   }
}

// --- fbs type → Rust type string ---
static std::string fbs_type_to_rust(psio::fbs_base_type type)
{
   switch (type)
   {
      case psio::fbs_base_type::bool_:
         return "bool";
      case psio::fbs_base_type::int8_:
         return "i8";
      case psio::fbs_base_type::uint8_:
         return "u8";
      case psio::fbs_base_type::int16_:
         return "i16";
      case psio::fbs_base_type::uint16_:
         return "u16";
      case psio::fbs_base_type::int32_:
         return "i32";
      case psio::fbs_base_type::uint32_:
         return "u32";
      case psio::fbs_base_type::int64_:
         return "i64";
      case psio::fbs_base_type::uint64_:
         return "u64";
      case psio::fbs_base_type::float32_:
         return "f32";
      case psio::fbs_base_type::float64_:
         return "f64";
      case psio::fbs_base_type::string_:
         return "String";
      default:
         return "/* unknown */";
   }
}

// Decode the encoded element_type_idx back to a capnp_type_tag for builtins
static psio::capnp_type_tag decode_list_element_tag(int32_t idx)
{
   if (idx < 0)
      return static_cast<psio::capnp_type_tag>(-(idx + 1));
   return psio::capnp_type_tag::struct_;  // positive = struct index
}

// Returns true for scalar types that should have {} default initializers in C++
static bool capnp_is_scalar(psio::capnp_type_tag tag)
{
   switch (tag)
   {
      case psio::capnp_type_tag::bool_:
      case psio::capnp_type_tag::int8:
      case psio::capnp_type_tag::int16:
      case psio::capnp_type_tag::int32:
      case psio::capnp_type_tag::int64:
      case psio::capnp_type_tag::uint8:
      case psio::capnp_type_tag::uint16:
      case psio::capnp_type_tag::uint32:
      case psio::capnp_type_tag::uint64:
      case psio::capnp_type_tag::float32:
      case psio::capnp_type_tag::float64:
         return true;
      default:
         return false;
   }
}

static bool fbs_is_scalar(psio::fbs_base_type type)
{
   switch (type)
   {
      case psio::fbs_base_type::bool_:
      case psio::fbs_base_type::int8_:
      case psio::fbs_base_type::uint8_:
      case psio::fbs_base_type::int16_:
      case psio::fbs_base_type::uint16_:
      case psio::fbs_base_type::int32_:
      case psio::fbs_base_type::uint32_:
      case psio::fbs_base_type::int64_:
      case psio::fbs_base_type::uint64_:
      case psio::fbs_base_type::float32_:
      case psio::fbs_base_type::float64_:
         return true;
      default:
         return false;
   }
}

// Convert camelCase to snake_case for Rust field names
static std::string to_snake_case(const std::string& s)
{
   std::string out;
   for (size_t i = 0; i < s.size(); ++i)
   {
      char c = s[i];
      if (std::isupper(c))
      {
         if (i > 0)
            out += '_';
         out += static_cast<char>(std::tolower(c));
      }
      else
      {
         out += c;
      }
   }
   return out;
}

// Get the full C++ type string for a capnp field (handles lists, structs, enums)
static std::string capnp_field_type_cpp(const psio::capnp_parsed_field& field,
                                        const psio::capnp_parsed_file&  file)
{
   auto tag = field.type.tag;

   if (tag == psio::capnp_type_tag::struct_)
   {
      int32_t idx = field.type.referenced_type_idx;
      if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
         return file.structs[idx].name;
      return "/* unknown struct */";
   }

   if (tag == psio::capnp_type_tag::enum_)
   {
      int32_t idx = field.type.referenced_type_idx;
      if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
         return file.enums[idx].name;
      return "/* unknown enum */";
   }

   if (tag == psio::capnp_type_tag::list)
   {
      int32_t elem_idx = field.type.element_type_idx;
      auto    elem_tag = decode_list_element_tag(elem_idx);
      if (elem_tag == psio::capnp_type_tag::struct_)
      {
         // Positive elem_idx is a struct index
         if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
            return "std::vector<" + file.structs[elem_idx].name + ">";
         return "std::vector</* unknown struct */>";
      }
      return "std::vector<" + capnp_tag_to_cpp(elem_tag) + ">";
   }

   return capnp_tag_to_cpp(tag);
}

// Get the full Rust type string for a capnp field
static std::string capnp_field_type_rust(const psio::capnp_parsed_field& field,
                                         const psio::capnp_parsed_file&  file)
{
   auto tag = field.type.tag;

   if (tag == psio::capnp_type_tag::struct_)
   {
      int32_t idx = field.type.referenced_type_idx;
      if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
         return file.structs[idx].name;
      return "/* unknown struct */";
   }

   if (tag == psio::capnp_type_tag::enum_)
   {
      int32_t idx = field.type.referenced_type_idx;
      if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
         return file.enums[idx].name;
      return "/* unknown enum */";
   }

   if (tag == psio::capnp_type_tag::list)
   {
      int32_t elem_idx = field.type.element_type_idx;
      auto    elem_tag = decode_list_element_tag(elem_idx);
      if (elem_tag == psio::capnp_type_tag::struct_)
      {
         if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
            return "Vec<" + file.structs[elem_idx].name + ">";
         return "Vec</* unknown struct */>";
      }
      return "Vec<" + capnp_tag_to_rust(elem_tag) + ">";
   }

   return capnp_tag_to_rust(tag);
}

// Get the full C++ type string for a fbs field
static std::string fbs_field_type_cpp(const psio::fbs_field_def& field,
                                      const psio::fbs_schema&    schema)
{
   if (field.type == psio::fbs_base_type::table_ || field.type == psio::fbs_base_type::struct_)
   {
      if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
         return schema.types[field.type_idx].name;
      return "/* unknown type */";
   }

   if (field.type == psio::fbs_base_type::enum_)
   {
      if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
         return schema.types[field.type_idx].name;
      return "/* unknown enum */";
   }

   if (field.type == psio::fbs_base_type::vector_)
   {
      if (field.elem_type == psio::fbs_base_type::table_ ||
          field.elem_type == psio::fbs_base_type::struct_)
      {
         if (field.elem_type_idx >= 0 &&
             field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
            return "std::vector<" + schema.types[field.elem_type_idx].name + ">";
         return "std::vector</* unknown type */>";
      }
      return "std::vector<" + fbs_type_to_cpp(field.elem_type) + ">";
   }

   return fbs_type_to_cpp(field.type);
}

// Get the full Rust type string for a fbs field
static std::string fbs_field_type_rust(const psio::fbs_field_def& field,
                                       const psio::fbs_schema&    schema)
{
   if (field.type == psio::fbs_base_type::table_ || field.type == psio::fbs_base_type::struct_)
   {
      if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
         return schema.types[field.type_idx].name;
      return "/* unknown type */";
   }

   if (field.type == psio::fbs_base_type::enum_)
   {
      if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
         return schema.types[field.type_idx].name;
      return "/* unknown enum */";
   }

   if (field.type == psio::fbs_base_type::vector_)
   {
      if (field.elem_type == psio::fbs_base_type::table_ ||
          field.elem_type == psio::fbs_base_type::struct_)
      {
         if (field.elem_type_idx >= 0 &&
             field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
            return "Vec<" + schema.types[field.elem_type_idx].name + ">";
         return "Vec</* unknown type */>";
      }
      return "Vec<" + fbs_type_to_rust(field.elem_type) + ">";
   }

   return fbs_type_to_rust(field.type);
}

// --- Generate C++ code from a parsed capnp file ---
static std::string codegen_capnp_cpp(const psio::capnp_parsed_file& file,
                                     const std::string&              source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "#pragma once\n";
   out << "#include <cstdint>\n";
   out << "#include <string>\n";
   out << "#include <vector>\n";
   out << "#include <optional>\n";
   out << "#include <psio/reflect.hpp>\n";
   out << "\n";

   // Emit enums
   for (const auto& e : file.enums)
   {
      out << "enum class " << e.name << " : uint16_t {\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         out << "    " << e.enumerants[i] << " = " << i;
         if (i + 1 < e.enumerants.size())
            out << ",";
         out << "\n";
      }
      out << "};\n";
      out << "PSIO_REFLECT_ENUM(" << e.name;
      for (const auto& en : e.enumerants)
         out << ", " << en;
      out << ")\n\n";
   }

   // Emit structs in declaration order (dependency order)
   for (const auto& s : file.structs)
   {
      out << "struct " << s.name << " {\n";
      for (const auto& f : s.fields)
      {
         std::string cpp_type = capnp_field_type_cpp(f, file);
         out << "    " << cpp_type << " " << f.name;
         if (capnp_is_scalar(f.type.tag))
            out << "{}";
         out << ";\n";
      }
      out << "};\n";
      out << "PSIO_REFLECT(" << s.name;
      for (const auto& f : s.fields)
         out << ", " << f.name;
      out << ")\n\n";
   }

   return out.str();
}

// --- Generate Rust code from a parsed capnp file ---
static std::string codegen_capnp_rust(const psio::capnp_parsed_file& file,
                                      const std::string&              source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "use psio::{Pack, Unpack};\n";
   out << "\n";

   // Emit enums
   for (const auto& e : file.enums)
   {
      out << "#[derive(Pack, Unpack, Clone, Copy, Debug, PartialEq, Eq)]\n";
      out << "#[fracpack(fracpack_mod = \"psio\")]\n";
      out << "#[repr(u16)]\n";
      out << "pub enum " << e.name << " {\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         out << "    " << e.enumerants[i] << " = " << i;
         if (i + 1 < e.enumerants.size())
            out << ",";
         out << "\n";
      }
      out << "}\n\n";
   }

   // Emit structs
   for (const auto& s : file.structs)
   {
      out << "#[derive(Pack, Unpack, Clone, Debug, PartialEq)]\n";
      out << "#[fracpack(fracpack_mod = \"psio\")]\n";
      out << "pub struct " << s.name << " {\n";
      for (const auto& f : s.fields)
      {
         std::string rust_type = capnp_field_type_rust(f, file);
         std::string rust_name = to_snake_case(f.name);
         out << "    pub " << rust_name << ": " << rust_type << ",\n";
      }
      out << "}\n\n";
   }

   return out.str();
}

// --- Generate C++ code from a parsed fbs schema ---
static std::string codegen_fbs_cpp(const psio::fbs_schema& schema,
                                   const std::string&      source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "#pragma once\n";
   out << "#include <cstdint>\n";
   out << "#include <string>\n";
   out << "#include <vector>\n";
   out << "#include <optional>\n";
   out << "#include <psio/reflect.hpp>\n";
   out << "\n";

   if (!schema.ns.empty())
   {
      // Convert dotted namespace to nested C++ namespaces
      std::string ns = schema.ns;
      size_t      pos;
      while ((pos = ns.find('.')) != std::string::npos)
      {
         out << "namespace " << ns.substr(0, pos) << " {\n";
         ns = ns.substr(pos + 1);
      }
      out << "namespace " << ns << " {\n";
      out << "\n";
   }

   // Emit enums first, then structs/tables
   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         std::string underlying = fbs_type_to_cpp(t.underlying_type);
         out << "enum class " << t.name << " : " << underlying << " {\n";
         for (size_t i = 0; i < t.enum_values.size(); ++i)
         {
            out << "    " << t.enum_values[i].name << " = " << t.enum_values[i].value;
            if (i + 1 < t.enum_values.size())
               out << ",";
            out << "\n";
         }
         out << "};\n";
         out << "PSIO_REFLECT_ENUM(" << t.name;
         for (const auto& ev : t.enum_values)
            out << ", " << ev.name;
         out << ")\n\n";
      }
   }

   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::table_ || t.kind == psio::fbs_type_kind::struct_)
      {
         out << "struct " << t.name << " {\n";
         for (const auto& f : t.fields)
         {
            std::string cpp_type = fbs_field_type_cpp(f, schema);
            out << "    " << cpp_type << " " << f.name;
            if (fbs_is_scalar(f.type))
               out << "{}";
            out << ";\n";
         }
         out << "};\n";
         out << "PSIO_REFLECT(" << t.name;
         for (const auto& f : t.fields)
            out << ", " << f.name;
         out << ")\n\n";
      }
   }

   if (!schema.ns.empty())
   {
      // Close namespace blocks
      std::string ns    = schema.ns;
      int         depth = 1;
      for (char c : ns)
         if (c == '.')
            depth++;
      for (int i = 0; i < depth; ++i)
         out << "} // namespace\n";
      out << "\n";
   }

   return out.str();
}

// --- Generate Rust code from a parsed fbs schema ---
static std::string codegen_fbs_rust(const psio::fbs_schema& schema,
                                    const std::string&      source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "use psio::{Pack, Unpack};\n";
   out << "\n";

   if (!schema.ns.empty())
   {
      // Convert dotted namespace to nested Rust modules
      std::string ns = schema.ns;
      size_t      pos;
      while ((pos = ns.find('.')) != std::string::npos)
      {
         out << "pub mod " << ns.substr(0, pos) << " {\n";
         out << "use psio::{Pack, Unpack};\n";
         ns = ns.substr(pos + 1);
      }
      out << "pub mod " << ns << " {\n";
      out << "use psio::{Pack, Unpack};\n\n";
   }

   // Emit enums
   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         std::string underlying = fbs_type_to_rust(t.underlying_type);
         out << "#[derive(Pack, Unpack, Clone, Copy, Debug, PartialEq, Eq)]\n";
         out << "#[fracpack(fracpack_mod = \"psio\")]\n";
         out << "#[repr(" << underlying << ")]\n";
         out << "pub enum " << t.name << " {\n";
         for (size_t i = 0; i < t.enum_values.size(); ++i)
         {
            out << "    " << t.enum_values[i].name << " = " << t.enum_values[i].value;
            if (i + 1 < t.enum_values.size())
               out << ",";
            out << "\n";
         }
         out << "}\n\n";
      }
   }

   // Emit structs/tables
   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::table_ || t.kind == psio::fbs_type_kind::struct_)
      {
         out << "#[derive(Pack, Unpack, Clone, Debug, PartialEq)]\n";
         out << "#[fracpack(fracpack_mod = \"psio\")]\n";
         out << "pub struct " << t.name << " {\n";
         for (const auto& f : t.fields)
         {
            std::string rust_type = fbs_field_type_rust(f, schema);
            std::string rust_name = to_snake_case(f.name);
            out << "    pub " << rust_name << ": " << rust_type << ",\n";
         }
         out << "}\n\n";
      }
   }

   if (!schema.ns.empty())
   {
      // Close module blocks
      std::string ns    = schema.ns;
      int         depth = 1;
      for (char c : ns)
         if (c == '.')
            depth++;
      for (int i = 0; i < depth; ++i)
         out << "} // mod\n";
      out << "\n";
   }

   return out.str();
}

// ============================================================================
// Code generation — Go, TypeScript, Python, Zig output from parsed schemas
// ============================================================================

// --- Naming helpers ---

// PascalCase for Go exported names (capitalize first letter)
static std::string go_pascal_case(const std::string& s)
{
   if (s.empty())
      return s;
   std::string result = s;
   result[0]          = static_cast<char>(std::toupper(result[0]));
   return result;
}

// camelCase for TypeScript fields (lowercase first letter)
static std::string ts_camel_case(const std::string& s)
{
   if (s.empty())
      return s;
   std::string result = s;
   result[0]          = static_cast<char>(std::tolower(result[0]));
   return result;
}

// --- capnp type → Go type string ---
static std::string capnp_field_type_go(const psio::capnp_parsed_field& field,
                                       const psio::capnp_parsed_file&  file)
{
   auto tag = field.type.tag;

   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "struct{}";
      case psio::capnp_type_tag::bool_:
         return "bool";
      case psio::capnp_type_tag::int8:
         return "int8";
      case psio::capnp_type_tag::int16:
         return "int16";
      case psio::capnp_type_tag::int32:
         return "int32";
      case psio::capnp_type_tag::int64:
         return "int64";
      case psio::capnp_type_tag::uint8:
         return "uint8";
      case psio::capnp_type_tag::uint16:
         return "uint16";
      case psio::capnp_type_tag::uint32:
         return "uint32";
      case psio::capnp_type_tag::uint64:
         return "uint64";
      case psio::capnp_type_tag::float32:
         return "float32";
      case psio::capnp_type_tag::float64:
         return "float64";
      case psio::capnp_type_tag::text:
         return "string";
      case psio::capnp_type_tag::data:
         return "[]byte";
      case psio::capnp_type_tag::any_ptr:
         return "interface{}";
      case psio::capnp_type_tag::struct_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
            return go_pascal_case(file.structs[idx].name);
         return "interface{}";
      }
      case psio::capnp_type_tag::enum_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
            return go_pascal_case(file.enums[idx].name);
         return "uint16";
      }
      case psio::capnp_type_tag::list:
      {
         int32_t elem_idx = field.type.element_type_idx;
         auto    elem_tag = decode_list_element_tag(elem_idx);
         if (elem_tag == psio::capnp_type_tag::struct_)
         {
            if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
               return "[]" + go_pascal_case(file.structs[elem_idx].name);
            return "[]interface{}";
         }
         // For primitive list elements, create a fake field to recurse
         psio::capnp_parsed_field fake;
         fake.type.tag = elem_tag;
         return "[]" + capnp_field_type_go(fake, file);
      }
      default:
         return "interface{}";
   }
}

// --- capnp type → TypeScript type string ---
static std::string capnp_field_type_ts(const psio::capnp_parsed_field& field,
                                       const psio::capnp_parsed_file&  file,
                                       std::string*                    comment = nullptr)
{
   auto tag = field.type.tag;

   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "void";
      case psio::capnp_type_tag::bool_:
         return "boolean";
      case psio::capnp_type_tag::int8:
      case psio::capnp_type_tag::int16:
      case psio::capnp_type_tag::int32:
      case psio::capnp_type_tag::uint8:
      case psio::capnp_type_tag::uint16:
      case psio::capnp_type_tag::uint32:
      case psio::capnp_type_tag::float32:
      case psio::capnp_type_tag::float64:
         return "number";
      case psio::capnp_type_tag::int64:
         if (comment)
            *comment = "int64 - use BigInt for values > 2^53";
         return "number";
      case psio::capnp_type_tag::uint64:
         if (comment)
            *comment = "uint64 - use BigInt for values > 2^53";
         return "number";
      case psio::capnp_type_tag::text:
         return "string";
      case psio::capnp_type_tag::data:
         return "Uint8Array";
      case psio::capnp_type_tag::any_ptr:
         return "unknown";
      case psio::capnp_type_tag::struct_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
            return file.structs[idx].name;
         return "unknown";
      }
      case psio::capnp_type_tag::enum_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
            return file.enums[idx].name;
         return "number";
      }
      case psio::capnp_type_tag::list:
      {
         int32_t elem_idx = field.type.element_type_idx;
         auto    elem_tag = decode_list_element_tag(elem_idx);
         if (elem_tag == psio::capnp_type_tag::struct_)
         {
            if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
               return file.structs[elem_idx].name + "[]";
            return "unknown[]";
         }
         psio::capnp_parsed_field fake;
         fake.type.tag = elem_tag;
         return capnp_field_type_ts(fake, file) + "[]";
      }
      default:
         return "unknown";
   }
}

// --- capnp type → Python type string ---
static std::string capnp_field_type_python(const psio::capnp_parsed_field& field,
                                           const psio::capnp_parsed_file&  file)
{
   auto tag = field.type.tag;

   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "None";
      case psio::capnp_type_tag::bool_:
         return "bool";
      case psio::capnp_type_tag::int8:
      case psio::capnp_type_tag::int16:
      case psio::capnp_type_tag::int32:
      case psio::capnp_type_tag::int64:
      case psio::capnp_type_tag::uint8:
      case psio::capnp_type_tag::uint16:
      case psio::capnp_type_tag::uint32:
      case psio::capnp_type_tag::uint64:
         return "int";
      case psio::capnp_type_tag::float32:
      case psio::capnp_type_tag::float64:
         return "float";
      case psio::capnp_type_tag::text:
         return "str";
      case psio::capnp_type_tag::data:
         return "bytes";
      case psio::capnp_type_tag::any_ptr:
         return "object";
      case psio::capnp_type_tag::struct_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
            return file.structs[idx].name;
         return "object";
      }
      case psio::capnp_type_tag::enum_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
            return file.enums[idx].name;
         return "int";
      }
      case psio::capnp_type_tag::list:
      {
         int32_t elem_idx = field.type.element_type_idx;
         auto    elem_tag = decode_list_element_tag(elem_idx);
         if (elem_tag == psio::capnp_type_tag::struct_)
         {
            if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
               return "list[" + file.structs[elem_idx].name + "]";
            return "list";
         }
         psio::capnp_parsed_field fake;
         fake.type.tag = elem_tag;
         return "list[" + capnp_field_type_python(fake, file) + "]";
      }
      default:
         return "object";
   }
}

// Python default value for a capnp type
static std::string capnp_python_default(const psio::capnp_parsed_field& field)
{
   switch (field.type.tag)
   {
      case psio::capnp_type_tag::bool_:
         return "False";
      case psio::capnp_type_tag::int8:
      case psio::capnp_type_tag::int16:
      case psio::capnp_type_tag::int32:
      case psio::capnp_type_tag::int64:
      case psio::capnp_type_tag::uint8:
      case psio::capnp_type_tag::uint16:
      case psio::capnp_type_tag::uint32:
      case psio::capnp_type_tag::uint64:
         return "0";
      case psio::capnp_type_tag::float32:
      case psio::capnp_type_tag::float64:
         return "0.0";
      case psio::capnp_type_tag::text:
         return "\"\"";
      case psio::capnp_type_tag::data:
         return "b\"\"";
      case psio::capnp_type_tag::list:
         return "field(default_factory=list)";
      case psio::capnp_type_tag::struct_:
         return "None";  // Optional[T] = None
      default:
         return "None";
   }
}

// --- capnp type → Zig type string ---
static std::string capnp_field_type_zig(const psio::capnp_parsed_field& field,
                                        const psio::capnp_parsed_file&  file)
{
   auto tag = field.type.tag;

   switch (tag)
   {
      case psio::capnp_type_tag::void_:
         return "void";
      case psio::capnp_type_tag::bool_:
         return "bool";
      case psio::capnp_type_tag::int8:
         return "i8";
      case psio::capnp_type_tag::int16:
         return "i16";
      case psio::capnp_type_tag::int32:
         return "i32";
      case psio::capnp_type_tag::int64:
         return "i64";
      case psio::capnp_type_tag::uint8:
         return "u8";
      case psio::capnp_type_tag::uint16:
         return "u16";
      case psio::capnp_type_tag::uint32:
         return "u32";
      case psio::capnp_type_tag::uint64:
         return "u64";
      case psio::capnp_type_tag::float32:
         return "f32";
      case psio::capnp_type_tag::float64:
         return "f64";
      case psio::capnp_type_tag::text:
         return "[]const u8";
      case psio::capnp_type_tag::data:
         return "[]const u8";
      case psio::capnp_type_tag::any_ptr:
         return "?*anyopaque";
      case psio::capnp_type_tag::struct_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.structs.size()))
            return file.structs[idx].name;
         return "void";
      }
      case psio::capnp_type_tag::enum_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
            return file.enums[idx].name;
         return "u16";
      }
      case psio::capnp_type_tag::list:
      {
         int32_t elem_idx = field.type.element_type_idx;
         auto    elem_tag = decode_list_element_tag(elem_idx);
         if (elem_tag == psio::capnp_type_tag::struct_)
         {
            if (elem_idx >= 0 && elem_idx < static_cast<int32_t>(file.structs.size()))
               return "[]const " + file.structs[elem_idx].name;
            return "[]const void";
         }
         psio::capnp_parsed_field fake;
         fake.type.tag = elem_tag;
         return "[]const " + capnp_field_type_zig(fake, file);
      }
      default:
         return "void";
   }
}

// Zig default value for a capnp type
static std::string capnp_zig_default(const psio::capnp_parsed_field& field,
                                     const psio::capnp_parsed_file&  file)
{
   switch (field.type.tag)
   {
      case psio::capnp_type_tag::bool_:
         return "false";
      case psio::capnp_type_tag::int8:
      case psio::capnp_type_tag::int16:
      case psio::capnp_type_tag::int32:
      case psio::capnp_type_tag::int64:
      case psio::capnp_type_tag::uint8:
      case psio::capnp_type_tag::uint16:
      case psio::capnp_type_tag::uint32:
      case psio::capnp_type_tag::uint64:
         return "0";
      case psio::capnp_type_tag::float32:
      case psio::capnp_type_tag::float64:
         return "0.0";
      case psio::capnp_type_tag::text:
      case psio::capnp_type_tag::data:
         return "\"\"";
      case psio::capnp_type_tag::list:
         return "&.{}";
      case psio::capnp_type_tag::struct_:
         return ".{}";
      case psio::capnp_type_tag::enum_:
      {
         int32_t idx = field.type.referenced_type_idx;
         if (idx >= 0 && idx < static_cast<int32_t>(file.enums.size()))
         {
            const auto& e = file.enums[idx];
            if (!e.enumerants.empty())
               return "." + to_snake_case(e.enumerants[0]);
         }
         return "@enumFromInt(0)";
      }
      default:
         return "undefined";
   }
}

// --- fbs type → Go type string ---
static std::string fbs_field_type_go(const psio::fbs_field_def& field,
                                     const psio::fbs_schema&    schema)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "bool";
      case psio::fbs_base_type::int8_:
         return "int8";
      case psio::fbs_base_type::uint8_:
         return "uint8";
      case psio::fbs_base_type::int16_:
         return "int16";
      case psio::fbs_base_type::uint16_:
         return "uint16";
      case psio::fbs_base_type::int32_:
         return "int32";
      case psio::fbs_base_type::uint32_:
         return "uint32";
      case psio::fbs_base_type::int64_:
         return "int64";
      case psio::fbs_base_type::uint64_:
         return "uint64";
      case psio::fbs_base_type::float32_:
         return "float32";
      case psio::fbs_base_type::float64_:
         return "float64";
      case psio::fbs_base_type::string_:
         return "string";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return go_pascal_case(schema.types[field.type_idx].name);
         return "interface{}";
      }
      case psio::fbs_base_type::enum_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return go_pascal_case(schema.types[field.type_idx].name);
         return "int32";
      }
      case psio::fbs_base_type::union_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return go_pascal_case(schema.types[field.type_idx].name);
         return "interface{}";
      }
      case psio::fbs_base_type::vector_:
      {
         if (field.elem_type == psio::fbs_base_type::table_ ||
             field.elem_type == psio::fbs_base_type::struct_)
         {
            if (field.elem_type_idx >= 0 &&
                field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
               return "[]" + go_pascal_case(schema.types[field.elem_type_idx].name);
            return "[]interface{}";
         }
         if (field.elem_type == psio::fbs_base_type::string_)
            return "[]string";
         if (field.elem_type == psio::fbs_base_type::uint8_)
            return "[]byte";
         // Build element type via recursion on a fake field
         psio::fbs_field_def fake;
         fake.type = field.elem_type;
         fake.type_idx = field.elem_type_idx;
         return "[]" + fbs_field_type_go(fake, schema);
      }
      default:
         return "interface{}";
   }
}

// --- fbs type → TypeScript type string ---
static std::string fbs_field_type_ts(const psio::fbs_field_def& field,
                                     const psio::fbs_schema&    schema,
                                     std::string*               comment = nullptr)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "boolean";
      case psio::fbs_base_type::int8_:
      case psio::fbs_base_type::uint8_:
      case psio::fbs_base_type::int16_:
      case psio::fbs_base_type::uint16_:
      case psio::fbs_base_type::int32_:
      case psio::fbs_base_type::uint32_:
      case psio::fbs_base_type::float32_:
      case psio::fbs_base_type::float64_:
         return "number";
      case psio::fbs_base_type::int64_:
         if (comment)
            *comment = "int64 - use BigInt for values > 2^53";
         return "number";
      case psio::fbs_base_type::uint64_:
         if (comment)
            *comment = "uint64 - use BigInt for values > 2^53";
         return "number";
      case psio::fbs_base_type::string_:
         return "string";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "unknown";
      }
      case psio::fbs_base_type::enum_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "number";
      }
      case psio::fbs_base_type::union_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
         {
            const auto& utype = schema.types[field.type_idx];
            if (!utype.union_members.empty())
            {
               std::string result;
               for (size_t i = 0; i < utype.union_members.size(); ++i)
               {
                  if (i > 0)
                     result += " | ";
                  result += utype.union_members[i].name;
               }
               return result;
            }
         }
         return "unknown";
      }
      case psio::fbs_base_type::vector_:
      {
         if (field.elem_type == psio::fbs_base_type::table_ ||
             field.elem_type == psio::fbs_base_type::struct_)
         {
            if (field.elem_type_idx >= 0 &&
                field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
               return schema.types[field.elem_type_idx].name + "[]";
            return "unknown[]";
         }
         if (field.elem_type == psio::fbs_base_type::string_)
            return "string[]";
         if (field.elem_type == psio::fbs_base_type::uint8_)
            return "Uint8Array";
         psio::fbs_field_def fake;
         fake.type = field.elem_type;
         fake.type_idx = field.elem_type_idx;
         return fbs_field_type_ts(fake, schema) + "[]";
      }
      default:
         return "unknown";
   }
}

// --- fbs type → Python type string ---
static std::string fbs_field_type_python(const psio::fbs_field_def& field,
                                         const psio::fbs_schema&    schema)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "bool";
      case psio::fbs_base_type::int8_:
      case psio::fbs_base_type::uint8_:
      case psio::fbs_base_type::int16_:
      case psio::fbs_base_type::uint16_:
      case psio::fbs_base_type::int32_:
      case psio::fbs_base_type::uint32_:
      case psio::fbs_base_type::int64_:
      case psio::fbs_base_type::uint64_:
         return "int";
      case psio::fbs_base_type::float32_:
      case psio::fbs_base_type::float64_:
         return "float";
      case psio::fbs_base_type::string_:
         return "str";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "object";
      }
      case psio::fbs_base_type::enum_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "int";
      }
      case psio::fbs_base_type::union_:
         return "object";
      case psio::fbs_base_type::vector_:
      {
         if (field.elem_type == psio::fbs_base_type::table_ ||
             field.elem_type == psio::fbs_base_type::struct_)
         {
            if (field.elem_type_idx >= 0 &&
                field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
               return "list[" + schema.types[field.elem_type_idx].name + "]";
            return "list";
         }
         if (field.elem_type == psio::fbs_base_type::string_)
            return "list[str]";
         if (field.elem_type == psio::fbs_base_type::uint8_)
            return "bytes";
         psio::fbs_field_def fake;
         fake.type = field.elem_type;
         fake.type_idx = field.elem_type_idx;
         return "list[" + fbs_field_type_python(fake, schema) + "]";
      }
      default:
         return "object";
   }
}

// Python default for an fbs field
static std::string fbs_python_default(const psio::fbs_field_def& field)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "False";
      case psio::fbs_base_type::int8_:
      case psio::fbs_base_type::uint8_:
      case psio::fbs_base_type::int16_:
      case psio::fbs_base_type::uint16_:
      case psio::fbs_base_type::int32_:
      case psio::fbs_base_type::uint32_:
      case psio::fbs_base_type::int64_:
      case psio::fbs_base_type::uint64_:
         return "0";
      case psio::fbs_base_type::float32_:
      case psio::fbs_base_type::float64_:
         return "0.0";
      case psio::fbs_base_type::string_:
         return "\"\"";
      case psio::fbs_base_type::vector_:
         return "field(default_factory=list)";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
         return "None";
      default:
         return "None";
   }
}

// --- fbs type → Zig type string ---
static std::string fbs_field_type_zig(const psio::fbs_field_def& field,
                                      const psio::fbs_schema&    schema)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "bool";
      case psio::fbs_base_type::int8_:
         return "i8";
      case psio::fbs_base_type::uint8_:
         return "u8";
      case psio::fbs_base_type::int16_:
         return "i16";
      case psio::fbs_base_type::uint16_:
         return "u16";
      case psio::fbs_base_type::int32_:
         return "i32";
      case psio::fbs_base_type::uint32_:
         return "u32";
      case psio::fbs_base_type::int64_:
         return "i64";
      case psio::fbs_base_type::uint64_:
         return "u64";
      case psio::fbs_base_type::float32_:
         return "f32";
      case psio::fbs_base_type::float64_:
         return "f64";
      case psio::fbs_base_type::string_:
         return "[]const u8";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "void";
      }
      case psio::fbs_base_type::enum_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "u16";
      }
      case psio::fbs_base_type::union_:
      {
         if (field.type_idx >= 0 && field.type_idx < static_cast<int32_t>(schema.types.size()))
            return schema.types[field.type_idx].name;
         return "?*anyopaque";
      }
      case psio::fbs_base_type::vector_:
      {
         if (field.elem_type == psio::fbs_base_type::table_ ||
             field.elem_type == psio::fbs_base_type::struct_)
         {
            if (field.elem_type_idx >= 0 &&
                field.elem_type_idx < static_cast<int32_t>(schema.types.size()))
               return "[]const " + schema.types[field.elem_type_idx].name;
            return "[]const void";
         }
         if (field.elem_type == psio::fbs_base_type::string_)
            return "[]const []const u8";
         psio::fbs_field_def fake;
         fake.type = field.elem_type;
         fake.type_idx = field.elem_type_idx;
         return "[]const " + fbs_field_type_zig(fake, schema);
      }
      default:
         return "void";
   }
}

// Zig default for an fbs field
static std::string fbs_zig_default(const psio::fbs_field_def& field)
{
   switch (field.type)
   {
      case psio::fbs_base_type::bool_:
         return "false";
      case psio::fbs_base_type::int8_:
      case psio::fbs_base_type::uint8_:
      case psio::fbs_base_type::int16_:
      case psio::fbs_base_type::uint16_:
      case psio::fbs_base_type::int32_:
      case psio::fbs_base_type::uint32_:
      case psio::fbs_base_type::int64_:
      case psio::fbs_base_type::uint64_:
         return "0";
      case psio::fbs_base_type::float32_:
      case psio::fbs_base_type::float64_:
         return "0.0";
      case psio::fbs_base_type::string_:
      case psio::fbs_base_type::vector_:
         return "&.{}";
      case psio::fbs_base_type::table_:
      case psio::fbs_base_type::struct_:
         return ".{}";
      default:
         return "undefined";
   }
}

// --- Generate Go code from a parsed capnp file ---
static std::string codegen_capnp_go(const psio::capnp_parsed_file& file,
                                    const std::string&              source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "package schemas\n\n";

   // Enums (dependency order: emit before structs that reference them)
   for (const auto& e : file.enums)
   {
      std::string tname = go_pascal_case(e.name);
      out << "type " << tname << " uint16\n\n";
      out << "const (\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         out << "\t" << tname << go_pascal_case(e.enumerants[i]) << " " << tname;
         if (i == 0)
            out << " = iota";
         out << "\n";
      }
      out << ")\n\n";
   }

   // Structs in declaration order
   for (const auto& s : file.structs)
   {
      out << "type " << go_pascal_case(s.name) << " struct {\n";
      for (const auto& f : s.fields)
      {
         std::string go_t  = capnp_field_type_go(f, file);
         std::string fname = go_pascal_case(f.name);
         out << "\t" << fname;
         size_t pad = (fname.size() < 12) ? 12 - fname.size() : 1;
         for (size_t i = 0; i < pad; ++i)
            out << ' ';
         out << go_t << " `json:\"" << f.name << "\"`\n";
      }
      out << "}\n\n";
   }

   return out.str();
}

// --- Generate Go code from a parsed fbs schema ---
static std::string codegen_fbs_go(const psio::fbs_schema& schema,
                                  const std::string&      source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "package schemas\n\n";

   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         std::string tname = go_pascal_case(t.name);
         out << "type " << tname << " int32\n\n";
         out << "const (\n";
         for (size_t i = 0; i < t.enum_values.size(); ++i)
         {
            out << "\t" << tname << go_pascal_case(t.enum_values[i].name) << " " << tname;
            if (i == 0)
               out << " = iota";
            out << "\n";
         }
         out << ")\n\n";
      }
      else if (t.kind == psio::fbs_type_kind::union_)
      {
         // Go interface for union
         out << "type " << go_pascal_case(t.name) << " interface {\n";
         out << "\tIs" << go_pascal_case(t.name) << "()\n";
         out << "}\n\n";
      }
      else
      {
         out << "type " << go_pascal_case(t.name) << " struct {\n";
         for (const auto& f : t.fields)
         {
            std::string go_t  = fbs_field_type_go(f, schema);
            std::string fname = go_pascal_case(f.name);
            out << "\t" << fname;
            size_t pad = (fname.size() < 12) ? 12 - fname.size() : 1;
            for (size_t i = 0; i < pad; ++i)
               out << ' ';
            out << go_t << " `json:\"" << f.name << "\"`\n";
         }
         out << "}\n\n";
      }
   }

   return out.str();
}

// --- Generate TypeScript code from a parsed capnp file ---
static std::string codegen_capnp_ts(const psio::capnp_parsed_file& file,
                                    const std::string&              source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n\n";

   // Enums
   for (const auto& e : file.enums)
   {
      out << "export enum " << e.name << " {\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         std::string ename = e.enumerants[i];
         ename[0]          = static_cast<char>(std::toupper(ename[0]));
         out << "    " << ename << " = " << i << ",\n";
      }
      out << "}\n\n";
   }

   // Structs
   for (const auto& s : file.structs)
   {
      out << "export interface " << s.name << " {\n";
      for (const auto& f : s.fields)
      {
         std::string comment;
         std::string ts_t  = capnp_field_type_ts(f, file, &comment);
         std::string fname = ts_camel_case(f.name);
         out << "    " << fname << ": " << ts_t << ";";
         if (!comment.empty())
            out << "  // " << comment;
         out << "\n";
      }
      out << "}\n\n";
   }

   return out.str();
}

// --- Generate TypeScript code from a parsed fbs schema ---
static std::string codegen_fbs_ts(const psio::fbs_schema& schema,
                                  const std::string&      source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n\n";

   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         out << "export enum " << t.name << " {\n";
         for (const auto& v : t.enum_values)
         {
            std::string ename = v.name;
            ename[0]          = static_cast<char>(std::toupper(ename[0]));
            out << "    " << ename << " = " << v.value << ",\n";
         }
         out << "}\n\n";
      }
      else if (t.kind == psio::fbs_type_kind::union_)
      {
         out << "export type " << t.name << " = ";
         for (size_t i = 0; i < t.union_members.size(); ++i)
         {
            if (i > 0)
               out << " | ";
            out << t.union_members[i].name;
         }
         out << ";\n\n";
      }
      else
      {
         out << "export interface " << t.name << " {\n";
         for (const auto& f : t.fields)
         {
            std::string comment;
            std::string ts_t  = fbs_field_type_ts(f, schema, &comment);
            std::string fname = ts_camel_case(f.name);
            out << "    " << fname << ": " << ts_t << ";";
            if (!comment.empty())
               out << "  // " << comment;
            out << "\n";
         }
         out << "}\n\n";
      }
   }

   return out.str();
}

// --- Generate Python code from a parsed capnp file ---
static std::string codegen_capnp_python(const psio::capnp_parsed_file& file,
                                        const std::string&              source_name)
{
   std::ostringstream out;
   out << "# Generated by psio-tool from " << source_name << "\n";
   out << "from dataclasses import dataclass, field\n";
   if (!file.enums.empty())
      out << "from enum import Enum\n";
   out << "from typing import Optional\n\n\n";

   // Enums
   for (const auto& e : file.enums)
   {
      out << "class " << e.name << "(Enum):\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         // UPPER_SNAKE_CASE for Python enum values
         std::string upper;
         for (size_t j = 0; j < e.enumerants[i].size(); ++j)
         {
            if (std::isupper(e.enumerants[i][j]) && j > 0)
               upper += '_';
            upper += static_cast<char>(std::toupper(e.enumerants[i][j]));
         }
         out << "    " << upper << " = " << i << "\n";
      }
      out << "\n\n";
   }

   // Structs
   for (const auto& s : file.structs)
   {
      out << "@dataclass\n";
      out << "class " << s.name << ":\n";
      if (s.fields.empty())
      {
         out << "    pass\n\n\n";
         continue;
      }
      for (const auto& f : s.fields)
      {
         std::string py_t   = capnp_field_type_python(f, file);
         std::string fname  = to_snake_case(f.name);
         std::string defval = capnp_python_default(f);

         // Struct-typed fields become Optional
         if (f.type.tag == psio::capnp_type_tag::struct_)
            py_t = "Optional[" + py_t + "]";

         out << "    " << fname << ": " << py_t << " = " << defval << "\n";
      }
      out << "\n\n";
   }

   return out.str();
}

// --- Generate Python code from a parsed fbs schema ---
static std::string codegen_fbs_python(const psio::fbs_schema& schema,
                                      const std::string&      source_name)
{
   std::ostringstream out;
   out << "# Generated by psio-tool from " << source_name << "\n";
   out << "from dataclasses import dataclass, field\n";

   bool has_enum = false;
   for (const auto& t : schema.types)
      if (t.kind == psio::fbs_type_kind::enum_)
         has_enum = true;
   if (has_enum)
      out << "from enum import Enum\n";
   out << "from typing import Optional, Union\n\n\n";

   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         out << "class " << t.name << "(Enum):\n";
         for (const auto& v : t.enum_values)
         {
            std::string upper;
            for (size_t j = 0; j < v.name.size(); ++j)
            {
               if (std::isupper(v.name[j]) && j > 0)
                  upper += '_';
               upper += static_cast<char>(std::toupper(v.name[j]));
            }
            out << "    " << upper << " = " << v.value << "\n";
         }
         out << "\n\n";
      }
      else if (t.kind == psio::fbs_type_kind::union_)
      {
         out << t.name << " = Union[";
         for (size_t i = 0; i < t.union_members.size(); ++i)
         {
            if (i > 0)
               out << ", ";
            out << t.union_members[i].name;
         }
         out << "]\n\n\n";
      }
      else
      {
         out << "@dataclass\n";
         out << "class " << t.name << ":\n";
         if (t.fields.empty())
         {
            out << "    pass\n\n\n";
            continue;
         }
         for (const auto& f : t.fields)
         {
            std::string py_t   = fbs_field_type_python(f, schema);
            std::string fname  = to_snake_case(f.name);
            std::string defval = fbs_python_default(f);

            if (f.type == psio::fbs_base_type::table_ || f.type == psio::fbs_base_type::struct_)
               py_t = "Optional[" + py_t + "]";

            out << "    " << fname << ": " << py_t << " = " << defval << "\n";
         }
         out << "\n\n";
      }
   }

   return out.str();
}

// --- Generate Zig code from a parsed capnp file ---
static std::string codegen_capnp_zig(const psio::capnp_parsed_file& file,
                                     const std::string&              source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "const std = @import(\"std\");\n\n";

   // Enums
   for (const auto& e : file.enums)
   {
      out << "pub const " << e.name << " = enum(u16) {\n";
      for (size_t i = 0; i < e.enumerants.size(); ++i)
      {
         out << "    " << to_snake_case(e.enumerants[i]) << " = " << i << ",\n";
      }
      out << "};\n\n";
   }

   // Structs
   for (const auto& s : file.structs)
   {
      out << "pub const " << s.name << " = struct {\n";
      for (const auto& f : s.fields)
      {
         std::string zig_t  = capnp_field_type_zig(f, file);
         std::string fname  = to_snake_case(f.name);
         std::string defval = capnp_zig_default(f, file);
         out << "    " << fname << ": " << zig_t << " = " << defval << ",\n";
      }
      out << "};\n\n";
   }

   return out.str();
}

// --- Generate Zig code from a parsed fbs schema ---
static std::string codegen_fbs_zig(const psio::fbs_schema& schema,
                                   const std::string&      source_name)
{
   std::ostringstream out;
   out << "// Generated by psio-tool from " << source_name << "\n";
   out << "const std = @import(\"std\");\n\n";

   for (const auto& t : schema.types)
   {
      if (t.kind == psio::fbs_type_kind::enum_)
      {
         std::string underlying;
         switch (t.underlying_type)
         {
            case psio::fbs_base_type::int8_:
               underlying = "i8";
               break;
            case psio::fbs_base_type::uint8_:
               underlying = "u8";
               break;
            case psio::fbs_base_type::int16_:
               underlying = "i16";
               break;
            case psio::fbs_base_type::uint16_:
               underlying = "u16";
               break;
            case psio::fbs_base_type::int64_:
               underlying = "i64";
               break;
            case psio::fbs_base_type::uint64_:
               underlying = "u64";
               break;
            default:
               underlying = "i32";
               break;
         }
         out << "pub const " << t.name << " = enum(" << underlying << ") {\n";
         for (const auto& v : t.enum_values)
         {
            out << "    " << to_snake_case(v.name) << " = " << v.value << ",\n";
         }
         out << "};\n\n";
      }
      else if (t.kind == psio::fbs_type_kind::union_)
      {
         out << "pub const " << t.name << " = union(enum) {\n";
         for (const auto& m : t.union_members)
         {
            out << "    " << to_snake_case(m.name) << ": " << m.name << ",\n";
         }
         out << "};\n\n";
      }
      else
      {
         out << "pub const " << t.name << " = struct {\n";
         for (const auto& f : t.fields)
         {
            std::string zig_t  = fbs_field_type_zig(f, schema);
            std::string fname  = to_snake_case(f.name);
            std::string defval = fbs_zig_default(f);
            out << "    " << fname << ": " << zig_t << " = " << defval << ",\n";
         }
         out << "};\n\n";
      }
   }

   return out.str();
}

static int cmd_codegen(int argc, char** argv)
{
   // psio-tool codegen <schema-format> <schema-file> --lang <language> [--output <file>]
   if (argc < 4)
   {
      std::cerr << "error: codegen command requires: <schema-format> <schema-file> --lang "
                   "<language>\n";
      std::cerr << "  psio-tool codegen capnp schema.capnp --lang cpp\n";
      std::cerr << "  psio-tool codegen fbs schema.fbs --lang rust\n";
      std::cerr << "  psio-tool codegen capnp schema.capnp --lang go\n";
      std::cerr << "  psio-tool codegen capnp schema.capnp --lang typescript\n";
      std::cerr << "  psio-tool codegen capnp schema.capnp --lang python\n";
      std::cerr << "  psio-tool codegen capnp schema.capnp --lang zig\n";
      return 1;
   }

   auto        fmt         = parse_format(argv[2]);
   std::string schema_path = argv[3];

   // Parse optional flags
   codegen_lang lang     = codegen_lang::cpp;  // default
   std::string  output_path;

   for (int i = 4; i < argc; ++i)
   {
      std::string_view arg = argv[i];
      if (arg == "--lang" && i + 1 < argc)
      {
         lang = parse_lang(argv[++i]);
      }
      else if (arg == "--output" && i + 1 < argc)
      {
         output_path = argv[++i];
      }
   }

   // Auto-detect format from file extension
   if (fmt == format_id::unknown)
      fmt = detect_schema_format(schema_path);
   if (fmt == format_id::unknown)
   {
      std::cerr << "error: cannot determine schema format. Specify capnp or fbs.\n";
      return 1;
   }

   if (fmt != format_id::capnp && fmt != format_id::flatbuf)
   {
      std::cerr << "error: codegen only supports capnp and fbs schema formats\n";
      return 1;
   }

   std::string schema_text = read_file(schema_path);
   std::string source_name = std::filesystem::path(schema_path).filename().string();
   std::string code;

   try
   {
      if (fmt == format_id::capnp)
      {
         auto file = psio::capnp_parse(schema_text);
         if (file.structs.empty() && file.enums.empty())
         {
            std::cerr << "error: schema contains no type definitions\n";
            return 1;
         }

         switch (lang)
         {
            case codegen_lang::cpp:
               code = codegen_capnp_cpp(file, source_name);
               break;
            case codegen_lang::rust:
               code = codegen_capnp_rust(file, source_name);
               break;
            case codegen_lang::go:
               code = codegen_capnp_go(file, source_name);
               break;
            case codegen_lang::typescript:
               code = codegen_capnp_ts(file, source_name);
               break;
            case codegen_lang::python:
               code = codegen_capnp_python(file, source_name);
               break;
            case codegen_lang::zig:
               code = codegen_capnp_zig(file, source_name);
               break;
         }
      }
      else if (fmt == format_id::flatbuf)
      {
         auto schema = psio::fbs_parse(schema_text);
         if (schema.types.empty())
         {
            std::cerr << "error: schema contains no type definitions\n";
            return 1;
         }

         switch (lang)
         {
            case codegen_lang::cpp:
               code = codegen_fbs_cpp(schema, source_name);
               break;
            case codegen_lang::rust:
               code = codegen_fbs_rust(schema, source_name);
               break;
            case codegen_lang::go:
               code = codegen_fbs_go(schema, source_name);
               break;
            case codegen_lang::typescript:
               code = codegen_fbs_ts(schema, source_name);
               break;
            case codegen_lang::python:
               code = codegen_fbs_python(schema, source_name);
               break;
            case codegen_lang::zig:
               code = codegen_fbs_zig(schema, source_name);
               break;
         }
      }
   }
   catch (const psio::capnp_parse_error& e)
   {
      std::cerr << "schema parse error: " << e.what() << "\n";
      return 1;
   }
   catch (const psio::fbs_parse_error& e)
   {
      std::cerr << "schema parse error: " << e.what() << "\n";
      return 1;
   }
   catch (const std::exception& e)
   {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
   }

   // Write output
   if (!output_path.empty())
   {
      std::ofstream out(output_path);
      if (!out)
      {
         std::cerr << "error: cannot write to " << output_path << "\n";
         return 1;
      }
      out << code;
      std::cerr << "wrote " << output_path << "\n";
   }
   else
   {
      std::cout << code;
   }

   return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv)
{
   if (argc < 2)
      usage();

   std::string_view cmd = argv[1];

   if (cmd == "info" || cmd == "--info" || cmd == "-i")
      return cmd_info();
   if (cmd == "inspect" || cmd == "dump")
      return cmd_inspect(argc, argv);
   if (cmd == "validate" || cmd == "check")
      return cmd_validate(argc, argv);
   if (cmd == "convert" || cmd == "conv")
      return cmd_convert(argc, argv);
   if (cmd == "schema" || cmd == "sch")
      return cmd_schema(argc, argv);
   if (cmd == "codegen" || cmd == "cg" || cmd == "generate")
      return cmd_codegen(argc, argv);
   if (cmd == "help" || cmd == "--help" || cmd == "-h")
      usage();

   std::cerr << "error: unknown command '" << cmd << "'\n\n";
   usage();
}
