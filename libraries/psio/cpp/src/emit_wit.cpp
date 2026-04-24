// Phase C: Schema IR → WIT text emitter.
//
// See psio/emit_wit.hpp for the contract and supported coverage.

#include <psio/emit_wit.hpp>
#include <psio/schema.hpp>

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace psio::schema_types
{
   namespace
   {
      // ── Name mangling ─────────────────────────────────────────────
      //
      // WIT identifiers are lowercase-kebab. Convert C++ names by:
      //   • turning `_` into `-`
      //   • splitting camel/Pascal on lowercase→uppercase transitions
      //   • lowercasing everything
      std::string to_kebab(std::string_view name)
      {
         std::string out;
         out.reserve(name.size() + 2);
         for (std::size_t i = 0; i < name.size(); ++i)
         {
            char c = name[i];
            if (c == '_')
            {
               out += '-';
               continue;
            }
            if (i > 0 && std::isupper(static_cast<unsigned char>(c)) &&
                std::islower(static_cast<unsigned char>(name[i - 1])))
            {
               out += '-';
            }
            out += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
         }
         return out;
      }

      // ── Package name handling ─────────────────────────────────────
      //
      // WIT packages are `namespace:name[@version]`. Our Package IR
      // carries a single `name` string; if it already contains a `:`
      // it is passed through verbatim, otherwise we emit the bare
      // identifier (consumer is responsible for producing a fully-
      // qualified name if they care about external-tool consumption).
      std::string format_package_id(const std::string& name,
                                    const std::string& version)
      {
         std::string out = name;
         if (!version.empty())
         {
            out += '@';
            out += version;
         }
         return out;
      }

      // ── Emitter ───────────────────────────────────────────────────
      class Emitter
      {
        public:
         explicit Emitter(const Schema& schema) : _schema{schema} {}

         std::string run()
         {
            emit_package();
            emit_uses();
            emit_interfaces();
            emit_worlds();
            emit_orphan_types();
            return _out.str();
         }

        private:
         const Schema&     _schema;
         std::ostringstream _out;
         int               _indent = 0;

         void indent()
         {
            for (int i = 0; i < _indent; ++i)
               _out << "  ";
         }

         // ── Envelope ────────────────────────────────────────────
         void emit_package()
         {
            const auto& p = _schema.package;
            if (p.name.empty())
               return;
            _out << "package " << format_package_id(p.name, p.version)
                 << ";\n\n";
         }

         void emit_uses()
         {
            if (_schema.uses.empty())
               return;
            for (const auto& u : _schema.uses)
            {
               _out << "use ";
               if (!u.package.empty())
                  _out << u.package << '/';
               _out << to_kebab(u.interface_name);
               if (!u.version.empty())
                  _out << '@' << u.version;
               _out << ";\n";
            }
            _out << "\n";
         }

         void emit_interfaces()
         {
            for (const auto& iface : _schema.interfaces)
               emit_interface(iface);
         }

         void emit_interface(const Interface& iface)
         {
            _out << "interface " << to_kebab(iface.name) << " {\n";
            ++_indent;
            for (const auto& tname : iface.type_names)
               emit_named_type(tname);
            for (const auto& fn : iface.funcs)
               emit_func(fn);
            --_indent;
            _out << "}\n\n";
         }

         void emit_worlds()
         {
            for (const auto& w : _schema.worlds)
               emit_world(w);
         }

         void emit_world(const World& w)
         {
            _out << "world " << to_kebab(w.name) << " {\n";
            ++_indent;
            for (const auto& imp : w.imports)
            {
               indent();
               _out << "import ";
               if (!imp.package.empty())
                  _out << imp.package << '/';
               _out << to_kebab(imp.interface_name) << ";\n";
            }
            for (const auto& exp : w.exports)
            {
               indent();
               _out << "export " << to_kebab(exp) << ";\n";
            }
            --_indent;
            _out << "}\n\n";
         }

         // Types that live at schema-level (schema.types) but aren't
         // claimed by any interface. Emitted at top level for
         // visibility. Uncommon; most reflected types land inside an
         // interface via PSIO_INTERFACE.
         //
         // Names starting with `@` are internal interning artifacts
         // produced by SchemaBuilder for primitive-shaped types; they
         // are resolved inline at the use site rather than emitted as
         // top-level aliases.
         void emit_orphan_types()
         {
            std::unordered_set<std::string> in_iface;
            for (const auto& iface : _schema.interfaces)
               for (const auto& tn : iface.type_names)
                  in_iface.insert(tn);
            for (const auto& [name, type] : _schema.types)
            {
               if (in_iface.contains(name))
                  continue;
               if (!name.empty() && name.front() == '@')
                  continue;
               emit_type_decl(name, type);
            }
         }

         // ── Type emission ───────────────────────────────────────
         void emit_named_type(const std::string& tname)
         {
            const AnyType* type = _schema.get(tname);
            if (!type)
            {
               indent();
               _out << "/* TODO: missing type \"" << tname << "\" */\n";
               return;
            }
            emit_type_decl(tname, *type);
         }

         void emit_type_decl(const std::string& tname, const AnyType& type)
         {
            std::visit(
                [&]<typename T>(const T& t)
                {
                   emit_type_decl_impl(tname, t);
                },
                type.value);
         }

         void emit_type_decl_impl(const std::string& tname, const Object& o)
         {
            emit_record(tname, o.members);
         }
         void emit_type_decl_impl(const std::string& tname, const Struct& s)
         {
            emit_record(tname, s.members);
         }
         void emit_type_decl_impl(const std::string& tname, const Variant& v)
         {
            emit_variant_decl(tname, v.members);
         }
         void emit_type_decl_impl(const std::string& tname, const Resource& r)
         {
            emit_resource_decl(r);
         }
         // Everything else: a bare `type alias = <inline>;`
         template <typename T>
         void emit_type_decl_impl(const std::string& tname, const T& t)
         {
            indent();
            _out << "type " << to_kebab(tname) << " = " << inline_type(t)
                 << ";\n";
         }

         void emit_record(const std::string&         tname,
                          const std::vector<Member>& members)
         {
            indent();
            _out << "record " << to_kebab(tname) << " {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const auto& m = members[i];
               indent();
               _out << to_kebab(m.name) << ": " << inline_any(m.type);
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_variant_decl(const std::string&         tname,
                                const std::vector<Member>& members)
         {
            indent();
            _out << "variant " << to_kebab(tname) << " {\n";
            ++_indent;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
               const auto& m = members[i];
               indent();
               _out << to_kebab(m.name);
               // Variant case with payload vs without: a Tuple with
               // zero members serves as the bare case form.
               if (auto* tup = std::get_if<Tuple>(&m.type.value);
                   !(tup && tup->members.empty()))
               {
                  _out << "(" << inline_any(m.type) << ")";
               }
               if (i + 1 < members.size())
                  _out << ",";
               _out << "\n";
            }
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_resource_decl(const Resource& r)
         {
            indent();
            _out << "resource " << to_kebab(r.name) << " {\n";
            ++_indent;
            for (const auto& method : r.methods)
               emit_func(method);
            --_indent;
            indent();
            _out << "}\n";
         }

         void emit_func(const Func& fn)
         {
            indent();
            _out << to_kebab(fn.name) << ": func(";
            for (std::size_t i = 0; i < fn.params.size(); ++i)
            {
               const auto& p = fn.params[i];
               if (i > 0)
                  _out << ", ";
               _out << to_kebab(p.name) << ": " << inline_any(p.type);
            }
            _out << ")";
            if (fn.result)
               _out << " -> " << inline_any(*fn.result);
            _out << ";\n";
         }

         // ── Inline type forms (no declaration) ──────────────────
         std::string inline_any(const AnyType& t)
         {
            return std::visit(
                [&]<typename T>(const T& v) { return inline_type(v); },
                t.value);
         }

         std::string inline_type(const Int& i)
         {
            std::string out;
            out += (i.isSigned ? 's' : 'u');
            out += std::to_string(i.bits);
            return out;
         }
         std::string inline_type(const Float& f)
         {
            if (f.exp == 8 && f.mantissa == 23)
               return "f32";
            if (f.exp == 11 && f.mantissa == 52)
               return "f64";
            return "/* TODO: non-IEEE float */";
         }
         std::string inline_type(const Type& t)
         {
            // `Type` is a by-name reference to another schema entry.
            // WIT built-ins are emitted directly; names starting with
            // `@` are SchemaBuilder interning artifacts and must be
            // resolved through the schema to their inline form; all
            // other names are user-defined types and kebab-cased.
            if (t.type == "bool")
               return "bool";
            if (t.type == "string")
               return "string";
            if (t.type == "char")
               return "char";
            if (!t.type.empty() && t.type.front() == '@')
            {
               if (const AnyType* resolved = _schema.get(t.type))
                  return inline_any(*resolved);
               return "/* TODO: unresolved " + t.type + " */";
            }
            return to_kebab(t.type);
         }
         std::string inline_type(const List& l)
         {
            return "list<" + inline_any(*l.type) + ">";
         }
         std::string inline_type(const BoundedList& l)
         {
            // WIT has no native bounded-list syntax. Emit as list<T> with a
            // psio convention comment carrying the bound — consumable by
            // psio-aware tooling on round-trip.
            return "list<" + inline_any(*l.type) +
                   ">/* @psio:max=" + std::to_string(l.maxCount) + " */";
         }
         std::string inline_type(const Option& o)
         {
            return "option<" + inline_any(*o.type) + ">";
         }
         std::string inline_type(const Tuple& t)
         {
            if (t.members.empty())
               return "tuple<>";
            std::string out = "tuple<";
            for (std::size_t i = 0; i < t.members.size(); ++i)
            {
               if (i > 0)
                  out += ", ";
               out += inline_any(t.members[i]);
            }
            out += ">";
            return out;
         }
         std::string inline_type(const Object&)
         {
            return "/* TODO: inline record not supported */";
         }
         std::string inline_type(const Struct&)
         {
            return "/* TODO: inline record not supported */";
         }
         std::string inline_type(const Variant&)
         {
            return "/* TODO: inline variant not supported */";
         }
         std::string inline_type(const Resource& r)
         {
            // Inline resource references are `own<name>`; borrowed
            // forms would be `borrow<name>` but aren't carried in
            // our IR yet.
            return "own<" + to_kebab(r.name) + ">";
         }
         std::string inline_type(const Array&)
         {
            return "/* TODO: fixed-size array */";
         }
         std::string inline_type(const FracPack&)
         {
            return "/* TODO: fracpack wrapper */";
         }
         std::string inline_type(const Custom& c)
         {
            // `Custom` annotates an underlying shape with a semantic
            // tag used for JSON/binary codecs. For WIT emission we
            // surface the annotation where a built-in spelling exists
            // ("bool", "string", "char") and otherwise fall through
            // to the underlying type (so `Custom{List<u8>, "hex"}`
            // renders as `list<u8>`).
            if (c.id == "bool")
               return "bool";
            if (c.id == "string")
               return "string";
            if (c.id == "char")
               return "char";
            return inline_any(*c.type);
         }
      };

   }  // namespace

   std::string emit_wit(const Schema& schema)
   {
      Emitter e{schema};
      return e.run();
   }

}  // namespace psio::schema_types
