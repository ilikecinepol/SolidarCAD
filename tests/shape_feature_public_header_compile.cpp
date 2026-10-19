#include "model/ShapeFeature.h"

#include <utility>

using ShapeFeatureV2FactoryResult = decltype(
    std::declval<solidar::ShapeFeature&>().createFaceReferenceV2(
        solidar::BodyId{}, 0));

static_assert(sizeof(ShapeFeatureV2FactoryResult) > 0);
