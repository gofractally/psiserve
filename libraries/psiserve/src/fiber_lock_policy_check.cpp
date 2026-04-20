// Placeholder — psitri's database is not currently templated on lock policy.
// When it becomes templated, add compile-time instantiation checks here.

#include <psiserve/fiber_lock_policy.hpp>

namespace
{
   static_assert(sizeof(psiserve::fiber_lock_policy) > 0,
                 "fiber_lock_policy must be a complete type");
}
