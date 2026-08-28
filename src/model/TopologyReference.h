#pragma once

#include <cstddef>
#include <string>

#include "model/Body.h"
#include "model/Feature.h"

namespace solidar {

enum class TopologyKind { Face, Edge };

// Stable owner identity plus an explicitly marked legacy index fallback.
// persistentTag/geometricSignature reserve the migration path to OCCT
// history-based naming without changing every feature parameter again.
struct TopologyReference {
  BodyId bodyId{kInvalidBodyId};
  FeatureId featureId{kInvalidFeatureId};
  TopologyKind kind{TopologyKind::Face};
  std::size_t legacyIndex{};
  std::string persistentTag;
  std::string geometricSignature;

  [[nodiscard]] bool hasValidOwner() const noexcept {
    return bodyId != kInvalidBodyId && featureId != kInvalidFeatureId;
  }

  friend bool operator==(const TopologyReference&,
                         const TopologyReference&) = default;
};

}  // namespace solidar
