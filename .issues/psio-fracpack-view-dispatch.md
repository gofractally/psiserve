---
id: psio-fracpack-view-dispatch
title: "psio: fracpack view format tag for zero-copy dispatch"
status: open
priority: high
area: psio
agent: ~
branch: ~
created: 2026-04-21
depends_on: []
blocks: []
---

## Summary

PSIBASE_DISPATCH currently uses `psio::from_frac<ParamTuple>()` to
deserialize action args, which allocates for strings/vectors. The
psibase dispatch uses `psio::view<const ParamTuple>` with fracpack
prevalidated data for zero-copy access — string parameters become
`string_view` into the fracpack buffer, no allocation.

Our psio fork has the view framework (`view<T, Fmt>` with format
parameterization) but no fracpack format tag (`psio::frac`) that
implements the field accessor protocol. psibase's psio has it built
into a single-parameter `view<T>`.

## What's needed

### 1. Fracpack format tag (`psio::frac`)

```cpp
namespace psio {
   struct frac {
      using ptr_t = const char*;

      template <typename T>
      static ptr_t root(const void* buf) { return static_cast<ptr_t>(buf); }

      template <typename T, size_t I>
      static auto field(ptr_t data) {
         // Compute fracpack field offset using is_packable<T>::fixed_size
         // for each preceding field. Variable-size fields use offset
         // pointer indirection.
         constexpr auto offset = /* fracpack field offset for field I */;
         using FieldType = /* member type at index I */;
         if constexpr (is_packable<FieldType>::is_variable_size)
            return view<FieldType, frac>{data + read_offset(data + offset)};
         else
            return view<FieldType, frac>{data + offset};
      }
   };
}
```

The field offset computation mirrors what `frac_proxy_view::get<idx>()`
does in psibase's view.hpp (lines 88-125). It uses
`is_packable<T>::fixed_size` to sum preceding field sizes, then reads
an offset pointer for variable-size fields.

### 2. Scalar view specializations

`view<uint64_t, frac>` should just read the value from the pointer.
`view<string, frac>` should return `string_view` (ptr + len from the
fracpack encoding). These are the "leaf" views that the proxy
resolves to.

### 3. tuple_size / get<I> for views

Already added to view.hpp in this branch:
- `psio::get<I>(view<T, Fmt>)` free function
- `std::tuple_size<view<tuple<...>, Fmt>>` specialization
- `std::tuple_element<I, view<tuple<...>, Fmt>>` specialization

### 4. Update PSIBASE_DISPATCH

Replace `from_frac<ParamTuple>` with:
```cpp
check(fracpack_validate<ParamTuple>(param_data), "invalid args");
view<const ParamTuple, frac> param_view(param_data.data());
view_call([&](auto... args) { (service.*member)(args...); }, param_view);
```

Method signatures can then use `string_view` for string parameters
(zero-copy from the fracpack buffer).

## Reference

psibase's implementation: `~/psibase/libraries/psio/include/psio/view.hpp`
lines 84-125 (`frac_proxy_view`), lines 400-419 (`get<I>` for tuples).

Our view framework: `libraries/psio/cpp/include/psio/view.hpp`
