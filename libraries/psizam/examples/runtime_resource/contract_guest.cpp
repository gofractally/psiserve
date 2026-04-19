// contract_guest.cpp — minimal smart contract.
// Pure C exports, no PSIO_MODULE, no guest_alloc, no std::vector.
// Minimal memory footprint to avoid nested guard-page conflicts.

#include <stdint.h>

extern "C" {
   __attribute__((export_name("add")))
   uint32_t add(uint32_t a, uint32_t b) {
      return a + b;
   }

   __attribute__((export_name("multiply")))
   uint32_t multiply(uint32_t a, uint32_t b) {
      return a * b;
   }
}
