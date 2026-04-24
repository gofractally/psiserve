//! psio3 — v2-architecture serialization library.
//!
//! Rust counterpart of the C++ psio3 crate. Implements the design
//! captured in `.issues/psio-v2-design.md` using Rust's proc macros
//! (today) and eventually native reflection (when that stabilizes).
//!
//! During development psio3 lives alongside psio (v1). Once every
//! phase's parity gate passes, psio3 replaces psio via a crate rename.
//!
//! Phase 0 scaffold: crate builds but exports nothing. Subsequent
//! phases add reflect / shapes / annotate / codec modules.

// Intentionally empty at Phase 0.

#[cfg(test)]
mod scaffold_test {
    #[test]
    fn crate_compiles() {
        // Proof-of-life: the crate builds and the test harness
        // runs. Real tests arrive in phase 1+.
    }
}
