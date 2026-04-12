// Tests for .pzam attestation and trust policy.

#include <psizam/pzam_attestation.hpp>
#include <catch2/catch.hpp>

using namespace psizam;

// Mock signer: uses the pubkey hash as the "signature" XORed with the digest.
// Not real crypto — just enough to test the infrastructure.
struct mock_signer : pzam_signer {
   std::array<uint8_t, 32> _key;

   explicit mock_signer(uint8_t key_byte) {
      _key.fill(key_byte);
   }

   std::array<uint8_t, 32> pubkey_hash() const override {
      return _key;
   }

   std::vector<uint8_t> sign(std::span<const uint8_t, 32> digest) const override {
      std::vector<uint8_t> sig(32);
      for (int i = 0; i < 32; i++)
         sig[i] = digest[i] ^ _key[i];
      return sig;
   }
};

// Mock verifier: checks XOR-based mock signatures.
struct mock_verifier : pzam_verifier {
   bool verify(std::span<const uint8_t, 32> digest,
               std::span<const uint8_t, 32> pubkey_hash,
               std::span<const uint8_t> signature) const override {
      if (signature.size() != 32) return false;
      for (int i = 0; i < 32; i++) {
         if (signature[i] != (digest[i] ^ pubkey_hash[i]))
            return false;
      }
      return true;
   }
};

static pzam_code_section make_test_section() {
   pzam_code_section cs;
   cs.arch = static_cast<uint8_t>(pzam_arch::aarch64);
   cs.opt_tier = static_cast<uint8_t>(pzam_opt_tier::jit2);
   cs.max_stack = 16;
   cs.compiler.compiler_name = "test";
   cs.functions = {{0, 20, 4}, {20, 24, 8}};
   cs.code_blob.assign(44, 0x90);
   return cs;
}

TEST_CASE("attestation: content hash is deterministic", "[attestation]") {
   auto cs = make_test_section();
   auto h1 = pzam_content_hash(cs);
   auto h2 = pzam_content_hash(cs);
   CHECK(h1 == h2);
}

TEST_CASE("attestation: content hash changes with code", "[attestation]") {
   auto cs1 = make_test_section();
   auto cs2 = make_test_section();
   cs2.code_blob[0] = 0xCC;

   auto h1 = pzam_content_hash(cs1);
   auto h2 = pzam_content_hash(cs2);
   CHECK(h1 != h2);
}

TEST_CASE("attestation: content hash changes with metadata", "[attestation]") {
   auto cs1 = make_test_section();
   auto cs2 = make_test_section();
   cs2.max_stack = 32;

   CHECK(pzam_content_hash(cs1) != pzam_content_hash(cs2));
}

TEST_CASE("attestation: content hash excludes attestations", "[attestation]") {
   auto cs1 = make_test_section();
   auto cs2 = make_test_section();

   pzam_attestation att;
   att.pubkey_hash.fill(0xAA);
   att.signature = {1, 2, 3};
   cs2.attestations.push_back(att);

   // Hash should be the same — attestations are excluded
   CHECK(pzam_content_hash(cs1) == pzam_content_hash(cs2));
}

TEST_CASE("attestation: sign and verify", "[attestation]") {
   auto cs = make_test_section();
   mock_signer signer(0x42);
   mock_verifier verifier;

   auto att = pzam_sign(cs, signer);
   CHECK(att.pubkey_hash[0] == 0x42);
   CHECK(att.signature.size() == 32);

   CHECK(pzam_verify(cs, att, verifier));
}

TEST_CASE("attestation: tampered code fails verification", "[attestation]") {
   auto cs = make_test_section();
   mock_signer signer(0x42);
   mock_verifier verifier;

   auto att = pzam_sign(cs, signer);

   // Tamper with code
   cs.code_blob[0] = 0xFF;

   CHECK_FALSE(pzam_verify(cs, att, verifier));
}

TEST_CASE("attestation: wrong key fails verification", "[attestation]") {
   auto cs = make_test_section();
   mock_signer signer1(0x42);
   mock_signer signer2(0x99);
   mock_verifier verifier;

   auto att = pzam_sign(cs, signer1);

   // Change pubkey to a different key
   att.pubkey_hash = signer2.pubkey_hash();

   CHECK_FALSE(pzam_verify(cs, att, verifier));
}

TEST_CASE("trust policy: trust_all accepts anything", "[attestation]") {
   auto cs = make_test_section();
   auto policy = pzam_trust_policy::allow_all();
   CHECK(policy.check(cs));
}

TEST_CASE("trust policy: require_any needs N attestations", "[attestation]") {
   auto cs = make_test_section();
   mock_signer signer1(0x11);
   mock_signer signer2(0x22);
   mock_verifier verifier;

   auto policy = pzam_trust_policy::require_attestations(2);

   // No attestations — fails
   CHECK_FALSE(policy.check(cs, verifier));

   // One attestation — fails
   cs.attestations.push_back(pzam_sign(cs, signer1));
   CHECK_FALSE(policy.check(cs, verifier));

   // Two attestations — passes
   cs.attestations.push_back(pzam_sign(cs, signer2));
   CHECK(policy.check(cs, verifier));
}

TEST_CASE("trust policy: require_trusted needs trusted keys", "[attestation]") {
   auto cs = make_test_section();
   mock_signer trusted_signer(0x11);
   mock_signer untrusted_signer(0x22);
   mock_verifier verifier;

   auto policy = pzam_trust_policy::require_trusted(1);
   policy.add_trusted_key(trusted_signer.pubkey_hash());

   // Attestation from untrusted key — fails
   cs.attestations.push_back(pzam_sign(cs, untrusted_signer));
   CHECK_FALSE(policy.check(cs, verifier));

   // Add attestation from trusted key — passes
   cs.attestations.push_back(pzam_sign(cs, trusted_signer));
   CHECK(policy.check(cs, verifier));
}

TEST_CASE("trust policy: custom predicate", "[attestation]") {
   auto cs = make_test_section();
   mock_verifier verifier;

   // Custom: require code size > 10
   auto policy = pzam_trust_policy::custom(
      [](const pzam_code_section& cs, const pzam_verifier&) {
         return cs.code_blob.size() > 10;
      });

   CHECK(policy.check(cs, verifier));

   cs.code_blob.resize(5);
   CHECK_FALSE(policy.check(cs, verifier));
}
