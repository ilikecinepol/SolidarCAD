#pragma once

#include <optional>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeSource.h"
#include "model/ShapeFeature.h"

namespace solidar {

enum class ExtrudeOperation { NewBody, Join, Cut };

class ExtrudeFeature final : public ShapeFeature {
 public:
  ExtrudeFeature(SketchId profileSketchId, double lengthMm,
                 std::string name = {});
  ExtrudeFeature(SketchId profileSketchId, double lengthMm, std::string name,
                 ExtrudeOperation operation, bool reversed = false);
  ExtrudeFeature(FeatureId id, SketchId profileSketchId, double lengthMm,
                 std::string name);
  ExtrudeFeature(FeatureId id, SketchId profileSketchId, double lengthMm,
                 std::string name, ExtrudeOperation operation,
                 bool reversed = false);

  // Native face extrusion constructors. NewBody is not supported for a face
  // source; use Join or Cut.
  ExtrudeFeature(FaceReference face, double lengthMm, std::string name,
                 ExtrudeOperation operation, bool reversed = false);
  ExtrudeFeature(FeatureId id, FaceReference face, double lengthMm,
                 std::string name, ExtrudeOperation operation,
                 bool reversed = false);

  [[nodiscard]] const ExtrudeSource& source() const noexcept;
  [[nodiscard]] bool isFaceSource() const noexcept;
  [[nodiscard]] SketchId profileSketchId() const noexcept;
  void setProfileSketchId(SketchId id) noexcept;
  // When the user picks one region from a sketch containing several closed
  // contours, keep only that region as the feature profile while retaining
  // the original SketchId for placement, support and dependency tracking.
  [[nodiscard]] const std::optional<sketch::Sketch>&
  profileOverride() const noexcept;
  void setProfileOverride(std::optional<sketch::Sketch> profile);
  [[nodiscard]] std::optional<FaceReference> faceReference() const noexcept;
  [[nodiscard]] double lengthMm() const noexcept;
  void setLengthMm(double value) noexcept;
  [[nodiscard]] ExtrudeOperation operation() const noexcept;
  void setOperation(ExtrudeOperation value) noexcept;
  [[nodiscard]] bool reversed() const noexcept;
  void setReversed(bool value) noexcept;

  [[nodiscard]] FeatureKind kind() const noexcept override {
    return FeatureKind::Extrude;
  }
  [[nodiscard]] FeatureDependencies dependencies() const override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;
  void prepareForHistory() override;

 protected:
  bool rebuildImpl(const RebuildContext& context) override;

 private:
  struct ProfileSelectionIds {
    std::vector<sketch::GeometryId> lineIds;
    std::vector<sketch::GeometryId> circleIds;
    std::vector<sketch::GeometryId> arcIds;
  };

  bool rebuildFaceSource(const RebuildContext& context,
                         const FaceExtrudeSource& source);
  bool rebuildSketchSource(const RebuildContext& context);
  bool resolveProfileOverride(const sketch::Sketch& source,
                              sketch::Sketch* selected,
                              std::string* error);

  ExtrudeSource source_{SketchExtrudeSource{}};
  std::optional<sketch::Sketch> profileOverride_;
  std::optional<ProfileSelectionIds> profileSelectionIds_;
  double lengthMm_{0.0};
  ExtrudeOperation operation_{ExtrudeOperation::NewBody};
  bool reversed_{false};
};

}  // namespace solidar
