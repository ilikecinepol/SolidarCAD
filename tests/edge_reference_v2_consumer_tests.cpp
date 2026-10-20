#ifdef NDEBUG
#undef NDEBUG
#endif

#include "TestAssertions.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "model/ChamferBuilder.h"
#include "model/ChamferFeature.h"
#include "model/ChamferToolSession.h"
#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/DraftToolSession.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/FilletToolSession.h"
#include "model/ImportedShapeFeature.h"
#include "model/TopologyLineage.h"
#include "model/TopologyReferenceResolver.h"
#include "ui/application/PartDesignCoordinator.h"

namespace {

solidar::TopologyProvenanceKey makeKey(
    solidar::FeatureId featureId, solidar::TopologyKind kind,
    std::string localName) {
  return {featureId, kind, solidar::TopologyProvenanceOperation::Seed,
          kind == solidar::TopologyKind::Face
              ? solidar::TopologySemanticRole::Side
              : solidar::TopologySemanticRole::SketchBoundary,
          std::move(localName)};
}

std::string pointName(const gp_Pnt& point) {
  const auto coordinate = [](double value) {
    return std::to_string(static_cast<long long>(std::llround(value * 1000.0)));
  };
  return coordinate(point.X()) + "," + coordinate(point.Y()) + "," +
         coordinate(point.Z());
}

std::string edgeGeometryName(const TopoDS_Shape& shape) {
  TopoDS_Vertex first;
  TopoDS_Vertex last;
  TopExp::Vertices(TopoDS::Edge(shape), first, last);
  CHECK(!first.IsNull() && !last.IsNull());
  std::string a = pointName(BRep_Tool::Pnt(first));
  std::string b = pointName(BRep_Tool::Pnt(last));
  if (b < a) std::swap(a, b);
  return "consumer:edge:" + a + "->" + b;
}

std::string faceGeometryName(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  return "consumer:face:" + pointName(gp_Pnt(x0, y0, z0)) + "->" +
         pointName(gp_Pnt(x1, y1, z1));
}

std::vector<std::shared_ptr<const TopoDS_Shape>> canonicalSubshapes(
    const TopoDS_Shape& shape, TopAbs_ShapeEnum kind) {
  std::vector<std::shared_ptr<const TopoDS_Shape>> result;
  for (TopExp_Explorer explorer(shape, kind); explorer.More(); explorer.Next()) {
    const auto candidate =
        std::make_shared<TopoDS_Shape>(explorer.Current());
    bool duplicate = false;
    for (const auto& existing : result)
      if (existing->IsSame(*candidate)) {
        duplicate = true;
        break;
      }
    if (!duplicate) result.push_back(std::move(candidate));
  }
  return result;
}

class SyntheticEdgeSource final : public solidar::ShapeFeature {
 public:
  explicit SyntheticEdgeSource(double offset = 0.0)
      : ShapeFeature("Synthetic edge source"), offset_(offset) {}

  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<SyntheticEdgeSource>(*this);
  }

  void setReverseBindings(bool value) {
    if (reverseBindings_ == value) return;
    reverseBindings_ = value;
    setDirty();
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override {
    auto shape = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeBox(gp_Pnt(offset_, 0.0, 0.0), 30.0, 20.0, 15.0)
            .Shape());
    std::vector<solidar::TopologyLineageBinding> bindings;
    const auto faces = canonicalSubshapes(*shape, TopAbs_FACE);
    const auto edges = canonicalSubshapes(*shape, TopAbs_EDGE);
    for (const auto& face : faces)
      bindings.push_back(
          {makeKey(id(), solidar::TopologyKind::Face,
                   faceGeometryName(*face)),
           face});
    for (const auto& edge : edges)
      bindings.push_back(
          {makeKey(id(), solidar::TopologyKind::Edge,
                   edgeGeometryName(*edge)),
           edge});
    if (reverseBindings_) std::reverse(bindings.begin(), bindings.end());
    std::string error;
    auto lineage = solidar::TopologyLineage::create(
        std::move(bindings),
        solidar::TopologyLineageCoverage::PartialFaces, &error);
    if (!lineage) {
      markError(std::move(error));
      return false;
    }
    setShapeAndTopologyLineage(std::move(shape), std::move(lineage));
    markValid();
    return true;
  }

 private:
  double offset_{};
  bool reverseBindings_{};
};

struct SourceModel {
  solidar::Body* body{};
  SyntheticEdgeSource* source{};
  std::shared_ptr<const solidar::TopologyIndex> topology;
};

SourceModel addSource(solidar::Document& document, double offset = 0.0) {
  auto& body = document.addBody();
  auto source = std::make_unique<SyntheticEdgeSource>(offset);
  auto* sourcePtr = source.get();
  body.addFeature(std::move(source));
  CHECK(document.recompute());
  const auto topology = sourcePtr->topologyIndex();
  CHECK(topology);
  return {&body, sourcePtr, topology};
}

struct EdgePair {
  solidar::EdgeReference v1;
  solidar::EdgeReference v2;
  std::size_t index{};
};

EdgePair usableEdge(const SourceModel& model) {
  for (std::size_t raw = 0; raw < model.topology->rawEdgeCount(); ++raw) {
    const auto v1 = model.topology->createEdgeReference(
        model.body->id(), model.source->id(), raw);
    const auto v2 = model.topology->createEdgeReferenceV2(
        model.body->id(), model.source->id(), raw);
    if (!v1 || !v2) continue;
    const auto resolved = model.topology->resolveEdge(v2.reference);
    if (!resolved) continue;
    if (!solidar::buildFilletShape(*model.source->shape(), {resolved.index},
                                   1.0))
      continue;
    if (!solidar::buildChamferShape(*model.source->shape(), {resolved.index},
                                    1.0))
      continue;
    return {v1.reference, v2.reference, resolved.index};
  }
  CHECK(false);
  return {};
}

solidar::EdgeReference missingReference(const solidar::EdgeReference& value) {
  CHECK(value.v2());
  auto missing = *value.v2();
  missing.provenance.localName += ":missing";
  return solidar::EdgeReference::fromV2(std::move(missing));
}

solidar::EdgeReference invalidKindReference(
    const solidar::EdgeReference& value) {
  CHECK(value.v2());
  auto invalid = *value.v2();
  invalid.provenance.kind = solidar::TopologyKind::Face;
  return solidar::EdgeReference::fromV2(std::move(invalid));
}

solidar::FaceReference missingReference(const solidar::FaceReference& value) {
  CHECK(value.v2());
  auto missing = *value.v2();
  missing.provenance.localName += ":missing";
  return solidar::FaceReference::fromV2(std::move(missing));
}

struct DraftReferences {
  solidar::FaceReference face;
  solidar::FaceReference faceV2;
  solidar::EdgeReference edge;
};

DraftReferences usableDraftReferences(const SourceModel& model,
                                      const solidar::Document& document) {
  for (std::size_t rawFace = 0;
       rawFace < model.topology->rawFaceCount(); ++rawFace) {
    const auto face = model.topology->createFaceReference(
        model.body->id(), model.source->id(), rawFace);
    const auto faceV2 = model.topology->createFaceReferenceV2(
        model.body->id(), model.source->id(), rawFace);
    if (!face || !faceV2) continue;
    for (std::size_t rawEdge = 0;
         rawEdge < model.topology->rawEdgeCount(); ++rawEdge) {
      const auto edge = model.topology->createEdgeReferenceV2(
          model.body->id(), model.source->id(), rawEdge);
      if (!edge) continue;
      gp_Pln plane;
      gp_Dir direction;
      const auto resolution = solidar::resolveDraftEdgeAxis(
          *model.source->shape(), *model.topology, face.reference,
          edge.reference, &plane, &direction);
      if (!resolution) continue;
      const auto resolvedFace = model.topology->resolveFace(face.reference);
      if (!resolvedFace) continue;
      if (!solidar::buildDraftShape(*model.source->shape(),
                                    {resolvedFace.index}, plane, direction,
                                    5.0, false))
        continue;
      (void)document;
      return {face.reference, faceV2.reference, edge.reference};
    }
  }
  CHECK(false);
  return {};
}

struct BodyPlaneDraftReferences {
  solidar::FaceReference draftedFace;
  solidar::PlaneReference neutralPlane;
  solidar::AxisReference pullDirection;
};

BodyPlaneDraftReferences usableBodyPlaneDraftReferences(
    const SourceModel& model, const solidar::Document& document) {
  for (std::size_t draftedRaw = 0;
       draftedRaw < model.topology->rawFaceCount(); ++draftedRaw) {
    const auto drafted = model.topology->createFaceReference(
        model.body->id(), model.source->id(), draftedRaw);
    if (!drafted) continue;
    for (std::size_t neutralRaw = 0;
         neutralRaw < model.topology->rawFaceCount(); ++neutralRaw) {
      const auto neutral = model.topology->createFaceReferenceV2(
          model.body->id(), model.source->id(), neutralRaw);
      if (!neutral) continue;
      const solidar::PlaneReference plane{
          solidar::NeutralPlaneType::BodyFace, neutral.reference};
      for (const auto axis : {solidar::AxisReferenceType::GlobalX,
                              solidar::AxisReferenceType::GlobalY,
                              solidar::AxisReferenceType::GlobalZ}) {
        const solidar::AxisReference direction{axis};
        gp_Pln resolvedPlane;
        gp_Dir resolvedDirection;
        if (!solidar::resolveDraftReferences(
                document, *model.source->shape(), *model.topology, plane,
                direction, &resolvedPlane, &resolvedDirection))
          continue;
        const auto resolvedFace =
            model.topology->resolveFace(drafted.reference);
        if (!resolvedFace) continue;
        if (!solidar::buildDraftShape(
                *model.source->shape(), {resolvedFace.index}, resolvedPlane,
                resolvedDirection, 5.0, false))
          continue;
        return {drafted.reference, plane, direction};
      }
    }
  }
  CHECK(false);
  return {};
}

solidar::EdgeReference usableV1Edge(
    const std::shared_ptr<const TopoDS_Shape>& shape,
    const std::shared_ptr<const solidar::TopologyIndex>& topology,
    solidar::BodyId bodyId, solidar::FeatureId featureId) {
  for (std::size_t raw = 0; raw < topology->rawEdgeCount(); ++raw) {
    const auto edge = topology->createEdgeReference(bodyId, featureId, raw);
    if (!edge) continue;
    const auto resolved = topology->resolveEdge(edge.reference);
    if (!resolved) continue;
    if (solidar::buildFilletShape(*shape, {resolved.index}, 1.0) &&
        solidar::buildChamferShape(*shape, {resolved.index}, 1.0))
      return edge.reference;
  }
  CHECK(false);
  return {};
}

struct V1DraftReferences {
  solidar::FaceReference face;
  solidar::PlaneReference plane;
  solidar::AxisReference direction;
};

V1DraftReferences usableV1DraftReferences(
    const solidar::Document& document,
    const std::shared_ptr<const TopoDS_Shape>& shape,
    const std::shared_ptr<const solidar::TopologyIndex>& topology,
    solidar::BodyId bodyId, solidar::FeatureId featureId) {
  const std::array planes{solidar::NeutralPlaneType::GlobalYZ,
                          solidar::NeutralPlaneType::GlobalXZ,
                          solidar::NeutralPlaneType::GlobalXY};
  const std::array axes{solidar::AxisReferenceType::GlobalX,
                        solidar::AxisReferenceType::GlobalY,
                        solidar::AxisReferenceType::GlobalZ};
  for (std::size_t raw = 0; raw < topology->rawFaceCount(); ++raw) {
    const auto face = topology->createFaceReference(bodyId, featureId, raw);
    if (!face) continue;
    const auto resolvedFace = topology->resolveFace(face.reference);
    if (!resolvedFace) continue;
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
      const solidar::PlaneReference plane{planes[axis]};
      const solidar::AxisReference direction{axes[axis]};
      gp_Pln resolvedPlane;
      gp_Dir resolvedDirection;
      if (!solidar::resolveDraftReferences(document, *shape, *topology, plane,
                                           direction, &resolvedPlane,
                                           &resolvedDirection))
        continue;
      if (solidar::buildDraftShape(*shape, {resolvedFace.index}, resolvedPlane,
                                   resolvedDirection, 5.0, false))
        return {face.reference, plane, direction};
    }
  }
  CHECK(false);
  return {};
}

}  // namespace

int main() {
  // Fillet consumes exact and legacy aliases in either order. Resolution of
  // the complete batch precedes stable deduplication and every OCCT build.
  {
    solidar::Document document;
    const auto model = addSource(document);
    const auto edge = usableEdge(model);
    auto feature = std::make_unique<solidar::FilletFeature>(
        std::vector<solidar::EdgeReference>{edge.v1, edge.v2}, 1.0,
        "Mixed v1/v2 fillet");
    auto* featurePtr = feature.get();
    model.body->addFeature(std::move(feature));
    CHECK(document.recompute());
    CHECK(featurePtr->isValid() && featurePtr->shape());
    featurePtr->setEdges({edge.v2, edge.v1});
    CHECK(document.recompute());
    const auto semanticName = edge.v2.v2()->provenance.localName;
    model.source->setReverseBindings(true);
    CHECK(document.recompute());
    const auto regeneratedTopology = model.source->topologyIndex();
    CHECK(regeneratedTopology);
    const auto regeneratedEdge = regeneratedTopology->resolveEdge(edge.v2);
    CHECK(regeneratedEdge);
    CHECK(edgeGeometryName(*regeneratedEdge.subshape) == semanticName);

    const auto lastValid = featurePtr->shape();
    featurePtr->setEdges({edge.v2, missingReference(edge.v2)});
    CHECK(!document.recompute());
    CHECK(featurePtr->isFailed());
    CHECK(!featurePtr->shape());
    CHECK(featurePtr->lastValidShape() == lastValid);
    featurePtr->setEdges({edge.v2});
    CHECK(document.recompute());
    CHECK(featurePtr->isValid());
  }

  // Chamfer has the same mixed-order contract and recovers after an exact
  // reference failure without losing its last committed B-Rep.
  {
    solidar::Document document;
    const auto model = addSource(document);
    const auto edge = usableEdge(model);
    auto feature = std::make_unique<solidar::ChamferFeature>(
        std::vector<solidar::EdgeReference>{edge.v2, edge.v1}, 1.0,
        "Mixed v2/v1 chamfer");
    auto* featurePtr = feature.get();
    model.body->addFeature(std::move(feature));
    CHECK(document.recompute());
    featurePtr->setEdges({edge.v1, edge.v2});
    CHECK(document.recompute());
    const auto lastValid = featurePtr->shape();
    featurePtr->setEdges({invalidKindReference(edge.v2)});
    CHECK(!document.recompute());
    CHECK(featurePtr->error() ==
          "Chamfer edge could not be resolved: Topology v2 reference has invalid edge identity");
    CHECK(featurePtr->lastValidShape() == lastValid);
    featurePtr->setEdges({edge.v2});
    CHECK(document.recompute());
  }

  // A model feature may consume only the exact immediate predecessor's
  // topology snapshot, even if an older feature owns an otherwise valid V2.
  {
    solidar::Document document;
    const auto first = addSource(document);
    const auto edge = usableEdge(first);
    auto second = std::make_unique<SyntheticEdgeSource>(50.0);
    first.body->addFeature(std::move(second));
    CHECK(document.recompute());
    auto feature = std::make_unique<solidar::FilletFeature>(edge.v2, 1.0);
    auto* featurePtr = feature.get();
    first.body->addFeature(std::move(feature));
    CHECK(!document.recompute());
    CHECK(featurePtr->error() == "Fillet source Feature could not be resolved");
  }
  {
    solidar::Document document;
    const auto first = addSource(document);
    const auto edge = usableEdge(first);
    first.body->addFeature(std::make_unique<SyntheticEdgeSource>(50.0));
    CHECK(document.recompute());
    auto feature = std::make_unique<solidar::ChamferFeature>(edge.v2, 1.0);
    auto* featurePtr = feature.get();
    first.body->addFeature(std::move(feature));
    CHECK(!document.recompute());
    CHECK(featurePtr->error() ==
          "Chamfer source Feature could not be resolved");
  }
  {
    solidar::Document document;
    const auto first = addSource(document);
    const auto references = usableDraftReferences(first, document);
    first.body->addFeature(std::make_unique<SyntheticEdgeSource>(50.0));
    CHECK(document.recompute());
    auto feature = std::make_unique<solidar::DraftFeature>(
        first.source->id(), std::vector{references.face},
        solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
        solidar::AxisReference{solidar::AxisReferenceType::GlobalZ}, 5.0,
        false, "Non-immediate draft", references.edge);
    auto* featurePtr = feature.get();
    first.body->addFeature(std::move(feature));
    CHECK(!document.recompute());
    CHECK(featurePtr->error() == "Draft source Feature could not be resolved");
  }

  static_assert(!std::is_default_constructible_v<
                solidar::FeatureTopologySnapshot>);
  static_assert(!std::is_constructible_v<
                solidar::FeatureTopologySnapshot, solidar::BodyId,
                solidar::FeatureId, solidar::ShapeRevision,
                std::shared_ptr<const TopoDS_Shape>,
                std::shared_ptr<const solidar::TopologyIndex>, bool>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::FilletToolSession::begin),
                solidar::FilletToolSession&, solidar::BodyId,
                solidar::FeatureId, solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::EdgeReference>, double>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::DraftToolSession::begin),
                solidar::DraftToolSession&, const solidar::Document&,
                solidar::BodyId, solidar::FeatureId,
                solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::FaceReference>>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::ChamferToolSession::begin),
                solidar::ChamferToolSession&, solidar::BodyId,
                solidar::FeatureId, solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::EdgeReference>, double>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::PartDesignCoordinator::beginFillet),
                solidar::PartDesignCoordinator&, solidar::BodyId,
                solidar::FeatureId, solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::EdgeReference>, double>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::PartDesignCoordinator::beginChamfer),
                solidar::PartDesignCoordinator&, solidar::BodyId,
                solidar::FeatureId, solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::EdgeReference>, double>);
  static_assert(!std::is_invocable_v<
                decltype(&solidar::PartDesignCoordinator::beginDraft),
                solidar::PartDesignCoordinator&, const solidar::Document&,
                solidar::BodyId, solidar::FeatureId,
                solidar::ShapeFeature::ShapePtr,
                std::vector<solidar::FaceReference>>);

  {
    solidar::Document document;
    const auto model = addSource(document);
    const auto snapshot = model.body->topologySnapshot(model.source->id());
    CHECK(snapshot && model.body->matchesTopologySnapshot(*snapshot));
    model.source->setReverseBindings(true);
    CHECK(document.recompute());
    CHECK(!model.body->matchesTopologySnapshot(*snapshot));
  }
  // A coherent Body-owned snapshot without lineage is a sealed V1-only
  // capability. Production coordinator seams consume it directly; they do
  // not detour through the legacy raw-shape API.
  {
    solidar::Document document;
    auto& body = document.addBody();
    const auto importedShape = std::make_shared<const TopoDS_Shape>(
        BRepPrimAPI_MakeBox(30.0, 20.0, 15.0).Shape());
    auto imported = std::make_unique<solidar::ImportedShapeFeature>(
        importedShape, "No-lineage source");
    auto* importedPtr = imported.get();
    body.addFeature(std::move(imported));
    CHECK(document.recompute());
    const auto snapshot = body.topologySnapshot(importedPtr->id());
    CHECK(snapshot);
    CHECK(!snapshot->v2Authoritative());
    CHECK(snapshot->topologyIndex());
    CHECK(!snapshot->topologyIndex()->lineage());
    const auto edge = usableV1Edge(snapshot->shape(),
                                   snapshot->topologyIndex(), body.id(),
                                   importedPtr->id());
    const auto draft = usableV1DraftReferences(
        document, snapshot->shape(), snapshot->topologyIndex(), body.id(),
        importedPtr->id());

    solidar::PartDesignCoordinator coordinator;
    CHECK(coordinator.beginFillet(*snapshot, {edge}, 0.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    auto state = coordinator.snapshot(solidar::PartDesignToolKind::Fillet);
    CHECK(state.lifecycle == solidar::ToolLifecycle::EditingParameters);
    CHECK(state.linearManipulator);
    coordinator.cancelAll();

    CHECK(coordinator.beginChamfer(*snapshot, {edge}, 0.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    state = coordinator.snapshot(solidar::PartDesignToolKind::Chamfer);
    CHECK(state.lifecycle == solidar::ToolLifecycle::EditingParameters);
    CHECK(state.linearManipulator);
    coordinator.cancelAll();

    CHECK(coordinator
              .beginDraft(document, *snapshot, {draft.face}, draft.plane,
                          draft.direction)
              .effect == solidar::PartDesignTransitionEffect::Activated);
    state = coordinator.snapshot(solidar::PartDesignToolKind::Draft, document);
    CHECK(state.lifecycle == solidar::ToolLifecycle::PreviewValid);
    CHECK(state.angularManipulator);
    coordinator.cancelAll();

    const auto v2Edge = solidar::EdgeReference::fromV2(
        {body.id(), makeKey(importedPtr->id(), solidar::TopologyKind::Edge,
                            "no-lineage:v2-edge")});
    solidar::FilletToolSession closedFillet;
    closedFillet.begin(*snapshot, {v2Edge}, 1.0);
    CHECK(closedFillet.previewBuildAttemptCount() == 0);
    CHECK(closedFillet.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(!closedFillet.manipulator());
    CHECK(coordinator.beginFillet(*snapshot, {v2Edge}, 1.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Fillet)
              .lifecycle == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(!coordinator.startCommit());
    coordinator.cancelAll();

    solidar::ChamferToolSession closedChamfer;
    closedChamfer.begin(*snapshot, {v2Edge}, 1.0);
    CHECK(closedChamfer.previewBuildAttemptCount() == 0);
    CHECK(closedChamfer.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(!closedChamfer.manipulator());
    CHECK(coordinator.beginChamfer(*snapshot, {v2Edge}, 1.0).effect ==
          solidar::PartDesignTransitionEffect::Activated);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Chamfer)
              .lifecycle == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(!coordinator.startCommit());
    coordinator.cancelAll();

    const auto v2Face = solidar::FaceReference::fromV2(
        {body.id(), makeKey(importedPtr->id(), solidar::TopologyKind::Face,
                            "no-lineage:v2-face")});
    solidar::DraftToolSession closedDraft;
    closedDraft.begin(document, *snapshot, {v2Face}, draft.plane,
                      draft.direction);
    CHECK(closedDraft.previewBuildAttemptCount() == 0);
    CHECK(closedDraft.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(!closedDraft.manipulator(document));
    CHECK(coordinator
              .beginDraft(document, *snapshot, {v2Face}, draft.plane,
                          draft.direction)
              .effect == solidar::PartDesignTransitionEffect::Activated);
    CHECK(coordinator.snapshot(solidar::PartDesignToolKind::Draft, document)
              .lifecycle == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(!coordinator.startCommit());
    coordinator.cancelAll();
  }

  // Body is the only capability issuer. Sessions consume that snapshot for
  // V2, resolve complete mixed batches atomically, and never build or expose
  // a manipulator for an invalid batch.
  {
    solidar::Document document;
    const auto model = addSource(document);
    const auto edge = usableEdge(model);
    const auto snapshot = model.body->topologySnapshot(model.source->id());
    CHECK(snapshot);
    CHECK(snapshot->v2Authoritative());
    CHECK(snapshot->shape().get() == model.source->shape().get());
    CHECK(snapshot->topologyIndex().get() == model.topology.get());
    CHECK(model.body->matchesTopologySnapshot(*snapshot));

    auto handedOffSnapshot =
        model.body->topologySnapshot(model.source->id());
    CHECK(handedOffSnapshot);
    solidar::FilletToolSession lifetime;
    lifetime.begin(*handedOffSnapshot, {edge.v2}, 0.0);
    handedOffSnapshot.reset();
    CHECK(lifetime.sourceSnapshot());
    lifetime.cancel();
    CHECK(!lifetime.sourceSnapshot());

    solidar::Document foreignDocument;
    const auto foreignModel = addSource(foreignDocument, 100.0);
    const auto foreignSnapshot =
        foreignModel.body->topologySnapshot(foreignModel.source->id());
    CHECK(foreignSnapshot && foreignSnapshot->v2Authoritative());
    CHECK(!model.body->matchesTopologySnapshot(*foreignSnapshot));
    std::string foreignError;
    CHECK(!foreignModel.body->topologySnapshot(model.source->id(),
                                               &foreignError));
    CHECK(!foreignError.empty());

    solidar::FilletToolSession foreignCapability;
    foreignCapability.begin(*foreignSnapshot, {edge.v2}, 1.0);
    CHECK(foreignCapability.previewBuildAttemptCount() == 0);
    CHECK(foreignCapability.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);
    CHECK(!foreignCapability.manipulator());
    solidar::PartDesignCoordinator foreignCoordinator;
    CHECK(foreignCoordinator.beginFillet(*foreignSnapshot, {edge.v2}, 1.0)
              .effect == solidar::PartDesignTransitionEffect::Activated);
    CHECK(foreignCoordinator.snapshot(solidar::PartDesignToolKind::Fillet)
              .lifecycle == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(!foreignCoordinator.startCommit());
    foreignCoordinator.cancelAll();

    solidar::FilletToolSession fillet;
    fillet.begin(*snapshot, {edge.v1, edge.v2}, 1.0);
    CHECK(fillet.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(fillet.previewShape() && fillet.manipulator());
    const auto attempts = fillet.previewBuildAttemptCount();
    fillet.setEdges({missingReference(edge.v2), edge.v1});
    CHECK(fillet.previewBuildAttemptCount() == attempts);
    CHECK(!fillet.manipulator());
    CHECK(fillet.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    fillet.setEdges({edge.v1, missingReference(edge.v2)});
    CHECK(fillet.previewBuildAttemptCount() == attempts);
    CHECK(!fillet.manipulator());
    CHECK(fillet.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    fillet.setEdges({edge.v2, edge.v1});
    CHECK(fillet.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    fillet.setRadiusFromPanel(1000.0);
    CHECK(fillet.refineRadiusToBoundary(1000.0));
    CHECK(fillet.limitReached());

    for (const auto& batch :
         std::vector<std::vector<solidar::EdgeReference>>{
             {edge.v2, edge.v1, edge.v1},
             {edge.v1, edge.v2, edge.v1},
             {edge.v1, edge.v1, edge.v2}}) {
      solidar::FilletToolSession legacy;
      legacy.beginLegacyV1(model.body->id(), model.source->id(),
                           model.source->shape(), batch, 1.0);
      CHECK(legacy.previewBuildAttemptCount() == 0);
      CHECK(!legacy.manipulator());
      CHECK(legacy.errorCode() ==
            solidar::OperationFailureCode::TopologyIndexUnavailable);
      legacy.setEdges({edge.v1});
      CHECK(legacy.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    }

    auto foreign = *edge.v2.v2();
    foreign.bodyId += 1;
    const auto attemptsBeforeForeign = fillet.previewBuildAttemptCount();
    fillet.setEdges(
        {solidar::EdgeReference::fromV2(std::move(foreign))});
    CHECK(fillet.previewBuildAttemptCount() == attemptsBeforeForeign);
    CHECK(!fillet.manipulator());
    CHECK(fillet.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);
    auto foreignFeature = *edge.v2.v2();
    foreignFeature.provenance.featureId += 1;
    const auto attemptsBeforeForeignFeature =
        fillet.previewBuildAttemptCount();
    fillet.setEdges(
        {solidar::EdgeReference::fromV2(std::move(foreignFeature))});
    CHECK(fillet.previewBuildAttemptCount() ==
          attemptsBeforeForeignFeature);
    CHECK(fillet.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);

    solidar::ChamferToolSession chamfer;
    chamfer.begin(*snapshot, {edge.v2, edge.v1}, 1.0);
    CHECK(chamfer.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(chamfer.previewShape() && chamfer.manipulator());
    const auto chamferAttempts = chamfer.previewBuildAttemptCount();
    chamfer.setEdges({edge.v1, missingReference(edge.v2)});
    CHECK(chamfer.previewBuildAttemptCount() == chamferAttempts);
    CHECK(!chamfer.manipulator());
    CHECK(chamfer.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    chamfer.setEdges({missingReference(edge.v2), edge.v1});
    CHECK(chamfer.previewBuildAttemptCount() == chamferAttempts);
    CHECK(!chamfer.manipulator());
    CHECK(chamfer.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    chamfer.setEdges({edge.v1, edge.v2});
    CHECK(chamfer.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    chamfer.setDistanceFromPanel(1000.0);
    CHECK(chamfer.refineDistanceToBoundary(1000.0));
    CHECK(chamfer.limitReached());

    for (const auto& batch :
         std::vector<std::vector<solidar::EdgeReference>>{
             {edge.v2, edge.v1, edge.v1},
             {edge.v1, edge.v2, edge.v1},
             {edge.v1, edge.v1, edge.v2}}) {
      solidar::ChamferToolSession legacy;
      legacy.beginLegacyV1(model.body->id(), model.source->id(),
                           model.source->shape(), batch, 1.0);
      CHECK(legacy.previewBuildAttemptCount() == 0);
      CHECK(!legacy.manipulator());
      CHECK(legacy.errorCode() ==
            solidar::OperationFailureCode::TopologyIndexUnavailable);
      legacy.setEdges({edge.v1});
      CHECK(legacy.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    }

    auto foreignChamfer = *edge.v2.v2();
    foreignChamfer.bodyId += 1;
    const auto chamferAttemptsBeforeForeign =
        chamfer.previewBuildAttemptCount();
    chamfer.setEdges(
        {solidar::EdgeReference::fromV2(std::move(foreignChamfer))});
    CHECK(chamfer.previewBuildAttemptCount() ==
          chamferAttemptsBeforeForeign);
    CHECK(!chamfer.manipulator());
    CHECK(chamfer.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);
    CHECK(chamfer.sourceSnapshot());
    chamfer.cancel();
    CHECK(!chamfer.sourceSnapshot());
  }

  // Draft's indexed adapter, model feature, session preview and manipulator
  // consume the exact V2 rotation edge. The unindexed overload fails closed.
  {
    solidar::Document document;
    const auto model = addSource(document);
    const auto references = usableDraftReferences(model, document);
    const auto bodyPlaneReferences =
        usableBodyPlaneDraftReferences(model, document);
    gp_Pln plane;
    gp_Dir direction;
    CHECK(solidar::resolveDraftEdgeAxis(
        *model.source->shape(), *model.topology, references.face,
        references.edge, &plane, &direction));
    const auto unindexed = solidar::resolveDraftEdgeAxis(
        *model.source->shape(), references.face, references.edge, &plane,
        &direction);
    CHECK(!unindexed);
    CHECK(unindexed.failure.code ==
          solidar::OperationFailureCode::TopologyReferenceInvalid);

    const auto snapshot = model.body->topologySnapshot(model.source->id());
    CHECK(snapshot);
    solidar::DraftToolSession session;
    session.begin(document, *snapshot, {references.face}, std::nullopt,
                  std::nullopt, 5.0, false, std::nullopt, references.edge);
    CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(session.previewShape() && session.manipulator(document));
    CHECK(session.setRotationEdge(document, references.edge));

    solidar::DraftToolSession absent;
    absent.beginLegacyV1(document, model.body->id(), model.source->id(),
                         model.source->shape(), {references.face},
                         std::nullopt, std::nullopt, 5.0, false,
                         std::nullopt, references.edge);
    CHECK(absent.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(absent.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(absent.setPrincipalAxis(document, 2));
    CHECK(absent.lifecycle() == solidar::ToolLifecycle::PreviewValid);
    CHECK(absent.previewShape());

    // Every Draft V2 slot is accepted only through the source-owned
    // capability. The legacy path fails closed before any geometry build.
    solidar::DraftToolSession v2Face;
    v2Face.begin(document, *snapshot, {references.faceV2}, std::nullopt,
                 std::nullopt, 5.0, false, std::nullopt, references.edge);
    CHECK(v2Face.lifecycle() == solidar::ToolLifecycle::PreviewValid);

    solidar::DraftToolSession orderedMissingFaces;
    const auto missingFace = missingReference(references.faceV2);
    orderedMissingFaces.begin(
        document, *snapshot, {missingFace, references.face},
        solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
        solidar::AxisReference{solidar::AxisReferenceType::GlobalZ});
    CHECK(orderedMissingFaces.previewBuildAttemptCount() == 0);
    CHECK(orderedMissingFaces.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    CHECK(!orderedMissingFaces.manipulator(document));
    orderedMissingFaces.setFaces(document, {references.face});
    CHECK(orderedMissingFaces.lifecycle() ==
          solidar::ToolLifecycle::PreviewValid);
    const auto attemptsAfterRecovery =
        orderedMissingFaces.previewBuildAttemptCount();
    orderedMissingFaces.setFaces(document,
                                  {references.face, missingFace});
    CHECK(orderedMissingFaces.previewBuildAttemptCount() ==
          attemptsAfterRecovery);
    CHECK(orderedMissingFaces.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);
    CHECK(!orderedMissingFaces.manipulator(document));
    orderedMissingFaces.setFaces(document, {references.face});
    CHECK(orderedMissingFaces.lifecycle() ==
          solidar::ToolLifecycle::PreviewValid);

    solidar::DraftToolSession legacyFace;
    legacyFace.beginLegacyV1(
        document, model.body->id(), model.source->id(), model.source->shape(),
        {references.faceV2},
        solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
        solidar::AxisReference{solidar::AxisReferenceType::GlobalZ});
    CHECK(legacyFace.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(legacyFace.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(!legacyFace.manipulator(document));

    solidar::DraftToolSession trustedNeutral;
    trustedNeutral.begin(document, *snapshot,
                         {bodyPlaneReferences.draftedFace},
                         bodyPlaneReferences.neutralPlane,
                         bodyPlaneReferences.pullDirection);
    CHECK(trustedNeutral.lifecycle() ==
          solidar::ToolLifecycle::PreviewValid);
    CHECK(trustedNeutral.manipulator(document));

    solidar::DraftToolSession legacyNeutral;
    legacyNeutral.beginLegacyV1(
        document, model.body->id(), model.source->id(), model.source->shape(),
        {bodyPlaneReferences.draftedFace}, bodyPlaneReferences.neutralPlane,
        bodyPlaneReferences.pullDirection);
    CHECK(legacyNeutral.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(legacyNeutral.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);

    solidar::DraftToolSession legacyEdge;
    legacyEdge.beginLegacyV1(
        document, model.body->id(), model.source->id(), model.source->shape(),
        {references.face}, std::nullopt, std::nullopt, 5.0, false,
        std::nullopt, references.edge);
    CHECK(legacyEdge.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
    CHECK(legacyEdge.errorCode() ==
          solidar::OperationFailureCode::TopologyIndexUnavailable);
    CHECK(legacyEdge.setPrincipalAxis(document, 2));
    CHECK(legacyEdge.lifecycle() == solidar::ToolLifecycle::PreviewValid);

    auto feature = std::make_unique<solidar::DraftFeature>(
        model.source->id(), std::vector{references.face},
        solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
        solidar::AxisReference{solidar::AxisReferenceType::GlobalZ}, 5.0,
        false, "V2 edge draft", references.edge);
    auto* featurePtr = feature.get();
    model.body->addFeature(std::move(feature));
    CHECK(document.recompute());
    CHECK(featurePtr->isValid() && featurePtr->shape());

    const auto lastValid = featurePtr->shape();
    featurePtr->setRotationEdge(missingReference(references.edge));
    CHECK(!document.recompute());
    CHECK(!featurePtr->shape());
    CHECK(featurePtr->lastValidShape() == lastValid);
    CHECK(featurePtr->error().find(
              "Draft rotation edge could not be resolved: ") == 0);
    featurePtr->setRotationEdge(references.edge);
    CHECK(document.recompute());
  }

  // Sessions retain the source-owned capability while active and release all
  // shape/index ownership and owner/edit ids on cancel. Coordinator cancelAll
  // provides the same teardown guarantee.
  {
    solidar::FilletToolSession session;
    std::weak_ptr<const TopoDS_Shape> shape;
    {
      solidar::Document document;
      const auto model = addSource(document);
      const auto edge = usableEdge(model);
      const auto snapshot = model.body->topologySnapshot(model.source->id());
      CHECK(snapshot);
      shape = snapshot->shape();
      session.begin(*snapshot, {edge.v2}, 0.0, model.source->id() + 100);
    }
    CHECK(!shape.expired());
    session.cancel();
    CHECK(shape.expired());
    CHECK(session.bodyId() == solidar::kInvalidBodyId);
    CHECK(session.sourceFeatureId() == solidar::kInvalidFeatureId);
    CHECK(!session.editingFeatureId());
  }
  {
    solidar::ChamferToolSession session;
    std::weak_ptr<const TopoDS_Shape> shape;
    {
      solidar::Document document;
      const auto model = addSource(document);
      const auto edge = usableEdge(model);
      const auto snapshot = model.body->topologySnapshot(model.source->id());
      CHECK(snapshot);
      shape = snapshot->shape();
      session.begin(*snapshot, {edge.v2}, 0.0, model.source->id() + 100);
    }
    CHECK(!shape.expired());
    session.cancel();
    CHECK(shape.expired());
    CHECK(session.bodyId() == solidar::kInvalidBodyId);
    CHECK(session.sourceFeatureId() == solidar::kInvalidFeatureId);
    CHECK(!session.editingFeatureId());
  }
  {
    solidar::DraftToolSession session;
    std::weak_ptr<const TopoDS_Shape> shape;
    {
      solidar::Document document;
      const auto model = addSource(document);
      const auto references = usableDraftReferences(model, document);
      const auto snapshot = model.body->topologySnapshot(model.source->id());
      CHECK(snapshot);
      shape = snapshot->shape();
      session.begin(document, *snapshot, {references.face}, std::nullopt,
                    std::nullopt, 5.0, false, model.source->id() + 100,
                    references.edge);
    }
    CHECK(!shape.expired());
    session.cancel();
    CHECK(shape.expired());
    CHECK(session.bodyId() == solidar::kInvalidBodyId);
    CHECK(session.sourceFeatureId() == solidar::kInvalidFeatureId);
    CHECK(!session.editingFeatureId());
  }
  {
    solidar::PartDesignCoordinator coordinator;
    std::weak_ptr<const TopoDS_Shape> shape;
    {
      solidar::Document document;
      const auto model = addSource(document);
      const auto edge = usableEdge(model);
      const auto snapshot = model.body->topologySnapshot(model.source->id());
      CHECK(snapshot);
      shape = snapshot->shape();
      CHECK(coordinator.beginFillet(*snapshot, {edge.v2}, 0.0).effect ==
            solidar::PartDesignTransitionEffect::Activated);
    }
    CHECK(!shape.expired());
    coordinator.cancelAll();
    CHECK(shape.expired());
  }

  return 0;
}
