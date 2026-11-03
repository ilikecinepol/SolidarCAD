#include "TestAssertions.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/AssemblyMate.h"
#include "model/Body.h"
#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/PlanarMateSolver.h"
#include "model/ShapeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "sketch/Sketch.h"

namespace solidar {
class AssemblyMateDependencyTestAccess final {
 public:
  static bool setPlacement(AssemblyDefinition& assembly, OccurrenceId id,
                           RigidTransform3d placement) noexcept {
    return assembly.setPlacement(id, std::move(placement));
  }
};
}  // namespace solidar

namespace {
using namespace solidar;

constexpr DefinitionId kRoot{1};
constexpr DefinitionId kPart{2};
constexpr OccurrenceId kFixed{10};
constexpr OccurrenceId kMoving{20};

OccurrencePath path(OccurrenceId id) {
  auto made = OccurrencePath::create({id});
  CHECK(made);
  return std::move(*made.path);
}

OccurrencePath path(std::initializer_list<OccurrenceId> ids) {
  auto made = OccurrencePath::create(std::vector<OccurrenceId>(ids));
  CHECK(made);
  return std::move(*made.path);
}

std::array<double, 3> rotateVector(
    const std::array<double, 4>& quaternion,
    const std::array<double, 3>& value) {
  const std::array<double, 3> qv{quaternion[1], quaternion[2], quaternion[3]};
  const std::array<double, 3> cross{
      qv[1] * value[2] - qv[2] * value[1],
      qv[2] * value[0] - qv[0] * value[2],
      qv[0] * value[1] - qv[1] * value[0]};
  const std::array<double, 3> twice{2.0 * cross[0], 2.0 * cross[1],
                                    2.0 * cross[2]};
  const std::array<double, 3> secondCross{
      qv[1] * twice[2] - qv[2] * twice[1],
      qv[2] * twice[0] - qv[0] * twice[2],
      qv[0] * twice[1] - qv[1] * twice[0]};
  return {value[0] + quaternion[0] * twice[0] + secondCross[0],
          value[1] + quaternion[0] * twice[1] + secondCross[1],
          value[2] + quaternion[0] * twice[2] + secondCross[2]};
}

std::array<double, 3> transformPoint(
    const RigidTransform3d& transform, const std::array<double, 3>& point) {
  auto result = rotateVector(transform.quaternionWxyz(), point);
  for (std::size_t index = 0; index < 3; ++index)
    result[index] += transform.translationMm()[index];
  return result;
}

double dot3(const std::array<double, 3>& left,
            const std::array<double, 3>& right) {
  return left[0] * right[0] + left[1] * right[1] +
         left[2] * right[2];
}

double dot4(const std::array<double, 4>& left,
            const std::array<double, 4>& right) {
  return left[0] * right[0] + left[1] * right[1] +
         left[2] * right[2] + left[3] * right[3];
}

struct FaceFrameSample final {
  std::array<double, 3> point;
  std::array<double, 3> normal;
};

struct AxisFrameSample final {
  std::array<double, 3> point;
  std::array<double, 3> direction;
};

AxisFrameSample localAxisFrame(const Document& document,
                               const MateAxisReference& reference) {
  if (const auto* datum = std::get_if<LocalDatumAxis>(&reference)) {
    CHECK(isValidLocalDatumAxis(*datum));
    return {{0.0, 0.0, 0.0},
            *datum == LocalDatumAxis::X
                ? std::array{1.0, 0.0, 0.0}
                : (*datum == LocalDatumAxis::Y
                       ? std::array{0.0, 1.0, 0.0}
                       : std::array{0.0, 0.0, 1.0})};
  }
  const auto& face = std::get<CylindricalFaceAxis>(reference).face;
  const auto* v2 = face.v2();
  CHECK(v2);
  const auto* body = document.findBody(v2->bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(v2->provenance.featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  const auto resolved = snapshot->topologyIndex()->resolveFace(face);
  CHECK(resolved);
  BRepAdaptor_Surface surface(*resolved.subshape, true);
  CHECK(surface.GetType() == GeomAbs_Cylinder);
  const auto cylinder = surface.Cylinder();
  std::array<double, 3> direction{cylinder.Axis().Direction().X(),
                                  cylinder.Axis().Direction().Y(),
                                  cylinder.Axis().Direction().Z()};
  for (double component : direction) {
    if (std::abs(component) <= 1.0e-15) continue;
    if (component < 0.0)
      for (double& value : direction) value = -value;
    break;
  }
  std::array<double, 3> point{cylinder.Location().X(),
                              cylinder.Location().Y(),
                              cylinder.Location().Z()};
  const double along = dot3(point, direction);
  for (std::size_t index = 0; index < 3; ++index)
    point[index] -= along * direction[index];
  return {point, direction};
}

AxisFrameSample transformAxisFrame(const AxisFrameSample& local,
                                   const RigidTransform3d& transform) {
  return {transformPoint(transform, local.point),
          rotateVector(transform.quaternionWxyz(), local.direction)};
}

double infiniteLineDistance(const AxisFrameSample& first,
                            const AxisFrameSample& second) {
  const std::array<double, 3> crossDirection{
      first.direction[1] * second.direction[2] -
          first.direction[2] * second.direction[1],
      first.direction[2] * second.direction[0] -
          first.direction[0] * second.direction[2],
      first.direction[0] * second.direction[1] -
          first.direction[1] * second.direction[0]};
  const double crossLength = std::sqrt(dot3(crossDirection, crossDirection));
  const std::array<double, 3> delta{
      second.point[0] - first.point[0], second.point[1] - first.point[1],
      second.point[2] - first.point[2]};
  if (crossLength > 1.0e-12)
    return std::abs(dot3(delta, crossDirection)) / crossLength;
  const std::array<double, 3> transverse{
      delta[1] * first.direction[2] - delta[2] * first.direction[1],
      delta[2] * first.direction[0] - delta[0] * first.direction[2],
      delta[0] * first.direction[1] - delta[1] * first.direction[0]};
  return std::sqrt(dot3(transverse, transverse));
}

FaceFrameSample localFaceFrame(const Document& document,
                               const FaceReference& reference) {
  const auto* v2 = reference.v2();
  CHECK(v2);
  const auto* body = document.findBody(v2->bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(v2->provenance.featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  const auto resolved = snapshot->topologyIndex()->resolveFace(reference);
  CHECK(resolved);
  BRepAdaptor_Surface surface(*resolved.subshape, true);
  CHECK(surface.GetType() == GeomAbs_Plane);
  const auto plane = surface.Plane();
  std::array<double, 3> normal{plane.Axis().Direction().X(),
                               plane.Axis().Direction().Y(),
                               plane.Axis().Direction().Z()};
  if (resolved.subshape->Orientation() == TopAbs_REVERSED)
    for (double& value : normal) value = -value;
  return {{plane.Location().X(), plane.Location().Y(), plane.Location().Z()},
          normal};
}

FaceFrameSample transformFaceFrame(const FaceFrameSample& local,
                                   const RigidTransform3d& transform) {
  return {transformPoint(transform, local.point),
          rotateVector(transform.quaternionWxyz(), local.normal)};
}

double pointDistance(const std::array<double, 3>& left,
                     const std::array<double, 3>& right) {
  double squared = 0.0;
  for (std::size_t index = 0; index < 3; ++index) {
    const double delta = left[index] - right[index];
    squared += delta * delta;
  }
  return std::sqrt(squared);
}

struct PartFixture final {
  std::unique_ptr<Document> document;
  FaceReference face;
};

PartFixture makePart(double scale = 1.0) {
  constexpr SketchId sketchId{1101};
  constexpr BodyId bodyId{1102};
  constexpr FeatureId featureId{1103};
  auto document = std::make_unique<Document>();
  sketch::Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {10.0 * scale, 8.0 * scale});
  document->addSketch(sketchId, "mate profile", std::move(sketch));
  auto& body = document->addBody(bodyId, "mate body");
  body.addFeature(std::make_unique<ExtrudeFeature>(
      featureId, sketchId, 5.0 * scale, "mate extrusion"));
  CHECK(document->recompute());
  std::optional<FaceReference> face;
  for (std::size_t index = 0; index < 32; ++index) {
    auto made = document->createFaceReferenceV2(bodyId, featureId, index);
    if (made) {
      face.emplace(std::move(made.reference));
      break;
    }
  }
  CHECK(face && face->isV2());
  return {std::move(document), std::move(*face)};
}

PartFixture makeFarLeverPart(double scale) {
  constexpr BodyId bodyId{1102};
  constexpr FeatureId featureId{1103};
  auto part = makePart(scale);
  const auto* body = part.document->findBody(bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  double farthest = -1.0;
  for (std::size_t index = 0; index < 32; ++index) {
    auto made = part.document->createFaceReferenceV2(bodyId, featureId, index);
    if (!made) continue;
    const auto resolved = snapshot->topologyIndex()->resolveFace(made.reference);
    if (!resolved) continue;
    BRepAdaptor_Surface surface(*resolved.subshape, true);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto location = surface.Plane().Location();
    const double magnitude = std::sqrt(location.X() * location.X() +
                                       location.Y() * location.Y() +
                                       location.Z() * location.Z());
    if (magnitude > farthest) {
      farthest = magnitude;
      part.face = std::move(made.reference);
    }
  }
  CHECK(farthest > 1.0e10);
  return part;
}

PartFixture makeCylinderPart() {
  constexpr SketchId sketchId{2101};
  constexpr BodyId bodyId{2102};
  constexpr FeatureId featureId{2103};
  auto document = std::make_unique<Document>();
  sketch::Sketch sketch;
  sketch.addCircle({0.0, 0.0}, 5.0);
  document->addSketch(sketchId, "cylinder profile", std::move(sketch));
  auto& body = document->addBody(bodyId, "cylinder body");
  body.addFeature(std::make_unique<ExtrudeFeature>(featureId, sketchId, 8.0,
                                                   "cylinder extrusion"));
  CHECK(document->recompute());
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  std::optional<FaceReference> cylindrical;
  for (std::size_t index = 0; index < 16; ++index) {
    auto made = document->createFaceReferenceV2(bodyId, featureId, index);
    if (!made) continue;
    const auto resolved = snapshot->topologyIndex()->resolveFace(made.reference);
    if (!resolved) continue;
    if (BRepAdaptor_Surface(*resolved.subshape, true).GetType() ==
        GeomAbs_Cylinder) {
      cylindrical.emplace(std::move(made.reference));
      break;
    }
  }
  CHECK(cylindrical);
  return {std::move(document), std::move(*cylindrical)};
}

EdgeReference circularEdgeOfCylinder(Document& document) {
  constexpr BodyId bodyId{2102};
  constexpr FeatureId featureId{2103};
  const auto* body = document.findBody(bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  auto* feature = dynamic_cast<ShapeFeature*>(
      document.findFeature(bodyId, featureId));
  CHECK(feature);
  for (std::size_t index = 0;
       index < snapshot->topologyIndex()->rawEdgeCount();
       ++index) {
    auto made = feature->createEdgeReferenceV2(bodyId, index);
    if (!made) continue;
    const auto resolved =
        snapshot->topologyIndex()->resolveEdge(made.reference);
    if (!resolved) continue;
    if (BRepAdaptor_Curve(*resolved.subshape).GetType() == GeomAbs_Circle)
      return std::move(made.reference);
  }
  CHECK(false);
  return {};
}

FaceReference planarFaceOfCylinder(const Document& document) {
  constexpr BodyId bodyId{2102};
  constexpr FeatureId featureId{2103};
  const auto* body = document.findBody(bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  for (std::size_t index = 0; index < 16; ++index) {
    auto made = document.createFaceReferenceV2(bodyId, featureId, index);
    if (!made) continue;
    const auto resolved = snapshot->topologyIndex()->resolveFace(made.reference);
    if (!resolved) continue;
    if (BRepAdaptor_Surface(*resolved.subshape, true).GetType() ==
        GeomAbs_Plane)
      return std::move(made.reference);
  }
  CHECK(false);
  return {};
}

class AmbiguousPlanarFeature final : public ShapeFeature {
 public:
  explicit AmbiguousPlanarFeature(FeatureId id)
      : ShapeFeature(id, "ambiguous planar source") {}

  [[nodiscard]] const TopologyProvenanceKey& ambiguousFaceKey() const {
    return ambiguousFaceKey_;
  }
  [[nodiscard]] const TopologyProvenanceKey& validFaceKey() const {
    return validFaceKey_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<AmbiguousPlanarFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 9.0, 8.0).Shape());
    ambiguousFaceKey_ = {
        id(), TopologyKind::Face, TopologyProvenanceOperation::Generated,
        TopologySemanticRole::Custom, "ambiguous-planar-face"};
    std::vector<TopologyLineageBinding> bindings;
    std::size_t faceIndex = 0;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next(), ++faceIndex) {
      auto key = ambiguousFaceKey_;
      if (faceIndex >= 2) key.localName += ":" + std::to_string(faceIndex);
      if (faceIndex == 2) validFaceKey_ = key;
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(faces.Current())});
    }
    CHECK(bindings.size() >= 2);
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  TopologyProvenanceKey ambiguousFaceKey_;
  TopologyProvenanceKey validFaceKey_;
};

struct AmbiguousPartFixture final {
  std::unique_ptr<Document> document;
  FaceReference ambiguousFace;
  FaceReference validFace;
};

class ManyEdgeCircularFeature final : public ShapeFeature {
 public:
  explicit ManyEdgeCircularFeature(FeatureId id,
                                   std::size_t cylinderCount = 24)
      : ShapeFeature(id, "many-edge circular source"),
        cylinderCount_(cylinderCount) {}

  [[nodiscard]] const TopologyProvenanceKey& circularEdgeKey() const {
    return circularEdgeKey_;
  }

  [[nodiscard]] const TopologyProvenanceKey& planarFaceKey() const {
    return planarFaceKey_;
  }

  [[nodiscard]] const TopologyProvenanceKey& cylindricalFaceKey() const {
    return cylindricalFaceKey_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<ManyEdgeCircularFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (std::size_t index = 0; index < cylinderCount_; ++index) {
      builder.Add(
          compound,
          BRepPrimAPI_MakeCylinder(
              gp_Ax2(gp_Pnt(static_cast<double>(index) * 20.0, 0.0, 0.0),
                     gp_Dir(0.0, 0.0, 1.0)),
              5.0, 8.0)
              .Shape());
    }
    auto shape = std::make_shared<TopoDS_Shape>(compound);
    std::vector<TopologyLineageBinding> bindings;
    std::vector<TopoDS_Shape> canonicalFaces;
    std::vector<TopoDS_Shape> canonicalEdges;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next()) {
      const auto candidate = faces.Current();
      if (std::any_of(canonicalFaces.begin(), canonicalFaces.end(),
                      [&](const TopoDS_Shape& existing) {
                        return existing.IsSame(candidate);
                      }))
        continue;
      const auto ordinal = canonicalFaces.size();
      canonicalFaces.push_back(candidate);
      TopologyProvenanceKey key{
          id(), TopologyKind::Face,
          TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Custom,
          "many-edge-face:" + std::to_string(ordinal)};
      if (planarFaceKey_.featureId == kInvalidFeatureId &&
          BRepAdaptor_Surface(TopoDS::Face(candidate)).GetType() ==
              GeomAbs_Plane)
        planarFaceKey_ = key;
      if (cylindricalFaceKey_.featureId == kInvalidFeatureId &&
          BRepAdaptor_Surface(TopoDS::Face(candidate)).GetType() ==
              GeomAbs_Cylinder)
        cylindricalFaceKey_ = key;
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(candidate)});
    }
    for (TopExp_Explorer edges(*shape, TopAbs_EDGE); edges.More();
         edges.Next()) {
      const auto candidate = edges.Current();
      if (std::any_of(canonicalEdges.begin(), canonicalEdges.end(),
                      [&](const TopoDS_Shape& existing) {
                        return existing.IsSame(candidate);
                      }))
        continue;
      const auto ordinal = canonicalEdges.size();
      canonicalEdges.push_back(candidate);
      TopologyProvenanceKey key{
          id(), TopologyKind::Edge, TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Custom,
          "many-edge-edge:" + std::to_string(ordinal)};
      if (circularEdgeKey_.featureId == kInvalidFeatureId &&
          BRepAdaptor_Curve(TopoDS::Edge(candidate)).GetType() ==
              GeomAbs_Circle)
        circularEdgeKey_ = key;
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(candidate)});
    }
    CHECK(canonicalEdges.size() >= cylinderCount_ * 3);
    CHECK(planarFaceKey_.featureId == id());
    CHECK(cylindricalFaceKey_.featureId == id());
    CHECK(circularEdgeKey_.featureId == id());
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  TopologyProvenanceKey planarFaceKey_;
  TopologyProvenanceKey cylindricalFaceKey_;
  TopologyProvenanceKey circularEdgeKey_;
  std::size_t cylinderCount_{24};
};

struct ManyEdgePartFixture final {
  std::unique_ptr<Document> document;
  FaceReference planarFace;
  FaceReference cylindricalFace;
  EdgeReference circularEdge;
  std::size_t faceCount{};
  std::size_t edgeCount{};
};

class NearCollinearPlanarFeature final : public ShapeFeature {
 public:
  NearCollinearPlanarFeature(
      FeatureId id, double tangent,
      TopAbs_Orientation secondOrientation = TopAbs_FORWARD,
      TopAbs_Orientation firstOrientation = TopAbs_FORWARD)
      : ShapeFeature(id, "near-collinear planar source"),
        tangent_(tangent),
        firstOrientation_(firstOrientation),
        secondOrientation_(secondOrientation) {}

  [[nodiscard]] const std::array<TopologyProvenanceKey, 2>& faceKeys()
      const noexcept {
    return faceKeys_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<NearCollinearPlanarFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    std::array faces{
        BRepBuilderAPI_MakeFace(
            gp_Pln(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
            -2.0, 2.0, -2.0, 2.0)
            .Face(),
        BRepBuilderAPI_MakeFace(
            gp_Pln(gp_Pnt(0.0, 0.0, 0.0),
                   gp_Dir(tangent_, 0.0, 1.0)),
            -2.0, 2.0, -2.0, 2.0)
            .Face()};
    std::vector<TopologyLineageBinding> bindings;
    for (std::size_t index = 0; index < faces.size(); ++index) {
      faces[index].Orientation(index == 1 ? secondOrientation_
                                          : firstOrientation_);
      builder.Add(compound, faces[index]);
      faceKeys_[index] = {
          id(), TopologyKind::Face, TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Custom,
          "near-collinear-face:" + std::to_string(index)};
      bindings.push_back(
          {faceKeys_[index], std::make_shared<TopoDS_Shape>(faces[index])});
    }
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(
        std::make_shared<TopoDS_Shape>(compound), std::move(lineage));
    markValid();
    return true;
  }

 private:
  double tangent_{};
  TopAbs_Orientation firstOrientation_{TopAbs_FORWARD};
  TopAbs_Orientation secondOrientation_{TopAbs_FORWARD};
  std::array<TopologyProvenanceKey, 2> faceKeys_{};
};

std::unique_ptr<ProductDocument> makeNearCollinearAngleProduct(
    double tangent, double target,
    TopAbs_Orientation secondOrientation = TopAbs_FORWARD,
    TopAbs_Orientation firstOrientation = TopAbs_FORWARD,
    PlanarMateOrientation mateOrientation = PlanarMateOrientation::Aligned) {
  constexpr BodyId bodyId{2282};
  constexpr FeatureId featureId{2283};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "near-collinear planar body");
  auto feature =
      std::make_unique<NearCollinearPlanarFeature>(featureId, tangent,
                                                   secondOrientation,
                                                   firstOrientation);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  const auto first = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->faceKeys()[0]});
  const auto second = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->faceKeys()[1]});
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            MateId{1}, MateEndpoint{path(kFixed), first},
            MateEndpoint{path(kMoving), second},
            mateOrientation, target}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

class AnalyticAngleFeature final : public ShapeFeature {
 public:
  AnalyticAngleFeature(FeatureId id, TopAbs_Orientation firstOrientation,
                       TopAbs_Orientation secondOrientation)
      : ShapeFeature(id, "analytic angle source"),
        firstOrientation_(firstOrientation),
        secondOrientation_(secondOrientation) {}

  [[nodiscard]] const std::array<TopologyProvenanceKey, 2>& faceKeys()
      const noexcept {
    return faceKeys_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<AnalyticAngleFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    std::array faces{
        BRepBuilderAPI_MakeFace(
            gp_Pln(gp_Pnt(2.0, -1.0, 4.0), gp_Dir(1.0, 2.0, 2.0)),
            -3.0, 3.0, -3.0, 3.0)
            .Face(),
        BRepBuilderAPI_MakeFace(
            gp_Pln(gp_Pnt(-3.0, 5.0, 2.0), gp_Dir(2.0, -2.0, 1.0)),
            -3.0, 3.0, -3.0, 3.0)
            .Face()};
    faces[0].Orientation(firstOrientation_);
    faces[1].Orientation(secondOrientation_);
    std::vector<TopologyLineageBinding> bindings;
    for (std::size_t index = 0; index < faces.size(); ++index) {
      builder.Add(compound, faces[index]);
      faceKeys_[index] = {
          id(), TopologyKind::Face, TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Custom,
          "analytic-angle-face:" + std::to_string(index)};
      bindings.push_back(
          {faceKeys_[index], std::make_shared<TopoDS_Shape>(faces[index])});
    }
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(
        std::make_shared<TopoDS_Shape>(compound), std::move(lineage));
    markValid();
    return true;
  }

 private:
  TopAbs_Orientation firstOrientation_{TopAbs_FORWARD};
  TopAbs_Orientation secondOrientation_{TopAbs_FORWARD};
  std::array<TopologyProvenanceKey, 2> faceKeys_{};
};

std::unique_ptr<ProductDocument> makeAnalyticAngleProduct(
    PlanarMateOrientation orientation, bool swapEndpoints = false,
    TopAbs_Orientation firstOrientation = TopAbs_FORWARD,
    TopAbs_Orientation secondOrientation = TopAbs_FORWARD,
    MateId id = MateId{1},
    RigidTransform3d commonFrame = RigidTransform3d::identity(),
    bool fixedGrounded = true, bool movingGrounded = false) {
  constexpr BodyId bodyId{2292};
  constexpr FeatureId featureId{2293};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "analytic angle body");
  auto feature = std::make_unique<AnalyticAngleFeature>(
      featureId, firstOrientation, secondOrientation);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  const auto fixedFace = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->faceKeys()[0]});
  const auto movingFace = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->faceKeys()[1]});
  MateEndpoint fixed{path(kFixed), fixedFace};
  MateEndpoint moving{path(kMoving), movingFace};
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, commonFrame,
            OccurrenceFlags{true, false, fixedGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, commonFrame,
            OccurrenceFlags{true, false, movingGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            id, swapEndpoints ? moving : fixed,
            swapEndpoints ? fixed : moving, orientation, 0.6}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

ManyEdgePartFixture makeManyEdgeCircularPart(
    std::size_t cylinderCount = 24) {
  constexpr BodyId bodyId{2272};
  constexpr FeatureId featureId{2273};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "many-edge circular body");
  auto feature =
      std::make_unique<ManyEdgeCircularFeature>(featureId, cylinderCount);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  CHECK(snapshot->topologyIndex()->faceCount() >= cylinderCount * 3);
  CHECK(snapshot->topologyIndex()->edgeCount() >= cylinderCount * 3);
  auto face = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->planarFaceKey()});
  auto cylinder = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->cylindricalFaceKey()});
  auto edge = EdgeReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->circularEdgeKey()});
  CHECK(snapshot->topologyIndex()->resolveFace(face));
  CHECK(snapshot->topologyIndex()->resolveFace(cylinder));
  CHECK(snapshot->topologyIndex()->resolveEdge(edge));
  return {std::move(document), std::move(face), std::move(cylinder),
          std::move(edge),
          snapshot->topologyIndex()->faceCount(),
          snapshot->topologyIndex()->edgeCount()};
}

class SyntheticCircularEdgeFeature final : public ShapeFeature {
 public:
  SyntheticCircularEdgeFeature(FeatureId id, bool trimmed, bool ambiguous,
                               bool authoritative)
      : ShapeFeature(id, "synthetic circular edge"),
        trimmed_(trimmed),
        ambiguous_(ambiguous),
        authoritative_(authoritative) {}

  [[nodiscard]] const TopologyProvenanceKey& edgeKey() const noexcept {
    return edgeKey_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<SyntheticCircularEdgeFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(
        compound,
        BRepPrimAPI_MakeCylinder(
            gp_Ax2(gp_Pnt(100.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
            2.0, 3.0)
            .Shape());
    const auto addCircle = [&](double x) {
      const gp_Circ circle(
          gp_Ax2(gp_Pnt(x, 2.0, 3.0), gp_Dir(0.0, 0.0, 1.0)), 4.0);
      const TopoDS_Edge edge =
          trimmed_ ? BRepBuilderAPI_MakeEdge(circle, 0.0, 1.25).Edge()
                   : BRepBuilderAPI_MakeEdge(circle).Edge();
      builder.Add(compound, edge);
      return edge;
    };
    const auto first = addCircle(0.0);
    std::optional<TopoDS_Edge> second;
    if (ambiguous_) second = addCircle(20.0);
    auto shape = std::make_shared<TopoDS_Shape>(compound);
    if (!authoritative_) {
      setShape(std::move(shape));
      markValid();
      return true;
    }
    edgeKey_ = {id(), TopologyKind::Edge,
                TopologyProvenanceOperation::Generated,
                TopologySemanticRole::Custom, "synthetic-circle-edge"};
    std::vector<TopologyLineageBinding> bindings;
    std::size_t faceOrdinal = 0;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next(), ++faceOrdinal) {
      bindings.push_back(
          {{id(), TopologyKind::Face,
            TopologyProvenanceOperation::Generated,
            TopologySemanticRole::Custom,
            "synthetic-circle-support-face:" +
                std::to_string(faceOrdinal)},
           std::make_shared<TopoDS_Shape>(faces.Current())});
    }
    bindings.push_back(
        {edgeKey_, std::make_shared<TopoDS_Shape>(first)});
    if (second)
      bindings.push_back(
          {edgeKey_, std::make_shared<TopoDS_Shape>(*second)});
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  bool trimmed_{};
  bool ambiguous_{};
  bool authoritative_{};
  TopologyProvenanceKey edgeKey_;
};

struct SyntheticCircularPartFixture final {
  std::unique_ptr<Document> document;
  EdgeReference edge;
};

SyntheticCircularPartFixture makeSyntheticCircularPart(
    bool trimmed, bool ambiguous = false, bool authoritative = true) {
  constexpr BodyId bodyId{2282};
  constexpr FeatureId featureId{2283};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "synthetic circular body");
  auto feature = std::make_unique<SyntheticCircularEdgeFeature>(
      featureId, trimmed, ambiguous, authoritative);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  const TopologyProvenanceKey key =
      authoritative
          ? featurePtr->edgeKey()
          : TopologyProvenanceKey{
                featureId, TopologyKind::Edge,
                TopologyProvenanceOperation::Generated,
                TopologySemanticRole::Custom, "synthetic-circle-edge"};
  auto edge = EdgeReference::fromV2(TopologyReferenceV2{bodyId, key});
  if (ambiguous)
    CHECK(snapshot->topologyIndex()->resolveEdge(edge).failure ==
          TopologyResolutionFailure::Ambiguous);
  else if (authoritative)
    CHECK(snapshot->topologyIndex()->resolveEdge(edge));
  else
    CHECK(!snapshot->v2Authoritative());
  return {std::move(document), std::move(edge)};
}

AmbiguousPartFixture makeAmbiguousPart() {
  constexpr BodyId bodyId{2202};
  constexpr FeatureId featureId{2203};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "ambiguous body");
  auto feature = std::make_unique<AmbiguousPlanarFeature>(featureId);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  auto ambiguousFace = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->ambiguousFaceKey()});
  auto validFace = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->validFaceKey()});
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  CHECK(snapshot->topologyIndex()->resolveFace(ambiguousFace).failure ==
        TopologyResolutionFailure::Ambiguous);
  CHECK(snapshot->topologyIndex()->resolveFace(validFace));
  return {std::move(document), std::move(ambiguousFace),
          std::move(validFace)};
}

class AmbiguousCylinderFeature final : public ShapeFeature {
 public:
  explicit AmbiguousCylinderFeature(FeatureId id)
      : ShapeFeature(id, "ambiguous cylinder source") {}

  [[nodiscard]] const TopologyProvenanceKey& ambiguousFaceKey() const {
    return ambiguousFaceKey_;
  }
  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<AmbiguousCylinderFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, BRepPrimAPI_MakeCylinder(5.0, 8.0).Shape());
    builder.Add(compound,
                BRepPrimAPI_MakeCylinder(
                    gp_Ax2(gp_Pnt(20.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)),
                    5.0, 8.0)
                    .Shape());
    auto shape = std::make_shared<TopoDS_Shape>(compound);
    ambiguousFaceKey_ = {
        id(), TopologyKind::Face, TopologyProvenanceOperation::Generated,
        TopologySemanticRole::Custom, "ambiguous-cylinder-face"};
    std::vector<TopologyLineageBinding> bindings;
    std::size_t cylinderIndex = 0;
    std::size_t otherIndex = 0;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next()) {
      auto key = ambiguousFaceKey_;
      if (BRepAdaptor_Surface(TopoDS::Face(faces.Current()), true).GetType() ==
          GeomAbs_Cylinder) {
        if (cylinderIndex++ >= 2)
          key.localName += ":cylinder:" + std::to_string(cylinderIndex);
      } else {
        key.localName += ":other:" + std::to_string(otherIndex++);
      }
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(faces.Current())});
    }
    CHECK(cylinderIndex == 2);
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  TopologyProvenanceKey ambiguousFaceKey_;
};

PartFixture makeAmbiguousCylinderPart() {
  constexpr BodyId bodyId{2252};
  constexpr FeatureId featureId{2253};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "ambiguous cylinder body");
  auto feature = std::make_unique<AmbiguousCylinderFeature>(featureId);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  auto face = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->ambiguousFaceKey()});
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  CHECK(snapshot->topologyIndex()->resolveFace(face).failure ==
        TopologyResolutionFailure::Ambiguous);
  return {std::move(document), std::move(face)};
}

class MaxOriginPlanarFeature final : public ShapeFeature {
 public:
  explicit MaxOriginPlanarFeature(FeatureId id)
      : ShapeFeature(id, "max-origin planar source") {}

  [[nodiscard]] const TopologyProvenanceKey& faceKey() const {
    return faceKey_;
  }

  [[nodiscard]] std::unique_ptr<Feature> clone() const override {
    return std::make_unique<MaxOriginPlanarFeature>(*this);
  }

 protected:
  bool rebuildImpl(const RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(10.0, 9.0, 8.0).Shape());
    std::vector<TopologyProvenanceOrigin> origins;
    origins.reserve(kMaximumTopologyImmediateOrigins);
    for (std::size_t index = 0; index < kMaximumTopologyImmediateOrigins;
         ++index) {
      origins.emplace_back(
          FeatureId{3000 + index}, TopologyKind::Face,
          TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Custom, "origin-" + std::to_string(index),
          TopologyOccurrenceKey{FeatureId{3000 + index}, index});
    }
    faceKey_ = {id(), TopologyKind::Face,
                TopologyProvenanceOperation::Generated,
                TopologySemanticRole::Custom, "max-origin-face",
                std::move(origins)};
    faceKey_.canonicalizeOrigins();
    std::vector<TopologyLineageBinding> bindings;
    std::size_t faceIndex = 0;
    for (TopExp_Explorer faces(*shape, TopAbs_FACE); faces.More();
         faces.Next(), ++faceIndex) {
      auto key = faceKey_;
      if (faceIndex != 0) key.localName += ":" + std::to_string(faceIndex);
      bindings.push_back(
          {std::move(key), std::make_shared<TopoDS_Shape>(faces.Current())});
    }
    std::string error;
    auto lineage = TopologyLineage::create(
        std::move(bindings), TopologyLineageCoverage::CompleteFaces, &error);
    CHECK(lineage && error.empty());
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  TopologyProvenanceKey faceKey_;
};

PartFixture makeMaxOriginPart() {
  constexpr BodyId bodyId{2302};
  constexpr FeatureId featureId{2303};
  auto document = std::make_unique<Document>();
  auto& body = document->addBody(bodyId, "max-origin body");
  auto feature = std::make_unique<MaxOriginPlanarFeature>(featureId);
  auto* featurePtr = feature.get();
  body.addFeature(std::move(feature));
  CHECK(document->recompute());
  auto face = FaceReference::fromV2(
      TopologyReferenceV2{bodyId, featurePtr->faceKey()});
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  CHECK(snapshot->topologyIndex()->resolveFace(face));
  return {std::move(document), std::move(face)};
}

struct OrientedPlanarPair final {
  FaceReference forward;
  FaceReference reversed;
  std::array<double, 3> forwardPoint;
  std::array<double, 3> forwardNormal;
  std::array<double, 3> reversedPoint;
  std::array<double, 3> reversedRawNormal;
};

OrientedPlanarPair orientedPlanarFaces(const Document& document, BodyId bodyId,
                                       FeatureId featureId) {
  const auto* body = document.findBody(bodyId);
  CHECK(body);
  const auto snapshot = body->topologySnapshot(featureId);
  CHECK(snapshot && snapshot->topologyIndex());
  struct FaceData final {
    FaceReference reference;
    TopAbs_Orientation orientation{};
    std::array<double, 3> point{};
    std::array<double, 3> normal{};
  };
  std::vector<FaceData> faces;
  for (std::size_t index = 0; index < 32; ++index) {
    auto made = document.createFaceReferenceV2(bodyId, featureId, index);
    if (!made) continue;
    const auto resolved = snapshot->topologyIndex()->resolveFace(made.reference);
    if (!resolved) continue;
    BRepAdaptor_Surface surface(*resolved.subshape, true);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto plane = surface.Plane();
    faces.push_back(
        {std::move(made.reference), resolved.subshape->Orientation(),
         {plane.Location().X(), plane.Location().Y(), plane.Location().Z()},
         {plane.Axis().Direction().X(), plane.Axis().Direction().Y(),
          plane.Axis().Direction().Z()}});
  }
  for (const auto& forward : faces) {
    if (forward.orientation != TopAbs_FORWARD) continue;
    for (const auto& reversed : faces) {
      if (reversed.orientation != TopAbs_REVERSED) continue;
      const double rawDot =
          forward.normal[0] * reversed.normal[0] +
          forward.normal[1] * reversed.normal[1] +
          forward.normal[2] * reversed.normal[2];
      if (std::abs(rawDot) > 0.999999)
        return {forward.reference, reversed.reference, forward.point,
                forward.normal, reversed.point, reversed.normal};
    }
  }
  CHECK(false);
  return {FaceReference{}, FaceReference{}, {}, {}, {}, {}};
}

struct AxisPartFixture final {
  std::unique_ptr<Document> document;
  std::array<FaceReference, 3> faces;
};

AxisPartFixture makeAxisPart() {
  constexpr SketchId sketchId{1201};
  constexpr BodyId bodyId{1202};
  constexpr FeatureId featureId{1203};
  auto document = std::make_unique<Document>();
  sketch::Sketch sketch;
  sketch.addRectangle({0.0, 0.0}, {10.0, 8.0});
  document->addSketch(sketchId, "axis profile", std::move(sketch));
  auto& body = document->addBody(bodyId, "axis body");
  body.addFeature(std::make_unique<ExtrudeFeature>(featureId, sketchId, 5.0,
                                                   "axis extrusion"));
  CHECK(document->recompute());
  const auto snapshot = body.topologySnapshot(featureId);
  CHECK(snapshot && snapshot->v2Authoritative() && snapshot->topologyIndex());
  std::array<std::optional<FaceReference>, 3> selected;
  for (std::size_t index = 0; index < 32; ++index) {
    auto made = document->createFaceReferenceV2(bodyId, featureId, index);
    if (!made) continue;
    const auto resolved = snapshot->topologyIndex()->resolveFace(made.reference);
    if (!resolved) continue;
    BRepAdaptor_Surface surface(*resolved.subshape, true);
    if (surface.GetType() != GeomAbs_Plane) continue;
    const auto direction = surface.Plane().Axis().Direction();
    const std::array<double, 3> magnitude{std::abs(direction.X()),
                                          std::abs(direction.Y()),
                                          std::abs(direction.Z())};
    const auto axis = static_cast<std::size_t>(
        std::max_element(magnitude.begin(), magnitude.end()) -
        magnitude.begin());
    if (!selected[axis]) selected[axis].emplace(std::move(made.reference));
  }
  CHECK(selected[0] && selected[1] && selected[2]);
  return {std::move(document),
          {std::move(*selected[0]), std::move(*selected[1]),
           std::move(*selected[2])}};
}

std::unique_ptr<ProductDocument> makeThreeNodeAngleChain(
    bool reverseInsertion, MateId firstId, MateId secondId) {
  auto part = makeAxisPart();
  const auto bPlacement = RigidTransform3d::create(
      {2.0, 3.0, 5.0}, {0.9238795325112867, 0.3826834323650898, 0.0, 0.0});
  const auto cPlacement = RigidTransform3d::create(
      {-7.0, 11.0, 13.0}, {0.8660254037844386, 0.0, 0.5, 0.0});
  CHECK(bPlacement && cPlacement);
  constexpr OccurrenceId a{10};
  constexpr OccurrenceId b{20};
  constexpr OccurrenceId c{30};
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            a, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(b, kPart, *bPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(c, kPart, *cPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertFirst = [&] {
    return root.mateRegistry().insert(PlanarAngleMate{
        firstId, MateEndpoint{path(a), part.faces[2]},
        MateEndpoint{path(b), part.faces[2]},
        PlanarMateOrientation::Aligned, 0.4});
  };
  const auto insertSecond = [&] {
    return root.mateRegistry().insert(PlanarAngleMate{
        secondId, MateEndpoint{path(b), part.faces[2]},
        MateEndpoint{path(c), part.faces[2]},
        PlanarMateOrientation::Aligned, 0.7});
  };
  CHECK((reverseInsertion ? insertSecond() : insertFirst()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK((reverseInsertion ? insertFirst() : insertSecond()) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeDisconnectedAngleProduct() {
  auto part = makeAxisPart();
  const auto goodPlacement = RigidTransform3d::create(
      {2.0, 3.0, 5.0}, {0.9238795325112867, 0.3826834323650898, 0.0, 0.0});
  CHECK(goodPlacement);
  constexpr OccurrenceId goodFixed{10};
  constexpr OccurrenceId goodMoving{20};
  constexpr OccurrenceId badFixed{30};
  constexpr OccurrenceId badMoving{40};
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            goodFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(goodMoving, kPart,
                                        *goodPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            badFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            badMoving, kPart, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            MateId{1}, MateEndpoint{path(goodFixed), part.faces[2]},
            MateEndpoint{path(goodMoving), part.faces[2]},
            PlanarMateOrientation::Aligned, 0.4}) ==
        MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            MateId{2}, MateEndpoint{path(badFixed), part.faces[2]},
            MateEndpoint{path(badMoving), part.faces[2]},
            PlanarMateOrientation::Aligned, 0.4}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeIndependentPlaneProduct(
    std::size_t mateCount) {
  auto part = makeAxisPart();
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  for (std::size_t index = 0; index < mateCount; ++index) {
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
              MateId{index + 1}, MateEndpoint{path(kFixed), part.faces[index]},
              MateEndpoint{path(kMoving), part.faces[index]},
              PlanarMateOrientation::Aligned, 0.0)) ==
          MateRegistryMutationStatus::Inserted);
  }
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makePermutedPlaneProduct(
    std::array<MateId, 3> ids, std::array<bool, 3> swapEndpoints,
    std::array<std::size_t, 3> insertionOrder) {
  auto part = makeAxisPart();
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insert = [&](std::size_t index) {
    MateEndpoint fixed{path(kFixed), part.faces[index]};
    MateEndpoint moving{path(kMoving), part.faces[index]};
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
              ids[index], swapEndpoints[index] ? moving : fixed,
              swapEndpoints[index] ? fixed : moving,
              PlanarMateOrientation::Aligned, 0.0)) ==
          MateRegistryMutationStatus::Inserted);
  };
  for (const auto index : insertionOrder) insert(index);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

struct AngularMateSpec final {
  AssemblyMateKind kind;
  PlanarMateOrientation orientation{PlanarMateOrientation::Aligned};
  MateId id;
  std::size_t faceAxis{2};
  double angleRad{0.52359877559829887308};
};

std::unique_ptr<ProductDocument> makeAngularProduct(
    const std::vector<AngularMateSpec>& specs, bool initiallyParallel,
    bool swapEndpoints = false, bool fixedGrounded = true,
    bool movingGrounded = false, bool reverseInsertion = false) {
  auto part = makeAxisPart();
  const double half = 0.25 * 3.14159265358979323846;
  const auto movingPlacement = initiallyParallel
                                   ? std::optional<RigidTransform3d>{
                                         RigidTransform3d::identity()}
                                   : RigidTransform3d::create(
                                         {7.0, 11.0, 13.0},
                                         {std::cos(half), std::sin(half),
                                          0.0, 0.0})
                                         .transform;
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, fixedGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, *movingPlacement,
            OccurrenceFlags{true, false, movingGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insert = [&](std::size_t index) {
    CHECK(specs[index].faceAxis < part.faces.size());
    MateEndpoint fixed{path(kFixed), part.faces[specs[index].faceAxis]};
    MateEndpoint moving{path(kMoving), part.faces[specs[index].faceAxis]};
    MateEndpoint first = swapEndpoints ? moving : fixed;
    MateEndpoint second = swapEndpoints ? fixed : moving;
    const MateId id = specs[index].id.isValid() ? specs[index].id
                                                 : MateId{index + 1};
    MateRegistryMutationStatus status = MateRegistryMutationStatus::Invalid;
    if (specs[index].kind == AssemblyMateKind::PlanarCoincident)
      status = root.mateRegistry().insert(PlanarCoincidentMate(
          id, std::move(first), std::move(second), specs[index].orientation,
          0.0));
    else if (specs[index].kind == AssemblyMateKind::PlanarParallel)
      status = root.mateRegistry().insert(PlanarParallelMate(
          id, std::move(first), std::move(second), specs[index].orientation));
    else if (specs[index].kind == AssemblyMateKind::PlanarAngle)
      status = root.mateRegistry().insert(PlanarAngleMate(
          id, std::move(first), std::move(second), specs[index].orientation,
          specs[index].angleRad));
    else
      status = root.mateRegistry().insert(PlanarPerpendicularMate(
          id, std::move(first), std::move(second)));
    CHECK(status == MateRegistryMutationStatus::Inserted);
  };
  if (reverseInsertion)
    for (std::size_t index = specs.size(); index > 0; --index)
      insert(index - 1);
  else
    for (std::size_t index = 0; index < specs.size(); ++index) insert(index);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeAngleDistanceProduct(
    bool reverseInsertion) {
  auto part = makeAxisPart();
  const double half = 0.25 * 3.14159265358979323846;
  const auto moving = RigidTransform3d::create(
      {5.0, 0.0, 0.0},
      {std::cos(half), std::sin(half), 0.0, 0.0});
  CHECK(moving);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *moving.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertAngle = [&] {
    return root.mateRegistry().insert(PlanarAngleMate{
        MateId{9}, MateEndpoint{path(kFixed), part.faces[2]},
        MateEndpoint{path(kMoving), part.faces[2]},
        PlanarMateOrientation::Aligned, 0.4});
  };
  const auto insertDistance = [&] {
    return root.mateRegistry().insert(PointDistanceMate{
        MateId{3}, MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
        MatePointEndpoint{path(kMoving), LocalDatumPoint::Origin}, 5.0});
  };
  CHECK((reverseInsertion ? insertDistance() : insertAngle()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK((reverseInsertion ? insertAngle() : insertDistance()) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

enum class AxisSource { DatumX, DatumY, DatumZ, Cylinder };

MateAxisReference axisReference(AxisSource source,
                                const FaceReference& cylinder) {
  switch (source) {
    case AxisSource::DatumX:
      return LocalDatumAxis::X;
    case AxisSource::DatumY:
      return LocalDatumAxis::Y;
    case AxisSource::DatumZ:
      return LocalDatumAxis::Z;
    case AxisSource::Cylinder:
      return CylindricalFaceAxis{cylinder};
  }
  return LocalDatumAxis::Z;
}

std::unique_ptr<ProductDocument> makeConcentricProduct(
    AxisSource firstSource, AxisSource secondSource,
    RigidTransform3d movingPlacement = RigidTransform3d::identity(),
    bool swapEndpoints = false, bool fixedGrounded = true,
    bool movingGrounded = false, MateId id = MateId{1},
    bool duplicate = false, bool planarGeometry = false,
    bool movingSuppressed = false) {
  auto part = planarGeometry ? makePart() : makeCylinderPart();
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, fixedGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, movingPlacement,
            OccurrenceFlags{true, movingSuppressed, movingGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insert = [&](MateId mateId) {
    MateAxisEndpoint fixed{path(kFixed),
                           axisReference(firstSource, part.face)};
    MateAxisEndpoint moving{path(kMoving),
                            axisReference(secondSource, part.face)};
    return root.mateRegistry().insert(AxisConcentricMate{
        mateId, swapEndpoints ? moving : fixed,
        swapEndpoints ? fixed : moving});
  };
  CHECK(insert(id) == MateRegistryMutationStatus::Inserted);
  if (duplicate)
    CHECK(insert(MateId{id.value() + 1}) ==
          MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makePointDistanceProduct(
    double initialSeparationMm, double distanceMm, bool swapEndpoints = false,
    bool fixedGrounded = true, bool movingGrounded = false,
    bool duplicate = false, double duplicateDistanceMm = 0.0,
    MateId firstId = MateId{1}) {
  auto part = makePart();
  const auto movingPlacement = RigidTransform3d::create(
      {initialSeparationMm, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, fixedGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, *movingPlacement.transform,
            OccurrenceFlags{true, false, movingGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const MatePointEndpoint fixed{path(kFixed), LocalDatumPoint::Origin};
  const MatePointEndpoint moving{path(kMoving), LocalDatumPoint::Origin};
  const auto insert = [&](MateId id, double authoredDistance) {
    return root.mateRegistry().insert(PointDistanceMate{
        id, swapEndpoints ? moving : fixed, swapEndpoints ? fixed : moving,
        authoredDistance});
  };
  CHECK(insert(firstId, distanceMm) == MateRegistryMutationStatus::Inserted);
  if (duplicate)
    CHECK(insert(MateId{firstId.value() + 1},
                 duplicateDistanceMm > 0.0 ? duplicateDistanceMm
                                           : distanceMm) ==
          MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makePointDistanceProduct3d(
    const std::array<double, 3>& fixedTranslation,
    const std::array<double, 3>& movingTranslation, double distanceMm,
    bool swapEndpoints = false, bool fixedGrounded = true,
    bool movingGrounded = false) {
  auto part = makePart();
  const auto fixedPlacement = RigidTransform3d::create(
      fixedTranslation, {1.0, 0.0, 0.0, 0.0});
  const auto movingPlacement = RigidTransform3d::create(
      movingTranslation, {1.0, 0.0, 0.0, 0.0});
  CHECK(fixedPlacement && movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, *fixedPlacement.transform,
            OccurrenceFlags{true, false, fixedGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, *movingPlacement.transform,
            OccurrenceFlags{true, false, movingGrounded})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const MatePointEndpoint fixed{path(kFixed), LocalDatumPoint::Origin};
  const MatePointEndpoint moving{path(kMoving), LocalDatumPoint::Origin};
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1}, swapEndpoints ? moving : fixed,
            swapEndpoints ? fixed : moving, distanceMm}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeLateCollapsePointDistanceProduct() {
  auto part = makePart();
  const auto grounded = RigidTransform3d::create(
      {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto b = RigidTransform3d::create(
      {2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(grounded && b);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, kPart, *grounded.transform,
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{20}, kPart,
                                        *b.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{30}, kPart, *grounded.transform,
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(OccurrenceId{10}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{20}),
                              LocalDatumPoint::Origin},
            1.0}) == MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{2},
            MatePointEndpoint{path(OccurrenceId{20}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{30}),
                              LocalDatumPoint::Origin},
            1.0e-16}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeCircularPointDistanceProduct(
    double distanceMm = 5.0) {
  auto part = makeCylinderPart();
  auto circular = circularEdgeOfCylinder(*part.document);
  const auto movingPlacement = RigidTransform3d::create(
      {2.0, 0.0, 3.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
            MatePointEndpoint{path(kMoving),
                              CircularEdgeCenter{std::move(circular)}},
            distanceMm}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeEdgePointDistanceProduct(
    std::unique_ptr<Document> document, EdgeReference edge,
    double distanceMm = 5.0) {
  const auto movingPlacement = RigidTransform3d::create(
      {2.0, 0.0, 3.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
            MatePointEndpoint{path(kMoving),
                              CircularEdgeCenter{std::move(edge)}},
            distanceMm}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

struct ManyEdgeDistanceProduct final {
  std::unique_ptr<ProductDocument> product;
  std::size_t edgeCount{};
};

ManyEdgeDistanceProduct makeManyEdgeCircularPointDistanceProduct() {
  auto part = makeManyEdgeCircularPart();
  const auto edgeCount = part.edgeCount;
  const auto movingPlacement = RigidTransform3d::create(
      {2.0, 0.0, 3.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
            MatePointEndpoint{path(kMoving),
                              CircularEdgeCenter{part.circularEdge}},
            5.0}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return {std::move(made.product), edgeCount};
}

struct ManyFaceAngleProduct final {
  std::unique_ptr<ProductDocument> product;
  std::size_t faceCount{};
};

ManyFaceAngleProduct makeManyFaceAngleProduct(
    std::size_t cylinderCount = 24) {
  auto part = makeManyEdgeCircularPart(cylinderCount);
  const auto faceCount = part.faceCount;
  const double halfPi = 0.5 * 3.14159265358979323846;
  const auto movingPlacement = RigidTransform3d::create(
      {7.0, 11.0, 13.0},
      {std::cos(0.5 * halfPi), std::sin(0.5 * halfPi), 0.0, 0.0});
  CHECK(movingPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            MateId{1}, MateEndpoint{path(kFixed), part.planarFace},
            MateEndpoint{path(kMoving), part.planarFace},
            PlanarMateOrientation::Aligned,
            0.52359877559829887308}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return {std::move(made.product), faceCount};
}

struct ManyFaceConcentricProduct final {
  std::unique_ptr<ProductDocument> product;
  std::size_t faceCount{};
};

ManyFaceConcentricProduct makeManyFaceConcentricProduct(
    std::size_t cylinderCount = 24) {
  auto part = makeManyEdgeCircularPart(cylinderCount);
  const auto faceCount = part.faceCount;
  const auto moving = RigidTransform3d::create(
      {2.0, 3.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(moving);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        *moving.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(AxisConcentricMate{
            MateId{1}, MateAxisEndpoint{path(kFixed), LocalDatumAxis::Z},
            MateAxisEndpoint{path(kMoving),
                             CylindricalFaceAxis{part.cylindricalFace}}}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return {std::move(made.product), faceCount};
}

std::unique_ptr<ProductDocument> makeThreeNodeAxisDistanceProduct(
    bool reverseInsertion) {
  auto part = makeCylinderPart();
  const auto bPlacement = RigidTransform3d::create(
      {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  const auto cPlacement = RigidTransform3d::create(
      {8.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(bPlacement && cPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{20}, kPart,
                                        *bPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{30}, kPart,
                                        *cPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertAxis = [&] {
    return root.mateRegistry().insert(AxisConcentricMate{
        MateId{11},
        MateAxisEndpoint{path(OccurrenceId{10}), LocalDatumAxis::Z},
        MateAxisEndpoint{path(OccurrenceId{20}), LocalDatumAxis::Z}});
  };
  const auto insertDistance = [&] {
    return root.mateRegistry().insert(PointDistanceMate{
        MateId{4},
        MatePointEndpoint{path(OccurrenceId{20}), LocalDatumPoint::Origin},
        MatePointEndpoint{path(OccurrenceId{30}), LocalDatumPoint::Origin},
        6.0});
  };
  CHECK((reverseInsertion ? insertDistance() : insertAxis()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK((reverseInsertion ? insertAxis() : insertDistance()) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeMixedConcentricPlanarProduct(
    AssemblyMateKind planarKind, bool reverseInsertion = false,
    MateId axisId = MateId{1}, MateId planarId = MateId{2}) {
  auto part = makeCylinderPart();
  const auto planar = planarFaceOfCylinder(*part.document);
  const double half = 0.25 * 3.14159265358979323846;
  const auto moving = RigidTransform3d::create(
      {3.0, 4.0, 5.0},
      planarKind == AssemblyMateKind::PlanarAngle
          ? std::array<double, 4>{std::cos(half), std::sin(half), 0.0, 0.0}
          : std::array<double, 4>{1.0, 0.0, 0.0, 0.0});
  CHECK(moving);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart, *moving.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertAxis = [&] {
    const auto datum = planarKind == AssemblyMateKind::PlanarAngle
                           ? LocalDatumAxis::X
                           : LocalDatumAxis::Z;
    return root.mateRegistry().insert(AxisConcentricMate{
        axisId, MateAxisEndpoint{path(kFixed), datum},
        MateAxisEndpoint{path(kMoving), datum}});
  };
  const auto insertPlanar = [&] {
    const MateEndpoint first{path(kFixed), planar};
    const MateEndpoint second{path(kMoving), planar};
    if (planarKind == AssemblyMateKind::PlanarCoincident)
      return root.mateRegistry().insert(PlanarCoincidentMate{
          planarId, first, second, PlanarMateOrientation::Aligned, 0.0});
    if (planarKind == AssemblyMateKind::PlanarParallel)
      return root.mateRegistry().insert(PlanarParallelMate{
          planarId, first, second, PlanarMateOrientation::Aligned});
    if (planarKind == AssemblyMateKind::PlanarAngle)
      return root.mateRegistry().insert(PlanarAngleMate{
          planarId, first, second, PlanarMateOrientation::Aligned, 0.4});
    return root.mateRegistry().insert(
        PlanarPerpendicularMate{planarId, first, second});
  };
  CHECK((reverseInsertion ? insertPlanar() : insertAxis()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK((reverseInsertion ? insertAxis() : insertPlanar()) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeThreeNodeMixedProduct(
    bool reverseInsertion, MateId axisId, MateId planarId) {
  auto part = makeCylinderPart();
  const auto planar = planarFaceOfCylinder(*part.document);
  const auto bPlacement = RigidTransform3d::create(
      {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  const auto cPlacement = RigidTransform3d::create(
      {8.0, 4.0, 9.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(bPlacement && cPlacement);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            OccurrenceId{10}, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{20}, kPart,
                                        *bPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(OccurrenceId{30}, kPart,
                                        *cPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertAxis = [&] {
    return root.mateRegistry().insert(AxisConcentricMate{
        axisId,
        MateAxisEndpoint{path(OccurrenceId{10}), LocalDatumAxis::Z},
        MateAxisEndpoint{path(OccurrenceId{20}), LocalDatumAxis::Z}});
  };
  const auto insertPlanar = [&] {
    return root.mateRegistry().insert(PlanarCoincidentMate{
        planarId, MateEndpoint{path(OccurrenceId{20}), planar},
        MateEndpoint{path(OccurrenceId{30}), planar},
        PlanarMateOrientation::Aligned, 0.0});
  };
  CHECK((reverseInsertion ? insertPlanar() : insertAxis()) ==
        MateRegistryMutationStatus::Inserted);
  CHECK((reverseInsertion ? insertAxis() : insertPlanar()) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeProduct(
    bool firstGrounded, double offsetMm = 7.0,
    PlanarMateOrientation orientation = PlanarMateOrientation::Aligned,
    bool secondGrounded = false, bool duplicate = false,
    bool secondSuppressed = false, bool firstVisible = true,
    bool reverseInsertion = false, double duplicateOffsetDelta = 0.0) {
  auto part = makePart();
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{firstVisible, false, firstGrounded}, "fixed")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        RigidTransform3d::identity(),
                                        OccurrenceFlags{true, secondSuppressed,
                                                        secondGrounded},
                                        "moving")) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  const auto insertMate = [&](MateId id) {
    return root.mateRegistry().insert(PlanarCoincidentMate(
        id, MateEndpoint{path(kFixed), part.face},
        MateEndpoint{path(kMoving), part.face}, orientation,
        offsetMm + (id == MateId{2} ? duplicateOffsetDelta : 0.0)));
  };
  if (duplicate && reverseInsertion)
    CHECK(insertMate(MateId{2}) == MateRegistryMutationStatus::Inserted);
  CHECK(insertMate(MateId{1}) == MateRegistryMutationStatus::Inserted);
  if (duplicate && !reverseInsertion)
    CHECK(insertMate(MateId{2}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeNestedProduct() {
  constexpr DefinitionId nestedId{3};
  constexpr OccurrenceId nestedLeaf{5};
  auto part = makePart();
  const auto nestedPlacement = RigidTransform3d::create(
      {3.0, 4.0, 5.0}, {0.7071067811865476, 0.0, 0.0,
                        0.7071067811865476});
  CHECK(nestedPlacement);
  AssemblyDefinition nested(nestedId);
  CHECK(nested.insert(ComponentOccurrence(nestedLeaf, kPart,
                                           *nestedPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, nestedId, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, nestedId, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
            MateId{1}, MateEndpoint{path({kFixed, nestedLeaf}), part.face},
            MateEndpoint{path({kMoving, nestedLeaf}), part.face},
            PlanarMateOrientation::Aligned, 6.0)) ==
        MateRegistryMutationStatus::Inserted);
  std::vector<PartDefinition> parts;
  parts.emplace_back(kPart, std::move(part.document));
  std::vector<AssemblyDefinition> assemblies;
  assemblies.push_back(std::move(nested));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(std::move(parts),
                                             std::move(assemblies), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeNestedAngleProduct() {
  constexpr DefinitionId nestedId{3};
  constexpr OccurrenceId nestedLeaf{5};
  auto part = makeAxisPart();
  const auto nestedPlacement = RigidTransform3d::create(
      {3.0, 4.0, 5.0},
      {0.9238795325112867, 0.0, 0.3826834323650898, 0.0});
  const double half = 0.25 * 3.14159265358979323846;
  const auto movingPlacement = RigidTransform3d::create(
      {7.0, 11.0, 13.0},
      {std::cos(half), std::sin(half), 0.0, 0.0});
  CHECK(nestedPlacement && movingPlacement);
  AssemblyDefinition nested(nestedId);
  CHECK(nested.insert(ComponentOccurrence(nestedLeaf, kPart,
                                           *nestedPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, nestedId, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, nestedId, *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarAngleMate{
            MateId{1},
            MateEndpoint{path({kFixed, nestedLeaf}), part.faces[2]},
            MateEndpoint{path({kMoving, nestedLeaf}), part.faces[2]},
            PlanarMateOrientation::Aligned, 0.4}) ==
        MateRegistryMutationStatus::Inserted);
  std::vector<PartDefinition> parts;
  parts.emplace_back(kPart, std::move(part.document));
  std::vector<AssemblyDefinition> assemblies;
  assemblies.push_back(std::move(nested));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeNestedPointDistanceProduct() {
  constexpr DefinitionId nestedId{3};
  constexpr OccurrenceId nestedLeaf{5};
  auto part = makePart();
  const auto nestedPlacement = RigidTransform3d::create(
      {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  const auto movingPlacement = RigidTransform3d::create(
      {2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(nestedPlacement && movingPlacement);
  AssemblyDefinition nested(nestedId);
  CHECK(nested.insert(ComponentOccurrence(nestedLeaf, kPart,
                                           *nestedPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, nestedId, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, nestedId, *movingPlacement.transform)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path({kFixed, nestedLeaf}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path({kMoving, nestedLeaf}),
                              LocalDatumPoint::Origin},
            5.0}) == MateRegistryMutationStatus::Inserted);
  std::vector<PartDefinition> parts;
  parts.emplace_back(kPart, std::move(part.document));
  std::vector<AssemblyDefinition> assemblies;
  assemblies.push_back(std::move(nested));
  assemblies.push_back(std::move(root));
  auto made = ProductDocument::createNested(
      std::move(parts), std::move(assemblies), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeDisconnectedPointDistanceProduct() {
  auto part = makePart();
  const auto goodMoving = RigidTransform3d::create(
      {2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(goodMoving);
  AssemblyDefinition root(kRoot);
  for (const auto [id, placement, grounded] : {
           std::tuple{OccurrenceId{10}, RigidTransform3d::identity(), true},
           std::tuple{OccurrenceId{20}, *goodMoving.transform, false},
           std::tuple{OccurrenceId{30}, RigidTransform3d::identity(), true},
           std::tuple{OccurrenceId{40}, RigidTransform3d::identity(), false}})
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, placement,
              OccurrenceFlags{true, false, grounded})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(OccurrenceId{10}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{20}),
                              LocalDatumPoint::Origin},
            5.0}) == MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{2},
            MatePointEndpoint{path(OccurrenceId{30}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{40}),
                              LocalDatumPoint::Origin},
            3.0}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument>
makeDisconnectedAmbiguousPointDistanceProduct() {
  auto part = makeSyntheticCircularPart(false, true);
  const auto goodMoving = RigidTransform3d::create(
      {2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto badFixed = RigidTransform3d::create(
      {10.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  const auto badMoving = RigidTransform3d::create(
      {20.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(goodMoving && badFixed && badMoving);
  AssemblyDefinition root(kRoot);
  for (const auto [id, placement, grounded] : {
           std::tuple{OccurrenceId{10}, RigidTransform3d::identity(), true},
           std::tuple{OccurrenceId{20}, *goodMoving.transform, false},
           std::tuple{OccurrenceId{30}, *badFixed.transform, true},
           std::tuple{OccurrenceId{40}, *badMoving.transform, false}})
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, placement,
              OccurrenceFlags{true, false, grounded})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{1},
            MatePointEndpoint{path(OccurrenceId{10}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{20}),
                              LocalDatumPoint::Origin},
            5.0}) == MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(PointDistanceMate{
            MateId{2},
            MatePointEndpoint{path(OccurrenceId{30}),
                              LocalDatumPoint::Origin},
            MatePointEndpoint{path(OccurrenceId{40}),
                              CircularEdgeCenter{part.edge}},
            3.0}) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(
            PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeDisconnectedProduct(bool broken) {
  auto part = makePart();
  AssemblyDefinition root(kRoot);
  const auto addOccurrence = [&](OccurrenceId id, bool grounded,
                                 bool suppressed) {
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, RigidTransform3d::identity(),
              OccurrenceFlags{true, suppressed, grounded})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  };
  addOccurrence(OccurrenceId{10}, true, false);
  addOccurrence(OccurrenceId{20}, false, false);
  addOccurrence(OccurrenceId{30}, true, false);
  addOccurrence(OccurrenceId{40}, true, broken);
  const auto addMate = [&](MateId id, OccurrenceId first, OccurrenceId second,
                           double offset) {
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
              id, MateEndpoint{path(first), part.face},
              MateEndpoint{path(second), part.face},
              PlanarMateOrientation::Aligned, offset)) ==
          MateRegistryMutationStatus::Inserted);
  };
  addMate(MateId{1}, OccurrenceId{10}, OccurrenceId{20}, 2.0);
  addMate(MateId{2}, OccurrenceId{30}, OccurrenceId{40}, 3.0);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeDisconnectedAngularProduct() {
  auto part = makeAxisPart();
  AssemblyDefinition root(kRoot);
  for (const auto [id, grounded, suppressed] :
       {std::tuple{OccurrenceId{10}, true, false},
        std::tuple{OccurrenceId{20}, false, false},
        std::tuple{OccurrenceId{30}, true, false},
        std::tuple{OccurrenceId{40}, false, true}}) {
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, RigidTransform3d::identity(),
              OccurrenceFlags{true, suppressed, grounded})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(root.mateRegistry().insert(PlanarParallelMate(
            MateId{1}, MateEndpoint{path(OccurrenceId{10}), part.faces[2]},
            MateEndpoint{path(OccurrenceId{20}), part.faces[2]},
            PlanarMateOrientation::Aligned)) ==
        MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarPerpendicularMate(
            MateId{2}, MateEndpoint{path(OccurrenceId{30}), part.faces[2]},
            MateEndpoint{path(OccurrenceId{40}), part.faces[2]})) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeDisconnectedConcentricProduct() {
  auto part = makePart();
  const auto offAxis = RigidTransform3d::create(
      {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
  CHECK(offAxis);
  AssemblyDefinition root(kRoot);
  for (const auto [id, placement, grounded, suppressed] :
       {std::tuple{OccurrenceId{10}, RigidTransform3d::identity(), true,
                   false},
        std::tuple{OccurrenceId{20}, *offAxis.transform, false, false},
        std::tuple{OccurrenceId{30}, RigidTransform3d::identity(), true,
                   false},
        std::tuple{OccurrenceId{40}, RigidTransform3d::identity(), false,
                   true}}) {
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, placement,
              OccurrenceFlags{true, suppressed, grounded})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  }
  CHECK(root.mateRegistry().insert(AxisConcentricMate{
            MateId{1},
            MateAxisEndpoint{path(OccurrenceId{10}), LocalDatumAxis::Z},
            MateAxisEndpoint{path(OccurrenceId{20}), LocalDatumAxis::Z}}) ==
        MateRegistryMutationStatus::Inserted);
  CHECK(root.mateRegistry().insert(AxisConcentricMate{
            MateId{2},
            MateAxisEndpoint{path(OccurrenceId{30}), LocalDatumAxis::Z},
            MateAxisEndpoint{path(OccurrenceId{40}), LocalDatumAxis::Z}}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeCycleProduct(bool inconsistent) {
  auto part = makePart();
  AssemblyDefinition root(kRoot);
  for (const auto id : {OccurrenceId{10}, OccurrenceId{20}, OccurrenceId{30}}) {
    CHECK(root.insert(ComponentOccurrence(
              id, kPart, RigidTransform3d::identity(),
              OccurrenceFlags{true, false, id == OccurrenceId{10}})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
  }
  const auto addMate = [&](MateId id, OccurrenceId first, OccurrenceId second,
                           double offset) {
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
              id, MateEndpoint{path(first), part.face},
              MateEndpoint{path(second), part.face},
              PlanarMateOrientation::Aligned, offset)) ==
          MateRegistryMutationStatus::Inserted);
  };
  addMate(MateId{1}, OccurrenceId{10}, OccurrenceId{20}, 0.0);
  addMate(MateId{2}, OccurrenceId{20}, OccurrenceId{30}, 0.0);
  addMate(MateId{3}, OccurrenceId{10}, OccurrenceId{30},
          inconsistent ? 1.0 : 0.0);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeMaximumWorkProduct() {
  auto part = makeMaxOriginPart();
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  for (std::size_t index = 0; index < kMaximumAssemblyMates; ++index) {
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
              MateId{index + 1}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Aligned, 0.0)) ==
          MateRegistryMutationStatus::Inserted);
  }
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeSingleFaceProduct(
    PartFixture part, FaceReference face,
    RigidTransform3d movingPlacement = RigidTransform3d::identity(),
    RigidTransform3d fixedPlacement = RigidTransform3d::identity(),
    PlanarMateOrientation orientation = PlanarMateOrientation::Aligned,
    double offsetMm = 0.0) {
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, fixedPlacement,
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart, movingPlacement)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
            MateId{1}, MateEndpoint{path(kFixed), face},
            MateEndpoint{path(kMoving), face}, orientation, offsetMm)) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeSingleAxisFaceProduct(
    PartFixture part, FaceReference face) {
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(kMoving, kPart,
                                        RigidTransform3d::identity())) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(AxisConcentricMate{
            MateId{1},
            MateAxisEndpoint{path(kFixed), CylindricalFaceAxis{face}},
            MateAxisEndpoint{path(kMoving), CylindricalFaceAxis{face}}}) ==
        MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

std::unique_ptr<ProductDocument> makeTwoFaceProduct(
    PartFixture part, FaceReference firstFace, FaceReference secondFace,
    PlanarMateOrientation orientation, double offsetMm,
    RigidTransform3d movingPlacement = RigidTransform3d::identity()) {
  AssemblyDefinition root(kRoot);
  CHECK(root.insert(ComponentOccurrence(
            kFixed, kPart, RigidTransform3d::identity(),
            OccurrenceFlags{true, false, true})) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.insert(ComponentOccurrence(
            kMoving, kPart, movingPlacement)) ==
        AssemblyOccurrenceInsertStatus::Inserted);
  CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
            MateId{1}, MateEndpoint{path(kFixed), std::move(firstFace)},
            MateEndpoint{path(kMoving), std::move(secondFace)}, orientation,
            offsetMm)) == MateRegistryMutationStatus::Inserted);
  DefinitionRegistry definitions;
  CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
        DefinitionInsertStatus::Inserted);
  CHECK(definitions.insert(std::move(root)) ==
        DefinitionInsertStatus::Inserted);
  auto made = ProductDocument::create(std::move(definitions), kRoot);
  CHECK(made);
  return std::move(made.product);
}

class Successor final : public ProductDocumentSuccessor {
 public:
  explicit Successor(ProductDocumentSuccessorStatus status) : status_(status) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument&, ProductDocumentSuccessorImpact impact) noexcept override {
    ++calls;
    lastImpact = impact;
    return status_;
  }

  int calls{};
  ProductDocumentSuccessorImpact lastImpact{
      ProductDocumentSuccessorImpact::FullProjection};

 private:
  ProductDocumentSuccessorStatus status_;
};

class ReentrantSuccessor final : public ProductDocumentSuccessor {
 public:
  explicit ReentrantSuccessor(const MatePlacementCandidate& candidate)
      : candidate_(candidate) {}

  ProductDocumentSuccessorStatus prepare(
      const ProductDocument& product,
      ProductDocumentSuccessorImpact) noexcept override {
    solveStatus = solvePlanarMates(product, kRoot).status;
    Successor nested(ProductDocumentSuccessorStatus::Prepared);
    applyStatus = const_cast<ProductDocument&>(product)
                      .applyMatePlacementCandidate(candidate_, nested);
    nestedSuccessorCalls = nested.calls;
    return ProductDocumentSuccessorStatus::Rejected;
  }

  MateSolveStatus solveStatus{MateSolveStatus::Solved};
  MatePlacementApplyStatus applyStatus{MatePlacementApplyStatus::Applied};
  int nestedSuccessorCalls{};

 private:
  const MatePlacementCandidate& candidate_;
};

}  // namespace

int main() {
  static_assert(!std::is_aggregate_v<PlanarMateAuthoringTransaction>);
  static_assert(!std::is_default_constructible_v<
                PlanarMateAuthoringTransaction>);
  static_assert(!std::is_constructible_v<
                MatePlacementCandidate, MatePlacementCandidateAddress,
                MateSolveRequest, std::vector<MateId>,
                std::vector<MatePlacementChange>>);
  static_assert(std::is_same_v<
                decltype(std::declval<const MatePlacementCandidate&>().changes()),
                const std::vector<MatePlacementChange>&>);
  static_assert(std::is_same_v<
                decltype(std::declval<const MatePlacementCandidate&>()
                             .componentMateIds()),
                const std::vector<MateId>&>);
  // A8a prospective authoring uses a staged registry overlay. Preview is
  // observably read-only; commit and inverse swap exact registry snapshots
  // together with every component placement.
  {
    auto product = makeProduct(true, 0.0);
    auto* root = const_cast<AssemblyDefinition*>(
        product->rootAssemblyDefinition());
    CHECK(root);
    const PlanarCoincidentMate authored = *root->mateRegistry().find(MateId{1});
    CHECK(root->mateRegistry().erase(MateId{1}) ==
          MateRegistryMutationStatus::Erased);
    const MateRegistry beforeRegistry = root->mateRegistry();
    const auto beforeFixed = root->find(kFixed)->placement();
    const auto beforeMoving = root->find(kMoving)->placement();
    auto prepared = preparePlanarMateAuthoring(
        *product, kRoot,
        PlanarCoincidentMate{*beforeRegistry.lowestFreeId(), authored.first(),
                             authored.second(), authored.orientation(), 0.0});
    CHECK(prepared.transaction);
    CHECK(prepared.status == MateSolveStatus::Underconstrained);
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->find(kFixed)->placement() == beforeFixed);
    CHECK(root->find(kMoving)->placement() == beforeMoving);
    CHECK(classifyPlanarMateEndpoint(*product, kRoot, authored.first()));
    auto v1 = authored.first();
    v1.face = FaceReference{};
    const auto rejected = classifyPlanarMateEndpoint(*product, kRoot, v1);
    CHECK(!rejected);
    CHECK(rejected.failure == MateEndpointFailure::V1Unsupported);

    const MateRegistry afterRegistry = prepared.transaction->afterRegistry();
    const auto expectedAfter =
        prepared.transaction->candidate().changes().back().afterPlacement;
    Successor applied(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyPlanarMateAuthoring(*prepared.transaction, applied) ==
          MatePlacementApplyStatus::Applied);
    CHECK(applied.calls == 1);
    CHECK(applied.lastImpact == ProductDocumentSuccessorImpact::MateProjection);
    CHECK(root->mateRegistry() == afterRegistry);
    CHECK(root->find(kMoving)->placement() == expectedAfter);
    Successor reverted(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->revertPlanarMateAuthoring(*prepared.transaction, reverted) ==
          MatePlacementApplyStatus::Applied);
    CHECK(reverted.calls == 1);
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->find(kFixed)->placement() == beforeFixed);
    CHECK(root->find(kMoving)->placement() == beforeMoving);

    Successor rejectedSuccessor(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyPlanarMateAuthoring(*prepared.transaction,
                                            rejectedSuccessor) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(root->mateRegistry() == beforeRegistry);
    CHECK(root->find(kMoving)->placement() == beforeMoving);

    const PlanarCoincidentMate unrelated{
        MateId{2}, authored.first(), authored.second(), authored.orientation(),
        0.0};
    Successor reapplied(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyPlanarMateAuthoring(*prepared.transaction, reapplied) ==
          MatePlacementApplyStatus::Applied);
    CHECK(root->mateRegistry().insert(unrelated) ==
          MateRegistryMutationStatus::Inserted);
    const MateRegistry appliedWithUnrelated = root->mateRegistry();
    const auto appliedMoving = root->find(kMoving)->placement();
    Successor staleRevert(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->revertPlanarMateAuthoring(*prepared.transaction,
                                             staleRevert) ==
          MatePlacementApplyStatus::Stale);
    CHECK(staleRevert.calls == 0);
    CHECK(root->mateRegistry() == appliedWithUnrelated);
    CHECK(root->find(kMoving)->placement() == appliedMoving);
  }
  {
    auto product = makeProduct(true, 0.0);
    auto* root = const_cast<AssemblyDefinition*>(
        product->rootAssemblyDefinition());
    CHECK(root);
    const PlanarCoincidentMate authored = *root->mateRegistry().find(MateId{1});
    CHECK(root->mateRegistry().erase(MateId{1}) ==
          MateRegistryMutationStatus::Erased);
    auto prepared = preparePlanarMateAuthoring(
        *product, kRoot,
        PlanarCoincidentMate{*root->mateRegistry().lowestFreeId(),
                             authored.first(), authored.second(),
                             authored.orientation(), 0.0});
    CHECK(prepared.transaction);
    const auto beforeFixed = root->find(kFixed)->placement();
    const auto beforeMoving = root->find(kMoving)->placement();
    CHECK(root->mateRegistry().insert(PlanarCoincidentMate{
              MateId{2}, authored.first(), authored.second(),
              authored.orientation(), 0.0}) ==
          MateRegistryMutationStatus::Inserted);
    const MateRegistry liveAfterUnrelated = root->mateRegistry();
    Successor staleApply(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyPlanarMateAuthoring(*prepared.transaction, staleApply) ==
          MatePlacementApplyStatus::Stale);
    CHECK(staleApply.calls == 0);
    CHECK(root->mateRegistry() == liveAfterUnrelated);
    CHECK(root->find(kFixed)->placement() == beforeFixed);
    CHECK(root->find(kMoving)->placement() == beforeMoving);
  }
  {
    auto part = makePart();
    const FaceReference face = part.face;
    constexpr OccurrenceId first{10};
    constexpr OccurrenceId middle{20};
    constexpr OccurrenceId last{30};
    AssemblyDefinition root(kRoot);
    CHECK(root.insert(ComponentOccurrence(
              first, kPart, RigidTransform3d::identity(),
              OccurrenceFlags{true, false, true})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(root.insert(ComponentOccurrence(
              middle, kPart, RigidTransform3d::identity())) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    const auto displaced = RigidTransform3d::create(
        {3.0, 5.0, 11.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(displaced);
    CHECK(root.insert(ComponentOccurrence(last, kPart,
                                          *displaced.transform)) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(root.mateRegistry().insert(PlanarCoincidentMate{
              MateId{1}, MateEndpoint{path(first), face},
              MateEndpoint{path(middle), face},
              PlanarMateOrientation::Aligned, 2.0}) ==
          MateRegistryMutationStatus::Inserted);
    DefinitionRegistry definitions;
    CHECK(definitions.insert(PartDefinition(kPart, std::move(part.document))) ==
          DefinitionInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(root)) ==
          DefinitionInsertStatus::Inserted);
    auto made = ProductDocument::create(std::move(definitions), kRoot);
    CHECK(made);
    auto& product = *made.product;
    const auto* beforeRoot = product.rootAssemblyDefinition();
    CHECK(beforeRoot);
    const std::array beforePlacements{
        beforeRoot->find(first)->placement(),
        beforeRoot->find(middle)->placement(),
        beforeRoot->find(last)->placement()};
    const MateRegistry beforeRegistry = beforeRoot->mateRegistry();
    auto prepared = preparePlanarMateAuthoring(
        product, kRoot,
        PlanarCoincidentMate{
            *beforeRegistry.lowestFreeId(), MateEndpoint{path(middle), face},
            MateEndpoint{path(last), face}, PlanarMateOrientation::Aligned,
            0.0});
    CHECK(prepared.transaction);
    CHECK(prepared.transaction->candidate().changes().size() == 3);
    CHECK(prepared.transaction->candidate().changes()[0].before.occurrenceId ==
          first);
    CHECK(prepared.transaction->candidate().changes()[1].before.occurrenceId ==
          middle);
    CHECK(prepared.transaction->candidate().changes()[2].before.occurrenceId ==
          last);
    Successor apply(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product.applyPlanarMateAuthoring(*prepared.transaction, apply) ==
          MatePlacementApplyStatus::Applied);
    CHECK(apply.calls == 1);
    const auto* afterRoot = product.rootAssemblyDefinition();
    CHECK(afterRoot->mateRegistry() ==
          prepared.transaction->afterRegistry());
    const std::array afterPlacements{
        afterRoot->find(first)->placement(),
        afterRoot->find(middle)->placement(),
        afterRoot->find(last)->placement()};
    CHECK(afterPlacements != beforePlacements);
    Successor undo(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product.revertPlanarMateAuthoring(*prepared.transaction, undo) ==
          MatePlacementApplyStatus::Applied);
    CHECK(undo.calls == 1);
    CHECK(product.rootAssemblyDefinition()->mateRegistry() == beforeRegistry);
    const std::array restoredPlacements{
        product.rootAssemblyDefinition()->find(first)->placement(),
        product.rootAssemblyDefinition()->find(middle)->placement(),
        product.rootAssemblyDefinition()->find(last)->placement()};
    CHECK(restoredPlacements == beforePlacements);
  }
  {
    for (const bool broken : {false, true}) {
      auto product = makeDisconnectedProduct(broken);
      const MateSolveRequest good{MateId{1}, std::nullopt};
      const auto goodResult = solvePlanarMates(*product, kRoot, good);
      CHECK(goodResult.candidate);
      CHECK(goodResult.candidate->componentMateIds() ==
            std::vector{MateId{1}});
      CHECK(goodResult.candidate->changes().size() == 2);
      CHECK(goodResult.candidate->changes()[0].before.occurrenceId ==
            OccurrenceId{10});
      CHECK(goodResult.candidate->changes()[1].before.occurrenceId ==
            OccurrenceId{20});
      const MateSolveRequest occurrenceSeed{std::nullopt, OccurrenceId{10}};
      CHECK(solvePlanarMates(*product, kRoot, occurrenceSeed).candidate);
      const MateSolveRequest dualSeed{MateId{1}, OccurrenceId{30}};
      CHECK(solvePlanarMates(*product, kRoot, dualSeed).status ==
            MateSolveStatus::InvalidInput);
      const MateSolveRequest bad{MateId{2}, std::nullopt};
      CHECK(solvePlanarMates(*product, kRoot, bad).status ==
            (broken ? MateSolveStatus::Broken
                    : MateSolveStatus::Overconstrained));
    }
  }

  {
    auto product = makeDisconnectedAngularProduct();
    const auto good = solvePlanarMates(
        *product, kRoot, MateSolveRequest{MateId{1}, std::nullopt});
    CHECK(good.candidate);
    CHECK(good.componentMateIds == std::vector{MateId{1}});
    CHECK(good.status == MateSolveStatus::Underconstrained);
    const auto bad = solvePlanarMates(
        *product, kRoot, MateSolveRequest{MateId{2}, std::nullopt});
    CHECK(bad.status == MateSolveStatus::Broken);
    CHECK(bad.componentMateIds == std::vector{MateId{2}});
    CHECK(bad.diagnostics.endpointFailure);
    CHECK(bad.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Suppressed);
  }

  {
    auto product = makeDisconnectedConcentricProduct();
    const auto good = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{1}, std::nullopt});
    CHECK(good.candidate);
    CHECK(good.componentMateIds == std::vector{MateId{1}});
    CHECK(good.status == MateSolveStatus::Underconstrained);
    CHECK(good.diagnostics.rank == 4);
    const auto bad = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{2}, std::nullopt});
    CHECK(bad.status == MateSolveStatus::Broken);
    CHECK(bad.componentMateIds == std::vector{MateId{2}});
    CHECK(bad.diagnostics.endpointFailure);
    CHECK(bad.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Suppressed);
  }

  {
    auto part = makePart();
    MateRegistry registry;
    const auto first = path(kFixed);
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{1}, MateEndpoint{first, part.face},
              MateEndpoint{first, part.face}, PlanarMateOrientation::Aligned,
              0.0)) == MateRegistryMutationStatus::Invalid);
    CHECK(registry.size() == 0);
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{3}, MateEndpoint{path(kFixed), FaceReference{}},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Aligned, 0.0)) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{4}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Aligned,
              std::numeric_limits<double>::infinity())) ==
          MateRegistryMutationStatus::Invalid);
    MateSolverOptions boundedOptions;
    boundedOptions.maximumAbsoluteOffsetMm = 100.0;
    CHECK(registry.setOptions(boundedOptions) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{5}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Aligned, 101.0)) ==
          MateRegistryMutationStatus::Invalid);
    const auto revision = registry.revision();
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{2}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Opposed, -0.0)) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.revision() == revision + 1);
    CHECK(registry.find(MateId{2})->offsetMm() == 0.0);
    CHECK(!std::signbit(registry.find(MateId{2})->offsetMm()));
    CHECK(registry.fingerprint() != 0);
  }

  // A single owner-local registry is authoritative for every planar mate
  // kind while the legacy coincident lookup remains source compatible.
  {
    auto part = makePart();
    MateRegistry registry;
    CHECK(registry.insert(PlanarParallelMate(
              MateId{1}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Aligned)) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.find(MateId{1}) == nullptr);
    CHECK(registry.findParallel(MateId{1}));
    CHECK(registry.findMate(MateId{1}));
    CHECK(assemblyMateKind(*registry.findMate(MateId{1})) ==
          AssemblyMateKind::PlanarParallel);
    const auto parallelFingerprint = registry.fingerprint();
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(PlanarPerpendicularMate(
              MateId{1}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face})) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.findPerpendicular(MateId{1}));
    CHECK(registry.fingerprint() != parallelFingerprint);
    const auto perpendicularFingerprint = registry.fingerprint();
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(PlanarAngleMate{
              MateId{1}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              PlanarMateOrientation::Opposed, 0.4}) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.findAngle(MateId{1}));
    CHECK(assemblyMateKind(*registry.findMate(MateId{1})) ==
          AssemblyMateKind::PlanarAngle);
    CHECK(registry.fingerprint() != perpendicularFingerprint);
    CHECK(registry.insert(PlanarParallelMate(
              MateId{2}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              static_cast<PlanarMateOrientation>(99))) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PlanarCoincidentMate(
              MateId{3}, MateEndpoint{path(kFixed), part.face},
              MateEndpoint{path(kMoving), part.face},
              static_cast<PlanarMateOrientation>(99), 0.0)) ==
          MateRegistryMutationStatus::Invalid);
    const auto revision = registry.revision();
    const auto fingerprint = registry.fingerprint();
    auto nonFiniteOptions = registry.options();
    nonFiniteOptions.maximumAngularStepRad =
        std::numeric_limits<double>::infinity();
    CHECK(registry.setOptions(nonFiniteOptions) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(registry.revision() == revision);
    CHECK(registry.fingerprint() == fingerprint);
  }

  {
    auto cylinder = makeCylinderPart();
    MateRegistry registry;
    CHECK(registry.insert(AxisConcentricMate{
              MateId{1},
              MateAxisEndpoint{path(kFixed), LocalDatumAxis::Z},
              MateAxisEndpoint{path(kMoving),
                               CylindricalFaceAxis{cylinder.face}}}) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.findConcentric(MateId{1}));
    CHECK(assemblyMateKind(*registry.findMate(MateId{1})) ==
          AssemblyMateKind::AxisConcentric);
    const auto firstView =
        assemblyMateFirstEndpoint(*registry.findMate(MateId{1}));
    CHECK(std::holds_alternative<
          std::reference_wrapper<const MateAxisEndpoint>>(firstView));
    CHECK(std::get<std::reference_wrapper<const MateAxisEndpoint>>(firstView)
              .get()
              .occurrencePath == path(kFixed));
    CHECK(assemblyPlanarMateFirst(*registry.findMate(MateId{1})) == nullptr);
    CHECK(registry.insert(AxisConcentricMate{
              MateId{2},
              MateAxisEndpoint{path(kFixed), LocalDatumAxis::X},
              MateAxisEndpoint{path(kFixed), LocalDatumAxis::Y}}) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(AxisConcentricMate{
              MateId{3},
              MateAxisEndpoint{path(kFixed),
                               static_cast<LocalDatumAxis>(99)},
              MateAxisEndpoint{path(kMoving), LocalDatumAxis::Z}}) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(AxisConcentricMate{
              MateId{4},
              MateAxisEndpoint{
                  path(kFixed),
                  CylindricalFaceAxis{FaceReference{2102, 2103, 0}}},
              MateAxisEndpoint{path(kMoving), LocalDatumAxis::Z}}) ==
          MateRegistryMutationStatus::Invalid);
    auto invalidOwner = *cylinder.face.v2();
    invalidOwner.bodyId = kInvalidBodyId;
    CHECK(registry.insert(AxisConcentricMate{
              MateId{5},
              MateAxisEndpoint{
                  path(kFixed),
                  CylindricalFaceAxis{
                      FaceReference::fromV2(std::move(invalidOwner))}},
              MateAxisEndpoint{path(kMoving), LocalDatumAxis::Z}}) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(!RigidTransform3d::create(
        {std::numeric_limits<double>::infinity(), 0.0, 0.0},
        {1.0, 0.0, 0.0, 0.0}));
  }

  // Datum/datum, cylinder/cylinder and cylinder/datum use the same rank-four
  // infinite-line projection. Off-axis projection is purely transverse.
  {
    const double half = 0.25 * 3.14159265358979323846;
    const auto offAxis = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    const auto intersecting = RigidTransform3d::create(
        {0.0, 0.0, 0.0}, {std::cos(half), std::sin(half), 0.0, 0.0});
    const auto skew = RigidTransform3d::create(
        {3.0, 4.0, 5.0},
        {std::cos(half), std::sin(half), 0.0, 0.0});
    CHECK(offAxis && intersecting && skew);
    struct Scenario final {
      AxisSource first;
      AxisSource second;
      RigidTransform3d placement;
      bool expectUnchanged{};
      bool expectAxialPreserved{};
    };
    const std::array scenarios{
        Scenario{AxisSource::DatumZ, AxisSource::DatumZ,
                 RigidTransform3d::identity(), true, false},
        Scenario{AxisSource::DatumZ, AxisSource::DatumZ,
                 *offAxis.transform, false, true},
        Scenario{AxisSource::Cylinder, AxisSource::Cylinder,
                 *offAxis.transform, false, true},
        Scenario{AxisSource::DatumZ, AxisSource::DatumZ,
                 *intersecting.transform, false, false},
        Scenario{AxisSource::Cylinder, AxisSource::DatumZ, *skew.transform,
                 false, false}};
    for (const auto& scenario : scenarios) {
      auto product = makeConcentricProduct(
          scenario.first, scenario.second, scenario.placement);
      const auto result = solveAssemblyMates(*product, kRoot);
      CHECK(result.candidate);
      CHECK(result.status == MateSolveStatus::Underconstrained);
      CHECK(result.diagnostics.rank == 4);
      CHECK(result.diagnostics.remainingDegreesOfFreedom == 2);
      CHECK(result.diagnostics.maximumLinearResidualMm <=
            product->rootAssemblyDefinition()
                ->mateRegistry()
                .options()
                .linearToleranceMm);
      CHECK(result.diagnostics.maximumAngularResidualRad <=
            product->rootAssemblyDefinition()
                ->mateRegistry()
                .options()
                .angularToleranceRad);
      const auto changed = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != result.candidate->changes().end());
      CHECK((changed->afterPlacement == changed->before.placement) ==
            scenario.expectUnchanged);
      if (scenario.expectAxialPreserved)
        CHECK(std::abs(changed->afterPlacement.translationMm()[2] - 5.0) <=
              1.0e-9);
      const auto* relation =
          product->rootAssemblyDefinition()->mateRegistry().findConcentric(
              MateId{1});
      CHECK(relation);
      const auto& document =
          product->definitions().findPart(kPart)->document();
      const auto fixedAxis = transformAxisFrame(
          localAxisFrame(document, relation->first().reference),
          product->rootAssemblyDefinition()->find(kFixed)->placement());
      const auto movingAxis = transformAxisFrame(
          localAxisFrame(document, relation->second().reference),
          changed->afterPlacement);
      CHECK(std::abs(std::abs(dot3(fixedAxis.direction,
                                   movingAxis.direction)) -
                     1.0) <= 1.0e-9);
      CHECK(infiniteLineDistance(fixedAxis, movingAxis) <= 1.0e-9);
    }
  }

  // Exact shortest-rotation/no-slide oracle. A +90 degree X rotation maps
  // local Z to -Y; the unique shortest correction is -90 degrees about X,
  // so the final quaternion is identity. Intersecting keeps the axis anchor,
  // while skew removes only X/Y and preserves the axial Z coordinate.
  {
    const double half = 0.25 * 3.14159265358979323846;
    for (const auto& [translation, expected] :
         std::array{
             std::pair{std::array{0.0, 0.0, 0.0},
                       std::array{0.0, 0.0, 0.0}},
             std::pair{std::array{3.0, 4.0, 5.0},
                       std::array{0.0, 0.0, 5.0}}}) {
      const auto initial = RigidTransform3d::create(
          translation,
          {std::cos(half), std::sin(half), 0.0, 0.0});
      CHECK(initial);
      auto product = makeConcentricProduct(
          AxisSource::DatumZ, AxisSource::DatumZ, *initial.transform);
      const auto result = solveAssemblyMates(*product, kRoot);
      CHECK(result.candidate);
      const auto change = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(change != result.candidate->changes().end());
      for (std::size_t index = 0; index < 3; ++index)
        CHECK(std::abs(change->afterPlacement.translationMm()[index] -
                       expected[index]) <= 1.0e-9);
      const auto quaternion = change->afterPlacement.quaternionWxyz();
      CHECK(std::abs(std::abs(quaternion[0]) - 1.0) <= 1.0e-9);
      CHECK(std::abs(quaternion[1]) <= 1.0e-9);
      CHECK(std::abs(quaternion[2]) <= 1.0e-9);
      CHECK(std::abs(quaternion[3]) <= 1.0e-9);
    }
  }

  // The axis is unoriented: an exact sign reversal is already concentric and
  // must not add an arbitrary twist.
  {
    const auto opposite = RigidTransform3d::create(
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0, 0.0});
    CHECK(opposite);
    auto product = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ, *opposite.transform);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.candidate);
    const auto change = std::find_if(
        result.candidate->changes().begin(),
        result.candidate->changes().end(), [](const auto& value) {
          return value.before.occurrenceId == kMoving;
        });
    CHECK(change != result.candidate->changes().end());
    CHECK(change->afterPlacement == *opposite.transform);
  }

  // Grounded/moving policy is symmetric, and a successful axis apply invokes
  // the successor exactly once with the exact sealed placement.
  {
    const auto offAxis = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(offAxis);
    auto product = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ, *offAxis.transform, false,
        false, true);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.candidate);
    const auto fixedChange = std::find_if(
        result.candidate->changes().begin(),
        result.candidate->changes().end(), [](const auto& value) {
          return value.before.occurrenceId == kFixed;
        });
    const auto movingChange = std::find_if(
        result.candidate->changes().begin(),
        result.candidate->changes().end(), [](const auto& value) {
          return value.before.occurrenceId == kMoving;
        });
    CHECK(fixedChange != result.candidate->changes().end());
    CHECK(movingChange != result.candidate->changes().end());
    CHECK(fixedChange->afterPlacement != fixedChange->before.placement);
    CHECK(movingChange->afterPlacement == movingChange->before.placement);
    const auto expectedFixed = fixedChange->afterPlacement;
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, successor) ==
          MatePlacementApplyStatus::Applied);
    CHECK(successor.calls == 1);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          expectedFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          *offAxis.transform);
  }

  {
    auto duplicate = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ,
        RigidTransform3d::identity(), false, true, false, MateId{1}, true);
    const auto result = solveAssemblyMates(*duplicate, kRoot);
    CHECK(result.candidate);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.diagnostics.rank == 4);
    CHECK(result.diagnostics.redundantMateIds == std::vector{MateId{2}});
  }

  // Real mixed components share one rank/conflict pipeline. A cap normal is
  // parallel to the datum Z axis, so Parallel is redundant, Coincident adds
  // exactly one axial row, and Perpendicular is incompatible.
  {
    auto product = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarParallel);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.candidate);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.diagnostics.rank == 4);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 2);
    CHECK(result.diagnostics.redundantMateIds == std::vector{MateId{2}});
  }
  {
    auto product = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarCoincident);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.candidate);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.diagnostics.rank == 5);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 1);
  }
  {
    auto forward = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarAngle, false, MateId{11}, MateId{4});
    auto reversed = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarAngle, true, MateId{11}, MateId{4});
    const auto result = solveAssemblyMates(*forward, kRoot);
    const auto permuted = solveAssemblyMates(*reversed, kRoot);
    CHECK(result.candidate && permuted.candidate);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.diagnostics.rank == 5);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 1);
    CHECK(result.diagnostics.maximumAngularResidualRad <= 1.0e-9);
    CHECK(result.candidate->changes() == permuted.candidate->changes());
    const auto& placement = result.candidate->changes().back().afterPlacement;
    CHECK(placement.translationMm() ==
          (std::array<double, 3>{3.0, -6.3903420522385813e-17,
                                 4.2362293798953922e-17}));
    CHECK(placement.quaternionWxyz() ==
          (std::array<double, 4>{0.98006657784124196,
                                 0.19866933079505955, 0.0, 0.0}));
  }
  {
    auto product = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarPerpendicular);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(!result.candidate);
    CHECK(result.componentMateIds == (std::vector{MateId{1}, MateId{2}}));
    CHECK(result.diagnostics.conflictMateIds ==
          (std::vector{MateId{1}, MateId{2}}));
  }
  {
    auto forward = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarCoincident, false, MateId{9}, MateId{3});
    auto reversed = makeMixedConcentricPlanarProduct(
        AssemblyMateKind::PlanarCoincident, true, MateId{9}, MateId{3});
    const auto first = solveAssemblyMates(*forward, kRoot);
    const auto second = solveAssemblyMates(*reversed, kRoot);
    CHECK(first.candidate && second.candidate);
    CHECK(first.componentMateIds == (std::vector{MateId{3}, MateId{9}}));
    CHECK(second.componentMateIds == first.componentMateIds);
    CHECK(first.candidate->changes() == second.candidate->changes());
  }
  {
    auto forward =
        makeThreeNodeMixedProduct(false, MateId{11}, MateId{4});
    auto reversed =
        makeThreeNodeMixedProduct(true, MateId{11}, MateId{4});
    const auto first = solveAssemblyMates(*forward, kRoot);
    const auto second = solveAssemblyMates(*reversed, kRoot);
    CHECK(first.candidate && second.candidate);
    CHECK(first.componentMateIds == (std::vector{MateId{4}, MateId{11}}));
    CHECK(second.componentMateIds == first.componentMateIds);
    CHECK(first.candidate->changes() == second.candidate->changes());
    CHECK(first.candidate->changes().size() == 3);
    const auto& a = first.candidate->changes()[0];
    const auto& b = first.candidate->changes()[1];
    const auto& c = first.candidate->changes()[2];
    CHECK(a.before.occurrenceId == OccurrenceId{10});
    CHECK(b.before.occurrenceId == OccurrenceId{20});
    CHECK(c.before.occurrenceId == OccurrenceId{30});
    CHECK(a.afterPlacement == a.before.placement);
    CHECK(b.afterPlacement.translationMm() ==
          (std::array<double, 3>{0.0, 0.0, 5.0}));
    CHECK(c.afterPlacement.translationMm() ==
          (std::array<double, 3>{8.0, 4.0, 5.0}));
    CHECK(b.afterPlacement.quaternionWxyz() ==
          (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));
    CHECK(c.afterPlacement.quaternionWxyz() ==
          (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));
  }

  // Endpoint order, authored ID and free/free insertion policy do not change
  // the unoriented infinite-line solution.
  {
    const auto placement = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(placement);
    std::optional<RigidTransform3d> oracle;
    for (const bool swapped : {false, true}) {
      for (const bool grounded : {true, false}) {
        auto product = makeConcentricProduct(
            AxisSource::DatumZ, AxisSource::DatumZ, *placement.transform,
            swapped, grounded, false, swapped ? MateId{19} : MateId{7});
        const auto result = solveAssemblyMates(*product, kRoot);
        CHECK(result.candidate);
        CHECK(result.diagnostics.rank == 4);
        const auto change = std::find_if(
            result.candidate->changes().begin(),
            result.candidate->changes().end(), [](const auto& value) {
              return value.before.occurrenceId == kMoving;
            });
        CHECK(change != result.candidate->changes().end());
        if (!oracle)
          oracle = change->afterPlacement;
        else
          CHECK(change->afterPlacement == *oracle);
      }
    }
  }

  {
    const auto offAxis = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(offAxis);
    auto grounded = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ, *offAxis.transform, false,
        true, true);
    CHECK(solveAssemblyMates(*grounded, kRoot).status ==
          MateSolveStatus::Overconstrained);
  }

  // A cylindrical reference is authoritative: a planar resolved face is
  // Broken/NonCylindrical, while suppressed paths retain the existing
  // taxonomy.
  {
    auto nonCylinder = makeConcentricProduct(
        AxisSource::Cylinder, AxisSource::Cylinder,
        RigidTransform3d::identity(), false, true, false, MateId{1}, false,
        true);
    const auto result = solveAssemblyMates(*nonCylinder, kRoot);
    CHECK(result.status == MateSolveStatus::Broken);
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::NonCylindrical);
  }
  {
    auto suppressed = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ,
        RigidTransform3d::identity(), false, true, false, MateId{1}, false,
        false, true);
    const auto result = solveAssemblyMates(*suppressed, kRoot);
    CHECK(result.status == MateSolveStatus::Broken);
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Suppressed);
  }
  {
    auto missingPath = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ);
    auto& registry = const_cast<MateRegistry&>(
        missingPath->rootAssemblyDefinition()->mateRegistry());
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(AxisConcentricMate{
              MateId{1},
              MateAxisEndpoint{path(OccurrenceId{999}), LocalDatumAxis::Z},
              MateAxisEndpoint{path(kMoving), LocalDatumAxis::Z}}) ==
          MateRegistryMutationStatus::Inserted);
    const auto result = solveAssemblyMates(*missingPath, kRoot);
    CHECK(result.status == MateSolveStatus::Broken);
    CHECK(result.componentMateIds == std::vector{MateId{1}});
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->mateId == MateId{1});
    CHECK(!result.diagnostics.endpointFailure->secondEndpoint);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::PathMissing);
  }

  // The work ceiling is exact for datum, cylindrical, and mixed endpoints.
  for (const auto sources :
       {std::pair{AxisSource::DatumZ, AxisSource::DatumZ},
        std::pair{AxisSource::Cylinder, AxisSource::Cylinder},
        std::pair{AxisSource::Cylinder, AxisSource::DatumZ}}) {
    const auto placement = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(placement);
    auto baselineProduct = makeConcentricProduct(
        sources.first, sources.second, *placement.transform);
    const auto baseline = solveAssemblyMates(*baselineProduct, kRoot);
    CHECK(baseline.candidate);
    CHECK(baseline.diagnostics.requiredWorkUnits > 1);
    const auto required = baseline.diagnostics.requiredWorkUnits;
    for (const auto allowance : {required, required - 1}) {
      auto product = makeConcentricProduct(
          sources.first, sources.second, *placement.transform);
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      auto authored = registry.options();
      authored.maximumWorkUnits = allowance;
      CHECK(registry.setOptions(authored) ==
            MateRegistryMutationStatus::Inserted);
      const auto result = solveAssemblyMates(*product, kRoot);
      CHECK(allowance == required
                ? result.candidate.has_value()
                : result.status == MateSolveStatus::ResourceFailure);
    }
  }

  // Candidate seals include the axis discriminator/datum/reference data and
  // apply rollback remains atomic for successor rejection/resource failure.
  {
    const auto placement = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(placement);
    auto product = makeConcentricProduct(
        AxisSource::DatumZ, AxisSource::DatumZ, *placement.transform);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto before =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor rejected(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, rejected) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          before);
    Successor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, resource) ==
          MatePlacementApplyStatus::ResourceFailure);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          before);

    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(AxisConcentricMate{
              MateId{1}, MateAxisEndpoint{path(kFixed), LocalDatumAxis::Y},
              MateAxisEndpoint{path(kMoving), LocalDatumAxis::Y}}) ==
          MateRegistryMutationStatus::Inserted);
    Successor accepted(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, accepted) ==
          MatePlacementApplyStatus::Stale);
    CHECK(accepted.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          before);
  }

  // Parallel is orientation-only (rank two), and both authored senses use
  // the same mixed localized solve/apply pipeline.
  for (const auto orientation : {PlanarMateOrientation::Aligned,
                                 PlanarMateOrientation::Opposed}) {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel, orientation}}, false);
    const auto before =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
    CHECK(result.diagnostics.rank == 2);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 4);
    CHECK(result.diagnostics.maximumLinearResidualMm == 0.0);
    CHECK(result.diagnostics.maximumAngularResidualRad <=
          product->rootAssemblyDefinition()
              ->mateRegistry()
              .options()
              .angularToleranceRad);
    const auto* relation =
        product->rootAssemblyDefinition()->mateRegistry().findMate(MateId{1});
    CHECK(relation);
    const auto& movingEndpoint =
        assemblyPlanarMateFirst(*relation)->occurrencePath.entries().front() == kMoving
            ? *assemblyPlanarMateFirst(*relation)
            : *assemblyPlanarMateSecond(*relation);
    const auto& fixedEndpoint =
        assemblyPlanarMateFirst(*relation)->occurrencePath.entries().front() == kFixed
            ? *assemblyPlanarMateFirst(*relation)
            : *assemblyPlanarMateSecond(*relation);
    const auto& partDocument =
        product->definitions().findPart(kPart)->document();
    const auto movingLocal = localFaceFrame(partDocument, movingEndpoint.face);
    const auto fixedLocal = localFaceFrame(partDocument, fixedEndpoint.face);
    const auto movingBefore = transformFaceFrame(movingLocal, before);
    const auto fixedWorld = transformFaceFrame(
        fixedLocal,
        product->rootAssemblyDefinition()->find(kFixed)->placement());
    const auto changed = std::find_if(
        result.candidate->changes().begin(), result.candidate->changes().end(),
        [](const auto& value) {
          return value.before.occurrenceId == kMoving;
        });
    CHECK(changed != result.candidate->changes().end());
    const auto movingAfter =
        transformFaceFrame(movingLocal, changed->afterPlacement);
    CHECK(pointDistance(movingBefore.point, movingAfter.point) <= 1.0e-9);
    CHECK(std::abs(dot3(fixedWorld.normal, movingAfter.normal) -
                   (orientation == PlanarMateOrientation::Aligned ? 1.0
                                                                  : -1.0)) <=
          1.0e-9);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, successor) ==
          MatePlacementApplyStatus::Applied);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() !=
          before);
  }

  // Endpoint permutation must not alter which occurrence moves or the
  // projected placement for either authored parallel sense.
  for (const auto orientation : {PlanarMateOrientation::Aligned,
                                 PlanarMateOrientation::Opposed}) {
    std::optional<RigidTransform3d> expectedPlacement;
    std::optional<std::array<double, 3>> expectedPivot;
    for (const bool swapped : {false, true}) {
      auto product = makeAngularProduct(
          {{AssemblyMateKind::PlanarParallel, orientation}}, false, swapped);
      const auto movingBefore =
          product->rootAssemblyDefinition()->find(kMoving)->placement();
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.candidate);
      const auto changed = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != result.candidate->changes().end());
      const auto* relation = product->rootAssemblyDefinition()
                                 ->mateRegistry()
                                 .findMate(MateId{1});
      CHECK(relation);
      const auto& first = *assemblyPlanarMateFirst(*relation);
      const auto& second = *assemblyPlanarMateSecond(*relation);
      const auto& movingEndpoint =
          first.occurrencePath.entries().front() == kMoving ? first : second;
      const auto& fixedEndpoint =
          first.occurrencePath.entries().front() == kFixed ? first : second;
      const auto& document =
          product->definitions().findPart(kPart)->document();
      const auto movingLocal = localFaceFrame(document, movingEndpoint.face);
      const auto fixedLocal = localFaceFrame(document, fixedEndpoint.face);
      const auto beforeFrame = transformFaceFrame(movingLocal, movingBefore);
      const auto afterFrame =
          transformFaceFrame(movingLocal, changed->afterPlacement);
      const auto fixedFrame = transformFaceFrame(
          fixedLocal,
          product->rootAssemblyDefinition()->find(kFixed)->placement());
      CHECK(pointDistance(beforeFrame.point, afterFrame.point) <= 1.0e-9);
      CHECK(std::abs(dot3(fixedFrame.normal, afterFrame.normal) -
                     (orientation == PlanarMateOrientation::Aligned ? 1.0
                                                                    : -1.0)) <=
            1.0e-9);
      if (expectedPlacement) {
        CHECK(changed->afterPlacement == *expectedPlacement);
        CHECK(pointDistance(afterFrame.point, *expectedPivot) <= 1.0e-12);
      } else {
        expectedPlacement = changed->afterPlacement;
        expectedPivot = afterFrame.point;
      }
    }
  }

  // A collinear perpendicular input takes the deterministic finite fallback.
  // Endpoint permutation must produce the same moving occurrence placement.
  {
    std::optional<RigidTransform3d> referencePlacement;
    for (const bool swapped : {false, true}) {
      auto product = makeAngularProduct(
          {{AssemblyMateKind::PlanarPerpendicular,
            PlanarMateOrientation::Aligned}},
          true, swapped);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.status == MateSolveStatus::Underconstrained);
      CHECK(result.candidate);
      CHECK(result.diagnostics.rank == 1);
      CHECK(result.diagnostics.remainingDegreesOfFreedom == 5);
      CHECK(result.diagnostics.maximumAngularResidualRad <=
            product->rootAssemblyDefinition()
                ->mateRegistry()
                .options()
                .angularToleranceRad);
      const auto* relation = product->rootAssemblyDefinition()
                                 ->mateRegistry()
                                 .findMate(MateId{1});
      CHECK(relation);
      const auto& firstEndpoint = *assemblyPlanarMateFirst(*relation);
      const auto& secondEndpoint = *assemblyPlanarMateSecond(*relation);
      const auto& movingEndpoint =
          firstEndpoint.occurrencePath.entries().front() == kMoving
              ? firstEndpoint
              : secondEndpoint;
      const auto& fixedEndpoint =
          firstEndpoint.occurrencePath.entries().front() == kFixed
              ? firstEndpoint
              : secondEndpoint;
      const auto& partDocument =
          product->definitions().findPart(kPart)->document();
      const auto movingLocal =
          localFaceFrame(partDocument, movingEndpoint.face);
      const auto fixedLocal = localFaceFrame(partDocument, fixedEndpoint.face);
      const auto movingBefore = transformFaceFrame(
          movingLocal,
          product->rootAssemblyDefinition()->find(kMoving)->placement());
      const auto fixedWorld = transformFaceFrame(
          fixedLocal,
          product->rootAssemblyDefinition()->find(kFixed)->placement());
      const auto changed = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != result.candidate->changes().end());
      const auto movingAfter =
          transformFaceFrame(movingLocal, changed->afterPlacement);
      CHECK(pointDistance(movingBefore.point, movingAfter.point) <= 1.0e-9);
      CHECK(std::abs(dot3(fixedWorld.normal, movingAfter.normal)) <= 1.0e-9);
      Successor successor(ProductDocumentSuccessorStatus::Prepared);
      CHECK(product->applyMatePlacementCandidate(*result.candidate,
                                                 successor) ==
            MatePlacementApplyStatus::Applied);
      const auto placement =
          product->rootAssemblyDefinition()->find(kMoving)->placement();
      if (referencePlacement)
        CHECK(placement == *referencePlacement);
      else
        referencePlacement = placement;
    }
  }

  // A7c4 Angle is one orientation-only scalar row. Endpoint order, authored
  // branch and grounded-side selection preserve the selected face pivot.
  for (const auto orientation : {PlanarMateOrientation::Aligned,
                                 PlanarMateOrientation::Opposed}) {
    std::optional<RigidTransform3d> expected;
    for (const bool swapped : {false, true}) {
      constexpr double target = 0.52359877559829887308;
      auto product = makeAngularProduct(
          {{AssemblyMateKind::PlanarAngle, orientation, MateId{7}, 2,
            target}},
          false, swapped);
      const auto before =
          product->rootAssemblyDefinition()->find(kMoving)->placement();
      const auto result = solveAssemblyMates(*product, kRoot);
      CHECK(result.status == MateSolveStatus::Underconstrained);
      CHECK(result.candidate);
      CHECK(result.diagnostics.rank == 1);
      CHECK(result.diagnostics.remainingDegreesOfFreedom == 5);
      CHECK(result.diagnostics.maximumLinearResidualMm == 0.0);
      CHECK(result.diagnostics.maximumAngularResidualRad <= 1.0e-9);
      const auto* relation = product->rootAssemblyDefinition()
                                 ->mateRegistry()
                                 .findAngle(MateId{7});
      CHECK(relation);
      const auto& movingEndpoint =
          relation->first().occurrencePath.entries().front() == kMoving
              ? relation->first()
              : relation->second();
      const auto& fixedEndpoint =
          relation->first().occurrencePath.entries().front() == kFixed
              ? relation->first()
              : relation->second();
      const auto& document =
          product->definitions().findPart(kPart)->document();
      const auto movingLocal = localFaceFrame(document, movingEndpoint.face);
      const auto fixedLocal = localFaceFrame(document, fixedEndpoint.face);
      const auto beforeFrame = transformFaceFrame(movingLocal, before);
      const auto fixedFrame = transformFaceFrame(
          fixedLocal,
          product->rootAssemblyDefinition()->find(kFixed)->placement());
      const auto changed = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != result.candidate->changes().end());
      const auto afterFrame =
          transformFaceFrame(movingLocal, changed->afterPlacement);
      CHECK(pointDistance(beforeFrame.point, afterFrame.point) <= 1.0e-9);
      const double expectedDot =
          (orientation == PlanarMateOrientation::Aligned ? 1.0 : -1.0) *
          std::cos(target);
      CHECK(std::abs(dot3(fixedFrame.normal, afterFrame.normal) - expectedDot) <=
            1.0e-9);
      if (expected)
        CHECK(changed->afterPlacement == *expected);
      else
        expected = changed->afterPlacement;
    }
  }

  // Grounded/free and free/free policies select the same physical mover under
  // endpoint reversal; the one-row Angle result remains deterministic.
  for (const auto grounding :
       {std::pair{true, false}, std::pair{false, true},
        std::pair{false, false}}) {
    std::optional<RigidTransform3d> expected;
    for (const bool swapped : {false, true}) {
      auto product = makeAngularProduct(
          {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
            MateId{3}, 2, 0.4}},
          false, swapped, grounding.first, grounding.second);
      const auto solved = solveAssemblyMates(*product, kRoot);
      CHECK(solved.candidate);
      CHECK(solved.diagnostics.rank == 1);
      const OccurrenceId moved = grounding.second ? kFixed : kMoving;
      const auto changed = std::find_if(
          solved.candidate->changes().begin(),
          solved.candidate->changes().end(), [moved](const auto& value) {
            return value.before.occurrenceId == moved;
          });
      CHECK(changed != solved.candidate->changes().end());
      CHECK(changed->afterPlacement != changed->before.placement);
      if (expected)
        CHECK(changed->afterPlacement == *expected);
      else
        expected = changed->afterPlacement;
    }
  }

  // Collinear unsatisfied Angle has no authored tangent branch. Movable input
  // is a typed retained failure; an immovable pair is truthfully conflicting.
  {
    auto movable = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        true);
    const auto broken = solveAssemblyMates(*movable, kRoot);
    CHECK(broken.status == MateSolveStatus::Broken);
    CHECK(!broken.candidate);
    CHECK(broken.diagnostics.relationFailure);
    CHECK(broken.diagnostics.relationFailure->mateId == MateId{1});
    CHECK(broken.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);

    auto immovable = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        true, false, true, true);
    CHECK(solveAssemblyMates(*immovable, kRoot).status ==
          MateSolveStatus::Overconstrained);
  }

  // A representable tangent below the generic 1e-15 normalization cutoff is
  // still a valid cone direction. Only exact zero is the typed degeneracy.
  {
    constexpr double target = 2.0e-9;
    auto product = makeNearCollinearAngleProduct(5.0e-16, target);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.status != MateSolveStatus::NonConvergent);
    CHECK(solved.candidate);
    CHECK(solved.diagnostics.rank == 1);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Applied);
    CHECK(successor.calls == 1);
    const auto fixedPoint = solveAssemblyMates(*product, kRoot);
    CHECK(fixedPoint.candidate);
    CHECK(fixedPoint.diagnostics.rank == 1);
    CHECK(fixedPoint.status != MateSolveStatus::NonConvergent);

    auto exact = makeNearCollinearAngleProduct(0.0, target);
    const auto broken = solveAssemblyMates(*exact, kRoot);
    CHECK(broken.status == MateSolveStatus::Broken);
    CHECK(!broken.candidate);
    CHECK(broken.diagnostics.relationFailure);
    CHECK(broken.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);
  }

  // A relation that is already exactly on its authored cone is a true fixed
  // point. The scale-free cone rotation must return identity for sine == 0
  // instead of turning the satisfied relation into NonConvergent.
  {
    constexpr double target = 0.4;
    auto product = makeNearCollinearAngleProduct(std::tan(target), target);
    const auto before =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(solved.status == MateSolveStatus::Underconstrained);
    CHECK(solved.diagnostics.rank == 1);
    CHECK(solved.diagnostics.maximumAngularResidualRad <=
          product->rootAssemblyDefinition()
              ->mateRegistry()
              .options()
              .angularToleranceRad);
    const auto changed = std::find_if(
        solved.candidate->changes().begin(), solved.candidate->changes().end(),
        [](const auto& value) { return value.before.occurrenceId == kMoving; });
    CHECK(changed != solved.candidate->changes().end());
    CHECK(changed->afterPlacement == before);
  }

  // The model tolerance floor is tied to the quaternion canonical grid.  The
  // exact boundary is authored and solvable; one representable value below
  // it is rejected by both direct option validation and registry mutation.
  {
    MateSolverOptions boundary;
    boundary.angularToleranceRad = kMinimumMateAngularToleranceRad;
    CHECK(boundary.isValid());
    auto below = boundary;
    below.angularToleranceRad = std::nextafter(
        kMinimumMateAngularToleranceRad, 0.0);
    CHECK(!below.isValid());
    MateRegistry registry;
    CHECK(registry.setOptions(boundary) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.setOptions(below) == MateRegistryMutationStatus::Invalid);

    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        false);
    auto& authored = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    auto options = authored.options();
    options.angularToleranceRad = kMinimumMateAngularToleranceRad;
    CHECK(authored.setOptions(options) == MateRegistryMutationStatus::Inserted);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(solved.status != MateSolveStatus::NonConvergent);
    CHECK(solved.diagnostics.rank == 1);
  }

  // Authoritative planar faces with INTERNAL/EXTERNAL orientation are not
  // silently treated as FORWARD; the precise endpoint failure is retained.
  for (const auto orientation : {TopAbs_INTERNAL, TopAbs_EXTERNAL}) {
    auto product = makeNearCollinearAngleProduct(0.25, 0.4, orientation);
    const auto rejected = solveAssemblyMates(*product, kRoot);
    CHECK(rejected.status == MateSolveStatus::Broken);
    CHECK(!rejected.candidate);
    CHECK(rejected.diagnostics.endpointFailure);
    CHECK(rejected.diagnostics.endpointFailure->mateId == MateId{1});
    CHECK(rejected.diagnostics.endpointFailure->secondEndpoint);
    CHECK(rejected.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::InvalidFaceOrientation);
  }

  // Reversal laws use two provenance-distinct faces on the same authored
  // planes, avoiding the false geometry change of selecting an opposite cap.
  {
    const auto solvedPlacement = [](TopAbs_Orientation firstOrientation,
                                    TopAbs_Orientation secondOrientation,
                                    PlanarMateOrientation orientation) {
      auto product = makeNearCollinearAngleProduct(
          0.25, 0.4, secondOrientation, firstOrientation, orientation);
      const auto solved = solveAssemblyMates(*product, kRoot);
      CHECK(solved.candidate);
      const auto changed = std::find_if(
          solved.candidate->changes().begin(),
          solved.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != solved.candidate->changes().end());
      return changed->afterPlacement;
    };
    const auto baseline = solvedPlacement(
        TopAbs_FORWARD, TopAbs_FORWARD, PlanarMateOrientation::Aligned);
    const auto oneReversed = solvedPlacement(
        TopAbs_FORWARD, TopAbs_REVERSED, PlanarMateOrientation::Opposed);
    const auto bothReversed = solvedPlacement(
        TopAbs_REVERSED, TopAbs_REVERSED, PlanarMateOrientation::Aligned);
    const auto equivalent = [](const RigidTransform3d& left,
                               const RigidTransform3d& right) {
      CHECK(pointDistance(left.translationMm(), right.translationMm()) <=
            1.0e-9);
      CHECK(std::abs(std::abs(dot4(left.quaternionWxyz(),
                                   right.quaternionWxyz())) -
                     1.0) <= 1.0e-10);
    };
    equivalent(oneReversed, baseline);
    equivalent(bothReversed, baseline);
  }

  // Independent arbitrary-3D oracle. Expected poses below are literal frozen
  // values from the analytic plane/pivot construction, not values calculated
  // with solver helpers in the assertion path.
  {
    constexpr std::array expectedTranslations{
        std::array<double, 3>{2.4790004649404462, 2.968876668856256,
                              3.9634387993685731},
        std::array<double, 3>{-4.1236844543369804, 1.8684291823100194,
                              -3.189469863181972}};
    constexpr std::array expectedQuaternions{
        std::array<double, 4>{0.88448925188354732, -0.31104037844518828,
                              -0.15552018922259236,
                              0.31104037844518828},
        std::array<double, 4>{0.88448925188354732, 0.31104037844518828,
                              0.15552018922259236,
                              -0.31104037844518828}};
    std::array<std::optional<RigidTransform3d>, 2> baseline;
    std::size_t orientationIndex = 0;
    for (const auto orientation : {PlanarMateOrientation::Aligned,
                                   PlanarMateOrientation::Opposed}) {
      for (const bool swapped : {false, true}) {
        auto product = makeAnalyticAngleProduct(
            orientation, swapped, TopAbs_FORWARD, TopAbs_FORWARD,
            swapped ? MateId{91} : MateId{7});
        const auto before =
            product->rootAssemblyDefinition()->find(kMoving)->placement();
        const auto solved = solveAssemblyMates(*product, kRoot);
        CHECK(solved.candidate);
        CHECK(solved.diagnostics.rank == 1);
        const auto changed = std::find_if(
            solved.candidate->changes().begin(),
            solved.candidate->changes().end(), [](const auto& value) {
              return value.before.occurrenceId == kMoving;
            });
        CHECK(changed != solved.candidate->changes().end());
        const auto& actual = changed->afterPlacement;
        CHECK(actual.translationMm() == expectedTranslations[orientationIndex]);
        CHECK(actual.quaternionWxyz() == expectedQuaternions[orientationIndex]);
        if (!swapped) {
          baseline[orientationIndex] = actual;
        } else {
          CHECK(baseline[orientationIndex]);
          CHECK(actual == *baseline[orientationIndex]);
        }
        const auto& document =
            product->definitions().findPart(kPart)->document();
        const auto* relation = product->rootAssemblyDefinition()
                                   ->mateRegistry()
                                   .findAngle(swapped ? MateId{91} : MateId{7});
        CHECK(relation);
        const auto& movingEndpoint =
            relation->first().occurrencePath.entries().front() == kMoving
                ? relation->first()
                : relation->second();
        const auto local = localFaceFrame(document, movingEndpoint.face);
        CHECK(pointDistance(transformFaceFrame(local, before).point,
                            transformFaceFrame(local, actual).point) <= 1.0e-12);
      }
      ++orientationIndex;
    }

    auto oneReversed = makeAnalyticAngleProduct(
        PlanarMateOrientation::Opposed, false, TopAbs_FORWARD,
        TopAbs_REVERSED);
    auto bothReversed = makeAnalyticAngleProduct(
        PlanarMateOrientation::Aligned, false, TopAbs_REVERSED,
        TopAbs_REVERSED);
    const auto reversedSolved = solveAssemblyMates(*oneReversed, kRoot);
    const auto bothSolved = solveAssemblyMates(*bothReversed, kRoot);
    CHECK(reversedSolved.candidate && bothSolved.candidate && baseline[0]);
    CHECK(reversedSolved.candidate->changes().back().afterPlacement ==
          *baseline[0]);
    CHECK(bothSolved.candidate->changes().back().afterPlacement ==
          *baseline[0]);

    // Applying one common non-principal rigid frame with a huge translation
    // commutes with the solve. Quaternion canonicalization is byte-exact;
    // world translation retains a small absolute allowance at 1e9 scale.
    const auto frame = RigidTransform3d::create(
        {1.0e9, -1.0e9, 5.0e8}, {0.91, 0.11, -0.23, 0.31});
    CHECK(frame && baseline[0]);
    auto covariant = makeAnalyticAngleProduct(
        PlanarMateOrientation::Aligned, false, TopAbs_FORWARD,
        TopAbs_FORWARD, MateId{1}, *frame.transform);
    const auto covariantSolved = solveAssemblyMates(*covariant, kRoot);
    CHECK(covariantSolved.candidate);
    const auto expectedWorld =
        RigidTransform3d::compose(*frame.transform, *baseline[0]);
    CHECK(expectedWorld);
    const auto& actualWorld =
        covariantSolved.candidate->changes().back().afterPlacement;
    CHECK(actualWorld.quaternionWxyz() ==
          expectedWorld.transform->quaternionWxyz());
    CHECK(pointDistance(actualWorld.translationMm(),
                        expectedWorld.transform->translationMm()) <= 5.0e-6);

    for (const auto grounding :
         {std::pair{false, true}, std::pair{false, false}}) {
      auto variant = makeAnalyticAngleProduct(
          PlanarMateOrientation::Aligned, true, TopAbs_FORWARD,
          TopAbs_FORWARD, MateId{37}, RigidTransform3d::identity(),
          grounding.first, grounding.second);
      const auto variantSolved = solveAssemblyMates(*variant, kRoot);
      CHECK(variantSolved.candidate);
      const OccurrenceId moved = grounding.second ? kFixed : kMoving;
      const auto variantChange = std::find_if(
          variantSolved.candidate->changes().begin(),
          variantSolved.candidate->changes().end(), [moved](const auto& value) {
            return value.before.occurrenceId == moved;
          });
      CHECK(variantChange != variantSolved.candidate->changes().end());
      const auto& value = variantChange->afterPlacement;
      if (grounding.second) {
        CHECK(value.translationMm() ==
              (std::array<double, 3>{0.80422189505343789,
                                     3.1078502255920615,
                                     2.3581470078494693}));
        CHECK(value.quaternionWxyz() ==
              (std::array<double, 4>{0.88448925188354732,
                                     0.31104037844518828,
                                     0.15552018922259236,
                                     -0.31104037844518828}));
      } else {
        CHECK(value.translationMm() == expectedTranslations[0]);
        CHECK(value.quaternionWxyz() == expectedQuaternions[0]);
      }
    }
  }

  // Structural and operational angle boundaries are model-owned. Tightening
  // options cannot strand an already-authored relation outside its tolerance.
  {
    auto part = makeAxisPart();
    const MateEndpoint first{path(kFixed), part.faces[2]};
    const MateEndpoint second{path(kMoving), part.faces[2]};
    MateRegistry registry;
    for (const double invalid : {
             0.0, -0.0, -1.0, 1.57079632679489661923,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()})
      CHECK(registry.insert(PlanarAngleMate{
                MateId{1}, first, second, PlanarMateOrientation::Aligned,
                invalid}) == MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Aligned,
              0.25}) == MateRegistryMutationStatus::Inserted);
    auto options = registry.options();
    options.angularToleranceRad = 0.25;
    CHECK(registry.setOptions(options) ==
          MateRegistryMutationStatus::Invalid);
    options.angularToleranceRad = 0.2;
    CHECK(registry.setOptions(options) ==
          MateRegistryMutationStatus::Inserted);

    constexpr double tolerance = 1.0e-6;
    const double halfPi = 1.57079632679489661923;
    MateRegistry lower;
    auto lowerOptions = lower.options();
    lowerOptions.angularToleranceRad = tolerance;
    CHECK(lower.setOptions(lowerOptions) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(lower.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Aligned,
              tolerance}) == MateRegistryMutationStatus::Invalid);
    CHECK(lower.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Aligned,
              std::nextafter(tolerance,
                             std::numeric_limits<double>::infinity())}) ==
          MateRegistryMutationStatus::Inserted);
    MateRegistry upper;
    auto upperOptions = upper.options();
    upperOptions.angularToleranceRad = tolerance;
    CHECK(upper.setOptions(upperOptions) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(upper.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Aligned,
              halfPi - tolerance}) == MateRegistryMutationStatus::Invalid);
    CHECK(upper.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Aligned,
              std::nextafter(halfPi - tolerance, 0.0)}) ==
          MateRegistryMutationStatus::Inserted);
  }

  // Stable atan2 rank rows remain rank one immediately inside both authored
  // operational boundaries.
  for (const double target : {
           std::nextafter(1.0e-9,
                          std::numeric_limits<double>::infinity()),
           std::nextafter(1.57079632679489661923 - 1.0e-9, 0.0)}) {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, target}},
        false);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(solved.diagnostics.rank == 1);
    CHECK(solved.status != MateSolveStatus::NonConvergent);
  }

  // One projection sweep has an exact angular-step boundary.  The complete
  // representable step reaches the frozen canonical quaternion; a step short
  // by four model tolerance quanta leaves a measured residual above tolerance
  // and is NonConvergent.  The separation deliberately exceeds pose-grid
  // quantization, so this catches a genuine <= versus < boundary error.
  {
    constexpr double target = 0.4;
    constexpr double tolerance = kMinimumMateAngularToleranceRad;
    const double exactStep = 1.57079632679489661923 - target;
    const auto solveWithStep = [&](double step) {
      auto product = makeAngularProduct(
          {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
            MateId{1}, 2, target}},
          false);
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      auto options = registry.options();
      options.angularToleranceRad = tolerance;
      options.maximumIterations = 1;
      options.maximumAngularStepRad = step;
      CHECK(registry.setOptions(options) ==
            MateRegistryMutationStatus::Inserted);
      return solveAssemblyMates(*product, kRoot);
    };
    const auto exact = solveWithStep(exactStep);
    CHECK(exact.candidate);
    const auto changed = std::find_if(
        exact.candidate->changes().begin(), exact.candidate->changes().end(),
        [](const auto& value) { return value.before.occurrenceId == kMoving; });
    CHECK(changed != exact.candidate->changes().end());
    CHECK(changed->afterPlacement.quaternionWxyz() ==
          (std::array<double, 4>{0.980066577841242,
                                 0.19866933079505955, 0.0, 0.0}));
    const auto shortStep =
        solveWithStep(exactStep - 4.0 * kMinimumMateAngularToleranceRad);
    CHECK(shortStep.status == MateSolveStatus::NonConvergent);
    CHECK(!shortStep.candidate);
    CHECK(shortStep.diagnostics.maximumAngularResidualRad > tolerance);
  }

  // Symmetric angular relations canonicalize their unordered endpoint pair.
  // Mate IDs, insertion order and endpoint order cannot change the solved
  // placements of a satisfiable mixed component.
  {
    const std::vector<AngularMateSpec> canonical{
        {AssemblyMateKind::PlanarParallel, PlanarMateOrientation::Aligned,
         MateId{1}, 0},
        {AssemblyMateKind::PlanarPerpendicular,
         PlanarMateOrientation::Aligned, MateId{2}, 2}};
    const std::vector<AngularMateSpec> permuted{
        {AssemblyMateKind::PlanarParallel, PlanarMateOrientation::Aligned,
         MateId{90}, 0},
        {AssemblyMateKind::PlanarPerpendicular,
         PlanarMateOrientation::Aligned, MateId{3}, 2}};
    std::optional<RigidTransform3d> expected;
    for (int permutation = 0; permutation < 2; ++permutation) {
      auto product = makeAngularProduct(
          permutation == 0 ? canonical : permuted, true,
          permutation != 0, true, false, permutation != 0);
      const auto movingBefore =
          product->rootAssemblyDefinition()->find(kMoving)->placement();
      const auto result = solvePlanarMates(
          *product, kRoot,
          MateSolveRequest{permutation == 0 ? MateId{1} : MateId{3},
                           std::nullopt});
      CHECK(result.candidate);
      CHECK(result.status == MateSolveStatus::Underconstrained);
      CHECK(result.diagnostics.rank == 3);
      const auto changed = std::find_if(
          result.candidate->changes().begin(),
          result.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != result.candidate->changes().end());
      CHECK(changed->afterPlacement != movingBefore);
      const auto& document =
          product->definitions().findPart(kPart)->document();
      for (const auto& [mateId, relation] :
           product->rootAssemblyDefinition()->mateRegistry().relations()) {
        static_cast<void>(mateId);
        const auto& first = *assemblyPlanarMateFirst(relation);
        const auto& second = *assemblyPlanarMateSecond(relation);
        const auto& movingEndpoint =
            first.occurrencePath.entries().front() == kMoving ? first : second;
        const auto& fixedEndpoint =
            first.occurrencePath.entries().front() == kFixed ? first : second;
        const auto movingLocal =
            localFaceFrame(document, movingEndpoint.face);
        const auto fixedLocal = localFaceFrame(document, fixedEndpoint.face);
        const auto beforeFrame =
            transformFaceFrame(movingLocal, movingBefore);
        const auto afterFrame =
            transformFaceFrame(movingLocal, changed->afterPlacement);
        const auto fixedFrame = transformFaceFrame(
            fixedLocal,
            product->rootAssemblyDefinition()->find(kFixed)->placement());
        const double solvedDot = dot3(fixedFrame.normal, afterFrame.normal);
        if (assemblyMateKind(relation) ==
            AssemblyMateKind::PlanarPerpendicular) {
          CHECK(std::abs(solvedDot) <= 1.0e-9);
          CHECK(pointDistance(beforeFrame.point, afterFrame.point) <= 1.0e-9);
        } else {
          CHECK(std::abs(solvedDot - 1.0) <= 1.0e-9);
        }
      }
      if (expected)
        CHECK(changed->afterPlacement == *expected);
      else
        expected = changed->afterPlacement;
    }
  }

  for (const auto grounding :
       {std::pair{false, true}, std::pair{false, false}}) {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel,
          PlanarMateOrientation::Aligned}},
        false, false, grounding.first, grounding.second);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.candidate);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.diagnostics.rank == 2);
    CHECK(result.diagnostics.maximumAngularResidualRad <=
          product->rootAssemblyDefinition()
              ->mateRegistry()
              .options()
              .angularToleranceRad);
  }
  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel,
          PlanarMateOrientation::Aligned}},
        false, false, true, true);
    CHECK(solvePlanarMates(*product, kRoot).status ==
          MateSolveStatus::Overconstrained);
  }

  // Mixed semantic rank identifies angular duplicates and contradictory
  // orientation families without changing coincident-only rank behavior.
  {
    auto duplicate = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4},
         {AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{2}, 2, 0.4}},
        false);
    const auto result = solveAssemblyMates(*duplicate, kRoot);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.diagnostics.rank == 1);
    CHECK(result.diagnostics.redundantMateIds ==
          std::vector<MateId>{MateId{2}});
  }
  {
    auto conflict = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.35},
         {AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{2}, 2, 0.65}},
        false);
    const auto result = solveAssemblyMates(*conflict, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(!result.candidate);
    CHECK(result.diagnostics.conflictMateIds ==
          (std::vector<MateId>{MateId{1}, MateId{2}}));
  }
  {
    auto duplicate = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel, PlanarMateOrientation::Aligned},
         {AssemblyMateKind::PlanarParallel, PlanarMateOrientation::Aligned}},
        false);
    const auto result = solvePlanarMates(*duplicate, kRoot);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.diagnostics.rank == 2);
    CHECK(result.diagnostics.redundantMateIds == std::vector{MateId{2}});
  }
  {
    auto coincidentParallel = makeAngularProduct(
        {{AssemblyMateKind::PlanarCoincident,
          PlanarMateOrientation::Aligned},
         {AssemblyMateKind::PlanarParallel, PlanarMateOrientation::Aligned}},
        false);
    const auto result = solvePlanarMates(*coincidentParallel, kRoot);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.diagnostics.rank == 3);
    CHECK(result.diagnostics.redundantMateIds == std::vector{MateId{2}});
  }
  for (const auto conflicting :
       {std::vector<AngularMateSpec>{
            {AssemblyMateKind::PlanarParallel,
             PlanarMateOrientation::Aligned},
            {AssemblyMateKind::PlanarParallel,
             PlanarMateOrientation::Opposed}},
        std::vector<AngularMateSpec>{
            {AssemblyMateKind::PlanarParallel,
             PlanarMateOrientation::Aligned},
            {AssemblyMateKind::PlanarPerpendicular,
             PlanarMateOrientation::Aligned}}}) {
    auto product = makeAngularProduct(conflicting, false);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(result.diagnostics.conflictMateIds ==
          (std::vector{MateId{1}, MateId{2}}));
    CHECK(!result.candidate);
  }
  // Mixed planar rows retain the Angle degeneracy contract. Parallel first
  // makes the normals exactly collinear, so the unsatisfied movable Angle is
  // Broken; immovable perpendicular/coincident combinations are conflicts.
  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel,
          PlanarMateOrientation::Aligned, MateId{1}, 2},
         {AssemblyMateKind::PlanarAngle,
          PlanarMateOrientation::Aligned, MateId{2}, 2, 0.4}},
        false);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Broken);
    CHECK(result.diagnostics.relationFailure);
    CHECK(result.diagnostics.relationFailure->mateId == MateId{2});
    CHECK(result.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);
  }
  for (const auto companion : {AssemblyMateKind::PlanarPerpendicular,
                               AssemblyMateKind::PlanarCoincident}) {
    auto product = makeAngularProduct(
        {{companion, PlanarMateOrientation::Aligned, MateId{1}, 2},
         {AssemblyMateKind::PlanarAngle,
          PlanarMateOrientation::Aligned, MateId{2}, 2, 0.4}},
        false, false, true, true);
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(!result.candidate);
  }
  {
    auto forward = makeAngleDistanceProduct(false);
    auto reversed = makeAngleDistanceProduct(true);
    const auto first = solveAssemblyMates(*forward, kRoot);
    const auto second = solveAssemblyMates(*reversed, kRoot);
    CHECK(first.candidate && second.candidate);
    CHECK(first.diagnostics.rank == 2);
    CHECK(first.diagnostics.remainingDegreesOfFreedom == 4);
    CHECK(first.componentMateIds == (std::vector{MateId{3}, MateId{9}}));
    CHECK(second.componentMateIds == first.componentMateIds);
    CHECK(second.candidate->changes() == first.candidate->changes());
    const auto& placement = first.candidate->changes().back().afterPlacement;
    CHECK(placement.translationMm() ==
          (std::array<double, 3>{3.7460335640491271,
                                 -2.7602603184569352,
                                 1.8298075066436399}));
    CHECK(placement.quaternionWxyz() ==
          (std::array<double, 4>{0.98006657784124196,
                                 0.19866933079505955, 0.0, 0.0}));
  }

  // A real three-node Angle chain remains byte-identical under insertion and
  // MateId permutations. Both relations are solved in one connected graph.
  {
    auto forward = makeThreeNodeAngleChain(false, MateId{2}, MateId{9});
    auto reversed = makeThreeNodeAngleChain(true, MateId{91}, MateId{7});
    const auto first = solveAssemblyMates(*forward, kRoot);
    const auto second = solveAssemblyMates(*reversed, kRoot);
    CHECK(first.candidate && second.candidate);
    CHECK(first.diagnostics.rank == 2);
    CHECK(first.diagnostics.remainingDegreesOfFreedom == 10);
    CHECK(first.candidate->changes().size() == 3);
    CHECK(first.candidate->changes() == second.candidate->changes());
    CHECK(first.diagnostics.maximumAngularResidualRad <= 1.0e-9);
  }

  // Component localization keeps a valid Angle candidate independent of an
  // exact-collinear broken Angle in another seeded component.
  {
    auto product = makeDisconnectedAngleProduct();
    const auto good = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{1}, std::nullopt});
    CHECK(good.candidate);
    CHECK(good.componentMateIds == std::vector{MateId{1}});
    const auto bad = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{2}, std::nullopt});
    CHECK(bad.status == MateSolveStatus::Broken);
    CHECK(!bad.candidate);
    CHECK(bad.componentMateIds == std::vector{MateId{2}});
    CHECK(bad.diagnostics.relationFailure);
    CHECK(bad.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);
  }
  for (const auto coincidentOrientation :
       {PlanarMateOrientation::Aligned, PlanarMateOrientation::Opposed}) {
    const auto parallelOrientation =
        coincidentOrientation == PlanarMateOrientation::Aligned
            ? PlanarMateOrientation::Opposed
            : PlanarMateOrientation::Aligned;
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarCoincident, coincidentOrientation},
         {AssemblyMateKind::PlanarParallel, parallelOrientation}},
        false);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(result.diagnostics.conflictMateIds ==
          (std::vector{MateId{1}, MateId{2}}));
    CHECK(!result.candidate);
  }

  // The authored work ceiling is exact for the new mixed rows as well.
  for (const auto& [specs, initiallyParallel] :
       std::vector<std::pair<std::vector<AngularMateSpec>, bool>>{
           {{{AssemblyMateKind::PlanarPerpendicular,
              PlanarMateOrientation::Aligned}},
            true},
           {{{AssemblyMateKind::PlanarParallel,
              PlanarMateOrientation::Aligned}},
            false},
           {{{AssemblyMateKind::PlanarAngle,
              PlanarMateOrientation::Aligned, MateId{1}, 2, 0.4}},
            false},
           {{{AssemblyMateKind::PlanarParallel,
              PlanarMateOrientation::Aligned, MateId{1}, 0},
             {AssemblyMateKind::PlanarPerpendicular,
              PlanarMateOrientation::Aligned, MateId{2}, 2}},
            false}}) {
    auto baselineProduct =
        makeAngularProduct(specs, initiallyParallel);
    const auto baseline = solvePlanarMates(*baselineProduct, kRoot);
    CHECK(baseline.candidate);
    CHECK(baseline.diagnostics.requiredWorkUnits > 1);
    const auto required = baseline.diagnostics.requiredWorkUnits;
    for (const auto allowance : {required, required - 1}) {
      auto product = makeAngularProduct(specs, initiallyParallel);
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      auto options = registry.options();
      options.maximumWorkUnits = allowance;
      CHECK(registry.setOptions(options) ==
            MateRegistryMutationStatus::Inserted);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(allowance == required
                ? result.candidate.has_value()
                : result.status == MateSolveStatus::ResourceFailure);
    }
  }

  {
    constexpr std::array<std::size_t, 3> expectedRank{3, 5, 6};
    constexpr std::array<std::size_t, 3> expectedRemaining{3, 1, 0};
    for (std::size_t count = 1; count <= 3; ++count) {
      auto product = makeIndependentPlaneProduct(count);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.candidate);
      CHECK(result.diagnostics.rank == expectedRank[count - 1]);
      CHECK(result.diagnostics.remainingDegreesOfFreedom ==
            expectedRemaining[count - 1]);
      CHECK(result.status == (count == 3 ? MateSolveStatus::Solved
                                        : MateSolveStatus::Underconstrained));
    }
  }

  {
    auto product = makeProduct(true);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
    CHECK(result.diagnostics.rank == 3);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 3);
    CHECK(result.diagnostics.maximumLinearResidualMm <= 1.0e-7);
    CHECK(result.diagnostics.maximumAngularResidualRad <= 1.0e-9);
    CHECK(result.candidate->changes().size() == 2);

    const auto beforeFixed = product->rootAssemblyDefinition()
                                 ->find(kFixed)
                                 ->placement();
    const auto beforeMoving = product->rootAssemblyDefinition()
                                  ->find(kMoving)
                                  ->placement();
    const auto topologyBuildsBeforeApply = TopologyIndex::buildAttemptCount();
    Successor rejected(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, rejected) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);

    ReentrantSuccessor reentrant(*result.candidate);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, reentrant) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(reentrant.solveStatus == MateSolveStatus::Busy);
    CHECK(reentrant.applyStatus == MatePlacementApplyStatus::Busy);
    CHECK(reentrant.nestedSuccessorCalls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);

    Successor accepted(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, accepted) ==
          MatePlacementApplyStatus::Applied);
    CHECK(accepted.calls == 1);
    CHECK(accepted.lastImpact == ProductDocumentSuccessorImpact::MateProjection);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() !=
          beforeMoving);
    CHECK(TopologyIndex::buildAttemptCount() == topologyBuildsBeforeApply);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, accepted) ==
          MatePlacementApplyStatus::Stale);
  }

  {
    auto product = makeNestedProduct();
    const auto nestedBefore = product->definitions()
                                  .findAssembly(DefinitionId{3})
                                  ->find(OccurrenceId{5})
                                  ->placement();
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
    CHECK(result.candidate->changes().size() == 2);
    CHECK(result.candidate->changes()[0].before.occurrenceId == kFixed);
    CHECK(result.candidate->changes()[1].before.occurrenceId == kMoving);
    Successor accepted(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*result.candidate, accepted) ==
          MatePlacementApplyStatus::Applied);
    CHECK(product->definitions()
              .findAssembly(DefinitionId{3})
              ->find(OccurrenceId{5})
              ->placement() == nestedBefore);
  }

  // Candidate dependencies include every nested tail occurrence, not only
  // the direct owner nodes that receive placement changes.
  {
    auto product = makeNestedProduct();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(solved.candidate->occurrenceDependencies().size() == 3);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    auto* nested = const_cast<AssemblyDefinition*>(
        product->definitions().findAssembly(DefinitionId{3}));
    CHECK(nested);
    const auto changed = RigidTransform3d::create(
        {9.0, 8.0, 7.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(changed);
    CHECK(AssemblyMateDependencyTestAccess::setPlacement(
        *nested, OccurrenceId{5}, *changed.transform));
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // Direct topology mutation is caught even when DefinitionStateId did not
  // change: the authoritative feature revision is part of the dependency
  // seal and validation runs before the first placement mutation.
  {
    const auto offAxis = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(offAxis);
    auto product = makeConcentricProduct(
        AxisSource::Cylinder, AxisSource::Cylinder, *offAxis.transform);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(!solved.candidate->definitionDependencies().empty());
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    auto* part = const_cast<PartDefinition*>(
        product->definitions().findPart(kPart));
    CHECK(part);
    const auto* relation =
        product->rootAssemblyDefinition()->mateRegistry().findConcentric(
            MateId{1});
    CHECK(relation);
    const auto& face =
        std::get<CylindricalFaceAxis>(relation->first().reference).face;
    CHECK(face.v2());
    auto* feature = part->document().findFeature(
        face.v2()->bodyId, face.v2()->provenance.featureId);
    CHECK(feature);
    feature->discardResult();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  {
    auto product = makeProduct(false, -3.0);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
    CHECK(result.diagnostics.rank == 3);
    CHECK(result.diagnostics.remainingDegreesOfFreedom == 9);
  }

  {
    auto product = makeProduct(true);
    MateSolverOptions options;
    options.maximumNodes = 1;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
    const auto baseline = solvePlanarMates(*product, kRoot);
    CHECK(baseline.candidate);
    CHECK(baseline.diagnostics.requiredWorkUnits > 0);
    options = MateSolverOptions{};
    options.maximumWorkUnits = baseline.diagnostics.requiredWorkUnits - 1;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
    options.maximumWorkUnits = baseline.diagnostics.requiredWorkUnits;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
    options = MateSolverOptions{};
    options.maximumIterations = 1;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
    options.maximumIterations = 0;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
  }

  {
    auto product = makeProduct(true, 4.0, PlanarMateOrientation::Aligned,
                               false, true);
    MateSolverOptions options;
    options.maximumMates = 1;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
    options.maximumMates = 2;
    CHECK(solvePlanarMates(*product, kRoot, options).status ==
          MateSolveStatus::InvalidInput);
  }

  {
    auto product = makeProduct(true, 0.0, PlanarMateOrientation::Opposed);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
    CHECK(result.diagnostics.maximumAngularResidualRad <= 1.0e-9);
  }

  {
    auto product = makeProduct(true, 4.0, PlanarMateOrientation::Aligned,
                               false, true);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Redundant);
    CHECK(result.candidate);
    CHECK(result.diagnostics.redundantMateIds.size() == 1);
    CHECK(result.diagnostics.redundantMateIds.front() == MateId{2});
  }

  {
    auto product = makeProduct(true, 4.0, PlanarMateOrientation::Aligned,
                               false, true, false, true, false, 2.0);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(!result.candidate);
    CHECK(result.diagnostics.conflictMateIds ==
          std::vector({MateId{1}, MateId{2}}));
  }

  {
    auto first = makeProduct(true, 4.0, PlanarMateOrientation::Aligned, false,
                             true, false, true, false);
    auto second = makeProduct(true, 4.0, PlanarMateOrientation::Aligned,
                              false, true, false, true, true);
    const auto firstResult = solvePlanarMates(*first, kRoot);
    const auto secondResult = solvePlanarMates(*second, kRoot);
    CHECK(firstResult.status == secondResult.status);
    CHECK(firstResult.candidate && secondResult.candidate);
    CHECK(firstResult.diagnostics.rank == secondResult.diagnostics.rank);
    CHECK(firstResult.diagnostics.redundantMateIds ==
          secondResult.diagnostics.redundantMateIds);
    CHECK(firstResult.candidate->address().mateRegistryFingerprint ==
          secondResult.candidate->address().mateRegistryFingerprint);
    CHECK(firstResult.candidate->changes() == secondResult.candidate->changes());
  }

  {
    auto product = makeProduct(true, 2.0, PlanarMateOrientation::Aligned,
                               true);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Overconstrained);
    CHECK(!result.candidate);
    CHECK(result.diagnostics.conflictMateIds == std::vector{MateId{1}});
  }

  {
    auto product = makeProduct(true, 2.0, PlanarMateOrientation::Aligned,
                               false, false, true);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Broken);
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Suppressed);
  }

  {
    auto product = makeProduct(true, 2.0, PlanarMateOrientation::Aligned,
                               false, false, false, false);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Underconstrained);
    CHECK(result.candidate);
  }

  {
    auto product = makeProduct(true);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate && solved.candidate->changes().size() == 2);
    Successor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, resource) ==
          MatePlacementApplyStatus::ResourceFailure);
    CHECK(resource.calls == 1);
    for (const auto& change : solved.candidate->changes())
      CHECK(product->rootAssemblyDefinition()
                ->find(change.before.occurrenceId)
                ->placement() == change.before.placement);
  }

  {
    auto cylinder = makeCylinderPart();
    const auto face = cylinder.face;
    auto product = makeSingleFaceProduct(std::move(cylinder), face);
    for (const auto kind : {AssemblyMateKind::PlanarCoincident,
                            AssemblyMateKind::PlanarParallel,
                            AssemblyMateKind::PlanarPerpendicular}) {
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      const auto* current = registry.findMate(MateId{1});
      CHECK(current);
      const MateEndpoint first = *assemblyPlanarMateFirst(*current);
      const MateEndpoint second = *assemblyPlanarMateSecond(*current);
      CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
      const auto inserted =
          kind == AssemblyMateKind::PlanarCoincident
              ? registry.insert(PlanarCoincidentMate{
                    MateId{1}, first, second, PlanarMateOrientation::Aligned,
                    0.0})
              : (kind == AssemblyMateKind::PlanarParallel
                     ? registry.insert(PlanarParallelMate{
                           MateId{1}, first, second,
                           PlanarMateOrientation::Aligned})
                     : registry.insert(PlanarPerpendicularMate{
                           MateId{1}, first, second}));
      CHECK(inserted == MateRegistryMutationStatus::Inserted);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.status == MateSolveStatus::Broken);
      CHECK(result.diagnostics.endpointFailure);
      CHECK(result.diagnostics.endpointFailure->failure ==
            MateEndpointFailure::NonPlanar);
    }
  }

  {
    auto ambiguous = makeAmbiguousPart();
    const auto face = ambiguous.ambiguousFace;
    auto product = makeSingleFaceProduct(
        PartFixture{std::move(ambiguous.document), face}, face);
    const auto result = solvePlanarMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Ambiguous);
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Ambiguous);
  }

  {
    auto ambiguous = makeAmbiguousCylinderPart();
    const auto face = ambiguous.face;
    auto product = makeSingleAxisFaceProduct(std::move(ambiguous), face);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    const auto result = solveAssemblyMates(*product, kRoot);
    CHECK(result.status == MateSolveStatus::Ambiguous);
    CHECK(!result.candidate);
    CHECK(result.componentMateIds == std::vector{MateId{1}});
    CHECK(result.diagnostics.endpointFailure);
    CHECK(result.diagnostics.endpointFailure->mateId == MateId{1});
    CHECK(!result.diagnostics.endpointFailure->secondEndpoint);
    CHECK(result.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Ambiguous);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  {
    constexpr BodyId bodyId{1102};
    constexpr FeatureId featureId{1103};
    constexpr double offset = 2.0;
    for (const auto orientation : {PlanarMateOrientation::Aligned,
                                   PlanarMateOrientation::Opposed}) {
      auto part = makePart();
      const auto pair =
          orientedPlanarFaces(*part.document, bodyId, featureId);
      auto product = makeTwoFaceProduct(
          std::move(part), pair.forward, pair.reversed, orientation, offset);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.status == MateSolveStatus::Underconstrained);
      CHECK(result.candidate && result.candidate->changes().size() == 2);
      const auto& moving = result.candidate->changes()[1].afterPlacement;
      const auto correctedReversedNormal =
          std::array{-pair.reversedRawNormal[0],
                     -pair.reversedRawNormal[1],
                     -pair.reversedRawNormal[2]};
      const auto solvedSecondNormal =
          rotateVector(moving.quaternionWxyz(), correctedReversedNormal);
      const double normalDot = dot3(pair.forwardNormal, solvedSecondNormal);
      CHECK(orientation == PlanarMateOrientation::Aligned
                ? normalDot > 1.0 - 1.0e-10
                : normalDot < -1.0 + 1.0e-10);
      const auto solvedSecondPoint =
          transformPoint(moving, pair.reversedPoint);
      const std::array<double, 3> separation{
          solvedSecondPoint[0] - pair.forwardPoint[0],
          solvedSecondPoint[1] - pair.forwardPoint[1],
          solvedSecondPoint[2] - pair.forwardPoint[2]};
      CHECK(std::abs(dot3(separation, pair.forwardNormal) - offset) <=
            1.0e-7);

      const double initialCorrectedDot =
          dot3(pair.forwardNormal, correctedReversedNormal);
      const bool shouldRotate =
          orientation == PlanarMateOrientation::Aligned
              ? initialCorrectedDot < 0.0
              : initialCorrectedDot > 0.0;
      const double absoluteW = std::abs(moving.quaternionWxyz()[0]);
      CHECK(shouldRotate ? absoluteW < 1.0e-8
                         : absoluteW > 1.0 - 1.0e-10);
    }
  }

  // Exact collinearity is invariant to reversing the authored moving face.
  // Both cases apply the same rigid rotation and preserve the same selected
  // face pivot; the final unoriented plane normals are equivalent.
  {
    constexpr BodyId bodyId{1102};
    constexpr FeatureId featureId{1103};
    struct ReversalResult final {
      RigidTransform3d placement;
      FaceFrameSample selectedFace;
    };
    const auto solveReversal = [&](bool reversed) {
      auto part = makePart();
      const auto pair =
          orientedPlanarFaces(*part.document, bodyId, featureId);
      const auto initial =
          reversed
              ? RigidTransform3d::create(
                    {pair.forwardPoint[0] - pair.reversedPoint[0],
                     pair.forwardPoint[1] - pair.reversedPoint[1],
                     pair.forwardPoint[2] - pair.reversedPoint[2]},
                    {1.0, 0.0, 0.0, 0.0})
                    .transform
              : std::optional<RigidTransform3d>{RigidTransform3d::identity()};
      CHECK(initial);
      const FaceReference movingFace =
          reversed ? pair.reversed : pair.forward;
      const auto movingLocal = localFaceFrame(*part.document, movingFace);
      auto product = makeTwoFaceProduct(
          std::move(part), pair.forward, movingFace,
          PlanarMateOrientation::Aligned, 0.0, *initial);
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      const auto* coincident = registry.find(MateId{1});
      CHECK(coincident);
      const MateEndpoint first = coincident->first();
      const MateEndpoint second = coincident->second();
      CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
      CHECK(registry.insert(PlanarPerpendicularMate{MateId{1}, first, second}) ==
            MateRegistryMutationStatus::Inserted);
      const auto solved = solvePlanarMates(*product, kRoot);
      CHECK(solved.candidate);
      CHECK(solved.status == MateSolveStatus::Underconstrained);
      const auto changed = std::find_if(
          solved.candidate->changes().begin(),
          solved.candidate->changes().end(), [](const auto& value) {
            return value.before.occurrenceId == kMoving;
          });
      CHECK(changed != solved.candidate->changes().end());
      return ReversalResult{
          changed->afterPlacement,
          transformFaceFrame(movingLocal, changed->afterPlacement)};
    };
    const auto forward = solveReversal(false);
    const auto reversed = solveReversal(true);
    for (std::size_t index = 0; index < 4; ++index)
      CHECK(std::abs(forward.placement.quaternionWxyz()[index] -
                     reversed.placement.quaternionWxyz()[index]) <= 1.0e-10);
    CHECK(pointDistance(forward.selectedFace.point,
                        reversed.selectedFace.point) <= 1.0e-9);
    CHECK(std::abs(std::abs(dot3(forward.selectedFace.normal,
                                 reversed.selectedFace.normal)) -
                   1.0) <= 1.0e-10);
  }

  {
    auto consistent = makeCycleProduct(false);
    const auto consistentResult = solvePlanarMates(*consistent, kRoot);
    CHECK(consistentResult.status == MateSolveStatus::Redundant);
    CHECK(consistentResult.candidate);
    CHECK(!consistentResult.diagnostics.redundantMateIds.empty());
    auto inconsistent = makeCycleProduct(true);
    const auto inconsistentResult = solvePlanarMates(*inconsistent, kRoot);
    CHECK(inconsistentResult.status == MateSolveStatus::Overconstrained);
    CHECK(!inconsistentResult.candidate);
    CHECK(inconsistentResult.diagnostics.conflictMateIds ==
          std::vector({MateId{1}, MateId{2}, MateId{3}}));
  }

  {
    auto basePart = makePart();
    const auto baseFace = basePart.face;
    auto base = makeSingleFaceProduct(std::move(basePart), baseFace);
    const auto baseResult = solvePlanarMates(*base, kRoot);
    CHECK(baseResult.candidate);
    const auto baselineRank = baseResult.diagnostics.rank;

    for (const double geometryScale : {1.0e-3, 1.0e6}) {
      auto part = makePart(geometryScale);
      const auto face = part.face;
      auto product = makeSingleFaceProduct(std::move(part), face);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.candidate);
      CHECK(result.diagnostics.rank == baselineRank);
    }

    for (const double translation : {1.0e-12, 1.0e12}) {
      auto part = makePart();
      const auto face = part.face;
      const auto transform =
          RigidTransform3d::create({translation, -translation, translation},
                                   {1.0, 0.0, 0.0, 0.0});
      CHECK(transform);
      auto product = makeSingleFaceProduct(std::move(part), face,
                                           *transform.transform,
                                           *transform.transform);
      const auto result = solvePlanarMates(*product, kRoot);
      CHECK(result.candidate);
      CHECK(result.diagnostics.rank == baselineRank);
    }

    const double halfAngle = 0.5e-10;
    const auto nearParallel = RigidTransform3d::create(
        {0.0, 0.0, 0.0},
        {std::cos(halfAngle), std::sin(halfAngle), 0.0, 0.0});
    CHECK(nearParallel);
    auto nearPart = makePart();
    const auto nearFace = nearPart.face;
    auto near = makeSingleFaceProduct(std::move(nearPart), nearFace,
                                      *nearParallel.transform);
    const auto nearResult = solvePlanarMates(*near, kRoot);
    CHECK(nearResult.candidate);
    CHECK(nearResult.diagnostics.rank == baselineRank);

    const double leverHalfAngle = 0.5e-6;
    const auto leverRotation = RigidTransform3d::create(
        {1.0e12, -1.0e12, 1.0e12},
        {std::cos(leverHalfAngle), std::sin(leverHalfAngle), 0.0, 0.0});
    const auto leverFixed = RigidTransform3d::create(
        {1.0e12, -1.0e12, 1.0e12}, {1.0, 0.0, 0.0, 0.0});
    CHECK(leverRotation && leverFixed);
    auto leverPart = makeFarLeverPart(2.0e9);
    const auto leverFace = leverPart.face;
    auto lever = makeSingleFaceProduct(
        std::move(leverPart), leverFace, *leverRotation.transform,
        *leverFixed.transform, PlanarMateOrientation::Aligned, 7.0);
    MateSolverOptions leverOptions;
    leverOptions.maximumTranslationStepMm = 1.0e12;
    const auto leverResult = solvePlanarMates(*lever, kRoot, leverOptions);
    CHECK(leverResult.status == MateSolveStatus::InvalidInput);
    CHECK(!leverResult.candidate);

    for (const double z : {std::nextafter(0.0, 1.0),
                           std::nextafter(0.0, -1.0)}) {
      const auto ulpPlacement =
          RigidTransform3d::create({0.0, 0.0, z}, {1.0, 0.0, 0.0, 0.0});
      CHECK(ulpPlacement);
      auto ulpPart = makePart();
      const auto ulpFace = ulpPart.face;
      auto ulp = makeSingleFaceProduct(std::move(ulpPart), ulpFace,
                                       *ulpPlacement.transform);
      const auto ulpResult = solvePlanarMates(*ulp, kRoot);
      CHECK(ulpResult.candidate);
      CHECK(ulpResult.diagnostics.rank == baselineRank);
    }
  }

  {
    auto product = makeProduct(true);
    const auto baseline = solvePlanarMates(*product, kRoot);
    CHECK(baseline.candidate);
    MateSolverOptions exact;
    exact.maximumWorkUnits = baseline.diagnostics.requiredWorkUnits;
    const auto exactResult = solvePlanarMates(*product, kRoot, exact);
    CHECK(exactResult.status == MateSolveStatus::InvalidInput);
    exact.maximumWorkUnits = baseline.diagnostics.requiredWorkUnits - 1;
    CHECK(solvePlanarMates(*product, kRoot, exact).status ==
          MateSolveStatus::InvalidInput);
    exact.maximumWorkUnits = std::numeric_limits<std::uint64_t>::max();
    const auto maximumResult = solvePlanarMates(*product, kRoot, exact);
    CHECK(maximumResult.status == MateSolveStatus::InvalidInput);
  }

  {
    auto product = makeMaximumWorkProduct();
    MateSolverOptions options;
    options.maximumMates = kMaximumAssemblyMates;
    options.maximumIterations = 1;
    options.maximumWorkUnits = std::numeric_limits<std::uint64_t>::max();
    const auto baseline = solvePlanarMates(*product, kRoot, options);
    CHECK(baseline.status == MateSolveStatus::InvalidInput);
    CHECK(!baseline.candidate);
  }

  {
    auto part = makePart();
    MateRegistry registry;
    const auto add = [&](MateId id, double offset) {
      return registry.insert(PlanarCoincidentMate(
          id, MateEndpoint{path(kFixed), part.face},
          MateEndpoint{path(kMoving), part.face},
          PlanarMateOrientation::Aligned, offset));
    };
    CHECK(add(MateId{1}, std::numeric_limits<double>::quiet_NaN()) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(add(MateId{2}, -std::numeric_limits<double>::infinity()) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(add(MateId{3}, 1.0e9 + 1.0) ==
          MateRegistryMutationStatus::Invalid);
    CHECK(add(MateId{4}, -0.0) == MateRegistryMutationStatus::Inserted);
    CHECK(!std::signbit(registry.find(MateId{4})->offsetMm()));

    auto product = makeProduct(true);
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity()}) {
      MateSolverOptions options;
      options.rankTolerance = invalid;
      CHECK(solvePlanarMates(*product, kRoot, options).status ==
            MateSolveStatus::InvalidInput);
    }

    const double maximum = std::numeric_limits<double>::max();
    const auto extreme = RigidTransform3d::create(
        {maximum, maximum, maximum}, {1.0, 0.0, 0.0, 0.0});
    CHECK(extreme);
    auto extremePart = makePart();
    const auto extremeFace = extremePart.face;
    auto extremeProduct = makeSingleFaceProduct(
        std::move(extremePart), extremeFace, *extreme.transform,
        *extreme.transform);
    const auto nonFiniteRank = solvePlanarMates(*extremeProduct, kRoot);
    CHECK(nonFiniteRank.status == MateSolveStatus::NonConvergent);
    CHECK(!nonFiniteRank.candidate);
  }

  {
    std::optional<std::vector<MatePlacementChange>> expectedChanges;
    std::array ids{MateId{1}, MateId{2}, MateId{3}};
    do {
      std::array<std::size_t, 3> insertionOrder{0, 1, 2};
      do {
        for (unsigned endpointMask = 0; endpointMask < 8; ++endpointMask) {
          const std::array swapEndpoints{
              (endpointMask & 1U) != 0, (endpointMask & 2U) != 0,
              (endpointMask & 4U) != 0};
          auto product =
              makePermutedPlaneProduct(ids, swapEndpoints, insertionOrder);
          const auto result = solvePlanarMates(
              *product, kRoot,
              MateSolveRequest{std::nullopt, kFixed});
          CHECK(result.status == MateSolveStatus::Solved);
          CHECK(result.candidate);
          CHECK(result.diagnostics.rank == 6);
          CHECK(result.diagnostics.remainingDegreesOfFreedom == 0);
          if (!expectedChanges)
            expectedChanges = result.candidate->changes();
          else
            CHECK(result.candidate->changes() == *expectedChanges);
        }
      } while (std::next_permutation(insertionOrder.begin(),
                                     insertionOrder.end()));
    } while (std::next_permutation(ids.begin(), ids.end()));
  }

  // Every V2 identity field participates in the registry seal. This guards
  // both top-level provenance and complete immediate-origin occurrence data.
  {
    auto part = makePart();
    const auto fingerprintFor = [&](FaceReference reference) {
      MateRegistry registry;
      CHECK(registry.insert(PlanarCoincidentMate(
                MateId{1}, MateEndpoint{path(kFixed), reference},
                MateEndpoint{path(kMoving), part.face},
                PlanarMateOrientation::Aligned, 0.0)) ==
            MateRegistryMutationStatus::Inserted);
      return registry.fingerprint();
    };
    const auto baseline = fingerprintFor(part.face);
    const auto verifyDifferent = [&](auto mutate) {
      auto value = *part.face.v2();
      mutate(value);
      auto changedReference = FaceReference::fromV2(std::move(value));
      CHECK(fingerprintFor(changedReference) != baseline);
    };
    verifyDifferent([](auto& value) { value.bodyId = BodyId{9991}; });
    verifyDifferent(
        [](auto& value) { value.provenance.featureId = FeatureId{9992}; });
    verifyDifferent([](auto& value) {
      value.provenance.kind = TopologyKind::Edge;
    });
    verifyDifferent([](auto& value) {
      value.provenance.operation = TopologyProvenanceOperation::Modified;
    });
    verifyDifferent([](auto& value) {
      value.provenance.role = TopologySemanticRole::Custom;
    });
    verifyDifferent(
        [](auto& value) { value.provenance.localName += "-changed"; });
    verifyDifferent([](auto& value) {
      value.provenance.occurrence =
          TopologyOccurrenceKey{FeatureId{9993}, 4};
    });
    verifyDifferent([](auto& value) {
      value.provenance.origins.emplace_back(
          FeatureId{9994}, TopologyKind::Face,
          TopologyProvenanceOperation::Generated,
          TopologySemanticRole::Side, "origin",
          TopologyOccurrenceKey{FeatureId{9995}, 7});
    });
  }

  // Missing/Broken/InvalidInput precedence is semantic rather than MateId or
  // insertion order. Ambiguous is covered with the authoritative
  // duplicate-key topology fixture above.
  {
    for (const auto ids : {std::array{MateId{1}, MateId{2}, MateId{3}, MateId{4}},
                           std::array{MateId{4}, MateId{1}, MateId{3}, MateId{2}}}) {
      auto part = makeAmbiguousPart();
      auto missingV2 = *part.validFace.v2();
      missingV2.provenance.localName += "-missing";
      auto invalidV2 = *part.validFace.v2();
      invalidV2.provenance.kind = TopologyKind::Edge;
      AssemblyDefinition root(kRoot);
      CHECK(root.insert(ComponentOccurrence(
                kFixed, kPart, RigidTransform3d::identity(),
                OccurrenceFlags{true, false, true})) ==
            AssemblyOccurrenceInsertStatus::Inserted);
      CHECK(root.insert(ComponentOccurrence(
                kMoving, kPart, RigidTransform3d::identity())) ==
            AssemblyOccurrenceInsertStatus::Inserted);
      const auto insert = [&](MateId id, MateEndpoint second) {
        CHECK(root.mateRegistry().insert(PlanarCoincidentMate(
                  id, MateEndpoint{path(kFixed), part.validFace},
                  std::move(second),
                  PlanarMateOrientation::Aligned, 0.0)) ==
              MateRegistryMutationStatus::Inserted);
      };
      insert(ids[0], MateEndpoint{path(kMoving), part.ambiguousFace});
      insert(ids[1], MateEndpoint{
                         path(kMoving),
                         FaceReference::fromV2(std::move(missingV2))});
      insert(ids[2],
             MateEndpoint{path(OccurrenceId{999}), part.validFace});
      insert(ids[3], MateEndpoint{
                         path(kMoving),
                         FaceReference::fromV2(std::move(invalidV2))});
      DefinitionRegistry definitions;
      CHECK(definitions.insert(
                PartDefinition(kPart, std::move(part.document))) ==
            DefinitionInsertStatus::Inserted);
      CHECK(definitions.insert(std::move(root)) ==
            DefinitionInsertStatus::Inserted);
      auto made = ProductDocument::create(std::move(definitions), kRoot);
      CHECK(made);
      const auto result = solvePlanarMates(
          *made.product, kRoot, MateSolveRequest{std::nullopt, kFixed});
      CHECK(result.status == MateSolveStatus::InvalidInput);
      CHECK(result.diagnostics.endpointFailure);
      CHECK(result.diagnostics.endpointFailure->mateId == ids[3]);
      CHECK(result.diagnostics.endpointFailure->failure ==
            MateEndpointFailure::InvalidOwner);
    }
  }

  // Incarnation, registry, placement and flags are independently revalidated
  // before the first occurrence mutation. Candidate address/seed/component
  // fields have no consumer-visible mutation or reseal API.
  {
    auto product = makeProduct(true);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate);
    auto foreign = makeProduct(true);
    Successor foreignSuccessor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(foreign->applyMatePlacementCandidate(*solved.candidate,
                                               foreignSuccessor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(foreignSuccessor.calls == 0);
  }

  {
    auto product = makeProduct(true);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto moved = RigidTransform3d::create(
        {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(moved);
    CHECK(product->setRootOccurrencePlacement(path(kMoving),
                                               *moved.transform) ==
          ProductOccurrenceMutationStatus::Committed);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
  }

  {
    auto product = makeProduct(true);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate);
    CHECK(product->setRootOccurrenceFlags(
              path(kMoving), OccurrenceFlags{false, false, false}) ==
          ProductOccurrenceMutationStatus::Committed);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
  }

  {
    auto product = makeProduct(true);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto* relation =
        product->rootAssemblyDefinition()->mateRegistry().find(MateId{1});
    CHECK(relation);
    PlanarCoincidentMate replacement = *relation;
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(std::move(replacement)) ==
          MateRegistryMutationStatus::Inserted);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
  }

  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarParallel,
          PlanarMateOrientation::Aligned}},
        false);
    const auto solved = solvePlanarMates(*product, kRoot);
    CHECK(solved.candidate);
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    const auto* authored = registry.findParallel(MateId{1});
    CHECK(authored);
    const MateEndpoint first = authored->first();
    const MateEndpoint second = authored->second();
    const auto beforeFingerprint = registry.fingerprint();
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(PlanarParallelMate{
              MateId{1}, first, second, PlanarMateOrientation::Opposed}) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.fingerprint() != beforeFingerprint);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);

    const auto opposed = solvePlanarMates(*product, kRoot);
    CHECK(opposed.candidate);
    Successor rejected(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyMatePlacementCandidate(*opposed.candidate, rejected) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(rejected.calls == 1);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
    Successor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyMatePlacementCandidate(*opposed.candidate, resource) ==
          MatePlacementApplyStatus::ResourceFailure);
    CHECK(resource.calls == 1);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // A7c3: an unsigned point distance is a symmetric rank-one translation
  // constraint.  It never rotates either occurrence and applying its sealed
  // candidate exactly once reaches a stable solve/apply/solve fixed point.
  {
    auto product = makePointDistanceProduct(2.0, 5.0);
    const auto beforeRotation = product->rootAssemblyDefinition()
                                    ->find(kMoving)
                                    ->placement()
                                    .quaternionWxyz();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.status == MateSolveStatus::Underconstrained);
    CHECK(solved.diagnostics.rank == 1);
    CHECK(solved.diagnostics.remainingDegreesOfFreedom == 5);
    CHECK(solved.candidate);
    CHECK(solved.candidate->changes().size() == 2);
    CHECK(std::abs(solved.candidate->changes().back()
                       .afterPlacement.translationMm()[0] -
                   5.0) <= 1.0e-9);
    CHECK(solved.candidate->changes().back()
              .afterPlacement.quaternionWxyz() == beforeRotation);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Applied);
    CHECK(successor.calls == 1);
    const auto fixedPoint = solveAssemblyMates(*product, kRoot);
    CHECK(fixedPoint.status == MateSolveStatus::Underconstrained);
    CHECK(fixedPoint.candidate);
    CHECK(std::all_of(fixedPoint.candidate->changes().begin(),
                      fixedPoint.candidate->changes().end(),
                      [](const auto& change) {
                        return change.afterPlacement == change.before.placement;
                      }));
    CHECK(fixedPoint.diagnostics.maximumLinearResidualMm <= 1.0e-9);
  }

  // Endpoint order and MateId do not choose the moving side or direction.
  {
    auto forward = makePointDistanceProduct(2.0, 5.0, false, true, false,
                                            false, 0.0, MateId{9});
    auto reversed = makePointDistanceProduct(2.0, 5.0, true, true, false,
                                             false, 0.0, MateId{3});
    const auto first = solveAssemblyMates(*forward, kRoot);
    const auto second = solveAssemblyMates(*reversed, kRoot);
    CHECK(first.candidate && second.candidate);
    CHECK(first.candidate->changes() == second.candidate->changes());
    CHECK(first.diagnostics.rank == second.diagnostics.rank);
  }

  // Oblique 3D projection is exactly radial, never rotates, and selects the
  // moving occurrence solely from grounded state then stable occurrence ID.
  {
    const auto expectTranslation = [](const MateSolveResult& result,
                                      std::size_t change,
                                      const std::array<double, 3>& expected) {
      CHECK(result.candidate);
      const auto& actual = result.candidate->changes()[change]
                               .afterPlacement.translationMm();
      for (std::size_t index = 0; index < 3; ++index)
        CHECK(std::abs(actual[index] - expected[index]) <= 1.0e-12);
      CHECK(result.candidate->changes()[change]
                .afterPlacement.quaternionWxyz() ==
            (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));
    };
    for (const bool swap : {false, true}) {
      auto product = makePointDistanceProduct3d(
          {0.0, 0.0, 0.0}, {2.0, 3.0, 6.0}, 14.0, swap);
      const auto solved = solveAssemblyMates(*product, kRoot);
      expectTranslation(solved, 1, {4.0, 6.0, 12.0});
    }
    auto inverse = makePointDistanceProduct3d(
        {0.0, 0.0, 0.0}, {2.0, 3.0, 6.0}, 14.0, false, false, true);
    const auto inverseSolved = solveAssemblyMates(*inverse, kRoot);
    expectTranslation(inverseSolved, 0, {-2.0, -3.0, -6.0});
    expectTranslation(inverseSolved, 1, {2.0, 3.0, 6.0});

    auto bothFree = makePointDistanceProduct3d(
        {0.0, 0.0, 0.0}, {2.0, 3.0, 6.0}, 14.0, false, false, false);
    const auto freeSolved = solveAssemblyMates(*bothFree, kRoot);
    expectTranslation(freeSolved, 0, {0.0, 0.0, 0.0});
    expectTranslation(freeSolved, 1, {4.0, 6.0, 12.0});
  }

  // Authored tolerance is not a direction-existence threshold: a small but
  // representable separation remains solvable.  True coincidence is a typed
  // retained relation failure and never invents a world/local fallback axis.
  {
    auto resolvable = makePointDistanceProduct(1.0e-8, 2.0);
    const auto solved = solveAssemblyMates(*resolvable, kRoot);
    CHECK(solved.status == MateSolveStatus::Underconstrained);
    CHECK(solved.candidate);
    CHECK(std::abs(solved.candidate->changes().back()
                       .afterPlacement.translationMm()[0] -
                   2.0) <= 1.0e-9);

    auto coincident = makePointDistanceProduct(0.0, 2.0);
    const auto broken = solveAssemblyMates(*coincident, kRoot);
    CHECK(broken.status == MateSolveStatus::Broken);
    CHECK(!broken.candidate);
    CHECK(broken.diagnostics.relationFailure);
    CHECK(broken.diagnostics.relationFailure->mateId == MateId{1});
    CHECK(broken.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);

    // Direction existence depends only on the finite delta, never on the
    // absolute world coordinate scale.
    auto translated = makePointDistanceProduct3d(
        {1.0e9, 1.0e9, 1.0e9}, {1.0e9 + 1.0e-6, 1.0e9, 1.0e9},
        2.0e-6);
    const auto translatedSolved = solveAssemblyMates(*translated, kRoot);
    CHECK(translatedSolved.status == MateSolveStatus::Underconstrained);
    CHECK(translatedSolved.candidate);
    CHECK(!translatedSolved.diagnostics.relationFailure);

    // Projection itself can round a positive tiny target to exact
    // coincidence.  The post-sweep audit returns a typed retained failure,
    // never a candidate.
    auto rounded = makePointDistanceProduct3d(
        {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, 1.0e-16);
    const auto roundedBroken = solveAssemblyMates(*rounded, kRoot);
    CHECK(roundedBroken.status == MateSolveStatus::Broken);
    CHECK(!roundedBroken.candidate);
    CHECK(roundedBroken.diagnostics.relationFailure);
    CHECK(roundedBroken.diagnostics.relationFailure->mateId == MateId{1});
    CHECK(roundedBroken.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);

    // A later relation in the same sweep can collapse the anchors of an
    // earlier one; the component-wide post-sweep validation catches it.
    auto lateCollapse = makeLateCollapsePointDistanceProduct();
    const auto lateBroken = solveAssemblyMates(*lateCollapse, kRoot);
    CHECK(lateBroken.status == MateSolveStatus::Broken);
    CHECK(!lateBroken.candidate);
    CHECK(lateBroken.diagnostics.relationFailure);
    CHECK(lateBroken.diagnostics.relationFailure->mateId == MateId{1});
    CHECK(lateBroken.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);
  }

  {
    auto movingGrounded = makePointDistanceProduct(
        2.0, 5.0, true, false, true);
    const auto solved = solveAssemblyMates(*movingGrounded, kRoot);
    CHECK(solved.status == MateSolveStatus::Underconstrained);
    CHECK(solved.candidate);
    CHECK(std::abs(solved.candidate->changes().front()
                       .afterPlacement.translationMm()[0] +
                   3.0) <= 1.0e-9);
    CHECK(solved.candidate->changes().back().afterPlacement ==
          solved.candidate->changes().back().before.placement);

    auto bothGrounded = makePointDistanceProduct(
        2.0, 5.0, false, true, true);
    const auto conflict = solveAssemblyMates(*bothGrounded, kRoot);
    CHECK(conflict.status == MateSolveStatus::Overconstrained);
    CHECK(!conflict.candidate);
    CHECK(conflict.diagnostics.conflictMateIds ==
          std::vector<MateId>{MateId{1}});
  }

  // Convergence accepts residual exactly at the authored tolerance.  One ULP
  // less translation step with the same single-iteration ceiling cannot
  // silently pass.
  {
    const auto solveWithTolerance = [](double tolerance) {
      auto product = makePointDistanceProduct(2.0, 5.0);
      auto& registry = const_cast<MateRegistry&>(
          product->rootAssemblyDefinition()->mateRegistry());
      auto options = registry.options();
      options.linearToleranceMm = tolerance;
      options.maximumTranslationStepMm = 2.0;
      options.maximumIterations = 1;
      CHECK(registry.setOptions(options) ==
            MateRegistryMutationStatus::Inserted);
      return solveAssemblyMates(*product, kRoot);
    };
    const auto exact = solveWithTolerance(1.0);
    CHECK(exact.status == MateSolveStatus::Underconstrained);
    CHECK(exact.candidate);
    CHECK(exact.diagnostics.maximumLinearResidualMm == 1.0);
    const auto below = solveWithTolerance(
        std::nextafter(1.0, 0.0));
    CHECK(below.status == MateSolveStatus::NonConvergent);
    CHECK(!below.candidate);
    CHECK(below.diagnostics.maximumLinearResidualMm >
          std::nextafter(1.0, 0.0));
  }

  // Distance candidate publication has the same exact successor rollback
  // contract as the older mate families.
  {
    auto product = makePointDistanceProduct(2.0, 5.0);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    for (const auto status : {ProductDocumentSuccessorStatus::Rejected,
                              ProductDocumentSuccessorStatus::ResourceFailure}) {
      Successor successor(status);
      const auto applied = product->applyMatePlacementCandidate(
          *solved.candidate, successor);
      CHECK(applied ==
            (status == ProductDocumentSuccessorStatus::Rejected
                 ? MatePlacementApplyStatus::SuccessorRejected
                 : MatePlacementApplyStatus::ResourceFailure));
      CHECK(successor.calls == 1);
      CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
            beforeFixed);
      CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
            beforeMoving);
    }
  }

  {
    auto duplicate =
        makePointDistanceProduct(2.0, 5.0, false, true, false, true);
    const auto redundant = solveAssemblyMates(*duplicate, kRoot);
    CHECK(redundant.status == MateSolveStatus::Redundant);
    CHECK(redundant.diagnostics.rank == 1);
    CHECK(redundant.diagnostics.redundantMateIds ==
          std::vector<MateId>{MateId{2}});

    auto conflict = makePointDistanceProduct(2.0, 5.0, false, true, false,
                                             true, 6.0);
    const auto overconstrained = solveAssemblyMates(*conflict, kRoot);
    CHECK(overconstrained.status == MateSolveStatus::Overconstrained);
    CHECK(overconstrained.diagnostics.conflictMateIds ==
          (std::vector<MateId>{MateId{1}, MateId{2}}));
  }

  // Concentric removes four DOF and point distance removes the remaining
  // axial translation without adding rotation.
  {
    const auto moving = RigidTransform3d::create(
        {3.0, 4.0, 5.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(moving);
    auto product = makeConcentricProduct(AxisSource::DatumZ,
                                         AxisSource::DatumZ,
                                         *moving.transform);
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    CHECK(registry.insert(PointDistanceMate{
              MateId{2},
              MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
              MatePointEndpoint{path(kMoving), LocalDatumPoint::Origin}, 5.0}) ==
          MateRegistryMutationStatus::Inserted);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.status == MateSolveStatus::Underconstrained);
    CHECK(solved.diagnostics.rank == 5);
    CHECK(solved.diagnostics.remainingDegreesOfFreedom == 1);
    CHECK(solved.candidate);
    CHECK(std::abs(solved.candidate->changes().back()
                       .afterPlacement.translationMm()[2] -
                   5.0) <= 1.0e-9);
  }

  // Model validation is authoritative for positive finite bounded distances.
  {
    MateRegistry registry;
    const MatePointEndpoint first{path(kFixed), LocalDatumPoint::Origin};
    const MatePointEndpoint second{path(kMoving), LocalDatumPoint::Origin};
    for (const double invalid : {0.0, -0.0, -1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 1.0e9 + 1.0})
      CHECK(registry.insert(
                PointDistanceMate{MateId{1}, first, second, invalid}) ==
            MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PointDistanceMate{
              MateId{1},
              MatePointEndpoint{path(kFixed),
                                static_cast<LocalDatumPoint>(99)},
              second, 1.0}) == MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PointDistanceMate{
              MateId{1}, first,
              MatePointEndpoint{path(kMoving),
                                CircularEdgeCenter{EdgeReference{}}},
              1.0}) == MateRegistryMutationStatus::Invalid);
    CHECK(registry.insert(PointDistanceMate{
              MateId{1}, first,
              MatePointEndpoint{
                  path(kMoving),
                  CircularEdgeCenter{
                      EdgeReference{BodyId{1}, FeatureId{2}, 0}}},
              1.0}) == MateRegistryMutationStatus::Invalid);
  }

  // A three-node axis+distance component keeps exact IDs and placements under
  // insertion permutation; the shared B node makes it one localized solve.
  {
    auto forward = makeThreeNodeAxisDistanceProduct(false);
    auto reversed = makeThreeNodeAxisDistanceProduct(true);
    const auto first = solveAssemblyMates(*forward, kRoot,
                                          MateSolveRequest{MateId{4},
                                                           std::nullopt});
    const auto second = solveAssemblyMates(*reversed, kRoot,
                                           MateSolveRequest{MateId{11},
                                                            std::nullopt});
    CHECK(first.status == MateSolveStatus::Underconstrained);
    CHECK(second.status == MateSolveStatus::Underconstrained);
    CHECK(first.componentMateIds ==
          (std::vector<MateId>{MateId{4}, MateId{11}}));
    CHECK(first.componentMateIds == second.componentMateIds);
    CHECK(first.diagnostics.rank == 5);
    CHECK(first.diagnostics.remainingDegreesOfFreedom == 7);
    CHECK(first.candidate && second.candidate);
    CHECK(first.candidate->changes() == second.candidate->changes());
    const auto& changes = first.candidate->changes();
    CHECK(changes.size() == 3);
    CHECK(changes[1].before.occurrenceId == OccurrenceId{20});
    CHECK(changes[2].before.occurrenceId == OccurrenceId{30});
    CHECK(changes[1].afterPlacement.translationMm() ==
          (std::array<double, 3>{0.0, 0.0, 5.0}));
    const auto b = changes[1].afterPlacement.translationMm();
    const auto c = changes[2].afterPlacement.translationMm();
    CHECK(std::abs(std::hypot(c[0] - b[0], c[1] - b[1], c[2] - b[2]) -
                   6.0) <= 1.0e-7);
  }

  // A broken point component is retained locally and cannot poison an
  // independently seeded good component.
  {
    auto product = makeDisconnectedPointDistanceProduct();
    const auto before30 = product->rootAssemblyDefinition()
                              ->find(OccurrenceId{30})
                              ->placement();
    const auto before40 = product->rootAssemblyDefinition()
                              ->find(OccurrenceId{40})
                              ->placement();
    const auto good = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{1}, std::nullopt});
    CHECK(good.candidate);
    CHECK(good.componentMateIds == std::vector<MateId>{MateId{1}});
    CHECK(good.candidate->changes().size() == 2);
    CHECK(product->rootAssemblyDefinition()
              ->find(OccurrenceId{30})
              ->placement() == before30);
    CHECK(product->rootAssemblyDefinition()
              ->find(OccurrenceId{40})
              ->placement() == before40);
    const auto bad = solveAssemblyMates(
        *product, kRoot, MateSolveRequest{MateId{2}, std::nullopt});
    CHECK(bad.status == MateSolveStatus::Broken);
    CHECK(!bad.candidate);
    CHECK(bad.componentMateIds == std::vector<MateId>{MateId{2}});
    CHECK(bad.diagnostics.relationFailure);
    CHECK(bad.diagnostics.relationFailure->failure ==
          MateEndpointFailure::DegenerateDirection);

    auto ambiguousProduct =
        makeDisconnectedAmbiguousPointDistanceProduct();
    const auto localizedGood = solveAssemblyMates(
        *ambiguousProduct, kRoot,
        MateSolveRequest{MateId{1}, std::nullopt});
    CHECK(localizedGood.candidate);
    CHECK(localizedGood.componentMateIds ==
          std::vector<MateId>{MateId{1}});
    const auto localizedAmbiguous = solveAssemblyMates(
        *ambiguousProduct, kRoot,
        MateSolveRequest{MateId{2}, std::nullopt});
    CHECK(localizedAmbiguous.status == MateSolveStatus::Ambiguous);
    CHECK(!localizedAmbiguous.candidate);
    CHECK(localizedAmbiguous.componentMateIds ==
          std::vector<MateId>{MateId{2}});
    CHECK(localizedAmbiguous.diagnostics.endpointFailure);
    CHECK(localizedAmbiguous.diagnostics.endpointFailure->mateId ==
          MateId{2});
    CHECK(localizedAmbiguous.diagnostics.endpointFailure->secondEndpoint);
    CHECK(localizedAmbiguous.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::Ambiguous);
  }

  // Distance candidates seal every nested tail occurrence and reject a tail
  // placement mutation before successor publication or any owner placement.
  {
    auto product = makeNestedPointDistanceProduct();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    auto* nested = const_cast<AssemblyDefinition*>(
        product->definitions().findAssembly(DefinitionId{3}));
    CHECK(nested);
    const auto changed = RigidTransform3d::create(
        {9.0, 8.0, 7.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(changed);
    CHECK(AssemblyMateDependencyTestAccess::setPlacement(
        *nested, OccurrenceId{5}, *changed.transform));
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // Work accounting includes the authoritative linear V2 edge lookup.  Both
  // datum-only and circular-reference paths accept exact N and reject N-1.
  {
    const auto exactBudget = [](const auto& factory) -> std::uint64_t {
      auto baselineProduct = factory();
      const auto baseline = solveAssemblyMates(*baselineProduct, kRoot);
      CHECK(baseline.candidate);
      const auto required = baseline.diagnostics.requiredWorkUnits;
      CHECK(required > 1);

      auto exactProduct = factory();
      auto exactOptions =
          exactProduct->rootAssemblyDefinition()->mateRegistry().options();
      exactOptions.maximumWorkUnits = required;
      CHECK(const_cast<MateRegistry&>(exactProduct->rootAssemblyDefinition()
                                         ->mateRegistry())
                .setOptions(exactOptions) ==
            MateRegistryMutationStatus::Inserted);
      const auto exact = solveAssemblyMates(*exactProduct, kRoot);
      CHECK(exact.status != MateSolveStatus::ResourceFailure);
      CHECK(exact.diagnostics.requiredWorkUnits == required);

      auto shortProduct = factory();
      auto shortOptions =
          shortProduct->rootAssemblyDefinition()->mateRegistry().options();
      shortOptions.maximumWorkUnits = required - 1;
      CHECK(const_cast<MateRegistry&>(shortProduct->rootAssemblyDefinition()
                                         ->mateRegistry())
                .setOptions(shortOptions) ==
            MateRegistryMutationStatus::Inserted);
      const auto shortResult = solveAssemblyMates(*shortProduct, kRoot);
      CHECK(shortResult.status == MateSolveStatus::ResourceFailure);
      return required;
    };
    const auto originWork = exactBudget([] {
      return makePointDistanceProduct(2.0, 5.0);
    });
    const auto circleWork = exactBudget([] {
      return makeCircularPointDistanceProduct();
    });
    CHECK(circleWork > originWork);

    // The lookup charge is linear in the authoritative index size and is
    // reserved before resolveEdge.  A many-edge V2 source therefore has the
    // same exact N/N-1 boundary, without an unmetered provenance scan.
    const auto manyBaseline = makeManyEdgeCircularPointDistanceProduct();
    CHECK(manyBaseline.product);
    CHECK(manyBaseline.edgeCount >= 72);
    const auto manySolved =
        solveAssemblyMates(*manyBaseline.product, kRoot);
    CHECK(manySolved.candidate);
    const auto manyRequired = manySolved.diagnostics.requiredWorkUnits;
    CHECK(manyRequired >= originWork + manyBaseline.edgeCount);

    auto manyExact = makeManyEdgeCircularPointDistanceProduct();
    auto manyExactOptions = manyExact.product->rootAssemblyDefinition()
                                ->mateRegistry()
                                .options();
    manyExactOptions.maximumWorkUnits = manyRequired;
    CHECK(const_cast<MateRegistry&>(manyExact.product
                                         ->rootAssemblyDefinition()
                                         ->mateRegistry())
              .setOptions(manyExactOptions) ==
          MateRegistryMutationStatus::Inserted);
    const auto manyExactResult =
        solveAssemblyMates(*manyExact.product, kRoot);
    CHECK(manyExactResult.status != MateSolveStatus::ResourceFailure);
    CHECK(manyExactResult.diagnostics.requiredWorkUnits == manyRequired);

    auto manyShort = makeManyEdgeCircularPointDistanceProduct();
    auto manyShortOptions = manyShort.product->rootAssemblyDefinition()
                                ->mateRegistry()
                                .options();
    manyShortOptions.maximumWorkUnits = manyRequired - 1;
    CHECK(const_cast<MateRegistry&>(manyShort.product
                                         ->rootAssemblyDefinition()
                                         ->mateRegistry())
              .setOptions(manyShortOptions) ==
          MateRegistryMutationStatus::Inserted);
    const auto manyShortResult =
        solveAssemblyMates(*manyShort.product, kRoot);
    CHECK(manyShortResult.status == MateSolveStatus::ResourceFailure);
  }

  // The shared planar resolver meters the complete authoritative V2 face
  // lookup before resolveFace.  A many-face Angle source therefore accepts
  // exact N and rejects N-1 without changing its geometric result.
  {
    auto small = makeManyFaceAngleProduct(1);
    const auto smallSolved = solveAssemblyMates(*small.product, kRoot);
    CHECK(smallSolved.candidate);
    const auto smallFaceCount = small.faceCount;

    auto baseline = makeManyFaceAngleProduct();
    CHECK(baseline.product);
    CHECK(baseline.faceCount >= 72);
    const auto solved = solveAssemblyMates(*baseline.product, kRoot);
    CHECK(solved.candidate);
    CHECK(solved.diagnostics.rank == 1);
    const auto required = solved.diagnostics.requiredWorkUnits;
    CHECK(required >= baseline.faceCount);
    CHECK(baseline.faceCount > smallFaceCount);
    CHECK(required - smallSolved.diagnostics.requiredWorkUnits >=
          baseline.faceCount - smallFaceCount);

    auto exact = makeManyFaceAngleProduct();
    auto exactOptions =
        exact.product->rootAssemblyDefinition()->mateRegistry().options();
    exactOptions.maximumWorkUnits = required;
    CHECK(const_cast<MateRegistry&>(exact.product->rootAssemblyDefinition()
                                       ->mateRegistry())
              .setOptions(exactOptions) ==
          MateRegistryMutationStatus::Inserted);
    const auto exactResult = solveAssemblyMates(*exact.product, kRoot);
    CHECK(exactResult.candidate);
    CHECK(exactResult.diagnostics.requiredWorkUnits == required);

    auto shortBudget = makeManyFaceAngleProduct();
    auto shortOptions = shortBudget.product->rootAssemblyDefinition()
                            ->mateRegistry()
                            .options();
    shortOptions.maximumWorkUnits = required - 1;
    CHECK(const_cast<MateRegistry&>(shortBudget.product
                                       ->rootAssemblyDefinition()
                                       ->mateRegistry())
              .setOptions(shortOptions) ==
          MateRegistryMutationStatus::Inserted);
    const auto rejected = solveAssemblyMates(*shortBudget.product, kRoot);
    CHECK(rejected.status == MateSolveStatus::ResourceFailure);
    CHECK(!rejected.candidate);
  }

  // Cylindrical V2 face lookup has the same differential and exact ceiling:
  // the many-face semantic solve costs at least the authoritative count delta.
  {
    auto small = makeManyFaceConcentricProduct(1);
    const auto smallSolved = solveAssemblyMates(*small.product, kRoot);
    CHECK(smallSolved.candidate);
    const auto smallFaceCount = small.faceCount;

    auto many = makeManyFaceConcentricProduct();
    CHECK(many.product && many.faceCount > smallFaceCount);
    const auto manySolved = solveAssemblyMates(*many.product, kRoot);
    CHECK(manySolved.candidate);
    const auto required = manySolved.diagnostics.requiredWorkUnits;
    CHECK(required - smallSolved.diagnostics.requiredWorkUnits >=
          many.faceCount - smallFaceCount);

    auto exact = makeManyFaceConcentricProduct();
    auto exactOptions =
        exact.product->rootAssemblyDefinition()->mateRegistry().options();
    exactOptions.maximumWorkUnits = required;
    CHECK(const_cast<MateRegistry&>(exact.product->rootAssemblyDefinition()
                                       ->mateRegistry())
              .setOptions(exactOptions) ==
          MateRegistryMutationStatus::Inserted);
    const auto exactSolved = solveAssemblyMates(*exact.product, kRoot);
    CHECK(exactSolved.candidate);
    CHECK(exactSolved.diagnostics.requiredWorkUnits == required);

    auto shortBudget = makeManyFaceConcentricProduct();
    auto shortOptions = shortBudget.product->rootAssemblyDefinition()
                            ->mateRegistry()
                            .options();
    shortOptions.maximumWorkUnits = required - 1;
    CHECK(const_cast<MateRegistry&>(shortBudget.product
                                       ->rootAssemblyDefinition()
                                       ->mateRegistry())
              .setOptions(shortOptions) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(solveAssemblyMates(*shortBudget.product, kRoot).status ==
          MateSolveStatus::ResourceFailure);
  }

  // Circular topology participates in the candidate seal; losing the feature
  // result after solve is a stale no-op before successor publication.
  {
    auto product = makeCircularPointDistanceProduct();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    auto* part =
        const_cast<PartDefinition*>(product->definitions().findPart(kPart));
    CHECK(part);
    const auto* relation =
        product->rootAssemblyDefinition()->mateRegistry().findDistance(
            MateId{1});
    CHECK(relation);
    const auto& edge =
        std::get<CircularEdgeCenter>(relation->second().reference).edge;
    CHECK(edge.v2());
    auto* feature = part->document().findFeature(
        edge.v2()->bodyId, edge.v2()->provenance.featureId);
    CHECK(feature);
    feature->discardResult();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // A successful terminal Part recompute invalidates the old candidate by
  // its V2 topology revision.  The authored provenance remains resolvable,
  // so a fresh solve/apply reaches the same stable fixed point.
  {
    auto product = makeCircularPointDistanceProduct();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    auto* part =
        const_cast<PartDefinition*>(product->definitions().findPart(kPart));
    CHECK(part);
    auto* feature = dynamic_cast<ExtrudeFeature*>(
        part->document().findFeature(BodyId{2102}, FeatureId{2103}));
    CHECK(feature);
    feature->setLengthMm(9.0);
    CHECK(part->document().recomputeDetailed().success);
    Successor staleSuccessor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate,
                                               staleSuccessor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(staleSuccessor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);

    const auto refreshed = solveAssemblyMates(*product, kRoot);
    CHECK(refreshed.candidate);
    Successor applied(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*refreshed.candidate,
                                               applied) ==
          MatePlacementApplyStatus::Applied);
    CHECK(applied.calls == 1);
    const auto fixedPoint = solveAssemblyMates(*product, kRoot);
    CHECK(fixedPoint.candidate);
    CHECK(std::all_of(fixedPoint.candidate->changes().begin(),
                      fixedPoint.candidate->changes().end(),
                      [](const auto& change) {
                        return change.afterPlacement ==
                               change.before.placement;
                      }));
  }

  {
    auto product = makePointDistanceProduct(2.0, 5.0);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    const auto* authored = registry.findDistance(MateId{1});
    CHECK(authored);
    const MatePointEndpoint first = authored->first();
    const MatePointEndpoint second = authored->second();
    const auto fingerprint = registry.fingerprint();
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(
              PointDistanceMate{MateId{1}, first, second, 6.0}) ==
          MateRegistryMutationStatus::Inserted);
    CHECK(registry.fingerprint() != fingerprint);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // Angle bits and orientation are sealed: an authored registry mutation is
  // stale before successor publication and before any placement mutation.
  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        false);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    const auto* authored = registry.findAngle(MateId{1});
    CHECK(authored);
    const MateEndpoint first = authored->first();
    const MateEndpoint second = authored->second();
    CHECK(registry.erase(MateId{1}) == MateRegistryMutationStatus::Erased);
    CHECK(registry.insert(PlanarAngleMate{
              MateId{1}, first, second, PlanarMateOrientation::Opposed,
              0.6}) == MateRegistryMutationStatus::Inserted);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // Referenced planar topology is part of the Angle seal. Discard and a
  // successful recompute stale the old candidate before publication; the
  // recomputed provenance supports a fresh solve/apply/fixed point.
  for (const bool recompute : {false, true}) {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        false);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    auto* part =
        const_cast<PartDefinition*>(product->definitions().findPart(kPart));
    CHECK(part);
    auto* feature = dynamic_cast<ExtrudeFeature*>(
        part->document().findFeature(BodyId{1202}, FeatureId{1203}));
    CHECK(feature);
    if (recompute) {
      feature->setLengthMm(7.0);
      CHECK(part->document().recomputeDetailed().success);
    } else {
      feature->discardResult();
    }
    Successor stale(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, stale) ==
          MatePlacementApplyStatus::Stale);
    CHECK(stale.calls == 0);
    if (recompute) {
      const auto refreshed = solveAssemblyMates(*product, kRoot);
      CHECK(refreshed.candidate);
      Successor applied(ProductDocumentSuccessorStatus::Prepared);
      CHECK(product->applyMatePlacementCandidate(*refreshed.candidate,
                                                 applied) ==
            MatePlacementApplyStatus::Applied);
      CHECK(applied.calls == 1);
      const auto fixedPoint = solveAssemblyMates(*product, kRoot);
      CHECK(fixedPoint.candidate);
      CHECK(fixedPoint.diagnostics.rank == 1);
      CHECK(fixedPoint.diagnostics.maximumAngularResidualRad <=
            product->rootAssemblyDefinition()
                ->mateRegistry()
                .options()
                .angularToleranceRad);
      CHECK(std::all_of(fixedPoint.candidate->changes().begin(),
                        fixedPoint.candidate->changes().end(),
                        [](const auto& change) {
                          return change.before.placement ==
                                 change.afterPlacement;
                        }));
    }
  }

  {
    auto product = makeNestedAngleProduct();
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    auto* nested = const_cast<AssemblyDefinition*>(
        product->definitions().findAssembly(DefinitionId{3}));
    CHECK(nested);
    const auto* relation = product->rootAssemblyDefinition()
                               ->mateRegistry()
                               .findAngle(MateId{1});
    CHECK(relation);
    const auto& document =
        product->definitions().findPart(kPart)->document();
    const auto local = localFaceFrame(document, relation->second().face);
    const auto* nestedLeaf = nested->find(OccurrenceId{5});
    CHECK(nestedLeaf);
    const auto pivotLocalToDirect =
        transformPoint(nestedLeaf->placement(), local.point);
    const auto movingBeforeSolve =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    const auto movingChange = std::find_if(
        solved.candidate->changes().begin(), solved.candidate->changes().end(),
        [](const auto& value) { return value.before.occurrenceId == kMoving; });
    CHECK(movingChange != solved.candidate->changes().end());
    CHECK(pointDistance(transformPoint(movingBeforeSolve, pivotLocalToDirect),
                        transformPoint(movingChange->afterPlacement,
                                       pivotLocalToDirect)) <= 1.0e-9);
    const auto changed = RigidTransform3d::create(
        {17.0, 19.0, 23.0},
        {0.8660254037844386, 0.0, 0.0, 0.5});
    CHECK(changed);
    CHECK(AssemblyMateDependencyTestAccess::setPlacement(
        *nested, OccurrenceId{5}, *changed.transform));
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
  }

  // Angle candidate publication is exactly-once and every failure path is a
  // placement no-op with precise successor call counts.
  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        false);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    const auto beforeFixed =
        product->rootAssemblyDefinition()->find(kFixed)->placement();
    const auto beforeMoving =
        product->rootAssemblyDefinition()->find(kMoving)->placement();
    Successor rejected(ProductDocumentSuccessorStatus::Rejected);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, rejected) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(rejected.calls == 1);
    Successor resource(ProductDocumentSuccessorStatus::ResourceFailure);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, resource) ==
          MatePlacementApplyStatus::ResourceFailure);
    CHECK(resource.calls == 1);
    ReentrantSuccessor busy(*solved.candidate);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, busy) ==
          MatePlacementApplyStatus::SuccessorRejected);
    CHECK(busy.solveStatus == MateSolveStatus::Busy);
    CHECK(busy.applyStatus == MatePlacementApplyStatus::Busy);
    CHECK(busy.nestedSuccessorCalls == 0);
    CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
          beforeFixed);
    CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
          beforeMoving);
    Successor accepted(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, accepted) ==
          MatePlacementApplyStatus::Applied);
    CHECK(accepted.calls == 1);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, accepted) ==
          MatePlacementApplyStatus::Stale);
    CHECK(accepted.calls == 1);
  }
  {
    auto product = makeAngularProduct(
        {{AssemblyMateKind::PlanarAngle, PlanarMateOrientation::Aligned,
          MateId{1}, 2, 0.4}},
        false);
    const auto solved = solveAssemblyMates(*product, kRoot);
    CHECK(solved.candidate);
    auto& registry = const_cast<MateRegistry&>(
        product->rootAssemblyDefinition()->mateRegistry());
    auto options = registry.options();
    options.maximumAngularStepRad *= 0.5;
    CHECK(registry.setOptions(options) ==
          MateRegistryMutationStatus::Inserted);
    Successor successor(ProductDocumentSuccessorStatus::Prepared);
    CHECK(product->applyMatePlacementCandidate(*solved.candidate, successor) ==
          MatePlacementApplyStatus::Stale);
    CHECK(successor.calls == 0);
  }

  // Full and trimmed circular curves share the exact OCCT circle center.
  {
    for (const bool trimmed : {false, true}) {
      auto part = makeSyntheticCircularPart(trimmed);
      auto product = makeEdgePointDistanceProduct(
          std::move(part.document), std::move(part.edge), 10.0);
      const auto solved = solveAssemblyMates(*product, kRoot);
      CHECK(solved.status == MateSolveStatus::Underconstrained);
      CHECK(solved.candidate);
      const auto& placement =
          solved.candidate->changes().back().afterPlacement.translationMm();
      const std::array<double, 3> localCenter{0.0, 2.0, 3.0};
      const std::array<double, 3> worldCenter{
          placement[0] + localCenter[0], placement[1] + localCenter[1],
          placement[2] + localCenter[2]};
      CHECK(std::abs(std::hypot(worldCenter[0], worldCenter[1],
                                worldCenter[2]) -
                     10.0) <= 1.0e-9);
      const double initialLength = std::sqrt(44.0);
      const std::array<double, 3> expectedWorld{
          2.0 * 10.0 / initialLength, 2.0 * 10.0 / initialLength,
          6.0 * 10.0 / initialLength};
      for (std::size_t index = 0; index < 3; ++index)
        CHECK(std::abs(worldCenter[index] - expectedWorld[index]) <=
              1.0e-9);
    }
  }

  // Circular V2 endpoint failures preserve exact mate/side taxonomy and do
  // not mutate either occurrence.
  {
    const auto expectEndpointFailure = [](std::unique_ptr<ProductDocument> product,
                                          MateSolveStatus status,
                                          MateEndpointFailure failure) {
      const auto beforeFixed =
          product->rootAssemblyDefinition()->find(kFixed)->placement();
      const auto beforeMoving =
          product->rootAssemblyDefinition()->find(kMoving)->placement();
      const auto result = solveAssemblyMates(*product, kRoot);
      CHECK(result.status == status);
      CHECK(!result.candidate);
      CHECK(result.diagnostics.endpointFailure);
      CHECK(result.diagnostics.endpointFailure->mateId == MateId{1});
      CHECK(result.diagnostics.endpointFailure->secondEndpoint);
      CHECK(result.diagnostics.endpointFailure->failure == failure);
      CHECK(product->rootAssemblyDefinition()->find(kFixed)->placement() ==
            beforeFixed);
      CHECK(product->rootAssemblyDefinition()->find(kMoving)->placement() ==
            beforeMoving);
    };

    auto ambiguous = makeSyntheticCircularPart(false, true);
    expectEndpointFailure(
        makeEdgePointDistanceProduct(std::move(ambiguous.document),
                                     std::move(ambiguous.edge)),
        MateSolveStatus::Ambiguous, MateEndpointFailure::Ambiguous);

    auto missing = makeSyntheticCircularPart(false);
    auto missingReference = *missing.edge.v2();
    missingReference.provenance.localName += ":missing";
    expectEndpointFailure(
        makeEdgePointDistanceProduct(
            std::move(missing.document),
            EdgeReference::fromV2(std::move(missingReference))),
        MateSolveStatus::Missing, MateEndpointFailure::Missing);

    auto bodyMissing = makeSyntheticCircularPart(false);
    auto bodyReference = *bodyMissing.edge.v2();
    bodyReference.bodyId = BodyId{999999};
    expectEndpointFailure(
        makeEdgePointDistanceProduct(
            std::move(bodyMissing.document),
            EdgeReference::fromV2(std::move(bodyReference))),
        MateSolveStatus::Missing, MateEndpointFailure::BodyMissing);

    auto featureMissing = makeSyntheticCircularPart(false);
    auto featureReference = *featureMissing.edge.v2();
    featureReference.provenance.featureId = FeatureId{999999};
    expectEndpointFailure(
        makeEdgePointDistanceProduct(
            std::move(featureMissing.document),
            EdgeReference::fromV2(std::move(featureReference))),
        MateSolveStatus::Missing, MateEndpointFailure::FeatureMissing);

    auto nonAuthoritative =
        makeSyntheticCircularPart(false, false, false);
    expectEndpointFailure(
        makeEdgePointDistanceProduct(std::move(nonAuthoritative.document),
                                     std::move(nonAuthoritative.edge)),
        MateSolveStatus::Stale,
        MateEndpointFailure::NonAuthoritativeTopology);
  }

  // A V2 edge that resolves authoritatively but is not circular is a precise
  // Broken endpoint diagnostic, not an arbitrary center or silent fallback.
  {
    auto part = makePart();
    constexpr BodyId bodyId{1102};
    constexpr FeatureId featureId{1103};
    auto* sourceFeature = dynamic_cast<ShapeFeature*>(
        part.document->findFeature(bodyId, featureId));
    CHECK(sourceFeature);
    auto edge = sourceFeature->createEdgeReferenceV2(bodyId, 0);
    CHECK(edge);
    AssemblyDefinition rootAssembly(kRoot);
    CHECK(rootAssembly.insert(ComponentOccurrence(
              kFixed, kPart, RigidTransform3d::identity(),
              OccurrenceFlags{true, false, true})) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    const auto moving = RigidTransform3d::create(
        {2.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0});
    CHECK(moving);
    CHECK(rootAssembly.insert(ComponentOccurrence(
              kMoving, kPart, *moving.transform)) ==
          AssemblyOccurrenceInsertStatus::Inserted);
    CHECK(rootAssembly.mateRegistry().insert(PointDistanceMate{
              MateId{1},
              MatePointEndpoint{path(kFixed), LocalDatumPoint::Origin},
              MatePointEndpoint{path(kMoving),
                                CircularEdgeCenter{edge.reference}},
              5.0}) == MateRegistryMutationStatus::Inserted);
    DefinitionRegistry definitions;
    CHECK(definitions.insert(
              PartDefinition(kPart, std::move(part.document))) ==
          DefinitionInsertStatus::Inserted);
    CHECK(definitions.insert(std::move(rootAssembly)) ==
          DefinitionInsertStatus::Inserted);
    auto made = ProductDocument::create(std::move(definitions), kRoot);
    CHECK(made);
    const auto broken = solveAssemblyMates(*made.product, kRoot);
    CHECK(broken.status == MateSolveStatus::Broken);
    CHECK(!broken.candidate);
    CHECK(broken.diagnostics.endpointFailure);
    CHECK(broken.diagnostics.endpointFailure->failure ==
          MateEndpointFailure::NonCircular);
  }

  return EXIT_SUCCESS;
}
