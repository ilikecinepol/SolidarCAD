#pragma once

#include <cstdint>

namespace solidar {

using BodyId = std::uint64_t;
inline constexpr BodyId kInvalidBodyId = 0;
using FeatureId = std::uint64_t;
inline constexpr FeatureId kInvalidFeatureId = 0;
using SketchId = std::uint64_t;
inline constexpr SketchId kInvalidSketchId = 0;
using ShapeRevision = std::uint64_t;
inline constexpr ShapeRevision kInvalidShapeRevision = 0;

}  // namespace solidar
