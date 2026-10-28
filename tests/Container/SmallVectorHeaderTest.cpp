// Linux inventory 2026-10-01, finding I2: SmallVector::max_size() uses
// std::numeric_limits, and the header did not include <limits>. libstdc++ does not pull
// <limits> in transitively, so GCC fails with "incomplete type
// 'std::numeric_limits<long unsigned int>'". SmallVector.hpp MUST stay the FIRST include
// of this TU: that ordering is the self-containment proof.
#include "Astra/Container/SmallVector.hpp"

#include <cstddef>
#include <limits>
#include <gtest/gtest.h>

TEST(SmallVectorHeader, MaxSizeInstantiatesFromASelfContainedInclude)
{
    Astra::SmallVector<int, 4> v;
    EXPECT_EQ(v.max_size(), std::numeric_limits<std::size_t>::max());
}
