#include "model/ShapeFeature.h"

#include <BRepCheck_Analyzer.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <atomic>
#include <utility>

#include "model/GeometryOperation.h"
#include "model/TopologyReferenceResolver.h"

namespace solidar {
namespace {

std::atomic<ShapeRevision> g_nextShapeRevision{1};

std::string exceptionDiagnostic(GeometryFailureKind kind) {
  switch (kind) {
  case GeometryFailureKind::OcctException:
    return "OpenCASCADE geometry operation failed";
  case GeometryFailureKind::StandardException:
    return "Geometry operation failed with a standard exception";
  case GeometryFailureKind::UnknownException:
    return "Geometry operation failed with an unknown exception";
  default:
    return "Geometry operation failed";
  }
}

} // namespace

const ShapeFeature::ShapePtr& ShapeFeature::shape() const noexcept {
  static const ShapePtr empty;
  return isValid() ? shape_ : empty;
}

const ShapeFeature::ShapePtr& ShapeFeature::lastValidShape() const noexcept {
  return shape_;
}

bool ShapeFeature::hasShape() const noexcept { return bool(shape()); }

bool ShapeFeature::hasLastValidShape() const noexcept { return bool(shape_); }

ShapeRevision ShapeFeature::shapeRevision() const noexcept {
  return shapeRevision_;
}

std::shared_ptr<const TopologyIndex> ShapeFeature::topologyIndex(
    std::string* error) const {
  if (!isValid()) {
    if (error) *error = "Topology is unavailable for an invalid Feature";
    return {};
  }
  return ensureTopologyIndex(error);
}

std::shared_ptr<const TopologyIndex> ShapeFeature::lastValidTopologyIndex(
    std::string* error) const {
  return ensureTopologyIndex(error);
}

std::shared_ptr<const TopologyLineage> ShapeFeature::topologyLineage()
    const noexcept {
  return isValid() ? topologyLineage_ : std::shared_ptr<const TopologyLineage>{};
}

const std::shared_ptr<const TopologyLineage>&
ShapeFeature::lastValidTopologyLineage() const noexcept {
  return topologyLineage_;
}

std::shared_ptr<const TopologyIndex> ShapeFeature::ensureTopologyIndex(
    std::string* error) const {
  if (!shape_ || shape_->IsNull() ||
      shapeRevision_ == kInvalidShapeRevision) {
    if (error) *error = "Topology source shape is unavailable";
    return {};
  }
  if (!topologyIndexAttempted_) {
    auto index = TopologyIndex::build(shape_, shapeRevision_,
                                      topologyLineage_, &topologyIndexError_);
    topologyIndex_ = std::move(index);
    topologyIndexAttempted_ = true;
  }
  if (error) *error = topologyIndexError_;
  return topologyIndex_;
}

bool ShapeFeature::rebuildAtBoundary(const RebuildContext& context) {
  const ShapePtr previousShape = shape_;
  const ShapeRevision previousRevision = shapeRevision_;
  const auto previousTopologyLineage = topologyLineage_;
  const bool previousTopologyLineageAttempted = topologyLineageAttempted_;
  const auto previousTopologyIndex = topologyIndex_;
  const bool previousTopologyAttempted = topologyIndexAttempted_;
  const std::string previousTopologyError = topologyIndexError_;
  try {
    GeometryFailure failure;
    bool rebuilt = false;
    const bool completed =
        runGeometryOperation([&] { rebuilt = rebuildImpl(context); }, &failure);

    std::string diagnostic;
    if (!completed) {
      diagnostic = exceptionDiagnostic(failure.kind);
    } else if (!rebuilt) {
      diagnostic = error();
    } else if (!isValid()) {
      diagnostic = "Geometry rebuild did not produce a valid feature state";
    } else if (!shape_ || shape_->IsNull()) {
      diagnostic = "Geometry rebuild did not produce a B-Rep shape";
    } else {
      bool brepValid = false;
      GeometryFailure validationFailure;
      const bool validationCompleted = runGeometryOperation(
          [&] { brepValid = BRepCheck_Analyzer(*shape_).IsValid(); },
          &validationFailure);
      if (validationCompleted && brepValid) {
        const ShapeRevision candidateRevision =
            g_nextShapeRevision.fetch_add(1, std::memory_order_relaxed);
        if (topologyLineageAttempted_ && !topologyLineage_) {
          diagnostic = "Geometry rebuild attempted invalid topology lineage";
        } else if (topologyLineageAttempted_) {
          std::string topologyError;
          auto index = TopologyIndex::build(shape_, candidateRevision,
                                            topologyLineage_, &topologyError);
          if (index) {
            shapeRevision_ = candidateRevision;
            topologyIndex_ = std::move(index);
            topologyIndexAttempted_ = true;
            topologyIndexError_.clear();
            return true;
          }
          diagnostic = topologyError.empty()
                           ? "Geometry rebuild produced invalid topology lineage"
                           : std::move(topologyError);
        } else {
          shapeRevision_ = candidateRevision;
          topologyIndex_.reset();
          topologyIndexAttempted_ = false;
          topologyIndexError_.clear();
          return true;
        }
      }
      if (diagnostic.empty()) {
        diagnostic = validationCompleted
                         ? "Geometry rebuild produced an invalid B-Rep shape"
                         : exceptionDiagnostic(validationFailure.kind);
      }
    }

    shape_ = previousShape;
    shapeRevision_ = previousRevision;
    topologyLineage_ = previousTopologyLineage;
    topologyLineageAttempted_ = previousTopologyLineageAttempted;
    topologyIndex_ = previousTopologyIndex;
    topologyIndexAttempted_ = previousTopologyAttempted;
    topologyIndexError_ = previousTopologyError;
    if (diagnostic.empty())
      diagnostic = "Geometry rebuild failed";
    markError(std::move(diagnostic));
    return false;
  } catch (...) {
    shape_ = previousShape;
    shapeRevision_ = previousRevision;
    topologyLineage_ = previousTopologyLineage;
    topologyLineageAttempted_ = previousTopologyLineageAttempted;
    topologyIndex_ = previousTopologyIndex;
    topologyIndexAttempted_ = previousTopologyAttempted;
    topologyIndexError_ = previousTopologyError;
    try {
      markError("Geometry rebuild failed at the exception boundary");
    } catch (...) {
    }
    return false;
  }
}

void ShapeFeature::discardResult() noexcept {
  clearShape();
  shapeRevision_ = kInvalidShapeRevision;
  topologyLineage_.reset();
  topologyLineageAttempted_ = false;
  topologyIndex_.reset();
  topologyIndexAttempted_ = false;
  topologyIndexError_.clear();
}

void ShapeFeature::prepareForHistory() {
  discardResult();
  setDirty();
}

void ShapeFeature::setShape(ShapePtr shape) noexcept {
  shape_ = std::move(shape);
  topologyLineage_.reset();
  topologyLineageAttempted_ = false;
}

void ShapeFeature::setShapeAndTopologyLineage(
    ShapePtr shape, std::shared_ptr<const TopologyLineage> lineage) noexcept {
  shape_ = std::move(shape);
  topologyLineage_ = std::move(lineage);
  topologyLineageAttempted_ = true;
}

void ShapeFeature::clearShape() noexcept {
  shape_.reset();
  topologyLineage_.reset();
  topologyLineageAttempted_ = false;
}

} // namespace solidar
