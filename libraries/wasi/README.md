# WASI bindings

Vendored WASI `.wit` interface definitions plus per-language bindings
against them.

## Layout

```
libraries/wasi/
├── wit/                       canonical WASI .wit (vendored upstream)
│   └── <version>/...
└── cpp/                       PSIO-reflected C++ bindings
    ├── CMakeLists.txt
    ├── include/wasi/
    │   └── <version>/*.hpp
    └── tests/
```

- `wit/` is language-neutral; any binding tree consumes it.
- `cpp/` uses PSIO reflection (`PSIO_PACKAGE` / `PSIO_INTERFACE` /
  `PSIO_REFLECT`) so `psio::emit_wit` round-trips back to the vendored
  `.wit`, and psiserve `Linker<world>` can wire `PSIO_IMPL` host
  implementations against any import.

Other language trees (Rust, Go, Python, JS, …) are out of scope.
Those ecosystems have mature tooling — `wit-bindgen`, `jco`,
`componentize-py`, `wit-bindgen-go`, etc. — that consumes `wit/`
directly. PSIO's scope ends at emitting clean WIT.

## Versioning

Each WASI release gets its own subdirectory
(`wit/0.2.3/`, `cpp/include/wasi/0.2.3/`, …). Multiple versions may
coexist during migration. Released versions are immutable once
vendored; updates add a new version subdirectory.

## Vendoring

See `wit/README.md` for the update procedure.
