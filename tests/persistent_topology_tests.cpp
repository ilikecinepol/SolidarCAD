#include "TestAssertions.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRep_Builder.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/SketchPlacement.h"
#include "model/TopologyReferenceResolver.h"

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

double dot(solidar::TopologyPoint3d a, solidar::TopologyPoint3d b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace

int main() {
  CHECK(solidar::operationFailureCode(
            solidar::TopologyResolutionFailure::None) ==
        solidar::OperationFailureCode::None);
  CHECK(solidar::operationFailureCode(
            solidar::TopologyResolutionFailure::InvalidReference) ==
        solidar::OperationFailureCode::TopologyReferenceInvalid);
  CHECK(solidar::operationFailureCode(
            solidar::TopologyResolutionFailure::Missing) ==
        solidar::OperationFailureCode::TopologyReferenceMissing);
  CHECK(solidar::operationFailureCode(
            solidar::TopologyResolutionFailure::Ambiguous) ==
        solidar::OperationFailureCode::TopologyReferenceAmbiguous);
  CHECK(solidar::operationFailureCode(
            solidar::TopologyResolutionFailure::Unexpected) ==
        solidar::OperationFailureCode::TopologyResolutionUnexpected);
  try {
    constexpr solidar::BodyId bodyId = 101;
    constexpr solidar::FeatureId featureId = 202;
    const TopoDS_Shape original = BRepPrimAPI_MakeBox(80.0, 35.0, 50.0).Shape();
    const TopoDS_Shape resized = BRepPrimAPI_MakeBox(95.0, 35.0, 60.0).Shape();
    std::string indexError;
    const auto originalIndex =
        solidar::TopologyIndex::build(original, 777, &indexError);
    CHECK(originalIndex != nullptr);
    CHECK(indexError.empty());
    CHECK(originalIndex->revision() == 777);
    CHECK(originalIndex->shape() != nullptr);
    CHECK(originalIndex->faceCount() > 0);
    CHECK(originalIndex->edgeCount() > 0);

    // A raw traversal can contain the same underlying TShape more than once
    // (here with opposite orientations). Both ordinals must canonicalize to
    // one IsSame edge identity instead of becoming distinct references.
    {
      const TopoDS_Edge sharedEdge = BRepBuilderAPI_MakeEdge(
          gp_Pnt(0.0, 0.0, 0.0), gp_Pnt(12.0, 0.0, 0.0));
      TopoDS_Compound duplicateCompound;
      BRep_Builder builder;
      builder.MakeCompound(duplicateCompound);
      builder.Add(duplicateCompound, sharedEdge);
      builder.Add(duplicateCompound, sharedEdge.Reversed());

      std::vector<TopoDS_Edge> rawEdges;
      for (TopExp_Explorer explorer(duplicateCompound, TopAbs_EDGE);
           explorer.More(); explorer.Next())
        rawEdges.push_back(TopoDS::Edge(explorer.Current()));
      CHECK(rawEdges.size() == 2);
      CHECK(rawEdges[0].IsSame(rawEdges[1]));

      const auto duplicateIndex =
          solidar::TopologyIndex::build(duplicateCompound);
      CHECK(duplicateIndex != nullptr);
      CHECK(duplicateIndex->rawEdgeCount() == 2);
      CHECK(duplicateIndex->edgeCount() == 1);

      const auto duplicateOrdinal = solidar::makeEdgeReference(
          duplicateCompound, bodyId, featureId, 1);
      CHECK(duplicateOrdinal.signature);
      CHECK(duplicateOrdinal.edgeIndex == 0);
      const auto duplicateResolution = solidar::resolveEdgeReference(
          duplicateCompound, duplicateOrdinal.topology());
      CHECK(duplicateResolution);
      CHECK(duplicateResolution.index == 0);
      CHECK(duplicateResolution.subshape &&
            duplicateResolution.subshape->IsSame(rawEdges[0]));
    }

    // A semantic planar extremum identifies the logical top face even when
    // dimensions and the legacy enumeration are no longer the identity.
    std::optional<solidar::FaceReference> top;
    for (std::size_t index = 0; index < 16; ++index) {
      auto reference = solidar::makeFaceReference(
          original, bodyId, featureId, index);
      if (reference.persistentTag == "planar:max-z") {
        top = std::move(reference);
        break;
      }
    }
    CHECK(top && top->signature);
    const auto resolvedTop =
        solidar::resolveFaceReference(resized, top->topology());
    CHECK(resolvedTop);
    CHECK(resolvedTop.method == solidar::TopologyMatchMethod::SemanticTag);
    const auto placement =
        solidar::resolveFacePlacement(resized, resolvedTop.index);
    CHECK(placement.planar);
    CHECK(placement.placement.normal().z > 0.99);
    CHECK(std::abs(placement.placement.origin.z - 60.0) < 1e-6);

    // A persisted signature is the identity evidence. A stale semantic tag
    // may narrow the search only when the tagged face is compatible with that
    // signature; it must not override the globally compatible face.
    std::optional<solidar::FaceReference> maxX;
    for (std::size_t index = 0; index < 16; ++index) {
      auto reference = solidar::makeFaceReference(
          original, bodyId, featureId, index);
      if (reference.persistentTag == "planar:max-x") {
        maxX = std::move(reference);
        break;
      }
    }
    CHECK(maxX);
    auto conflictingTag = *top;
    conflictingTag.persistentTag = maxX->persistentTag;
    const auto resolvedConflictingTag =
        solidar::resolveFaceReference(original, conflictingTag.topology());
    CHECK(resolvedConflictingTag);
    CHECK(resolvedConflictingTag.method ==
          solidar::TopologyMatchMethod::GeometricSignature);
    const solidar::FaceReference legacyTop{bodyId, featureId, top->faceIndex};
    const auto expectedTop =
        solidar::resolveFaceReference(original, legacyTop.topology());
    CHECK(expectedTop && expectedTop.subshape &&
          resolvedConflictingTag.subshape);
    CHECK(resolvedConflictingTag.subshape->IsSame(*expectedTop.subshape));

    // A tag-only native reference must fail closed when its tag disappeared.
    // Only genuinely legacy references (no tag and no signature) may use the
    // raw traversal ordinal fallback.
    solidar::FaceReference missingTagOnly{bodyId, featureId, top->faceIndex};
    missingTagOnly.persistentTag = "planar:missing-regression-tag";
    const auto missingTagOnlyResolution =
        solidar::resolveFaceReference(original, missingTagOnly.topology());
    CHECK(!missingTagOnlyResolution);
    CHECK(missingTagOnlyResolution.method == solidar::TopologyMatchMethod::None);

    // A geometric edge signature follows a distinctive vertical corner edge
    // through an ordinary upstream dimension edit.
    std::optional<solidar::EdgeReference> vertical;
    for (std::size_t index = 0; index < 32; ++index) {
      auto reference = solidar::makeEdgeReference(
          original, bodyId, featureId, index);
      if (!reference.signature) break;
      const auto& signature = *reference.signature;
      if (std::abs(signature.tangent.z) > 0.99 &&
          signature.midpoint.x < 1e-6 && signature.midpoint.y < 1e-6) {
        vertical = std::move(reference);
        break;
      }
    }
    CHECK(vertical && vertical->signature);
    const auto resolvedVertical =
        solidar::resolveEdgeReference(resized, vertical->topology());
    CHECK(resolvedVertical);
    CHECK(resolvedVertical.method ==
          solidar::TopologyMatchMethod::GeometricSignature);

    // Old v2 references contain only an index and must retain their fallback.
    solidar::EdgeReference legacy{bodyId, featureId, 0};
    const auto resolvedLegacy =
        solidar::resolveEdgeReference(original, legacy.topology());
    CHECK(resolvedLegacy);
    CHECK(resolvedLegacy.method == solidar::TopologyMatchMethod::LegacyIndex);

    // Batch resolution preserves input order and is behaviorally identical to
    // scalar resolution against the same immutable snapshot.
    {
      const std::vector<solidar::TopologyReference> faceReferences{
          top->topology(), missingTagOnly.topology(), legacyTop.topology()};
      const auto batch =
          solidar::resolveFaceReferences(*originalIndex, faceReferences);
      CHECK(batch.size() == faceReferences.size());
      for (std::size_t index = 0; index < faceReferences.size(); ++index) {
        const auto scalar = originalIndex->resolveFace(faceReferences[index]);
        CHECK(bool(batch[index]) == bool(scalar));
        CHECK(batch[index].index == scalar.index);
        CHECK(batch[index].method == scalar.method);
        CHECK(batch[index].error == scalar.error);
        CHECK(!batch[index].subshape ||
              batch[index].subshape->IsSame(*scalar.subshape));
      }

      const std::vector<solidar::TopologyReference> edgeReferences{
          vertical->topology(), legacy.topology()};
      const auto batchEdges = originalIndex->resolveEdges(edgeReferences);
      CHECK(batchEdges.size() == edgeReferences.size());
      for (std::size_t index = 0; index < edgeReferences.size(); ++index) {
        const auto scalar = originalIndex->resolveEdge(edgeReferences[index]);
        CHECK(bool(batchEdges[index]) == bool(scalar));
        CHECK(batchEdges[index].index == scalar.index);
        CHECK(batchEdges[index].method == scalar.method);
        CHECK(batchEdges[index].error == scalar.error);
        CHECK(!batchEdges[index].subshape ||
              batchEdges[index].subshape->IsSame(*scalar.subshape));
      }
    }

    // Reference factories are all-or-nothing. Invalid inputs must never leak
    // an owner-filled, index-only object that later masquerades as a legacy
    // reference.
    {
      const TopoDS_Shape nullShape;
      CHECK(solidar::makeFaceReference(nullShape, bodyId, featureId, 0) ==
            solidar::FaceReference{});
      CHECK(solidar::makeEdgeReference(nullShape, bodyId, featureId, 0) ==
            solidar::EdgeReference{});
      CHECK(solidar::makeFaceReference(original, bodyId, featureId, 9999) ==
            solidar::FaceReference{});
      CHECK(solidar::makeEdgeReference(original, bodyId, featureId, 9999) ==
            solidar::EdgeReference{});
      CHECK(solidar::makeFaceReference(original, solidar::kInvalidBodyId,
                                       featureId, top->faceIndex) ==
            solidar::FaceReference{});
      CHECK(solidar::makeEdgeReference(original, bodyId,
                                       solidar::kInvalidFeatureId, 0) ==
            solidar::EdgeReference{});

      const auto invalidOwner = originalIndex->createFaceReference(
          solidar::kInvalidBodyId, featureId, top->faceIndex);
      CHECK(!invalidOwner);
      CHECK(invalidOwner.reference == solidar::FaceReference{});
      CHECK(!invalidOwner.error.empty());
      const auto outOfRange = originalIndex->createEdgeReference(
          bodyId, featureId, originalIndex->rawEdgeCount());
      CHECK(!outOfRange);
      CHECK(outOfRange.reference == solidar::EdgeReference{});
      CHECK(outOfRange.error.find("out of range") != std::string::npos);

      const auto strictFace = originalIndex->createFaceReference(
          bodyId, featureId, top->faceIndex);
      CHECK(strictFace);
      CHECK(strictFace.error.empty());
      CHECK(strictFace.reference == solidar::makeFaceReference(
                                          original, bodyId, featureId,
                                          top->faceIndex));

      indexError.clear();
      const auto nullIndex = solidar::TopologyIndex::build(
          TopoDS_Shape{}, 778, &indexError);
      CHECK(nullIndex == nullptr);
      CHECK(!indexError.empty());
    }

    // Boundary values are accepted, while an arbitrarily small excursion
    // past each centralized tolerance fails closed.
    {
      const double shapeDiagonal =
          std::hypot(std::hypot(80.0, 35.0), 50.0);

      auto measureBoundary = vertical->topology();
      measureBoundary.edgeSignature->length *=
          1.0 - solidar::kTopologyMeasureRelativeTolerance;
      CHECK(originalIndex->resolveEdge(measureBoundary));
      measureBoundary.edgeSignature->length =
          vertical->signature->length *
          (1.0 - solidar::kTopologyMeasureRelativeTolerance - 1e-6);
      CHECK(!originalIndex->resolveEdge(measureBoundary));

      auto directionBoundary = vertical->topology();
      const double directionSign =
          std::copysign(1.0, vertical->signature->tangent.z);
      const double acceptedCosine =
          solidar::kTopologyDirectionCosineTolerance;
      directionBoundary.edgeSignature->tangent = {
          std::sqrt(1.0 - acceptedCosine * acceptedCosine), 0.0,
          directionSign * acceptedCosine};
      CHECK(originalIndex->resolveEdge(directionBoundary));
      const double rejectedCosine = acceptedCosine - 1e-4;
      directionBoundary.edgeSignature->tangent = {
          std::sqrt(1.0 - rejectedCosine * rejectedCosine), 0.0,
          directionSign * rejectedCosine};
      CHECK(!originalIndex->resolveEdge(directionBoundary));

      auto positionBoundary = vertical->topology();
      positionBoundary.edgeSignature->midpoint.x +=
          shapeDiagonal * solidar::kTopologyPositionRelativeTolerance;
      CHECK(originalIndex->resolveEdge(positionBoundary));
      positionBoundary.edgeSignature->midpoint =
          vertical->signature->midpoint;
      positionBoundary.edgeSignature->midpoint.x +=
          shapeDiagonal *
          (solidar::kTopologyPositionRelativeTolerance + 1e-6);
      CHECK(!originalIndex->resolveEdge(positionBoundary));
    }

    // Persisted non-finite input is rejected with a stable diagnostic before
    // any scoring or fallback can take place.
    {
      auto nonFiniteFace = top->topology();
      nonFiniteFace.faceSignature->centroid.x =
          std::numeric_limits<double>::quiet_NaN();
      const auto faceFailure = originalIndex->resolveFace(nonFiniteFace);
      CHECK(!faceFailure);
      CHECK(faceFailure.failure ==
            solidar::TopologyResolutionFailure::InvalidReference);
      CHECK(faceFailure.error.find("non-finite") != std::string::npos);

      auto nonFiniteEdge = vertical->topology();
      nonFiniteEdge.edgeSignature->length =
          std::numeric_limits<double>::infinity();
      const auto edgeFailure = originalIndex->resolveEdge(nonFiniteEdge);
      CHECK(!edgeFailure);
      CHECK(edgeFailure.failure ==
            solidar::TopologyResolutionFailure::InvalidReference);
      CHECK(edgeFailure.error.find("non-finite") != std::string::npos);
    }

    // Put a synthetic signature exactly between two equivalent parallel box
    // edges. The resolver must reject the tie instead of selecting iteration
    // order. This exercises safety independently of a particular edge index.
    std::vector<solidar::EdgeReference> edges;
    for (std::size_t index = 0; index < 32; ++index) {
      auto reference = solidar::makeEdgeReference(
          original, bodyId, featureId, index);
      if (!reference.signature) break;
      edges.push_back(std::move(reference));
    }
    std::optional<std::pair<std::size_t, std::size_t>> equivalent;
    for (std::size_t first = 0; first < edges.size() && !equivalent; ++first)
      for (std::size_t second = first + 1; second < edges.size(); ++second) {
        const auto& a = *edges[first].signature;
        const auto& b = *edges[second].signature;
        if (a.curve == b.curve &&
            std::abs(a.length - b.length) < 1e-7 &&
            std::abs(dot(a.tangent, b.tangent)) > 0.999) {
          equivalent = {{first, second}};
          break;
        }
      }
    CHECK(equivalent);
    auto ambiguous = edges[equivalent->first];
    const auto& other = *edges[equivalent->second].signature;
    ambiguous.signature->midpoint = {
        (ambiguous.signature->midpoint.x + other.midpoint.x) * 0.5,
        (ambiguous.signature->midpoint.y + other.midpoint.y) * 0.5,
        (ambiguous.signature->midpoint.z + other.midpoint.z) * 0.5};
    ambiguous.signature->bounds.minimum = {
        (ambiguous.signature->bounds.minimum.x + other.bounds.minimum.x) * 0.5,
        (ambiguous.signature->bounds.minimum.y + other.bounds.minimum.y) * 0.5,
        (ambiguous.signature->bounds.minimum.z + other.bounds.minimum.z) * 0.5};
    ambiguous.signature->bounds.maximum = {
        (ambiguous.signature->bounds.maximum.x + other.bounds.maximum.x) * 0.5,
        (ambiguous.signature->bounds.maximum.y + other.bounds.maximum.y) * 0.5,
        (ambiguous.signature->bounds.maximum.z + other.bounds.maximum.z) * 0.5};
    const auto ambiguity =
        solidar::resolveEdgeReference(original, ambiguous.topology());
    CHECK(!ambiguity);
    CHECK(ambiguity.failure ==
          solidar::TopologyResolutionFailure::Ambiguous);
    CHECK(ambiguity.error.find("ambiguous") != std::string::npos);

    auto impossible = vertical->topology();
    impossible.edgeSignature->midpoint = {10000.0, 10000.0, 10000.0};
    const auto missing = solidar::resolveEdgeReference(original, impossible);
    CHECK(!missing);
    CHECK(missing.failure == solidar::TopologyResolutionFailure::Missing);
    CHECK(missing.error.find("no longer") != std::string::npos);

    // Face disambiguation: two coplanar max-z faces share one auto-tag after a
    // boolean fuse. A FaceSignature must select the correct tagged candidate
    // (GeometricSignature), not fail with "ambiguous by semantic tag".
    {
      const TopoDS_Shape boxA = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
      const TopoDS_Shape boxB =
          BRepPrimAPI_MakeBox(gp_Pnt(10.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
      BRepAlgoAPI_Fuse fuse(boxA, boxB);
      fuse.Build();
      CHECK(fuse.IsDone());
      const TopoDS_Shape fused = fuse.Shape();

      std::vector<solidar::FaceReference> tops;
      for (std::size_t index = 0; index < 32; ++index) {
        auto reference = solidar::makeFaceReference(fused, bodyId, featureId,
                                                    index);
        if (reference.persistentTag == "planar:max-z" && reference.signature)
          tops.push_back(std::move(reference));
      }
      CHECK(tops.size() == 2);

      // Unique best: the saved signature resolves to its own face.
      const solidar::FaceReference& saved = tops[0];
      const auto resolved =
          solidar::resolveFaceReference(fused, saved.topology());
      CHECK(resolved);
      CHECK(resolved.method ==
            solidar::TopologyMatchMethod::GeometricSignature);
      CHECK(resolved.subshape);
      GProp_GProps properties;
      BRepGProp::SurfaceProperties(*resolved.subshape, properties);
      CHECK(std::abs(properties.CentreOfMass().X() -
                     saved.signature->centroid.x) < 1e-3);

      // True symmetric ambiguity: a midpoint signature scores both equally.
      solidar::FaceReference ambiguous = tops[0];
      ambiguous.signature->centroid.x =
          (tops[0].signature->centroid.x + tops[1].signature->centroid.x) * 0.5;
      ambiguous.signature->bounds.minimum.x =
          (tops[0].signature->bounds.minimum.x +
           tops[1].signature->bounds.minimum.x) * 0.5;
      ambiguous.signature->bounds.maximum.x =
          (tops[0].signature->bounds.maximum.x +
           tops[1].signature->bounds.maximum.x) * 0.5;
      const auto ambiguity =
          solidar::resolveFaceReference(fused, ambiguous.topology());
      CHECK(!ambiguity);
      CHECK(ambiguity.failure ==
            solidar::TopologyResolutionFailure::Ambiguous);
      CHECK(ambiguity.error.find("ambiguous") != std::string::npos);

      // Duplicate tag without a signature keeps the hard semantic-tag failure.
      solidar::FaceReference noSignature{bodyId, featureId, 0};
      noSignature.persistentTag = "planar:max-z";
      const auto noSignatureResolved =
          solidar::resolveFaceReference(fused, noSignature.topology());
      CHECK(!noSignatureResolved);
      CHECK(noSignatureResolved.failure ==
            solidar::TopologyResolutionFailure::Ambiguous);
      CHECK(noSignatureResolved.error.find("ambiguous by semantic tag") !=
            std::string::npos);
    }

    // A unique semantic tag is only a candidate locator. Full signature
    // compatibility, including bounds, remains mandatory at the exact
    // tolerance boundary and may fall through to a global signature search.
    {
      const TopoDS_Shape cube = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
      std::optional<solidar::FaceReference> cubeTop;
      for (std::size_t index = 0; index < 16; ++index) {
        auto reference = solidar::makeFaceReference(cube, bodyId, featureId,
                                                    index);
        if (reference.persistentTag == "planar:max-z") {
          cubeTop = std::move(reference);
          break;
        }
      }
      CHECK(cubeTop && cubeTop->signature);
      const double diagonal = std::sqrt(300.0);
      const double insideShift =
          diagonal * (solidar::kTopologyPositionRelativeTolerance - 0.005);
      const double outsideShift =
          diagonal * (solidar::kTopologyPositionRelativeTolerance + 0.005);
      const TopoDS_Shape inside = BRepPrimAPI_MakeBox(
          gp_Pnt(insideShift, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
      const TopoDS_Shape outside = BRepPrimAPI_MakeBox(
          gp_Pnt(outsideShift, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
      const auto insideResolution =
          solidar::resolveFaceReference(inside, cubeTop->topology());
      CHECK(insideResolution);
      CHECK(insideResolution.method ==
            solidar::TopologyMatchMethod::SemanticTag);
      const auto outsideResolution =
          solidar::resolveFaceReference(outside, cubeTop->topology());
      CHECK(!outsideResolution);
      CHECK(outsideResolution.method == solidar::TopologyMatchMethod::None);
      CHECK(outsideResolution.error.find("no longer") != std::string::npos);

      auto boundsInside = cubeTop->topology();
      const double insideExtent =
          10.0 / (1.0 - solidar::kTopologyMeasureRelativeTolerance + 0.005);
      boundsInside.faceSignature->bounds.maximum.x = insideExtent;
      CHECK(solidar::resolveFaceReference(cube, boundsInside));
      auto boundsOutside = cubeTop->topology();
      const double outsideExtent =
          10.0 / (1.0 - solidar::kTopologyMeasureRelativeTolerance - 0.005);
      boundsOutside.faceSignature->bounds.maximum.x = outsideExtent;
      const auto boundsFailure =
          solidar::resolveFaceReference(cube, boundsOutside);
      CHECK(!boundsFailure);
      CHECK(boundsFailure.method == solidar::TopologyMatchMethod::None);
    }

    // Split and merge transitions invalidate the saved geometric identity.
    // Native references must fail closed and never fall back to raw ordinal.
    {
      const TopoDS_Shape beforeSplit =
          BRepPrimAPI_MakeBox(20.0, 10.0, 10.0).Shape();
      std::optional<solidar::FaceReference> splitTop;
      for (std::size_t index = 0; index < 16; ++index) {
        auto reference = solidar::makeFaceReference(
            beforeSplit, bodyId, featureId, index);
        if (reference.persistentTag == "planar:max-z") {
          splitTop = std::move(reference);
          break;
        }
      }
      CHECK(splitTop && splitTop->signature);
      BRepAlgoAPI_Cut split(
          beforeSplit,
          BRepPrimAPI_MakeBox(gp_Pnt(9.0, -1.0, 5.0), 2.0, 12.0, 6.0)
              .Shape());
      split.Build();
      CHECK(split.IsDone());
      const auto splitResolution =
          solidar::resolveFaceReference(split.Shape(), splitTop->topology());
      CHECK(!splitResolution);
      CHECK(splitResolution.method == solidar::TopologyMatchMethod::None);
      CHECK(splitResolution.error.find("no longer") != std::string::npos ||
            splitResolution.error.find("ambiguous") != std::string::npos);

      const TopoDS_Shape left =
          BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
      const TopoDS_Shape right = BRepPrimAPI_MakeBox(
          gp_Pnt(10.0, 0.0, 0.0), 10.0, 10.0, 10.0).Shape();
      BRepAlgoAPI_Fuse beforeMergeBuilder(left, right);
      beforeMergeBuilder.Build();
      CHECK(beforeMergeBuilder.IsDone());
      std::optional<solidar::FaceReference> mergeTop;
      for (std::size_t index = 0; index < 32; ++index) {
        auto reference = solidar::makeFaceReference(
            beforeMergeBuilder.Shape(), bodyId, featureId, index);
        if (reference.persistentTag == "planar:max-z" &&
            reference.signature && reference.signature->area < 150.0) {
          mergeTop = std::move(reference);
          break;
        }
      }
      CHECK(mergeTop && mergeTop->signature);
      const TopoDS_Shape afterMerge =
          BRepPrimAPI_MakeBox(20.0, 10.0, 10.0).Shape();
      const auto mergeResolution =
          solidar::resolveFaceReference(afterMerge, mergeTop->topology());
      CHECK(!mergeResolution);
      CHECK(mergeResolution.method == solidar::TopologyMatchMethod::None);
      CHECK(mergeResolution.error.find("no longer") != std::string::npos);
    }
  } catch (const std::exception& error) {
    std::cerr << "persistent topology regression failure: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
