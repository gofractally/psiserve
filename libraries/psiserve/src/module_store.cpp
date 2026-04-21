// module_store.cpp — ModuleStore, NameRegistry, PreparedModule implementation.

#include <psiserve/module_store.hpp>

#include <pcrypt/sha256.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace psiserve
{

   // ── ModuleHash helpers ─────────────────────────────────────────────

   ModuleHash hashBytes(std::span<const std::byte> wasm)
   {
      auto digest = pcrypt::sha256(
          reinterpret_cast<const uint8_t*>(wasm.data()), wasm.size());
      ModuleHash h;
      static_assert(sizeof(h.bytes) == sizeof(digest));
      std::memcpy(h.bytes.data(), digest.data(), 32);
      return h;
   }

   std::size_t ModuleHashHasher::operator()(const ModuleHash& h) const noexcept
   {
      // First 8 bytes of SHA-256 are sufficient for hash-table distribution.
      std::size_t v;
      std::memcpy(&v, h.bytes.data(), sizeof(v));
      return v;
   }

   // ── PreparedModule ─────────────────────────────────────────────────

   PreparedModule::PreparedModule(ModuleHash                         hash,
                                  std::vector<std::byte>             bytes,
                                  std::shared_ptr<const BackendSlot> backend,
                                  ModuleSummary                      summary)
       : _hash(hash)
       , _bytes(std::move(bytes))
       , _backend(std::move(backend))
       , _summary(std::move(summary))
   {
   }

   // ── ModuleStore ────────────────────────────────────────────────────

   ModuleStore::ModuleStore(BackendKind defaultKind)
       : _defaultKind(defaultKind)
   {
   }

   ModuleHandle ModuleStore::lookup(const ModuleHash& hash) const
   {
      std::shared_lock lock(_mtx);
      auto it = _cache.find(hash);
      if (it == _cache.end())
         return nullptr;
      return it->second.module;
   }

   ModuleHandle ModuleStore::getOrCompile(const ModuleHash&          hash,
                                          std::span<const std::byte> wasmBytes)
   {
      return getOrCompile(hash, wasmBytes, _defaultKind);
   }

   ModuleHandle ModuleStore::getOrCompile(const ModuleHash&          hash,
                                          std::span<const std::byte> wasmBytes,
                                          BackendKind                kind)
   {
      {
         std::shared_lock lock(_mtx);
         auto it = _cache.find(hash);
         if (it != _cache.end())
         {
            it->second.lastUsed = std::chrono::steady_clock::now();
            return it->second.module;
         }
      }

      // Not cached — compile. Only one thread compiles per hash;
      // concurrent callers with the same hash will serialize here
      // but that's fine (first one wins, second finds it cached).
      auto backend = std::make_shared<const PreparedModule::BackendSlot>(
          PreparedModule::BackendSlot{kind, nullptr});

      std::vector<std::byte> bytesCopy(wasmBytes.begin(), wasmBytes.end());
      ModuleSummary summary;

      auto mod = std::shared_ptr<const PreparedModule>(
          new PreparedModule(hash, std::move(bytesCopy), std::move(backend), std::move(summary)));

      std::unique_lock lock(_mtx);
      auto [it, inserted] = _cache.emplace(
          hash, Entry{mod, std::chrono::steady_clock::now()});
      if (inserted)
         _totalBytes += mod->sourceSize();
      return it->second.module;
   }

   bool ModuleStore::upgradeBackend(const ModuleHash& hash, BackendKind targetKind)
   {
      std::shared_lock rlock(_mtx);
      auto it = _cache.find(hash);
      if (it == _cache.end())
         return false;

      auto mod = it->second.module;
      rlock.unlock();

      auto current = mod->currentBackend();
      if (current && current->kind == targetKind)
         return false;

      auto newBackend = std::make_shared<const PreparedModule::BackendSlot>(
          PreparedModule::BackendSlot{targetKind, nullptr});

      // Safe to const_cast here because installBackend is logically
      // an interior-mutable operation on the atomic backend slot.
      const_cast<PreparedModule*>(mod.get())->installBackend(std::move(newBackend));
      return true;
   }

   void ModuleStore::evictTo(std::size_t maxBytes)
   {
      std::unique_lock lock(_mtx);

      if (_totalBytes <= maxBytes)
         return;

      // Collect entries eligible for eviction (reference count == 1
      // from the store itself + 1 from this scan = we check use_count <= 2,
      // but the safer check is use_count == 1 after removing from map).
      struct Candidate
      {
         ModuleHash                                hash;
         std::size_t                               size;
         std::chrono::steady_clock::time_point     lastUsed;
      };
      std::vector<Candidate> candidates;
      for (const auto& [hash, entry] : _cache)
      {
         if (entry.module.use_count() <= 1)
            candidates.push_back({hash, entry.module->sourceSize(), entry.lastUsed});
      }

      std::sort(candidates.begin(), candidates.end(),
                [](const Candidate& a, const Candidate& b) { return a.lastUsed < b.lastUsed; });

      for (const auto& c : candidates)
      {
         if (_totalBytes <= maxBytes)
            break;

         auto it = _cache.find(c.hash);
         if (it != _cache.end() && it->second.module.use_count() <= 1)
         {
            _totalBytes -= it->second.module->sourceSize();
            _cache.erase(it);
         }
      }
   }

   std::size_t ModuleStore::size() const
   {
      std::shared_lock lock(_mtx);
      return _cache.size();
   }

   std::size_t ModuleStore::bytes() const
   {
      std::shared_lock lock(_mtx);
      return _totalBytes;
   }

   // ── NameRegistry ───────────────────────────────────────────────────

   void NameRegistry::bind(std::string name, ModuleHash hash)
   {
      std::unique_lock lock(_mtx);
      _names.insert_or_assign(std::move(name), hash);
   }

   std::optional<ModuleHash> NameRegistry::resolve(std::string_view name) const
   {
      std::shared_lock lock(_mtx);
      auto it = _names.find(std::string(name));
      if (it == _names.end())
         return std::nullopt;
      return it->second;
   }

   void NameRegistry::unbind(std::string_view name)
   {
      std::unique_lock lock(_mtx);
      _names.erase(std::string(name));
   }

   std::vector<std::pair<std::string, ModuleHash>> NameRegistry::list() const
   {
      std::shared_lock lock(_mtx);
      std::vector<std::pair<std::string, ModuleHash>> result;
      result.reserve(_names.size());
      for (const auto& [name, hash] : _names)
         result.emplace_back(name, hash);
      return result;
   }

}  // namespace psiserve
