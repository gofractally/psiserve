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

    // Pre-allocate seed buffer
    let mut seed_buf = vec![0u8; 4096];

    for _ in 0..count {
        rng.fill_bytes(&mut seed_buf);
        let mut u = Unstructured::new(&seed_buf);

        let config = wasm_smith::Config {
            max_memories: 1,
            max_tables: 2,
            max_instructions: 1000,
            max_memory32_bytes: 655360,
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
