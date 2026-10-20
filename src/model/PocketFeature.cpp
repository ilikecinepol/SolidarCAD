#include "model/PocketFeature.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopExp_Explorer.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <utility>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/OcctBooleanTopologyHistoryCapture.h"
#include "model/SketchExtrudeBuilder.h"
#include "model/SketchProfileBuilder.h"
#include "model/TopologyLineage.h"

namespace solidar {
namespace {

std::size_t solidCount(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next())
    ++count;
  return count;
}

bool containsFace(const TopoDS_Shape& shape, const TopoDS_Shape& face) {
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next())
    if (faces.Current().IsSame(face)) return true;
  return false;
}

}  // namespace

PocketFeature::PocketFeature(SketchId profileSketchId, double depthMm,
                             std::string name)
    : ShapeFeature(name.empty() ? "Pocket" : std::move(name)),
      profileSketchId_(profileSketchId), depthMm_(depthMm) {}

PocketFeature::PocketFeature(FeatureId id, SketchId profileSketchId,
                             double depthMm, std::string name)
    : ShapeFeature(id, std::move(name)), profileSketchId_(profileSketchId),
      depthMm_(depthMm) {}

SketchId PocketFeature::profileSketchId() const noexcept {
  return profileSketchId_;
}
double PocketFeature::depthMm() const noexcept { return depthMm_; }
void PocketFeature::setProfileSketchId(SketchId id) noexcept {
  if (profileSketchId_ == id) return;
  profileSketchId_ = id;
  setDirty();
}
void PocketFeature::setDepthMm(double value) noexcept {
  if (depthMm_ == value) return;
  depthMm_ = value;
  setDirty();
}

FeatureDependencies PocketFeature::dependencies() const {
  FeatureDependencies result;
  if (profileSketchId_ != kInvalidSketchId)
    result.sketchIds.push_back(profileSketchId_);
  return result;
}

bool PocketFeature::rebuildImpl(const RebuildContext& context) {
  clearShape();
  if (!std::isfinite(depthMm_) || depthMm_ <= 0.0) {
    markError("Pocket depth must be a finite positive value");
    return false;
  }
  if (!context.previousShape || context.previousShape->IsNull()) {
    markError("Pocket base shape is missing");
    return false;
  }
  const auto* profile = context.document.findSketch(profileSketchId_);
  if (!profile) {
    markError("Pocket profile sketch was not found");
    return false;
  }

  const ShapeFeature* baseFeature = context.previousFeature;
  const auto baseLineage =
      baseFeature ? baseFeature->topologyLineage()
                  : std::shared_ptr<const TopologyLineage>{};
  if (baseLineage &&
      baseLineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
    markError("Pocket base face lineage is incomplete");
    return false;
  }

  bool useStrictLineage = false;
  if (baseLineage) {
    if (!baseFeature->shape() ||
        !baseFeature->shape()->IsSame(*context.previousShape)) {
      markError("Pocket base snapshot is incoherent");
      return false;
    }
    useStrictLineage = isSupportedSingleSketchProfile(*profile);
    // Eligibility only selects the strict lineage path. If it is not selected,
    // the exact historical Pocket builder below remains the sole validator and
    // diagnostic source for valid-out-of-scope and malformed profiles alike.
  }

  std::string profileError;
  TopoDS_Shape prismShape;
  std::shared_ptr<const TopologyLineage> toolLineage;
  if (useStrictLineage) {
    SketchExtrusionWithLineage tool;
    if (!buildSimpleNewBodyExtrusionWithLineage(
            *profile, id(), depthMm_, true, &tool, &profileError) ||
        !tool.shape || !tool.lineage ||
        tool.lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
      markError("Pocket " +
                (profileError.empty() ? "tool face lineage is incomplete"
                                      : profileError));
      return false;
    }
    prismShape = *tool.shape;
    toolLineage = std::move(tool.lineage);
  } else {
    if (!buildExtrusionPrismFromSketch(*profile, depthMm_, true, &prismShape,
                                       &profileError)) {
      markError("Pocket " + profileError);
      return false;
    }
  }

  try {
    BRepAlgoAPI_Cut cut(*context.previousShape, prismShape);
    cut.Build();
    if (!cut.IsDone() || cut.Shape().IsNull()) {
      markError("Pocket boolean cut failed");
      return false;
    }
    GProp_GProps beforeProperties;
    GProp_GProps afterProperties;
    BRepGProp::VolumeProperties(*context.previousShape, beforeProperties);
    BRepGProp::VolumeProperties(cut.Shape(), afterProperties);
    const double before = beforeProperties.Mass();
    const double tolerance = std::max(1e-7, std::abs(before) * 1e-10);
    if (afterProperties.Mass() >= before - tolerance) {
      markError("Pocket profile does not intersect the body");
      return false;
    }
    TopoDS_Shape result = cut.Shape();
    const std::size_t beforeSolidCount = solidCount(*context.previousShape);
    const std::size_t resultSolidCount = solidCount(result);
    if (resultSolidCount == 0) {
      markError("Pocket result does not contain a solid");
      return false;
    }
    if (resultSolidCount > beforeSolidCount) {
      markError("Pocket would split the body");
      return false;
    }
    BRepCheck_Analyzer analyzer(result);
    if (!analyzer.IsValid()) {
      markError("Pocket result is invalid");
      return false;
    }

    if (useStrictLineage) {
      const std::array<BooleanTopologySource, 2> sources{{
          {BooleanOperandRole::Object,
           {baseFeature->id(), baseFeature->shape(), baseLineage}},
          {BooleanOperandRole::Tool,
           {id(), std::make_shared<TopoDS_Shape>(prismShape), toolLineage}},
      }};
      BooleanTopologyCaptureOptions options;
      options.simplifyResult = false;
      BooleanTopologyCaptureFailure captureFailure;
      auto captured = captureBooleanFaceHistory(
          cut, id(), std::span<const BooleanTopologySource>(sources), {},
          options, &captureFailure);
      if (!captured) {
        markError(captureFailure.detail.empty()
                      ? "Pocket face history capture failed"
                      : std::move(captureFailure.detail));
        return false;
      }
      auto lineage = captured->builder.build(&profileError);
      if (!lineage ||
          lineage->coverage() != TopologyLineageCoverage::CompleteFaces) {
        markError(profileError.empty() ? "Pocket face lineage build failed"
                                       : std::move(profileError));
        return false;
      }

      ShapeFeature::ShapePtr committedShape = captured->outputShape;
      // Preserve the historical single-solid root without changing topology:
      // every captured face must remain the exact IsSame subshape of the
      // extracted solid. Otherwise fail instead of publishing incoherent
      // shape/lineage snapshots.
      if (resultSolidCount == 1 &&
          committedShape->ShapeType() != TopAbs_SOLID) {
        TopExp_Explorer solids(*committedShape, TopAbs_SOLID);
        if (!solids.More()) {
          markError("Pocket result solid could not be normalized");
          return false;
        }
        const TopoDS_Shape normalized = solids.Current();
        if (std::any_of(lineage->bindings().begin(), lineage->bindings().end(),
                        [&](const auto& binding) {
                          return !binding.subshape || binding.subshape->IsNull() ||
                                 !containsFace(normalized, *binding.subshape);
                        })) {
          markError("Pocket result normalization changed captured faces");
          return false;
        }
        committedShape = std::make_shared<TopoDS_Shape>(normalized);
      }
      FaceBoundaryEdgeProvenanceFailure edgeFailure;
      const auto augmented =
          augmentCompleteFaceLineageWithUniqueManifoldEdges(
              {id(), committedShape, lineage}, &edgeFailure);
      if (!augmented) {
        markError("Pocket " +
                  (edgeFailure.detail.empty()
                       ? std::string("edge lineage is invalid")
                       : std::move(edgeFailure.detail)));
        return false;
      }
      setShapeAndTopologyLineage(
          committedShape, augmented->augmentedSource.lineage);
      markValid();
      return true;
    }

    // Preserve multi-solid containers.  Keep the historical single-solid
    // normalization so existing callers still receive a TopAbs_SOLID root.
    if (resultSolidCount == 1 && result.ShapeType() != TopAbs_SOLID) {
      TopExp_Explorer solids(result, TopAbs_SOLID);
      result = solids.Current();
    }
    setShape(std::make_shared<TopoDS_Shape>(result));
    markValid();
    return true;
  } catch (const Standard_Failure& failure) {
    const char* message = failure.what();
    markError(message && *message ? std::string("OCCT Pocket error: ") + message
                                  : "OCCT Pocket operation failed");
    return false;
  } catch (...) {
    markError("Unexpected Pocket geometry error");
    return false;
  }
}

std::unique_ptr<Feature> PocketFeature::clone() const {
  return std::make_unique<PocketFeature>(*this);
}

}  // namespace solidar
