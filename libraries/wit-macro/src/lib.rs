extern crate proc_macro;

use proc_macro::TokenStream;
use quote::quote;
use syn::{parse_macro_input, ItemTrait, TraitItem, FnArg, ReturnType, Type, PathSegment};

/// Convert a Rust snake_case identifier to WIT kebab-case.
fn to_kebab(s: &str) -> String {
    s.replace('_', "-")
}

/// Map a Rust type to its WIT representation.
fn rust_type_to_wit(ty: &Type) -> String {
    match ty {
        Type::Path(tp) => {
            let seg = tp.path.segments.last().unwrap();
            path_segment_to_wit(seg)
        }
        Type::Tuple(t) if t.elems.is_empty() => "_".to_string(), // unit
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
        other => to_kebab(other),
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

/// Generate WIT text for a single method.
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

/// Parse the `#[wit_interface(package = "...")]` attribute arguments.
struct InterfaceArgs {
    package: String,
}

impl InterfaceArgs {
    fn parse(attr: TokenStream) -> Self {
        let mut package = None;

        let parser = syn::meta::parser(|meta| {
            if meta.path.is_ident("package") {
                let value: syn::LitStr = meta.value()?.parse()?;
                package = Some(value.value());
                Ok(())
            } else {
                Err(meta.error("expected `package = \"...\"`"))
            }
        });

        syn::parse::Parser::parse(parser, attr).expect("Failed to parse wit_interface arguments");

        InterfaceArgs {
            package: package.expect("#[wit_interface(package = \"...\")] is required"),
        }
    }
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

/// Attribute macro that generates WIT from a Rust trait and embeds it
/// as a `component-type` custom section in the WASM binary.
///
/// # Usage
///
/// ```rust,ignore
/// #[wit_interface(package = "myapp:balance@1.0.0")]
/// pub trait BalanceApi {
///     fn get_balance(&self, account_id: u32) -> u64;
///     fn transfer(&self, sender: u32, receiver: u32, amount: u64) -> Result<(), String>;
/// }
/// ```
#[proc_macro_attribute]
pub fn wit_interface(attr: TokenStream, item: TokenStream) -> TokenStream {
    let args = InterfaceArgs::parse(attr);
    let trait_def = parse_macro_input!(item as ItemTrait);

    // Derive interface name from trait: BalanceApi → balance-api
    let trait_name = trait_def.ident.to_string();
    let interface_name = to_kebab(&camel_to_kebab(&trait_name));

    // Build WIT text from trait methods
    let mut wit_funcs = Vec::new();
    for item in &trait_def.items {
        if let TraitItem::Fn(method) = item {
            wit_funcs.push(method_to_wit(method));
        }
    }

    let world_name = format!("{}-world", interface_name);
    let wit_text = format!(
        "package {};\n\ninterface {} {{\n{}\n}}\n\nworld {} {{\n    export {};\n}}\n",
        args.package,
        interface_name,
        wit_funcs.join("\n"),
        world_name,
        interface_name,
    );

    // Encode to Component Model binary
    let encoded_bytes = encode_wit(&wit_text);
    let byte_count = encoded_bytes.len();
    let byte_literals: Vec<proc_macro2::TokenTree> = encoded_bytes
        .iter()
        .map(|b| {
            proc_macro2::Literal::u8_suffixed(*b).into()
        })
        .collect();

    let section_name = format!("component-type:{}", interface_name);
    let static_name = syn::Ident::new(
        &format!("__WIT_{}", trait_name.to_uppercase()),
        trait_def.ident.span(),
    );

    let wit_const_name = syn::Ident::new(
        &format!("{}_WIT", trait_name.to_uppercase()),
        trait_def.ident.span(),
    );
    let wit_text_literal = proc_macro2::Literal::string(&wit_text);

    let output = quote! {
        #trait_def

        /// Generated WIT text for this interface.
        pub const #wit_const_name: &str = #wit_text_literal;

        /// Component Model binary encoding, embedded as a WASM custom section
        /// when compiled to wasm32.
        #[cfg(target_arch = "wasm32")]
        #[unsafe(link_section = #section_name)]
        #[used]
        static #static_name: [u8; #byte_count] = [#(#byte_literals),*];
    };

    output.into()
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
