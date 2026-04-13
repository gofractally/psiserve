#include <psizam/backend.hpp>
#include <catch2/catch.hpp>
#include "utils.hpp"

using namespace psizam;
using namespace psizam::detail;

// Helper: build a minimal WASM module with EH
// All tests use the interpreter backend since JIT backends trap on throw.

/*
 * Module: simple throw + catch
 *
 * (module
 *   (type $t0 (func (param i32)))          ;; type 0: (i32) -> ()
 *   (type $t1 (func (result i32)))         ;; type 1: () -> (i32)
 *   (tag $e0 (type $t0))                   ;; tag 0: carries one i32
 *   (func (export "catch_simple") (result i32)
 *     (block $handler (result i32)         ;; label 0: handler target
 *       (try_table (result i32) (catch $e0 $handler)
 *         (throw $e0 (i32.const 42))
 *         (i32.const 0)                    ;; unreachable
 *       )
 *     )
 *   )
 * )
 *
 * Expected: catch_simple() returns 42
 */
TEST_CASE("EH: simple throw and catch via try_table", "[eh][interpreter]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,   // magic
      0x01, 0x00, 0x00, 0x00,   // version

      // Type section (id=1): 2 types, 9 bytes
      0x01, 0x09,
      0x02,
      0x60, 0x01, 0x7f, 0x00,   // type 0: (i32) -> ()
      0x60, 0x00, 0x01, 0x7f,   // type 1: () -> (i32)

      // Function section (id=3): 1 func, 2 bytes
      0x03, 0x02, 0x01, 0x01,

      // Tag section (id=13): 1 tag, 3 bytes
      0x0d, 0x03, 0x01, 0x00, 0x00,

      // Export section (id=7): 16 bytes
      0x07, 0x10, 0x01,
      0x0c, 'c','a','t','c','h','_','s','i','m','p','l','e',
      0x00, 0x00,

      // Code section (id=10)
      // body: 0locals(1) + block(2) + try_table(6) + i32.const(2) + throw(2) + i32.const(2) + end(3) = 18
      // section: count(1) + bodysize(1) + 18 = 20
      0x0a, 0x14, 0x01,
      0x12,                      // body size = 18
      0x00,                      // 0 locals
      0x02, 0x7f,                // block (result i32) — $handler
      0x1f, 0x7f,                // try_table (result i32)
      0x01,                      //   1 catch clause
      0x00, 0x00, 0x01,          //   catch tag=0 label=1
      0x41, 0x2a,                // i32.const 42
      0x08, 0x00,                // throw tag=0
      0x41, 0x00,                // i32.const 0 (unreachable fallthrough)
      0x0b,                      // end (try_table)
      0x0b,                      // end (block)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, interpreter>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "catch_simple");
   CHECK(result->to_ui32() == 42u);
}

/*
 * Module: catch_all
 *
 * (module
 *   (type $t0 (func (param i32)))
 *   (type $t1 (func (result i32)))
 *   (tag $e0 (type $t0))
 *   (func (export "catch_all_test") (result i32)
 *     (block $handler                       ;; label 0, void block
 *       (try_table (catch_all $handler)
 *         (throw $e0 (i32.const 99))
 *       )
 *       (return (i32.const 0))              ;; unreachable
 *     )
 *     (i32.const 1)                         ;; caught — return 1
 *   )
 * )
 *
 * Expected: catch_all_test() returns 1
 */
TEST_CASE("EH: catch_all catches any exception", "[eh][interpreter]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,
      0x01, 0x00, 0x00, 0x00,

      // Type section: 9 bytes
      0x01, 0x09, 0x02,
      0x60, 0x01, 0x7f, 0x00,   // type 0: (i32) -> ()
      0x60, 0x00, 0x01, 0x7f,   // type 1: () -> (i32)

      // Function section
      0x03, 0x02, 0x01, 0x01,

      // Tag section
      0x0d, 0x03, 0x01, 0x00, 0x00,

      // Export section: 1+1+14+1+1 = 18 bytes
      0x07, 0x12, 0x01,
      0x0e, 'c','a','t','c','h','_','a','l','l','_','t','e','s','t',
      0x00, 0x00,

      // Code section: body=20, section=22
      0x0a, 0x16, 0x01,
      0x14,                      // body size = 20
      0x00,                      // 0 locals
      0x02, 0x40,                // block $handler (void)
      0x1f, 0x40,                // try_table (void)
      0x01,                      //   1 clause
      0x02, 0x01,                //   catch_all label=1
      0x41, 0x63,                // i32.const 99
      0x08, 0x00,                // throw tag=0
      0x0b,                      // end (try_table)
      0x41, 0x00,                // i32.const 0
      0x0f,                      // return
      0x0b,                      // end (block)
      0x41, 0x01,                // i32.const 1
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, interpreter>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "catch_all_test");
   CHECK(result->to_ui32() == 1u);
}

/*
 * Module: unhandled exception traps
 *
 * (module
 *   (type $t0 (func (param i32)))
 *   (type $t1 (func))
 *   (tag $e0 (type $t0))
 *   (func (export "unhandled") (type $t1)
 *     (throw $e0 (i32.const 1))
 *   )
 * )
 *
 * Expected: unhandled() throws a wasm_interpreter_exception
 */
TEST_CASE("EH: unhandled exception traps", "[eh][interpreter]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,
      0x01, 0x00, 0x00, 0x00,

      // Type section
      0x01,
      0x08,
      0x02,
      0x60, 0x01, 0x7f, 0x00,   // type 0: (i32) -> ()
      0x60, 0x00, 0x00,         // type 1: () -> ()

      // Function section
      0x03, 0x02, 0x01, 0x01,

      // Tag section
      0x0d, 0x03, 0x01, 0x00, 0x00,

      // Export section
      0x07,
      0x0d,
      0x01,
      0x09,
      'u','n','h','a','n','d','l','e','d',
      0x00, 0x00,

      // Code section
      0x0a,
      0x08,
      0x01,
      0x06,                      // body size
      0x00,                      // 0 locals
      0x41, 0x01,                // i32.const 1
      0x08, 0x00,                // throw tag 0
      0x0b,                      // end
   };

   using backend_t = backend<std::nullptr_t, interpreter>;
   backend_t bkend(code, &wa);
   CHECK_THROWS_AS(bkend.call("env", "unhandled"), wasm_interpreter_exception);
}
