extern crate proc_macro;

use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemStruct, ItemTrait, TraitItem, FnArg, ReturnType, Type,
          PathSegment};

/// Convert a Rust snake_case identifier to WIT kebab-case.
fn to_kebab(s: &str) -> String {
    s.replace('_', "-")
}

/// Convert CamelCase to kebab-case: "BalanceApi" → "balance-api"
fn camel_to_kebab(s: &str) -> String {
    let mut result = String::new();
    for (i, c) in s.chars().enumerate() {
        if c.is_uppercase() {
            if i > 0 {
                result.push('-');
            }
            result.push(c.to_ascii_lowercase());
        } else {
            result.push(c);
        }
    }
    result
}

/// Map a Rust type to its WIT representation.
fn rust_type_to_wit(ty: &Type) -> String {
    match ty {
        Type::Path(tp) => {
            let seg = tp.path.segments.last().unwrap();
            path_segment_to_wit(seg)
        }
        Type::Tuple(t) if t.elems.is_empty() => "_".to_string(),
        Type::Tuple(t) => {
            let inner: Vec<_> = t.elems.iter().map(rust_type_to_wit).collect();
            format!("tuple<{}>", inner.join(", "))
        }
        Type::Reference(r) => rust_type_to_wit(&r.elem),
        _ => panic!("Unsupported type in WIT interface: {}", quote!(#ty)),
    }
}

fn path_segment_to_wit(seg: &PathSegment) -> String {
    let name = seg.ident.to_string();
    match name.as_str() {
        "bool" => "bool".to_string(),
        "u8" => "u8".to_string(),
        "u16" => "u16".to_string(),
        "u32" => "u32".to_string(),
        "u64" => "u64".to_string(),
        "i8" => "s8".to_string(),
        "i16" => "s16".to_string(),
        "i32" => "s32".to_string(),
        "i64" => "s64".to_string(),
        "f32" => "f32".to_string(),
        "f64" => "f64".to_string(),
        "String" | "str" => "string".to_string(),
        "Vec" => {
            let args = get_generic_args(seg);
            format!("list<{}>", args[0])
        }
        "Option" => {
            let args = get_generic_args(seg);
            format!("option<{}>", args[0])
        }
        "Result" => {
            let args = get_generic_args(seg);
            let ok = if args[0] == "_" { "_".to_string() } else { args[0].clone() };
            let err = if args.len() > 1 { args[1].clone() } else { "_".to_string() };
            format!("result<{}, {}>", ok, err)
        }
        other => to_kebab(&camel_to_kebab(other)),
    }
}

fn get_generic_args(seg: &PathSegment) -> Vec<String> {
    match &seg.arguments {
        syn::PathArguments::AngleBracketed(args) => {
            args.args
                .iter()
                .map(|a| match a {
                    syn::GenericArgument::Type(t) => rust_type_to_wit(t),
                    _ => panic!("Unsupported generic argument"),
                })
                .collect()
        }
        _ => vec![],
    }
}

/// Generate WIT text for a single trait method.
fn method_to_wit(method: &syn::TraitItemFn) -> String {
    let name = to_kebab(&method.sig.ident.to_string());

    let params: Vec<String> = method
        .sig
        .inputs
        .iter()
        .filter_map(|arg| match arg {
            FnArg::Receiver(_) => None,
            FnArg::Typed(pat) => {
                let param_name = match pat.pat.as_ref() {
                    syn::Pat::Ident(id) => to_kebab(&id.ident.to_string()),
                    _ => panic!("Unsupported parameter pattern"),
                };
                let ty = rust_type_to_wit(&pat.ty);
                Some(format!("{}: {}", param_name, ty))
            }
        })
        .collect();

    let ret = match &method.sig.output {
        ReturnType::Default => String::new(),
        ReturnType::Type(_, ty) => {
            let wit_ty = rust_type_to_wit(ty);
            format!(" -> {}", wit_ty)
        }
    };

    format!("    {}: func({}){};", name, params.join(", "), ret)
}

/// Generate WIT record text from a struct's fields.
fn struct_to_wit_record(s: &ItemStruct) -> String {
    let record_name = to_kebab(&camel_to_kebab(&s.ident.to_string()));
    let fields: Vec<String> = match &s.fields {
        syn::Fields::Named(named) => {
            named.named.iter().map(|f| {
                let fname = to_kebab(&f.ident.as_ref().unwrap().to_string());
                let ftype = rust_type_to_wit(&f.ty);
                format!("    {}: {},", fname, ftype)
            }).collect()
        }
        _ => panic!("wit_record only supports named fields"),
    };
    format!("  record {} {{\n{}\n  }}", record_name, fields.join("\n"))
}

/// Encode WIT text into Component Model binary bytes using wit-component.
fn encode_wit(wit_text: &str) -> Vec<u8> {
    use wit_component::{metadata, StringEncoding};
    use wit_parser::{Resolve, UnresolvedPackageGroup};

    let mut resolve = Resolve::default();
    let unresolved = UnresolvedPackageGroup::parse("macro.wit", wit_text)
        .unwrap_or_else(|e| panic!("Failed to parse generated WIT:\n{}\nError: {}", wit_text, e));
    let pkg_id = resolve
        .push_group(unresolved)
        .unwrap_or_else(|e| panic!("Failed to resolve WIT package: {}", e));

    let pkg = &resolve.packages[pkg_id];
    let world_id = *pkg
        .worlds
        .values()
        .next()
        .expect("WIT package must contain a world");

    metadata::encode(&resolve, world_id, StringEncoding::UTF8, None)
        .expect("Failed to encode WIT as component-type")
}

// =============================================================================
// #[wit_record] — generate WIT record from a Rust struct
// =============================================================================

/// Attribute macro that generates a WIT record definition const from a Rust struct.
#[proc_macro_attribute]
pub fn wit_record(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let struct_def = parse_macro_input!(item as ItemStruct);
    let wit_text = struct_to_wit_record(&struct_def);

    let const_name = syn::Ident::new(
        &format!("{}_WIT_RECORD", struct_def.ident.to_string().to_uppercase()),
        struct_def.ident.span(),
    );
    let wit_literal = proc_macro2::Literal::string(&wit_text);

    let output = quote! {
        #struct_def

        /// Generated WIT record definition for this struct.
        pub const #const_name: &str = #wit_literal;
    };

    output.into()
}

// =============================================================================
// wit_world! — bundle records + trait into a complete WIT package
// =============================================================================

/// Procedural macro that takes struct definitions and a trait definition,
/// generates a complete WIT package with records and interface, and embeds
/// it as a component-type custom section.
///
/// ```rust,ignore
/// wit_macro::wit_world! {
///     package = "test:inventory@1.0.0";
///
///     pub struct Item {
///         pub id: u64,
///         pub name: String,
///     }
///
///     pub trait InventoryApi {
///         fn get_item(&self, id: u32) -> Option<Item>;
///     }
/// }
/// ```
#[proc_macro]
pub fn wit_world(input: TokenStream) -> TokenStream {
    let world_input = parse_macro_input!(input as WorldInput);

    let interface_name = to_kebab(&camel_to_kebab(&world_input.trait_def.ident.to_string()));
    let world_name = format!("{}-world", interface_name);

    // Generate WIT records from structs
    let record_wits: Vec<String> = world_input.structs.iter()
        .map(|s| struct_to_wit_record(s))
        .collect();

    // Generate WIT functions from trait
    let mut wit_funcs = Vec::new();
    for item in &world_input.trait_def.items {
        if let TraitItem::Fn(method) = item {
            wit_funcs.push(method_to_wit(method));
        }
    }

    let records_section = if record_wits.is_empty() {
        String::new()
    } else {
        format!("\n{}\n\n", record_wits.join("\n\n"))
    };

    let wit_text = format!(
        "package {};\n\ninterface {} {{{}\n{}\n}}\n\nworld {} {{\n    export {};\n}}\n",
        world_input.package,
        interface_name,
        records_section,
        wit_funcs.join("\n"),
        world_name,
        interface_name,
    );

    // Encode to Component Model binary
    let encoded_bytes = encode_wit(&wit_text);
    let byte_count = encoded_bytes.len();
    let byte_literals: Vec<proc_macro2::TokenTree> = encoded_bytes
        .iter()
        .map(|b| proc_macro2::Literal::u8_suffixed(*b).into())
        .collect();

    let section_name = format!("component-type:{}", interface_name);
    let trait_name = world_input.trait_def.ident.to_string();
    let static_name = syn::Ident::new(
        &format!("__WIT_{}", trait_name.to_uppercase()),
        world_input.trait_def.ident.span(),
    );
    let wit_const_name = syn::Ident::new(
        &format!("{}_WIT", trait_name.to_uppercase()),
        world_input.trait_def.ident.span(),
    );
    let wit_text_literal = proc_macro2::Literal::string(&wit_text);

    let structs = &world_input.structs;
    let trait_def = &world_input.trait_def;

    let output = quote! {
        #(#structs)*

        #trait_def

        /// Generated WIT text for this world.
        pub const #wit_const_name: &str = #wit_text_literal;

        /// Component Model binary encoding, embedded as a WASM custom section.
        #[cfg(target_arch = "wasm32")]
        #[unsafe(link_section = #section_name)]
        #[used]
        static #static_name: [u8; #byte_count] = [#(#byte_literals),*];
    };

    output.into()
}

// =============================================================================
// Parser for wit_world! macro input
// =============================================================================

struct WorldInput {
    package: String,
    structs: Vec<ItemStruct>,
    trait_def: ItemTrait,
}

impl syn::parse::Parse for WorldInput {
    fn parse(input: syn::parse::ParseStream) -> syn::Result<Self> {
        // Parse: package = "...";
        let _: syn::Ident = input.parse()?; // "package"
        let _: syn::Token![=] = input.parse()?;
        let pkg: syn::LitStr = input.parse()?;
        let _: syn::Token![;] = input.parse()?;

        let mut structs = Vec::new();
        let mut trait_def = None;

        while !input.is_empty() {
            // Peek to see if it's a struct or trait
            let fork = input.fork();
            // Skip visibility
            let _vis: syn::Visibility = fork.parse()?;

            if fork.peek(syn::Token![struct]) {
                let s: ItemStruct = input.parse()?;
                structs.push(s);
            } else if fork.peek(syn::Token![trait]) {
                trait_def = Some(input.parse()?);
            } else {
                return Err(input.error("expected struct or trait"));
            }
        }

        Ok(WorldInput {
            package: pkg.value(),
            structs,
            trait_def: trait_def.ok_or_else(|| input.error("expected a trait definition"))?,
        })
    }
}
