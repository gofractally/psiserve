Fracpack Competitive Analysis
==============================

A deep comparison of fracpack against the major binary serialization
formats across every dimension that matters for production systems.


At a Glance
-----------

Columns ordered left-to-right by similarity to fracpack.

                        Frac  WIT  Cap'n Flat       Proto       Msg        Bin  ASN1
                        pack  ABI  Proto Buf  Borsh buf  CBOR   Pack Avro  code DER  BSON JSON Boost
                        ----  ---  ----- ---- ----- ---- ----   ---- ----  ---- ---- ---- ---- -----
  DATA ACCESS
  Zero-copy views        Y     Y    Y     Y    .     .    .      .    .     .    .    .    .    .
  Safe in-place mutate   Y     Y    Y     !!   .     .    .      .    .     .    .    ~    .    .
  Compact (no padding)   Y     .    .     ~    Y     Y    Y      Y    Y     Y    ~    .    .    ~

  CORRECTNESS
  Canonical encoding     Y     Y    ~     .    Y     .    Y      .    .     ~    Y    .    .    .
  Validate w/o unpack    Y     ~    Y     Y    .     .    .      .    .     .    .    .    .    .

  EVOLUTION
  Forward compat [1]   Y [3]  .    Y     Y    .     Y    ~      ~    ~     .    ~    ~    Y    ~
  Backward compat [2]  Y [3]  .    Y     Y    .     Y    ~      ~    Y     .    ~    ~    ~    ~

  SCHEMA & DX
  No codegen build step  Y     ~    .     .    Y     .    Y      Y    ~     Y    .    Y    Y    Y
  Schema from code [4]   Y     Y    .     .    ~     .    .      .    .     ~    .    .    .    ~
  Use your own types     Y     Y    .     .    Y     .    ~      ~    .     Y    .    ~    ~    Y
  Any-order construct    Y     Y    .     .    Y     Y    Y      Y    ~     Y    Y    Y    Y    Y
  Self-describing        .     .    .     .    .     ~    Y      Y    .     .    ~    Y    Y    ~
  Binary format          Y     Y    Y     Y    Y     Y    Y      Y    Y     Y    Y    Y    .    Y

  STRUCTURE
  Graph/DAG support      .     .    .     ~    .     .    ~      .    .     .    .    .    .    Y

                        ----  ---  ----- ---- ----- ---- ----   ---- ----  ---- ---- ---- ---- -----
  Languages              6    all   10+   14+   7    11+  20+    50+  10+    1   all  12+  all   1

  Y = yes   ~ = partial   !! = yes but unsafe   . = no

  [1] Forward compatible: old code safely reads new data
  [2] Backward compatible: new code safely reads old data
  [3] Opt-in per struct. Extensible (default) gets evolution;
      fixedStruct trades it for zero overhead. No other format
      offers this per-type choice.
  [4] Schema derived from source types at compile time. Different
      from "schema-less" (MessagePack/JSON have no schema at all)
      and from "schema-first" (protobuf writes IDL, generates code).
      Fracpack derives a schema from code and exports it two ways:
      AnyType (fracpack's own type vocabulary, available as JSON or
      as fracpack binary — the schema format is self-hosting) or
      WIT (sufficient for common case — see §10 for two edge gaps).
      Both exports + fracpack encoding rules = encode/decode.
      Borsh/bincode derive schemas but cannot export them.

  Fracpack is the ONLY format that delivers all of: canonical encoding
  + zero-copy views + forward & backward compat + code-first schema
  + dense packing + safe in-place mutation.


Contenders
----------

  Format            Origin                    Primary Use Case
  ----------------  ------------------------  -------------------------------------------
  Fracpack          Fractally                 WASM IPC, blockchain state, cross-lang interop
  WIT Canonical ABI W3C WASI (Component Mdl)  WASM component boundary data passing
  Protocol Buffers  Google                    RPC (gRPC), general interchange
  FlatBuffers       Google                    Game engines, zero-copy access
  Cap'n Proto       Sandstorm (Kenton Varda)  Zero-copy RPC with promise pipelining
  MessagePack       Sadayuki Furuhashi        Schema-less binary JSON replacement
  CBOR              IETF (RFC 8949)           IoT, WebAuthn, constrained devices
  Avro              Apache                    Hadoop/Kafka bulk data pipelines
  Bincode           Rust ecosystem            Rust-to-Rust fast serialization
  Borsh             NEAR Protocol             Blockchain consensus, deterministic hashing
  ASN.1/DER         ITU-T                     X.509 certificates, telecom
  BSON              MongoDB                   Document databases
  JSON              Douglas Crockford         Universal text interchange
  Boost.Serial.     Boost (Robert Ramey)      C++ object persistence, graph serialization


========================================================================
1. Forward Compatibility (old code reads new data)
========================================================================

Can software compiled against schema v1 safely read data written with
schema v2 (which has new fields the old code doesn't know about)?

FRACPACK: ✅  Extensible structs (the default) carry a u16 header that
records the actual fixed-region size. Old code sees a larger-than-
expected fixed region, reads the fields it knows, and skips the unknown
tail. The variant u32 content_size lets old code skip unknown variant
cases. The validation layer reports "extended" (not "invalid") when it
detects unknown fields, so the application can choose whether to accept
or reject.

This is opt-in at the struct level. Fixed structs (fixedStruct /
fracpack_fixed) deliberately sacrifice extensibility for density: no
u16 header, no ability to add fields later, but zero overhead. The
programmer chooses per type -- coordinate pairs, hash digests, and
color values get fixedStruct; API messages and database rows get the
extensible default. No other format offers this per-type choice.

WIT CANONICAL ABI: ·  The Canonical ABI uses positional layout with
natural alignment. Fields are at fixed byte offsets determined by the WIT
record definition order and type alignment rules. There is no header,
no length prefix, no field tags. Adding a field changes the offsets of
subsequent fields and the overall struct size. This is a breaking change
-- old code reading new data will misinterpret bytes. The Component Model
does not provide schema evolution; interface versioning is handled at
the world/package level (semantic versioning of the entire interface),
not at the field level.

CAP'N PROTO: ✅  Struct pointers encode data-section and pointer-section
word counts. Old code sees smaller section sizes in its compiled schema
and simply doesn't read past them. Unknown data in the expanded sections
is ignored. Works cleanly.

FLATBUFFERS: ✅  The vtable maps field slots to offsets. Old code's
vtable has fewer slots, so it never looks up the new fields. The unknown
data sits in the buffer untouched. Clean and safe.

PROTOCOL BUFFERS: ✅  Fields are tagged with numbers. Old code encounters
an unknown field number, reads its wire type to determine how many bytes
to skip, and moves on. Unknown fields are even preserved and re-
serialized, giving round-trip safety. The strongest forward-compat story
in the industry.

JSON: ✅  Old code simply ignores unknown keys in the object. Trivially
forward-compatible by convention (as long as the application doesn't
reject unknown keys).

AVRO: ◐  Requires the writer's schema to be available at read time. If
the reader has access to the writer's schema, it can skip fields that
don't appear in its own schema. Without the writer's schema, the data is
unparseable (no field tags in the wire format). Partial credit.

MESSAGEPACK / CBOR / BSON: ◐  Schema-less formats. Unknown keys in maps
are naturally skipped. But there is no schema to tell you what "unknown"
means -- it's the application's responsibility to handle unexpected data
gracefully. No structural mechanism; just convention.

ASN.1/DER: ◐  Extensibility markers (...) in the schema declare where
new elements may appear. Old parsers can skip unknown TLV elements by
reading the length and jumping ahead. Works in theory, but the
complexity of the format makes correct implementation difficult.

BORSH / BINCODE: ·  No mechanism. Data is purely positional. A new field
shifts all subsequent byte offsets, causing old code to read garbage.
Adding a field to a struct is a breaking change, full stop.


========================================================================
2. Backward Compatibility (new code reads old data)
========================================================================

Can software compiled against schema v2 safely read data written with
schema v1 (which is missing fields the new code expects)?

FRACPACK: ✅  For extensible structs (the default), the u16 header tells
the reader how much fixed region is actually present. If the fixed region
is shorter than expected, missing trailing fields are treated as absent
optionals. New fields must be optional and must be appended to the end --
but that constraint is enforced by the type system. Fixed structs opt out
of this: no header, no evolution, no overhead. The choice is per-type.

WIT CANONICAL ABI: ·  Same as forward compat. Positional layout with no
versioning mechanism. Old data is shorter than new code expects. The
Component Model relies on semantic versioning of entire interfaces
(world/package), not field-level evolution.

CAP'N PROTO: ✅  New code sees the old data's smaller section sizes and
fills in defaults (XOR'd zeros) for the missing fields. Clean.

FLATBUFFERS: ✅  New code's vtable has more slots. When it looks up a
new field's offset in the old data's vtable, it gets zero (absent),
and the application receives the schema-declared default value.

PROTOCOL BUFFERS: ✅  New code expects a field number that never appears
in the old data. The missing field takes its default value (zero, empty
string, etc. in proto3; explicitly declared defaults in proto2). Works
seamlessly.

AVRO: ✅  The reader's schema declares defaults for new fields. When the
runtime resolves the writer's schema (which lacks the field) against the
reader's schema (which has it with a default), the default is used. The
most flexible backward-compat mechanism -- supports field reordering and
name aliases, not just appending.

JSON: ◐  New code checks for a key and doesn't find it. Whether this
works depends entirely on the application -- there is no schema to
declare defaults. Most JSON libraries return null/nil for missing keys,
which is usually fine, but there's no guarantee.

MESSAGEPACK / CBOR / BSON: ◐  Same as JSON -- schema-less, so "missing
field" is an application concept, not a format concept. Works by
convention, not by mechanism.

ASN.1/DER: ◐  OPTIONAL and DEFAULT keywords in the schema let new code
handle missing elements. But the parser must know which elements are
optional and in what order they appear -- the TLV tags disambiguate, but
getting it right across schema versions is error-prone.

BORSH / BINCODE: ·  No mechanism. Old data is shorter than new code
expects. The parser reads past the end of the buffer or misinterprets
bytes. Breaking change.


========================================================================
3. Validation and Security
========================================================================

How much can you trust untrusted data?

FRACPACK enforces structural validation without unpacking:

  - Offset integrity: every relative offset must point within the buffer
    and must not wrap around.
  - Boolean strictness: values must be 0x00 or 0x01, not arbitrary
    non-zero.
  - Variant tag bounds: tag byte must be < 128 (bit 7 reserved) and
    within the known set of cases.
  - Container size consistency: vec fixed_bytes must match element count
    times element size. Nested object headers must be plausible.
  - Canonical detection: the validator distinguishes canonical form
    (trailing optionals trimmed, empty containers use offset 0) from
    non-canonical but decodable forms. Non-canonical data is valid
    for reading; canonical form is required for hashing/signing.
  - Three-state result: valid (canonical match), extended (forward-
    compatible unknowns detected), invalid (reject).

The security surface is small because fracpack uses forward-only relative
offsets (not general pointers). Every offset points ahead in the buffer,
never backward -- this structurally prevents reference cycles and
guarantees any traversal terminates in bounded time without a traversal
limiter. There are no alignment requirements to get wrong, no variable-
length integer encoding to overflow, and no self-describing type tags
to confuse.

CAP'N PROTO has the largest attack surface among zero-copy formats. Its
pointer-based design has produced real CVEs: list-of-struct downgrade
attacks that escape bounds checks, CPU amplification via zero-element-
size lists (claiming 2^29 elements in 8 bytes), and integer overflow in
pointer offset calculation. The format requires a traversal limiter to
cap total bytes visited.

FLATBUFFERS provides a Verifier that checks bounds, alignment, and
offset validity. Critical limitation: it does NOT detect overlapping
data structures. In-place mutation of a verified but adversarial buffer
can corrupt unrelated tables because the verifier cannot rule out
structural aliasing.

PROTOCOL BUFFERS does minimal validation. The parser trusts the data
structurally -- it will read a varint and interpret whatever value it
finds. There is no verification pass separate from deserialization.
Malformed varints or truncated length-delimited fields cause parse
errors, but there is no mechanism to validate a buffer without parsing
it.

BORSH is designed for security-critical environments (blockchain
consensus). Its canonical encoding prevents hash-collision-via-alternate-
encoding attacks. The Rust implementation is almost entirely safe code.
However, it provides no structural validation separate from
deserialization -- if deserialization succeeds, the data was valid.

ASN.1/DER has the worst security track record in the industry. The
complexity of the format (variable-length tags, variable-length-of-
lengths, dozens of types, multiple encoding rules) has produced decades
of parser vulnerabilities, including buffer overflows, integer overflows,
and stack overflows from deep nesting. Let's Encrypt's documentation
describes ASN.1 as having "a long history of vulnerabilities in
decoders."

VERDICT: Fracpack's validation is thorough yet simple. The format avoids
the features that cause security problems in competitors: no pointers
(Cap'n Proto), no overlapping structures (FlatBuffers), no variable-
length integers in structural positions (protobuf), no variable-length-
of-lengths (ASN.1). The validate-without-unpacking design means you can
reject bad data before allocating any memory for it.


========================================================================
4. Canonical Representation
========================================================================

Can you compare two serialized values byte-for-byte and get a meaningful
equality answer?

This property is critical for: consensus systems (all nodes must agree
on state hashes), content-addressed storage (deduplication), digital
signatures (sign the bytes, not a semantic interpretation), and Merkle
trees.

FRACPACK: Yes. Non-canonical forms are valid and decodable (a receiver
can read a message with un-trimmed trailing optionals or non-zero empty
container offsets), but there exists an unambiguous canonical
serialization for every value. The canonical rules are:

  - Integers: fixed-width, little-endian (no varint ambiguity).
  - Booleans: exactly 0x00 or 0x01.
  - Field order: determined by schema position, not encoding order.
  - Trailing trimming: absent trailing optionals MUST be omitted.
  - Empty containers: MUST use offset 0.
  - Offsets: relative to their own position (deterministic given field
    positions).

When canonical form is needed (hashing, signing, consensus), the
validator distinguishes canonical from non-canonical. Two values that
are semantically equal always produce identical canonical bytes.

BORSH: Yes, primary design goal. The name literally stands for "Binary
Object Representation Serializer for Hashing." Achieves canonicality via
fixed-width integers (no varint), sorted map keys (lexicographic order
of serialized keys), fixed field order, and u8 enum discriminants. Borsh
was designed specifically for the same use case as fracpack's canonical
property.

CBOR: Yes, formally specified. RFC 8949 Section 4.2 defines "Core
Deterministic Encoding Requirements": shortest-form integers, shortest-
form floats (1.5 as float16, not float64), sorted map keys (bytewise
lexicographic order of their deterministic encodings), and no indefinite-
length encoding. The dCBOR profile from Blockchain Commons adds even
stricter rules.

ASN.1/DER: Yes, by subset restriction. DER (Distinguished Encoding
Rules) eliminates all ambiguity from BER: shortest-form lengths, sorted
SET OF elements, boolean TRUE = 0xFF. This is why DER is used for X.509
certificates and cryptographic signatures.

PROTOCOL BUFFERS: No. Google's official documentation states: "Protobuf
serialization is not canonical." Maps have undefined order. Unknown
fields may be serialized in any order. Even with Deterministic: true
options in specific language implementations, the output is not
guaranteed canonical across languages.

FLATBUFFERS: No. Encoder flexibility in vtable layout, object ordering,
and field storage order means the same logical data can produce different
byte sequences. Vtable deduplication is an optimization, not a guarantee.

CAP'N PROTO: Defined but not default. The spec defines a canonical form
(single segment, preorder traversal, truncated trailing zeros) that can
be produced by explicit canonicalization. But normal encoders do not
produce it, and the multi-segment message format means the same data has
many valid encodings in normal operation.

MESSAGEPACK: No. The spec says serializers SHOULD use minimal integer
representations, but this is not mandatory. Map key order is undefined.

  Format        Canonical?             Mechanism
  ------------  ---------------------  ------------------------------------------
  Fracpack      Yes (defined)          Fixed-width, trimming rules, offset determinism
  Borsh         Yes (enforced)         Fixed-width, sorted maps, positional fields
  CBOR/dCBOR    Yes (specified)        Shortest-form, sorted maps, no indefinite length
  ASN.1/DER     Yes (subset)           Shortest-form lengths, sorted sets
  Cap'n Proto   Defined, not default   Canonicalization pass available
  Protobuf      No                     Multiple valid encodings by design
  FlatBuffers   No                     Encoder-dependent layout
  MessagePack   No                     SHOULD, not MUST for minimal forms
  Bincode       Mostly                 Deterministic but not formally specified
  Avro          No                     Schema-dependent, not canonicalized
  BSON          No                     Field order preserved but not canonicalized

VERDICT: Fracpack is one of only three formats (alongside borsh and
CBOR/DER) that enforce canonical encoding as a first-class property.
Unlike borsh, fracpack also provides zero-copy views and schema
evolution. Unlike DER, fracpack is simple enough to implement correctly.


========================================================================
5. Speed of Reading: View vs Deserialize
========================================================================

How fast can you access a single field of a large struct?

The fundamental divide: parse-then-access formats (protobuf, Avro,
bincode, borsh, MessagePack, CBOR) require O(n) deserialization before
any field is readable. View/zero-copy formats (fracpack, FlatBuffers,
Cap'n Proto) allow O(1) field access via pointer arithmetic into the
serialized buffer.

For simple structs (all scalar fields, fixedStruct), fracpack's encoding
is byte-identical to a #pragma pack(1) C struct on a little-endian
machine -- just fields concatenated with no padding, no headers, no
metadata. The "zero-copy view" in this case isn't an abstraction; the
serialized buffer IS the memory layout. You can reinterpret_cast the
buffer as a packed C struct.

Fracpack does not impose alignment padding -- but if a developer WANTS
aligned fields for performance (e.g., aligning a f64 to an 8-byte
boundary), they just declare explicit padding fields in their struct.
The padding is visible in the type definition, not hidden in the format
spec. This is the opposite of Cap'n Proto (which forces 64-bit word
alignment on everything) or FlatBuffers (which aligns per-field). With
fracpack, you get dense packing by default and opt into alignment only
where profiling shows it matters.

For extensible structs and variable-size fields, fracpack adds a u16
header and relative offsets to the heap region, but field access within
the fixed region remains a direct load at a compile-time constant
offset. In C++ and Rust, most of this offset math is resolved at
compile time -- the generated code is a direct load at a constant
offset, with no runtime arithmetic. For variable-size fields, one
additional dereference follows the relative offset to the heap.

In C++ and Rust, packed views are often FASTER than accessing native
structs. Native structs scatter data across allocations: a struct
with three std::string/String fields has three separate heap
allocations that thrash the cache. Fracpack's two-region layout
packs all fixed fields contiguously and all variable data
contiguously, giving dramatically better cache locality. Reading
10 fields from a fracpack view may touch 1-2 cache lines; reading
the same fields from a native struct may touch 10+ cache lines
chasing heap pointers.

Both C++ and Rust resolve offset math at compile time. In C++,
template metaprogramming computes field offsets as compile-time
constants. In Rust, Pack::FIXED_SIZE associated consts and const
expressions achieve the same -- the generated view accessor compiles
down to a direct load at a constant offset. The Rust implementation
also uses const generics (MutBase<const FAST: bool>) to resolve
fast-path vs canonical-path branching at compile time via dead code
elimination.

Scalar vecs (vec(u32), vec(f64)) are directly viewable as typed array
slices -- zero copies, zero per-element decoding. This is possible
because fracpack stores elements densely with no per-element overhead.

FLATBUFFERS achieves similar zero-copy performance with one additional
indirection per field access (vtable lookup). For selective access to
large messages, FlatBuffers and fracpack are comparable. FlatBuffers'
vtable indirection costs ~1-2ns per access but enables the field-
optionality that makes evolution possible.

CAP'N PROTO uses the same format for construction and access -- you
build messages in-place in the wire buffer. However, the XOR-default
mechanism (defaults are XOR'd into the data section, so zero-initialized
memory has correct defaults) adds a branch per field read. Fracpack's
wire format is equally a memory format: views read fields directly from
the serialized buffer with no XOR step, no vtable lookup, and no
branch.

PROTOCOL BUFFERS requires full deserialization. Lazy parsing options
exist (proto [lazy=true] for sub-messages) but do not provide per-field
random access. Arena allocation reduces malloc overhead by 40-60% but
does not eliminate the O(n) initial parse.

AVRO is the worst case for selective field access. Because there are no
field tags or offsets in the wire format, reading the 50th field requires
parsing all 49 fields before it. The format is optimized for streaming
full-row scans (Hadoop/Kafka), not random access.

VERDICT: Fracpack provides zero-copy view performance that exceeds
FlatBuffers (no vtable indirection) and Cap'n Proto (no XOR branch).
In C++ and Rust, compile-time offset resolution means view access
compiles down to direct loads at constant offsets -- often faster than
accessing native structs due to superior cache locality. The two-region
layout eliminates the pointer-chasing that dominates native struct
access patterns.


========================================================================
6. Native Data Size (Compactness)
========================================================================

How many bytes does a typical message consume?

Fracpack's design philosophy: pack dense, no alignment, fixed-width
scalars. This is a deliberate middle ground between varint compactness
(protobuf) and alignment-padded zero-copy (Cap'n Proto, FlatBuffers).

Per-field overhead comparison:

  Format       u32 field              string "hello"                   bool     struct overhead
  -----------  ---------------------  -------------------------------  -------  -----------------------
  Fracpack     4 bytes (inline)       4 (off) + 4 (len) + 5 = 13      1 byte   2 bytes (u16 header)
  WIT Cn ABI   4 + 0-3 pad (align)   4 (ptr) + 4 (len) = 8 [5]       1 + pad  0 (positional)
  Protobuf     1 (tag) + 1-5 (var)   1 (tag) + 1 (len) + 5 = 7       2        0 (tag-value pairs)
  FlatBuffers  4 + 2 (vtable slot)   4 (off) + 4 (len) + 5 + pad     1 + pad  vtable (4+ shared)
  Cap'n Proto  4 + 4 pad (8B word)   8 (ptr) + 8 (rounded to word)   1 bit    8 bytes (struct ptr)
  Borsh        4 bytes               4 (len) + 5 = 9                  1 byte   0
  Avro         1-5 (varint)          1 (var len) + 5 = 6              1        0
  BSON         name + 6              name + NUL + 4 + 5 + NUL         name+3   5 (len + NUL term)

  [5] WIT Canonical ABI strings are (pointer, length) pairs stored inline.
      The string data itself lives separately in linear memory. The record
      stores only the 8-byte descriptor; actual bytes are out-of-line.

Key size observations:

Protobuf and Avro win on raw compactness for small integers because
varint encoding compresses common values. A u32 with value 1 takes 1
byte in protobuf vs 4 in fracpack. But varints prevent zero-copy access
-- you cannot jump to field N without scanning varints for fields 1..N-1.

Cap'n Proto is the least compact without packing. The 8-byte word
alignment means a struct with three booleans takes 8 bytes (1 data word)
vs 3 bytes in fracpack. Cap'n Proto's packed encoding (zero-byte run-
length compression) closes the gap but adds a decode step.

FlatBuffers adds per-type vtable overhead plus natural alignment padding
between fields. Vtable deduplication helps when many objects share a
layout, but each unique layout pays the full vtable cost.

Fracpack is ~15-20% smaller than Cap'n Proto canonical ABI for typical
messages. The absence of alignment padding is the primary saving.
Compared to protobuf, fracpack is larger for messages dominated by small
integers, comparable for mixed types, and can be smaller for messages
with many fixed-size fields (no per-field tag bytes).

BSON is the least compact format. Field names are stored as NUL-
terminated strings in every document instance.

VERDICT: Fracpack trades varint compactness (saving 1-3 bytes per small
integer) for zero-copy field access (saving the entire O(n)
deserialization pass). For the WASM IPC use case -- where messages are
read far more often than they traverse a network -- this is the right
trade-off. The 15-20% size reduction vs canonical ABI formats (Cap'n
Proto, FlatBuffers) is a bonus from eliminating alignment padding.


========================================================================
7. Language Support
========================================================================

How many languages can produce and consume the format?

  Format            Implementations   Notes
  ----------------  ----------------  -----------------------------------------
  MessagePack       50+               Widest adoption; trivial to implement
  Protobuf          11+ official      Plus extensive third-party
  CBOR              20+               Strong IoT/WebAuthn ecosystem
  FlatBuffers       14+               C++, C#, C, Go, Java, JS, Kotlin, etc.
  ASN.1/DER         Universal         Every language with TLS has one
  BSON              12+               MongoDB driver ecosystem
  Cap'n Proto       10+               C++ most mature
  Avro              10+               Java most mature
  Borsh             7                 Blockchain-focused
  Fracpack          6                 All maintained in-tree, native idioms
  Bincode           1 (Rust)          Intentionally Rust-only

Fracpack's implementations are notable for being native to each
language's idioms rather than generated from a shared IDL:

  C++         PSIO1_REFLECT macro + template metaprogramming
  Rust        #[derive(Pack, Unpack)] proc macros
  Zig         comptime generics (marshal/view/unmarshal from native structs)
  Go          reflect + struct tags (follows encoding/json conventions)
  TypeScript  Schema-builder functions with full type inference (Infer<T>)
  MoonBit     Trait-based Packable + builder helpers

No external code generator is needed in any language. The schema is the
code.

VERDICT: Fracpack has fewer implementations than the Google-backed
formats, but six languages with native-idiomatic APIs is sufficient for
a WASM ecosystem. The code-first approach (no .proto file, no flatc, no
capnpc) eliminates the build-step friction that plagues protobuf/
FlatBuffers/Cap'n Proto in practice.


========================================================================
8. Developer Friendliness
========================================================================

How much boilerplate and conceptual overhead does a developer face?

Fracpack in each language aims to feel native:

  TypeScript -- schema-builder with full type inference:

    const Person = struct({ name: str, age: u32, bio: optional(str) });
    const data = Person.pack({ name: 'Alice', age: 30, bio: null });
    const view = Person.view(data);   // lazy, zero-copy

  Zig -- comptime generics, native structs:

    const Person = struct { name: []const u8, age: u32, bio: ?[]const u8 };
    const data = try fracpack.marshal(Person, alloc, .{ .name = "Alice", .age = 30, .bio = null });
    const v = try fracpack.view(Person, data);   // v.name is a zero-copy slice

  Go -- reflection + struct tags, encoding/json conventions:

    type Person struct { Name string `fracpack:"name"`; Age uint32 `fracpack:"age"` }
    data, _ := fracpack.Marshal(Person{Name: "Alice", Age: 30})
    var p Person; fracpack.Unmarshal(data, &p)

  Rust -- derive macros:

    #[derive(Pack, Unpack)]
    struct Person { name: String, age: u32, bio: Option<String> }
    let data = person.packed();
    let view = PersonView::new(&data);   // zero-copy

PROTOCOL BUFFERS requires a .proto file, a protoc compiler step,
generated code you cannot modify, and From/Into conversions between your
domain types and the generated types. The generated code quality varies
by language. Build integration (Bazel rules, CMake generators, build.rs
scripts) is a persistent source of friction.

FLATBUFFERS has the most complex developer experience. The builder API
requires bottom-up construction (children before parents), strings and
nested objects must be pre-created, and the finished buffer is read-only
until you opt into the mutable "Object API" (which loses zero-copy
benefits). The cognitive load of understanding vtables, offsets, and the
builder pattern is significant.

CAP'N PROTO sits between protobuf and FlatBuffers in complexity. The
schema language uses unique syntax (@N annotations, using aliases). The
builder works with "orphans" for out-of-order construction. The XOR-
default mechanism is clever but confusing when inspecting raw bytes.

MESSAGEPACK AND CBOR are the simplest: no schema, no code generation,
serialize any dictionary/list structure. The trade-off is no type
safety, no documentation, and no validation.

BORSH AND BINCODE are trivially simple within Rust (#[derive(...)] and
done) but offer nothing outside Rust.

CONSTRUCTION ORDER:

How you build a message is a surprisingly large part of the developer
experience.

  Any order, then pack -- build a native object however you like (set
  fields in any order, use constructors, factory functions, whatever),
  then call pack(). Serialization is a single call at the end.
    Fracpack, Protobuf, Borsh, Bincode, JSON, CBOR, MessagePack, Boost

  Post-order (bottom-up) -- must create children before parents. Inner
  tables and strings are built first (returning Offset<T> handles),
  then the outer table references those handles. You cannot create a
  parent table before its children exist.
    FlatBuffers

  In-place (preorder) -- you allocate a MessageBuilder, then write
  fields directly into the wire-format buffer, root first. The object
  IS the serialized form. Efficient, but construction order is
  constrained and the API is unlike normal object construction.
    Cap'n Proto

  Schema-ordered builder -- fill fields via a generated builder or
  GenericRecord in schema-defined order. Can be any-order for
  specific records, but the API steers you toward schema awareness.
    Avro

FlatBuffers' bottom-up requirement is the single most-cited pain point
in developer surveys. Fracpack, borsh, and protobuf all let you build
objects naturally and serialize at the end.

TYPE OWNERSHIP:

Whose types does the developer use throughout their codebase?

  Your types -- the serializer works directly with your domain types.
  No generated code, no translation layer, no "two representations"
  problem. You define a struct, add a reflection/derive annotation,
  and you're done.
    Fracpack (PSIO1_REFLECT / derive macros / comptime generics)
    Borsh (#[derive(BorshSerialize, BorshDeserialize)])
    Bincode (serde derives)
    Boost.Serialization (serialize() method on your types)

  Generated types -- a code generator produces types from an IDL file.
  You either use the generated types everywhere (coupling your
  codebase to the generator's naming and style choices) or maintain
  From/Into conversions between generated types and your domain types.
    Protobuf (protoc -> generated message classes)
    FlatBuffers (flatc -> generated builder/reader types)
    Cap'n Proto (capnpc -> generated builder/reader types)
    Avro (avro-tools -> generated SpecificRecord classes)
    ASN.1 (asn1c / various -> generated codec types)

  Library types -- the serializer defines its own object model that
  you construct manually (e.g., Document, BsonValue).
    BSON, some CBOR/MessagePack libraries

  Native dynamic -- serialize from native maps/arrays/objects. No
  schema, no types to worry about. Fast to start, hard to maintain.
    JSON, MessagePack (some libraries), CBOR (some libraries)

The "your types" model means fracpack is invisible in your codebase.
There is no serialization layer to learn, debug, or maintain. Your
domain struct IS the message. The "generated types" model means you
either live with the generator's choices (naming, ownership, error
handling) or maintain a conversion layer that doubles the type count.

VERDICT: Fracpack's code-first approach eliminates the "two
representations of every type" problem that plagues schema-first
formats. You define your type once in your language's native syntax,
build objects in any order using normal constructors, and call pack().
No bottom-up gymnastics, no builder patterns, no generated types to
convert from. This is a material developer experience advantage.


========================================================================
9. Other Properties Serialization Libraries Advocate
========================================================================

Streaming / Incremental Parsing
-------------------------------
Protobuf supports streaming via length-delimited message framing. Avro
is designed for streaming with its block-based container format. CBOR
supports indefinite-length encoding for streaming. Fracpack is message-
oriented (each buffer is a complete value) and does not support
incremental parsing. For the WASM IPC use case this is fine -- messages
are complete before dispatch.

Compression Friendliness
------------------------
Cap'n Proto compresses exceptionally well because zero-initialized
defaults produce long runs of zero bytes. Fracpack compresses well
because the two-region layout groups similar data (all fixed fields
together, all heap data together), which improves LZ77 dictionary hits.
Protobuf and Avro are already compact, so compression yields smaller
absolute gains.

Human Readability / Debugging
-----------------------------
Protobuf has a text format and JSON mapping. CBOR has diagnostic
notation. MessagePack and BSON can be trivially dumped as JSON. Fracpack
has hex utilities and JSON conversion (via the TypeScript
implementation's fracToJson/jsonToFrac), plus schema-driven validation
that reports exactly where a buffer is malformed.

RPC Integration
---------------
Cap'n Proto is unique in providing a built-in RPC protocol with promise
pipelining (chaining calls without waiting for intermediate results).
Protobuf has gRPC (separate project). Fracpack integrates with the WASM
component model via WIT generation -- the serialization format is the
method-dispatch format, not a separate concern.

Multi-Format Schema (One Schema, Multiple Encodings)
-----------------------------------------------------
Most serialization libraries are a single format: protobuf encodes one
way, FlatBuffers encodes one way. PSIO's reflection system is a
universal driver that targets multiple wire encodings from the same
type definitions:

  Format     Wire encoding           Zero-copy  Extensible  Best for
  ---------  ----------------------  ---------  ----------  ----------------
  Fracpack   Fixed-width + heap      Yes        Opt-in      Read-heavy, IPC
  Bin        Varint, schema-ordered  No         No          Compact storage
  JSON       Text                    No         Yes         Debug, web APIs

The bin format is closest to Avro's binary encoding: zig-zag varints,
no field tags, fields in schema-defined order, no extensibility. It
serializes slightly faster than fracpack (no offset computation) and
produces smaller output for integer-heavy data (varints vs fixed-width).
The trade-off: it requires full deserialization and has no canonical form.

The JSON path provides fracToJson/jsonToFrac for binary-to-JSON
conversion, plus valueToJson/jsonToValue for working with unpacked
values. All three encodings are driven by the same schema -- define
your types once, serialize to whichever format suits the context.

No other serialization ecosystem offers this. Protobuf has a JSON
mapping, but it's a protocol-level feature of protobuf itself, not
a schema-driven multi-format system.

Schema Reflection / Introspection
---------------------------------
Protobuf has Descriptor and Reflection APIs for runtime schema
inspection. FlatBuffers supports schema-less parsing via a "flex" mode.
Fracpack provides typeToAnyType() (JSON schema description compatible
with the Rust implementation) and generateSchema() for registry-style
schema dumps. The C++ implementation has full compile-time reflection
via PSIO1_REFLECT.


========================================================================
10. Schema: Where It Lives and How It Flows
========================================================================

Every serialization format has a schema -- the question is where it
lives and which direction it flows.

  SCHEMA-FIRST (IDL -> code):
  Write an interface definition file, run a code generator, implement
  against the generated types.

  CODE-FIRST (code -> schema):
  Write your types in your language, derive the schema automatically
  at compile time.

  SCHEMA-LESS:
  No schema at all. Type info embedded in every message (self-describing)
  or left entirely to the application.

  Format       Schema Direction             External Tool Required?
  -----------  ---------------------------  ----------------------------------
  Fracpack     Code -> schema (exportable)  No. Schema derived + exported
  WIT Cn ABI   IDL -> code (or code-first)  Yes (wit-bindgen), or No (psio)
  Protobuf     IDL -> code                  Yes (protoc)
  FlatBuffers  IDL -> code                  Yes (flatc)
  Cap'n Proto  IDL -> code                  Yes (capnpc)
  Avro         IDL -> code (or dynamic)     Yes in Java/C++, No in Python
  Borsh        Code only (not exportable)   No (derive macros)
  Bincode      Code only (not exportable)   No (serde derives)
  Boost.Ser.   Code only (not exportable)   No (serialize() methods)
  MessagePack  Schema-less                  No
  CBOR         Schema-less (CDDL optional)  No
  ASN.1        IDL -> code                  Yes
  BSON         Schema-less                  No

Fracpack is unique in being code-first AND schema-exportable. The schema
is derived automatically from source types, then can be exported in two
forms. The key insight: a schema doesn't need to define the algorithm --
it just needs enough type information that algorithm + schema = encode/
decode.

  - AnyType: typeToAnyType() produces schema descriptions using
    fracpack's own type vocabulary (Object vs Struct, Int, List, Option,
    Variant, Array with length, Custom semantic labels). This is a
    complete schema: every type distinction fracpack makes is
    represented. AnyType + fracpack encoding rules = fully sufficient
    to encode/decode any message.

    AnyType is available in two forms:
      - JSON: human-readable, used for debugging and cross-language
        schema exchange at rest.
      - Binary: AnyType itself is packed via fracpack (the schema
        format is self-hosting). Compact, machine-readable, and
        embeddable in WASM custom sections.

    The TypeScript implementation uses AnyType for dynamic parsing at
    runtime. The Rust implementation derives AnyType via #[derive(ToSchema)].

  - WIT text/binary: generateWit() and generate_wit_binary<T>() produce
    standard WebAssembly Interface Types consumed by wit-bindgen and
    wasm-tools. WIT preserves field names, types, order, option<T>
    wrapping, variant cases, and tuple structure -- enough to drive the
    fracpack algorithm for the COMMON CASE (all extensible structs, no
    fixed-length arrays). Two edge gaps exist:

      1. Extensible vs fixed struct: WIT has only "record" -- no way
         to mark a struct as fixedStruct (no u16 header). If you default
         to extensible (which most fracpack types are), this works.
      2. Fixed-length arrays: WIT has only list<T>. Fracpack's
         array(T, N) serializes without a length prefix; the length is
         part of the type. This is lost in WIT.

    WIT supports doc comments (/// and /** */) which could carry
    structured metadata (@fracpack fixed, @fracpack array 3), but
    comments are stripped when WIT text is compiled into the binary
    component-type custom section -- the only standard WASM section
    for WIT. There is no standard text-based WIT section.

    However, the component-type binary IS lossless for type structure
    (field names, order, option wrapping, variants all round-trip).
    For the common case (extensible structs, variable-length lists),
    the standard binary section + fracpack algorithm = sufficient.

    For the two edge cases (fixedStruct, array lengths), a module
    embeds a second custom section: component-wit:<name> containing
    annotated WIT text with @fracpack doc comments. The standard
    component-type section stays for Canonical ABI consumers; our
    tools read component-wit for the full fracpack schema. Both
    coexist -- standard tooling ignores sections it doesn't
    recognize.

Both exports enable cross-language dynamic parsing without shared source.
A service written in Rust exports its schemas, and a tool in TypeScript
dynamically packs/unpacks those messages at runtime -- no code generation,
no build step. The schema flows FROM code rather than TO code.

Borsh and bincode are code-first but cannot export their schemas -- the
only way to know the format is to read the Rust source. Schema-first
formats (protobuf, FlatBuffers, Cap'n Proto) can export schemas
trivially (the IDL IS the export), but at the cost of the "two
representations" problem: your domain types and the generated types are
different, and you maintain conversion code between them.

VERDICT: Fracpack eliminates the code generator while preserving the
schema as a distributable artifact. No other format achieves both.


========================================================================
11. Serialization / Deserialization Speed
========================================================================

Raw encode/decode throughput, independent of the view/zero-copy
dimension.

Tier 1 -- Fastest (memcpy-class):
  - Fracpack fixedStruct (all scalars): Encoding is identical to
    #pragma pack(1) -- a single memcpy of contiguous fields. No
    headers, no offsets, no passes. Deserialization is a pointer cast.
  - WIT Canonical ABI: For all-scalar records, the layout IS the C
    struct layout with natural alignment. "Serialization" is the struct
    itself. "Deserialization" is a pointer cast into linear memory.
    Zero overhead for scalar-only types. For records with strings or
    lists, the record stores (pointer, length) pairs and the data lives
    separately -- lowering/lifting requires copying string data.
  - Cap'n Proto: Serialization is essentially writing into a pre-
    allocated buffer. Deserialization is a pointer cast. The wire format
    IS the working format.
  - Bincode: Serde's zero-overhead trait dispatch plus simple sequential
    encoding. For fixed-size structs, this approaches memcpy speed.
  - Borsh: Fixed-width integers eliminate varint branch logic. Slightly
    faster than bincode for integer-heavy payloads in some benchmarks.

Tier 2 -- Fast (one-pass encode/decode):
  - Fracpack extensible structs: Two-pass encoding (fixed region, then
    heap with offset patching). Single-pass decoding. No varint
    overhead. The two-pass encode is ~10-20% slower than single-pass
    formats, but the view path eliminates the deserialization cost
    entirely for read-heavy workloads.
  - FlatBuffers: Builder construction is fast but requires bottom-up
    ordering. Deserialization is zero-copy (instant).
  - MessagePack: Simple sequential byte processing. No schema overhead.
    Typically 2-5x faster than JSON.

Tier 3 -- Moderate:
  - Protobuf: Varint encoding adds branching for every integer. Tag-
    value parsing requires per-field dispatch. Arena allocation is the
    main optimization lever.
  - CBOR: Similar to MessagePack with slightly more complex initial-byte
    decoding (3+5 bit split).
  - Avro: Fast for sequential bulk reads (its sweet spot) but cannot
    skip fields.

Tier 4 -- Slower:
  - ASN.1/DER: TLV parsing with variable-length tags and variable-
    length-of-lengths. Complex type dispatch.
  - BSON: String field-name comparison for every field lookup. Length-
    prefix enables skipping but not random access.

Fracpack's position: For workloads that are read-heavy (the common case
in server applications), fracpack's view path eliminates deserialization
entirely, making it competitive with Cap'n Proto. For write-heavy
workloads, the two-pass encoding is slightly slower than single-pass
formats but faster than protobuf's varint encoding. The combination of
fast-enough serialization + zero-cost deserialization (views) is the
optimal point for request/response patterns.


========================================================================
12. In-Place Modification
========================================================================

Can you mutate a field in a serialized buffer without re-serializing the
entire message?

  Format       Scalar Mutation              Var-Size Mutation        Safety
  -----------  ---------------------------  ----------------------  -------------------------
  Fracpack     Yes (known offsets)          Requires repack         Safe (relative offsets)
  WIT Cn ABI   Yes (direct memory write)    No (ptr+len immutable)  Safe (linear memory)
  FlatBuffers  Yes (mutate_* methods)       No (cannot resize)      UNSAFE on untrusted bufs
  Cap'n Proto  Yes (builder side)           Requires realloc        Safe (builder tracks)
  Protobuf     No                           No                      N/A
  MessagePack  No                           No                      N/A
  CBOR         No                           No                      N/A
  Avro         No                           No                      N/A
  Bincode      No                           No                      N/A
  Borsh        No                           No                      N/A
  ASN.1/DER    No (lengths cascade)         No                      N/A
  BSON         Limited (MongoDB padding)    No                      N/A

Fracpack's TypeScript implementation provides setField():

    // Fixed-size field: patched in-place, O(field_size)
    const updated = Person.setField(data, 'age', 31);

    // Variable-size field: full repack, O(total_size)
    const renamed = Person.setField(data, 'name', 'Bob');

For fixed-size fields, this is a direct write at a computed offset -- no
reallocation, no copying. The relative-offset design means mutating a
fixed field never invalidates other offsets (unlike Cap'n Proto where
absolute offsets could be affected by segment reorganization).

VERDICT: Fracpack supports safe in-place mutation of fixed-size fields,
which covers the most common mutation patterns (counters, timestamps,
status flags). FlatBuffers also supports this but is unsafe on untrusted
buffers due to possible structural aliasing. Most formats do not support
in-place mutation at all.


========================================================================
What Fracpack Is Missing
========================================================================


1. Graph Data Structures (DAGs and Cycles)
------------------------------------------

Fracpack serializes trees only. If the same object is referenced from
two places, it gets serialized twice (duplication). Cyclic references
cannot be represented at all.

This matters because in-memory data structures routinely form graphs:

  - A shared sub-object referenced by multiple parents (DAG). Serializing
    it twice wastes space and loses the identity relationship -- after
    deserialization, mutation of one copy no longer affects the other.

  - Cyclic references (parent <-> child back-pointers, doubly-linked
    lists, graphs with cycles). These are common in practice and
    impossible to represent in a tree format without application-level
    ID indirection.

WHO SUPPORTS GRAPHS:

  BOOST.SERIALIZATION (C++) is the gold standard. It tracks object
  identity via an address-to-ID map during serialization. When a pointer
  is encountered a second time, it emits a back-reference (sequential
  object ID) instead of re-serializing. On deserialization, the reverse
  map reconstructs the original pointer topology -- shared objects are
  shared, cycles are cycles. The BOOST_CLASS_TRACKING macro controls
  per-type behavior: track_never (no overhead), track_selectively
  (default -- tracks only when serialized through pointers), or
  track_always. Supports text, binary, and XML archive formats. Has
  worked reliably for two decades. C++ only.

  FLATBUFFERS partially supports DAGs at the wire format level. Multiple
  offset fields can point to the same serialized object -- the format
  explicitly documents this as valid. CreateSharedString() deduplicates
  identical strings via an internal hash map. The verifier deliberately
  does NOT reject overlapping structures, noting "Sometimes this is
  indeed valid, such as a DAG." However, cycles are structurally
  impossible (offsets are unsigned and always point forward). The builder
  API does not make arbitrary DAG construction easy -- you must manually
  reuse Offset<T> values. DAGs are a zero-copy-compatible optimization,
  not a general graph mechanism.

  CBOR (RFC 8949) supports shared references and cycles via registered
  tags 28 (shareable) and 29 (sharedref). Tag 28 wraps the first
  occurrence; tag 29 references it by sequential index. Cycles are
  possible because "it must be possible to refer to a value before it
  is completely decoded." These tags are in the IANA registry but not
  part of the core spec -- support varies across implementations.

  APACHE FURY provides cross-language graph serialization (Java, Python,
  Go, Rust, C++, JavaScript). When withRefTracking(true) is enabled, it
  maintains a table of previously-written objects and emits reference IDs
  for duplicates. Cycles are fully supported. Extends protobuf/
  FlatBuffers IDL syntax with (fury).ref = true attributes, though the
  wire format is Fury's own binary protocol.

  PYTHON PICKLE uses a memo table (dict of index -> object). PUT/MEMOIZE
  opcodes store objects; GET opcodes retrieve them by index. Objects are
  memoized before their contents are populated, enabling cycles.
  Notoriously insecure for untrusted data (arbitrary code execution via
  REDUCE opcode, plus amplification via memo).

  JAVA SERIALIZATION uses a handle table (sequential IDs starting at
  0x7E0000). TC_REFERENCE (0x71) + 4-byte handle provides back-
  references. Full cycle support. One of the most exploited attack
  surfaces in computing history.

  KRYO (Java) uses an IdentityObjectIntMap for reference tracking,
  controlled by setReferences(true). Full DAG and cycle support with
  per-class opt-out. Varint reference IDs.

  CAP'N PROTO explicitly forbids DAGs: "no more than one pointer can
  point at each object -- objects and pointers form a tree, not a graph."
  A hand-crafted message with duplicate pointers violates the spec.

  PROTOBUF, BORSH, BINCODE, AVRO, MESSAGEPACK have no graph support.

THE TRADE-OFFS:

  BENEFITS:
  - Faithful representation of in-memory pointer topology.
  - Deduplication: shared sub-objects serialized once, not N times.
  - Round-trip fidelity: deserialize(serialize(obj)) preserves pointer
    identity, not just value equality.

  COSTS:
  - Cycles create infinite-loop risk in any recursive-descent traversal
    (pack, unpack, validate, hash, compare). The serializer must track
    visited nodes, adding O(n) state.
  - Amplification attacks: a small back-reference can point to a large
    sub-graph, allowing a tiny payload to expand to enormous memory.
    This is the "billion laughs" class of attack (well-documented in
    YAML, Pickle, Java Serialization).
  - Breaks canonical encoding: a DAG can be linearized in multiple valid
    orders (any topological sort), so the same logical graph can produce
    different byte sequences.
  - Complicates validation: the verifier must build a visited-set to
    detect cycles, turning simple structural checks into stateful
    traversal.

  Note: DAGs do NOT break zero-copy views. Multiple offsets pointing to
  the same buffer location are perfectly viewable -- the data is still
  there, you're just reading it from two paths. FlatBuffers proves this:
  it supports both DAGs and zero-copy views. Cycles, however, do break
  view traversal (infinite descent).

The workaround for tree-only formats is application-level: assign IDs
to nodes, serialize a flat list of (id, node_data, edge_list) tuples,
and reconstruct the graph in application code. This is how relational
databases model graphs (foreign keys), and it gives the application
explicit control over cycle handling and traversal limits.

Fracpack's position: graph support is deliberately omitted. All relative
offsets point strictly forward in the buffer -- never backward. This
forward-only invariant structurally prevents cycles and guarantees
bounded traversal without a visited-set or traversal limiter. It also
preserves canonical encoding (the higher-priority property for consensus
use cases), since DAG deduplication introduces multiple valid
linearization orders for the same logical value.


2. Not Self-Describing (per-message)
-------------------------------------

Fracpack does not embed type information in every message. The binary
stream contains no type tags, field names, or structural markers -- it
is pure data in schema-determined positions.

This is a deliberate choice, not a missing feature. Fracpack HAS a full
schema -- it is derived automatically from source types at compile time.
That schema can be exported as:

  - WIT text (WebAssembly Interface Types) via generateWit()
  - WIT binary (Component Model standard) via generate_wit_binary<T>()
  - AnyType JSON (Rust-compatible registry) via typeToAnyType()

These exported schemas enable dynamic parsing by tools that don't share
the original source code. A schema registry can distribute them. The
difference from protobuf is direction: protobuf starts with an IDL and
generates code; fracpack starts with code and generates the IDL. The
schema exists in both cases.

The same schema also drives multiple wire encodings (fracpack, bin,
JSON) from the same type definitions -- see §9 "Multi-Format Schema."

Formats that embed type info in EVERY MESSAGE: MessagePack, CBOR, BSON,
JSON. Each value carries its own type tag. You can parse the data
without a schema, at the cost of per-message overhead.

Formats that keep schema SEPARATE from data: Fracpack, protobuf, Avro,
FlatBuffers, Cap'n Proto, borsh, bincode. The schema is available
out-of-band (in the code, in .proto files, in a registry). This is
more compact and enables compile-time offset computation.

For debugging, fracpack provides schema-driven JSON conversion
(fracToJson) and hex utilities. The validation function produces
structured error messages that identify exactly where a buffer is
malformed, including the field path.


3. No Native Map/Dictionary Type
---------------------------------

Fracpack encodes maps as vec(struct({ key: K, value: V })) -- a vector
of key-value pair structs. This is semantically correct but means:

  - Key lookup is O(n) sequential scan, not O(1) hash lookup.
  - No uniqueness constraint on keys (the same key can appear twice).
  - No sorted-key guarantee (depends on the application).

Protobuf has native map<K, V> syntax (though it is sugar over repeated
key-value messages with similar limitations). BSON is built around
string-keyed documents. Cap'n Proto does not support maps natively.

For fracpack's use case (IPC message passing), maps are uncommon in API
signatures. When they appear, the O(n) lookup is acceptable for typical
map sizes in API messages (< 100 entries).


4. No Union/Variant Default Case
---------------------------------

Fracpack variants use a u8 tag (0-127). If a reader encounters an
unknown tag (from a newer schema), it can skip the variant's data (the
u32 content_size enables this), but it cannot provide a meaningful
default value. The data is structurally valid but semantically opaque.

Protobuf's oneof handles this naturally since each case is a regular
field with a field number -- unknown field numbers are preserved as
unknown fields.


5. No Streaming or Chunked Encoding
------------------------------------

Fracpack is message-oriented: each buffer is a complete, self-contained
value. There is no framing protocol, no chunked transfer encoding, and
no incremental parsing.

CBOR supports indefinite-length arrays and strings for streaming. Avro's
container format supports block-based streaming with per-block
compression. Protobuf can be framed with length-delimited wrappers for
streaming.

For fracpack's WASM IPC use case, messages are always complete before
dispatch, so streaming is not needed. For network transfer or file
storage, an application-level framing layer would be required.


6. Maximum Message Size
------------------------

Fracpack uses u32 offsets, limiting the maximum distance between an
offset field and its target to ~4GB. In practice, the u16 header on
extensible structs limits the fixed region to 65,535 bytes (though the
heap has no structural limit within the u32 offset space).

Cap'n Proto uses 30-bit word offsets (2GB per segment) but supports
multi-segment messages. FlatBuffers uses u32 offsets (4GB). Protobuf has
no structural size limit (varint lengths can be arbitrarily large,
though implementations typically cap at 2GB).

For IPC messages, 4GB is more than sufficient. For bulk data storage,
fracpack would need an application-level chunking strategy.


========================================================================
Fracpack's Unique Position
========================================================================

Fracpack is the only format that simultaneously provides:

  1. Unambiguous canonical encoding -- every value has exactly one
     canonical binary form, while non-canonical forms remain decodable.

  2. Zero-copy view access -- read fields without deserialization.

  3. Forward compatibility -- old code safely reads new data.

  4. Backward compatibility -- new code safely reads old data.

  5. Code-first exportable schema -- no external IDL file or code
     generator, yet the schema can be exported as WIT or JSON for
     dynamic parsing by other tools.

  6. Multi-format schema -- the same type definitions and schema drive
     fracpack (zero-copy), bin (compact varint, Avro-like), and JSON
     from a single set of type definitions. No other serialization
     ecosystem offers this.

  7. Dense packing -- no alignment padding, 15-20% smaller than
     canonical ABI formats.

  8. Safe in-place mutation -- fixed-size fields can be patched without
     structural risk.

No other format achieves all eight:

  WIT Cn ABI   has #1, #2, #8 but not #3, #4, #5 (partial), #6, or #7
  Cap'n Proto  has #2, #3, #4, #8 but not #1, #5, #6, or #7
  FlatBuffers  has #2, #3, #4 but not #1, #5, #6; #8 is unsafe
  Borsh        has #1, #7 but not #2, #3, #4, #5, or #6
  Protobuf     has #3, #4, #7 but not #1, #2, #5, or #6
  CBOR         has #1 but not #2, #3 (partial), #5, #6, or #8
  JSON         has #3 but not #1, #2, #5, #6, #7, or #8

This combination is not accidental -- it reflects the specific
requirements of WASM inter-module communication where messages are:
created once, read many times (favoring views over fast serialization),
compared by hash (requiring canonicality), evolved over time (requiring
both forward and backward compatibility), and processed across language
boundaries (requiring code-first ergonomics).
