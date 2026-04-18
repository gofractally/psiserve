# Vendored WASI WIT

Canonical WASI interface definitions, copied verbatim from upstream
(https://github.com/WebAssembly/WASI). Never edited locally.

## Layout

```
wit/
├── 0.2.3/
│   ├── io/{streams,poll,error-context}.wit
│   ├── clocks/{monotonic-clock,wall-clock,timezone}.wit
│   ├── random/{random,insecure,insecure-seed}.wit
│   ├── filesystem/{types,preopens}.wit
│   ├── sockets/{network,instance-network,ip-name-lookup,
│   │             tcp,tcp-create-socket,udp,udp-create-socket}.wit
│   ├── http/{types,incoming-handler,outgoing-handler}.wit
│   └── cli/{command,environment,exit,stdin,stdout,stderr,...}.wit
└── <future versions>/
```

## Updating

1. Pick an upstream release tag (e.g. `wasi-0.2.3`).
2. Create the matching version subdirectory if it doesn't exist.
3. Copy the `.wit` files under it, preserving the directory structure.
4. Run `ctest` — the C++ round-trip test (`cpp/tests/round_trip`) will
   flag any structural drift that the C++ bindings need to track.
5. If `deps.toml` tracks pinned revisions, update the entry.

## Why not edit in place

Released WASI versions are part of other people's wire contracts. An
edit here would silently change the contract psiserve implements.
Subsequent versions go in a new subdirectory; the old one stays frozen.
