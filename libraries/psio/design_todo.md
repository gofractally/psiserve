
  PSIO
    - dynamic verification of binary blobs given schemas...
    - there are N different ways of expressing a schema (FracPack, FlatBuf, CapNProto, others...) but all of them
    fundamentally point to the same underlying datatypes...
    - so given any of these schemas, and a binary, we should be able to determine if the binary conforms.
       * what I mean is this, I can verify whether a binary.fracpack is compliant with the schema in any of the above..
       * this means users can pick any schema they want to express their datatypes, and validate any binary format they want
    - this dynamic validation means we need the ability not just to generate these schemas, but to parse them
    - for frac-pack specifically, we need to parse them and compile their essence into something that allows rapid verification
      of whether or not a .frac buffer is compatible with the schema
          - use case, a database table uses the schema to define rows, gets an insert request with an untrusted binary blob,
            needs to determine whether:
                 a. exact compat - everything matches
                 b. forward compat - elements in data that are not in schema
                 c. backward compat - schema defines elements not in data
                 d. exact and canonical


  ## Three representations of schema (design note)

  A "schema" is three artifacts, not one. Each serves a distinct purpose and
  is optimized differently:

  1. **Exchange IR** — `AnyType` / `Schema` in `schema.hpp`. Rich, self-hosting
     (FracPack-serializable), lossless. Cross-language/cross-tool interchange.
     Carries names, docs, semantic tags, alternative encodings. NOT indexed
     for rapid validation.

  2. **Match IR** — `CompiledType`. Resolved references, cached structure.
     Purpose: schema-vs-schema graded compat via `match()` (`SchemaDifference`).
     NOT for validating a blob against a schema.

  3. **Validator program** — *missing*. Specialized to one schema × one wire
     format. For FracPack: a flat worklist with
      - fixed-region size check,
      - per-pointer-slot: offset in-bounds, forward-only, no-overlap,
      - extension-table heap-offset monotonicity + size consistency,
      - recursive descent precomputed (not tree-walked),
      - semantic checks (UTF-8, sorted-keys, unique-keys) as a *separate*
        optional pass gated by invariant flags on the IR.

  Relationship: source → AST → bytecode. You would not interpret an AST on
  the hot path. Same principle here: lower the Exchange IR into a
  Validator Program before serving high-volume ingestion traffic.

  Shape validation is load-bearing for the view API: once shape is proven
  (all pointers deref-safe, no overlapping regions), views can trust byte
  ranges and serve O(log N) binary-search lookups over packed bytes.
  Semantic validation (sorted invariant, UTF-8) is the second layer — run
  at ingestion, once, before exposing a view.

  ## Tooling to build

  - [x] `CompiledValidator` + `compile(schema, root_type_name)`: bundles a
        `CompiledSchema` with a resolved root `CompiledType*`. Shipped in
        `schema.hpp`. Self-hosting roundtrip (schema as FracPack → rehydrate →
        compile → validate) covered by `fracpack_validate_tests.cpp`.
  - [x] Rapid validator runtime (shape-only): the existing
        `fracpack_validate(data, CompiledSchema, type)` is now wrapped by
        `fracpack_validate(data, CompiledValidator)` — the single-arg
        compiled artifact form.
  - [ ] **Graded return**: today we return `validation_t{invalid,valid,extended}`.
        The vision is `SchemaDifference{equivalent, upgrade, downgrade,
        incompatible}` with a diagnostic diff. `extended` maps to upgrade;
        downgrade is not currently detected (missing-optional-trailing case).
  - [ ] Semantic validator pass: optional, runs after shape validation,
        checks invariants declared in the IR (UTF-8, sorted, unique keys).
        Blocked on IR enrichment (promote string/map/bool to first-class
        `AnyType` cases so the invariants have a home).
  - [ ] Validator program as a third representation: lower `CompiledType`
        tree into a flat worklist specialized for FracPack rapid validation.
        Optimization over the current tree walk — defer until profiling
        shows it's needed.
  - [ ] One-call API: `validate(schema_bytes, blob_bytes)` that decodes the
        `Schema` FracPack + compiles + validates in one call. Requires
        either (a) the schema blob naming its root, or (b) the caller
        providing the root name.
  - [ ] Self-describing envelope: ship blobs as FracPack of `{Schema, blob}`.
        Deferred — the pair form is trivial once the one-call API lands.

  ## Authored schema format (open question)

  FracPack is a reflection export, not an authored schema. For humans who
  want to write schemas directly — or for human-readable exports — candidates:

  - **WIT** (W3C WASI Component Model) — best-fit today. Expressive type
    algebra (record/variant/enum/flags/list/option/result/tuple/resource),
    has interfaces/packages/worlds for RPC, stable W3C spec. Already parsed.
    Semantic invariants can ride on documented attributes / comments.
  - **Cap'n Proto `.capnp`** — readable, evolution-aware, but interface model
    and type algebra are narrower than WIT.
  - **FlatBuffers `.fbs`** — readable but less expressive (no sum types with
    payload in the traditional sense).
  - **Protobuf `.proto`** — ubiquitous but carries Google-specific baggage.

  Recommended path: **WIT + documented attribute extensions for semantic
  invariants** (`@sorted`, `@unique-keys`, `@canonical`, etc.) as the
  authored form. Parse WIT into the Exchange IR; unknown attributes pass
  through. Export back to WIT with the same attribute vocabulary for
  human-consumption roundtrip.
