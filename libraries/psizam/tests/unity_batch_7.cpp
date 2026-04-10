// Unity build batch 7: Remaining limit tests, instantiation, null, and watchdog

#include <iostream>
#include <list>
#include <atomic>
#include <chrono>
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/watchdog.hpp>
#include "utils.hpp"

namespace unity_instantiation_tests {
#include "instantiation_tests.cpp"
}

namespace unity_max_nested_structures_tests {
#include "max_nested_structures_tests.cpp"
}

namespace unity_max_local_sets_tests {
#include "max_local_sets_tests.cpp"
}

namespace unity_max_code_bytes_tests {
#include "max_code_bytes_tests.cpp"
}

namespace unity_max_br_table_elements_tests {
#include "max_br_table_elements_tests.cpp"
}

namespace unity_max_table_elements_tests {
#include "max_table_elements_tests.cpp"
}

namespace unity_null_tests {
#include "null_tests.cpp"
}

namespace unity_watchdog_tests {
#include "watchdog_tests.cpp"
}
