// fracpack-viewgen generates typed zero-copy view structs from Go struct
// definitions. It parses Go source files in the current directory, finds
// the named struct types, maps their fields to fracpack binary layout,
// and emits view structs with accessor methods at compile-time-known
// byte offsets.
//
// Usage:
//
//	//go:generate fracpack-viewgen -type Point,UserProfile,Order -fixed Point
//
// Install:
//
//	go install github.com/psibase/psio/fracpack-viewgen@latest
//
// Flags:
//
//	-type    Comma-separated list of struct type names to generate views for.
//	-fixed   Comma-separated list of types that are non-extensible
//	         (definitionWillNotChange — no u16 header). Default: all extensible.
//	-o       Output file name. Default: fracpack_views_gen.go
//
// Field mapping:
//
//	Go type        fracpack kind    fixed size
//	bool           bool             1
//	uint8          u8               1
//	int8           i8               1
//	uint16         u16              2
//	int16          i16              2
//	uint32         u32              4
//	int32          i32              4
//	uint64         u64              8
//	int64          i64              8
//	float32        f32              4
//	float64        f64              8
//	string         string           4 (offset)
//	*string        optional<string> 4 (offset)
//	*uint32        optional<u32>    4 (offset)
//	[]string       vec<string>      4 (offset)
//	[]T            vec<T>           4 (offset)
//	StructName     object           4 (offset)
//
// Field names are converted from Go CamelCase to snake_case automatically.
// Override with a struct tag: `fracpack:"custom_name"`.
package main

import (
	"flag"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"unicode"

	"github.com/psibase/psio/fracpack"
)

func main() {
	typeFlag := flag.String("type", "", "comma-separated struct type names")
	fixedFlag := flag.String("fixed", "", "comma-separated non-extensible type names")
	outFlag := flag.String("o", "fracpack_views_gen.go", "output file name")
	flag.Parse()

	if *typeFlag == "" {
		fmt.Fprintln(os.Stderr, "fracpack-viewgen: -type is required")
		os.Exit(1)
	}

	typeNames := strings.Split(*typeFlag, ",")
	fixedSet := make(map[string]bool)
	if *fixedFlag != "" {
		for _, n := range strings.Split(*fixedFlag, ",") {
			fixedSet[strings.TrimSpace(n)] = true
		}
	}

	// Determine package name from $GOPACKAGE (set by go generate) or parse source.
	pkg := os.Getenv("GOPACKAGE")

	// Parse all Go files in the current directory.
	fset := token.NewFileSet()
	dir, _ := os.Getwd()
	pkgs, err := parser.ParseDir(fset, dir, func(fi os.FileInfo) bool {
		name := fi.Name()
		// Skip test files and generated output.
		return !strings.HasSuffix(name, "_test.go") &&
			name != filepath.Base(*outFlag)
	}, 0)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fracpack-viewgen: parse error: %v\n", err)
		os.Exit(1)
	}

	// Find the package (there should be exactly one non-test package).
	var pkgAST *ast.Package
	for _, p := range pkgs {
		pkgAST = p
		if pkg == "" {
			pkg = p.Name
		}
		break
	}
	if pkgAST == nil {
		fmt.Fprintln(os.Stderr, "fracpack-viewgen: no Go package found in current directory")
		os.Exit(1)
	}

	// Collect all struct type declarations.
	structDecls := make(map[string]*ast.StructType)
	for _, file := range pkgAST.Files {
		for _, decl := range file.Decls {
			gd, ok := decl.(*ast.GenDecl)
			if !ok || gd.Tok != token.TYPE {
				continue
			}
			for _, spec := range gd.Specs {
				ts := spec.(*ast.TypeSpec)
				st, ok := ts.Type.(*ast.StructType)
				if !ok {
					continue
				}
				structDecls[ts.Name.Name] = st
			}
		}
	}

	// Build TypeDefs from the requested types.
	// First pass: create empty TypeDefs for forward references.
	byName := make(map[string]*fracpack.TypeDef)
	var defs []*fracpack.TypeDef
	for _, name := range typeNames {
		name = strings.TrimSpace(name)
		if _, ok := structDecls[name]; !ok {
			fmt.Fprintf(os.Stderr, "fracpack-viewgen: struct %q not found\n", name)
			os.Exit(1)
		}
		td := &fracpack.TypeDef{
			Name:       name,
			Extensible: !fixedSet[name],
		}
		byName[name] = td
		defs = append(defs, td)
	}

	// Second pass: resolve fields.
	for _, td := range defs {
		st := structDecls[td.Name]
		fields, err := resolveFields(st, structDecls, byName)
		if err != nil {
			fmt.Fprintf(os.Stderr, "fracpack-viewgen: type %s: %v\n", td.Name, err)
			os.Exit(1)
		}
		td.Fields = fields
	}

	// Determine import path — if we're generating inside the fracpack
	// package itself, use internal mode (no import).
	importPath := "github.com/psibase/psio/fracpack"
	if pkg == "fracpack" {
		importPath = ""
	}

	// Generate.
	f, err := os.Create(*outFlag)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fracpack-viewgen: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	if err := fracpack.GenerateViews(f, pkg, importPath, defs); err != nil {
		fmt.Fprintf(os.Stderr, "fracpack-viewgen: %v\n", err)
		os.Exit(1)
	}
}

func resolveFields(st *ast.StructType, allStructs map[string]*ast.StructType, byName map[string]*fracpack.TypeDef) ([]fracpack.FieldDef, error) {
	var fields []fracpack.FieldDef
	for _, field := range st.Fields.List {
		if len(field.Names) == 0 {
			continue // skip embedded fields
		}
		for _, ident := range field.Names {
			if !ident.IsExported() {
				continue
			}
			// Determine fracpack field name.
			fpName := camelToSnake(ident.Name)
			if field.Tag != nil {
				tag := reflect.StructTag(strings.Trim(field.Tag.Value, "`"))
				if v, ok := tag.Lookup("fracpack"); ok && v != "" {
					fpName = v
				}
			}

			fd, err := goTypeToFieldDef(fpName, field.Type, allStructs, byName)
			if err != nil {
				return nil, fmt.Errorf("field %s: %w", ident.Name, err)
			}
			fields = append(fields, fd)
		}
	}
	return fields, nil
}

func goTypeToFieldDef(name string, expr ast.Expr, allStructs map[string]*ast.StructType, byName map[string]*fracpack.TypeDef) (fracpack.FieldDef, error) {
	fd := fracpack.FieldDef{Name: name}

	switch t := expr.(type) {
	case *ast.Ident:
		// Simple type: bool, uint32, float64, string, or struct reference.
		switch t.Name {
		case "bool":
			fd.Kind = fracpack.KindBool
			fd.FixedSize = 1
		case "uint8":
			fd.Kind = fracpack.KindU8
			fd.FixedSize = 1
		case "int8":
			fd.Kind = fracpack.KindI8
			fd.FixedSize = 1
		case "uint16":
			fd.Kind = fracpack.KindU16
			fd.FixedSize = 2
		case "int16":
			fd.Kind = fracpack.KindI16
			fd.FixedSize = 2
		case "uint32":
			fd.Kind = fracpack.KindU32
			fd.FixedSize = 4
		case "int32":
			fd.Kind = fracpack.KindI32
			fd.FixedSize = 4
		case "uint64":
			fd.Kind = fracpack.KindU64
			fd.FixedSize = 8
		case "int64":
			fd.Kind = fracpack.KindI64
			fd.FixedSize = 8
		case "float32":
			fd.Kind = fracpack.KindF32
			fd.FixedSize = 4
		case "float64":
			fd.Kind = fracpack.KindF64
			fd.FixedSize = 8
		case "string":
			fd.Kind = fracpack.KindString
			fd.FixedSize = 4
			fd.IsVar = true
		default:
			// Must be a struct reference.
			ref, ok := byName[t.Name]
			if !ok {
				// Check if the struct exists but wasn't requested.
				if _, exists := allStructs[t.Name]; exists {
					return fd, fmt.Errorf("type %q is used as a field but not listed in -type; add it", t.Name)
				}
				return fd, fmt.Errorf("unknown type %q", t.Name)
			}
			fd.Kind = fracpack.KindObject
			fd.InnerDef = ref
			fd.FixedSize = 4
			fd.IsVar = true
		}

	case *ast.StarExpr:
		// Pointer → optional.
		inner, ok := t.X.(*ast.Ident)
		if !ok {
			return fd, fmt.Errorf("unsupported optional type %T", t.X)
		}
		fd.Kind = fracpack.KindOptional
		fd.FixedSize = 4
		fd.IsVar = true
		switch inner.Name {
		case "string":
			fd.ElemKind = fracpack.KindString
		case "uint32":
			fd.ElemKind = fracpack.KindU32
		case "uint64":
			fd.ElemKind = fracpack.KindU64
		case "int32":
			fd.ElemKind = fracpack.KindI32
		case "int64":
			fd.ElemKind = fracpack.KindI64
		default:
			return fd, fmt.Errorf("unsupported optional element type %q", inner.Name)
		}

	case *ast.ArrayType:
		// Slice → vec.
		if t.Len != nil {
			return fd, fmt.Errorf("fixed-size arrays not supported, use slices")
		}
		fd.Kind = fracpack.KindVec
		fd.FixedSize = 4
		fd.IsVar = true
		inner, ok := t.Elt.(*ast.Ident)
		if !ok {
			return fd, fmt.Errorf("unsupported vec element type %T", t.Elt)
		}
		switch inner.Name {
		case "string":
			fd.ElemKind = fracpack.KindString
		case "uint32":
			fd.ElemKind = fracpack.KindU32
		case "uint64":
			fd.ElemKind = fracpack.KindU64
		case "float64":
			fd.ElemKind = fracpack.KindF64
		default:
			// Struct element.
			ref, ok := byName[inner.Name]
			if !ok {
				if _, exists := allStructs[inner.Name]; exists {
					return fd, fmt.Errorf("type %q is used as vec element but not listed in -type; add it", inner.Name)
				}
				return fd, fmt.Errorf("unknown vec element type %q", inner.Name)
			}
			fd.ElemKind = fracpack.KindObject
			fd.ElemDef = ref
		}

	default:
		return fd, fmt.Errorf("unsupported Go type %T", expr)
	}

	return fd, nil
}

// camelToSnake converts CamelCase to snake_case.
// Handles acronyms: DeviceID → device_id, AccelX → accel_x, SignalDBM → signal_dbm.
func camelToSnake(s string) string {
	var b strings.Builder
	runes := []rune(s)
	for i, r := range runes {
		if unicode.IsUpper(r) {
			if i > 0 {
				// Insert underscore before:
				// - uppercase followed by lowercase (e.g., the 'I' in "DeviceId")
				// - uppercase preceded by lowercase (e.g., the 'X' in "AccelX")
				prev := runes[i-1]
				if unicode.IsLower(prev) {
					b.WriteRune('_')
				} else if i+1 < len(runes) && unicode.IsLower(runes[i+1]) {
					// Inside an acronym run, break before the last capital
					// that starts a new word: "SignalDBM" is already at end,
					// "DeviceID" → "device_id"
					b.WriteRune('_')
				}
			}
			b.WriteRune(unicode.ToLower(r))
		} else {
			b.WriteRune(r)
		}
	}
	return b.String()
}
