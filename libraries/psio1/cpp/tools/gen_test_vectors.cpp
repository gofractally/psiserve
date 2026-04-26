// Fracpack cross-language test vector generator
//
// Generates a JSON file containing test vectors for every fracpack feature:
// schemas, packed hex, and expected JSON for each test type and instance.
// Other language implementations consume these vectors to verify compatibility.
//
// Usage: gen_test_vectors [output_file]
//   If no output file is given, writes to stdout.

#include <psio1/fracpack.hpp>
#include <psio1/schema.hpp>
#include <psio1/to_json.hpp>
#include <psio1/to_hex.hpp>

#include <fstream>
#include <iostream>
#include <span>

using namespace psio1;
using namespace psio1::schema_types;

// ============================================================
// Test Type Definitions
// ============================================================

// --- Fixed-size structs (definitionWillNotChange → Struct in schema, no 2-byte header) ---

struct FixedInts
{
   int32_t x;
   int32_t y;
};
PSIO1_REFLECT(FixedInts, definitionWillNotChange(), x, y)

struct FixedMixed
{
   bool     b;
   uint8_t  u8;
   uint16_t u16;
   uint32_t u32;
   uint64_t u64;
};
PSIO1_REFLECT(FixedMixed, definitionWillNotChange(), b, u8, u16, u32, u64)

// --- Extensible structs (Object in schema, has 2-byte fixed-size header) ---

struct AllPrimitives
{
   bool     b;
   uint8_t  u8v;
   int8_t   i8v;
   uint16_t u16v;
   int16_t  i16v;
   uint32_t u32v;
   int32_t  i32v;
   uint64_t u64v;
   int64_t  i64v;
   float    f32v;
   double   f64v;
};
PSIO1_REFLECT(AllPrimitives, b, u8v, i8v, u16v, i16v, u32v, i32v, u64v, i64v, f32v, f64v)

struct WithStrings
{
   std::string empty_str;
   std::string hello;
   std::string unicode;
};
PSIO1_REFLECT(WithStrings, empty_str, hello, unicode)

struct WithVectors
{
   std::vector<uint32_t>    ints;
   std::vector<std::string> strings;
};
PSIO1_REFLECT(WithVectors, ints, strings)

struct WithOptionals
{
   std::optional<uint32_t>    opt_int;
   std::optional<std::string> opt_str;
};
PSIO1_REFLECT(WithOptionals, opt_int, opt_str)

struct Inner
{
   uint32_t    value;
   std::string label;
};
PSIO1_REFLECT(Inner, value, label)

struct Outer
{
   Inner       inner;
   std::string name;
};
PSIO1_REFLECT(Outer, inner, name)

struct WithVariant
{
   std::variant<uint32_t, std::string, Inner> data;
};
PSIO1_REFLECT(WithVariant, data)

struct VecOfStructs
{
   std::vector<Inner> items;
};
PSIO1_REFLECT(VecOfStructs, items)

struct OptionalStruct
{
   std::optional<Inner> item;
};
PSIO1_REFLECT(OptionalStruct, item)

struct VecOfOptionals
{
   std::vector<std::optional<uint32_t>> items;
};
PSIO1_REFLECT(VecOfOptionals, items)

struct OptionalVec
{
   std::optional<std::vector<uint32_t>> items;
};
PSIO1_REFLECT(OptionalVec, items)

struct NestedVecs
{
   std::vector<std::vector<uint32_t>> matrix;
};
PSIO1_REFLECT(NestedVecs, matrix)

struct Complex
{
   std::vector<Inner>                      items;
   std::optional<std::vector<uint32_t>>    opt_vec;
   std::vector<std::optional<std::string>> vec_opt;
   std::optional<Inner>                    opt_struct;
};
PSIO1_REFLECT(Complex, items, opt_vec, vec_opt, opt_struct)

struct SingleBool
{
   bool value;
};
PSIO1_REFLECT(SingleBool, definitionWillNotChange(), value)

struct SingleU32
{
   uint32_t value;
};
PSIO1_REFLECT(SingleU32, definitionWillNotChange(), value)

struct SingleString
{
   std::string value;
};
PSIO1_REFLECT(SingleString, value)

struct FixedArray
{
   std::array<uint32_t, 3> arr;
};
PSIO1_REFLECT(FixedArray, definitionWillNotChange(), arr)

struct EmptyExtensible
{
   uint32_t dummy;
};
PSIO1_REFLECT(EmptyExtensible, dummy)

// ============================================================
// JSON Writer — minimal streaming JSON builder
// ============================================================

class JsonWriter
{
   std::ostream&     out;
   std::vector<bool> stack;  // true = need comma before next value
   int               depth    = 0;
   bool              after_key = false;  // suppress newline for value after key

   void pre_value()
   {
      if (!stack.empty() && stack.back())
         out << ',';
      if (!stack.empty())
         stack.back() = true;
      if (!after_key)
      {
         out << '\n';
         indent();
      }
      after_key = false;
   }

   void indent()
   {
      for (int i = 0; i < depth; ++i)
         out << "  ";
   }

  public:
   JsonWriter(std::ostream& out) : out(out) {}

   void begin_object()
   {
      pre_value();
      out << '{';
      stack.push_back(false);
      ++depth;
   }
   void end_object()
   {
      --depth;
      out << '\n';
      indent();
      out << '}';
      stack.pop_back();
   }

   void begin_array()
   {
      pre_value();
      out << '[';
      stack.push_back(false);
      ++depth;
   }
   void end_array()
   {
      --depth;
      out << '\n';
      indent();
      out << ']';
      stack.pop_back();
   }

   void key(std::string_view k)
   {
      pre_value();
      out << '"' << k << "\": ";
      stack.back() = false;
      after_key    = true;
   }

   void str(std::string_view v)
   {
      pre_value();
      out << '"';
      for (char c : v)
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
               out << c;
         }
      }
      out << '"';
   }

   void num(int64_t v)
   {
      pre_value();
      out << v;
   }

   // Insert pre-formatted JSON value
   void raw(const std::string& json)
   {
      pre_value();
      out << json;
   }

   // key + str combined
   void kv(std::string_view k, std::string_view v)
   {
      key(k);
      str(v);
   }

   // key + raw combined
   void kr(std::string_view k, const std::string& json)
   {
      key(k);
      raw(json);
   }
};

// ============================================================
// Helpers
// ============================================================

template <typename T>
std::string pack_hex(const T& value)
{
   auto packed = psio1::to_frac(value);
   return psio1::to_hex(std::span<const char>(packed.data(), packed.size()));
}

template <typename T>
void emit_case(JsonWriter& w, const char* name, const T& value)
{
   w.begin_object();
   w.kv("name", name);
   w.kv("packed_hex", pack_hex(value));
   w.kr("json", convert_to_json(value));
   w.end_object();
}

template <typename T>
void begin_group(JsonWriter& w, const char* name)
{
   w.begin_object();
   w.kv("name", name);

   auto schema     = SchemaBuilder{}.insert<T>(name).build();
   auto schema_hex = psio1::to_frac(schema);

   w.kr("schema", convert_to_json(schema));
   w.kv("schema_hex", psio1::to_hex(std::span<const char>(schema_hex.data(), schema_hex.size())));
   w.kv("root_type", name);

   w.key("cases");
   w.begin_array();
}

void end_group(JsonWriter& w)
{
   w.end_array();
   w.end_object();
}

// ============================================================
// Test Vector Generation
// ============================================================

int main(int argc, char** argv)
{
   std::ostream* out = &std::cout;
   std::ofstream file;
   if (argc > 1)
   {
      file.open(argv[1]);
      if (!file)
      {
         std::cerr << "Cannot open " << argv[1] << "\n";
         return 1;
      }
      out = &file;
   }

   JsonWriter w(*out);
   w.begin_object();
   w.key("format_version");
   w.num(1);

   w.key("types");
   w.begin_array();

   // ---- 1. Fixed-size integer struct ----
   begin_group<FixedInts>(w, "FixedInts");
   emit_case(w, "zeros", FixedInts{0, 0});
   emit_case(w, "positive", FixedInts{42, 100});
   emit_case(w, "negative", FixedInts{-1, -2147483648});
   emit_case(w, "max", FixedInts{2147483647, 2147483647});
   end_group(w);

   // ---- 2. Fixed-size mixed types ----
   begin_group<FixedMixed>(w, "FixedMixed");
   emit_case(w, "zeros", FixedMixed{false, 0, 0, 0, 0});
   emit_case(w, "ones", FixedMixed{true, 1, 1, 1, 1});
   emit_case(w, "max", FixedMixed{true, 255, 65535, 4294967295u, 18446744073709551615ull});
   end_group(w);

   // ---- 3. All primitive types ----
   begin_group<AllPrimitives>(w, "AllPrimitives");
   emit_case(w, "zeros", AllPrimitives{false, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f, 0.0});
   emit_case(w, "ones", AllPrimitives{true, 1, 1, 1, 1, 1, 1, 1, 1, 1.0f, 1.0});
   emit_case(w, "max_unsigned",
             AllPrimitives{true, 255, 127, 65535, 32767, 4294967295u, 2147483647, UINT64_MAX,
                           INT64_MAX, 3.14159f, 2.718281828459045});
   emit_case(w, "min_signed",
             AllPrimitives{false, 0, -128, 0, -32768, 0, -2147483648, 0, INT64_MIN, -1.0f, -1.0});
   emit_case(w, "fractional_floats",
             AllPrimitives{false, 0, 0, 0, 0, 0, 0, 0, 0, 0.1f, 0.1});
   end_group(w);

   // ---- 4. Single bool (fixed) ----
   begin_group<SingleBool>(w, "SingleBool");
   emit_case(w, "false", SingleBool{false});
   emit_case(w, "true", SingleBool{true});
   end_group(w);

   // ---- 5. Single u32 (fixed) ----
   begin_group<SingleU32>(w, "SingleU32");
   emit_case(w, "zero", SingleU32{0});
   emit_case(w, "one", SingleU32{1});
   emit_case(w, "max", SingleU32{4294967295u});
   emit_case(w, "hex_pattern", SingleU32{0xDEADBEEF});
   end_group(w);

   // ---- 6. Single string (extensible) ----
   begin_group<SingleString>(w, "SingleString");
   emit_case(w, "empty", SingleString{""});
   emit_case(w, "hello", SingleString{"hello"});
   emit_case(w, "with_spaces", SingleString{"hello world"});
   emit_case(w, "special_chars", SingleString{"tab\there\nnewline"});
   emit_case(w, "unicode", SingleString{"caf\xc3\xa9 \xe2\x98\x95 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"});
   emit_case(w, "escapes", SingleString{"quote\"backslash\\"});
   end_group(w);

   // ---- 7. Strings ----
   begin_group<WithStrings>(w, "WithStrings");
   emit_case(w, "all_empty", WithStrings{"", "", ""});
   emit_case(w, "mixed",
             WithStrings{"", "hello", "\xc3\xa9mojis: \xf0\x9f\x8e\x89\xf0\x9f\x9a\x80"});
   end_group(w);

   // ---- 8. Vectors ----
   begin_group<WithVectors>(w, "WithVectors");
   emit_case(w, "both_empty", WithVectors{{}, {}});
   emit_case(w, "ints_only", WithVectors{{1, 2, 3}, {}});
   emit_case(w, "strings_only", WithVectors{{}, {"a", "bb", "ccc"}});
   emit_case(w, "both_filled", WithVectors{{10, 20}, {"hello", "world"}});
   emit_case(w, "single_elements", WithVectors{{42}, {"only"}});
   end_group(w);

   // ---- 9. Optionals ----
   begin_group<WithOptionals>(w, "WithOptionals");
   emit_case(w, "both_null", WithOptionals{std::nullopt, std::nullopt});
   emit_case(w, "int_only", WithOptionals{42, std::nullopt});
   emit_case(w, "str_only", WithOptionals{std::nullopt, "hello"});
   emit_case(w, "both_present", WithOptionals{99, "world"});
   emit_case(w, "zero_int", WithOptionals{0u, std::nullopt});
   end_group(w);

   // ---- 10. Nested structs ----
   begin_group<Inner>(w, "Inner");
   emit_case(w, "simple", Inner{42, "hello"});
   emit_case(w, "empty_label", Inner{0, ""});
   emit_case(w, "max_value", Inner{UINT32_MAX, "max"});
   end_group(w);

   begin_group<Outer>(w, "Outer");
   emit_case(w, "simple", Outer{Inner{1, "inner"}, "outer"});
   emit_case(w, "empty_strings", Outer{Inner{0, ""}, ""});
   emit_case(w, "nested_unicode", Outer{Inner{42, "caf\xc3\xa9"}, "na\xc3\xafve"});
   end_group(w);

   // ---- 11. Variants ----
   begin_group<WithVariant>(w, "WithVariant");
   emit_case(w, "uint32_alt", WithVariant{uint32_t(42)});
   emit_case(w, "string_alt", WithVariant{std::string("hello")});
   emit_case(w, "struct_alt", WithVariant{Inner{7, "variant_inner"}});
   emit_case(w, "uint32_zero", WithVariant{uint32_t(0)});
   emit_case(w, "string_empty", WithVariant{std::string("")});
   end_group(w);

   // ---- 12. Vector of structs ----
   begin_group<VecOfStructs>(w, "VecOfStructs");
   emit_case(w, "empty", VecOfStructs{{}});
   emit_case(w, "single", VecOfStructs{{Inner{1, "one"}}});
   emit_case(w, "multiple", VecOfStructs{{Inner{1, "one"}, Inner{2, "two"}, Inner{3, "three"}}});
   end_group(w);

   // ---- 13. Optional struct ----
   begin_group<OptionalStruct>(w, "OptionalStruct");
   emit_case(w, "null", OptionalStruct{std::nullopt});
   emit_case(w, "present", OptionalStruct{Inner{42, "exists"}});
   end_group(w);

   // ---- 14. Vector of optionals ----
   begin_group<VecOfOptionals>(w, "VecOfOptionals");
   emit_case(w, "empty", VecOfOptionals{{}});
   emit_case(w, "all_null",
             VecOfOptionals{
                 {std::nullopt, std::nullopt, std::nullopt}
   });
   emit_case(w, "all_present",
             VecOfOptionals{
                 {1u, 2u, 3u}
   });
   emit_case(w, "mixed",
             VecOfOptionals{
                 {1u, std::nullopt, 3u, std::nullopt}
   });
   end_group(w);

   // ---- 15. Optional vector ----
   begin_group<OptionalVec>(w, "OptionalVec");
   emit_case(w, "null", OptionalVec{std::nullopt});
   emit_case(w, "empty_vec", OptionalVec{std::vector<uint32_t>{}});
   emit_case(w, "with_values", OptionalVec{std::vector<uint32_t>{10, 20, 30}});
   end_group(w);

   // ---- 16. Nested vectors (matrix) ----
   begin_group<NestedVecs>(w, "NestedVecs");
   emit_case(w, "empty", NestedVecs{{}});
   emit_case(w, "empty_rows", NestedVecs{{{}, {}, {}}});
   emit_case(w, "identity_2x2", NestedVecs{{{1, 0}, {0, 1}}});
   emit_case(w, "ragged", NestedVecs{{{1}, {2, 3}, {4, 5, 6}}});
   end_group(w);

   // ---- 17. Fixed-size array ----
   begin_group<FixedArray>(w, "FixedArray");
   emit_case(w, "zeros", FixedArray{{0, 0, 0}});
   emit_case(w, "sequence", FixedArray{{1, 2, 3}});
   emit_case(w, "max", FixedArray{{UINT32_MAX, UINT32_MAX, UINT32_MAX}});
   end_group(w);

   // ---- 18. Complex combinations ----
   begin_group<Complex>(w, "Complex");
   emit_case(w, "all_empty",
             Complex{
                 .items      = {},
                 .opt_vec    = std::nullopt,
                 .vec_opt    = {},
                 .opt_struct = std::nullopt,
             });
   emit_case(w, "all_populated",
             Complex{
                 .items      = {Inner{1, "a"}, Inner{2, "b"}},
                 .opt_vec    = std::vector<uint32_t>{10, 20},
                 .vec_opt    = {"x", std::nullopt, "z"},
                 .opt_struct = Inner{99, "present"},
             });
   emit_case(w, "sparse",
             Complex{
                 .items      = {Inner{42, "only"}},
                 .opt_vec    = std::nullopt,
                 .vec_opt    = {std::nullopt, std::nullopt},
                 .opt_struct = std::nullopt,
             });
   end_group(w);

   // ---- 19. Extensible struct with minimal field ----
   begin_group<EmptyExtensible>(w, "EmptyExtensible");
   emit_case(w, "zero", EmptyExtensible{0});
   emit_case(w, "max", EmptyExtensible{UINT32_MAX});
   end_group(w);

   w.end_array();
   w.end_object();
   *out << '\n';

   if (argc > 1)
   {
      std::cerr << "Generated test vectors: " << argv[1] << "\n";
   }
   return 0;
}
