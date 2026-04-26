// capnp_parser.hpp — Recursive-descent parser for Cap'n Proto .capnp IDL text
//
// Parses Cap'n Proto schema language into a runtime capnp_parsed_schema tree
// with computed wire layout (data_words, ptr_count, per-field FieldLoc).
//
//   auto schema = psio1::capnp_parse(R"(
//       @0xdeadbeef;
//       struct Point {
//          x @0 :Float64;
//          y @1 :Float64;
//       }
//   )");
//
// The parsed schema can be used for runtime introspection of Cap'n Proto wire
// data without requiring compile-time C++ types.
//
// Supported:
//   - struct with scalar, Text, Data, List, nested struct fields
//   - Field ordinals (@N)
//   - Anonymous unions inside structs
//   - Enums
//   - using Name = Type aliases
//   - Nested struct definitions
//   - Default values (parsed but stored as string; not used for XOR)
//   - # line comments
//   - Generic types: List(T)
//   - Built-in types: Void, Bool, Int8..Float64, Text, Data, AnyPointer

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psio1
{

   // =========================================================================
   // Parsed schema data structures
   // =========================================================================

   /// Capnp primitive/builtin type tags
   enum class capnp_type_tag : uint8_t
   {
      void_     = 0,
      bool_     = 1,
      int8      = 2,
      int16     = 3,
      int32     = 4,
      int64     = 5,
      uint8     = 6,
      uint16    = 7,
      uint32    = 8,
      uint64    = 9,
      float32   = 10,
      float64   = 11,
      text      = 12,
      data      = 13,
      list      = 14,  // parameterized: element type in element_type_idx
      struct_   = 15,  // reference to a parsed struct by index
      enum_     = 16,  // reference to a parsed enum by index
      any_ptr   = 17,
   };

   /// Location of a field within the capnp wire format
   struct capnp_field_loc
   {
      bool     is_ptr    = false;  // true = pointer section, false = data section
      uint32_t offset    = 0;      // byte offset in data section, or index in ptr section
      uint8_t  bit_index = 0;      // for bool: bit within the byte (0-7)
   };

   /// A type reference — either a builtin or a reference to a compound type
   struct capnp_type_ref
   {
      capnp_type_tag tag                = capnp_type_tag::void_;
      int32_t        element_type_idx   = -1;  // for list: index into file.types
      int32_t        referenced_type_idx = -1;  // for struct/enum: index into file.structs/enums
   };

   /// A parsed field within a struct
   struct capnp_parsed_field
   {
      std::string    name;
      uint32_t       ordinal    = 0;
      capnp_type_ref type;
      std::string    default_value;  // raw text of default value, empty if none
      capnp_field_loc loc;           // computed layout location
   };

   /// A parsed union within a struct
   struct capnp_parsed_union
   {
      std::vector<capnp_parsed_field> alternatives;
      capnp_field_loc                 discriminant_loc;  // location of u16 discriminant
   };

   /// A parsed enum definition
   struct capnp_parsed_enum
   {
      std::string              name;
      uint64_t                 id = 0;
      std::vector<std::string> enumerants;  // in ordinal order
   };

   /// A parsed struct definition (may be nested)
   struct capnp_parsed_struct
   {
      std::string                      name;
      uint64_t                         id = 0;
      std::vector<capnp_parsed_field>  fields;     // non-union fields
      std::vector<capnp_parsed_union>  unions;      // anonymous unions
      uint16_t                         data_words = 0;
      uint16_t                         ptr_count  = 0;

      // nested types are stored in the file-level vectors and referenced by index
   };

   /// A type alias (using Name = Type)
   struct capnp_type_alias
   {
      std::string    name;
      capnp_type_ref type;
   };

   /// A complete parsed .capnp file
   struct capnp_parsed_file
   {
      uint64_t                          file_id = 0;
      std::vector<capnp_parsed_struct>  structs;
      std::vector<capnp_parsed_enum>    enums;
      std::vector<capnp_type_alias>     aliases;

      // Type reference table: maps type names to their kind + index
      // This allows List(MyStruct) to resolve MyStruct
      std::unordered_map<std::string, capnp_type_ref> type_map;

      /// Lookup a struct by name, returns nullptr if not found
      const capnp_parsed_struct* find_struct(const std::string& name) const
      {
         for (auto& s : structs)
            if (s.name == name)
               return &s;
         return nullptr;
      }

      /// Lookup an enum by name, returns nullptr if not found
      const capnp_parsed_enum* find_enum(const std::string& name) const
      {
         for (auto& e : enums)
            if (e.name == name)
               return &e;
         return nullptr;
      }
   };

   // =========================================================================
   // Parser error
   // =========================================================================

   struct capnp_parse_error : std::runtime_error
   {
      uint32_t line;
      uint32_t column;
      capnp_parse_error(const std::string& msg, uint32_t line, uint32_t col)
          : std::runtime_error("capnp:" + std::to_string(line) + ":" + std::to_string(col) +
                               ": " + msg),
            line(line),
            column(col)
      {
      }
   };

   // =========================================================================
   // Lexer
   // =========================================================================

   namespace capnp_parser_detail
   {

      enum class tok : uint8_t
      {
         eof,
         ident,
         number,
         string_lit,
         // punctuation
         lbrace,
         rbrace,
         lparen,
         rparen,
         at,
         colon,
         semicolon,
         equal,
         dot,
         comma,
         // keywords
         kw_struct,
         kw_union,
         kw_enum,
         kw_using,
         kw_const,
         kw_annotation,
         kw_interface,
         kw_import,
         // type keywords
         kw_void,
         kw_bool,
         kw_int8,
         kw_int16,
         kw_int32,
         kw_int64,
         kw_uint8,
         kw_uint16,
         kw_uint32,
         kw_uint64,
         kw_float32,
         kw_float64,
         kw_text,
         kw_data,
         kw_list,
         kw_any_pointer,
         // literals
         kw_true,
         kw_false,
      };

      struct token
      {
         tok              kind = tok::eof;
         std::string_view text;
         uint32_t         line = 1;
         uint32_t         col  = 1;
      };

      class lexer
      {
       public:
         explicit lexer(std::string_view src) : src_(src), pos_(0), line_(1), col_(1) {}

         token next()
         {
            skip_ws_and_comments();
            if (pos_ >= src_.size())
               return {tok::eof, {}, line_, col_};

            uint32_t start_line = line_;
            uint32_t start_col  = col_;
            char     c          = src_[pos_];

            // Single-char punctuation
            auto single = [&](tok k) -> token
            {
               auto sv = src_.substr(pos_, 1);
               advance(1);
               return {k, sv, start_line, start_col};
            };

            switch (c)
            {
               case '{':
                  return single(tok::lbrace);
               case '}':
                  return single(tok::rbrace);
               case '(':
                  return single(tok::lparen);
               case ')':
                  return single(tok::rparen);
               case '@':
                  return single(tok::at);
               case ':':
                  return single(tok::colon);
               case ';':
                  return single(tok::semicolon);
               case '=':
                  return single(tok::equal);
               case '.':
                  return single(tok::dot);
               case ',':
                  return single(tok::comma);
               default:
                  break;
            }

            // String literals
            if (c == '"')
            {
               return lex_string();
            }

            // Numbers (decimal, hex, octal, float, negative)
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

            throw capnp_parse_error(
                std::string("unexpected character '") + c + "'", line_, col_);
         }

         token peek()
         {
            auto saved_pos  = pos_;
            auto saved_line = line_;
            auto saved_col  = col_;
            auto t          = next();
            pos_             = saved_pos;
            line_            = saved_line;
            col_             = saved_col;
            return t;
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
               // # line comments
               if (c == '#')
               {
                  while (pos_ < src_.size() && src_[pos_] != '\n')
                     advance(1);
                  continue;
               }
               break;
            }
         }

         static bool is_ident_start(char c)
         {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
         }

         static bool is_ident_cont(char c)
         {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
         }

         token lex_string()
         {
            uint32_t start_line = line_;
            uint32_t start_col  = col_;
            size_t   start      = pos_;
            advance(1);  // skip opening quote
            while (pos_ < src_.size() && src_[pos_] != '"')
            {
               if (src_[pos_] == '\\')
                  advance(1);  // skip escaped char
               advance(1);
            }
            if (pos_ < src_.size())
               advance(1);  // skip closing quote
            return {tok::string_lit, src_.substr(start, pos_ - start), start_line, start_col};
         }

         token lex_number(uint32_t start_line, uint32_t start_col)
         {
            size_t start = pos_;
            if (src_[pos_] == '-' || src_[pos_] == '+')
               advance(1);

            // Hex: 0x...
            if (pos_ + 1 < src_.size() && src_[pos_] == '0' &&
                (src_[pos_ + 1] == 'x' || src_[pos_ + 1] == 'X'))
            {
               advance(2);
               while (pos_ < src_.size() &&
                      std::isxdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
               return {tok::number, src_.substr(start, pos_ - start), start_line, start_col};
            }

            // Decimal, octal, float
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])))
               advance(1);

            // Float: digits.digits or digits.digitsEexp
            if (pos_ < src_.size() && src_[pos_] == '.')
            {
               advance(1);
               while (pos_ < src_.size() &&
                      std::isdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
            }
            if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E'))
            {
               advance(1);
               if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                  advance(1);
               while (pos_ < src_.size() &&
                      std::isdigit(static_cast<unsigned char>(src_[pos_])))
                  advance(1);
            }

            // Handle special float values like "inf" and "nan" preceded by sign
            // (they'll be caught as ident)

            return {tok::number, src_.substr(start, pos_ - start), start_line, start_col};
         }

         static tok classify_keyword(std::string_view text)
         {
            if (text == "struct")
               return tok::kw_struct;
            if (text == "union")
               return tok::kw_union;
            if (text == "enum")
               return tok::kw_enum;
            if (text == "using")
               return tok::kw_using;
            if (text == "const")
               return tok::kw_const;
            if (text == "annotation")
               return tok::kw_annotation;
            if (text == "interface")
               return tok::kw_interface;
            if (text == "import")
               return tok::kw_import;
            // Types
            if (text == "Void")
               return tok::kw_void;
            if (text == "Bool")
               return tok::kw_bool;
            if (text == "Int8")
               return tok::kw_int8;
            if (text == "Int16")
               return tok::kw_int16;
            if (text == "Int32")
               return tok::kw_int32;
            if (text == "Int64")
               return tok::kw_int64;
            if (text == "UInt8")
               return tok::kw_uint8;
            if (text == "UInt16")
               return tok::kw_uint16;
            if (text == "UInt32")
               return tok::kw_uint32;
            if (text == "UInt64")
               return tok::kw_uint64;
            if (text == "Float32")
               return tok::kw_float32;
            if (text == "Float64")
               return tok::kw_float64;
            if (text == "Text")
               return tok::kw_text;
            if (text == "Data")
               return tok::kw_data;
            if (text == "List")
               return tok::kw_list;
            if (text == "AnyPointer")
               return tok::kw_any_pointer;
            // Literals
            if (text == "true")
               return tok::kw_true;
            if (text == "false")
               return tok::kw_false;
            if (text == "inf" || text == "nan")
               return tok::number;  // treat as number literal
            return tok::ident;
         }
      };

   }  // namespace capnp_parser_detail

   // =========================================================================
   // Layout computation
   // =========================================================================

   namespace capnp_parser_detail
   {
      /// Byte size for a data-section scalar type. Returns 0 for bool (bit-level)
      /// and 0 for pointer/void types (not in data section).
      inline uint32_t type_byte_size(capnp_type_tag tag)
      {
         switch (tag)
         {
            case capnp_type_tag::bool_:
               return 0;
            case capnp_type_tag::int8:
            case capnp_type_tag::uint8:
               return 1;
            case capnp_type_tag::int16:
            case capnp_type_tag::uint16:
            case capnp_type_tag::enum_:
               return 2;
            case capnp_type_tag::int32:
            case capnp_type_tag::uint32:
            case capnp_type_tag::float32:
               return 4;
            case capnp_type_tag::int64:
            case capnp_type_tag::uint64:
            case capnp_type_tag::float64:
               return 8;
            default:
               return 0;  // pointer or void
         }
      }

      /// Is this a data-section type (scalar/bool/enum)?
      inline bool is_data_type(capnp_type_tag tag)
      {
         switch (tag)
         {
            case capnp_type_tag::bool_:
            case capnp_type_tag::int8:
            case capnp_type_tag::int16:
            case capnp_type_tag::int32:
            case capnp_type_tag::int64:
            case capnp_type_tag::uint8:
            case capnp_type_tag::uint16:
            case capnp_type_tag::uint32:
            case capnp_type_tag::uint64:
            case capnp_type_tag::float32:
            case capnp_type_tag::float64:
            case capnp_type_tag::enum_:
               return true;
            default:
               return false;
         }
      }

      /// Is this a pointer-section type?
      inline bool is_ptr_type(capnp_type_tag tag)
      {
         switch (tag)
         {
            case capnp_type_tag::text:
            case capnp_type_tag::data:
            case capnp_type_tag::list:
            case capnp_type_tag::struct_:
            case capnp_type_tag::any_ptr:
               return true;
            default:
               return false;
         }
      }

      /// Compute the capnp wire layout for a parsed struct.
      /// This replicates the alloc_bits / slot-allocation algorithm from
      /// capnp_layout<T>::compute() but driven by runtime parsed types.
      ///
      /// Fields are processed in ordinal order.  Union alternatives share
      /// overlapping slots (each allocated independently, allowing the
      /// largest to define the struct size).  Discriminants for unions are
      /// allocated after all ordinal fields.
      inline void compute_layout(capnp_parsed_struct& s)
      {
         // Bit-level occupancy tracker (max 32 words = 2048 bits)
         bool occupied[2048] = {};

         uint16_t data_words = 0;
         uint16_t ptr_count  = 0;

         auto alloc_bits = [&](uint32_t bit_count, uint32_t bit_align) -> uint32_t
         {
            for (uint32_t bit = 0;; bit += bit_align)
            {
               uint32_t end_bit      = bit + bit_count;
               uint32_t words_needed = (end_bit + 63) / 64;
               if (words_needed > data_words)
                  data_words = static_cast<uint16_t>(words_needed);

               bool ok = true;
               for (uint32_t b = bit; b < end_bit; ++b)
               {
                  if (b < 2048 && occupied[b])
                  {
                     ok = false;
                     break;
                  }
               }
               if (ok)
               {
                  for (uint32_t b = bit; b < end_bit; ++b)
                  {
                     if (b < 2048)
                        occupied[b] = true;
                  }
                  return bit;
               }
            }
         };

         auto alloc_type_slot = [&](capnp_type_tag tag) -> capnp_field_loc
         {
            if (tag == capnp_type_tag::void_)
            {
               return {};  // no space
            }
            else if (is_ptr_type(tag))
            {
               return {true, ptr_count++, 0};
            }
            else if (tag == capnp_type_tag::bool_)
            {
               uint32_t bit = alloc_bits(1, 1);
               return {false, bit / 8, static_cast<uint8_t>(bit % 8)};
            }
            else
            {
               uint32_t sz  = type_byte_size(tag);
               uint32_t bit = alloc_bits(sz * 8, sz * 8);
               return {false, bit / 8, 0};
            }
         };

         // Collect all items to process in ordinal order.
         // We need to interleave regular fields and union alternatives by ordinal.
         // Strategy: build a flat list of (ordinal, type_tag) entries for allocation,
         // then assign back to fields/unions.

         // First: sort regular fields by ordinal
         std::sort(s.fields.begin(), s.fields.end(),
                   [](const capnp_parsed_field& a, const capnp_parsed_field& b)
                   { return a.ordinal < b.ordinal; });

         // For unions: sort alternatives within each union by ordinal
         for (auto& u : s.unions)
         {
            std::sort(u.alternatives.begin(), u.alternatives.end(),
                      [](const capnp_parsed_field& a, const capnp_parsed_field& b)
                      { return a.ordinal < b.ordinal; });
         }

         // Build merged ordinal list: (ordinal, source_type, source_index, sub_index)
         struct ordinal_entry
         {
            uint32_t ordinal;
            bool     is_union_alt;
            size_t   union_idx;
            size_t   alt_idx;
            size_t   field_idx;
         };

         std::vector<ordinal_entry> entries;

         for (size_t i = 0; i < s.fields.size(); ++i)
         {
            ordinal_entry e;
            e.ordinal      = s.fields[i].ordinal;
            e.is_union_alt = false;
            e.field_idx    = i;
            e.union_idx    = 0;
            e.alt_idx      = 0;
            entries.push_back(e);
         }

         for (size_t ui = 0; ui < s.unions.size(); ++ui)
         {
            for (size_t ai = 0; ai < s.unions[ui].alternatives.size(); ++ai)
            {
               ordinal_entry e;
               e.ordinal      = s.unions[ui].alternatives[ai].ordinal;
               e.is_union_alt = true;
               e.union_idx    = ui;
               e.alt_idx      = ai;
               e.field_idx    = 0;
               entries.push_back(e);
            }
         }

         // Sort by ordinal
         std::sort(entries.begin(), entries.end(),
                   [](const ordinal_entry& a, const ordinal_entry& b)
                   { return a.ordinal < b.ordinal; });

         // Allocate slots in ordinal order
         for (auto& e : entries)
         {
            if (e.is_union_alt)
            {
               auto& alt = s.unions[e.union_idx].alternatives[e.alt_idx];
               alt.loc   = alloc_type_slot(alt.type.tag);
            }
            else
            {
               auto& f = s.fields[e.field_idx];
               f.loc    = alloc_type_slot(f.type.tag);
            }
         }

         // Allocate discriminants for unions (uint16, after all ordinal fields)
         for (auto& u : s.unions)
         {
            uint32_t bit   = alloc_bits(16, 16);
            u.discriminant_loc = {false, bit / 8, 0};
         }

         s.data_words = data_words;
         s.ptr_count  = ptr_count;
      }

   }  // namespace capnp_parser_detail

   // =========================================================================
   // Parser
   // =========================================================================

   class capnp_parser
   {
    public:
      explicit capnp_parser(std::string_view source) : lex_(source) {}

      /// Parse the complete .capnp source into a capnp_parsed_file.
      capnp_parsed_file parse()
      {
         capnp_parsed_file file;

         // Parse optional file ID: @0xNNNN;
         if (peek_is(tok::at))
         {
            lex_.next();  // consume '@'
            auto num_tok = expect(tok::number);
            file.file_id = parse_uint64(num_tok.text);
            expect(tok::semicolon);
         }

         // Parse top-level items
         while (!peek_is(tok::eof))
         {
            auto t = lex_.peek();
            switch (t.kind)
            {
               case tok::kw_struct:
                  parse_struct(file);
                  break;
               case tok::kw_enum:
                  parse_enum(file);
                  break;
               case tok::kw_using:
                  parse_using(file);
                  break;
               case tok::kw_const:
                  skip_const();
                  break;
               case tok::kw_annotation:
                  skip_annotation_decl();
                  break;
               case tok::kw_interface:
                  skip_interface();
                  break;
               default:
                  error(t, "expected struct, enum, using, const, annotation, or interface");
            }
         }

         // Compute layouts for all structs
         for (auto& s : file.structs)
            capnp_parser_detail::compute_layout(s);

         return file;
      }

      /// Convenience: parse a .capnp source string.
      static capnp_parsed_file parse_capnp(std::string_view source)
      {
         capnp_parser p(source);
         return p.parse();
      }

    private:
      using tok = capnp_parser_detail::tok;
      capnp_parser_detail::lexer lex_;

      // ----- Token helpers -----

      bool peek_is(tok k) { return lex_.peek().kind == k; }

      capnp_parser_detail::token expect(tok k)
      {
         auto t = lex_.next();
         if (t.kind != k)
            error(t, "expected " + tok_name(k) + ", got " + tok_name(t.kind) + " '" +
                         std::string(t.text) + "'");
         return t;
      }

      capnp_parser_detail::token expect_ident()
      {
         auto t = lex_.next();
         if (t.kind != tok::ident)
            error(t, "expected identifier, got " + tok_name(t.kind));
         return t;
      }

      [[noreturn]] void error(const capnp_parser_detail::token& t, const std::string& msg)
      {
         throw capnp_parse_error(msg, t.line, t.col);
      }

      static std::string tok_name(tok k)
      {
         switch (k)
         {
            case tok::eof:
               return "EOF";
            case tok::ident:
               return "identifier";
            case tok::number:
               return "number";
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
            case tok::at:
               return "'@'";
            case tok::colon:
               return "':'";
            case tok::semicolon:
               return "';'";
            case tok::equal:
               return "'='";
            case tok::dot:
               return "'.'";
            case tok::comma:
               return "','";
            default:
               return "keyword";
         }
      }

      // ----- Number parsing -----

      static uint64_t parse_uint64(std::string_view text)
      {
         // Handle 0x prefix
         if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
         {
            uint64_t val = 0;
            for (size_t i = 2; i < text.size(); ++i)
            {
               char c = text[i];
               val <<= 4;
               if (c >= '0' && c <= '9')
                  val |= (c - '0');
               else if (c >= 'a' && c <= 'f')
                  val |= (c - 'a' + 10);
               else if (c >= 'A' && c <= 'F')
                  val |= (c - 'A' + 10);
            }
            return val;
         }
         // Decimal
         uint64_t val = 0;
         for (char c : text)
         {
            if (c >= '0' && c <= '9')
               val = val * 10 + (c - '0');
         }
         return val;
      }

      static uint32_t parse_uint32(std::string_view text) { return static_cast<uint32_t>(parse_uint64(text)); }

      // ----- Struct parsing -----

      void parse_struct(capnp_parsed_file& file)
      {
         expect(tok::kw_struct);
         auto name_tok = expect_ident();
         std::string name(name_tok.text);

         capnp_parsed_struct s;
         s.name = name;

         // Optional schema ID: @0xNNNN
         if (peek_is(tok::at))
         {
            lex_.next();
            auto id_tok = expect(tok::number);
            s.id        = parse_uint64(id_tok.text);
         }

         // Optional generic parameters (just skip them)
         if (peek_is(tok::lparen))
         {
            skip_generic_params();
         }

         expect(tok::lbrace);
         parse_struct_body(file, s);
         expect(tok::rbrace);

         // Register in type map before adding
         capnp_type_ref ref;
         ref.tag                 = capnp_type_tag::struct_;
         ref.referenced_type_idx = static_cast<int32_t>(file.structs.size());
         file.type_map[name]     = ref;

         file.structs.push_back(std::move(s));
      }

      void parse_struct_body(capnp_parsed_file& file, capnp_parsed_struct& s)
      {
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            auto t = lex_.peek();

            if (t.kind == tok::kw_struct)
            {
               // Nested struct
               parse_struct(file);
               continue;
            }

            if (t.kind == tok::kw_enum)
            {
               // Nested enum
               parse_enum(file);
               continue;
            }

            if (t.kind == tok::kw_using)
            {
               parse_using(file);
               continue;
            }

            if (t.kind == tok::kw_union)
            {
               parse_union(file, s);
               continue;
            }

            if (t.kind == tok::kw_const)
            {
               skip_const();
               continue;
            }

            if (t.kind == tok::kw_annotation)
            {
               skip_annotation_decl();
               continue;
            }

            if (t.kind == tok::ident)
            {
               // Field: fieldName @N :Type [= default] ;
               parse_field(file, s);
               continue;
            }

            error(t, "unexpected token in struct body");
         }
      }

      void parse_field(capnp_parsed_file& file, capnp_parsed_struct& s)
      {
         auto name_tok = expect_ident();

         capnp_parsed_field field;
         field.name = std::string(name_tok.text);

         // @N ordinal
         expect(tok::at);
         auto ord_tok  = expect(tok::number);
         field.ordinal = parse_uint32(ord_tok.text);

         expect(tok::colon);
         field.type = parse_type_ref(file);

         // Optional default value
         if (peek_is(tok::equal))
         {
            lex_.next();
            field.default_value = skip_default_value();
         }

         // Optional annotation (e.g., $MyAnnotation)
         skip_field_annotations();

         expect(tok::semicolon);

         s.fields.push_back(std::move(field));
      }

      void parse_union(capnp_parsed_file& file, capnp_parsed_struct& s)
      {
         expect(tok::kw_union);

         capnp_parsed_union u;

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            auto t = lex_.peek();
            if (t.kind == tok::ident)
            {
               // Alternative: name @N :Type ;
               auto alt_name = expect_ident();

               capnp_parsed_field alt;
               alt.name = std::string(alt_name.text);

               expect(tok::at);
               auto ord_tok = expect(tok::number);
               alt.ordinal  = parse_uint32(ord_tok.text);

               expect(tok::colon);
               alt.type = parse_type_ref(file);

               // Optional default value
               if (peek_is(tok::equal))
               {
                  lex_.next();
                  alt.default_value = skip_default_value();
               }

               skip_field_annotations();
               expect(tok::semicolon);

               u.alternatives.push_back(std::move(alt));
            }
            else
            {
               error(t, "expected field in union");
            }
         }
         expect(tok::rbrace);

         s.unions.push_back(std::move(u));
      }

      // ----- Enum parsing -----

      void parse_enum(capnp_parsed_file& file)
      {
         expect(tok::kw_enum);
         auto name_tok = expect_ident();
         std::string name(name_tok.text);

         capnp_parsed_enum e;
         e.name = name;

         // Optional schema ID
         if (peek_is(tok::at))
         {
            lex_.next();
            auto id_tok = expect(tok::number);
            e.id        = parse_uint64(id_tok.text);
         }

         expect(tok::lbrace);
         while (!peek_is(tok::rbrace) && !peek_is(tok::eof))
         {
            auto label = expect_ident();
            // @N ordinal
            expect(tok::at);
            auto ord_tok = expect(tok::number);
            // We store enumerants in declaration order; the ordinal
            // should match the position.
            (void)ord_tok;

            expect(tok::semicolon);
            e.enumerants.push_back(std::string(label.text));
         }
         expect(tok::rbrace);

         capnp_type_ref ref;
         ref.tag                 = capnp_type_tag::enum_;
         ref.referenced_type_idx = static_cast<int32_t>(file.enums.size());
         file.type_map[name]     = ref;

         file.enums.push_back(std::move(e));
      }

      // ----- Using (type alias) -----

      void parse_using(capnp_parsed_file& file)
      {
         expect(tok::kw_using);
         auto name_tok = expect_ident();
         std::string name(name_tok.text);

         expect(tok::equal);
         auto type = parse_type_ref(file);
         expect(tok::semicolon);

         capnp_type_alias alias;
         alias.name = name;
         alias.type = type;
         file.aliases.push_back(std::move(alias));

         // Register in type map
         file.type_map[name] = type;
      }

      // ----- Type references -----

      capnp_type_ref parse_type_ref(capnp_parsed_file& file)
      {
         auto t = lex_.peek();

         // Builtin scalar types
         switch (t.kind)
         {
            case tok::kw_void:
               lex_.next();
               return {capnp_type_tag::void_};
            case tok::kw_bool:
               lex_.next();
               return {capnp_type_tag::bool_};
            case tok::kw_int8:
               lex_.next();
               return {capnp_type_tag::int8};
            case tok::kw_int16:
               lex_.next();
               return {capnp_type_tag::int16};
            case tok::kw_int32:
               lex_.next();
               return {capnp_type_tag::int32};
            case tok::kw_int64:
               lex_.next();
               return {capnp_type_tag::int64};
            case tok::kw_uint8:
               lex_.next();
               return {capnp_type_tag::uint8};
            case tok::kw_uint16:
               lex_.next();
               return {capnp_type_tag::uint16};
            case tok::kw_uint32:
               lex_.next();
               return {capnp_type_tag::uint32};
            case tok::kw_uint64:
               lex_.next();
               return {capnp_type_tag::uint64};
            case tok::kw_float32:
               lex_.next();
               return {capnp_type_tag::float32};
            case tok::kw_float64:
               lex_.next();
               return {capnp_type_tag::float64};
            case tok::kw_text:
               lex_.next();
               return {capnp_type_tag::text};
            case tok::kw_data:
               lex_.next();
               return {capnp_type_tag::data};
            case tok::kw_any_pointer:
               lex_.next();
               return {capnp_type_tag::any_ptr};
            default:
               break;
         }

         // List(ElementType)
         if (t.kind == tok::kw_list)
         {
            lex_.next();
            expect(tok::lparen);
            auto elem_type = parse_type_ref(file);
            expect(tok::rparen);

            // Store the element type in the file's type_map isn't needed;
            // we store the full ref inline via a special list type_ref
            // that carries the element's tag.
            capnp_type_ref ref;
            ref.tag              = capnp_type_tag::list;
            ref.element_type_idx = encode_type_ref(elem_type);
            return ref;
         }

         // Named type reference (struct or enum name)
         if (t.kind == tok::ident)
         {
            auto name = lex_.next();
            std::string type_name(name.text);

            // Handle dotted names (e.g., Outer.Inner)
            while (peek_is(tok::dot))
            {
               lex_.next();
               auto part = expect_ident();
               type_name += ".";
               type_name += part.text;
            }

            // Optional generic parameters (e.g., SomeType(Arg))
            if (peek_is(tok::lparen))
            {
               skip_generic_params();
            }

            auto it = file.type_map.find(type_name);
            if (it != file.type_map.end())
               return it->second;

            // Forward reference: assume struct
            capnp_type_ref ref;
            ref.tag                 = capnp_type_tag::struct_;
            ref.referenced_type_idx = -1;  // unresolved
            return ref;
         }

         error(t, "expected type");
      }

      // Encode a type_ref into a single int32_t for storage in list element_type_idx.
      // Negative values encode builtins, positive values encode struct/enum indices.
      static int32_t encode_type_ref(const capnp_type_ref& ref)
      {
         if (ref.tag == capnp_type_tag::struct_)
            return ref.referenced_type_idx;
         if (ref.tag == capnp_type_tag::enum_)
            return ref.referenced_type_idx;
         // For builtins, use negative encoding
         return -(static_cast<int32_t>(ref.tag) + 1);
      }

      // ----- Default value skipping -----

      std::string skip_default_value()
      {
         std::string val;
         auto        t = lex_.peek();

         // Handle parenthesized/bracketed expressions
         if (t.kind == tok::lparen)
         {
            val += "(";
            lex_.next();
            int depth = 1;
            while (depth > 0 && !peek_is(tok::eof))
            {
               auto inner = lex_.next();
               if (inner.kind == tok::lparen)
                  depth++;
               else if (inner.kind == tok::rparen)
                  depth--;
               if (depth > 0)
                  val += std::string(inner.text);
            }
            val += ")";
            return val;
         }

         // Simple values: number, string, true, false, identifier (enum value)
         // Also handle list literals: [...]
         auto v = lex_.next();
         val    = std::string(v.text);

         // Handle negative numbers (already lexed as single token)
         // Handle struct literal: (field = val, ...)
         // Handle list literal: [val, val, ...]

         return val;
      }

      // ----- Skip annotation expressions -----

      void skip_field_annotations()
      {
         // Cap'n Proto field annotations look like: $annotation(args)
         while (peek_is(tok::ident) || lex_.peek().text == "$")
         {
            auto t = lex_.peek();
            if (t.text.empty() || t.text[0] != '$')
               break;
            // It would show up as an ident starting with '$' but our lexer
            // doesn't handle '$'. Just break.
            break;
         }
      }

      // ----- Skip various top-level constructs -----

      void skip_const()
      {
         expect(tok::kw_const);
         // const name @id :Type = value;
         expect_ident();
         expect(tok::at);
         expect(tok::number);
         expect(tok::colon);
         // Skip type
         skip_type();
         expect(tok::equal);
         skip_default_value();
         expect(tok::semicolon);
      }

      void skip_annotation_decl()
      {
         expect(tok::kw_annotation);
         // annotation name @id (targets) :Type;
         // Just skip to semicolon
         while (!peek_is(tok::semicolon) && !peek_is(tok::eof))
            lex_.next();
         if (peek_is(tok::semicolon))
            lex_.next();
      }

      void skip_interface()
      {
         expect(tok::kw_interface);
         expect_ident();
         // Optional ID
         if (peek_is(tok::at))
         {
            lex_.next();
            expect(tok::number);
         }
         // Skip body (brace-delimited)
         if (peek_is(tok::lbrace))
            skip_braced_block();
      }

      void skip_type()
      {
         auto t = lex_.next();
         // Handle List(T) or Name(T)
         if (peek_is(tok::lparen))
            skip_generic_params();
         // Handle dotted names
         while (peek_is(tok::dot))
         {
            lex_.next();
            expect_ident();
         }
      }

      void skip_generic_params()
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

      void skip_braced_block()
      {
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
   };

   // =========================================================================
   // Public API
   // =========================================================================

   /// Parse a .capnp schema source string into a capnp_parsed_file.
   inline capnp_parsed_file capnp_parse(std::string_view source)
   {
      return capnp_parser::parse_capnp(source);
   }

}  // namespace psio1
