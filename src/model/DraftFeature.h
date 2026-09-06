#pragma once

#include <optional>
#include <vector>

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>

#include "model/RevolveFeature.h"
#include "model/ShapeFeature.h"
#include "model/SketchPlacement.h"

namespace solidar {

enum class NeutralPlaneType { GlobalXY, GlobalXZ, GlobalYZ, BodyFace };

struct PlaneReference {
  NeutralPlaneType type{NeutralPlaneType::GlobalXY};
  std::optional<FaceReference> face;
  friend bool operator==(const PlaneReference&, const PlaneReference&) = default;
};

class DraftFeature final : public ShapeFeature {
 public:
  DraftFeature(FeatureId sourceFeatureId, std::vector<FaceReference> draftedFaces,
               PlaneReference neutralPlane, AxisReference pullDirection,
               double angleDeg = 5.0, bool reversed = false,
               std::string name = {});
  DraftFeature(FeatureId id, FeatureId sourceFeatureId,
               std::vector<FaceReference> draftedFaces,
               PlaneReference neutralPlane, AxisReference pullDirection,
               double angleDeg, bool reversed, std::string name);

  [[nodiscard]] FeatureId sourceFeatureId() const noexcept;
  [[nodiscard]] const std::vector<FaceReference>& draftedFaces() const noexcept;
  [[nodiscard]] const PlaneReference& neutralPlane() const noexcept;
  [[nodiscard]] const AxisReference& pullDirection() const noexcept;
  [[nodiscard]] double angleDeg() const noexcept;
  [[nodiscard]] bool reversed() const noexcept;
  void setDraftedFaces(std::vector<FaceReference> value);
  void setNeutralPlane(PlaneReference value);
  void setPullDirection(AxisReference value);
  void setAngleDeg(double value) noexcept;
  void setReversed(bool value) noexcept;

  [[nodiscard]] std::string typeName() const override;
  [[nodiscard]] bool dependsOnSketch(SketchId id) const noexcept override;
  bool rebuild(const RebuildContext& context) override;
  [[nodiscard]] std::unique_ptr<Feature> clone() const override;

 private:
  FeatureId sourceFeatureId_{kInvalidFeatureId};
  std::vector<FaceReference> draftedFaces_;
  PlaneReference neutralPlane_;
  AxisReference pullDirection_{AxisReferenceType::GlobalZ,
                               kInvalidSketchId,
                               sketch::kInvalidGeometryId};
  double angleDeg_{5.0};
  bool reversed_{false};
};

[[nodiscard]] bool resolveDraftReferences(
    const Document& document, const TopoDS_Shape& baseShape,
    const PlaneReference& plane, const AxisReference& direction,
    gp_Pln* resolvedPlane, gp_Dir* resolvedDirection, std::string* error);

}  // namespace solidar
