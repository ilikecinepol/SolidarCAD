#ifdef NDEBUG
#undef NDEBUG
#endif

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <cassert>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

#include "model/Document.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletFeature.h"

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
}
