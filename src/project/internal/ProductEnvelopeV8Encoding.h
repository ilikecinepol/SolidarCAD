#pragma once

#include "project/ProductEnvelopeV8Encoder.h"

namespace solidar::project::detail {

// Shared only with the strict V9 writer.  Produces the inherited V8 graph and
// part payload without deciding whether authored mates may be discarded.
[[nodiscard]] ProductEnvelopeV8EncodeResult encodeProductEnvelopeV8BaseRoot(
    const ProductDocument& source, ProjectId outputProjectId,
    const ProductEnvelopeV5Metadata& metadata, bool rejectAuthoredMates);

}  // namespace solidar::project::detail
