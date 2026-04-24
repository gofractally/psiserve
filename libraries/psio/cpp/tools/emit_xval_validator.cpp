// Emit a test Validator in SSZ to cross-validate Rust's port.
#include "/Users/dlarimer/psiserve-agent3/libraries/psio/cpp/benchmarks/beacon_types.hpp"
#include <psio/to_ssz.hpp>
#include <cstdio>

int main()
{
   eth::Validator v;
   for (int i = 0; i < 48; ++i) v.pubkey[i] = i + 1;
   for (int i = 0; i < 32; ++i) v.withdrawal_credentials[i] = 100 + i;
   v.effective_balance = 32'000'000'000ULL;
   v.slashed = true;
   v.activation_eligibility_epoch = 10;
   v.activation_epoch = 20;
   v.exit_epoch = 0xffffffffffffffffULL;
   v.withdrawable_epoch = 0xffffffffffffffffULL;

   auto b = psio::convert_to_ssz(v);
   std::printf("validator_sample.ssz (%zu bytes)\n", b.size());
   for (auto c : b) std::printf("%02x", (unsigned char)c);
   std::printf("\n");
}
