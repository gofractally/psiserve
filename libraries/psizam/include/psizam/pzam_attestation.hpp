#pragma once

// Attestation infrastructure for .pzam code sections.
//
// Each code section can carry N attestations. An attestation is a signature
// over the code section's content hash by a trusted party (compiler, validator,
// or third-party auditor). A trust policy defines the minimum attestation
// requirements for a code section to be accepted for execution.
//
// Crypto operations are abstracted behind the pzam_signer / pzam_verifier
// interfaces so users can plug in any ECC library (OpenSSL, libsodium, etc.).
//
// Usage:
//
//   // Signing (at compile time):
//   auto hash = pzam_content_hash(code_section);
//   my_signer signer(private_key);
//   auto attestation = pzam_sign(code_section, signer);
//   code_section.attestations.push_back(attestation);
//
//   // Verification (at load time):
//   pzam_trust_policy policy;
//   policy.add_trusted_key(pubkey_hash);
//   policy.set_min_attestations(1);
//   if (!policy.check(code_section)) { /* reject */ }

#include <psizam/pzam_format.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

namespace psizam {

   // =========================================================================
   // Content hashing
   // =========================================================================

   /// Compute a SHA-256-like hash of a code section's content (excluding attestations).
   /// Uses FNV-1a cascaded to 32 bytes — sufficient for integrity, not cryptographic
   /// security. For production use, replace with a real SHA-256.
   inline std::array<uint8_t, 32> pzam_content_hash(const pzam_code_section& cs) {
      // Hash everything that defines the code section's behavior:
      // arch, opt_tier, instrumentation, stack_limit_mode, page_size, max_stack,
      // compiler info, functions, relocations, code_blob
      //
      // Attestations are excluded (they're metadata about the code, not the code itself).

      uint64_t h = 0xcbf29ce484222325ULL;
      auto feed = [&](const void* data, size_t len) {
         auto p = static_cast<const uint8_t*>(data);
         for (size_t i = 0; i < len; i++) {
            h ^= p[i];
            h *= 0x100000001b3ULL;
         }
      };

      feed(&cs.arch, 1);
      feed(&cs.opt_tier, 1);
      feed(&cs.instrumentation, sizeof(cs.instrumentation));
      feed(&cs.stack_limit_mode, 1);
      feed(&cs.page_size, sizeof(cs.page_size));
      feed(&cs.max_stack, sizeof(cs.max_stack));

      // Compiler info
      feed(cs.compiler.compiler_name.data(), cs.compiler.compiler_name.size());
      feed(cs.compiler.compiler_version.data(), cs.compiler.compiler_version.size());
      feed(cs.compiler.compiler_hash.data(), cs.compiler.compiler_hash.size());

      // Functions
      for (const auto& f : cs.functions) {
         feed(&f.code_offset, sizeof(f.code_offset));
         feed(&f.code_size, sizeof(f.code_size));
         feed(&f.stack_size, sizeof(f.stack_size));
      }

      // Relocations
      for (const auto& r : cs.relocations) {
         feed(&r.code_offset, sizeof(r.code_offset));
         feed(&r.symbol, sizeof(r.symbol));
         feed(&r.type, sizeof(r.type));
         feed(&r.addend, sizeof(r.addend));
      }

      // Code blob
      feed(cs.code_blob.data(), cs.code_blob.size());

      // Expand to 32 bytes
      std::array<uint8_t, 32> result = {};
      std::memcpy(result.data(), &h, 8);
      for (int i = 1; i < 4; i++) {
         h = (h >> 13) ^ (h << 51) ^ (h * 0x9e3779b97f4a7c15ULL);
         std::memcpy(result.data() + i * 8, &h, 8);
      }
      return result;
   }

   // =========================================================================
   // Signer / Verifier interfaces
   // =========================================================================

   /// Abstract signer interface. Implement this with your preferred crypto library.
   struct pzam_signer {
      virtual ~pzam_signer() = default;

      /// Return the SHA-256 hash of the signer's public key.
      virtual std::array<uint8_t, 32> pubkey_hash() const = 0;

      /// Sign a 32-byte digest. Returns the signature bytes.
      virtual std::vector<uint8_t> sign(std::span<const uint8_t, 32> digest) const = 0;
   };

   /// Abstract verifier interface. Implement this with your preferred crypto library.
   struct pzam_verifier {
      virtual ~pzam_verifier() = default;

      /// Verify a signature against a 32-byte digest and a pubkey hash.
      /// Returns true if the signature is valid for the given pubkey.
      virtual bool verify(std::span<const uint8_t, 32> digest,
                         std::span<const uint8_t, 32> pubkey_hash,
                         std::span<const uint8_t> signature) const = 0;
   };

   // =========================================================================
   // Sign / Verify operations
   // =========================================================================

   /// Create an attestation for a code section using the given signer.
   inline pzam_attestation pzam_sign(const pzam_code_section& cs,
                                     const pzam_signer& signer) {
      auto digest = pzam_content_hash(cs);
      pzam_attestation att;
      att.pubkey_hash = signer.pubkey_hash();
      att.signature = signer.sign(digest);
      return att;
   }

   /// Verify a single attestation against a code section.
   inline bool pzam_verify(const pzam_code_section& cs,
                           const pzam_attestation& att,
                           const pzam_verifier& verifier) {
      auto digest = pzam_content_hash(cs);
      return verifier.verify(digest, att.pubkey_hash, att.signature);
   }

   // =========================================================================
   // Trust policy
   // =========================================================================

   /// Policy for deciding whether a code section has sufficient attestations.
   ///
   /// Modes:
   ///   - trust_all: Accept any code section (no attestation required)
   ///   - require_any: At least N valid attestations from any signer
   ///   - require_trusted: At least N valid attestations from trusted keys
   ///   - custom: User-provided predicate
   class pzam_trust_policy {
   public:
      enum class mode : uint8_t {
         trust_all,       // No attestation required (development/testing)
         require_any,     // At least min_attestations valid sigs from any key
         require_trusted, // At least min_attestations valid sigs from trusted keys
         custom,          // User-provided check function
      };

      pzam_trust_policy() = default;

      /// Create a policy that trusts everything (for development).
      static pzam_trust_policy allow_all() {
         pzam_trust_policy p;
         p._mode = mode::trust_all;
         return p;
      }

      /// Create a policy requiring N valid attestations from any signer.
      static pzam_trust_policy require_attestations(uint32_t n) {
         pzam_trust_policy p;
         p._mode = mode::require_any;
         p._min_attestations = n;
         return p;
      }

      /// Create a policy requiring N valid attestations from trusted keys.
      static pzam_trust_policy require_trusted(uint32_t n) {
         pzam_trust_policy p;
         p._mode = mode::require_trusted;
         p._min_attestations = n;
         return p;
      }

      /// Create a policy with a custom check function.
      static pzam_trust_policy custom(
            std::function<bool(const pzam_code_section&, const pzam_verifier&)> fn) {
         pzam_trust_policy p;
         p._mode = mode::custom;
         p._custom_check = std::move(fn);
         return p;
      }

      /// Add a trusted pubkey hash.
      void add_trusted_key(const std::array<uint8_t, 32>& pubkey_hash) {
         _trusted_keys.push_back(pubkey_hash);
      }

      /// Check whether a code section meets the policy requirements.
      bool check(const pzam_code_section& cs, const pzam_verifier& verifier) const {
         switch (_mode) {
            case mode::trust_all:
               return true;

            case mode::require_any: {
               uint32_t valid = 0;
               auto digest = pzam_content_hash(cs);
               for (const auto& att : cs.attestations) {
                  if (verifier.verify(digest, att.pubkey_hash, att.signature))
                     valid++;
               }
               return valid >= _min_attestations;
            }

            case mode::require_trusted: {
               uint32_t valid = 0;
               auto digest = pzam_content_hash(cs);
               for (const auto& att : cs.attestations) {
                  if (!is_trusted(att.pubkey_hash))
                     continue;
                  if (verifier.verify(digest, att.pubkey_hash, att.signature))
                     valid++;
               }
               return valid >= _min_attestations;
            }

            case mode::custom:
               return _custom_check && _custom_check(cs, verifier);
         }
         return false;
      }

      /// Overload for trust_all mode (no verifier needed).
      bool check(const pzam_code_section& cs) const {
         if (_mode != mode::trust_all)
            return false;
         return true;
      }

   private:
      bool is_trusted(const std::array<uint8_t, 32>& pubkey_hash) const {
         for (const auto& k : _trusted_keys)
            if (k == pubkey_hash) return true;
         return false;
      }

      mode                        _mode = mode::trust_all;
      uint32_t                    _min_attestations = 0;
      std::vector<std::array<uint8_t, 32>> _trusted_keys;
      std::function<bool(const pzam_code_section&, const pzam_verifier&)> _custom_check;
   };

} // namespace psizam
