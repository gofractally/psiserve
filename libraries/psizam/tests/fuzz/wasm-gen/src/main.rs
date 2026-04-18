// wasm-gen: bulk WASM module generator for differential fuzzing
//
// Generates N valid WASM modules using wasm-smith and writes them
// to stdout as length-prefixed records: [u32le length][wasm bytes]...
//
// Usage: wasm-gen <count> [seed]

use arbitrary::Unstructured;
use rand::rngs::StdRng;
use rand::{RngCore, SeedableRng};
use std::io::{self, Write};
use wasm_smith::Module;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let count: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1000);
    let seed: u64 = args
        .get(2)
        .and_then(|s| s.parse().ok())
        .unwrap_or_else(|| {
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos() as u64
        });

    let stdout = io::stdout();
    let mut out = io::BufWriter::new(stdout.lock());
    let mut rng = StdRng::seed_from_u64(seed);
    let mut generated = 0u32;
    let mut failed = 0u32;

    // Pre-allocate seed buffer. Size is chosen so wasm-smith rarely runs out
    // of entropy before reaching max_instructions on complex modules.
    let mut seed_buf = vec![0u8; 16384];

    for _ in 0..count {
        rng.fill_bytes(&mut seed_buf);
        let mut u = Unstructured::new(&seed_buf);

        let config = wasm_smith::Config {
            // Resource limits
            max_memories: 1,
            max_tables: 2,
            max_instructions: 2500,
            max_memory32_bytes: 655360,
            // Disable proposals not fully supported by all backends
            gc_enabled: false,
            relaxed_simd_enabled: false,
            custom_page_sizes_enabled: false,
            exceptions_enabled: true,
            reference_types_enabled: true,
            memory64_enabled: false,         // limited JIT support
            threads_enabled: false,          // shared memory not supported in fuzzer
            // Enabled — we want to surface bugs in these, not hide them
            wide_arithmetic_enabled: true,
            tail_call_enabled: true,
            simd_enabled: true,
            bulk_memory_enabled: true,
            multi_value_enabled: true,
            sign_extension_ops_enabled: true,
            saturating_float_to_int_enabled: true,
            extended_const_enabled: true,
            ..Default::default()
        };

        match Module::new(config, &mut u) {
            Ok(mut module) => {
                // Add fuel-based termination checks to all loops/calls
                let _ = module.ensure_termination(100);
                let wasm = module.to_bytes();
                let len = wasm.len() as u32;
                out.write_all(&len.to_le_bytes()).unwrap();
                out.write_all(&wasm).unwrap();
                generated += 1;
            }
            Err(_) => {
                failed += 1;
            }
        }
    }
    out.flush().unwrap();

    eprintln!(
        "wasm-gen: {} generated, {} failed (seed={})",
        generated, failed, seed
    );
}
