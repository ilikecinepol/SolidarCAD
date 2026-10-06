#pragma once

#include <string>

#include "model/Body.h"
#include "model/ShapeFeature.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildJoinedBodiesShape(
    const TopoDS_Shape& first, const TopoDS_Shape& second,
    std::string* error = nullptr);

class JoinBodiesFeature final : public ShapeFeature {
 public:
  JoinBodiesFeature(BodyId firstBodyId, FeatureId firstFeatureId,
                    BodyId secondBodyId, FeatureId secondFeatureId,
                    std::string name = {});
  JoinBodiesFeature(FeatureId id, BodyId firstBodyId,
                    FeatureId firstFeatureId, BodyId secondBodyId,
                    FeatureId secondFeatureId, std::string name);

  [[nodiscard]] BodyId firstBodyId() const noexcept;
  [[nodiscard]] FeatureId firstFeatureId() const noexcept;
  [[nodiscard]] BodyId secondBodyId() const noexcept;
  [[nodiscard]] FeatureId secondFeatureId() const noexcept;

  [[nodiscard]] bool dependsOnFeature(
      FeatureId featureId) const noexcept override;
  [[nodiscard]] std::string typeName() const override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  BodyId firstBodyId_{kInvalidBodyId};
  FeatureId firstFeatureId_{kInvalidFeatureId};
  BodyId secondBodyId_{kInvalidBodyId};
  FeatureId secondFeatureId_{kInvalidFeatureId};
};

}  // namespace solidar
