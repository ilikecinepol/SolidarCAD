#pragma once

#include <string>

#include "model/Body.h"
#include "model/OperationFailure.h"
#include "model/ShapeFeature.h"

namespace solidar {

[[nodiscard]] ShapeFeature::ShapePtr buildJoinedBodiesShape(
    const TopoDS_Shape& first, const TopoDS_Shape& second,
    std::string* error = nullptr,
    OperationFailureCode* code = nullptr);

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
  void setInputs(BodyId firstBodyId, FeatureId firstFeatureId,
                 BodyId secondBodyId, FeatureId secondFeatureId);

  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::JoinBodies;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  BodyId firstBodyId_{kInvalidBodyId};
  FeatureId firstFeatureId_{kInvalidFeatureId};
  BodyId secondBodyId_{kInvalidBodyId};
  FeatureId secondFeatureId_{kInvalidFeatureId};
};

}  // namespace solidar
