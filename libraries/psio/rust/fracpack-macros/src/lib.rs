//! Derive macros for the [fracpack crate](https://docs.rs/fracpack).
//!
//! Provides `Pack`, `Unpack`, `FracView`, `FracMutView`, and `ToSchema` derive macros.

use fracpack_macro::{fracmutview_macro_impl, fracpack_macro_impl, fracview_macro_impl};
use json_macro::{from_canonical_json_macro_impl, to_canonical_json_macro_impl};
use proc_macro::TokenStream;
use proc_macro_error::proc_macro_error;
use schema_macro::schema_derive_macro;

mod fracpack_macro;
mod json_macro;
mod schema_macro;

// TODO: remove
#[proc_macro_derive(Fracpack, attributes(fracpack))]
pub fn derive_fracpack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, true, true)
}

#[proc_macro_derive(Pack, attributes(fracpack))]
pub fn derive_pack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, true, false)
}

#[proc_macro_derive(Unpack, attributes(fracpack))]
pub fn derive_unpack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, false, true)
}

#[proc_macro_error]
#[proc_macro_derive(FracView, attributes(fracpack))]
pub fn derive_frac_view(input: TokenStream) -> TokenStream {
    fracview_macro_impl(input)
}

#[proc_macro_error]
#[proc_macro_derive(FracMutView, attributes(fracpack))]
pub fn derive_frac_mut_view(input: TokenStream) -> TokenStream {
    fracmutview_macro_impl(input)
}

#[proc_macro_derive(ToSchema, attributes(schema, fracpack))]
pub fn to_schema(input: TokenStream) -> TokenStream {
    schema_derive_macro(input)
}

#[proc_macro_derive(ToCanonicalJson, attributes(fracpack))]
pub fn derive_to_canonical_json(input: TokenStream) -> TokenStream {
    to_canonical_json_macro_impl(input)
}

#[proc_macro_derive(FromCanonicalJson, attributes(fracpack))]
pub fn derive_from_canonical_json(input: TokenStream) -> TokenStream {
    from_canonical_json_macro_impl(input)
}
