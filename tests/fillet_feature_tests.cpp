#ifdef NDEBUG
#undef NDEBUG
#endif

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"
#include "model/FilletToolSession.h"
#include "model/TopologyReferenceResolver.h"

#define CHECK(condition)                                                   \
  do {                                                                     \
    if (!(condition)) {                                                    \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
      return EXIT_FAILURE;                                                 \
    }                                                                      \
  } while (false)

namespace {

double volumeOf(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}

std::size_t countFaces(const TopoDS_Shape& shape) {
  std::size_t count = 0;
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next())
    ++count;
  return count;
}

bool hasCurvedFace(const TopoDS_Shape& shape) {
  for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
       explorer.Next()) {
    const BRepAdaptor_Surface surface(TopoDS::Face(explorer.Current()));
    if (surface.GetType() != GeomAbs_Plane) return true;
  }
  return false;
}

struct BoxModel {
  solidar::BodyId bodyId{};
  solidar::SketchId sketchId{};
  solidar::FeatureId extrudeId{};
};

BoxModel addBox(solidar::Document& document, double x = 80.0,
                double y = 35.0, double z = 50.0) {
  auto& profile = document.addSketch("Box profile");
  profile.geometry.addRectangle({0.0, 0.0}, {x, y});
  const auto sketchId = profile.id;
  auto& body = document.addBody();
  auto& feature = body.addFeature(
      std::make_unique<solidar::ExtrudeFeature>(sketchId, z, "Extrude"));
  return {body.id(), sketchId, feature.id()};
}

solidar::FilletFeature* addFillet(solidar::Document& document,
                                  const BoxModel& box, double radius,
                                  std::size_t edgeIndex = 0) {
  auto* body = document.findBody(box.bodyId);
  assert(body);
  auto& feature = body->addFeature(std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{box.bodyId, box.extrudeId, edgeIndex}, radius,
      "Fillet"));
  return dynamic_cast<solidar::FilletFeature*>(&feature);
}

}  // namespace

int main() {
  // Real OCCT fillet on one selected edge changes volume and creates a curved
  // surface while remaining a single solid.
  solidar::Document document;
  const BoxModel box = addBox(document);
  assert(document.rebuild());
  const double baseVolume = volumeOf(*document.findBody(box.bodyId)->resultShape());
  const std::size_t baseFaces = countFaces(*document.findBody(box.bodyId)->resultShape());
  auto* fillet = addFillet(document, box, 5.0);
  assert(fillet);
  assert(fillet->edge().bodyId == box.bodyId);
  assert(fillet->edge().featureId == box.extrudeId);
  const auto topologyReference = fillet->edge().topology();
  assert(topologyReference.kind == solidar::TopologyKind::Edge);
  assert(topologyReference.hasValidOwner());
  assert(topologyReference.legacyIndex == fillet->edge().edgeIndex);
  const auto sourceShape =
      document.findBody(box.bodyId)->features().front()->shape();
  assert(sourceShape);
  // Viewport picking reports the raw TopExp traversal index. Every visible
  // occurrence, including an alias of an already visited OCCT edge, must be
  // canonicalized into a resolvable persistent reference.
  std::size_t rawEdgeIndex = 0;
  for (TopExp_Explorer explorer(*sourceShape, TopAbs_EDGE); explorer.More();
       explorer.Next(), ++rawEdgeIndex) {
    const auto picked = solidar::makeEdgeReference(
        *sourceShape, box.bodyId, box.extrudeId, rawEdgeIndex);
    assert(picked.signature);
    assert(solidar::resolveEdgeReference(*sourceShape, picked.topology()));
  }
  assert(rawEdgeIndex > 0);
  assert(solidar::resolveEdge(
      *sourceShape, topologyReference));
  auto wrongKind = topologyReference;
  wrongKind.kind = solidar::TopologyKind::Face;
  assert(!solidar::resolveEdge(
      *sourceShape, wrongKind));
  assert(document.rebuild());
  assert(fillet->isValid());
  assert(fillet->hasShape());
  assert(fillet->shape()->ShapeType() == TopAbs_SOLID);
  assert(volumeOf(*fillet->shape()) < baseVolume - 1e-7);
  assert(countFaces(*fillet->shape()) != baseFaces);
  assert(hasCurvedFace(*fillet->shape()));

  // Radius edits dirty and rebuild the feature. Snapshot ownership remains
  // independent and preserves FeatureId, radius, and the earlier B-Rep.
  fillet->setRadiusMm(2.0);
  assert(fillet->isDirty());
  assert(document.rebuild());
  const double radius2Volume = volumeOf(*fillet->shape());
  const solidar::Document snapshot = document;
  const auto filletId = fillet->id();
  fillet->setRadiusMm(5.0);
  assert(document.rebuild());
  assert(fillet->isValid());
  assert(std::abs(volumeOf(*fillet->shape()) - radius2Volume) > 1e-7);
  const auto* snapshotFillet = dynamic_cast<const solidar::FilletFeature*>(
      snapshot.findBody(box.bodyId)->activeFeature());
  assert(snapshotFillet);
  assert(snapshotFillet->id() == filletId);
  assert(snapshotFillet->radiusMm() == 2.0);
  assert(std::abs(volumeOf(*snapshotFillet->shape()) - radius2Volume) < 1e-7);

  // Upstream edits propagate Dirty state through Body::rebuild and rebuild the
  // fillet from the new previousShape.
  auto* body = document.findBody(box.bodyId);
  auto* extrude = dynamic_cast<solidar::ExtrudeFeature*>(body->features()[0].get());
  assert(extrude);
  extrude->setLengthMm(80.0);
  body->markDirtyFrom(0);
  assert(document.rebuild());
  assert(fillet->isValid() && fillet->hasShape());

  // Invalid references and radii fail without retaining a stale shape.
  solidar::Document invalidEdgeDocument;
  const BoxModel invalidBox = addBox(invalidEdgeDocument);
  assert(invalidEdgeDocument.rebuild());
  auto* invalidEdge = addFillet(invalidEdgeDocument, invalidBox, 5.0, 999999);
  assert(!invalidEdgeDocument.rebuild());
  assert(invalidEdge->state() == solidar::FeatureState::Error);
  assert(invalidEdge->error() == "Fillet edge could not be resolved");
  assert(!invalidEdge->hasShape());

  for (const double invalidRadius :
       {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    solidar::Document invalidRadiusDocument;
    const BoxModel invalidRadiusBox = addBox(invalidRadiusDocument);
    assert(invalidRadiusDocument.rebuild());
    auto* invalid =
        addFillet(invalidRadiusDocument, invalidRadiusBox, invalidRadius);
    assert(!invalidRadiusDocument.rebuild());
    assert(invalid->state() == solidar::FeatureState::Error);
    assert(!invalid->hasShape());
  }

  solidar::Document oversizedDocument;
  const BoxModel oversizedBox = addBox(oversizedDocument);
  assert(oversizedDocument.rebuild());
  auto* oversized = addFillet(oversizedDocument, oversizedBox, 1000.0);
  assert(!oversizedDocument.rebuild());
  assert(oversized->error() ==
         "Fillet could not be built with the requested radius");
  assert(!oversized->hasShape());

  // Repeated valid/invalid radius edits remain recoverable and never expose a
  // stale result after OCCT rejects a radius.
  solidar::Document stressDocument;
  const BoxModel stressBox = addBox(stressDocument, 40.0, 30.0, 20.0);
  assert(stressDocument.rebuild());
  auto* stressFillet = addFillet(stressDocument, stressBox, 1.0);
  for (const double radius : {1.0, 2.0, 4.0}) {
    stressFillet->setRadiusMm(radius);
    assert(stressDocument.rebuild());
    assert(stressFillet->isValid() && stressFillet->hasShape());
  }
  stressFillet->setRadiusMm(1000.0);
  assert(!stressDocument.rebuild());
  assert(stressFillet->isFailed());
  assert(!stressFillet->hasShape());
  stressFillet->setRadiusMm(2.0);
  assert(stressDocument.rebuild());
  assert(stressFillet->isValid() && stressFillet->hasShape());

  solidar::Document missingBaseDocument;
  auto& emptyBody = missingBaseDocument.addBody();
  auto& missingBase = emptyBody.addFeature(
      std::make_unique<solidar::FilletFeature>(
          solidar::EdgeReference{emptyBody.id(), 123, 0}, 5.0));
  assert(!missingBaseDocument.rebuild());
  assert(missingBase.error() == "Fillet base shape is missing");
  assert(!missingBase.hasShape());

  // Exact BodyId binding keeps a second Body unchanged.
  solidar::Document multiBodyDocument;
  const BoxModel first = addBox(multiBodyDocument);
  const BoxModel second = addBox(multiBodyDocument, 30.0, 30.0, 30.0);
  assert(multiBodyDocument.rebuild());
  const double firstBefore =
      volumeOf(*multiBodyDocument.findBody(first.bodyId)->resultShape());
  const double secondBefore =
      volumeOf(*multiBodyDocument.findBody(second.bodyId)->resultShape());
  addFillet(multiBodyDocument, first, 3.0);
  assert(multiBodyDocument.rebuild());
  assert(multiBodyDocument.bodies().size() == 2);
  assert(volumeOf(*multiBodyDocument.findBody(first.bodyId)->resultShape()) <
         firstBefore - 1e-7);
  assert(std::abs(volumeOf(
                      *multiBodyDocument.findBody(second.bodyId)->resultShape()) -
                  secondBefore) < 1e-7);

  // One feature resolves and fillets multiple source edges.
  solidar::Document multiEdgeDocument;
  const BoxModel multiEdgeBox = addBox(multiEdgeDocument);
  assert(multiEdgeDocument.rebuild());
  auto* multiEdgeBody = multiEdgeDocument.findBody(multiEdgeBox.bodyId);
  const auto multiEdgeSource = multiEdgeBody->resultShape();
  CHECK(multiEdgeSource);
  std::vector<solidar::EdgeReference> verticalEdges;
  for (std::size_t index = 0; index < 32 && verticalEdges.size() < 2; ++index) {
    const auto edge = solidar::resolveEdge(*multiEdgeBody->resultShape(), index);
    if (!edge) break;
    BRepAdaptor_Curve curve(*edge);
    const gp_Pnt start = curve.Value(curve.FirstParameter());
    const gp_Pnt end = curve.Value(curve.LastParameter());
    if (std::abs(std::abs(end.Z() - start.Z()) - 50.0) < 1e-6)
      verticalEdges.push_back(solidar::makeEdgeReference(
          *multiEdgeSource, multiEdgeBody->id(), multiEdgeBox.extrudeId,
          index));
  }
  CHECK(verticalEdges.size() == 2);
  CHECK(verticalEdges[0].signature && verticalEdges[1].signature);

  solidar::FilletToolSession multiEdgeSession;
  multiEdgeSession.begin(multiEdgeBody->id(), multiEdgeBox.extrudeId,
                         multiEdgeSource, verticalEdges, 2.0);
  CHECK(multiEdgeSession.lifecycle() ==
        solidar::ToolLifecycle::PreviewValid);
  const auto selectedReferences = multiEdgeSession.edges();
  multiEdgeSession.setRadiusFromPanel(3.0);
  CHECK(multiEdgeSession.lifecycle() ==
        solidar::ToolLifecycle::PreviewValid);
  CHECK(multiEdgeSession.edges() == selectedReferences);
  multiEdgeSession.setRadiusFromManipulator(1.0);
  CHECK(multiEdgeSession.lifecycle() ==
        solidar::ToolLifecycle::PreviewValid);
  CHECK(multiEdgeSession.edges() == selectedReferences);
  multiEdgeSession.setRadiusFromPanel(1000.0);
  CHECK(multiEdgeSession.lifecycle() ==
        solidar::ToolLifecycle::PreviewInvalid);
  CHECK(!multiEdgeSession.previewShape());
  CHECK(multiEdgeSession.edges() == selectedReferences);
  multiEdgeSession.setRadiusFromPanel(2.0);
  CHECK(multiEdgeSession.lifecycle() ==
        solidar::ToolLifecycle::PreviewValid);
  CHECK(multiEdgeSession.edges() == selectedReferences);
  const double multiEdgeBefore = volumeOf(*multiEdgeBody->resultShape());
  auto feature =
      std::make_unique<solidar::FilletFeature>(verticalEdges, 3.0, "Two edges");
  auto* multiFillet = feature.get();
  multiEdgeBody->addFeature(std::move(feature));
  assert(multiEdgeDocument.rebuild());
  assert(multiFillet->isValid() && multiFillet->edges().size() == 2);
  assert(std::abs(volumeOf(*multiEdgeBody->resultShape()) - multiEdgeBefore) >
         1e-3);

  solidar::FilletToolSession editSession;
  editSession.begin(multiEdgeBody->id(), multiEdgeBox.extrudeId,
                    multiEdgeSource, multiFillet->edges(),
                    multiFillet->radiusMm(), multiFillet->id());
  CHECK(editSession.editingFeatureId() == multiFillet->id());
  CHECK(editSession.edges() == selectedReferences);

  // Preview geometry and synchronized parameter updates do not mutate the
  // Document or add history until the UI accepts the session.
  solidar::Document previewDocument;
  const BoxModel previewBox = addBox(previewDocument, 40.0, 30.0, 20.0);
  assert(previewDocument.rebuild());
  auto* previewBody = previewDocument.findBody(previewBox.bodyId);
  const auto originalShape = previewBody->resultShape();
  const double originalVolume = volumeOf(*originalShape);
  const std::size_t originalFeatureCount = previewBody->features().size();
  solidar::FilletToolSession session;
  session.begin(previewBody->id(), previewBox.extrudeId, originalShape, {}, 2.0);
  assert(session.lifecycle() == solidar::ToolLifecycle::SelectingInput);
  assert(session.selectionStage() ==
         solidar::ToolSelectionStage::SelectingInput);
  assert(!session.previewShape() && session.error().empty());
  session.setEdges({{previewBody->id(), previewBox.extrudeId, 0}});
  assert(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  session.begin(previewBody->id(), previewBox.extrudeId, originalShape,
                {{previewBody->id(), previewBox.extrudeId, 0}}, 2.0);
  assert(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  session.setRadiusFromPanel(3.0);
  assert(session.radiusMm() == 3.0);
  session.setRadiusFromManipulator(4.0);
  assert(session.radiusMm() == 4.0);
  assert(previewBody->features().size() == originalFeatureCount);
  assert(previewBody->resultShape() == originalShape);
  assert(std::abs(volumeOf(*previewBody->resultShape()) - originalVolume) < 1e-7);
  session.cancel();
  assert(session.lifecycle() == solidar::ToolLifecycle::Inactive);
  assert(previewBody->features().size() == originalFeatureCount);
}
