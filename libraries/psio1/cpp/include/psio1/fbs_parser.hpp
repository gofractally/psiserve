// psio/fbs_parser.hpp — Recursive-descent FlatBuffers IDL (.fbs) parser
//
// Parses FlatBuffers schema text into a runtime fbs_schema tree that
// describes the wire layout of tables, structs, enums, and unions.
// This enables dynamic_view to read FlatBuffer bytes without compile-time
// type information.
//
//   auto schema = psio1::fbs_parse(R"(
//       namespace game;
//       table Monster {
//           name:string;
//           hp:short = 100;
//           pos:Vec3;
//           inventory:[ubyte];
//       }
//       struct Vec3 { x:float; y:float; z:float; }
//       root_type Monster;
//   )");
//
//   auto& monster = schema.find_type("Monster");
//   // monster.kind == fbs_type_kind::table_
//   // monster.fields[0].name == "name"
//   // monster.fields[0].type == fbs_field_type::string_
//
// Header-only. Zero external dependencies beyond <psio1/capnp_view.hpp>
// (for dynamic_schema, dynamic_field_desc, xxh64_hash, dynamic_type).

#pragma once

#include <psio1/capnp_view.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psio1
{

   // =========================================================================
   // Parse error
   // =========================================================================

   struct fbs_parse_error : std::runtime_error
   {
      uint32_t line;
      uint32_t column;
      fbs_parse_error(const std::string& msg, uint32_t line, uint32_t col)
          : std::runtime_error("fbs:" + std::to_string(line) + ":" + std::to_string(col) +
                               ": " + msg),
            line(line),
            column(col)
      {
      }
   };

   // =========================================================================
   // Schema types — runtime representation of parsed .fbs
   // =========================================================================

   // Scalar / base type classification
   enum class fbs_base_type : uint8_t
   {
      none_ = 0,
      bool_,
      int8_,
      uint8_,
      int16_,
      uint16_,
      int32_,
      uint32_,
      int64_,
      uint64_,
      float32_,
      float64_,
      string_,
      vector_,   // [T] — element type in element_type_idx
      table_,    // reference to a table type
      struct_,   // reference to a struct type
      enum_,     // reference to an enum type
      union_,    // reference to a union type
   };

   // What kind of top-level definition
   enum class fbs_type_kind : uint8_t
   {
      table_  = 0,
      struct_ = 1,
      enum_   = 2,
      union_  = 3,
   };

   // Field metadata (from parenthesized attributes)
   struct fbs_field_metadata
   {
      bool    deprecated = false;
      int32_t id         = -1;  // -1 means "use declaration order"
      bool    required   = false;
      // TODO: nested_flatbuffer, flexbuffer, key, hash, etc.
   };

   // An enum value (name + integer value)
   struct fbs_enum_val
   {
      std::string name;
      int64_t     value = 0;
   };

   // A union member
   struct fbs_union_member
   {
      std::string name;
      int32_t     type_idx = -1;  // index into fbs_schema::types
   };

   // Forward declaration
   struct fbs_type_def;

   // A field within a table or struct
   struct fbs_field_def
   {
      std::string        name;
      fbs_base_type      type      = fbs_base_type::none_;
      int32_t            type_idx  = -1;  // for table/struct/enum/union refs: index into types[]
      fbs_base_type      elem_type = fbs_base_type::none_;  // for vectors: element type
      int32_t            elem_type_idx = -1;                // for vector of table/struct/enum
      std::string        unresolved_type_name;              // type name before resolution
      std::string        unresolved_elem_name;              // vector element type name before resolution
      fbs_field_metadata metadata;

      // Layout info (computed after parsing)
      int32_t  vtable_slot    = -1;    // vtable slot index (table fields only)
      uint32_t struct_offset  = 0;     // byte offset within struct (struct fields only)
      uint8_t  wire_size      = 0;     // wire byte size (for inline scalars/structs)
      uint8_t  wire_align     = 0;     // alignment requirement
      bool     is_offset_type = false; // true if field stores a relative offset (strings, vectors, tables)

      // Default value (parsed from `= value`)
      int64_t     default_int    = 0;
      double      default_float  = 0.0;
      std::string default_string;
      bool        has_default    = false;
   };

   // A top-level type definition (table, struct, enum, or union)
   struct fbs_type_def
   {
      std::string                 name;
      std::string                 full_name;  // namespace.name
      fbs_type_kind               kind = fbs_type_kind::table_;

      // For tables and structs
      std::vector<fbs_field_def>  fields;

      // For enums
      fbs_base_type               underlying_type = fbs_base_type::int32_;
      std::vector<fbs_enum_val>   enum_values;

      // For unions
      std::vector<fbs_union_member> union_members;

      // Layout info (computed after parsing)
      uint16_t vtable_slot_count = 0;  // number of vtable slots (tables only)
      uint32_t struct_size       = 0;  // total byte size (structs only)
      uint32_t struct_align      = 0;  // alignment (structs only)

      // Runtime schema (built by finalize())
      std::unique_ptr<dynamic_schema>        dyn_schema;
      std::vector<dynamic_field_desc>        dyn_fields;
      std::vector<std::string>               dyn_field_names;
      std::vector<const char*>               dyn_ordered_names;
      std::vector<uint8_t>                   dyn_tags;
   };

   // Top-level schema container
   struct fbs_schema
   {
      std::string                      ns;         // namespace
      std::string                      root_type;  // root_type declaration
      std::string                      file_identifier;
      std::string                      file_extension;
      std::vector<fbs_type_def>        types;
      std::vector<std::string>         includes;
      std::vector<std::string>         attributes;

      // Name resolution
      std::unordered_map<std::string, uint32_t> type_map;  // full_name -> index

      // Find a type by name (tries both bare name and namespace-qualified)
      const fbs_type_def* find_type(std::string_view name) const
      {
         std::string key(name);
         auto        it = type_map.find(key);
         if (it != type_map.end())
            return &types[it->second];
         // Try with namespace prefix
         if (!ns.empty())
         {
            key = ns + "." + std::string(name);
            it  = type_map.find(key);
            if (it != type_map.end())
               return &types[it->second];
         }
         return nullptr;
      }

      fbs_type_def* find_type_mut(std::string_view name)
      {
         return const_cast<fbs_type_def*>(
             static_cast<const fbs_schema*>(this)->find_type(name));
      }
   };

   // =========================================================================
   // Lexer
   // =========================================================================

   namespace detail::fbs_detail
   {

      enum class fbs_tok : uint8_t
      {
         eof,
         ident,
         integer,
         float_lit,
         string_lit,
         // punctuation
         lbrace,
         rbrace,
         lparen,
         rparen,
         lbracket,
         rbracket,
         colon,
         semicolon,
         equal,
         comma,
         dot,
         // keywords
         kw_table,
         kw_struct,
         kw_enum,
         kw_union,
         kw_root_type,
         kw_namespace,
         kw_attribute,
         kw_include,
         kw_file_identifier,
         kw_file_extension,
         kw_rpc_service,
         // built-in types
         kw_bool,
         kw_byte,
         kw_ubyte,
         kw_short,
         kw_ushort,
         kw_int,
         kw_uint,
         kw_long,
         kw_ulong,
         kw_float,
         kw_double,
         kw_int8,
         kw_uint8,
         kw_int16,
         kw_uint16,
         kw_int32,
         kw_uint32,
         kw_int64,
         kw_uint64,
         kw_float32,
         kw_float64,
         kw_string,
      };

      struct fbs_token
      {
         fbs_tok          kind = fbs_tok::eof;
         std::string_view text;
         uint32_t         line = 1;
         uint32_t         col  = 1;
      };

      class fbs_lexer
      {
       public:
         explicit fbs_lexer(std::string_view src) : src_(src), pos_(0), line_(1), col_(1) {}

         fbs_token next()
         {
            skip_ws_and_comments();
            if (pos_ >= src_.size())
               return {fbs_tok::eof, {}, line_, col_};

            uint32_t start_line = line_;
            uint32_t start_col  = col_;
            char     c          = src_[pos_];

            // Single-char punctuation
            auto single = [&](fbs_tok k) -> fbs_token
            {
               auto sv = src_.substr(pos_, 1);
               advance(1);
               return {k, sv, start_line, start_col};
            };

            switch (c)
            {
               case '{':
                  return single(fbs_tok::lbrace);
               case '}':
                  return single(fbs_tok::rbrace);
               case '(':
                  return single(fbs_tok::lparen);
               case ')':
                  return single(fbs_tok::rparen);
               case '[':
                  return single(fbs_tok::lbracket);
               case ']':
                  return single(fbs_tok::rbracket);
               case ':':
                  return single(fbs_tok::colon);
               case ';':
                  return single(fbs_tok::semicolon);
               case '=':
                  return single(fbs_tok::equal);
               case ',':
                  return single(fbs_tok::comma);
               case '.':
                  return single(fbs_tok::dot);
               default:
                  break;
            }

            // String literal
            if (c == '"')
            {
               advance(1);  // skip opening quote
               size_t start = pos_;
               while (pos_ < src_.size() && src_[pos_] != '"')
               {
                  if (src_[pos_] == '\\' && pos_ + 1 < src_.size())
                     advance(1);  // skip escape
                  advance(1);
               }
               auto sv = src_.substr(start, pos_ - start);
               if (pos_ < src_.size())
                  advance(1);  // skip closing quote
               return {fbs_tok::string_lit, sv, start_line, start_col};
            }

            // Numbers (integers and floats, including negative)
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+')
            {
               return lex_number(start_line, start_col);
            }

            // Identifiers and keywords
            if (is_ident_start(c))
            {
               size_t start = pos_;
               advance(1);
               while (pos_ < src_.size() && is_ident_cont(src_[pos_]))
                  advance(1);
               auto text = src_.substr(start, pos_ - start);
               auto kind = classify_keyword(text);
               return {kind, text, start_line, start_col};
            }

            throw fbs_parse_error(
                std::string("unexpected character '") + c + "'", line_, col_);
         }

         fbs_token peek()
         {
            auto saved_pos  = pos_;
            auto saved_line = line_;
            auto saved_col  = col_;
            auto tok        = next();
            pos_            = saved_pos;
            line_           = saved_line;
            col_            = saved_col;
            return tok;
         }

       private:
         std::string_view src_;
         size_t           pos_;
         uint32_t         line_;
         uint32_t         col_;

         void advance(size_t n)
         {
            for (size_t i = 0; i < n && pos_ < src_.size(); i++, pos_++)
            {
               if (src_[pos_] == '\n')
               {
                  line_++;
                  col_ = 1;
               }
               else
               {
                  col_++;
               }
            }
         }

         void skip_ws_and_comments()
         {
            while (pos_ < src_.size())
            {
               char c = src_[pos_];
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
               {
                  advance(1);
                  continue;
               }
               if (c == '/' && pos_ + 1 < src_.size())
               {
                  if (src_[pos_ + 1] == '/')
                  {
                     // Line comment
                     while (pos_ < src_.size() && src_[pos_] != '\n')
                        advance(1);
                     continue;
                  }
                  if (src_[pos_ + 1] == '*')
                  {
                     // Block comment
                     advance(2);
                     while (pos_ + 1 < src_.size() &&
                            !(src_[pos_] == '*' && src_[pos_ + 1] == '/'))
                        advance(1);
                     if (pos_ + 1 < src_.size())
                        advance(2);
                     continue;
                  }
               }
               break;
            }
         }

         fbs_token lex_number(uint32_t start_line, uint32_t start_col)
         {
            size_t start    = pos_;
            bool   is_float = false;

            // Optional sign
            if (src_[pos_] == '-' || src_[pos_] == '+')
               advance(1);

            // Hex prefix
            if (pos_ + 1 < src_.size() && src_[pos_] == '0' &&
                (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X'))
            {
               advance(2);
               while (pos_ < src_.size() && is_hex_digit(src_[pos_]))
                  advance(1);
               return {fbs_tok::integer, src_.substr(start, pos_ - start), start_line, start_col};
            }

            // Digits
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
               advance(1);

            // Decimal point
            if (pos_ < src_.size() && src_[pos_] == '.')
            {
               is_float = true;
               advance(1);
               while (pos_ < src_.size() &&
                      std::isdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
            }

            // Exponent
            if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E'))
            {
               is_float = true;
               advance(1);
               if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                  advance(1);
               while (pos_ < src_.size() &&
                      std::isdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
            }

            auto text = src_.substr(start, pos_ - start);
            return {is_float ? fbs_tok::float_lit : fbs_tok::integer, text, start_line,
                    start_col};
         }

         static bool is_ident_start(char c)
         {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
         }

         static bool is_ident_cont(char c)
         {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
         }

         static bool is_hex_digit(char c)
         {
            return std::isxdigit(static_cast<unsigned char>(c));
         }

         static fbs_tok classify_keyword(std::string_view text)
         {
            // Structure keywords
            if (text == "table")
               return fbs_tok::kw_table;
            if (text == "struct")
               return fbs_tok::kw_struct;
            if (text == "enum")
               return fbs_tok::kw_enum;
            if (text == "union")
               return fbs_tok::kw_union;
            if (text == "root_type")
               return fbs_tok::kw_root_type;
            if (text == "namespace")
               return fbs_tok::kw_namespace;
            if (text == "attribute")
               return fbs_tok::kw_attribute;
            if (text == "include")
               return fbs_tok::kw_include;
            if (text == "file_identifier")
               return fbs_tok::kw_file_identifier;
            if (text == "file_extension")
               return fbs_tok::kw_file_extension;
            if (text == "rpc_service")
               return fbs_tok::kw_rpc_service;
            // Built-in types
            if (text == "bool")
               return fbs_tok::kw_bool;
            if (text == "byte" || text == "int8")
               return fbs_tok::kw_int8;
            if (text == "ubyte" || text == "uint8")
               return fbs_tok::kw_uint8;
            if (text == "short" || text == "int16")
               return fbs_tok::kw_int16;
            if (text == "ushort" || text == "uint16")
               return fbs_tok::kw_uint16;
            if (text == "int" || text == "int32")
               return fbs_tok::kw_int32;
            if (text == "uint" || text == "uint32")
               return fbs_tok::kw_uint32;
            if (text == "long" || text == "int64")
               return fbs_tok::kw_int64;
            if (text == "ulong" || text == "uint64")
               return fbs_tok::kw_uint64;
            if (text == "float" || text == "float32")
               return fbs_tok::kw_float32;
            if (text == "double" || text == "float64")
               return fbs_tok::kw_float64;
            if (text == "string")
               return fbs_tok::kw_string;
            return fbs_tok::ident;
         }
      };

   }  // namespace detail::fbs_detail

   // =========================================================================
   // Parser
   // =========================================================================

   class fbs_parser
   {
    public:
      explicit fbs_parser(std::string_view source) : lex_(source) {}

      fbs_schema parse()
      {
         fbs_schema schema;

         while (!peek_is(tok::eof))
         {
            auto t = lex_.peek();
            switch (t.kind)
            {
               case tok::kw_namespace:
                  parse_namespace(schema);
                  break;
               case tok::kw_table:
                  parse_table(schema);
                  break;
               case tok::kw_struct:
                  parse_struct(schema);
                  break;
               case tok::kw_enum:
                  parse_enum(schema);
                  break;
               case tok::kw_union:
                  parse_union(schema);
                  break;
               case tok::kw_root_type:
                  parse_root_type(schema);
                  break;
               case tok::kw_file_identifier:
                  parse_file_identifier(schema);
                  break;
               case tok::kw_file_extension:
                  parse_file_extension(schema);
                  break;
               case tok::kw_include:
                  parse_include(schema);
                  break;
               case tok::kw_attribute:
                  parse_attribute(schema);
                  break;
               case tok::kw_rpc_service:
                  skip_rpc_service();
                  break;
               default:
                  error(t, "expected top-level declaration (table, struct, enum, "
                           "union, namespace, root_type, include, attribute)");
            }
         }

         resolve_types(schema);
         compute_layouts(schema);
         build_dynamic_schemas(schema);

         return schema;
      }

      static fbs_schema parse_fbs(std::string_view source)
      {
         fbs_parser p(source);
         return p.parse();
      }

    private:
      detail::fbs_detail::fbs_lexer lex_;
      using tok = detail::fbs_detail::fbs_tok;

      // ----- Token helpers -----

      bool peek_is(tok k) { return lex_.peek().kind == k; }

      detail::fbs_detail::fbs_token expect(tok k)
      {
         auto t = lex_.next();
         if (t.kind != k)
            error(t, "expected " + tok_name(k) + ", got '" + std::string(t.text) + "'");
         return t;
      }

      detail::fbs_detail::fbs_token expect_ident()
      {
         auto t = lex_.next();
         if (t.kind != tok::ident)
            error(t, "expected identifier, got '" + std::string(t.text) + "'");
         return t;
      }

      [[noreturn]] void error(const detail::fbs_detail::fbs_token& t, const std::string& msg)
      {
         throw fbs_parse_error(msg, t.line, t.col);
      }

      static std::string tok_name(tok k)
      {
         switch (k)
         {
            case tok::eof:
               return "EOF";
            case tok::ident:
               return "identifier";
            case tok::integer:
               return "integer";
            case tok::float_lit:
               return "float";
            case tok::string_lit:
               return "string";
            case tok::lbrace:
               return "'{'";
            case tok::rbrace:
               return "'}'";
            case tok::lparen:
               return "'('";
            case tok::rparen:
               return "')'";
            case tok::lbracket:
               return "'['";
            case tok::rbracket:
               return "']'";
            case tok::colon:
               return "':'";
            case tok::semicolon:
               return "';'";
            case tok::equal:
               return "'='";
            case tok::comma:
               return "','";
            case tok::dot:
               return "'.'";
            default:
               return "keyword";
         }
      }

      // ----- Namespace -----

      void parse_namespace(fbs_schema& schema)
      {
         expect(tok::kw_namespace);
         std::string ns;
         auto        name = expect_ident();
         ns               = std::string(name.text);
         while (peek_is(tok::dot))
         {
            lex_.next();
            auto part = expect_ident();
            ns += ".";
            ns += part.text;
         }
         expect(tok::semicolon);
         schema.ns = std::move(ns);
      }

      // ----- Table -----

      void parse_table(fbs_schema& schema)
      {
         expect(tok::kw_table);
         auto name = expect_ident();

         fbs_type_def td;
         td.name = std::string(name.text);
         td.kind = fbs_type_kind::table_;
         if (!schema.ns.empty())
            td.full_name = schema.ns + "." + td.name;
         else
            td.full_name = td.name;

         // Optional metadata before the brace
         if (peek_is(tok::lparen))
            skip_metadata();

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            parse_field(td);
         }
         expect(tok::rbrace);

         uint32_t idx               = static_cast<uint32_t>(schema.types.size());
         schema.type_map[td.name]   = idx;
         schema.type_map[td.full_name] = idx;
         schema.types.push_back(std::move(td));
      }

      // ----- Struct -----

      void parse_struct(fbs_schema& schema)
      {
         expect(tok::kw_struct);
         auto name = expect_ident();

         fbs_type_def td;
         td.name = std::string(name.text);
         td.kind = fbs_type_kind::struct_;
         if (!schema.ns.empty())
            td.full_name = schema.ns + "." + td.name;
         else
            td.full_name = td.name;

         // Optional metadata before the brace
         if (peek_is(tok::lparen))
            skip_metadata();

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            parse_field(td);
         }
         expect(tok::rbrace);

         uint32_t idx               = static_cast<uint32_t>(schema.types.size());
         schema.type_map[td.name]   = idx;
         schema.type_map[td.full_name] = idx;
         schema.types.push_back(std::move(td));
      }

      // ----- Enum -----

      void parse_enum(fbs_schema& schema)
      {
         expect(tok::kw_enum);
         auto name = expect_ident();

         fbs_type_def td;
         td.name = std::string(name.text);
         td.kind = fbs_type_kind::enum_;
         if (!schema.ns.empty())
            td.full_name = schema.ns + "." + td.name;
         else
            td.full_name = td.name;

         // : underlying_type
         expect(tok::colon);
         td.underlying_type = parse_scalar_base_type();

         // Optional metadata
         if (peek_is(tok::lparen))
            skip_metadata();

         expect(tok::lbrace);

         int64_t next_value = 0;
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            auto val_name = expect_ident();

            fbs_enum_val ev;
            ev.name = std::string(val_name.text);

            if (peek_is(tok::equal))
            {
               lex_.next();
               ev.value   = parse_integer_value();
               next_value = ev.value + 1;
            }
            else
            {
               ev.value = next_value++;
            }

            td.enum_values.push_back(std::move(ev));

            // Optional comma or semicolon
            if (peek_is(tok::comma))
               lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx               = static_cast<uint32_t>(schema.types.size());
         schema.type_map[td.name]   = idx;
         schema.type_map[td.full_name] = idx;
         schema.types.push_back(std::move(td));
      }

      // ----- Union -----

      void parse_union(fbs_schema& schema)
      {
         expect(tok::kw_union);
         auto name = expect_ident();

         fbs_type_def td;
         td.name = std::string(name.text);
         td.kind = fbs_type_kind::union_;
         if (!schema.ns.empty())
            td.full_name = schema.ns + "." + td.name;
         else
            td.full_name = td.name;

         // Optional metadata
         if (peek_is(tok::lparen))
            skip_metadata();

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            // Union member: TypeName or aliased_name:TypeName
            auto first = expect_ident();
            fbs_union_member um;

            if (peek_is(tok::colon))
            {
               // aliased_name:TypeName
               lex_.next();
               auto type_name = expect_ident();
               um.name        = std::string(first.text);
               // type_idx resolved later by name = type_name.text
               // Store the type name temporarily in name for resolution
               // Actually we need both names. Store the type name for resolution:
               um.type_idx = -1;  // resolved in resolve_types
               // We need a way to remember the type name. Use a separate mechanism:
               // Store type name in the name field with a special prefix? No.
               // Simpler: just store both. The fbs_union_member has name for the alias.
               // We need the type name for resolution. Let's add it.
               // For now, store the type name as a second string. Since we can't
               // add fields retroactively, we'll use a workaround: store the
               // unresolved type name in a temporary map.
               // Actually, let's just store the type reference name in the name
               // field when there's no alias, and handle aliased case with the
               // struct field as-is. We resolve by scanning types by name.
               // For aliased unions, store alias as name and resolve type from
               // the identifier after ':'. But we need to remember the type name.
               // Simple fix: use type_idx as a negative sentinel and store the
               // index of the type by looking it up at resolution time.
               // Let's just push a placeholder with name=type_name and resolve later.
               um.name = std::string(type_name.text);  // Use type name for resolution
               // Store alias separately (we'll lose it for now — not needed for layout)
            }
            else
            {
               um.name = std::string(first.text);
            }

            td.union_members.push_back(std::move(um));

            if (peek_is(tok::comma))
               lex_.next();
         }
         expect(tok::rbrace);

         uint32_t idx               = static_cast<uint32_t>(schema.types.size());
         schema.type_map[td.name]   = idx;
         schema.type_map[td.full_name] = idx;
         schema.types.push_back(std::move(td));
      }

      // ----- Field parsing (for table and struct) -----

      void parse_field(fbs_type_def& td)
      {
         auto field_name_tok = expect_ident();

         fbs_field_def field;
         field.name = std::string(field_name_tok.text);

         expect(tok::colon);

         // Parse type
         parse_field_type(field);

         // Optional default value
         if (peek_is(tok::equal))
         {
            lex_.next();
            parse_default_value(field);
         }

         // Optional metadata in parens
         if (peek_is(tok::lparen))
         {
            parse_field_metadata(field);
         }

         expect(tok::semicolon);

         td.fields.push_back(std::move(field));
      }

      // Parse field type: scalar, string, [vector], or type reference
      void parse_field_type(fbs_field_def& field)
      {
         auto t = lex_.peek();

         // Vector type: [element_type]
         if (t.kind == tok::lbracket)
         {
            lex_.next();
            field.type = fbs_base_type::vector_;
            // Parse element type
            auto elem_tok = lex_.peek();
            if (is_scalar_type_tok(elem_tok.kind))
            {
               lex_.next();
               field.elem_type = scalar_tok_to_base_type(elem_tok.kind);
            }
            else if (elem_tok.kind == tok::kw_string)
            {
               lex_.next();
               field.elem_type = fbs_base_type::string_;
            }
            else if (elem_tok.kind == tok::ident)
            {
               lex_.next();
               // Type reference — resolved later
               field.elem_type        = fbs_base_type::table_;  // placeholder
               field.elem_type_idx    = -1;
               field.unresolved_elem_name = std::string(elem_tok.text);  // store name for resolution
            }
            else
            {
               error(elem_tok, "expected type in vector declaration");
            }
            expect(tok::rbracket);
            return;
         }

         // Scalar types
         if (is_scalar_type_tok(t.kind))
         {
            lex_.next();
            field.type = scalar_tok_to_base_type(t.kind);
            return;
         }

         // String
         if (t.kind == tok::kw_string)
         {
            lex_.next();
            field.type = fbs_base_type::string_;
            return;
         }

         // Type reference (table, struct, enum, or union)
         if (t.kind == tok::ident)
         {
            lex_.next();
            std::string type_name(t.text);
            // May have dots for qualified names
            while (peek_is(tok::dot))
            {
               lex_.next();
               auto part = expect_ident();
               type_name += ".";
               type_name += part.text;
            }
            // Store as unresolved reference; resolve_types() will fill in type/type_idx
            field.type           = fbs_base_type::table_;  // placeholder
            field.type_idx       = -1;
            field.unresolved_type_name = type_name;  // store name for resolution
            return;
         }

         error(t, "expected field type");
      }

      // Parse default value after '='
      void parse_default_value(fbs_field_def& field)
      {
         auto t = lex_.peek();

         if (t.kind == tok::integer)
         {
            lex_.next();
            field.default_int = parse_integer_literal(t.text);
            field.has_default = true;
         }
         else if (t.kind == tok::float_lit)
         {
            lex_.next();
            field.default_float = parse_float_literal(t.text);
            field.has_default   = true;
         }
         else if (t.kind == tok::ident)
         {
            // Could be an enum value name, true, false, nan, inf, etc.
            lex_.next();
            auto val = t.text;
            if (val == "true")
            {
               field.default_int = 1;
               field.has_default = true;
            }
            else if (val == "false")
            {
               field.default_int = 0;
               field.has_default = true;
            }
            else if (val == "nan")
            {
               field.default_float = std::numeric_limits<double>::quiet_NaN();
               field.has_default   = true;
            }
            else if (val == "inf" || val == "infinity")
            {
               field.default_float = std::numeric_limits<double>::infinity();
               field.has_default   = true;
            }
            else
            {
               // Enum value name — store as string default
               field.default_string = std::string(val);
               field.has_default    = true;
            }
         }
         else if (t.kind == tok::string_lit)
         {
            lex_.next();
            field.default_string = std::string(t.text);
            field.has_default    = true;
         }
         else
         {
            // Negative number
            if (t.text == "-")
            {
               lex_.next();
               auto num = lex_.next();
               if (num.kind == tok::integer)
               {
                  field.default_int = -parse_integer_literal(num.text);
                  field.has_default = true;
               }
               else if (num.kind == tok::float_lit)
               {
                  field.default_float = -parse_float_literal(num.text);
                  field.has_default   = true;
               }
               else if (num.kind == tok::ident)
               {
                  auto val = num.text;
                  if (val == "inf" || val == "infinity")
                  {
                     field.default_float = -std::numeric_limits<double>::infinity();
                     field.has_default   = true;
                  }
               }
            }
         }
      }

      // Parse field metadata: (deprecated, id: N, required, ...)
      void parse_field_metadata(fbs_field_def& field)
      {
         expect(tok::lparen);
         while (!peek_is(tok::rparen) && !peek_is(tok::eof))
         {
            auto attr = expect_ident();
            auto attr_name = attr.text;

            if (attr_name == "deprecated")
            {
               field.metadata.deprecated = true;
            }
            else if (attr_name == "id")
            {
               expect(tok::colon);
               auto val              = expect(tok::integer);
               field.metadata.id     = static_cast<int32_t>(parse_integer_literal(val.text));
            }
            else if (attr_name == "required")
            {
               field.metadata.required = true;
            }
            else
            {
               // Unknown attribute — skip optional value
               if (peek_is(tok::colon))
               {
                  lex_.next();
                  // Skip the value (could be number, string, or ident)
                  lex_.next();
               }
            }

            if (peek_is(tok::comma))
               lex_.next();
         }
         expect(tok::rparen);
      }

      // Skip metadata block on types
      void skip_metadata()
      {
         expect(tok::lparen);
         int depth = 1;
         while (depth > 0 && !peek_is(tok::eof))
         {
            auto t = lex_.next();
            if (t.kind == tok::lparen)
               depth++;
            else if (t.kind == tok::rparen)
               depth--;
         }
      }

      // ----- Top-level directives -----

      void parse_root_type(fbs_schema& schema)
      {
         expect(tok::kw_root_type);
         auto name      = expect_ident();
         schema.root_type = std::string(name.text);
         // May have dots
         while (peek_is(tok::dot))
         {
            lex_.next();
            auto part = expect_ident();
            schema.root_type += ".";
            schema.root_type += part.text;
         }
         expect(tok::semicolon);
      }

      void parse_file_identifier(fbs_schema& schema)
      {
         expect(tok::kw_file_identifier);
         auto str                = expect(tok::string_lit);
         schema.file_identifier  = std::string(str.text);
         expect(tok::semicolon);
      }

      void parse_file_extension(fbs_schema& schema)
      {
         expect(tok::kw_file_extension);
         auto str              = expect(tok::string_lit);
         schema.file_extension = std::string(str.text);
         expect(tok::semicolon);
      }

      void parse_include(fbs_schema& schema)
      {
         expect(tok::kw_include);
         auto str = expect(tok::string_lit);
         schema.includes.push_back(std::string(str.text));
         expect(tok::semicolon);
      }

      void parse_attribute(fbs_schema& schema)
      {
         expect(tok::kw_attribute);
         if (peek_is(tok::string_lit))
         {
            auto str = lex_.next();
            schema.attributes.push_back(std::string(str.text));
         }
         else
         {
            auto name = expect_ident();
            schema.attributes.push_back(std::string(name.text));
         }
         expect(tok::semicolon);
      }

      void skip_rpc_service()
      {
         expect(tok::kw_rpc_service);
         expect_ident();
         expect(tok::lbrace);
         int depth = 1;
         while (depth > 0 && !peek_is(tok::eof))
         {
            auto t = lex_.next();
            if (t.kind == tok::lbrace)
               depth++;
            else if (t.kind == tok::rbrace)
               depth--;
         }
      }

      // ----- Scalar type helpers -----

      static bool is_scalar_type_tok(tok k)
      {
         switch (k)
         {
            case tok::kw_bool:
            case tok::kw_int8:
            case tok::kw_uint8:
            case tok::kw_int16:
            case tok::kw_uint16:
            case tok::kw_int32:
            case tok::kw_uint32:
            case tok::kw_int64:
            case tok::kw_uint64:
            case tok::kw_float32:
            case tok::kw_float64:
               return true;
            default:
               return false;
         }
      }

      static fbs_base_type scalar_tok_to_base_type(tok k)
      {
         switch (k)
         {
            case tok::kw_bool:
               return fbs_base_type::bool_;
            case tok::kw_int8:
               return fbs_base_type::int8_;
            case tok::kw_uint8:
               return fbs_base_type::uint8_;
            case tok::kw_int16:
               return fbs_base_type::int16_;
            case tok::kw_uint16:
               return fbs_base_type::uint16_;
            case tok::kw_int32:
               return fbs_base_type::int32_;
            case tok::kw_uint32:
               return fbs_base_type::uint32_;
            case tok::kw_int64:
               return fbs_base_type::int64_;
            case tok::kw_uint64:
               return fbs_base_type::uint64_;
            case tok::kw_float32:
               return fbs_base_type::float32_;
            case tok::kw_float64:
               return fbs_base_type::float64_;
            default:
               return fbs_base_type::none_;
         }
      }

      fbs_base_type parse_scalar_base_type()
      {
         auto t = lex_.next();
         if (is_scalar_type_tok(t.kind))
            return scalar_tok_to_base_type(t.kind);
         error(t, "expected scalar type (bool, byte, ubyte, short, ushort, "
                   "int, uint, long, ulong, float, double)");
      }

      // ----- Number parsing -----

      static int64_t parse_integer_literal(std::string_view text)
      {
         if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
         {
            // Hex
            uint64_t val = 0;
            for (size_t i = 2; i < text.size(); ++i)
            {
               val <<= 4;
               char c = text[i];
               if (c >= '0' && c <= '9')
                  val |= static_cast<uint64_t>(c - '0');
               else if (c >= 'a' && c <= 'f')
                  val |= static_cast<uint64_t>(c - 'a' + 10);
               else if (c >= 'A' && c <= 'F')
                  val |= static_cast<uint64_t>(c - 'A' + 10);
            }
            return static_cast<int64_t>(val);
         }
         // Decimal
         int64_t val  = 0;
         bool    neg  = false;
         size_t  i    = 0;
         if (i < text.size() && (text[i] == '-' || text[i] == '+'))
         {
            neg = (text[i] == '-');
            ++i;
         }
         for (; i < text.size(); ++i)
         {
            val = val * 10 + (text[i] - '0');
         }
         return neg ? -val : val;
      }

      int64_t parse_integer_value()
      {
         bool neg = false;
         if (peek_is(tok::ident))
         {
            // Could be a negative sign that got lexed oddly, or an enum ident
            auto t = lex_.peek();
            if (t.text == "-")
            {
               neg = true;
               lex_.next();
            }
         }
         auto t = lex_.next();
         if (t.kind == tok::integer)
         {
            auto val = parse_integer_literal(t.text);
            return neg ? -val : val;
         }
         error(t, "expected integer value");
      }

      static double parse_float_literal(std::string_view text)
      {
         // Use standard library parsing
         std::string s(text);
         return std::stod(s);
      }

      // =====================================================================
      // Post-parse: type resolution
      // =====================================================================

      void resolve_types(fbs_schema& schema)
      {
         for (auto& td : schema.types)
         {
            if (td.kind == fbs_type_kind::table_ || td.kind == fbs_type_kind::struct_)
            {
               for (auto& field : td.fields)
               {
                  resolve_field_type(schema, field);
               }
            }
            else if (td.kind == fbs_type_kind::union_)
            {
               for (auto& um : td.union_members)
               {
                  auto it = schema.type_map.find(um.name);
                  if (it != schema.type_map.end())
                     um.type_idx = static_cast<int32_t>(it->second);
               }
            }
         }
      }

      void resolve_field_type(fbs_schema& schema, fbs_field_def& field)
      {
         // If type is a placeholder table_ with type_idx == -1, resolve by name
         if (field.type == fbs_base_type::table_ && field.type_idx == -1 &&
             !field.unresolved_type_name.empty())
         {
            const auto& type_name = field.unresolved_type_name;
            auto        it        = schema.type_map.find(type_name);
            if (it != schema.type_map.end())
            {
               auto& ref_type = schema.types[it->second];
               switch (ref_type.kind)
               {
                  case fbs_type_kind::table_:
                     field.type     = fbs_base_type::table_;
                     field.type_idx = static_cast<int32_t>(it->second);
                     break;
                  case fbs_type_kind::struct_:
                     field.type     = fbs_base_type::struct_;
                     field.type_idx = static_cast<int32_t>(it->second);
                     break;
                  case fbs_type_kind::enum_:
                     field.type     = fbs_base_type::enum_;
                     field.type_idx = static_cast<int32_t>(it->second);
                     break;
                  case fbs_type_kind::union_:
                     field.type     = fbs_base_type::union_;
                     field.type_idx = static_cast<int32_t>(it->second);
                     break;
               }
               field.unresolved_type_name.clear();
            }
            // If not found, leave as unresolved table_ reference
         }

         // Resolve vector element type
         if (field.type == fbs_base_type::vector_ && field.elem_type == fbs_base_type::table_ &&
             field.elem_type_idx == -1 && !field.unresolved_elem_name.empty())
         {
            const auto& type_name = field.unresolved_elem_name;
            auto        it        = schema.type_map.find(type_name);
            if (it != schema.type_map.end())
            {
               auto& ref_type = schema.types[it->second];
               switch (ref_type.kind)
               {
                  case fbs_type_kind::table_:
                     field.elem_type     = fbs_base_type::table_;
                     field.elem_type_idx = static_cast<int32_t>(it->second);
                     break;
                  case fbs_type_kind::struct_:
                     field.elem_type     = fbs_base_type::struct_;
                     field.elem_type_idx = static_cast<int32_t>(it->second);
                     break;
                  case fbs_type_kind::enum_:
                     field.elem_type     = fbs_base_type::enum_;
                     field.elem_type_idx = static_cast<int32_t>(it->second);
                     break;
                  default:
                     break;
               }
               field.unresolved_elem_name.clear();
            }
         }
      }

      // =====================================================================
      // Post-parse: layout computation
      // =====================================================================

      void compute_layouts(fbs_schema& schema)
      {
         for (auto& td : schema.types)
         {
            if (td.kind == fbs_type_kind::table_)
               compute_table_layout(schema, td);
            else if (td.kind == fbs_type_kind::struct_)
               compute_struct_layout(schema, td);
         }
      }

      void compute_table_layout(const fbs_schema& schema, fbs_type_def& td)
      {
         // Assign vtable slots.
         // If any field has (id: N) metadata, use explicit IDs.
         // Otherwise, assign sequentially by declaration order.
         // Union fields take 2 slots (discriminant u8 + offset).

         bool has_explicit_ids = false;
         for (const auto& f : td.fields)
         {
            if (f.metadata.id >= 0)
            {
               has_explicit_ids = true;
               break;
            }
         }

         if (has_explicit_ids)
         {
            // Use explicit IDs
            for (auto& f : td.fields)
            {
               if (f.metadata.id >= 0)
                  f.vtable_slot = f.metadata.id;
               else
                  f.vtable_slot = 0;  // default if not specified
            }
         }
         else
         {
            // Sequential assignment
            int slot = 0;
            for (auto& f : td.fields)
            {
               f.vtable_slot = slot;
               if (f.type == fbs_base_type::union_)
                  slot += 2;  // type byte + offset
               else
                  slot += 1;
            }
         }

         // Compute max slot
         int max_slot = 0;
         for (const auto& f : td.fields)
         {
            int end_slot = f.vtable_slot;
            if (f.type == fbs_base_type::union_)
               end_slot += 2;
            else
               end_slot += 1;
            max_slot = std::max(max_slot, end_slot);
         }
         td.vtable_slot_count = static_cast<uint16_t>(max_slot);

         // Set wire size and alignment for each field
         for (auto& f : td.fields)
         {
            compute_field_wire_info(schema, f);
         }
      }

      void compute_struct_layout(const fbs_schema& schema, fbs_type_def& td)
      {
         uint32_t offset    = 0;
         uint32_t max_align = 1;

         for (auto& f : td.fields)
         {
            compute_field_wire_info(schema, f);

            uint32_t align = f.wire_align;
            if (align == 0)
               align = 1;
            max_align = std::max(max_align, align);

            // Align the offset
            offset = (offset + align - 1) & ~(align - 1);
            f.struct_offset = offset;
            offset += f.wire_size;
         }

         // Pad to alignment
         td.struct_align = max_align;
         td.struct_size  = (offset + max_align - 1) & ~(max_align - 1);
      }

      void compute_field_wire_info(const fbs_schema& schema, fbs_field_def& f)
      {
         switch (f.type)
         {
            case fbs_base_type::bool_:
               f.wire_size      = 1;
               f.wire_align     = 1;
               f.is_offset_type = false;
               break;
            case fbs_base_type::int8_:
            case fbs_base_type::uint8_:
               f.wire_size      = 1;
               f.wire_align     = 1;
               f.is_offset_type = false;
               break;
            case fbs_base_type::int16_:
            case fbs_base_type::uint16_:
               f.wire_size      = 2;
               f.wire_align     = 2;
               f.is_offset_type = false;
               break;
            case fbs_base_type::int32_:
            case fbs_base_type::uint32_:
            case fbs_base_type::float32_:
               f.wire_size      = 4;
               f.wire_align     = 4;
               f.is_offset_type = false;
               break;
            case fbs_base_type::int64_:
            case fbs_base_type::uint64_:
            case fbs_base_type::float64_:
               f.wire_size      = 8;
               f.wire_align     = 8;
               f.is_offset_type = false;
               break;
            case fbs_base_type::string_:
               f.wire_size      = 4;
               f.wire_align     = 4;
               f.is_offset_type = true;
               break;
            case fbs_base_type::vector_:
               f.wire_size      = 4;
               f.wire_align     = 4;
               f.is_offset_type = true;
               break;
            case fbs_base_type::table_:
               f.wire_size      = 4;
               f.wire_align     = 4;
               f.is_offset_type = true;
               break;
            case fbs_base_type::struct_:
               if (f.type_idx >= 0 && f.type_idx < static_cast<int32_t>(schema.types.size()))
               {
                  const auto& ref = schema.types[static_cast<size_t>(f.type_idx)];
                  f.wire_size     = static_cast<uint8_t>(ref.struct_size);
                  f.wire_align    = static_cast<uint8_t>(ref.struct_align);
               }
               f.is_offset_type = false;
               break;
            case fbs_base_type::enum_:
               // Size depends on underlying type
               if (f.type_idx >= 0 && f.type_idx < static_cast<int32_t>(schema.types.size()))
               {
                  const auto& ref = schema.types[static_cast<size_t>(f.type_idx)];
                  auto        sz  = base_type_size(ref.underlying_type);
                  f.wire_size     = sz;
                  f.wire_align    = sz;
               }
               else
               {
                  f.wire_size  = 4;
                  f.wire_align = 4;
               }
               f.is_offset_type = false;
               break;
            case fbs_base_type::union_:
               // Union: 2 vtable slots — u8 discriminant + u32 offset
               // The discriminant is separate from the offset
               f.wire_size      = 4;  // the offset part
               f.wire_align     = 4;
               f.is_offset_type = true;
               break;
            default:
               break;
         }
      }

      static uint8_t base_type_size(fbs_base_type bt)
      {
         switch (bt)
         {
            case fbs_base_type::bool_:
            case fbs_base_type::int8_:
            case fbs_base_type::uint8_:
               return 1;
            case fbs_base_type::int16_:
            case fbs_base_type::uint16_:
               return 2;
            case fbs_base_type::int32_:
            case fbs_base_type::uint32_:
            case fbs_base_type::float32_:
               return 4;
            case fbs_base_type::int64_:
            case fbs_base_type::uint64_:
            case fbs_base_type::float64_:
               return 8;
            default:
               return 4;
         }
      }

      // =====================================================================
      // Post-parse: build dynamic_schema for runtime access
      // =====================================================================

      static dynamic_type fbs_to_dynamic_type(fbs_base_type bt)
      {
         switch (bt)
         {
            case fbs_base_type::bool_:
               return dynamic_type::t_bool;
            case fbs_base_type::int8_:
               return dynamic_type::t_i8;
            case fbs_base_type::uint8_:
               return dynamic_type::t_u8;
            case fbs_base_type::int16_:
               return dynamic_type::t_i16;
            case fbs_base_type::uint16_:
               return dynamic_type::t_u16;
            case fbs_base_type::int32_:
               return dynamic_type::t_i32;
            case fbs_base_type::uint32_:
               return dynamic_type::t_u32;
            case fbs_base_type::int64_:
               return dynamic_type::t_i64;
            case fbs_base_type::uint64_:
               return dynamic_type::t_u64;
            case fbs_base_type::float32_:
               return dynamic_type::t_f32;
            case fbs_base_type::float64_:
               return dynamic_type::t_f64;
            case fbs_base_type::string_:
               return dynamic_type::t_text;
            case fbs_base_type::vector_:
               return dynamic_type::t_vector;
            case fbs_base_type::table_:
            case fbs_base_type::struct_:
               return dynamic_type::t_struct;
            case fbs_base_type::enum_:
               // Enums present as their underlying integer type
               return dynamic_type::t_i32;
            case fbs_base_type::union_:
               return dynamic_type::t_variant;
            default:
               return dynamic_type::t_void;
         }
      }

      void build_dynamic_schemas(fbs_schema& schema)
      {
         // Build dynamic_schema for each table/struct type
         for (auto& td : schema.types)
         {
            if (td.kind != fbs_type_kind::table_ && td.kind != fbs_type_kind::struct_)
               continue;

            size_t nfields = td.fields.size();
            td.dyn_fields.resize(nfields);
            td.dyn_field_names.resize(nfields);
            td.dyn_ordered_names.resize(nfields);
            td.dyn_tags.resize(nfields);

            for (size_t i = 0; i < nfields; ++i)
            {
               const auto& sf  = td.fields[i];
               auto&       df  = td.dyn_fields[i];
               uint64_t    h   = xxh64_hash(sf.name);

               td.dyn_field_names[i] = sf.name;

               df.name_hash = h;
               df.name      = nullptr;  // set after sorting
               df.type      = fbs_to_dynamic_type(sf.type);
               df.byte_size = sf.wire_size;

               // For enum fields, use the dynamic_type corresponding to the underlying type
               if (sf.type == fbs_base_type::enum_ && sf.type_idx >= 0 &&
                   sf.type_idx < static_cast<int32_t>(schema.types.size()))
               {
                  const auto& enum_def = schema.types[static_cast<size_t>(sf.type_idx)];
                  df.type              = fbs_to_dynamic_type(enum_def.underlying_type);
                  df.byte_size         = base_type_size(enum_def.underlying_type);
               }

               if (td.kind == fbs_type_kind::table_)
               {
                  // For FlatBuffer tables, offset is the vtable byte offset:
                  // vtable_offset = 4 + 2 * vtable_slot
                  df.offset = static_cast<uint32_t>(4 + 2 * sf.vtable_slot);
                  df.is_ptr = sf.is_offset_type;
               }
               else
               {
                  // For structs, offset is the byte offset within the struct
                  df.offset = sf.struct_offset;
                  df.is_ptr = false;
               }

               // Set nested schema pointer for table/struct references
               if ((sf.type == fbs_base_type::table_ || sf.type == fbs_base_type::struct_) &&
                   sf.type_idx >= 0 && sf.type_idx < static_cast<int32_t>(schema.types.size()))
               {
                  // Will be set after all schemas are built (second pass)
                  df.nested = nullptr;
               }

               // Union/variant support
               if (sf.type == fbs_base_type::union_)
               {
                  // discriminant is at vtable_slot, value at vtable_slot+1
                  df.disc_offset = static_cast<uint32_t>(4 + 2 * sf.vtable_slot);
                  df.offset      = static_cast<uint32_t>(4 + 2 * (sf.vtable_slot + 1));
               }
            }

            // Sort by name_hash for SIMD-friendly tag lookup
            // Build an index array so we can reorder
            std::vector<size_t> sort_order(nfields);
            for (size_t i = 0; i < nfields; ++i)
               sort_order[i] = i;
            std::sort(sort_order.begin(), sort_order.end(),
                      [&](size_t a, size_t b)
                      { return td.dyn_fields[a].name_hash < td.dyn_fields[b].name_hash; });

            // Apply sort
            std::vector<dynamic_field_desc> sorted_fields(nfields);
            for (size_t i = 0; i < nfields; ++i)
            {
               sorted_fields[i] = td.dyn_fields[sort_order[i]];
            }
            td.dyn_fields = std::move(sorted_fields);

            // Set name pointers and ordered names (declaration order)
            for (size_t i = 0; i < nfields; ++i)
            {
               td.dyn_ordered_names[i] = td.dyn_field_names[i].c_str();
            }

            // Set name pointers in sorted fields
            for (size_t i = 0; i < nfields; ++i)
            {
               size_t orig_idx   = sort_order[i];
               td.dyn_fields[i].name = td.dyn_field_names[orig_idx].c_str();
               td.dyn_tags[i]        = static_cast<uint8_t>(td.dyn_fields[i].name_hash);
            }

            // Build the dynamic_schema
            td.dyn_schema = std::make_unique<dynamic_schema>();
            td.dyn_schema->sorted_fields = td.dyn_fields.data();
            td.dyn_schema->field_count   = nfields;
            td.dyn_schema->ordered_names = td.dyn_ordered_names.data();
            td.dyn_schema->tags          = td.dyn_tags.data();
            // data_words and ptr_count are Cap'n Proto concepts; for FlatBuffers
            // we set them based on the vtable slot count (approximate)
            td.dyn_schema->data_words = 0;
            td.dyn_schema->ptr_count  = 0;
         }

         // Second pass: set nested schema pointers
         for (auto& td : schema.types)
         {
            if (td.kind != fbs_type_kind::table_ && td.kind != fbs_type_kind::struct_)
               continue;

            for (size_t i = 0; i < td.dyn_fields.size(); ++i)
            {
               // Find the original field for this sorted entry
               auto field_name_sv = std::string_view(td.dyn_fields[i].name);
               for (const auto& sf : td.fields)
               {
                  if (sf.name == field_name_sv)
                  {
                     int32_t ref_idx = sf.type_idx;
                     if ((sf.type == fbs_base_type::table_ ||
                          sf.type == fbs_base_type::struct_) &&
                         ref_idx >= 0 &&
                         ref_idx < static_cast<int32_t>(schema.types.size()))
                     {
                        auto& ref_type = schema.types[static_cast<size_t>(ref_idx)];
                        if (ref_type.dyn_schema)
                           td.dyn_fields[i].nested = ref_type.dyn_schema.get();
                     }
                     break;
                  }
               }
            }
         }
      }
   };

   // =========================================================================
   // Convenience free function
   // =========================================================================

   inline fbs_schema fbs_parse(std::string_view source)
   {
      return fbs_parser::parse_fbs(source);
   }

}  // namespace psio1
