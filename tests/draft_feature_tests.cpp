#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomAbs_CurveType.hxx>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include "TestAssertions.h"
#include <cmath>
#include <memory>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "model/Document.h"
#include "model/DraftBuilder.h"
#include "model/DraftFeature.h"
#include "model/DraftToolSession.h"
#include "model/ExtrudeFeature.h"
#include "model/TopologyReferenceResolver.h"
#include "project/ProjectFile.h"

namespace {
double volume(const TopoDS_Shape& shape) {
  GProp_GProps properties;
  BRepGProp::VolumeProperties(shape, properties);
  return properties.Mass();
}
}

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  solidar::Document document;
  auto& sketch = document.addSketch();
  sketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
  const auto profileSketchId = sketch.id;
  auto& staleAxisSketch = document.addSketch("Stale global-axis owner");
  staleAxisSketch.geometry.addLine({0.0, 0.0}, {5.0, 0.0});
  const auto staleAxisSketchId = staleAxisSketch.id;
  auto& body = document.addBody();
  auto& source = body.addFeature(std::make_unique<solidar::ExtrudeFeature>(
      profileSketchId, 20.0, "Box", solidar::ExtrudeOperation::NewBody));
  const auto bodyId = body.id();
  const auto sourceId = source.id();
  CHECK(document.recompute());

  // Choose a vertical planar face accepted by OCCT for the XY neutral plane.
  std::size_t chosen = static_cast<std::size_t>(-1);
  std::size_t index = 0;
  for (TopExp_Explorer faces(*source.shape(), TopAbs_FACE); faces.More();
       faces.Next(), ++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if (surface.GetType() != GeomAbs_Plane ||
        std::abs(surface.Plane().Axis().Direction().Z()) > 0.1)
      continue;
    std::string ignored;
    if (solidar::buildDraftShape(*source.shape(), {index},
          gp_Pln(gp_Pnt(0,0,0), gp_Dir(0,0,1)), gp_Dir(0,0,1),
          5.0, false, &ignored)) { chosen = index; break; }
  }
  CHECK(chosen != static_cast<std::size_t>(-1));
  const auto face = solidar::makeFaceReference(*source.shape(), body.id(),
                                                source.id(), chosen);
  solidar::PlaneReference plane{solidar::NeutralPlaneType::GlobalXY};
  solidar::AxisReference direction{solidar::AxisReferenceType::GlobalZ,
                                    staleAxisSketchId,
                                    solidar::sketch::kInvalidGeometryId};
  auto draft = std::make_unique<solidar::DraftFeature>(
      source.id(), std::vector{face}, plane, direction, 5.0, false, "Draft 1");
  auto* draftPtr = draft.get(); const auto draftId = draftPtr->id();
  body.addFeature(std::move(draft));
  CHECK(document.recompute());
  const double firstVolume = volume(*draftPtr->shape());
  CHECK(std::abs(firstVolume - volume(*source.shape())) > 0.01);

  // Global datum axes ignore stale sketch payload. An unrelated Sketch edit
  // or removal must not dirty/cascade-delete the Draft.
  const auto globalAxisRevision = draftPtr->shapeRevision();
  solidar::sketch::Sketch changedStaleAxis;
  changedStaleAxis.addLine({0.0, 0.0}, {25.0, 0.0});
  CHECK(document.replaceSketchGeometry(staleAxisSketchId, changedStaleAxis));
  CHECK(!draftPtr->isDirty());
  CHECK(document.recompute());
  CHECK(draftPtr->shapeRevision() == globalAxisRevision);
  const auto staleAxisRemoval =
      document.planSketchRemoval(staleAxisSketchId);
  CHECK(staleAxisRemoval.applicable);
  CHECK(staleAxisRemoval.featureIds.empty());
  std::string removalError;
  CHECK(document.applyRemovalPlan(staleAxisRemoval, &removalError));
  CHECK(removalError.empty());
  draftPtr = dynamic_cast<solidar::DraftFeature*>(document.findFeature(draftId));
  CHECK(draftPtr);
  auto* retainedBody = document.findBody(bodyId);
  auto* retainedSource = document.findFeature(sourceId);
  CHECK(retainedBody && retainedSource);
  CHECK(draftPtr->shapeRevision() == globalAxisRevision);

  draftPtr->setAngleDeg(8.0);
  CHECK(document.recompute());
  CHECK(std::abs(volume(*draftPtr->shape()) - firstVolume) > 0.01);
  draftPtr->setReversed(true);
  CHECK(document.recompute());
  draftPtr->setAngleDeg(0.0);
  CHECK(document.recompute());
  CHECK(!draftPtr->isFailed());
  CHECK(std::abs(volume(*draftPtr->shape()) - volume(*retainedSource->shape())) <
        0.01);
  draftPtr->setAngleDeg(5.0);
  CHECK(document.recompute() && draftPtr->id() == draftId);

  solidar::DraftToolSession session;
  session.begin(document, bodyId, sourceId, retainedSource->shape(), {face},
                plane, direction, 5.0, false, draftId,
                std::nullopt, retainedSource->topologyIndex());
  CHECK(session.previewShape() && session.manipulator(document));

  // Draft keeps topology resolution categories intact end-to-end. Missing and
  // ambiguous references request reselection, while unsupported geometry is a
  // separate deterministic input/geometry failure.
  {
    auto missingFace = face;
    missingFace.signature->centroid.x += 100000.0;
    missingFace.signature->bounds.minimum.x += 100000.0;
    missingFace.signature->bounds.maximum.x += 100000.0;
    solidar::PlaneReference missingPlane{
        solidar::NeutralPlaneType::BodyFace, missingFace};
    solidar::DraftToolSession missingSession;
    missingSession.begin(document, bodyId, sourceId,
                         retainedSource->shape(), {face}, missingPlane,
                         direction, 5.0, false, std::nullopt, std::nullopt,
                         retainedSource->topologyIndex());
    CHECK(missingSession.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMissing);

    auto invalidFace = face;
    invalidFace.signature->centroid.x =
        std::numeric_limits<double>::quiet_NaN();
    solidar::PlaneReference invalidPlane{
        solidar::NeutralPlaneType::BodyFace, invalidFace};
    solidar::DraftToolSession invalidSession;
    invalidSession.begin(document, bodyId, sourceId,
                         retainedSource->shape(), {face}, invalidPlane,
                         direction, 5.0, false, std::nullopt, std::nullopt,
                         retainedSource->topologyIndex());
    CHECK(invalidSession.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceInvalid);

    const TopoDS_Shape firstBox = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf translation;
    translation.SetTranslation(gp_Vec(20.0, 0.0, 0.0));
    const TopoDS_Shape secondBox =
        BRepBuilderAPI_Transform(firstBox, translation, true).Shape();
    BRepAlgoAPI_Fuse fuse(firstBox, secondBox);
    fuse.Build();
    CHECK(fuse.IsDone() && !fuse.Shape().IsNull());
    const auto compound = std::make_shared<TopoDS_Shape>(fuse.Shape());
    const auto compoundIndex = solidar::TopologyIndex::build(compound);
    CHECK(compoundIndex);
    const auto drafted = solidar::makeFaceReference(
        *compound, bodyId, sourceId, 0);
    CHECK(drafted.signature);
    solidar::FaceReference ambiguousFace{bodyId, sourceId, 0};
    ambiguousFace.persistentTag = "planar:max-z";
    solidar::PlaneReference ambiguousPlane{
        solidar::NeutralPlaneType::BodyFace, ambiguousFace};
    solidar::DraftToolSession ambiguousSession;
    ambiguousSession.begin(document, bodyId, sourceId, compound, {drafted},
                           ambiguousPlane, direction, 5.0, false,
                           std::nullopt, std::nullopt, compoundIndex);
    CHECK(ambiguousSession.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceAmbiguous);

    const auto cylinder = std::make_shared<TopoDS_Shape>(
        BRepPrimAPI_MakeCylinder(10.0, 20.0).Shape());
    const auto cylinderIndex = solidar::TopologyIndex::build(cylinder);
    CHECK(cylinderIndex);
    std::optional<solidar::FaceReference> curved;
    for (std::size_t faceIndex = 0; faceIndex < 3; ++faceIndex) {
      auto candidate = solidar::makeFaceReference(
          *cylinder, bodyId, sourceId, faceIndex);
      const auto resolved = cylinderIndex->resolveFace(candidate.topology());
      if (!resolved) continue;
      BRepAdaptor_Surface surface(*resolved.subshape);
      if (surface.GetType() != GeomAbs_Plane) {
        curved = std::move(candidate);
        break;
      }
    }
    CHECK(curved);
    solidar::PlaneReference curvedPlane{
        solidar::NeutralPlaneType::BodyFace, *curved};
    solidar::DraftToolSession curvedSession;
    curvedSession.begin(document, bodyId, sourceId, cylinder, {*curved},
                        curvedPlane, direction, 5.0, false, std::nullopt,
                        std::nullopt, cylinderIndex);
    CHECK(curvedSession.errorCode() ==
          solidar::OperationFailureCode::UnsupportedGeometry);
  }

  session.clearPrincipalAxis(document);
  CHECK(session.selectionRequirement()->type == solidar::SelectionType::Axis);
  CHECK(session.faces().size() == 1 && session.angleDeg() == 5.0);
  CHECK(session.setPrincipalAxis(document, 2));
  CHECK(session.principalAxisIndex() == 2);
  CHECK(session.neutralPlane()->type == solidar::NeutralPlaneType::GlobalXY);
  CHECK(session.pullDirection()->type ==
         solidar::AxisReferenceType::GlobalZ);
  CHECK(session.previewShape() && session.editingFeatureId() == draftId);
  CHECK(session.parameters().size() == 1);
  CHECK(session.parameters().front().minimum == -89.99);
  CHECK(session.parameters().front().maximum == 89.99);
  session.setAngleFromManipulator(document, -5.0);
  CHECK(session.angleDeg() == -5.0);
  CHECK(session.previewShape() && session.manipulator(document));
  CHECK(session.manipulator(document)->angleDeg == -5.0);
  CHECK(session.manipulator(document)->minimumDeg == -89.99);
  CHECK(session.manipulator(document)->maximumDeg == 89.99);
  session.setAngleFromManipulator(document, 0.0);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  session.setAngleFromManipulator(document, 5.0);

  solidar::DraftToolSession legacyReversedSession;
  legacyReversedSession.begin(document, bodyId, sourceId,
                              retainedSource->shape(), {face}, plane, direction,
                              5.0, true, solidar::kInvalidFeatureId,
                              std::nullopt, retainedSource->topologyIndex());
  CHECK(legacyReversedSession.angleDeg() == -5.0);
  CHECK(legacyReversedSession.previewShape());

  // Parameter-domain failure keeps the last valid visual preview, disables
  // commit through PreviewInvalid and recovers in the same session when the
  // angle returns to a buildable value.
  bool foundInvalidAngle = false;
  double invalidAngle = 0.0;
  for (double angle = 10.0; angle <= 88.0; angle += 2.0) {
    const auto lastValidPreview = session.previewShape();
    session.setAngleFromPanel(document, angle);
    if (session.lifecycle() != solidar::ToolLifecycle::PreviewInvalid) continue;
    foundInvalidAngle = true;
    invalidAngle = angle;
    CHECK(!session.error().empty());
    CHECK(session.previewShape() == lastValidPreview);
    break;
  }
  CHECK(foundInvalidAngle);
  // Last-valid geometry belongs only to the same faces/reference/reversed
  // context. Changing a structural input at an invalid angle must not surface
  // a preview built for the previous context.
  session.clearNeutralPlane(document);
  CHECK(!session.previewShape());
  session.setNeutralPlane(document, plane);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewInvalid);
  CHECK(session.angleDeg() == invalidAngle);
  CHECK(!session.previewShape());
  session.setAngleFromPanel(document, 5.0);
  CHECK(session.lifecycle() == solidar::ToolLifecycle::PreviewValid);
  CHECK(session.previewShape());
  CHECK(session.editingFeatureId() == draftId);

  // A straight edge of the selected face is a first-class rotation axis. Its
  // persistent EdgeReference must drive both the live preview and the saved
  // feature, rather than relying on an unstable OCCT edge ordinal.
  std::optional<solidar::EdgeReference> adjacentEdge;
  const auto resolvedChosenFace =
      solidar::resolveFaceReference(*retainedSource->shape(), face.topology());
  CHECK(resolvedChosenFace);
  std::size_t edgeIndex = 0;
  for (TopExp_Explorer edges(*retainedSource->shape(), TopAbs_EDGE); edges.More();
       edges.Next(), ++edgeIndex) {
    BRepAdaptor_Curve curve(TopoDS::Edge(edges.Current()));
    if (curve.GetType() != GeomAbs_Line) continue;
    bool belongsToFace = false;
    for (TopExp_Explorer faceEdges(*resolvedChosenFace.subshape, TopAbs_EDGE);
         faceEdges.More(); faceEdges.Next()) {
      if (TopoDS::Edge(faceEdges.Current()).IsSame(
              TopoDS::Edge(edges.Current()))) {
        belongsToFace = true;
        break;
      }
    }
    if (!belongsToFace) continue;
    auto candidate = solidar::makeEdgeReference(
        *retainedSource->shape(), bodyId, sourceId, edgeIndex);
    gp_Pln candidatePlane;
    gp_Dir candidateDirection;
    std::string candidateError;
    if (!solidar::resolveDraftEdgeAxis(*retainedSource->shape(), face,
                                       candidate, &candidatePlane,
                                       &candidateDirection))
      continue;
    if (solidar::buildDraftShape(*retainedSource->shape(), {chosen}, candidatePlane,
                                 candidateDirection, 5.0, false,
                                 &candidateError)) {
      adjacentEdge = std::move(candidate);
      break;
    }
  }
  CHECK(adjacentEdge && adjacentEdge->signature);
  CHECK(session.setRotationEdge(document, *adjacentEdge));
  CHECK(session.rotationEdge() == adjacentEdge);
  CHECK(!session.principalAxisIndex());
  CHECK(session.previewShape() && session.manipulator(document));

  draftPtr->setReversed(false);
  draftPtr->setRotationEdge(adjacentEdge);
  CHECK(document.recompute());
  CHECK(draftPtr->shape() && !draftPtr->shape()->IsNull());
  auto* sourceExtrude = dynamic_cast<solidar::ExtrudeFeature*>(retainedSource);
  CHECK(sourceExtrude);
  sourceExtrude->setLengthMm(22.0);
  CHECK(document.recompute());
  CHECK(draftPtr->shape() && !draftPtr->shape()->IsNull());

  QTemporaryDir temporary(QDir::current().filePath(
      QStringLiteral("draft-feature-tests-XXXXXX")));
  CHECK(temporary.isValid());
  const QString path = temporary.filePath("draft.solidar"); QString error;
  CHECK(solidar::project::ProjectFile::saveDocument(path, document, &error));
  solidar::Document loaded;
  CHECK(solidar::project::ProjectFile::loadDocument(path, &loaded, &error));
  CHECK(loaded.recompute());
  const auto* loadedDraft = dynamic_cast<const solidar::DraftFeature*>(
      loaded.activeBody()->activeFeature());
  CHECK(loadedDraft && loadedDraft->id() == draftId);
  CHECK(loadedDraft->draftedFaces().front().signature);
  CHECK(loadedDraft->rotationEdge());
  CHECK(loadedDraft->rotationEdge()->signature);
  CHECK(loadedDraft->rotationEdge()->bodyId == bodyId);
  CHECK(loadedDraft->rotationEdge()->featureId == sourceId);
  CHECK(loadedDraft->pullDirection().type == solidar::AxisReferenceType::GlobalZ);

  draftPtr->setRotationEdge(std::nullopt);
  draftPtr->setNeutralPlane(
      {solidar::NeutralPlaneType::GlobalXY, std::nullopt});
  draftPtr->setPullDirection(
      {static_cast<solidar::AxisReferenceType>(999), staleAxisSketchId,
       solidar::sketch::kInvalidGeometryId});
  CHECK(!document.recompute());
  CHECK(draftPtr->error() == "Draft pull direction type is unsupported");

  // BodyFace neutral planes are resolved against the immediate source Shape,
  // so a foreign Body/Feature owner is unsupported and must fail closed in
  // direct model use, the live session, and persisted input validation.
  {
    solidar::Document ownerDocument;
    auto& firstSketch = ownerDocument.addSketch("First");
    firstSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 20.0});
    const auto firstSketchId = firstSketch.id;
    auto& secondSketch = ownerDocument.addSketch("Second");
    secondSketch.geometry.addRectangle({30.0, 0.0}, {50.0, 20.0});
    const auto secondSketchId = secondSketch.id;
    auto& firstBodyRef = ownerDocument.addBody("First body");
    const auto firstBodyId = firstBodyRef.id();
    firstBodyRef.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        firstSketchId, 10.0, "First source",
        solidar::ExtrudeOperation::NewBody));
    auto& secondBodyRef = ownerDocument.addBody("Second body");
    const auto secondBodyId = secondBodyRef.id();
    secondBodyRef.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        secondSketchId, 10.0, "Second source",
        solidar::ExtrudeOperation::NewBody));
    CHECK(ownerDocument.recompute());
    auto* firstBody = ownerDocument.findBody(firstBodyId);
    auto* secondBody = ownerDocument.findBody(secondBodyId);
    CHECK(firstBody && secondBody && firstBody->activeFeature() &&
          secondBody->activeFeature());
    auto* firstSource = firstBody->activeFeature();
    auto* secondSource = secondBody->activeFeature();
    const auto drafted = solidar::makeFaceReference(
        *firstSource->shape(), firstBodyId, firstSource->id(), 0);
    const auto foreignNeutral = solidar::makeFaceReference(
        *secondSource->shape(), secondBodyId, secondSource->id(), 0);
    CHECK(drafted.signature && foreignNeutral.signature);
    solidar::PlaneReference foreignPlane{
        solidar::NeutralPlaneType::BodyFace, foreignNeutral};
    solidar::AxisReference globalDirection{
        solidar::AxisReferenceType::GlobalZ,
        solidar::kInvalidSketchId,
        solidar::sketch::kInvalidGeometryId};
    auto foreignDraft = std::make_unique<solidar::DraftFeature>(
        firstSource->id(), std::vector{drafted}, foreignPlane,
        globalDirection, 5.0, false, "Foreign neutral");
    auto* foreignDraftPtr = foreignDraft.get();
    firstBody->addFeature(std::move(foreignDraft));
    CHECK(!ownerDocument.recompute());
    CHECK(foreignDraftPtr->error().find("active source Feature") !=
          std::string::npos);

    solidar::DraftToolSession ownerSession;
    ownerSession.begin(ownerDocument, firstBodyId, firstSource->id(),
                       firstSource->lastValidShape(), {drafted}, foreignPlane,
                       globalDirection, 5.0, false, std::nullopt,
                       std::nullopt, firstSource->lastValidTopologyIndex());
    CHECK(ownerSession.lifecycle() ==
          solidar::ToolLifecycle::PreviewInvalid);
    CHECK(ownerSession.errorCode() ==
          solidar::OperationFailureCode::TopologyReferenceMismatch);
    CHECK(ownerSession.error().find("active source Feature") !=
          std::string::npos);

    // An unknown enum must not inherit the BodyFace branch. Keep a locally
    // resolvable topology payload but foreign owner IDs to prove both the
    // Feature boundary and the live session reject the enum itself first.
    auto foreignOwnedLocalFace = drafted;
    foreignOwnedLocalFace.bodyId = secondBodyId;
    foreignOwnedLocalFace.featureId = secondSource->id();
    const solidar::PlaneReference unknownPlane{
        static_cast<solidar::NeutralPlaneType>(999), foreignOwnedLocalFace};
    foreignDraftPtr->setNeutralPlane(unknownPlane);
    CHECK(!ownerDocument.recompute());
    CHECK(foreignDraftPtr->error() ==
          "Draft neutral plane type is unsupported");

    gp_Pln unresolvedPlane;
    gp_Dir unresolvedDirection;
    const auto unknownResolution = solidar::resolveDraftReferences(
        ownerDocument, *firstSource->lastValidShape(),
        *firstSource->lastValidTopologyIndex(), unknownPlane, globalDirection,
        &unresolvedPlane, &unresolvedDirection);
    CHECK(!unknownResolution);
    CHECK(unknownResolution.failure.code ==
          solidar::OperationFailureCode::InvalidInput);
    CHECK(unknownResolution.failure.detail ==
          "Draft neutral plane type is unsupported");

    solidar::DraftToolSession unknownPlaneSession;
    unknownPlaneSession.begin(
        ownerDocument, firstBodyId, firstSource->id(),
        firstSource->lastValidShape(), {drafted}, unknownPlane,
        globalDirection, 5.0, false, std::nullopt, std::nullopt,
        firstSource->lastValidTopologyIndex());
    CHECK(unknownPlaneSession.lifecycle() ==
          solidar::ToolLifecycle::PreviewInvalid);
    CHECK(unknownPlaneSession.error() ==
          "Draft neutral plane type is unsupported");

    // Serialize the known-valid main Draft first, then turn a copy of its
    // drafted-face payload into a neutral-plane reference owned by the Draft
    // itself instead of by its immediate source Feature. This exercises the
    // loader boundary without asking the writer to accept an invalid model.
    draftPtr->setNeutralPlane(
        {solidar::NeutralPlaneType::GlobalXY, std::nullopt});
    draftPtr->setPullDirection(direction);
    CHECK(document.recompute());
    const QString validPath = temporary.filePath("valid-neutral.solidar");
    error.clear();
    const bool savedValidFixture = solidar::project::ProjectFile::saveDocument(
        validPath, document, &error);
    if (!savedValidFixture) std::cerr << error.toStdString() << '\n';
    CHECK(savedValidFixture);
    QFile validFile(validPath);
    CHECK(validFile.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(validFile.readAll()).object();
    validFile.close();
    auto model = root.value("model").toObject();
    auto bodies = model.value("bodies").toArray();
    auto firstBodyObject = bodies.at(0).toObject();
    auto features = firstBodyObject.value("features").toArray();
    auto persistedDraft = features.at(features.size() - 1).toObject();
    auto persistedNeutral = persistedDraft.value("draftedFaces")
                                .toArray()
                                .at(0)
                                .toObject();
    persistedNeutral["featureId"] = static_cast<qint64>(draftId);
    persistedDraft["neutralPlaneType"] =
        static_cast<int>(solidar::NeutralPlaneType::BodyFace);
    persistedDraft["neutralPlaneFace"] = persistedNeutral;
    features[features.size() - 1] = persistedDraft;
    firstBodyObject["features"] = features;
    bodies[0] = firstBodyObject;
    model["bodies"] = bodies;
    root["model"] = model;

    const QString foreignPath = temporary.filePath("foreign-neutral.solidar");
    QFile foreignFile(foreignPath);
    CHECK(foreignFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    CHECK(foreignFile.write(QJsonDocument(root).toJson()) > 0);
    foreignFile.close();
    solidar::Document rejected;
    error.clear();
    CHECK(!solidar::project::ProjectFile::loadDocument(
        foreignPath, &rejected, &error));
    CHECK(!error.isEmpty());
  }
  return 0;
}
