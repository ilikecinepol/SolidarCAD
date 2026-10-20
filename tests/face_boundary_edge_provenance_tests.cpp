#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <NCollection_IndexedMap.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "model/FaceBoundaryEdgeProvenance.h"
#include "model/TopologyReferenceResolver.h"
#include "model/TransformFeatureHistoryBuilder.h"

namespace {

constexpr solidar::BodyId kBodyId = 4101;
using ShapeIndexedMap =
    NCollection_IndexedMap<TopoDS_Shape, TopTools_ShapeMapHasher>;

std::string semanticFaceName(const TopoDS_Face& face) {
  BRepAdaptor_Surface surface(face);
  if (surface.GetType() == GeomAbs_Cylinder) return "cylinder-side";
  CHECK(surface.GetType() == GeomAbs_Plane);
  gp_Dir direction = surface.Plane().Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED) direction.Reverse();
  const double x = direction.X();
  const double y = direction.Y();
  const double z = direction.Z();
  if (std::abs(x) >= std::abs(y) && std::abs(x) >= std::abs(z))
    return x >= 0.0 ? "x+" : "x-";
  if (std::abs(y) >= std::abs(z)) return y >= 0.0 ? "y+" : "y-";
  return z >= 0.0 ? "z+" : "z-";
}

solidar::TopologyProvenanceKey faceKey(solidar::FeatureId featureId,
                                       std::string name) {
  return {featureId, solidar::TopologyKind::Face,
          solidar::TopologyProvenanceOperation::Generated,
          solidar::TopologySemanticRole::Original, std::move(name)};
}

solidar::TopologySourceSnapshot semanticSource(const TopoDS_Shape& shape,
                                               solidar::FeatureId featureId) {
  auto ownedShape = std::make_shared<TopoDS_Shape>(shape);
  ShapeIndexedMap faces;
  TopExp::MapShapes(*ownedShape, TopAbs_FACE, faces);
  std::vector<solidar::TopologyLineageBinding> bindings;
  bindings.reserve(static_cast<std::size_t>(faces.Extent()));
  for (int index = 1; index <= faces.Extent(); ++index) {
    const TopoDS_Face face = TopoDS::Face(faces(index));
    bindings.push_back(
        {faceKey(featureId, semanticFaceName(face)),
         std::make_shared<TopoDS_Face>(face)});
  }
  std::string error;
  auto lineage = solidar::TopologyLineage::create(
      std::move(bindings), solidar::TopologyLineageCoverage::CompleteFaces,
      &error);
  CHECK(lineage);
  CHECK(error.empty());
  return {featureId, std::move(ownedShape), std::move(lineage)};
}

solidar::TopologySourceSnapshot namedSource(
    const TopoDS_Shape& shape, solidar::FeatureId featureId,
    const std::vector<std::string>& names) {
  auto ownedShape = std::make_shared<TopoDS_Shape>(shape);
  ShapeIndexedMap faces;
  TopExp::MapShapes(*ownedShape, TopAbs_FACE, faces);
  CHECK(static_cast<std::size_t>(faces.Extent()) == names.size());
  std::vector<solidar::TopologyLineageBinding> bindings;
  for (int index = 1; index <= faces.Extent(); ++index)
    bindings.push_back(
        {faceKey(featureId, names[static_cast<std::size_t>(index - 1)]),
         std::make_shared<TopoDS_Face>(TopoDS::Face(faces(index)))});
  std::string error;
  auto lineage = solidar::TopologyLineage::create(
      std::move(bindings), solidar::TopologyLineageCoverage::CompleteFaces,
      &error);
  CHECK(lineage);
  return {featureId, std::move(ownedShape), std::move(lineage)};
}

solidar::FaceReference referenceFor(
    const solidar::TopologySourceSnapshot& source, std::string_view name) {
  const auto found = std::find_if(
      source.lineage->bindings().begin(), source.lineage->bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Face &&
               binding.provenance.localName == name;
      });
  CHECK(found != source.lineage->bindings().end());
  return solidar::FaceReference::fromV2(
      {kBodyId, found->provenance});
}

const solidar::TopologyProvenanceKey& faceKeyFor(
    const solidar::TopologySourceSnapshot& source, std::string_view name) {
  const auto found = std::find_if(
      source.lineage->bindings().begin(), source.lineage->bindings().end(),
      [&](const auto& binding) {
        return binding.provenance.kind == solidar::TopologyKind::Face &&
               binding.provenance.localName == name;
      });
  CHECK(found != source.lineage->bindings().end());
  return found->provenance;
}

bool originLess(const solidar::TopologyProvenanceOrigin& left,
                const solidar::TopologyProvenanceOrigin& right) {
  const auto occurrenceTuple = [](const auto& origin) {
    return std::tuple{origin.occurrence.has_value(),
                      origin.occurrence
                          ? origin.occurrence->featureId
                          : solidar::kInvalidFeatureId,
                      origin.occurrence ? origin.occurrence->slot : 0};
  };
  const auto leftIdentity = std::tie(left.featureId, left.kind,
                                     left.operation, left.role,
                                     left.localName);
  const auto rightIdentity = std::tie(right.featureId, right.kind,
                                      right.operation, right.role,
                                      right.localName);
  if (leftIdentity != rightIdentity) return leftIdentity < rightIdentity;
  return occurrenceTuple(left) < occurrenceTuple(right);
}

void appendLengthPrefixed(std::string& output, std::string_view value) {
  output += std::to_string(value.size());
  output.push_back(':');
  output.append(value);
}

std::string independentlyEncodedOrigin(
    const solidar::TopologyProvenanceOrigin& origin) {
  std::string result;
  appendLengthPrefixed(result, std::to_string(origin.featureId));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.kind)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.operation)));
  appendLengthPrefixed(
      result, std::to_string(static_cast<unsigned>(origin.role)));
  appendLengthPrefixed(result, origin.localName);
  appendLengthPrefixed(result, origin.occurrence ? "1" : "0");
  if (origin.occurrence) {
    appendLengthPrefixed(result,
                         std::to_string(origin.occurrence->featureId));
    appendLengthPrefixed(result, std::to_string(origin.occurrence->slot));
  }
  return result;
}

std::string independentlyEncodedEdgeName(
    std::vector<solidar::TopologyProvenanceOrigin> origins) {
  CHECK(origins.size() == 2);
  std::sort(origins.begin(), origins.end(), originLess);
  std::string result = "face-boundary-edge:v1;";
  for (const auto& origin : origins)
    appendLengthPrefixed(result, independentlyEncodedOrigin(origin));
  return result;
}

solidar::TopologyProvenanceKey expectedEdgeKey(
    solidar::FeatureId producer,
    const solidar::TopologyProvenanceKey& firstFace,
    const solidar::TopologyProvenanceKey& secondFace) {
  std::vector<solidar::TopologyProvenanceOrigin> origins{
      firstFace.immediateOrigin(), secondFace.immediateOrigin()};
  const std::string localName = independentlyEncodedEdgeName(origins);
  return {producer, solidar::TopologyKind::Edge,
          solidar::TopologyProvenanceOperation::Generated,
          solidar::TopologySemanticRole::Custom, localName,
          std::move(origins)};
}

std::vector<solidar::TopologyProvenanceKey> expectedBoundaryKeys(
    solidar::FeatureId producer,
    const solidar::TopologyProvenanceKey& selected,
    std::span<const solidar::TopologyProvenanceKey* const> adjacent) {
  std::vector<solidar::TopologyProvenanceKey> result;
  for (const auto* face : adjacent)
    result.push_back(expectedEdgeKey(producer, selected, *face));
  std::sort(result.begin(), result.end(), [](const auto& left,
                                             const auto& right) {
    return left.localName < right.localName;
  });
  return result;
}

std::vector<solidar::TopologyProvenanceKey> seedKeys(
    const solidar::FaceBoundaryEdgeProvenanceResult& result);

void checkExactBoundaryKeys(
    const solidar::FaceBoundaryEdgeProvenanceResult& result,
    const std::vector<solidar::TopologyProvenanceKey>& expected) {
  CHECK(seedKeys(result) == expected);
  for (const auto& seed : result.boundarySeeds) {
    CHECK(seed.provenance.localName ==
          independentlyEncodedEdgeName(seed.provenance.origins));
    CHECK(seed.provenance.localName.find("body=") == std::string::npos);
    CHECK(seed.provenance.localName.find("ordinal=") == std::string::npos);
    CHECK(seed.provenance.localName.find("index=") == std::string::npos);
    CHECK(seed.provenance.localName.find("signature") == std::string::npos);
    CHECK(seed.provenance.localName.find("orientation=") ==
          std::string::npos);
  }
}

std::string boxTopEdgeToken(const TopoDS_Edge& edge, double xLength,
                            double yLength, double zHeight,
                            gp_Vec translation = {}) {
  GProp_GProps properties;
  BRepGProp::LinearProperties(edge, properties);
  const gp_Pnt center = properties.CentreOfMass();
  const double x = center.X() - translation.X();
  const double y = center.Y() - translation.Y();
  const double z = center.Z() - translation.Z();
  CHECK(std::abs(z - zHeight) < 1e-7);
  if (std::abs(x) < 1e-7) return "x-";
  if (std::abs(x - xLength) < 1e-7) return "x+";
  if (std::abs(y) < 1e-7) return "y-";
  CHECK(std::abs(y - yLength) < 1e-7);
  return "y+";
}

std::vector<std::pair<std::string, std::string>> keyToBoxEdgeMapping(
    const solidar::FaceBoundaryEdgeProvenanceResult& result,
    double xLength, double yLength, double zHeight,
    gp_Vec translation = {}) {
  std::vector<std::pair<std::string, std::string>> mapping;
  for (const auto& seed : result.boundarySeeds)
    mapping.emplace_back(seed.provenance.localName,
                         boxTopEdgeToken(*seed.edge, xLength, yLength,
                                         zHeight, translation));
  std::sort(mapping.begin(), mapping.end());
  return mapping;
}

std::vector<solidar::TopologyProvenanceKey> seedKeys(
    const solidar::FaceBoundaryEdgeProvenanceResult& result) {
  std::vector<solidar::TopologyProvenanceKey> keys;
  for (const auto& seed : result.boundarySeeds) keys.push_back(seed.provenance);
  std::sort(keys.begin(), keys.end(), [](const auto& left, const auto& right) {
    return left.localName < right.localName;
  });
  return keys;
}

std::optional<solidar::FaceBoundaryEdgeProvenanceResult> capture(
    const solidar::TopologySourceSnapshot& source,
    const solidar::FaceReference& reference,
    solidar::FaceBoundaryEdgeProvenanceFailureCode expected =
        solidar::FaceBoundaryEdgeProvenanceFailureCode::None) {
  solidar::FaceBoundaryEdgeProvenanceFailure failure;
  auto result = solidar::captureUniqueManifoldFaceBoundary(
      source, reference, &failure);
  if (failure.code != expected)
    std::cerr << "capture failure mismatch: expected="
              << static_cast<unsigned>(expected) << " actual="
              << static_cast<unsigned>(failure.code) << " detail="
              << failure.detail << '\n';
  CHECK(failure.code == expected);
  if (expected == solidar::FaceBoundaryEdgeProvenanceFailureCode::None) {
    CHECK(result);
    CHECK(failure.detail.empty());
  } else {
    CHECK(!result);
    CHECK(!failure.detail.empty());
  }
  return result;
}

TopoDS_Shape coincidentFaces(std::size_t count) {
  BRepBuilderAPI_MakePolygon polygon;
  polygon.Add(gp_Pnt(0.0, 0.0, 0.0));
  polygon.Add(gp_Pnt(10.0, 0.0, 0.0));
  polygon.Add(gp_Pnt(10.0, 10.0, 0.0));
  polygon.Add(gp_Pnt(0.0, 10.0, 0.0));
  polygon.Close();
  CHECK(polygon.IsDone());
  const auto wire = polygon.Wire();
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  for (std::size_t index = 0; index < count; ++index) {
    BRepBuilderAPI_MakeFace maker(wire);
    CHECK(maker.IsDone());
    builder.Add(compound, maker.Face());
  }
  return compound;
}

solidar::TopologySourceSnapshot lineageVariant(
    const solidar::TopologySourceSnapshot& source,
    std::vector<solidar::TopologyLineageBinding> bindings,
    solidar::TopologyLineageCoverage coverage) {
  std::string error;
  auto lineage = solidar::TopologyLineage::create(
      std::move(bindings), coverage, &error);
  CHECK(lineage);
  return {source.featureId, source.shape, std::move(lineage)};
}

}  // namespace

int main() {
  constexpr solidar::FeatureId kBoxFeature = 4201;
  const auto boxSource = semanticSource(
      BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape(), kBoxFeature);
  const auto boxTop = referenceFor(boxSource, "z+");
  const auto originalLineage = boxSource.lineage;
  const std::size_t originalBindingCount = originalLineage->bindings().size();
  auto boxResult = capture(boxSource, boxTop);
  CHECK(boxResult->boundarySeeds.size() == 4);
  const std::array<const solidar::TopologyProvenanceKey*, 4> boxAdjacent{
      &faceKeyFor(boxSource, "x-"), &faceKeyFor(boxSource, "x+"),
      &faceKeyFor(boxSource, "y-"), &faceKeyFor(boxSource, "y+")};
  const auto expectedBoxKeys = expectedBoundaryKeys(
      kBoxFeature, faceKeyFor(boxSource, "z+"), boxAdjacent);
  checkExactBoundaryKeys(*boxResult, expectedBoxKeys);
  const auto boxKeyToEdge =
      keyToBoxEdgeMapping(*boxResult, 10.0, 20.0, 30.0);
  CHECK(boxResult->augmentedSource.shape == boxSource.shape);
  CHECK(boxResult->augmentedSource.lineage->coverage() ==
        solidar::TopologyLineageCoverage::CompleteFaces);
  CHECK(boxResult->augmentedSource.lineage->bindings().size() ==
        originalBindingCount + 4);
  CHECK(boxSource.lineage == originalLineage);
  CHECK(boxSource.lineage->bindings().size() == originalBindingCount);

  std::string indexError;
  const auto augmentedIndex = solidar::TopologyIndex::build(
      boxResult->augmentedSource.shape, 1,
      boxResult->augmentedSource.lineage, &indexError);
  CHECK(augmentedIndex);
  for (const auto& seed : boxResult->boundarySeeds) {
    CHECK(seed.edge);
    CHECK(!seed.edge->IsNull());
    CHECK(seed.edge->Orientation() == TopAbs_FORWARD ||
          seed.edge->Orientation() == TopAbs_REVERSED);
    CHECK(seed.provenance.featureId == kBoxFeature);
    CHECK(seed.provenance.kind == solidar::TopologyKind::Edge);
    CHECK(seed.provenance.operation ==
          solidar::TopologyProvenanceOperation::Generated);
    CHECK(seed.provenance.role == solidar::TopologySemanticRole::Custom);
    CHECK(seed.provenance.origins.size() == 2);
    CHECK(!seed.provenance.occurrence);
    CHECK(seed.origin == seed.provenance.immediateOrigin());
    const auto resolved = augmentedIndex->resolveEdge(
        solidar::TopologyReferenceV2{kBodyId, seed.provenance});
    CHECK(resolved);
    CHECK(resolved.method == solidar::TopologyMatchMethod::OperationProvenance);
    CHECK(resolved.subshape->IsSame(*seed.edge));
  }
  auto missingEdgeKey = boxResult->boundarySeeds.front().provenance;
  missingEdgeKey.localName += ":missing";
  const auto missingEdge = augmentedIndex->resolveEdge(
      solidar::TopologyReferenceV2{kBodyId, missingEdgeKey});
  CHECK(!missingEdge);
  CHECK(missingEdge.failure == solidar::TopologyResolutionFailure::Missing);

  // Recompute with changed dimensions preserves identities because neither
  // geometry nor traversal order participates in the derived key.
  const auto resizedSource = semanticSource(
      BRepPrimAPI_MakeBox(14.0, 23.0, 41.0).Shape(), kBoxFeature);
  auto resizedResult = capture(resizedSource, referenceFor(resizedSource, "z+"));
  const std::array<const solidar::TopologyProvenanceKey*, 4> resizedAdjacent{
      &faceKeyFor(resizedSource, "x-"), &faceKeyFor(resizedSource, "x+"),
      &faceKeyFor(resizedSource, "y-"), &faceKeyFor(resizedSource, "y+")};
  checkExactBoundaryKeys(
      *resizedResult,
      expectedBoundaryKeys(kBoxFeature, faceKeyFor(resizedSource, "z+"),
                           resizedAdjacent));
  CHECK(seedKeys(*resizedResult) == expectedBoxKeys);

  // Reordering lineage evidence and using reversed IsSame aliases does not
  // affect the canonical pair identity.
  auto permutedBindings = boxSource.lineage->bindings();
  std::reverse(permutedBindings.begin(), permutedBindings.end());
  for (auto& binding : permutedBindings) {
    if (binding.provenance.kind == solidar::TopologyKind::Face)
      binding.subshape =
          std::make_shared<TopoDS_Shape>(binding.subshape->Reversed());
  }
  const auto permutedSource = lineageVariant(
      boxSource, std::move(permutedBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  auto permutedResult = capture(permutedSource, referenceFor(permutedSource, "z+"));
  checkExactBoundaryKeys(*permutedResult, expectedBoxKeys);
  CHECK(keyToBoxEdgeMapping(*permutedResult, 10.0, 20.0, 30.0) ==
        boxKeyToEdge);

  // A transformed source keeps the same face keys across parameter edits;
  // therefore the boundary keys remain stable as well.
  const auto transformedCapture = [&](double distance) {
    gp_Trsf transform;
    transform.SetTranslation(gp_Vec(distance, -distance * 0.5, distance));
    const solidar::TransformFeatureInstanceRequest instance{
        solidar::TransformFeatureInstanceRelation::ModifiedCopy, transform,
        solidar::TopologySemanticRole::Original, std::nullopt};
    solidar::TransformFeatureHistoryBuild transformed;
    std::string error;
    CHECK(solidar::buildTransformFeatureHistory(
        boxSource, 4250, std::span(&instance, 1), true, &transformed, &error));
    solidar::TopologySourceSnapshot transformedSource{
        4250, transformed.shape, transformed.lineage};
    const auto transformedFaceKey = [&](std::string_view name) {
      const auto key = solidar::deriveTransformFaceProvenance(
          faceKeyFor(boxSource, name).immediateOrigin(), 4250,
          solidar::TopologyProvenanceOperation::Modified,
          solidar::TopologySemanticRole::Original, {}, &error);
      CHECK(key);
      CHECK(!key->occurrence);
      return *key;
    };
    const auto transformedTop = transformedFaceKey("z+");
    const std::array<solidar::TopologyProvenanceKey, 4> adjacentValues{
        transformedFaceKey("x-"), transformedFaceKey("x+"),
        transformedFaceKey("y-"), transformedFaceKey("y+")};
    const std::array<const solidar::TopologyProvenanceKey*, 4>
        transformedAdjacent{&adjacentValues[0], &adjacentValues[1],
                            &adjacentValues[2], &adjacentValues[3]};
    auto result = capture(
        transformedSource,
        solidar::FaceReference::fromV2({kBodyId, transformedTop}));
    checkExactBoundaryKeys(
        *result,
        expectedBoundaryKeys(4250, transformedTop, transformedAdjacent));
    for (const auto& seed : result->boundarySeeds) {
      CHECK(!seed.provenance.occurrence);
      CHECK(std::all_of(seed.provenance.origins.begin(),
                        seed.provenance.origins.end(), [](const auto& origin) {
                          return !origin.occurrence;
                        }));
    }
    return result;
  };
  auto translatedFive = transformedCapture(5.0);
  auto translatedFifty = transformedCapture(50.0);
  CHECK(seedKeys(*translatedFive) == seedKeys(*translatedFifty));
  CHECK(keyToBoxEdgeMapping(*translatedFive, 10.0, 20.0, 30.0,
                            gp_Vec(5.0, -2.5, 5.0)) ==
        keyToBoxEdgeMapping(*translatedFifty, 10.0, 20.0, 30.0,
                            gp_Vec(50.0, -25.0, 50.0)));

  constexpr solidar::FeatureId kCylinderFeature = 4301;
  const auto cylinderSource = semanticSource(
      BRepPrimAPI_MakeCylinder(5.0, 12.0).Shape(), kCylinderFeature);
  auto capResult = capture(cylinderSource, referenceFor(cylinderSource, "z+"));
  CHECK(capResult->boundarySeeds.size() == 1);
  const std::array<const solidar::TopologyProvenanceKey*, 1> capAdjacent{
      &faceKeyFor(cylinderSource, "cylinder-side")};
  checkExactBoundaryKeys(
      *capResult,
      expectedBoundaryKeys(kCylinderFeature,
                           faceKeyFor(cylinderSource, "z+"), capAdjacent));
  capture(cylinderSource, referenceFor(cylinderSource, "cylinder-side"),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::InvalidBoundaryEdge);

  // A lone face is open; two coincident faces share several edges and violate
  // the injective face-pair mapping; three are non-manifold.
  const auto openSource = namedSource(coincidentFaces(1), 4401, {"open"});
  capture(openSource, referenceFor(openSource, "open"),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::OpenBoundaryEdge);
  const auto repeatedPairSource =
      namedSource(coincidentFaces(2), 4402, {"front", "back"});
  capture(repeatedPairSource, referenceFor(repeatedPairSource, "front"),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::RepeatedFacePair);
  solidar::FaceBoundaryEdgeProvenanceFailure augmentationFailure;
  const auto repeatedPairAugmentation =
      solidar::augmentCompleteFaceLineageWithUniqueManifoldEdges(
          repeatedPairSource, &augmentationFailure);
  CHECK(repeatedPairAugmentation);
  CHECK(augmentationFailure.code ==
        solidar::FaceBoundaryEdgeProvenanceFailureCode::None);
  CHECK(repeatedPairAugmentation->addedBindingCount == 0);

  ShapeIndexedMap repeatedEdges;
  TopExp::MapShapes(*repeatedPairSource.shape, TopAbs_EDGE, repeatedEdges);
  CHECK(repeatedEdges.Extent() == 4);
  auto preboundRepeatedPairBindings = repeatedPairSource.lineage->bindings();
  preboundRepeatedPairBindings.push_back(
      {expectedEdgeKey(4402, faceKeyFor(repeatedPairSource, "front"),
                       faceKeyFor(repeatedPairSource, "back")),
       std::make_shared<TopoDS_Edge>(TopoDS::Edge(repeatedEdges(1)))});
  const auto preboundRepeatedPairSource = lineageVariant(
      repeatedPairSource, std::move(preboundRepeatedPairBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  const auto preboundLineage = preboundRepeatedPairSource.lineage;
  CHECK(!solidar::augmentCompleteFaceLineageWithUniqueManifoldEdges(
      preboundRepeatedPairSource, &augmentationFailure));
  CHECK(augmentationFailure.code ==
        solidar::FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity);
  CHECK(preboundRepeatedPairSource.lineage == preboundLineage);
  CHECK(preboundRepeatedPairSource.lineage->bindings().size() == 3);
  const auto nonManifoldSource = namedSource(
      coincidentFaces(3), 4403, {"first", "second", "third"});
  capture(nonManifoldSource, referenceFor(nonManifoldSource, "first"),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::NonManifoldBoundaryEdge);

  // Typed source/reference failures and atomic input preservation.
  const auto partialSource = lineageVariant(
      boxSource, boxSource.lineage->bindings(),
      solidar::TopologyLineageCoverage::PartialFaces);
  capture(partialSource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::IncompleteFaceCoverage);

  auto missingBindings = boxSource.lineage->bindings();
  missingBindings.pop_back();
  const auto missingIdentitySource = lineageVariant(
      boxSource, std::move(missingBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  capture(missingIdentitySource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::MissingFaceIdentity);

  auto multipleBindings = boxSource.lineage->bindings();
  auto extra = multipleBindings.front();
  extra.provenance.localName += ":second";
  multipleBindings.push_back(std::move(extra));
  const auto multipleIdentitySource = lineageVariant(
      boxSource, std::move(multipleBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  capture(multipleIdentitySource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::AmbiguousFaceIdentity);

  auto duplicateKeyBindings = boxSource.lineage->bindings();
  CHECK(duplicateKeyBindings.size() >= 2);
  duplicateKeyBindings[1].provenance = duplicateKeyBindings[0].provenance;
  const auto duplicateKeySource = lineageVariant(
      boxSource, std::move(duplicateKeyBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  capture(duplicateKeySource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::AmbiguousFaceIdentity);

  auto foreignBindings = boxSource.lineage->bindings();
  foreignBindings.front().provenance.featureId = 999999;
  const auto foreignIdentitySource = lineageVariant(
      boxSource, std::move(foreignBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  capture(foreignIdentitySource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::ForeignFaceIdentity);

  auto missingFaceKey = *boxTop.v2();
  missingFaceKey.provenance.localName += ":absent";
  capture(boxSource, solidar::FaceReference::fromV2(missingFaceKey),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::MissingSelectedFace);
  capture(boxSource, solidar::FaceReference(kBodyId, kBoxFeature, 0),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::InvalidSelectedFace);
  auto foreignReference = *boxTop.v2();
  foreignReference.provenance.featureId = 999998;
  capture(boxSource, solidar::FaceReference::fromV2(foreignReference),
          solidar::FaceBoundaryEdgeProvenanceFailureCode::ForeignSelectedFace);

  // Exact existing bindings are idempotently reused. A conflicting key on
  // the same canonical edge fails without changing either input lineage.
  const std::size_t augmentedCount =
      boxResult->augmentedSource.lineage->bindings().size();
  auto reused = capture(boxResult->augmentedSource, boxTop);
  CHECK(reused->augmentedSource.lineage->bindings().size() == augmentedCount);
  checkExactBoundaryKeys(*reused, expectedBoxKeys);
  for (const auto& originalSeed : boxResult->boundarySeeds) {
    const auto reusedSeed = std::find_if(
        reused->boundarySeeds.begin(), reused->boundarySeeds.end(),
        [&](const auto& candidate) {
          return candidate.provenance == originalSeed.provenance;
        });
    CHECK(reusedSeed != reused->boundarySeeds.end());
    CHECK(reusedSeed->edge->IsSame(*originalSeed.edge));
  }
  for (const auto& originalBinding :
       boxResult->augmentedSource.lineage->bindings()) {
    const auto reusedBinding = std::find_if(
        reused->augmentedSource.lineage->bindings().begin(),
        reused->augmentedSource.lineage->bindings().end(),
        [&](const auto& candidate) {
          return candidate.provenance == originalBinding.provenance &&
                 candidate.subshape == originalBinding.subshape;
        });
    CHECK(reusedBinding !=
          reused->augmentedSource.lineage->bindings().end());
  }
  auto conflictingBindings = boxSource.lineage->bindings();
  const auto& firstSeed = boxResult->boundarySeeds.front();
  auto conflictingKey = firstSeed.provenance;
  conflictingKey.localName += ":conflict";
  conflictingBindings.push_back(
      {std::move(conflictingKey),
       std::make_shared<TopoDS_Edge>(*firstSeed.edge)});
  const auto conflictSource = lineageVariant(
      boxSource, std::move(conflictingBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  const auto conflictLineage = conflictSource.lineage;
  capture(conflictSource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity);
  CHECK(conflictSource.lineage == conflictLineage);
  CHECK(conflictSource.lineage->bindings().size() == originalBindingCount + 1);

  auto inverseConflictBindings = boxSource.lineage->bindings();
  CHECK(boxResult->boundarySeeds.size() >= 2);
  inverseConflictBindings.push_back(
      {boxResult->boundarySeeds[0].provenance,
       std::make_shared<TopoDS_Edge>(
           *boxResult->boundarySeeds[1].edge)});
  const auto inverseConflictSource = lineageVariant(
      boxSource, std::move(inverseConflictBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  capture(inverseConflictSource, boxTop,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::ConflictingEdgeIdentity);

  // The length bound is checked before publishing any derived binding.
  auto oversizedBindings = boxSource.lineage->bindings();
  auto selectedBinding = std::find_if(
      oversizedBindings.begin(), oversizedBindings.end(), [](const auto& item) {
        return item.provenance.localName == "z+";
      });
  CHECK(selectedBinding != oversizedBindings.end());
  selectedBinding->provenance.localName = std::string(20000, 'x');
  const auto oversizedSource = lineageVariant(
      boxSource, std::move(oversizedBindings),
      solidar::TopologyLineageCoverage::CompleteFaces);
  const auto oversizedReference = solidar::FaceReference::fromV2(
      {kBodyId, std::find_if(oversizedSource.lineage->bindings().begin(),
                             oversizedSource.lineage->bindings().end(),
                             [](const auto& item) {
                               return item.provenance.localName.size() == 20000;
                             })
                    ->provenance});
  capture(oversizedSource, oversizedReference,
          solidar::FaceBoundaryEdgeProvenanceFailureCode::ResourceLimit);

  return EXIT_SUCCESS;
}
