#pragma once

#include <memory>
#include <utility>

#include "model/ModelIds.h"
#include "model/TopologyReference.h"

class TopoDS_Shape;

namespace solidar {

class Body;
class TopologyIndex;

class FeatureTopologySnapshot final {
 public:
  FeatureTopologySnapshot(const FeatureTopologySnapshot&) = default;
  FeatureTopologySnapshot& operator=(const FeatureTopologySnapshot&) = default;
  FeatureTopologySnapshot(FeatureTopologySnapshot&&) noexcept = default;
  FeatureTopologySnapshot& operator=(FeatureTopologySnapshot&&) noexcept = default;

  [[nodiscard]] BodyId bodyId() const noexcept { return bodyId_; }
  [[nodiscard]] FeatureId featureId() const noexcept { return featureId_; }
  [[nodiscard]] ShapeRevision revision() const noexcept { return revision_; }
  [[nodiscard]] const std::shared_ptr<const TopoDS_Shape>& shape() const noexcept {
    return shape_;
  }
  [[nodiscard]] const std::shared_ptr<const TopologyIndex>& topologyIndex()
      const noexcept {
    return topologyIndex_;
  }
  [[nodiscard]] bool v2Authoritative() const noexcept {
    return v2Authoritative_;
  }

 private:
  friend class Body;
  FeatureTopologySnapshot(BodyId bodyId, FeatureId featureId,
                          ShapeRevision revision,
                          std::shared_ptr<const TopoDS_Shape> shape,
                          std::shared_ptr<const TopologyIndex> topologyIndex,
                          bool v2Authoritative) noexcept
      : bodyId_(bodyId), featureId_(featureId), revision_(revision),
        shape_(std::move(shape)), topologyIndex_(std::move(topologyIndex)),
        v2Authoritative_(v2Authoritative) {}

  BodyId bodyId_{kInvalidBodyId};
  FeatureId featureId_{kInvalidFeatureId};
  ShapeRevision revision_{kInvalidShapeRevision};
  std::shared_ptr<const TopoDS_Shape> shape_;
  std::shared_ptr<const TopologyIndex> topologyIndex_;
  bool v2Authoritative_{};
};

}  // namespace solidar
