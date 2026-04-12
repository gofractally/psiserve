// Unity build batch 3: Variant, varint, vector, and preconditions tests

#include <algorithm>
#include <vector>
#include <iterator>
#include <cstdlib>
#include <fstream>
#include <string>
#include <limits>
#include <exception>
#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <psizam/detail/variant.hpp>
#include <psizam/detail/leb128.hpp>
#include <psizam/types.hpp>
#include <psizam/detail/vector.hpp>
#include "utils.hpp"

namespace unity_variant_tests {
#include "variant_tests.cpp"
}

namespace unity_varint_tests {
#include "varint_tests.cpp"
}

namespace unity_vector_tests {
#include "vector_tests.cpp"
}

namespace unity_preconditions_tests {
#include "preconditions_tests.cpp"
}
