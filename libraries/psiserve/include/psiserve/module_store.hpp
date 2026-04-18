#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Module store, linker, composition lifecycle for psiserve.
//
// Component-first design. Every public noun names something a developer
// already thinks in terms of — world, interface, module, composition —
// never a `*_tag` or a psizam backend identifier. Tags exist; they are
// an implementation detail consumed by inference.
//
// ── Main objects ─────────────────────────────────────────────────────
//
//   ModuleStore     — content-addressed cache of compiled WASM modules.
//                     Key = 32-byte hash of the bytes. The backend
//                     that compiled a module (interpreter / jit / jit2
//                     / …) is a *property* of the compiled form, not
//                     of its identity; it may be recompiled later and
//                     atomically swapped without breaking linkage.
//
//   NameRegistry    — optional overlay of human names → hashes. Pin a
//                     version or hot-swap a dev build by rebinding.
//
//   MemoryPool      — fixed-size arena pool; each Instance leases one
//                     for its lifetime, arena is zero-initialized at
//                     acquire (determinism — no state between runs).
//
//   Linker<World>   — builds one Composition against a specific world.
//                     `provide(impl)` and `add(module)` both register
//                     something that exports an interface tag; both
//                     go into the same provider registry. `instantiate`
//                     resolves the graph, validates it, and produces
//                     a live Composition.
//
//   Composition     — the resolved graph of instances for one world.
//                     Owns every Instance it contains and their memory
//                     leases. Destroy = tear down in reverse-dependency
//                     order.
//
//   Instance        — one live module. Produced internally by the
//                     linker; external code typically reaches a module
//                     via `composition.get<Interface>()` rather than
//                     naming the instance directly.
//
// ── Wiring rule ──────────────────────────────────────────────────────
//
// Every WASM module declares imports and exports. The linker resolves
// each import slot to exactly one provider, where a provider is any
// of:
//
//   (a) a host implementation type registered with PSIO_HOST_MODULE — the
//       impl's tag is found by `psio::impl_of<T>::type`;
//   (b) another module in the same composition, whose PSIO-reflected
//       export interfaces declare their tags;
//   (c) an already-instantiated Instance carrying the same exports as
//       (b).
//
// Duplicate providers for one slot, unresolved imports, and cycles in
// the module graph are runtime errors at `instantiate()`, with the
// offending tag / module named in the exception message.
//
// Compile-time enforcement of "world's outer imports all have
// `provide<>` calls" is feasible via the world's known import list and
// is implemented where the cost-to-ergonomics ratio is right (outer
// imports only; inner-module wiring is unavoidably runtime).
//
// ── Hiding *_tag names ───────────────────────────────────────────────
//
// `Linker<World>` accepts the world type, not its tag. Users say
// `Linker<node>` after `PSIO_WORLD(node, …)`. The macro is responsible
// for making `node` resolve to a public type (today: a thin wrapper
// that aliases to `::psio::detail::node_world_tag`). No `_tag` name
// appears in any public API.
//
// ── Thread safety ────────────────────────────────────────────────────
//
//   ModuleStore     — thread-safe (shared_mutex; reads parallel;
//                     background `upgradeBackend` serialized per hash).
//   NameRegistry    — thread-safe (shared_mutex).
//   PreparedModule  — identity immutable; the backend slot is
//                     atomic<shared_ptr<const BackendSlot>>. Readers
//                     take a snapshot via `currentBackend()`; Instance
//                     does this at construction. Upgrades never
//                     disturb in-flight instances.
//   MemoryPool      — thread-safe (internal synchronization).
//   Linker          — NOT thread-safe, one per composition.
//   Composition     — NOT thread-safe, pinned to instantiating thread.
//   Instance        — NOT thread-safe.
//
// ─────────────────────────────────────────────────────────────────────────────

#include <psizam/backend.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <variant>
#include <vector>

namespace psiserve
{

   // ── Identity ──────────────────────────────────────────────────────────

   /// 32-byte content hash of a WASM module.
   struct ModuleHash
   {
      std::array<std::uint8_t, 32> bytes{};

      friend bool operator==(const ModuleHash&, const ModuleHash&) = default;
   };

   struct ModuleHashHasher
   {
      std::size_t operator()(const ModuleHash&) const noexcept;
   };

   /// Canonical hash of WASM bytes. Backend-independent.
   ModuleHash hashBytes(std::span<const std::byte> wasm);

   /// Which psizam backend currently compiles a cache entry. Swappable
   /// property, not part of identity: a module compiled quickly with
   /// the interpreter can be asynchronously recompiled under jit2 and
   /// atomically installed without breaking linkage — the structural
   /// summary is derived from the WASM bytes, not the backend.
   enum class BackendKind : std::uint8_t
   {
      Interpreter,
      Jit,
      Jit2,
      JitProfile,
      NullBackend,
   };

   // ── Structural summary (for link-time wiring) ─────────────────────────

   enum class ImportKind : std::uint8_t { Function, Table, Memory, Global };
   enum class ExportKind : std::uint8_t { Function, Table, Memory, Global };

   /// One entry from the WASM import section. `module` names the
   /// interface the function belongs to (component-ABI namespace
   /// convention); `name` names the specific import within it.
   struct ImportDesc
   {
      std::string   module;
      std::string   name;
      ImportKind    kind;
      std::uint32_t typeIdx;
   };

   /// One entry from the WASM export section.
   struct ExportDesc
   {
      std::string   name;
      ExportKind    kind;
      std::uint32_t index;
   };

   /// Tags that this module *exports*. Populated by the compile step
   /// from the module's reflected metadata (or, for raw core-WASM
   /// modules without WIT metadata, left empty — those can still be
   /// added to compositions but only participate as providers when
   /// the user wires them manually).
   struct ModuleSummary
   {
      std::vector<ImportDesc> imports;
      std::vector<ExportDesc> exports;
      std::vector<std::type_index> exportedInterfaces;  ///< tag type_indices
   };

   // ── PreparedModule ────────────────────────────────────────────────────

   /// Content-addressed, pre-compiled module.
   ///
   /// Identity (hash, bytes, summary) is immutable. The compiled
   /// backend sits behind an atomic shared_ptr and may be swapped by
   /// the ModuleStore. Readers (the Linker, at instantiation time)
   /// grab a snapshot via `currentBackend()` and hold it for the
   /// lifetime of whatever they derive from it; an upgrade swap never
   /// invalidates a snapshot.
   ///
   /// No HostFunctions template parameter. The compiled backend is a
   /// variant over the psizam specializations the build knows about,
   /// each parameterized on an internally-generated host-functions
   /// struct that the Linker synthesizes per-composition from the
   /// providers it's been given. This is what lets the same compiled
   /// module plug into different compositions with different host
   /// impls without recompilation.
   class PreparedModule
   {
     public:
      /// The per-backend compiled artifact. Implementation detail —
      /// type-erased behind `void*` + a tag so the store doesn't need
      /// to know every backend's template args up front. The Linker
      /// unwraps at instantiation time.
      struct BackendSlot
      {
         BackendKind    kind;
         std::shared_ptr<void> handle;  ///< backend-specific compiled blob
      };

      const ModuleHash&    hash()    const noexcept { return _hash; }
      const ModuleSummary& summary() const noexcept { return _summary; }

      /// WASM byte size — used by the store for cache budgeting.
      std::size_t sourceSize() const noexcept { return _bytes.size(); }

      /// Underlying WASM bytes, retained so the store can recompile
      /// under a different backend without the caller re-supplying.
      std::span<const std::byte> bytes() const noexcept { return _bytes; }

      /// Snapshot of the currently installed backend. Safe to hold for
      /// the duration of any work derived from it.
      std::shared_ptr<const BackendSlot> currentBackend() const noexcept
      {
         return std::atomic_load_explicit(&_backend, std::memory_order_acquire);
      }

     private:
      friend class ModuleStore;
      friend struct detail_instantiate_access;

      PreparedModule(ModuleHash                         hash,
                     std::vector<std::byte>             bytes,
                     std::shared_ptr<const BackendSlot> backend,
                     ModuleSummary                      summary);

      void installBackend(std::shared_ptr<const BackendSlot> next) noexcept
      {
         std::atomic_store_explicit(&_backend, std::move(next),
                                    std::memory_order_release);
      }

      ModuleHash                                 _hash;
      std::vector<std::byte>                     _bytes;
      mutable std::shared_ptr<const BackendSlot> _backend;
      ModuleSummary                              _summary;
   };

   using ModuleHandle = std::shared_ptr<const PreparedModule>;

   // ── ModuleStore ───────────────────────────────────────────────────────

   /// Thread-safe registry of prepared modules, keyed by content hash.
   ///
   /// Process-wide default BackendKind supplied at construction. An
   /// entry's backend can be upgraded later via `upgradeBackend`
   /// without perturbing in-flight instances.
   class ModuleStore
   {
     public:
      explicit ModuleStore(BackendKind defaultKind = BackendKind::Jit);

      BackendKind defaultBackend() const noexcept { return _defaultKind; }

      /// Null if not cached. Does not compile.
      ModuleHandle lookup(const ModuleHash& hash) const;

      /// Lookup or compile. `wasmBytes` must hash to `hash` (debug-
      /// asserted; release trusts the caller). Safe to call
      /// concurrently with the same hash — only one compile happens
      /// per hash.
      ModuleHandle getOrCompile(const ModuleHash&          hash,
                                std::span<const std::byte> wasmBytes);
      ModuleHandle getOrCompile(const ModuleHash&          hash,
                                std::span<const std::byte> wasmBytes,
                                BackendKind                kind);

      /// Recompile an already-cached module under `targetKind` and
      /// atomically install the new backend. Returns false if the
      /// module isn't cached or already matches. In-flight instances
      /// keep running on their captured backend; new instantiations
      /// pick up the upgrade.
      bool upgradeBackend(const ModuleHash& hash, BackendKind targetKind);

      /// Drop entries whose only outstanding reference is the store's,
      /// oldest-first, until total bytes ≤ maxBytes.
      void evictTo(std::size_t maxBytes);

      std::size_t size()  const;
      std::size_t bytes() const;  ///< sum of sourceSize() across cached

     private:
      struct Entry
      {
         ModuleHandle                          module;
         std::chrono::steady_clock::time_point lastUsed;
      };

      BackendKind                                              _defaultKind;
      mutable std::shared_mutex                                _mtx;
      std::unordered_map<ModuleHash, Entry, ModuleHashHasher>  _cache;
      std::size_t                                              _totalBytes{0};
   };

   // ── NameRegistry ──────────────────────────────────────────────────────

   /// Optional overlay of names → hashes. Separate from the store so a
   /// pure-hash deployment can run without it.
   class NameRegistry
   {
     public:
      void                      bind(std::string name, ModuleHash hash);
      std::optional<ModuleHash> resolve(std::string_view name) const;
      void                      unbind(std::string_view name);

      std::vector<std::pair<std::string, ModuleHash>> list() const;

     private:
      mutable std::shared_mutex                   _mtx;
      std::unordered_map<std::string, ModuleHash> _names;
   };

   // ── MemoryPool ────────────────────────────────────────────────────────

   /// Fixed-size pool of pre-allocated linear-memory arenas.
   ///
   /// Each Instance leases one for its lifetime. Acquire zeroes the
   /// arena (determinism — no state between runs); destruction returns
   /// it to the pool.
   class MemoryPool
   {
     public:
      struct Config
      {
         std::size_t slotCount{0};
         std::size_t bytesPerSlot{0};
      };

      explicit MemoryPool(Config cfg);
      MemoryPool(const MemoryPool&)            = delete;
      MemoryPool& operator=(const MemoryPool&) = delete;
      ~MemoryPool();

      class Lease
      {
        public:
         char*       data() noexcept;
         std::size_t capacity() const noexcept;

         Lease(Lease&&) noexcept;
         Lease& operator=(Lease&&) noexcept;
         Lease(const Lease&)            = delete;
         Lease& operator=(const Lease&) = delete;
         ~Lease();

        private:
         friend class MemoryPool;
         Lease(MemoryPool* pool, std::size_t slot);
         MemoryPool* _pool{nullptr};
         std::size_t _slot{0};
      };

      Lease                acquire();         ///< blocks until a slot is free
      std::optional<Lease> tryAcquire();      ///< non-blocking

      std::size_t slotCount()    const noexcept;
      std::size_t bytesPerSlot() const noexcept;

     private:
      struct Impl;
      std::unique_ptr<Impl> _impl;
   };

   // ── Provider tag-keyed registry (linker internal) ─────────────────────

   /// Erased handle to something that fills one or more interface
   /// slots. The linker stores these keyed by exported interface tag
   /// (as `std::type_index`). Two flavors:
   ///   • Host — wraps a C++ impl object + its reflected method table.
   ///   • Instance — wraps a live Instance from the same composition.
   struct Provider
   {
      enum class Kind { Host, Instance };
      Kind        kind;
      const void* payload;  ///< impl* or Instance*
      std::type_index implType{typeid(void)};  ///< for Host: decltype(impl)
   };

   // ── Instance ──────────────────────────────────────────────────────────

   /// A live, thread-pinned module instance.
   ///
   /// Captures the PreparedModule's backend snapshot at instantiation,
   /// so later backend upgrades in the store don't affect it.
   class Instance
   {
     public:
      using BackendSlot = PreparedModule::BackendSlot;

      /// Exported-symbol lookup, for another module in the same
      /// composition that imports from this one.
      std::optional<ExportDesc> findExport(std::string_view name) const;

      const PreparedModule& module() const noexcept { return *_module; }

      /// The BackendKind frozen at instantiation — unaffected by later
      /// ModuleStore::upgradeBackend.
      BackendKind backendKind() const noexcept { return _backend->kind; }

      Instance(Instance&&) noexcept;
      Instance& operator=(Instance&&) noexcept;
      Instance(const Instance&)            = delete;
      Instance& operator=(const Instance&) = delete;
      ~Instance();

     private:
      template <typename W> friend class Linker;
      friend class Composition;
      Instance(ModuleHandle                       module,
               std::shared_ptr<const BackendSlot> backend,
               MemoryPool::Lease                  memory);

      ModuleHandle                       _module;
      std::shared_ptr<const BackendSlot> _backend;
      MemoryPool::Lease                  _memory;
      struct Impl;
      std::unique_ptr<Impl> _impl;
   };

   // ── Composition ───────────────────────────────────────────────────────

   /// The resolved, running graph of instances for one world.
   ///
   /// Owns every Instance it contains and the memory leases they hold.
   /// Destruction tears them down in reverse topological order so no
   /// instance outlives anything it imports from.
   ///
   /// External code reaches into a composition by interface, not by
   /// instance: `composition.get<http>()` returns a typed view onto
   /// whichever instance exports `http` in this world.
   class Composition
   {
     public:
      /// Typed view onto an exported interface. Implementation is
      /// specialized per-interface by the codegen layer (Phase D); the
      /// primary template is incomplete on purpose so a missing
      /// specialization is a clear compile error.
      template <typename Interface>
      class View;

      template <typename Interface>
      View<Interface> get();

      /// Count of instances in the graph (introspection / debugging).
      std::size_t instanceCount() const noexcept { return _instances.size(); }

      Composition()                              = default;
      Composition(Composition&&) noexcept        = default;
      Composition& operator=(Composition&&) noexcept = default;
      Composition(const Composition&)            = delete;
      Composition& operator=(const Composition&) = delete;
      ~Composition()                             = default;

     private:
      template <typename W> friend class Linker;

      /// Instances in dependency order (leaves first). Destruction
      /// runs the vector in reverse.
      std::vector<std::unique_ptr<Instance>> _instances;

      /// Exported interface tag → Instance*. Populated by the linker.
      std::unordered_map<std::type_index, Instance*> _byInterface;
   };

   // ── Linker ────────────────────────────────────────────────────────────

   /// Builds one Composition against world `World`.
   ///
   /// `World` is the public world type emitted by PSIO_WORLD (e.g.
   /// `PSIO_WORLD(node, …)` makes `node` usable as `Linker<node>`).
   /// The linker reads `World`'s import list at compile time for the
   /// outer-import check, and its declared exports for the "did the
   /// composition cover everything the world promises?" check.
   ///
   /// Short-lived and single-threaded. Destroy the linker after
   /// `instantiate()` returns; the Composition owns everything it
   /// needs.
   template <typename World>
   class Linker
   {
     public:
      Linker() = default;

      /// Register a host implementation. Slot is resolved via
      /// `psio::impl_of<T>::type`, so the impl class must have been
      /// declared with PSIO_HOST_MODULE. Fails to compile if it wasn't.
      template <typename Impl>
      Linker& provide(Impl& impl);

      /// Explicit-slot form — use when one impl type could satisfy
      /// multiple slots, or when inference is ambiguous.
      template <typename Interface, typename Impl>
      Linker& provide(Impl& impl);

      /// Register a module to be instantiated as part of this
      /// composition. The module's exported interfaces (from its
      /// ModuleSummary) automatically become providers for other
      /// modules in the composition that import them.
      Linker& add(ModuleHandle module);

      /// Register an already-running Instance (e.g. from a sibling
      /// composition) as a provider. The instance must outlive this
      /// linker; the produced Composition will NOT take ownership.
      Linker& provide(Instance& instance);

      /// Resolve the graph and instantiate all modules. Throws with a
      /// clear message on: unresolved import, duplicate provider,
      /// cycle, or world-level unmet export.
      Composition instantiate(MemoryPool& memory);

     private:
      struct PendingModule
      {
         ModuleHandle handle;
      };

      std::vector<PendingModule> _pending;

      /// Tag type_index → Provider. Populated by `provide` and `add`;
      /// consumed at `instantiate`.
      std::unordered_map<std::type_index, Provider> _providers;

      /// Instances provided externally — referenced, not owned.
      std::vector<Instance*> _externalInstances;
   };

}  // namespace psiserve
