#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "model/FaceReference.h"
#include "model/EdgeReference.h"
#include "model/Feature.h"

class TopoDS_Shape;

namespace solidar {

class TopologyIndex;
class TopologyLineage;

// Parametric features own their generated B-Rep result. The incomplete OCCT
// type keeps the document model buildable until the geometry adapter is linked.
class ShapeFeature : public Feature {
public:
  using ShapePtr = std::shared_ptr<const TopoDS_Shape>;

  using Feature::Feature;
  ~ShapeFeature() override = default;

  [[nodiscard]] const ShapePtr& shape() const noexcept;
  [[nodiscard]] const ShapePtr& lastValidShape() const noexcept;
  [[nodiscard]] bool hasShape() const noexcept;
  [[nodiscard]] bool hasLastValidShape() const noexcept;
  [[nodiscard]] ShapeRevision shapeRevision() const noexcept;
  // The current dependency-facing index is unavailable while the Feature is
  // failed. The explicitly named fallback is presentation/diagnostic only.
  [[nodiscard]] std::shared_ptr<const TopologyIndex> topologyIndex(
      std::string* error = nullptr) const;
  [[nodiscard]] std::shared_ptr<const TopologyIndex> lastValidTopologyIndex(
      std::string* error = nullptr) const;
  [[nodiscard]] std::shared_ptr<const TopologyLineage> topologyLineage()
      const noexcept;
  [[nodiscard]] const std::shared_ptr<const TopologyLineage>&
  lastValidTopologyLineage() const noexcept;
  [[nodiscard]] FaceReferenceV2Creation createFaceReferenceV2(
      BodyId bodyId, std::size_t rawFaceIndex) const;
  [[nodiscard]] EdgeReferenceV2Creation createEdgeReferenceV2(
      BodyId bodyId, std::size_t rawEdgeIndex) const;
  void discardResult() noexcept;
  // Converts a retained history definition to a cold, recomputable payload.
  // The base implementation drops all runtime B-Rep/topology caches.
  virtual void prepareForHistory();

protected:
  virtual bool rebuildImpl(const RebuildContext& context) = 0;
  // Legacy/no-lineage result. This explicitly clears any staged lineage.
  void setShape(ShapePtr shape) noexcept;
  // V2 result. A null lineage is an invalid attempted v2 result, not a legacy
  // result; the rebuild boundary fails closed and restores the last snapshot.
  void setShapeAndTopologyLineage(
      ShapePtr shape, std::shared_ptr<const TopologyLineage> lineage) noexcept;
  void clearShape() noexcept;

private:
  bool rebuildAtBoundary(const RebuildContext& context) override final;
  [[nodiscard]] std::shared_ptr<const TopologyIndex> ensureTopologyIndex(
      std::string* error) const;
  ShapePtr shape_;
  ShapeRevision shapeRevision_{kInvalidShapeRevision};
  std::shared_ptr<const TopologyLineage> topologyLineage_;
  bool topologyLineageAttempted_{};
  mutable std::shared_ptr<const TopologyIndex> topologyIndex_;
  mutable bool topologyIndexAttempted_{};
  mutable std::string topologyIndexError_;
};

} // namespace solidar
