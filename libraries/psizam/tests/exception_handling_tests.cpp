#include <psizam/backend.hpp>
#include <catch2/catch.hpp>
#include "utils.hpp"

using namespace psizam;
using namespace psizam::detail;

// EH tests: interpreter + jit + jit2 + llvm
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
  #if defined(__x86_64__)
    #define EH_TEST_CASE(name, tags) \
      TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2, psizam::jit_llvm)
  #elif defined(__aarch64__)
    #define EH_TEST_CASE(name, tags) \
      TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit_llvm)
  #else
    #define EH_TEST_CASE(name, tags) \
      TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit_llvm)
  #endif
#elif defined(__x86_64__)
  #define EH_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2)
#elif defined(__aarch64__)
  #define EH_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit)
#else
  #define EH_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter)
#endif

// Helper: build a minimal WASM module with EH

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
EH_TEST_CASE("EH: simple throw and catch via try_table", "[eh]") {
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
      0x00, 0x00, 0x00,          //   catch tag=0 label=0 ($handler)
      0x41, 0x2a,                // i32.const 42
      0x08, 0x00,                // throw tag=0
      0x41, 0x00,                // i32.const 0 (unreachable fallthrough)
      0x0b,                      // end (try_table)
      0x0b,                      // end (block)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
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
EH_TEST_CASE("EH: catch_all catches any exception", "[eh]") {
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
      0x02, 0x00,                //   catch_all label=0 ($handler)
      0x41, 0x63,                // i32.const 99
      0x08, 0x00,                // throw tag=0
      0x0b,                      // end (try_table)
      0x41, 0x00,                // i32.const 0
      0x0f,                      // return
      0x0b,                      // end (block)
      0x41, 0x01,                // i32.const 1
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
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
EH_TEST_CASE("EH: unhandled exception traps", "[eh]") {
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

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   CHECK_THROWS_AS(bkend.call("env", "unhandled"), psizam::exception);
}

/*
 * Module: catch_tag_ref — catches with exnref + payload
 *
 * (module
 *   (type $t0 (func (param i32)))                   ;; type 0: (i32) -> ()
 *   (type $t1 (func (result i32)))                  ;; type 1: () -> (i32)
 *   (type $t2 (func (result i32 exnref)))           ;; type 2: () -> (i32, exnref)
 *   (tag $e0 (type $t0))
 *   (func (export "catch_ref_test") (result i32)
 *     (block $handler (type $t2)                    ;; block result = (i32, exnref)
 *       (try_table (catch_ref $e0 $handler)         ;; catch_ref delivers (i32, exnref)
 *         (throw $e0 (i32.const 77))
 *       )
 *       (unreachable)
 *     )
 *     ;; handler: stack has [i32, exnref]
 *     (drop)                                        ;; drop exnref
 *     ;; i32 payload (77) remains
 *   )
 * )
 *
 * Expected: catch_ref_test() returns 77
 */
EH_TEST_CASE("EH: catch_tag_ref catches with payload and exnref", "[eh]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,
      0x01, 0x00, 0x00, 0x00,

      // Type section: 3 types, 14 bytes
      0x01, 0x0e, 0x03,
      0x60, 0x01, 0x7f, 0x00,         // type 0: (i32) -> ()
      0x60, 0x00, 0x01, 0x7f,         // type 1: () -> (i32)
      0x60, 0x00, 0x02, 0x7f, 0x69,   // type 2: () -> (i32, exnref)

      // Function section
      0x03, 0x02, 0x01, 0x01,

      // Tag section
      0x0d, 0x03, 0x01, 0x00, 0x00,

      // Export section: "catch_ref_test"
      0x07, 0x12, 0x01,
      0x0e, 'c','a','t','c','h','_','r','e','f','_','t','e','s','t',
      0x00, 0x00,

      // Code section: body=19, section=21
      0x0a, 0x15, 0x01,
      0x13,                      // body size = 19
      0x00,                      // 0 locals
      0x02, 0x02,                // block (type 2) — result (i32, exnref)
      0x1f, 0x40,                // try_table (void)
      0x01,                      //   1 catch clause
      0x01, 0x00, 0x00,          //   catch_ref tag=0 label=0 ($handler)
      0x41, 0xcd, 0x00,          // i32.const 77 (LEB128)
      0x08, 0x00,                // throw tag=0
      0x0b,                      // end (try_table)
      0x00,                      // unreachable
      0x0b,                      // end (block)
      0x1a,                      // drop (exnref)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "catch_ref_test");
   CHECK(result->to_ui32() == 77u);
}

/*
 * Module: catch_all_ref — catches any exception, provides exnref
 *
 * (module
 *   (type $t0 (func (param i32)))
 *   (type $t1 (func (result i32)))
 *   (tag $e0 (type $t0))
 *   (func (export "catch_all_ref_test") (result i32)
 *     (block $handler (result exnref)       ;; catch_all_ref delivers exnref
 *       (try_table (catch_all_ref $handler)
 *         (throw $e0 (i32.const 55))
 *       )
 *       (unreachable)
 *     )
 *     ;; handler: exnref is on stack
 *     (drop)                                ;; drop exnref
 *     (i32.const 1)                         ;; return 1 = caught
 *   )
 * )
 *
 * Expected: catch_all_ref_test() returns 1
 */
EH_TEST_CASE("EH: catch_all_ref catches with exnref", "[eh]") {
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

      // Export section: "catch_all_ref_test"
      0x07, 0x16, 0x01,
      0x12, 'c','a','t','c','h','_','a','l','l','_','r','e','f','_','t','e','s','t',
      0x00, 0x00,

      // Code section: body=19, section=21
      0x0a, 0x15, 0x01,
      0x13,                      // body size = 19
      0x00,                      // 0 locals
      0x02, 0x69,                // block $handler (result exnref)
      0x1f, 0x40,                // try_table (void)
      0x01,                      //   1 clause
      0x03, 0x00,                //   catch_all_ref label=0 ($handler)
      0x41, 0x37,                // i32.const 55
      0x08, 0x00,                // throw tag=0
      0x0b,                      // end (try_table)
      0x00,                      // unreachable
      0x0b,                      // end (block)
      0x1a,                      // drop (exnref)
      0x41, 0x01,                // i32.const 1
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "catch_all_ref_test");
   CHECK(result->to_ui32() == 1u);
}

/*
 * Module: throw_ref — catch with exnref then re-throw
 *
 * (module
 *   (type $t0 (func (param i32)))
 *   (type $t1 (func (result i32)))
 *   (tag $e0 (type $t0))
 *   (func (export "throw_ref_test") (result i32)
 *     (block $outer (result i32)             ;; outer catch: gets i32 payload
 *       (try_table (result i32) (catch $e0 $outer)
 *         (block $inner (result exnref)      ;; inner catch: gets exnref
 *           (try_table (catch_all_ref $inner)
 *             (throw $e0 (i32.const 99))
 *           )
 *           (unreachable)
 *         )
 *         ;; inner handler: exnref on stack — re-throw it
 *         (throw_ref)
 *         (i32.const 0)                      ;; unreachable fallthrough
 *       )
 *     )
 *     ;; outer handler: i32 payload (99) on stack
 *   )
 * )
 *
 * Expected: throw_ref_test() returns 99
 */
EH_TEST_CASE("EH: throw_ref re-throws caught exception", "[eh]") {
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

      // Export section: "throw_ref_test"
      0x07, 0x12, 0x01,
      0x0e, 't','h','r','o','w','_','r','e','f','_','t','e','s','t',
      0x00, 0x00,

      // Code section: body=30, section=32
      0x0a, 0x20, 0x01,
      0x1e,                      // body size = 30
      0x00,                      // 0 locals
      0x02, 0x7f,                // block $outer (result i32)
      0x1f, 0x7f,                // try_table (result i32)
      0x01,                      //   1 clause
      0x00, 0x00, 0x00,          //   catch tag=0 label=0 ($outer)
      0x02, 0x69,                // block $inner (result exnref)
      0x1f, 0x40,                // try_table (void)
      0x01,                      //   1 clause
      0x03, 0x00,                //   catch_all_ref label=0 ($inner)
      0x41, 0xe3, 0x00,          // i32.const 99 (LEB128)
      0x08, 0x00,                // throw tag=0
      0x0b,                      // end (try_table inner)
      0x00,                      // unreachable
      0x0b,                      // end (block inner)
      // inner handler: exnref on stack
      0x0a,                      // throw_ref
      0x41, 0x00,                // i32.const 0 (unreachable)
      0x0b,                      // end (try_table outer)
      0x0b,                      // end (block outer)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "throw_ref_test");
   CHECK(result->to_ui32() == 99u);
}

/*
 * Module: multiple catch clauses — first matching wins
 *
 * (module
 *   (type $t0 (func (param i32)))          ;; type 0: (i32) -> ()
 *   (type $t1 (func (param i64)))          ;; type 1: (i64) -> ()
 *   (type $t2 (func (result i32)))         ;; type 2: () -> (i32)
 *   (tag $e0 (type $t0))                   ;; tag 0: i32 payload
 *   (tag $e1 (type $t1))                   ;; tag 1: i64 payload
 *   (func (export "multi_catch") (result i32)
 *     (block $catch_all_handler             ;; void — catch_all pushes nothing
 *       (block $e1_handler (result i64)     ;; i64 — tag 1 payload
 *         (block $e0_handler (result i32)   ;; i32 — tag 0 payload
 *           (try_table (catch $e0 $e0_handler) (catch $e1 $e1_handler) (catch_all $catch_all_handler)
 *             (throw $e1 (i64.const 100))
 *           )
 *           (i32.const 0) (return)          ;; normal path: never reached
 *         )
 *         ;; e0 handler: i32 on stack — return it
 *         (return)
 *       )
 *       ;; e1 handler: i64 on stack — wrap and return
 *       (i32.wrap_i64)
 *       (return)
 *     )
 *     ;; catch_all handler: nothing on stack
 *     (i32.const -1)
 *   )
 * )
 *
 * Expected: multi_catch() returns 100
 */
EH_TEST_CASE("EH: multiple catch clauses select correct handler", "[eh]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,
      0x01, 0x00, 0x00, 0x00,

      // Type section: 3 types, 13 bytes
      0x01, 0x0d, 0x03,
      0x60, 0x01, 0x7f, 0x00,   // type 0: (i32) -> ()
      0x60, 0x01, 0x7e, 0x00,   // type 1: (i64) -> ()
      0x60, 0x00, 0x01, 0x7f,   // type 2: () -> (i32)

      // Function section
      0x03, 0x02, 0x01, 0x02,   // func 0 uses type 2

      // Tag section: 2 tags
      0x0d, 0x05, 0x02,
      0x00, 0x00,               // tag 0: type 0
      0x00, 0x01,               // tag 1: type 1

      // Export section: "multi_catch"
      0x07, 0x0f, 0x01,
      0x0b, 'm','u','l','t','i','_','c','a','t','c','h',
      0x00, 0x00,

      // Code section: body=36, section=38
      0x0a, 0x26, 0x01,
      0x24,                      // body size = 36
      0x00,                      // 0 locals
      0x02, 0x40,                // block $catch_all_handler (void)
      0x02, 0x7e,                // block $e1_handler (result i64)
      0x02, 0x7f,                // block $e0_handler (result i32)
      0x1f, 0x40,                // try_table (void)
      0x03,                      //   3 catch clauses
      0x00, 0x00, 0x00,          //   catch tag=0 label=0 ($e0_handler)
      0x00, 0x01, 0x01,          //   catch tag=1 label=1 ($e1_handler)
      0x02, 0x02,                //   catch_all label=2 ($catch_all_handler)
      0x42, 0xe4, 0x00,          // i64.const 100 (LEB128)
      0x08, 0x01,                // throw tag=1
      0x0b,                      // end (try_table)
      0x41, 0x00,                // i32.const 0
      0x0f,                      // return
      0x0b,                      // end (block $e0_handler)
      // e0 handler: i32 payload on stack
      0x0f,                      // return
      0x0b,                      // end (block $e1_handler)
      // e1 handler: i64 payload on stack
      0xa7,                      // i32.wrap_i64
      0x0f,                      // return
      0x0b,                      // end (block $catch_all_handler)
      // catch_all handler: nothing
      0x41, 0x7f,                // i32.const -1
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "multi_catch");
   CHECK(result->to_ui32() == 100u);
}

/*
 * Module: nested try_table — inner doesn't match, outer catches
 *
 * (module
 *   (type $t0 (func (param i32)))          ;; type 0: (i32) -> ()
 *   (type $t1 (func (param i64)))          ;; type 1: (i64) -> ()
 *   (type $t2 (func (result i32)))         ;; type 2: () -> (i32)
 *   (tag $e0 (type $t0))                   ;; tag 0: i32
 *   (tag $e1 (type $t1))                   ;; tag 1: i64
 *   (func (export "nested") (result i32)
 *     (block $outer_catch (result i32)     ;; catches e0 payload
 *       (try_table (result i32) (catch $e0 $outer_catch)
 *         (try_table (result i32) (catch $e1 ???)  ;; inner only catches e1
 *           (throw $e0 (i32.const 42))             ;; throw e0 — inner won't match
 *           (i32.const 0)
 *         )
 *       )
 *     )
 *   )
 * )
 *
 * The inner try_table catches e1 but we throw e0. Dispatch should skip inner,
 * find outer's catch $e0 handler.
 *
 * Expected: nested() returns 42
 */
EH_TEST_CASE("EH: nested try_table propagates to outer handler", "[eh]") {
   std::vector<uint8_t> code = {
      0x00, 0x61, 0x73, 0x6d,
      0x01, 0x00, 0x00, 0x00,

      // Type section: 3 types, 13 bytes
      0x01, 0x0d, 0x03,
      0x60, 0x01, 0x7f, 0x00,   // type 0: (i32) -> ()
      0x60, 0x01, 0x7e, 0x00,   // type 1: (i64) -> ()
      0x60, 0x00, 0x01, 0x7f,   // type 2: () -> (i32)

      // Function section
      0x03, 0x02, 0x01, 0x02,

      // Tag section: 2 tags
      0x0d, 0x05, 0x02,
      0x00, 0x00,               // tag 0: type 0
      0x00, 0x01,               // tag 1: type 1

      // Export section: "nested"
      0x07, 0x0a, 0x01,
      0x06, 'n','e','s','t','e','d',
      0x00, 0x00,

      // Code section
      // Need a block for inner catch_e1 to target too — use a dummy block
      // body: 0locals(1) + block_outer(2) + try_table_outer(6)
      //       + block_dummy(2) + try_table_inner(6) + i32.const(2) + throw(2)
      //       + i32.const(2) + end(1) + end(1) + end(1) + end(1) + end(1) = 28
      // Wait, the inner try_table catch $e1 needs a label target.
      // Label 0 = try_table_inner itself, label 1 = block_dummy,
      // label 2 = try_table_outer, label 3 = block_outer
      // So inner catch $e1 targets label 1 (block_dummy), which accepts i64
      // But block_dummy result type needs to match — let's use void and catch_all instead
      // Simpler approach: inner catch_all goes to label 0 (i.e. the try_table itself)
      // But that's circular. Let me use a different structure.
      //
      // Actually simplest: inner has catch $e1 targeting label=2 (try_table_outer scope)
      // But the labels must be in-range... let me think about this more carefully.
      //
      // Structure:
      // block $outer (result i32)                   ;; pc_stack depth=1
      //   try_table (result i32) (catch $e0 $outer) ;; catch resolves from enclosing: label 0=$outer
      //     block $dummy (result i32)               ;; pc_stack depth=3
      //       try_table (result i32) (catch $e1 $dummy) ;; catch resolves from enclosing: label 0=$dummy
      //         throw $e0 (i32.const 42)
      //         i32.const 0
      //       end
      //     end
      //   end
      // end
      //
      // Catch labels resolve from enclosing scope (try_table not in scope for its own catch clauses)
      // catch $e1 label=0 means branch to $dummy

      0x0a, 0x1e, 0x01,
      0x1c,                      // body size = 28
      0x00,                      // 0 locals
      0x02, 0x7f,                // block $outer (result i32)
      0x1f, 0x7f,                // try_table_outer (result i32)
      0x01,                      //   1 clause
      0x00, 0x00, 0x00,          //   catch tag=0 label=0 ($outer)
      0x02, 0x7f,                // block $dummy (result i32)
      0x1f, 0x7f,                // try_table_inner (result i32)
      0x01,                      //   1 clause
      0x00, 0x01, 0x00,          //   catch tag=1 label=0 ($dummy)
      0x41, 0x2a,                // i32.const 42
      0x08, 0x00,                // throw tag=0
      0x41, 0x00,                // i32.const 0
      0x0b,                      // end (try_table inner)
      0x0b,                      // end (block $dummy)
      0x0b,                      // end (try_table outer)
      0x0b,                      // end (block $outer)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "nested");
   CHECK(result->to_ui32() == 42u);
}

/*
 * Module: try_table normal flow — no exception, falls through
 *
 * (module
 *   (type $t0 (func (param i32)))
 *   (type $t1 (func (result i32)))
 *   (tag $e0 (type $t0))
 *   (func (export "no_throw") (result i32)
 *     (block $handler (result i32)
 *       (try_table (result i32) (catch $e0 $handler)
 *         (i32.const 42)                   ;; no throw — value flows through
 *       )
 *     )
 *   )
 * )
 *
 * Expected: no_throw() returns 42
 */
EH_TEST_CASE("EH: try_table normal flow without exception", "[eh]") {
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

      // Export section: "no_throw"
      0x07, 0x0c, 0x01,
      0x08, 'n','o','_','t','h','r','o','w',
      0x00, 0x00,

      // Code section
      // body: 0locals(1) + block(2) + try_table(6) + i32.const(2) + end(3) = 14
      // section: count(1) + bodysize(1) + 14 = 16
      0x0a, 0x10, 0x01,
      0x0e,                      // body size = 14
      0x00,                      // 0 locals
      0x02, 0x7f,                // block (result i32)
      0x1f, 0x7f,                // try_table (result i32)
      0x01,                      //   1 clause
      0x00, 0x00, 0x00,          //   catch tag=0 label=0 ($handler)
      0x41, 0x2a,                // i32.const 42
      0x0b,                      // end (try_table)
      0x0b,                      // end (block)
      0x0b,                      // end (func)
   };

   using backend_t = backend<std::nullptr_t, TestType>;
   backend_t bkend(code, &wa);
   auto result = bkend.call_with_return("env", "no_throw");
   CHECK(result->to_ui32() == 42u);
}
