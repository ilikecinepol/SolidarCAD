#pragma once

#include <memory>

#include "model/Feature.h"

class TopoDS_Shape;

namespace solidar {

class TopologyIndex;

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
  void discardResult() noexcept;
  // Converts a retained history definition to a cold, recomputable payload.
  // The base implementation drops all runtime B-Rep/topology caches.
  virtual void prepareForHistory();

protected:
  virtual bool rebuildImpl(const RebuildContext& context) = 0;
  void setShape(ShapePtr shape) noexcept;
  void clearShape() noexcept;

private:
  bool rebuildAtBoundary(const RebuildContext& context) override final;
  [[nodiscard]] std::shared_ptr<const TopologyIndex> ensureTopologyIndex(
      std::string* error) const;
  ShapePtr shape_;
  ShapeRevision shapeRevision_{kInvalidShapeRevision};
  mutable std::shared_ptr<const TopologyIndex> topologyIndex_;
  mutable bool topologyIndexAttempted_{};
  mutable std::string topologyIndexError_;
};

} // namespace solidar
