## Building psizam

psizam requires a C++23 compliant toolchain (Clang 18+ recommended).

Since psizam is designed to be a header-only library (with the exception of softfloat), building the CMake project is not necessary to use psizam in a C++ project. However, if you need softfloat capabilities, building the library is required.

## Using The Example Tools

Once you have built psizam with `-DPSIZAM_ENABLE_TOOLS=ON`, you will find tools in `build/tools/`. You can run test WASMs with `psizam-interp <path>/<wasm>.wasm` to execute all exported functions. The `hello-driver` tool demonstrates host function integration with a prebaked hello-world WASM.

## Integrating Into an Existing CMake Project

Add psizam as a subdirectory and link against the `psizam` target:

```cmake
add_subdirectory(path/to/psizam)
target_link_libraries(your_target psizam)
```

CMake options can be found in `CMakeLists.txt` or by running `ccmake ..`.

### Getting Started

1. Create a type alias of `psizam::backend` with the host function class type.
   - By default this creates the interpreter backend. Set the second template argument to `psizam::jit` for JIT execution.
2. Create a `watchdog` timer with a duration, or use `null_watchdog` for unbounded execution.
3. Load your WASM — either read from a file or pass a `std::vector<uint8_t>` to the `backend` constructor.
4. Register and resolve host functions (see below).
5. Execute exports via the `()` operator of `psizam::backend`.

See `tools/hello_driver.cpp` for a complete example.

### Adding Host Functions

Host functions can be:
1. C-style functions
2. Static methods of a class
3. Member methods of a class

#### Registered Host Functions

```cpp
using rhf_t = psizam::registered_host_functions<HostClass>;

rhf_t::add<&HostClass::my_function>("module", "function_name");

// Resolve imports against a module
rhf_t::resolve(module);
```

See `tools/hello_driver.cpp` for a working example.
