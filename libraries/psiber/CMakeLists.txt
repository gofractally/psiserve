# ── psiber: fiber scheduling & concurrency library ──────────────────────────────

add_library(psiber
   src/scheduler.cpp
   src/send_queue.cpp
   src/fiber_mutex.cpp
   src/strand.cpp
   src/reactor.cpp
   src/tcp_socket.cpp
   src/thread.cpp
)

if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
   target_sources(psiber PRIVATE src/io_engine_kqueue.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_sources(psiber PRIVATE src/io_engine_epoll.cpp)
else()
   message(FATAL_ERROR "No I/O engine for this platform")
endif()

target_include_directories(psiber PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

find_package(Boost REQUIRED COMPONENTS context)
target_link_libraries(psiber PUBLIC
   ucc
   Boost::boost
   Boost::context
   Threads::Threads
)

# ── Optional: async DNS (c-ares) ───────────────────────────────────────────────
option(PSIBER_ENABLE_DNS "Build with async DNS support (requires c-ares)" ON)
if(PSIBER_ENABLE_DNS)
   find_package(c-ares QUIET)
   if(c-ares_FOUND)
      target_sources(psiber PRIVATE src/dns.cpp)
      target_link_libraries(psiber PUBLIC c-ares::cares)
      target_compile_definitions(psiber PUBLIC PSIBER_HAS_DNS=1)
      message(STATUS "psiber: async DNS enabled (c-ares)")
   else()
      message(STATUS "psiber: async DNS disabled (c-ares not found)")
   endif()
endif()

# ── Optional: TLS (OpenSSL) ────────────────────────────────────────────────────
option(PSIBER_ENABLE_TLS "Build with TLS support (requires OpenSSL)" ON)
if(PSIBER_ENABLE_TLS)
   find_package(OpenSSL QUIET)
   if(OpenSSL_FOUND)
      target_sources(psiber PRIVATE src/tls_socket.cpp)
      target_link_libraries(psiber PUBLIC OpenSSL::SSL OpenSSL::Crypto)
      target_compile_definitions(psiber PUBLIC PSIBER_HAS_TLS=1)
      message(STATUS "psiber: TLS enabled (OpenSSL ${OPENSSL_VERSION})")
   else()
      message(STATUS "psiber: TLS disabled (OpenSSL not found)")
   endif()
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
option(PSIBER_ENABLE_TESTS "Build psiber tests" OFF)
if(PSIBER_ENABLE_TESTS)
   set(_psiber_test_sources
      tests/fiber_wake_tests.cpp
      tests/send_queue_tests.cpp
      tests/fiber_mutex_tests.cpp
      tests/scheduler_tests.cpp
      tests/stress_tests.cpp
      tests/deadlock_tests.cpp
      tests/edge_case_tests.cpp
      tests/integration_tests.cpp
      tests/lifo_slot_tests.cpp
      tests/tcp_socket_tests.cpp
      tests/signal_tests.cpp
      tests/strand_tests.cpp
      tests/reactor_tests.cpp
      tests/reactor_integration_tests.cpp
      tests/detail_atomic_tests.cpp
   )

   if(c-ares_FOUND AND PSIBER_ENABLE_DNS)
      list(APPEND _psiber_test_sources tests/dns_tests.cpp)
   endif()
   if(OpenSSL_FOUND AND PSIBER_ENABLE_TLS)
      list(APPEND _psiber_test_sources tests/tls_socket_tests.cpp)
   endif()

   add_executable(psiber_tests ${_psiber_test_sources})
   target_link_libraries(psiber_tests PRIVATE psiber Threads::Threads)
   target_include_directories(psiber_tests PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
   )
   add_test(NAME psiber_tests COMMAND psiber_tests)
endif()

# ── Benchmarks ─────────────────────────────────────────────────────────────────
option(PSIBER_ENABLE_BENCHMARKS "Build psiber benchmarks" OFF)
if(PSIBER_ENABLE_BENCHMARKS)
   add_executable(psiber_bench benchmarks/psiber_bench.cpp)
   target_link_libraries(psiber_bench PRIVATE psiber Threads::Threads)
   target_include_directories(psiber_bench PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
   )

   add_executable(sync_vs_call_bench benchmarks/sync_vs_call_bench.cpp)
   target_link_libraries(sync_vs_call_bench PRIVATE psiber Threads::Threads)

   # Optional Boost.Fiber comparison benchmark (for marketing/evaluation)
   option(PSIBER_ENABLE_BOOST_FIBER_BENCH "Build Boost.Fiber comparison benchmark" OFF)
   if(PSIBER_ENABLE_BOOST_FIBER_BENCH)
      find_package(Boost REQUIRED COMPONENTS fiber context)
      add_executable(boost_fiber_bench benchmarks/boost_fiber_bench.cpp)
      target_link_libraries(boost_fiber_bench PRIVATE
         Boost::fiber Boost::context Threads::Threads
      )
   endif()
endif()
