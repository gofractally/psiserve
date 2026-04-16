//! PSIO — Multi-format serialization library.
//!
//! Supports fracpack, Cap'n Proto, FlatBuffers, WIT Canonical ABI, JSON,
//! and more from a single set of derive macros.
//!
//! # Fracpack (default format)
//!
//! ```
//! use psio::{Pack, Unpack, Result};
//!
//! #[derive(Pack, Unpack, PartialEq, Debug)]
//! #[fracpack(fracpack_mod = "psio")]
//! struct Example {
//!     a_string: String,
//!     a_tuple: (u32, String),
//! }
//!
//! let orig = Example {
//!     a_string: "content".into(),
//!     a_tuple: (1234, "5678".into()),
//! };
//!
//! let packed: Vec<u8> = orig.packed();
//! let unpacked = Example::unpacked(&packed)?;
//! assert_eq!(orig, unpacked);
//! # Ok::<(), psio::Error>(())
//! ```

// Core fracpack format (re-exported at top level for convenience)
#[path = "fracpack.rs"]
mod fracpack_impl;

pub use fracpack_impl::*;

// New modules (populated in subsequent phases)
pub mod xxh64;
pub mod dynamic_schema;

// Multi-format wire formats
pub mod capnp;
pub mod flatbuf;
pub mod wit;

// Dynamic dispatch (runtime field access by name)
pub mod dynamic_view;

// Schema tooling
pub mod schema_export;
pub mod schema_import;
