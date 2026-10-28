#pragma once

// Result lives in Mosaic now (shared with Arcane and the other Starworks
// libraries, which use it for exception-free error handling); Astra keeps its
// name. Result<T, E> and Result<void, E> are unchanged.
#include <Mosaic/Result.hpp>

namespace Astra
{
    using Mosaic::Result;
}
