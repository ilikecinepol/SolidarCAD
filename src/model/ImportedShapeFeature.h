#pragma once

#include "model/ShapeFeature.h"

namespace solidar {

// A non-parametric B-Rep imported from an external CAD format.  Keep a
// separate immutable source shape so Body::rebuild() can recreate the feature
// result after it has been dirtied or restored from a native project.
class ImportedShapeFeature final : public ShapeFeature {
 public:
  ImportedShapeFeature(ShapePtr shape, std::string name = {});
  ImportedShapeFeature(FeatureId id, ShapePtr shape, std::string name = {});

  [[nodiscard]] const ShapePtr& importedShape() const noexcept;
  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::ImportedShape;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;
  void prepareForHistory() override;
  [[nodiscard]] const std::string& historyArchive() const noexcept;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  ShapePtr importedShape_;
  std::string historyArchive_;
};

}  // namespace solidar
