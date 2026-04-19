// Compile-time check that psiserve::fiber_lock_policy satisfies psitri's
// LockPolicy requirements. Instantiates basic_database and basic_dwal_database
// with fiber_lock_policy so any future drift in either side's API surfaces
// at build time rather than at first use.

#include <psiserve/fiber_lock_policy.hpp>

#include <psitri/database.hpp>
#include <psitri/dwal/dwal_database.hpp>

namespace
{
   using fiber_database      = psitri::basic_database<psiserve::fiber_lock_policy>;
   using fiber_dwal_database = psitri::dwal::basic_dwal_database<psiserve::fiber_lock_policy>;

   static_assert(sizeof(fiber_database) > 0,
                 "basic_database must be instantiable with fiber_lock_policy");
   static_assert(sizeof(fiber_dwal_database) > 0,
                 "basic_dwal_database must be instantiable with fiber_lock_policy");
}  // namespace
