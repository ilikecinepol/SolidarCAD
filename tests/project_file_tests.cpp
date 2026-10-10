#include "project/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <BRep_Builder.hxx>
#include <TopoDS_Face.hxx>

#include "TestAssertions.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "TestGeometryUtils.h"
#include "model/ChamferFeature.h"
#include "model/CircularPatternFeature.h"
#include "model/DraftFeature.h"
#include "model/ExtrudeFeature.h"
#include "model/FilletBuilder.h"
#include "model/FilletFeature.h"
#include "model/ImportedShapeFeature.h"
#include "model/JoinBodiesFeature.h"
#include "model/LinearPatternFeature.h"
#include "model/PocketFeature.h"
#include "model/MirrorFeature.h"
#include "model/MoveFeature.h"
#include "model/RevolveFeature.h"
#include "model/ShellFeature.h"
#include "sketch/SketchConstraintDiagnostics.h"

namespace {

class UnsupportedFeature final : public solidar::ShapeFeature {
 public:
  UnsupportedFeature() : ShapeFeature("Unsupported adapter") {}
  [[nodiscard]] std::unique_ptr<solidar::Feature> clone() const override {
    return std::make_unique<UnsupportedFeature>(*this);
  }

 protected:
  bool rebuildImpl(const solidar::RebuildContext&) override { return false; }
};

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  // Keep test artifacts under CTest's writable build directory. This also
  // avoids platform policies that deny atomic QSaveFile replacement in the
  // user's global temporary directory.
  QTemporaryDir directory(
      QDir::current().filePath(QStringLiteral("project-file-tests-XXXXXX")));
  if (!directory.isValid()) {
    std::fprintf(stderr, "QTemporaryDir failed: %s\n",
                 directory.errorString().toUtf8().constData());
    return 1;
  }

  // FeatureKind is a stable Qt-free model identity. The project registry is
  // the single bidirectional kind/token mapping used by validation, encoding
  // and decoding; every persisted concrete feature must preserve it on clone.
  static_assert(static_cast<int>(solidar::FeatureKind::ImportedShape) == 1);
  static_assert(static_cast<int>(solidar::FeatureKind::Extrude) == 2);
  static_assert(static_cast<int>(solidar::FeatureKind::Revolve) == 3);
  static_assert(static_cast<int>(solidar::FeatureKind::Pocket) == 4);
  static_assert(static_cast<int>(solidar::FeatureKind::Fillet) == 5);
  static_assert(static_cast<int>(solidar::FeatureKind::Chamfer) == 6);
  static_assert(static_cast<int>(solidar::FeatureKind::Mirror) == 7);
  static_assert(static_cast<int>(solidar::FeatureKind::Move) == 8);
  static_assert(static_cast<int>(solidar::FeatureKind::LinearPattern) == 9);
  static_assert(static_cast<int>(solidar::FeatureKind::CircularPattern) == 10);
  static_assert(static_cast<int>(solidar::FeatureKind::JoinBodies) == 11);
  static_assert(static_cast<int>(solidar::FeatureKind::Shell) == 12);
  static_assert(static_cast<int>(solidar::FeatureKind::Draft) == 13);

  // Independent compatibility oracle: these are the exact v2 tokens already
  // present in historical .solidar files. Do not derive this table from the
  // registry or feature implementation, otherwise an accidental coordinated
  // rename would
  // make the test pass while breaking existing projects.
  constexpr std::array<
      std::pair<solidar::FeatureKind, std::string_view>,
      13>
      kHistoricalFeatureTokens{{
          {solidar::FeatureKind::ImportedShape, "ImportedShape"},
          {solidar::FeatureKind::Extrude, "Extrude"},
          {solidar::FeatureKind::Revolve, "Revolve"},
          {solidar::FeatureKind::Pocket, "Pocket"},
          {solidar::FeatureKind::Fillet, "Fillet"},
          {solidar::FeatureKind::Chamfer, "Chamfer"},
          {solidar::FeatureKind::Mirror, "Mirror"},
          {solidar::FeatureKind::Move, "Move"},
          {solidar::FeatureKind::LinearPattern, "LinearPattern"},
          {solidar::FeatureKind::CircularPattern, "CircularPattern"},
          {solidar::FeatureKind::JoinBodies, "JoinBodies"},
          {solidar::FeatureKind::Shell, "Shell"},
          {solidar::FeatureKind::Draft, "Draft"},
      }};
  static_assert(kHistoricalFeatureTokens.size() == 13);

  const auto descriptors = solidar::project::featureCodecDescriptors();
  CHECK(descriptors.size() == solidar::kPersistedFeatureKindCount);
  CHECK(descriptors.size() == kHistoricalFeatureTokens.size());
  std::unordered_set<int> registeredKinds;
  std::unordered_set<std::string> registeredTokens;
  for (std::size_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    CHECK(descriptor.kind == kHistoricalFeatureTokens[index].first);
    CHECK(descriptor.token == kHistoricalFeatureTokens[index].second);
    CHECK(registeredKinds.insert(static_cast<int>(descriptor.kind)).second);
    CHECK(registeredTokens.insert(std::string(descriptor.token)).second);
    const auto tokenRoundTrip = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&descriptor](const auto& candidate) {
          return candidate.token == descriptor.token;
        });
    CHECK(tokenRoundTrip != descriptors.end());
    CHECK(tokenRoundTrip->kind == descriptor.kind);
  }

  const solidar::EdgeReference edge{1, 1, 0};
  const solidar::FaceReference face{1, 1, 0};
  std::vector<std::unique_ptr<solidar::Feature>> featureKinds;
  featureKinds.push_back(std::make_unique<solidar::ImportedShapeFeature>(
      std::make_shared<const TopoDS_Shape>(), "ImportedShape"));
  featureKinds.push_back(
      std::make_unique<solidar::ExtrudeFeature>(1, 5.0, "Extrude"));
  featureKinds.push_back(std::make_unique<solidar::RevolveFeature>(
      1,
      solidar::AxisReference{solidar::AxisReferenceType::GlobalZ,
                             solidar::kInvalidSketchId,
                             solidar::sketch::kInvalidGeometryId},
      90.0, "Revolve"));
  featureKinds.push_back(
      std::make_unique<solidar::PocketFeature>(1, 2.0, "Pocket"));
  featureKinds.push_back(
      std::make_unique<solidar::FilletFeature>(edge, 1.0, "Fillet"));
  featureKinds.push_back(std::make_unique<solidar::ChamferFeature>(
      std::vector<solidar::EdgeReference>{edge}, 1.0, "Chamfer"));
  featureKinds.push_back(std::make_unique<solidar::MirrorFeature>(
      1, solidar::MirrorPlane::YZ, "Mirror"));
  featureKinds.push_back(std::make_unique<solidar::MoveFeature>(
      1, solidar::Vector3d{1.0, 2.0, 3.0}, "Move"));
  featureKinds.push_back(std::make_unique<solidar::LinearPatternFeature>(
      1, solidar::PrincipalAxis::X, 2, 5.0, "LinearPattern"));
  featureKinds.push_back(std::make_unique<solidar::CircularPatternFeature>(
      1, solidar::PrincipalAxis::Z, 2, 180.0, "CircularPattern"));
  featureKinds.push_back(std::make_unique<solidar::JoinBodiesFeature>(
      1, 1, 2, 2, "JoinBodies"));
  featureKinds.push_back(std::make_unique<solidar::ShellFeature>(
      1, std::vector<solidar::FaceReference>{face}, 1.0, false, "Shell"));
  featureKinds.push_back(std::make_unique<solidar::DraftFeature>(
      1, std::vector<solidar::FaceReference>{face},
      solidar::PlaneReference{solidar::NeutralPlaneType::GlobalXY},
      solidar::AxisReference{solidar::AxisReferenceType::GlobalZ,
                             solidar::kInvalidSketchId,
                             solidar::sketch::kInvalidGeometryId},
      5.0, false, "Draft"));
  CHECK(featureKinds.size() == descriptors.size());
  for (const auto& feature : featureKinds) {
    const auto descriptor = std::find_if(
        descriptors.begin(), descriptors.end(),
        [&feature](const auto& candidate) {
          return candidate.kind == feature->kind();
        });
    CHECK(descriptor != descriptors.end());
    const auto clone = feature->clone();
    CHECK(clone);
    CHECK(clone->kind() == feature->kind());
  }
  const QString path = directory.filePath("sample.solidar");

  QString error;
  if (!solidar::project::ProjectFile::create(path, &error)) {
    std::fprintf(stderr, "ProjectFile::create failed for %s: %s\n",
                 path.toUtf8().constData(), error.toUtf8().constData());
    return 1;
  }
  CHECK(QFile::exists(path));
  CHECK(solidar::project::ProjectFile::validate(path, &error));

  // A freshly created project is already a canonical v2 document.  This is
  // important because Home -> New immediately feeds the file into the editor's
  // document loader; it must not take a legacy-only initialization path.
  solidar::Document createdDocument;
  CHECK(solidar::project::ProjectFile::loadDocument(
      path, &createdDocument, &error));
  CHECK(createdDocument.sketches().empty());
  CHECK(createdDocument.bodies().empty());

  // Project v2 round-trip preserves IDs, parameters, face support and the
  // complete editable feature chain, then rebuilds B-Rep from history.
  solidar::Document source;
  auto& arcSketch = source.addSketch("Arc constraints");
  const auto arcSketchId = arcSketch.id;
  arcSketch.geometry.addLine({-50.0, 0.0}, {50.0, 0.0});
  arcSketch.geometry.addArc({10.0, 20.0}, 8.0,
                            3.14159265358979323846,
                            3.14159265358979323846);
  arcSketch.geometry.addCircle({35.0, 20.0}, 6.0);
  solidar::sketch::Dimension circleDiameter;
  circleDiameter.kind = solidar::sketch::DimensionKind::CircleDiameter;
  circleDiameter.geometryId = arcSketch.geometry.circleId(0);
  circleDiameter.valueMm = 12.0;
  circleDiameter.offsetMm = 3.0;
  arcSketch.geometry.storeDimension(circleDiameter);
  const auto circleDiameterId = arcSketch.geometry.dimensions().back().id;
  solidar::sketch::Dimension pointDistance;
  pointDistance.kind = solidar::sketch::DimensionKind::PointDistance;
  pointDistance.firstPoint = {arcSketch.geometry.lineId(0), true};
  pointDistance.secondPoint = {arcSketch.geometry.lineId(0), false};
  pointDistance.valueMm = 100.0;
  pointDistance.offsetMm = 5.0;
  arcSketch.geometry.storeDimension(pointDistance);
  const auto pointDistanceId = arcSketch.geometry.dimensions().back().id;
  solidar::sketch::Constraint arcTangent;
  arcTangent.type = solidar::sketch::ConstraintType::Tangent;
  arcTangent.firstGeometry = arcSketch.geometry.lineId(0);
  arcTangent.secondGeometry = arcSketch.geometry.arcId(0);
  CHECK(arcSketch.geometry.addConstraint(arcTangent) !=
         solidar::sketch::kInvalidConstraintId);
  auto& baseSketch = source.addSketch("Base");
  const auto baseSketchId = baseSketch.id;
  baseSketch.geometry.addRectangle({0.0, 0.0}, {80.0, 35.0});
  auto& body = source.addBody("Body");
  const auto bodyId = body.id();
  auto extrude = std::make_unique<solidar::ExtrudeFeature>(
      baseSketchId, 50.0, "Extrude");
  auto* extrudePtr = extrude.get();
  body.addFeature(std::move(extrude));
  CHECK(source.recompute());
  const auto topFace =
      solidar::test::topPlanarFace(*body.resultShape(), 50.0);
  CHECK(topFace);
  auto& pocketSketch = source.addSketch("Pocket sketch");
  const auto pocketSketchId = pocketSketch.id;
  CHECK(source.attachSketchToFace(
      pocketSketchId, {bodyId, extrudePtr->id(), *topFace}));
  pocketSketch.geometry.addRectangle({-10.0, -5.0}, {10.0, 5.0});
  auto pocket = std::make_unique<solidar::PocketFeature>(
      pocketSketchId, 10.0, "Pocket");
  auto* pocketPtr = pocket.get();
  body.addFeature(std::move(pocket));
  CHECK(source.recompute());
  std::size_t edgeIndex = 0;
  for (;; ++edgeIndex) {
    std::string buildError;
    if (solidar::buildFilletShape(*body.resultShape(), {edgeIndex}, 2.0,
                                  &buildError))
      break;
    CHECK(edgeIndex < 64);
  }
  auto fillet = std::make_unique<solidar::FilletFeature>(
      solidar::EdgeReference{bodyId, pocketPtr->id(), edgeIndex}, 2.0,
      "Fillet");
  const auto filletId = fillet->id();
  body.addFeature(std::move(fillet));
  CHECK(source.recompute());
  const double sourceVolume =
      solidar::test::volumeOf(*body.resultShape());

  const QString modelPath = directory.filePath("parametric-v2.solidar");
  CHECK(solidar::project::ProjectFile::saveDocument(modelPath, source,
                                                      &error));
  {
    auto staged = solidar::project::ProjectFile::stageLoad(modelPath);
    CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV2);
    CHECK(staged.document.has_value());
  }
  solidar::Document restored;
  CHECK(solidar::project::ProjectFile::loadDocument(modelPath, &restored,
                                                      &error));
  const auto* restoredBody = restored.findBody(bodyId);
  CHECK(restoredBody && restoredBody->features().size() == 3);
  CHECK(restored.findSketch(baseSketchId));
  const auto* restoredArcSketch = restored.findSketch(arcSketchId);
  CHECK(restoredArcSketch);
  CHECK(restoredArcSketch->geometry.arcs().size() == 1);
  CHECK(restoredArcSketch->geometry.circles().size() == 1);
  CHECK(restoredArcSketch->geometry.dimensions().size() == 2);
  CHECK(restoredArcSketch->geometry.dimensions().front().kind ==
        solidar::sketch::DimensionKind::CircleDiameter);
  CHECK(restoredArcSketch->geometry.dimensions().front().id ==
        circleDiameterId);
  CHECK(restoredArcSketch->geometry.dimensions().front().geometryId ==
        restoredArcSketch->geometry.circleId(0));
  CHECK(solidar::test::near(
      restoredArcSketch->geometry.dimensions().front().valueMm, 12.0));
  const auto& restoredPointDistance =
      restoredArcSketch->geometry.dimensions()[1];
  CHECK(restoredPointDistance.id == pointDistanceId);
  CHECK(restoredPointDistance.kind ==
        solidar::sketch::DimensionKind::PointDistance);
  CHECK(restoredPointDistance.firstPoint.lineId ==
        restoredArcSketch->geometry.lineId(0));
  CHECK(restoredPointDistance.firstPoint.start);
  CHECK(restoredPointDistance.secondPoint.lineId ==
        restoredArcSketch->geometry.lineId(0));
  CHECK(!restoredPointDistance.secondPoint.start);
  CHECK(solidar::test::near(restoredPointDistance.valueMm, 100.0));
  CHECK(restoredArcSketch->geometry.constraints().size() == 1);
  CHECK(restoredArcSketch->geometry.constraints().front().type ==
         solidar::sketch::ConstraintType::Tangent);
  CHECK(restoredArcSketch->geometry.constraints().front().secondGeometry ==
         restoredArcSketch->geometry.arcId(0));
  const auto* restoredPocketSketch = restored.findSketch(pocketSketchId);
  CHECK(restoredPocketSketch);
  CHECK(restoredPocketSketch->support.type ==
         solidar::SketchSupportType::Face);
  CHECK(restoredPocketSketch->support.face.featureId == extrudePtr->id());
  CHECK(restoredBody->features().back()->id() == filletId);
  CHECK(restoredBody->features().back()->kind() ==
         solidar::FeatureKind::Fillet);
  CHECK(solidar::test::near(
      solidar::test::volumeOf(*restoredBody->resultShape()), sourceVolume,
      1e-4));

  // A constraint accepted through the transactional boundary retains its
  // stable ID and component diagnostics across a native v2 round-trip.
  {
    solidar::Document transactionalDocument;
    auto& safetySketch =
        transactionalDocument.addSketch("Transactional constraints");
    const auto safetySketchId = safetySketch.id;
    safetySketch.geometry.addLine({0.0, 0.0}, {10.0, 2.0});
    solidar::sketch::Constraint horizontal;
    horizontal.type = solidar::sketch::ConstraintType::Horizontal;
    horizontal.firstGeometry = safetySketch.geometry.lineId(0);
    const auto horizontalApplied =
        safetySketch.geometry.tryApplyConstraint(horizontal);
    CHECK(horizontalApplied.accepted());
    const auto safetyDiagnostics = solidar::sketch::analyzeConstraintSystem(
        safetySketch.geometry);
    CHECK(!safetyDiagnostics.conflicting);
    CHECK(!solidar::sketch::hasConstraintViolation(
        safetyDiagnostics, horizontalApplied.constraintId));

    const QString safetyPath =
        directory.filePath("transactional-constraint-v2.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        safetyPath, transactionalDocument, &error));
    solidar::Document transactionalRestored;
    CHECK(solidar::project::ProjectFile::loadDocument(
        safetyPath, &transactionalRestored, &error));
    const auto* restoredSafetySketch =
        transactionalRestored.findSketch(safetySketchId);
    CHECK(restoredSafetySketch);
    CHECK(restoredSafetySketch->geometry.constraints().size() == 1);
    CHECK(restoredSafetySketch->geometry.constraints().front().id ==
          horizontalApplied.constraintId);
    CHECK(restoredSafetySketch->geometry.constraints().front().type ==
          solidar::sketch::ConstraintType::Horizontal);
    const auto restoredSafetyDiagnostics =
        solidar::sketch::analyzeConstraintSystem(
            restoredSafetySketch->geometry);
    CHECK(restoredSafetyDiagnostics.equationRank ==
          safetyDiagnostics.equationRank);
    CHECK(restoredSafetyDiagnostics.degreesOfFreedom ==
          safetyDiagnostics.degreesOfFreedom);
    CHECK(restoredSafetyDiagnostics.components.size() ==
          safetyDiagnostics.components.size());
    CHECK(!solidar::sketch::hasConstraintViolation(
        restoredSafetyDiagnostics, horizontalApplied.constraintId));
  }

  // A native Line + Arc profile remains editable after a complete project
  // round-trip. IDs and feature parameters must survive serialization, while
  // the B-Rep is rebuilt from the restored parametric history.
  {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kExtrudeLength = 25.0;
    solidar::Document lineArcDocument;
    auto& lineArcSketch = lineArcDocument.addSketch("Line and Arc profile");
    const auto lineArcSketchId = lineArcSketch.id;
    lineArcSketch.geometry.addLine({-10.0, 0.0}, {10.0, 0.0});
    lineArcSketch.geometry.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    CHECK(lineArcSketch.geometry.lines().size() == 1);
    CHECK(lineArcSketch.geometry.arcs().size() == 1);
    CHECK(lineArcSketch.geometry.circles().empty());
    CHECK(lineArcSketch.geometry.isClosed());
    const auto sourceArc = lineArcSketch.geometry.arcs().front();

    auto& lineArcBody = lineArcDocument.addBody("Line and Arc body");
    const auto lineArcBodyId = lineArcBody.id();
    auto lineArcExtrude = std::make_unique<solidar::ExtrudeFeature>(
        lineArcSketchId, kExtrudeLength, "Line and Arc Extrude",
        solidar::ExtrudeOperation::NewBody, true);
    auto* lineArcExtrudePtr = lineArcExtrude.get();
    const auto lineArcExtrudeId = lineArcExtrudePtr->id();
    const auto sourceOperation = lineArcExtrudePtr->operation();
    const bool sourceReversed = lineArcExtrudePtr->reversed();
    lineArcBody.addFeature(std::move(lineArcExtrude));

    CHECK(lineArcDocument.recompute());
    CHECK(lineArcExtrudePtr->state() == solidar::FeatureState::Valid);
    CHECK(lineArcExtrudePtr->shape());
    CHECK(!lineArcExtrudePtr->shape()->IsNull());
    CHECK(solidar::test::solidCount(*lineArcExtrudePtr->shape()) == 1);
    const double lineArcVolume =
        solidar::test::volumeOf(*lineArcExtrudePtr->shape());
    CHECK(lineArcVolume > 0.0);

    const QString lineArcPath = directory.filePath("line-arc.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        lineArcPath, lineArcDocument, &error));

    solidar::Document loadedLineArcDocument;
    CHECK(solidar::project::ProjectFile::loadDocument(
        lineArcPath, &loadedLineArcDocument, &error));
    auto* loadedLineArcSketch =
        loadedLineArcDocument.findSketch(lineArcSketchId);
    CHECK(loadedLineArcSketch);
    CHECK(loadedLineArcSketch->id == lineArcSketchId);
    CHECK(loadedLineArcSketch->geometry.lines().size() == 1);
    CHECK(loadedLineArcSketch->geometry.arcs().size() == 1);
    CHECK(loadedLineArcSketch->geometry.circles().empty());
    CHECK(loadedLineArcSketch->geometry.isClosed());
    const auto& loadedArc = loadedLineArcSketch->geometry.arcs().front();
    CHECK(solidar::test::near(loadedArc.center.xMm, sourceArc.center.xMm));
    CHECK(solidar::test::near(loadedArc.center.yMm, sourceArc.center.yMm));
    CHECK(solidar::test::near(loadedArc.radiusMm, sourceArc.radiusMm));
    CHECK(solidar::test::near(loadedArc.startAngleRad,
                              sourceArc.startAngleRad));
    CHECK(solidar::test::near(loadedArc.sweepAngleRad,
                              sourceArc.sweepAngleRad));

    auto* loadedLineArcBody =
        loadedLineArcDocument.findBody(lineArcBodyId);
    CHECK(loadedLineArcBody);
    CHECK(loadedLineArcBody->features().size() == 1);
    auto* loadedLineArcExtrude = dynamic_cast<solidar::ExtrudeFeature*>(
        loadedLineArcBody->features().front().get());
    CHECK(loadedLineArcExtrude);
    CHECK(loadedLineArcExtrude->id() == lineArcExtrudeId);
    CHECK(loadedLineArcExtrude->profileSketchId() == lineArcSketchId);
    CHECK(solidar::test::near(loadedLineArcExtrude->lengthMm(),
                              kExtrudeLength));
    CHECK(loadedLineArcExtrude->operation() == sourceOperation);
    CHECK(loadedLineArcExtrude->reversed() == sourceReversed);

    CHECK(loadedLineArcDocument.recompute());
    CHECK(loadedLineArcExtrude->state() == solidar::FeatureState::Valid);
    CHECK(loadedLineArcExtrude->shape());
    CHECK(!loadedLineArcExtrude->shape()->IsNull());
    CHECK(solidar::test::solidCount(*loadedLineArcExtrude->shape()) == 1);
    const double loadedLineArcVolume =
        solidar::test::volumeOf(*loadedLineArcExtrude->shape());
    CHECK(solidar::test::near(loadedLineArcVolume, lineArcVolume, 1e-4));

    loadedLineArcSketch->geometry.removeArc(0);
    const double editedRadius = 12.0;
    const double centerOffset =
        std::sqrt(editedRadius * editedRadius - 10.0 * 10.0);
    const double startAngle = std::atan2(centerOffset, 10.0);
    loadedLineArcSketch->geometry.addArc(
        {0.0, -centerOffset}, editedRadius, startAngle,
        kPi - 2.0 * startAngle);
    CHECK(loadedLineArcSketch->geometry.lines().size() == 1);
    CHECK(loadedLineArcSketch->geometry.arcs().size() == 1);
    CHECK(loadedLineArcSketch->geometry.isClosed());
    CHECK(loadedLineArcDocument.markSketchDirty(lineArcSketchId));
    CHECK(loadedLineArcExtrude->isDirty());
    CHECK(loadedLineArcDocument.recompute());
    CHECK(loadedLineArcExtrude->id() == lineArcExtrudeId);
    CHECK(loadedLineArcExtrude->state() == solidar::FeatureState::Valid);
    CHECK(loadedLineArcExtrude->shape());
    CHECK(!loadedLineArcExtrude->shape()->IsNull());
    CHECK(solidar::test::solidCount(*loadedLineArcExtrude->shape()) == 1);
    CHECK(std::abs(solidar::test::volumeOf(*loadedLineArcExtrude->shape()) -
                   loadedLineArcVolume) > 1e-4);
  }

  // A classified outer contour + analytic hole survives native save/load and
  // rebuilds to the same one-solid topology and volume.
  {
    constexpr double kPi = 3.14159265358979323846;
    solidar::Document holeDocument;
    auto& holeSketch = holeDocument.addSketch("Profile with hole");
    const auto holeSketchId = holeSketch.id;
    holeSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 30.0});
    holeSketch.geometry.addCircle({20.0, 15.0}, 5.0);
    auto& holeBody = holeDocument.addBody("Body with hole");
    const auto holeBodyId = holeBody.id();
    auto holeExtrude = std::make_unique<solidar::ExtrudeFeature>(
        holeSketchId, 12.0, "Extrude with hole");
    const auto holeFeatureId = holeExtrude->id();
    holeBody.addFeature(std::move(holeExtrude));
    CHECK(holeDocument.recompute());
    const double expectedVolume = (40.0 * 30.0 - kPi * 25.0) * 12.0;
    CHECK(solidar::test::solidCount(*holeBody.resultShape()) == 1);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*holeBody.resultShape()), expectedVolume,
        1e-3));

    const QString holePath = directory.filePath("profile-with-hole.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        holePath, holeDocument, &error));
    solidar::Document restoredHoleDocument;
    CHECK(solidar::project::ProjectFile::loadDocument(
        holePath, &restoredHoleDocument, &error));
    const auto* restoredHoleSketch =
        restoredHoleDocument.findSketch(holeSketchId);
    const auto* restoredHoleBody = restoredHoleDocument.findBody(holeBodyId);
    CHECK(restoredHoleSketch);
    CHECK(restoredHoleSketch->geometry.lines().size() == 4);
    CHECK(restoredHoleSketch->geometry.circles().size() == 1);
    CHECK(restoredHoleBody);
    CHECK(restoredHoleBody->features().front()->id() == holeFeatureId);
    CHECK(solidar::test::solidCount(*restoredHoleBody->resultShape()) == 1);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*restoredHoleBody->resultShape()),
        expectedVolume, 1e-3));
  }

  // Extrude from one selected region of a multi-profile sketch.  The source
  // sketch remains intact, while the feature stores only the picked region.
  {
    solidar::Document selectedProfileDocument;
    auto& sourceSketch = selectedProfileDocument.addSketch("Multi profile");
    const auto sourceSketchId = sourceSketch.id;
    sourceSketch.geometry.addRectangle({0.0, 0.0}, {40.0, 25.0});
    sourceSketch.geometry.addCircle({70.0, 12.5}, 10.0);

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kExpectedSelectedArea = 0.5 * kPi * 10.0 * 10.0 +
                                             kPi * 5.0 * 5.0;
    solidar::sketch::Sketch selectedRegion;
    selectedRegion.addLine({-10.0, 0.0}, {10.0, 0.0});
    selectedRegion.addArc({0.0, 0.0}, 10.0, 0.0, kPi);
    selectedRegion.addCircle({40.0, 5.0}, 5.0);

    auto& selectedBody = selectedProfileDocument.addBody("Selected body");
    auto selectedExtrude = std::make_unique<solidar::ExtrudeFeature>(
        sourceSketchId, 12.0, "Selected region");
    selectedExtrude->setProfileOverride(selectedRegion);
    selectedBody.addFeature(std::move(selectedExtrude));

    CHECK(selectedProfileDocument.recompute());
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*selectedBody.resultShape()),
        kExpectedSelectedArea * 12.0, 1e-4));
    CHECK(solidar::test::solidCount(*selectedBody.resultShape()) == 2);

    const QString selectedPath =
        directory.filePath("selected-profile.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        selectedPath, selectedProfileDocument, &error));

    solidar::Document selectedRestored;
    CHECK(solidar::project::ProjectFile::loadDocument(
        selectedPath, &selectedRestored, &error));
    const auto* restoredSelectedBody = selectedRestored.activeBody();
    CHECK(restoredSelectedBody);
    CHECK(restoredSelectedBody->features().size() == 1);
    const auto* restoredExtrude =
        dynamic_cast<const solidar::ExtrudeFeature*>(
            restoredSelectedBody->features().front().get());
    CHECK(restoredExtrude);
    CHECK(restoredExtrude->profileOverride());
    const auto& restoredOverride = *restoredExtrude->profileOverride();
    CHECK(restoredOverride.lines().size() == 1);
    CHECK(restoredOverride.arcs().size() == 1);
    CHECK(restoredOverride.circles().size() == 1);
    const auto& restoredArc = restoredOverride.arcs().front();
    CHECK(solidar::test::near(restoredArc.center.xMm, 0.0));
    CHECK(solidar::test::near(restoredArc.center.yMm, 0.0));
    CHECK(solidar::test::near(restoredArc.radiusMm, 10.0));
    CHECK(solidar::test::near(restoredArc.startAngleRad, 0.0));
    CHECK(solidar::test::near(restoredArc.sweepAngleRad, kPi));
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*restoredSelectedBody->resultShape()),
        kExpectedSelectedArea * 12.0, 1e-4));
    CHECK(solidar::test::solidCount(*restoredSelectedBody->resultShape()) == 2);
  }

  // A true whole-sketch multi-region extrusion survives save/load with stable
  // identities, parameters and both disjoint solids rebuilt from history.
  {
    constexpr double kLength = 9.0;
    constexpr double kExpectedVolume =
        (20.0 * 10.0 + 15.0 * 10.0) * kLength;
    solidar::Document multiRegionDocument;
    auto& multiRegionSketch =
        multiRegionDocument.addSketch("Persistent multi-region");
    const auto sketchId = multiRegionSketch.id;
    multiRegionSketch.geometry.addRectangle({0.0, 0.0}, {20.0, 10.0});
    multiRegionSketch.geometry.addRectangle({40.0, 0.0}, {55.0, 10.0});
    auto& multiRegionBody = multiRegionDocument.addBody("Multi-region body");
    const auto bodyId = multiRegionBody.id();
    auto extrude = std::make_unique<solidar::ExtrudeFeature>(
        sketchId, kLength, "Multi-region Extrude",
        solidar::ExtrudeOperation::NewBody, false);
    auto* extrudePtr = extrude.get();
    const auto featureId = extrudePtr->id();
    multiRegionBody.addFeature(std::move(extrude));

    CHECK(multiRegionDocument.recompute());
    CHECK(extrudePtr->state() == solidar::FeatureState::Valid);
    CHECK(extrudePtr->shape());
    CHECK(solidar::test::solidCount(*extrudePtr->shape()) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*extrudePtr->shape()), kExpectedVolume, 1e-4));

    const QString multiRegionPath =
        directory.filePath("multi-region.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        multiRegionPath, multiRegionDocument, &error));
    solidar::Document restored;
    CHECK(solidar::project::ProjectFile::loadDocument(
        multiRegionPath, &restored, &error));
    auto* restoredSketch = restored.findSketch(sketchId);
    auto* restoredBody = restored.findBody(bodyId);
    CHECK(restoredSketch);
    CHECK(restoredSketch->id == sketchId);
    CHECK(restoredSketch->geometry.lines().size() == 8);
    CHECK(restoredBody);
    CHECK(restoredBody->features().size() == 1);
    auto* restoredExtrude = dynamic_cast<solidar::ExtrudeFeature*>(
        restoredBody->features().front().get());
    CHECK(restoredExtrude);
    CHECK(restoredExtrude->id() == featureId);
    CHECK(restoredExtrude->profileSketchId() == sketchId);
    CHECK(restoredExtrude->operation() == solidar::ExtrudeOperation::NewBody);
    CHECK(!restoredExtrude->reversed());
    CHECK(solidar::test::near(restoredExtrude->lengthMm(), kLength));
    CHECK(restored.recompute());
    CHECK(restoredExtrude->state() == solidar::FeatureState::Valid);
    CHECK(restoredExtrude->shape());
    CHECK(solidar::test::solidCount(*restoredExtrude->shape()) == 2);
    CHECK(solidar::test::near(
        solidar::test::volumeOf(*restoredExtrude->shape()), kExpectedVolume,
        1e-4));
  }

  // Deleting one Body is persisted as a model change: the removed Body and
  // its features do not reappear after save/load, while reusable base-plane
  // sketches and the survivor keep their stable identifiers.
  {
    solidar::Document removalDocument;
    auto& removedSketch = removalDocument.addSketch("Removed profile");
    const auto removedSketchId = removedSketch.id;
    removedSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& removedBody = removalDocument.addBody("Removed body");
    const auto removedBodyId = removedBody.id();
    removedBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        removedSketchId, 5.0));
    auto& keptSketch = removalDocument.addSketch("Kept profile");
    const auto keptSketchId = keptSketch.id;
    keptSketch.geometry.addRectangle({30.0, 0.0}, {42.0, 8.0});
    auto& keptBody = removalDocument.addBody("Kept body");
    const auto keptBodyId = keptBody.id();
    keptBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
        keptSketchId, 7.0));
    CHECK(removalDocument.recompute());
    std::string removalError;
    CHECK(removalDocument.removeBodyCascade(removedBodyId, &removalError));
    CHECK(removalDocument.recompute());

    const QString removalPath = directory.filePath("removed-body.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        removalPath, removalDocument, &error));
    solidar::Document restoredRemoval;
    CHECK(solidar::project::ProjectFile::loadDocument(
        removalPath, &restoredRemoval, &error));
    CHECK(restoredRemoval.findBody(removedBodyId) == nullptr);
    CHECK(restoredRemoval.findSketch(removedSketchId));
    CHECK(restoredRemoval.findBody(keptBodyId));
    CHECK(restoredRemoval.findSketch(keptSketchId));
    CHECK(restoredRemoval.bodies().size() == 1);
  }

  // A sketch-line axis is persisted by its validated position, not by the
  // transient in-memory GeometryId.  Interleaving geometry creation and then
  // deleting an earlier line deliberately leaves a gap in those IDs.
  {
    solidar::Document axisDocument;
    auto& profile = axisDocument.addSketch("Revolve profile");
    const auto profileId = profile.id;
    profile.geometry.addCircle({15.0, 0.0}, 2.0);

    auto& axisSketch = axisDocument.addSketch("Revolve axis");
    axisSketch.geometry.addCircle({30.0, 30.0}, 2.0);
    axisSketch.geometry.addLine({-5.0, -5.0}, {-5.0, 5.0});
    axisSketch.geometry.addLine({0.0, -20.0}, {0.0, 20.0});
    const auto selectedAxisId = axisSketch.geometry.lineId(1);
    axisSketch.geometry.removeLine(0);
    CHECK(axisSketch.geometry.lines().size() == 1);
    CHECK(axisSketch.geometry.lineId(0) == selectedAxisId);
    CHECK(selectedAxisId != 1);

    auto& revolveBody = axisDocument.addBody("Revolve body");
    revolveBody.addFeature(std::make_unique<solidar::RevolveFeature>(
        profileId,
        solidar::AxisReference{solidar::AxisReferenceType::SketchLine,
                               axisSketch.id, selectedAxisId},
        360.0, "Revolve", solidar::ExtrudeOperation::NewBody));
    const bool axisRecomputed = axisDocument.recompute();
    if (!axisRecomputed)
      std::fprintf(stderr, "axis revolve rebuild failed: %s\n",
                   axisDocument.rebuildError().c_str());
    CHECK(axisRecomputed);

    const QString axisPath = directory.filePath("revolve-axis-gap.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        axisPath, axisDocument, &error));
    QFile axisFile(axisPath);
    CHECK(axisFile.open(QIODevice::ReadOnly));
    const auto axisJson = QJsonDocument::fromJson(axisFile.readAll());
    CHECK(axisJson.isObject());
    const auto savedBodies =
        axisJson.object().value("model").toObject().value("bodies").toArray();
    CHECK(savedBodies.size() == 1);
    const auto savedFeatures =
        savedBodies[0].toObject().value("features").toArray();
    CHECK(savedFeatures.size() == 1);
    const auto savedRevolve = savedFeatures[0].toObject();
    CHECK(savedRevolve.value("axisLineIndex").toInteger(-1) == 0);
    CHECK(savedRevolve.value("axisLineId").toInteger(-1) == 1);

    const auto stagedAxis =
        solidar::project::ProjectFile::stageLoad(axisPath);
    CHECK(stagedAxis.kind == solidar::project::ProjectLoadKind::ValidV2);
    CHECK(stagedAxis.document.has_value());
    const auto* restoredAxisSketch =
        stagedAxis.document->findSketch(axisSketch.id);
    const auto* restoredRevolveBody =
        stagedAxis.document->findBody(revolveBody.id());
    CHECK(restoredAxisSketch);
    CHECK(restoredRevolveBody);
    CHECK(restoredRevolveBody->features().size() == 1);
    const auto* restoredRevolve =
        dynamic_cast<const solidar::RevolveFeature*>(
            restoredRevolveBody->features()[0].get());
    CHECK(restoredRevolve);
    CHECK(restoredRevolve->axis().lineId ==
          restoredAxisSketch->geometry.lineId(0));
    CHECK(restoredRevolve->isValid());
    CHECK(restoredRevolve->shape());

    // Early v2 stored only the in-memory GeometryId. Raw ID 1 is unambiguous
    // (the first surviving line); larger IDs cannot be mapped after grouped
    // serialization or deletions and are therefore reported as unsupported.
    auto historicalAxisRoot = axisJson.object();
    historicalAxisRoot.remove("constraintTypeEncoding");
    auto historicalModel = historicalAxisRoot.value("model").toObject();
    auto historicalBodies = historicalModel.value("bodies").toArray();
    auto historicalBody = historicalBodies[0].toObject();
    auto historicalFeatures = historicalBody.value("features").toArray();
    auto historicalRevolve = historicalFeatures[0].toObject();
    historicalRevolve.remove("axisLineIndex");
    historicalRevolve["axisLineId"] = 1;
    historicalFeatures[0] = historicalRevolve;
    historicalBody["features"] = historicalFeatures;
    historicalBodies[0] = historicalBody;
    historicalModel["bodies"] = historicalBodies;
    historicalAxisRoot["model"] = historicalModel;
    const auto writeAxisRoot = [&](const QString& fileName,
                                   const QJsonObject& root) {
      const QString filePath = directory.filePath(fileName);
      QFile file(filePath);
      if (!file.open(QIODevice::WriteOnly)) return QString{};
      const QByteArray bytes =
          QJsonDocument(root).toJson(QJsonDocument::Compact);
      if (file.write(bytes) != bytes.size()) return QString{};
      file.close();
      return filePath;
    };
    const QString historicalAxisPath =
        writeAxisRoot("historical-revolve-axis.solidar", historicalAxisRoot);
    CHECK(!historicalAxisPath.isEmpty());
    CHECK(solidar::project::ProjectFile::stageLoad(historicalAxisPath).kind ==
          solidar::project::ProjectLoadKind::ValidV2);

    historicalRevolve["axisLineId"] = 2;
    historicalFeatures[0] = historicalRevolve;
    historicalBody["features"] = historicalFeatures;
    historicalBodies[0] = historicalBody;
    historicalModel["bodies"] = historicalBodies;
    historicalAxisRoot["model"] = historicalModel;
    const QString ambiguousAxisPath =
        writeAxisRoot("ambiguous-revolve-axis.solidar", historicalAxisRoot);
    CHECK(!ambiguousAxisPath.isEmpty());
    CHECK(solidar::project::ProjectFile::stageLoad(ambiguousAxisPath).kind ==
          solidar::project::ProjectLoadKind::Unsupported);
  }

  // Also keep the non-zero positional case: a loader that always selected
  // line 0 would pass the deletion-gap test above but bind this feature to the
  // dummy line instead of the selected axis.
  {
    solidar::Document indexedAxisDocument;
    auto& profile = indexedAxisDocument.addSketch("Indexed profile");
    const auto profileId = profile.id;
    profile.geometry.addCircle({15.0, 0.0}, 2.0);
    auto& axisSketch = indexedAxisDocument.addSketch("Indexed axis");
    const auto axisSketchId = axisSketch.id;
    axisSketch.geometry.addCircle({30.0, 30.0}, 2.0);
    axisSketch.geometry.addLine({-5.0, -5.0}, {-5.0, 5.0});
    axisSketch.geometry.addLine({0.0, -20.0}, {0.0, 20.0});
    const auto selectedAxisId = axisSketch.geometry.lineId(1);
    CHECK(selectedAxisId != 2);
    auto& body = indexedAxisDocument.addBody("Indexed revolve body");
    body.addFeature(std::make_unique<solidar::RevolveFeature>(
        profileId,
        solidar::AxisReference{solidar::AxisReferenceType::SketchLine,
                               axisSketchId, selectedAxisId},
        360.0, "Indexed revolve", solidar::ExtrudeOperation::NewBody));
    CHECK(indexedAxisDocument.recompute());

    const QString indexedAxisPath =
        directory.filePath("revolve-axis-index-one.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        indexedAxisPath, indexedAxisDocument, &error));
    QFile savedFile(indexedAxisPath);
    CHECK(savedFile.open(QIODevice::ReadOnly));
    const auto savedRoot =
        QJsonDocument::fromJson(savedFile.readAll()).object();
    const auto savedFeature =
        savedRoot.value("model").toObject().value("bodies").toArray()[0]
            .toObject().value("features").toArray()[0].toObject();
    CHECK(savedFeature.value("axisLineIndex").toInteger(-1) == 1);
    CHECK(savedFeature.value("axisLineId").toInteger(-1) == 2);

    const auto staged =
        solidar::project::ProjectFile::stageLoad(indexedAxisPath);
    CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV2);
    CHECK(staged.document.has_value());
    const auto* restoredSketch = staged.document->findSketch(axisSketchId);
    const auto* restoredBody = staged.document->findBody(body.id());
    CHECK(restoredSketch);
    CHECK(restoredBody);
    const auto* restoredFeature =
        dynamic_cast<const solidar::RevolveFeature*>(
            restoredBody->features()[0].get());
    CHECK(restoredFeature);
    CHECK(restoredFeature->axis().lineId ==
          restoredSketch->geometry.lineId(1));
    CHECK(restoredFeature->isValid());
  }

  // A failed imported-shape serialization must not replace the last valid
  // project. The empty face is non-null but invalid, so validation fails
  // while the replacement payload is still only in memory.
  {
    const QString atomicPath = directory.filePath("atomic-save.solidar");
    const QByteArray previousPayload("previous valid project payload\n");
    QFile previous(atomicPath);
    CHECK(previous.open(QIODevice::WriteOnly));
    CHECK(previous.write(previousPayload) == previousPayload.size());
    previous.close();

    TopoDS_Face invalidFace;
    BRep_Builder builder;
    builder.MakeFace(invalidFace);
    solidar::Document invalidDocument;
    auto& body = invalidDocument.addBody("Invalid imported shape");
    body.addFeature(std::make_unique<solidar::ImportedShapeFeature>(
        std::make_shared<const TopoDS_Shape>(invalidFace), "Invalid"));

    CHECK(!solidar::project::ProjectFile::saveDocument(
        atomicPath, invalidDocument, &error));
    CHECK(!error.isEmpty());
    QFile preserved(atomicPath);
    CHECK(preserved.open(QIODevice::ReadOnly));
    CHECK(preserved.readAll() == previousPayload);
  }

  // An unregistered feature kind is rejected before atomic replacement and
  // never replaces an existing target with a partial project.
  {
    const QString atomicPath =
        directory.filePath("atomic-save-unsupported-feature.solidar");
    const QByteArray previousPayload("project survives unsupported feature\n");
    QFile previous(atomicPath);
    CHECK(previous.open(QIODevice::WriteOnly));
    CHECK(previous.write(previousPayload) == previousPayload.size());
    previous.close();

    solidar::Document unsupportedDocument;
    unsupportedDocument.addBody("Unsupported adapter")
        .addFeature(std::make_unique<UnsupportedFeature>());
    CHECK(!solidar::project::ProjectFile::saveDocument(
        atomicPath, unsupportedDocument, &error));
    CHECK(!error.isEmpty());
    QFile preserved(atomicPath);
    CHECK(preserved.open(QIODevice::ReadOnly));
    CHECK(preserved.readAll() == previousPayload);
  }

  // Serialization errors and a history that cannot be rebuilt are rejected
  // before QSaveFile is opened, preserving the prior project byte-for-byte.
  {
    const QByteArray previousPayload("previous project must survive\n");
    const auto seedFile = [&](const QString& filePath) {
      QFile file(filePath);
      if (!file.open(QIODevice::WriteOnly)) return false;
      const bool written = file.write(previousPayload) == previousPayload.size();
      file.close();
      return written;
    };
    const auto payloadPreserved = [&](const QString& filePath) {
      QFile file(filePath);
      return file.open(QIODevice::ReadOnly) &&
             file.readAll() == previousPayload;
    };

    solidar::project::ProjectData invalidDimensions;
    solidar::sketch::Sketch invalidGeometry;
    invalidGeometry.addLine({0.0, 0.0}, {10.0, 0.0});
    solidar::sketch::Dimension danglingDimension;
    danglingDimension.kind = solidar::sketch::DimensionKind::LineLength;
    danglingDimension.geometryId = 999999;
    danglingDimension.valueMm = 10.0;
    invalidGeometry.storeDimension(danglingDimension);
    invalidDimensions.sketches.push_back(
        {std::move(invalidGeometry), QStringLiteral("XY")});
    const QString invalidDimensionPath =
        directory.filePath("atomic-invalid-dimension.solidar");
    CHECK(seedFile(invalidDimensionPath));
    CHECK(!solidar::project::ProjectFile::save(
        invalidDimensionPath, invalidDimensions, &error));
    CHECK(!error.isEmpty());
    CHECK(payloadPreserved(invalidDimensionPath));

    solidar::Document invalidHistory;
    auto& historyProfile = invalidHistory.addSketch("Profile");
    historyProfile.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
    auto& historyBody = invalidHistory.addBody("Invalid history");
    auto extrusion = std::make_unique<solidar::ExtrudeFeature>(
        historyProfile.id, 5.0, "Extrude");
    const auto extrusionId = extrusion->id();
    historyBody.addFeature(std::move(extrusion));
    auto firstMove = std::make_unique<solidar::MoveFeature>(
        extrusionId, solidar::Vector3d{1.0, 0.0, 0.0}, "Move 1");
    historyBody.addFeature(std::move(firstMove));
    // Move features consume the immediately preceding shape. Referring back
    // to Extrude from the second Move is structurally resolvable but invalid.
    historyBody.addFeature(std::make_unique<solidar::MoveFeature>(
        extrusionId, solidar::Vector3d{2.0, 0.0, 0.0}, "Move 2"));
    const QString invalidHistoryPath =
        directory.filePath("atomic-invalid-history.solidar");
    CHECK(seedFile(invalidHistoryPath));
    CHECK(!solidar::project::ProjectFile::saveDocument(
        invalidHistoryPath, invalidHistory, &error));
    CHECK(!error.isEmpty());
    CHECK(payloadPreserved(invalidHistoryPath));
  }

  // The staged API makes legacy compatibility explicit instead of conflating
  // a valid v1 project with a failed v2 model decode.
  {
    // The initial v1 writer stored only project metadata and document bounds;
    // sketches/extrusion did not exist yet and therefore default to empty/off.
    const QString earliestV1Path =
        directory.filePath("earliest-v1-minimal.solidar");
    QFile earliestV1File(earliestV1Path);
    CHECK(earliestV1File.open(QIODevice::WriteOnly));
    const QJsonObject earliestV1Root{
        {"format", QStringLiteral("solidar-project")},
        {"version", 1},
        {"name", QStringLiteral("earliest-v1")},
        {"createdAt", QStringLiteral("2026-07-01T00:00:00Z")},
        {"document", QJsonObject{{"widthMm", 60.0},
                                  {"heightMm", 40.0},
                                  {"extrusionMm", 25.0}}}};
    const QByteArray earliestV1Bytes =
        QJsonDocument(earliestV1Root).toJson(QJsonDocument::Compact);
    CHECK(earliestV1File.write(earliestV1Bytes) == earliestV1Bytes.size());
    earliestV1File.close();
    auto earliestV1 =
        solidar::project::ProjectFile::stageLoad(earliestV1Path);
    CHECK(earliestV1.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(earliestV1.succeeded());
    CHECK(earliestV1.legacy.sketches.empty());
    CHECK(!earliestV1.legacy.hasExtrusion);
    CHECK(!earliestV1.legacy.extrusionSourceSketch.has_value());
    CHECK(solidar::test::near(earliestV1.legacy.box.widthMm, 60.0));
    CHECK(solidar::test::near(earliestV1.legacy.box.depthMm, 40.0));
    CHECK(solidar::test::near(earliestV1.legacy.box.heightMm, 25.0));

    // Literal shape emitted by bdb9530: PointDistance inherited the old
    // geometryIndex default (0), and the later optional channel groups did
    // not exist yet.
    {
      const QJsonObject bdbRoot{
          {"format", QStringLiteral("solidar-project")},
          {"version", 1},
          {"name", QStringLiteral("bdb9530")},
          {"createdAt", QStringLiteral("2026-07-12T00:00:00Z")},
          {"document", QJsonObject{{"widthMm", 60.0},
                                    {"heightMm", 40.0},
                                    {"extrusionMm", 25.0}}},
          {"sketches",
           QJsonArray{QJsonObject{
               {"support", QStringLiteral("XY")},
               {"lines",
                QJsonArray{QJsonObject{{"x1", 0.0}, {"y1", 0.0},
                                       {"x2", 10.0}, {"y2", 0.0},
                                       {"elementId", 1},
                                       {"dashed", false}}}},
               {"circles",
                QJsonArray{QJsonObject{{"x", 20.0}, {"y", 0.0},
                                       {"radius", 2.0},
                                       {"dashed", false}}}},
               {"dimensions",
                QJsonArray{QJsonObject{{"kind", 0},
                                       {"geometryIndex", 0},
                                       {"firstLine", 0},
                                       {"firstStart", true},
                                       {"secondLine", 0},
                                       {"secondStart", true},
                                       {"value", 10.0},
                                       {"offset", 4.0},
                                       {"angle", 0.0}},
                           QJsonObject{{"kind", 1},
                                       {"geometryIndex", 0},
                                       {"firstLine", 0},
                                       {"firstStart", true},
                                       {"secondLine", 0},
                                       {"secondStart", false},
                                       {"value", 10.0},
                                       {"offset", 4.0},
                                       {"angle", 0.0}},
                           QJsonObject{{"kind", 2},
                                       {"geometryIndex", 0},
                                       {"firstLine", 0},
                                       {"firstStart", true},
                                       {"secondLine", 0},
                                       {"secondStart", true},
                                       {"value", 4.0},
                                       {"offset", 4.0},
                                       {"angle", 0.0}}}}}}},
          {"extrusion", QJsonObject{{"enabled", false}}}};
      const QString bdbPath = directory.filePath("literal-bdb9530.solidar");
      QFile bdbFile(bdbPath);
      CHECK(bdbFile.open(QIODevice::WriteOnly));
      const QByteArray bytes =
          QJsonDocument(bdbRoot).toJson(QJsonDocument::Compact);
      CHECK(bdbFile.write(bytes) == bytes.size());
      bdbFile.close();
      const auto bdb = solidar::project::ProjectFile::stageLoad(bdbPath);
      CHECK(bdb.kind == solidar::project::ProjectLoadKind::ValidV1);
      CHECK(bdb.legacy.sketches.size() == 1);
      CHECK(bdb.legacy.sketches[0].geometry.dimensions().size() == 3);
      const auto& migratedDimensions =
          bdb.legacy.sketches[0].geometry.dimensions();
      CHECK(migratedDimensions[0].id != solidar::sketch::kInvalidDimensionId);
      CHECK(migratedDimensions[1].id != migratedDimensions[0].id);
      CHECK(migratedDimensions[2].id != migratedDimensions[0].id);
      CHECK(migratedDimensions[2].id != migratedDimensions[1].id);
      const auto& dimension =
          bdb.legacy.sketches[0].geometry.dimensions()[1];
      CHECK(dimension.kind == solidar::sketch::DimensionKind::PointDistance);
      CHECK(dimension.firstPoint.lineId ==
            bdb.legacy.sketches[0].geometry.lineId(0));
      CHECK(dimension.secondPoint.lineId ==
            bdb.legacy.sketches[0].geometry.lineId(0));
      CHECK(bdb.legacy.sketches[0].geometry.dimensions()[0].kind ==
            solidar::sketch::DimensionKind::LineLength);
      CHECK(bdb.legacy.sketches[0].geometry.dimensions()[2].kind ==
            solidar::sketch::DimensionKind::CircleDiameter);
    }

    // Literal first-v2 shape from bf8934d: no body.visible, sourceKind,
    // arcs or later point-reference channel groups.
    {
      const auto line = [](double x1, double y1, double x2, double y2,
                           int elementId) {
        return QJsonObject{{"x1", x1}, {"y1", y1}, {"x2", x2},
                           {"y2", y2}, {"elementId", elementId},
                           {"dashed", false}};
      };
      const QJsonObject bfRoot{
          {"format", QStringLiteral("solidar-project")},
          {"version", 2},
          {"name", QStringLiteral("bf8934d")},
          {"createdAt", QStringLiteral("2026-08-28T00:00:00Z")},
          {"document", QJsonObject{{"widthMm", 60.0},
                                    {"heightMm", 40.0},
                                    {"extrusionMm", 25.0}}},
          {"sketches",
           QJsonArray{QJsonObject{
               {"support", QStringLiteral("XY")},
               {"lines", QJsonArray{line(0, 0, 10, 0, 1),
                                     line(10, 0, 10, 10, 2),
                                     line(10, 10, 0, 10, 3),
                                     line(0, 10, 0, 0, 4)}},
               {"circles", QJsonArray{}},
               {"dimensions", QJsonArray{}},
               {"constraints", QJsonArray{}},
               {"centerNodeElementIds", QJsonArray{}}}}},
          {"extrusion", QJsonObject{{"enabled", false}}},
          {"model",
           QJsonObject{
               {"sketches",
                QJsonArray{QJsonObject{
                    {"id", 501}, {"name", QStringLiteral("Profile")},
                    {"origin", QJsonArray{0.0, 0.0, 0.0}},
                    {"xDirection", QJsonArray{1.0, 0.0, 0.0}},
                    {"yDirection", QJsonArray{0.0, 1.0, 0.0}},
                    {"support", QJsonObject{{"type", 0}}}}}},
               {"bodies",
                QJsonArray{QJsonObject{
                    {"id", 601}, {"name", QStringLiteral("Body")},
                    {"features",
                     QJsonArray{QJsonObject{
                         {"id", 701},
                         {"name", QStringLiteral("Extrude")},
                         {"type", QStringLiteral("Extrude")},
                         {"sketchId", 501}, {"lengthMm", 10.0},
                         {"operation", 0}, {"reversed", false}}}}}}}}}};
      const QString bfPath = directory.filePath("literal-bf8934d.solidar");
      QFile bfFile(bfPath);
      CHECK(bfFile.open(QIODevice::WriteOnly));
      const QByteArray bytes =
          QJsonDocument(bfRoot).toJson(QJsonDocument::Compact);
      CHECK(bfFile.write(bytes) == bytes.size());
      bfFile.close();
      const auto bf = solidar::project::ProjectFile::stageLoad(bfPath);
      CHECK(bf.kind == solidar::project::ProjectLoadKind::ValidV2);
      CHECK(bf.document.has_value());
      CHECK(bf.document->bodies().size() == 1);
      CHECK(bf.document->bodies()[0].visible());
      CHECK(bf.document->bodies()[0].resultShape());
      CHECK(!bf.document->bodies()[0].resultShape()->IsNull());
    }

    solidar::project::ProjectData legacy;
    legacy.box = {12.0, 13.0, 14.0};
    solidar::project::SavedSketch saved;
    saved.geometry.addRectangle({0.0, 0.0}, {3.0, 4.0});
    saved.geometry.addCircle({8.0, 6.0}, 2.5);
    solidar::sketch::Dimension diameter;
    diameter.kind = solidar::sketch::DimensionKind::CircleDiameter;
    diameter.geometryId = saved.geometry.circleId(0);
    diameter.valueMm = 5.0;
    diameter.offsetMm = 2.0;
    saved.geometry.storeDimension(diameter);
    solidar::sketch::Dimension pointDistance;
    pointDistance.kind = solidar::sketch::DimensionKind::PointDistance;
    pointDistance.firstPoint = {saved.geometry.lineId(0), true};
    pointDistance.secondPoint = {saved.geometry.lineId(0), false};
    pointDistance.valueMm = 3.0;
    pointDistance.offsetMm = 1.0;
    saved.geometry.storeDimension(pointDistance);
    solidar::sketch::Constraint horizontal;
    horizontal.id = 42;
    horizontal.type = solidar::sketch::ConstraintType::Horizontal;
    horizontal.firstGeometry = saved.geometry.lineId(0);
    CHECK(saved.geometry.restoreConstraints({horizontal}));
    CHECK(saved.geometry.constraints().size() == 1);
    legacy.sketches.push_back(saved);
    CHECK(legacy.sketches[0].geometry.constraints().size() == 1);
    const QString legacyPath = directory.filePath("legacy-v1.solidar");
    CHECK(solidar::project::ProjectFile::save(legacyPath, legacy, &error));
    QFile serializedLegacy(legacyPath);
    CHECK(serializedLegacy.open(QIODevice::ReadOnly));
    const auto serializedLegacyRoot =
        QJsonDocument::fromJson(serializedLegacy.readAll()).object();
    CHECK(serializedLegacyRoot.value("sketches").toArray()[0]
              .toObject().value("constraints").toArray().size() == 1);
    auto staged = solidar::project::ProjectFile::stageLoad(legacyPath);
    CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(staged.succeeded());
    CHECK(!staged.document.has_value());
    CHECK(staged.legacy.sketches.size() == 1);
    CHECK(staged.legacy.sketches[0].geometry.dimensions().size() == 2);
    CHECK(staged.legacy.sketches[0].geometry.dimensions()[0].kind ==
          solidar::sketch::DimensionKind::CircleDiameter);
    CHECK(staged.legacy.sketches[0].geometry.constraints().size() == 1);

    QFile legacyFile(legacyPath);
    CHECK(legacyFile.open(QIODevice::ReadOnly));
    auto historicalRoot =
        QJsonDocument::fromJson(legacyFile.readAll()).object();
    historicalRoot.remove("constraintTypeEncoding");
    auto historicalSketches = historicalRoot.value("sketches").toArray();
    auto historicalSketch = historicalSketches[0].toObject();
    historicalSketch.remove("centerNodeElementIds");
    auto historicalDimensions = historicalSketch.value("dimensions").toArray();
    auto historicalPointDimension = historicalDimensions[1].toObject();
    CHECK(historicalPointDimension.value("kind").toInt() ==
          static_cast<int>(solidar::sketch::DimensionKind::PointDistance));
    historicalPointDimension.remove("firstOrigin");
    historicalPointDimension.remove("secondOrigin");
    historicalPointDimension.remove("firstCircle");
    historicalPointDimension.remove("secondCircle");
    historicalPointDimension.remove("firstArc");
    historicalPointDimension.remove("secondArc");
    historicalPointDimension.remove("firstElementCenter");
    historicalPointDimension.remove("secondElementCenter");
    historicalDimensions[1] = historicalPointDimension;
    historicalSketch["dimensions"] = historicalDimensions;
    auto historicalConstraints = historicalSketch.value("constraints").toArray();
    auto historicalConstraint = historicalConstraints[0].toObject();
    historicalConstraint.remove("typeKey");
    historicalConstraint.remove("firstPointOrigin");
    historicalConstraint.remove("secondPointOrigin");
    historicalConstraint.remove("firstPointElementCenter");
    historicalConstraint.remove("secondPointElementCenter");
    historicalConstraints[0] = historicalConstraint;
    historicalSketch["constraints"] = historicalConstraints;
    historicalSketches[0] = historicalSketch;
    historicalRoot["sketches"] = historicalSketches;
    const QString historicalPath =
        directory.filePath("historical-v1-optional-fields.solidar");
    QFile historicalFile(historicalPath);
    CHECK(historicalFile.open(QIODevice::WriteOnly));
    const QByteArray historicalBytes =
        QJsonDocument(historicalRoot).toJson(QJsonDocument::Compact);
    CHECK(historicalFile.write(historicalBytes) == historicalBytes.size());
    historicalFile.close();
    auto historical =
        solidar::project::ProjectFile::stageLoad(historicalPath);
    CHECK(historical.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(historical.legacy.sketches.size() == 1);
    const auto& historicalGeometry = historical.legacy.sketches[0].geometry;
    CHECK(historicalGeometry.dimensions().size() == 2);
    const auto& restoredPointDistance = historicalGeometry.dimensions()[1];
    CHECK(restoredPointDistance.kind ==
          solidar::sketch::DimensionKind::PointDistance);
    CHECK(!restoredPointDistance.firstPoint.origin);
    CHECK(!restoredPointDistance.secondPoint.origin);
    CHECK(restoredPointDistance.firstPoint.lineId ==
          historicalGeometry.lineId(0));
    CHECK(restoredPointDistance.firstPoint.start);
    CHECK(restoredPointDistance.secondPoint.lineId ==
          historicalGeometry.lineId(0));
    CHECK(!restoredPointDistance.secondPoint.start);
    CHECK(solidar::test::near(restoredPointDistance.valueMm, 3.0));
    CHECK(historical.legacy.sketches[0].geometry.constraints().size() == 1);
    CHECK(!historical.legacy.sketches[0]
               .geometry.constraints()[0].firstPoint.origin);
    CHECK(!historical.legacy.sketches[0]
               .geometry.constraints()[0].secondPoint.origin);
    CHECK(historical.legacy.sketches[0]
              .geometry.constraints()[0].firstPoint.elementCenterId == 0);
    CHECK(historical.legacy.sketches[0]
              .geometry.constraints()[0].secondPoint.elementCenterId == 0);
  }

  // Historical files can preserve constraint vector order independently of
  // numeric IDs.  Deleting their shared geometry must produce a delta that is
  // reversible regardless of that order, remain editable, and round-trip in
  // both project generations.
  {
    auto orderedGeometry = []() -> std::optional<solidar::sketch::Sketch> {
      solidar::sketch::Sketch geometry;
      geometry.addLine({0.0, 0.0}, {10.0, 0.0});
      geometry.addLine({100.0, 0.0}, {120.0, 0.0});
      geometry.addLine({200.0, 0.0}, {230.0, 0.0});
      const auto lineId = geometry.lineId(0);
      solidar::sketch::Constraint first;
      first.id = 20;
      first.type = solidar::sketch::ConstraintType::Equal;
      first.firstGeometry = geometry.lineId(1);
      first.secondGeometry = geometry.lineId(2);
      solidar::sketch::Constraint second;
      second.id = 10;
      second.type = solidar::sketch::ConstraintType::Equal;
      second.firstGeometry = lineId;
      second.secondGeometry = geometry.lineId(2);
      if (!geometry.restoreConstraints({first, second})) return std::nullopt;
      for (int index = 0; index < 3; ++index) {
        solidar::sketch::Dimension dimension;
        dimension.kind = solidar::sketch::DimensionKind::LineLength;
        dimension.geometryId = lineId;
        dimension.valueMm = 20.0 + index;
        dimension.offsetMm = 3.0 + index;
        geometry.storeDimension(dimension);
      }
      return geometry;
    };
    const auto stripHistoricalPointFields = [](QJsonObject* root,
                                                bool earlyEncoding) {
      if (earlyEncoding) root->remove("constraintTypeEncoding");
      auto sketches = root->value("sketches").toArray();
      if (sketches.isEmpty()) return false;
      auto sketch = sketches[0].toObject();
      auto constraints = sketch.value("constraints").toArray();
      if (constraints.size() != 2 ||
          constraints[0].toObject().value("id").toInteger() != 20 ||
          constraints[1].toObject().value("id").toInteger() != 10)
        return false;
      for (qsizetype index = 0; index < constraints.size(); ++index) {
        auto constraint = constraints[index].toObject();
        if (earlyEncoding) constraint.remove("typeKey");
        constraint.remove("firstPointOrigin");
        constraint.remove("secondPointOrigin");
        constraint.remove("firstPointElementCenter");
        constraint.remove("secondPointElementCenter");
        constraints[index] = constraint;
      }
      sketch["constraints"] = constraints;
      if (earlyEncoding) sketch.remove("centerNodeElementIds");
      sketches[0] = sketch;
      (*root)["sketches"] = sketches;
      return true;
    };
    const auto exerciseDelta = [](solidar::sketch::Sketch* geometry) {
      if (!geometry || geometry->constraints().size() != 2 ||
          geometry->constraints()[0].id != 20 ||
          geometry->constraints()[1].id != 10)
        return false;
      for (const auto& line : geometry->lines())
        if (!solidar::test::near(
                std::hypot(line.end.xMm - line.start.xMm,
                           line.end.yMm - line.start.yMm),
                20.0))
          return false;
      const auto original = geometry->semanticFingerprint();
      geometry->beginDeltaJournal();
      geometry->removeLine(2);  // Shared operand of constraints [20, 10].
      auto delta = geometry->finishDeltaJournal();
      if (geometry->lines().size() != 2 || !geometry->constraints().empty() ||
          !geometry->applyDelta(delta, false) ||
          geometry->semanticFingerprint() != original ||
          geometry->lines().size() != 3 ||
          !geometry->applyDelta(delta, true) ||
          geometry->lines().size() != 2 ||
          !geometry->applyDelta(delta, false) ||
          geometry->lines().size() != 3 ||
          geometry->constraints().size() != 2 ||
          geometry->constraints()[0].id != 20 ||
          geometry->constraints()[1].id != 10)
        return false;
      const auto beforeDimensionDelete = geometry->semanticFingerprint();
      geometry->beginDeltaJournal();
      if (!geometry->removeDimension(1)) return false;
      const auto dimensionDelete = geometry->finishDeltaJournal();
      const auto afterDimensionDelete = geometry->semanticFingerprint();
      for (int cycle = 0; cycle < 2; ++cycle) {
        if (!geometry->applyDelta(dimensionDelete, false) ||
            geometry->semanticFingerprint() != beforeDimensionDelete ||
            geometry->dimensions().size() != 3 ||
            !geometry->applyDelta(dimensionDelete, true) ||
            geometry->semanticFingerprint() != afterDimensionDelete ||
            geometry->dimensions().size() != 2)
          return false;
      }
      geometry->beginDeltaJournal();
      geometry->setLineDashedById(geometry->lineId(0), true);
      const auto edit = geometry->finishDeltaJournal();
      return !edit.empty() && geometry->lines()[0].dashed;
    };

    solidar::project::ProjectData v1;
    const auto v1Geometry = orderedGeometry();
    CHECK(v1Geometry.has_value());
    v1.sketches.push_back({*v1Geometry, QStringLiteral("XY")});
    const QString v1Seed = directory.filePath("ordered-constraints-v1-seed.solidar");
    CHECK(solidar::project::ProjectFile::save(v1Seed, v1, &error));
    QFile v1SeedFile(v1Seed);
    CHECK(v1SeedFile.open(QIODevice::ReadOnly));
    auto v1Root = QJsonDocument::fromJson(v1SeedFile.readAll()).object();
    CHECK(stripHistoricalPointFields(&v1Root, true));
    const QString v1Historical =
        directory.filePath("ordered-constraints-historical-v1.solidar");
    QFile v1HistoricalFile(v1Historical);
    CHECK(v1HistoricalFile.open(QIODevice::WriteOnly));
    CHECK(v1HistoricalFile.write(
              QJsonDocument(v1Root).toJson(QJsonDocument::Compact)) > 0);
    v1HistoricalFile.close();
    auto loadedV1 = solidar::project::ProjectFile::stageLoad(v1Historical);
    CHECK(loadedV1.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(exerciseDelta(&loadedV1.legacy.sketches[0].geometry));
    const QString v1Edited = directory.filePath("ordered-constraints-v1-edited.solidar");
    CHECK(solidar::project::ProjectFile::save(v1Edited, loadedV1.legacy,
                                               &error));
    solidar::project::ProjectData reloadedV1;
    CHECK(solidar::project::ProjectFile::load(v1Edited, &reloadedV1, &error));
    CHECK(reloadedV1.sketches[0].geometry.lines()[0].dashed);
    CHECK(reloadedV1.sketches[0].geometry.constraints()[0].id == 20);
    CHECK(reloadedV1.sketches[0].geometry.constraints()[1].id == 10);
    CHECK(reloadedV1.sketches[0].geometry.dimensions().size() == 2);
    CHECK(solidar::test::near(
        reloadedV1.sketches[0].geometry.dimensions()[0].valueMm, 20.0));
    CHECK(solidar::test::near(
        reloadedV1.sketches[0].geometry.dimensions()[1].valueMm, 22.0));

    solidar::Document v2;
    auto& v2Sketch = v2.addSketch("Ordered constraints");
    const auto v2Geometry = orderedGeometry();
    CHECK(v2Geometry.has_value());
    v2Sketch.geometry = *v2Geometry;
    const QString v2Seed = directory.filePath("ordered-constraints-v2-seed.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(v2Seed, v2, &error));
    QFile v2SeedFile(v2Seed);
    CHECK(v2SeedFile.open(QIODevice::ReadOnly));
    auto v2Root = QJsonDocument::fromJson(v2SeedFile.readAll()).object();
    CHECK(stripHistoricalPointFields(&v2Root, false));
    const QString v2Historical =
        directory.filePath("ordered-constraints-historical-v2.solidar");
    QFile v2HistoricalFile(v2Historical);
    CHECK(v2HistoricalFile.open(QIODevice::WriteOnly));
    CHECK(v2HistoricalFile.write(
              QJsonDocument(v2Root).toJson(QJsonDocument::Compact)) > 0);
    v2HistoricalFile.close();
    auto loadedV2 = solidar::project::ProjectFile::stageLoad(v2Historical);
    if (loadedV2.kind != solidar::project::ProjectLoadKind::ValidV2)
      std::cerr << "historical ordered v2: "
                << loadedV2.error.toStdString() << '\n';
    CHECK(loadedV2.kind == solidar::project::ProjectLoadKind::ValidV2);
    CHECK(loadedV2.document.has_value());
    auto* loadedV2Sketch = loadedV2.document->sketchAt(0);
    CHECK(loadedV2Sketch != nullptr);
    CHECK(exerciseDelta(&loadedV2Sketch->geometry));
    const QString v2Edited = directory.filePath("ordered-constraints-v2-edited.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        v2Edited, *loadedV2.document, &error));
    solidar::Document reloadedV2;
    CHECK(solidar::project::ProjectFile::loadDocument(v2Edited, &reloadedV2,
                                                       &error));
    CHECK(reloadedV2.sketches()[0].geometry.lines()[0].dashed);
    CHECK(reloadedV2.sketches()[0].geometry.constraints()[0].id == 20);
    CHECK(reloadedV2.sketches()[0].geometry.constraints()[1].id == 10);
    CHECK(reloadedV2.sketches()[0].geometry.dimensions().size() == 2);
    CHECK(solidar::test::near(
        reloadedV2.sketches()[0].geometry.dimensions()[0].valueMm, 20.0));
    CHECK(solidar::test::near(
        reloadedV2.sketches()[0].geometry.dimensions()[1].valueMm, 22.0));
  }

  // Every PointDistance source channel and per-line dashed state survives
  // both v1 and v2 round-trips. Lines sharing one composite element must not
  // inherit each other's construction state during load.
  {
    solidar::sketch::Sketch geometry;
    geometry.clear();
    constexpr std::size_t kCenteredElement = 900;
    geometry.addLine({0.0, 0.0}, {10.0, 0.0}, kCenteredElement);
    geometry.addLine({10.0, 0.0}, {10.0, 10.0}, kCenteredElement);
    geometry.addLine({10.0, 10.0}, {0.0, 10.0}, kCenteredElement);
    geometry.addLine({0.0, 10.0}, {0.0, 0.0}, kCenteredElement);
    geometry.markElementCenterNode(kCenteredElement);
    geometry.addLine({20.0, 0.0}, {30.0, 5.0});
    geometry.addCircle({40.0, 10.0}, 3.0);
    geometry.addArc({50.0, 10.0}, 4.0, 0.0,
                    3.14159265358979323846);
    geometry.setLineDashedById(geometry.lineId(0), true);
    geometry.setLineDashedById(geometry.lineId(2), true);

    solidar::sketch::Dimension originToLine;
    originToLine.kind = solidar::sketch::DimensionKind::PointDistance;
    originToLine.firstPoint.origin = true;
    originToLine.secondPoint.lineId = geometry.lineId(4);
    originToLine.secondPoint.start = false;
    originToLine.valueMm = 30.4138126515;
    geometry.storeDimension(originToLine);

    solidar::sketch::Dimension circleToArc;
    circleToArc.kind = solidar::sketch::DimensionKind::PointDistance;
    circleToArc.firstPoint.circleId = geometry.circleId(0);
    circleToArc.secondPoint.arcId = geometry.arcId(0);
    circleToArc.secondPoint.start = true;
    circleToArc.valueMm = 6.0;
    geometry.storeDimension(circleToArc);

    solidar::sketch::Dimension centerToLine;
    centerToLine.kind = solidar::sketch::DimensionKind::PointDistance;
    centerToLine.firstPoint.elementCenterId = kCenteredElement;
    centerToLine.secondPoint.lineId = geometry.lineId(4);
    centerToLine.secondPoint.start = true;
    centerToLine.valueMm = std::hypot(15.0, 5.0);
    geometry.storeDimension(centerToLine);

    const auto checkChannels = [&](const solidar::sketch::Sketch& restored) {
      if (restored.lines().size() != 5 ||
          !restored.lines()[0].dashed || restored.lines()[1].dashed ||
          !restored.lines()[2].dashed || restored.lines()[3].dashed ||
          !restored.hasElementCenterNode(kCenteredElement) ||
          restored.dimensions().size() != 3)
        return false;
      const auto& first = restored.dimensions()[0];
      const auto& second = restored.dimensions()[1];
      const auto& third = restored.dimensions()[2];
      return first.firstPoint.origin &&
             first.secondPoint.lineId == restored.lineId(4) &&
             second.firstPoint.circleId == restored.circleId(0) &&
             second.secondPoint.arcId == restored.arcId(0) &&
             third.firstPoint.elementCenterId == kCenteredElement &&
             third.secondPoint.lineId == restored.lineId(4);
    };

    solidar::project::ProjectData channelData;
    channelData.sketches.push_back({geometry, QStringLiteral("XY")});
    const QString channelsV1 = directory.filePath("point-channels-v1.solidar");
    CHECK(solidar::project::ProjectFile::save(channelsV1, channelData, &error));
    const auto stagedV1 =
        solidar::project::ProjectFile::stageLoad(channelsV1);
    CHECK(stagedV1.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(checkChannels(stagedV1.legacy.sketches[0].geometry));

    solidar::Document channelDocument;
    auto& channelSketch = channelDocument.addSketch("Channels");
    channelSketch.geometry = geometry;
    const QString channelsV2 = directory.filePath("point-channels-v2.solidar");
    CHECK(solidar::project::ProjectFile::saveDocument(
        channelsV2, channelDocument, &error));
    solidar::Document restoredChannels;
    CHECK(solidar::project::ProjectFile::loadDocument(
        channelsV2, &restoredChannels, &error));
    CHECK(restoredChannels.sketches().size() == 1);
    CHECK(checkChannels(restoredChannels.sketches()[0].geometry));
  }

  // Malformed v2 inputs are rejected with diagnostics before a destination
  // Document can be changed. Each mutation starts from a known-good v2 file.
  {
    QFile sourceFile(modelPath);
    CHECK(sourceFile.open(QIODevice::ReadOnly));
    const auto parsed = QJsonDocument::fromJson(sourceFile.readAll());
    CHECK(parsed.isObject());
    const QJsonObject validRoot = parsed.object();

    {
      auto historicalRoot = validRoot;
      historicalRoot.remove("constraintTypeEncoding");
      auto model = historicalRoot.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      bodyObject.remove("visible");
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      historicalRoot["model"] = model;

      auto sketches = historicalRoot.value("sketches").toArray();
      auto sketchObject = sketches[0].toObject();
      auto dimensions = sketchObject.value("dimensions").toArray();
      auto pointDimension = dimensions[1].toObject();
      CHECK(pointDimension.value("kind").toInt() ==
            static_cast<int>(solidar::sketch::DimensionKind::PointDistance));
      pointDimension.remove("firstOrigin");
      pointDimension.remove("secondOrigin");
      pointDimension.remove("firstCircle");
      pointDimension.remove("secondCircle");
      pointDimension.remove("firstArc");
      pointDimension.remove("secondArc");
      pointDimension.remove("firstElementCenter");
      pointDimension.remove("secondElementCenter");
      dimensions[1] = pointDimension;
      for (qsizetype dimensionIndex = 0;
           dimensionIndex < dimensions.size(); ++dimensionIndex) {
        auto historicalDimension = dimensions[dimensionIndex].toObject();
        historicalDimension.remove("id");
        dimensions[dimensionIndex] = historicalDimension;
      }
      sketchObject["dimensions"] = dimensions;
      auto constraints = sketchObject.value("constraints").toArray();
      auto constraint = constraints[0].toObject();
      constraint.remove("typeKey");
      constraint.remove("firstPointOrigin");
      constraint.remove("secondPointOrigin");
      constraints[0] = constraint;
      sketchObject["constraints"] = constraints;
      sketches[0] = sketchObject;
      for (qsizetype sketchIndex = 0; sketchIndex < sketches.size();
           ++sketchIndex) {
        auto oldSketch = sketches[sketchIndex].toObject();
        auto oldConstraints = oldSketch.value("constraints").toArray();
        for (qsizetype constraintIndex = 0;
             constraintIndex < oldConstraints.size(); ++constraintIndex) {
          auto oldConstraint = oldConstraints[constraintIndex].toObject();
          oldConstraint.remove("typeKey");
          oldConstraints[constraintIndex] = oldConstraint;
        }
        oldSketch["constraints"] = oldConstraints;
        sketches[sketchIndex] = oldSketch;
      }
      historicalRoot["sketches"] = sketches;

      const QString historicalPath =
          directory.filePath("historical-v2-optional-fields.solidar");
      QFile historicalFile(historicalPath);
      CHECK(historicalFile.open(QIODevice::WriteOnly));
      const QByteArray bytes =
          QJsonDocument(historicalRoot).toJson(QJsonDocument::Compact);
      CHECK(historicalFile.write(bytes) == bytes.size());
      historicalFile.close();
      auto historical =
          solidar::project::ProjectFile::stageLoad(historicalPath);
      CHECK(historical.kind == solidar::project::ProjectLoadKind::ValidV2);
      CHECK(historical.document.has_value());
      CHECK(historical.document->bodies().size() == 1);
      CHECK(historical.document->bodies()[0].visible());
      CHECK(historical.document->sketches()[0].geometry.constraints().size() ==
            1);
      const auto& historicalGeometry =
          historical.document->sketches()[0].geometry;
      CHECK(historicalGeometry.dimensions().size() == 2);
      CHECK(historicalGeometry.dimensions()[0].id !=
            solidar::sketch::kInvalidDimensionId);
      CHECK(historicalGeometry.dimensions()[1].id !=
            historicalGeometry.dimensions()[0].id);
      const auto& restoredPointDistance = historicalGeometry.dimensions()[1];
      CHECK(restoredPointDistance.kind ==
            solidar::sketch::DimensionKind::PointDistance);
      CHECK(!restoredPointDistance.firstPoint.origin);
      CHECK(!restoredPointDistance.secondPoint.origin);
      CHECK(restoredPointDistance.firstPoint.lineId ==
            historicalGeometry.lineId(0));
      CHECK(restoredPointDistance.firstPoint.start);
      CHECK(restoredPointDistance.secondPoint.lineId ==
            historicalGeometry.lineId(0));
      CHECK(!restoredPointDistance.secondPoint.start);
      CHECK(solidar::test::near(restoredPointDistance.valueMm, 100.0));
      CHECK(!historical.document->sketches()[0]
                 .geometry.constraints()[0].firstPoint.origin);
      CHECK(!historical.document->sketches()[0]
                 .geometry.constraints()[0].secondPoint.origin);
      CHECK(historical.document->sketches()[0]
                .geometry.constraints()[0].firstPoint.elementCenterId == 0);
      CHECK(historical.document->sketches()[0]
                .geometry.constraints()[0].secondPoint.elementCenterId == 0);
    }

    {
      auto unsupportedRoot = validRoot;
      unsupportedRoot["version"] = 3;
      const QString unsupportedPath =
          directory.filePath("unsupported-version.solidar");
      QFile unsupportedFile(unsupportedPath);
      CHECK(unsupportedFile.open(QIODevice::WriteOnly));
      const QByteArray bytes =
          QJsonDocument(unsupportedRoot).toJson(QJsonDocument::Compact);
      CHECK(unsupportedFile.write(bytes) == bytes.size());
      unsupportedFile.close();
      const auto unsupported =
          solidar::project::ProjectFile::stageLoad(unsupportedPath);
      CHECK(unsupported.kind ==
            solidar::project::ProjectLoadKind::Unsupported);
      CHECK(!unsupported.error.isEmpty());
    }

    auto writeRoot = [&](const QString& name, const QJsonObject& root) {
      const QString filePath = directory.filePath(name);
      QFile file(filePath);
      if (!file.open(QIODevice::WriteOnly)) return QString{};
      const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
      if (file.write(payload) != payload.size()) return QString{};
      file.close();
      return filePath;
    };
    {
      // First-v2 topology references were index-only. They remain valid when
      // their referenced B-Rep can be rebuilt, while the stable writer may add
      // persistent tags and signatures for stronger matching.
      auto root = validRoot;
      root.remove("constraintTypeEncoding");
      auto rootSketches = root.value("sketches").toArray();
      for (qsizetype sketchIndex = 0; sketchIndex < rootSketches.size();
           ++sketchIndex) {
        auto sketch = rootSketches[sketchIndex].toObject();
        auto constraints = sketch.value("constraints").toArray();
        for (qsizetype constraintIndex = 0;
             constraintIndex < constraints.size(); ++constraintIndex) {
          auto constraint = constraints[constraintIndex].toObject();
          constraint.remove("typeKey");
          constraints[constraintIndex] = constraint;
        }
        sketch["constraints"] = constraints;
        rootSketches[sketchIndex] = sketch;
      }
      root["sketches"] = rootSketches;

      auto model = root.value("model").toObject();
      auto sketches = model.value("sketches").toArray();
      for (qsizetype index = 0; index < sketches.size(); ++index) {
        auto sketch = sketches[index].toObject();
        auto support = sketch.value("support").toObject();
        if (support.value("type").toInt() ==
            static_cast<int>(solidar::SketchSupportType::Face)) {
          support.remove("persistentTag");
          support.remove("signature");
          sketch["support"] = support;
          sketches[index] = sketch;
        }
      }
      model["sketches"] = sketches;
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      for (qsizetype featureIndex = 0; featureIndex < features.size();
           ++featureIndex) {
        auto feature = features[featureIndex].toObject();
        if (feature.value("type").toString() != QStringLiteral("Fillet"))
          continue;
        auto edges = feature.value("edges").toArray();
        for (qsizetype edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
          auto edge = edges[edgeIndex].toObject();
          edge.remove("persistentTag");
          edge.remove("signature");
          edges[edgeIndex] = edge;
        }
        feature["edges"] = edges;
        features[featureIndex] = feature;
      }
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;

      const QString path =
          writeRoot("historical-index-only-topology.solidar", root);
      CHECK(!path.isEmpty());
      const auto staged = solidar::project::ProjectFile::stageLoad(path);
      if (staged.kind != solidar::project::ProjectLoadKind::ValidV2)
        std::fprintf(stderr, "historical topology stage failed: %s\n",
                     staged.error.toUtf8().constData());
      CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV2);
      CHECK(staged.document.has_value());
      CHECK(staged.document->bodies().size() == 1);
      CHECK(staged.document->bodies()[0].features().size() == 3);
      CHECK(staged.document->bodies()[0].features()[2]->isValid());
    }
    auto historicalJoinPattern = [&](const QString& fileName,
                                     const QString& patternType) -> bool {
      auto root = validRoot;
      root.remove("constraintTypeEncoding");
      auto rootSketches = root.value("sketches").toArray();
      for (qsizetype sketchIndex = 0; sketchIndex < rootSketches.size();
           ++sketchIndex) {
        auto sketch = rootSketches[sketchIndex].toObject();
        auto constraints = sketch.value("constraints").toArray();
        for (qsizetype constraintIndex = 0;
             constraintIndex < constraints.size(); ++constraintIndex) {
          auto constraint = constraints[constraintIndex].toObject();
          constraint.remove("typeKey");
          constraints[constraintIndex] = constraint;
        }
        sketch["constraints"] = constraints;
        rootSketches[sketchIndex] = sketch;
      }
      root["sketches"] = rootSketches;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      if (features.size() < 3) return false;
      auto pattern = features[2].toObject();
      pattern["type"] = patternType;
      pattern["sourceFeatureId"] =
          features[1].toObject().value("id");
      pattern.remove("sourceBodyId");
      pattern.remove("operation");
      pattern["count"] = 3;
      if (patternType == QStringLiteral("LinearPattern")) {
        pattern["direction"] = 0;
        pattern["spacingMm"] = 100.0;
      } else {
        pattern["axis"] = 2;
        pattern["angleDeg"] = 360.0;
      }
      features[2] = pattern;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;

      const QString path = writeRoot(fileName, root);
      if (path.isEmpty()) return false;
      auto staged = solidar::project::ProjectFile::stageLoad(path);
      if (staged.kind != solidar::project::ProjectLoadKind::ValidV2 ||
          !staged.document || staged.document->bodies().size() != 1) {
        std::fprintf(stderr, "historical pattern stage failed: %s\n",
                     staged.error.toUtf8().constData());
        return false;
      }
      auto& restoredBody = staged.document->bodies()[0];
      if (restoredBody.features().size() != 3 ||
          restoredBody.features()[2]->kind() !=
              (patternType == QStringLiteral("LinearPattern")
                   ? solidar::FeatureKind::LinearPattern
                   : solidar::FeatureKind::CircularPattern) ||
          !staged.document->recompute()) {
        std::fprintf(stderr, "historical pattern rebuild failed: %s\n",
                     staged.document->rebuildError().c_str());
        return false;
      }
      const auto expectedSource = restoredBody.features()[1]->id();
      if (patternType == QStringLiteral("LinearPattern")) {
        const auto* restored =
            dynamic_cast<const solidar::LinearPatternFeature*>(
                restoredBody.features()[2].get());
        return restored &&
               restored->operation() == solidar::PatternOperation::Join &&
               restored->sourceBodyId() == solidar::kInvalidBodyId &&
               restored->sourceFeatureId() == expectedSource &&
               restored->isValid() && restored->shape();
      }
      const auto* restored =
          dynamic_cast<const solidar::CircularPatternFeature*>(
              restoredBody.features()[2].get());
      return restored &&
             restored->operation() == solidar::PatternOperation::Join &&
             restored->sourceBodyId() == solidar::kInvalidBodyId &&
             restored->sourceFeatureId() == expectedSource &&
             restored->isValid() && restored->shape();
    };
    CHECK(historicalJoinPattern("historical-linear-pattern.solidar",
                                QStringLiteral("LinearPattern")));
    CHECK(historicalJoinPattern("historical-circular-pattern.solidar",
                                QStringLiteral("CircularPattern")));

    auto expectInvalid = [&](const QString& name, QJsonObject root) -> bool {
      const QString invalidPath = writeRoot(name, root);
      if (invalidPath.isEmpty()) return false;
      QFile beforeFile(invalidPath);
      if (!beforeFile.open(QIODevice::ReadOnly)) return false;
      const QByteArray beforeBytes = beforeFile.readAll();
      auto staged = solidar::project::ProjectFile::stageLoad(invalidPath);
      if (staged.kind != solidar::project::ProjectLoadKind::Invalid ||
          staged.succeeded() || staged.error.isEmpty())
        return false;
      if (solidar::project::ProjectFile::validate(invalidPath, &error) ||
          error.isEmpty())
        return false;

      solidar::Document destination;
      destination.setBox({91.0, 92.0, 93.0});
      auto& sentinel = destination.addSketch("sentinel");
      const auto sentinelId = sentinel.id;
      if (solidar::project::ProjectFile::loadDocument(
              invalidPath, &destination, &error) ||
          destination.box().widthMm != 91.0 ||
          destination.box().depthMm != 92.0 ||
          destination.box().heightMm != 93.0 ||
          !destination.findSketch(sentinelId))
        return false;
      QFile after(invalidPath);
      if (!after.open(QIODevice::ReadOnly) || after.readAll() != beforeBytes)
        return false;
      return true;
    };

    auto expectUnsupported = [&](const QString& name,
                                 const QJsonObject& root) -> bool {
      const QString unsupportedPath = writeRoot(name, root);
      if (unsupportedPath.isEmpty()) return false;
      const auto staged =
          solidar::project::ProjectFile::stageLoad(unsupportedPath);
      return staged.kind == solidar::project::ProjectLoadKind::Unsupported &&
             !staged.succeeded() && !staged.error.isEmpty();
    };

    {
      auto downgradedV2 = validRoot;
      downgradedV2["version"] = 1;
      CHECK(expectInvalid("v2-model-downgraded-to-v1.solidar",
                          downgradedV2));
    }

    auto makeConstraint = [](qint64 id, int type,
                             const QString& typeKey) {
      QJsonObject constraint{
          {"id", id},
          {"type", type},
          {"firstGeometryKind", QString{}},
          {"firstGeometry", -1},
          {"secondGeometryKind", QString{}},
          {"secondGeometry", -1},
          {"firstPointLine", -1},
          {"firstPointStart", true},
          {"firstPointCircle", -1},
          {"firstPointArc", -1},
          {"firstPointElementCenter", 0},
          {"firstPointOrigin", false},
          {"secondPointLine", -1},
          {"secondPointStart", false},
          {"secondPointCircle", -1},
          {"secondPointArc", -1},
          {"secondPointElementCenter", 0},
          {"secondPointOrigin", false},
          {"value", 0.0}};
      if (!typeKey.isEmpty()) constraint["typeKey"] = typeKey;
      return constraint;
    };

    auto applyCurrentConstraintShape = [](QJsonObject* constraint, int type) {
      auto firstLine = [&] {
        (*constraint)["firstGeometryKind"] = QStringLiteral("line");
        (*constraint)["firstGeometry"] = 0;
      };
      auto secondLine = [&] {
        (*constraint)["secondGeometryKind"] = QStringLiteral("line");
        (*constraint)["secondGeometry"] = 1;
      };
      auto firstCircle = [&] {
        (*constraint)["firstGeometryKind"] = QStringLiteral("circle");
        (*constraint)["firstGeometry"] = 0;
      };
      auto secondCircle = [&] {
        (*constraint)["secondGeometryKind"] = QStringLiteral("circle");
        (*constraint)["secondGeometry"] = 1;
      };
      auto firstPoint = [&] {
        (*constraint)["firstPointLine"] = 0;
        (*constraint)["firstPointStart"] = true;
      };
      auto secondPoint = [&] {
        (*constraint)["secondPointLine"] = 1;
        (*constraint)["secondPointStart"] = false;
      };
      switch (type) {
        case 0:
        case 1:
          firstLine();
          break;
        case 2:
          firstPoint();
          secondPoint();
          break;
        case 3:
          firstLine();
          secondPoint();
          break;
        case 4:
          firstPoint();
          secondPoint();
          (*constraint)["value"] = 5.0;
          break;
        case 5:
          firstLine();
          (*constraint)["value"] = 10.0;
          break;
        case 6:
          firstCircle();
          (*constraint)["value"] = 2.0;
          break;
        case 7:
          firstCircle();
          (*constraint)["value"] = 4.0;
          break;
        case 8:
        case 9:
          firstLine();
          secondLine();
          break;
        case 10:
          firstCircle();
          secondCircle();
          break;
        case 11:
          firstLine();
          secondLine();
          (*constraint)["value"] = 45.0;
          break;
        case 12:
        case 13:
          firstPoint();
          secondPoint();
          (*constraint)["value"] = 5.0;
          break;
        case 14:
          firstCircle();
          secondPoint();
          break;
        case 15:
          firstLine();
          (*constraint)["secondGeometryKind"] = QStringLiteral("circle");
          (*constraint)["secondGeometry"] = 0;
          break;
        case 16:
          firstLine();
          secondLine();
          (*constraint)["value"] = 5.0;
          break;
        case 17:
          firstLine();
          break;
        case 18:
          (*constraint)["firstGeometryKind"] = QStringLiteral("arc");
          (*constraint)["firstGeometry"] = 0;
          secondPoint();
          break;
        case 19:
          firstLine();
          secondPoint();
          break;
        case 20:
        case 21:
          secondPoint();
          break;
      }
    };

    auto makeRootSketch = [](const QJsonArray& constraints) {
      return QJsonObject{
          {"support", QStringLiteral("XY")},
          {"lines",
           QJsonArray{
               QJsonObject{{"x1", 0.0}, {"y1", 0.0}, {"x2", 10.0},
                           {"y2", 1.0}, {"elementId", 1}, {"dashed", false}},
               QJsonObject{{"x1", 0.0}, {"y1", 5.0}, {"x2", 10.0},
                           {"y2", 6.0}, {"elementId", 2}, {"dashed", false}}}},
          {"circles",
           QJsonArray{
               QJsonObject{{"x", 5.0}, {"y", 5.0}, {"radius", 2.0},
                           {"dashed", false}},
               QJsonObject{{"x", 15.0}, {"y", 5.0}, {"radius", 3.0},
                           {"dashed", false}}}},
          {"arcs",
           QJsonArray{QJsonObject{{"x", 5.0}, {"y", 5.0},
                                  {"radius", 2.0}, {"startAngle", 0.0},
                                  {"sweepAngle", 3.14159265358979323846},
                                  {"dashed", false}}}},
          {"dimensions", QJsonArray{}},
          {"constraints", constraints},
          {"centerNodeElementIds", QJsonArray{}}};
    };

    auto makeV1Root = [](const QJsonArray& sketches) {
      return QJsonObject{
          {"format", QStringLiteral("solidar-project")},
          {"version", 1},
          {"name", QStringLiteral("literal-history")},
          {"createdAt", QStringLiteral("2026-08-01T00:00:00Z")},
          {"document", QJsonObject{{"widthMm", 60.0},
                                    {"heightMm", 40.0},
                                    {"extrusionMm", 25.0}}},
          {"sketches", sketches},
          {"extrusion", QJsonObject{{"enabled", false}}}};
    };

    auto makeV2Root = [](QJsonObject root) {
      root["version"] = 2;
      QJsonArray metadata;
      const auto rootSketches = root.value("sketches").toArray();
      for (qsizetype index = 0; index < rootSketches.size(); ++index) {
        metadata.append(QJsonObject{
            {"id", static_cast<qint64>(index + 1)},
            {"name", QStringLiteral("Sketch %1").arg(index + 1)},
            {"origin", QJsonArray{0.0, 0.0, 0.0}},
            {"xDirection", QJsonArray{1.0, 0.0, 0.0}},
            {"yDirection", QJsonArray{0.0, 1.0, 0.0}},
            {"support", QJsonObject{{"type", 0}}}});
      }
      root["model"] =
          QJsonObject{{"sketches", metadata}, {"bodies", QJsonArray{}}};
      return root;
    };

    const std::array<const char*, 22> stableTypeKeys{
        "Horizontal",   "Vertical",     "Coincident",  "PointOnLine",
        "Distance",     "Length",       "Radius",      "Diameter",
        "Parallel",     "Perpendicular", "Equal",       "Angle",
        "DistanceX",    "DistanceY",    "PointOnCircle", "Tangent",
        "LineDistance", "Lock",         "PointOnArc",  "Midpoint",
        "PointOnXAxis", "PointOnYAxis"};
    const std::array<solidar::sketch::ConstraintType, 22> stableTypes{
        solidar::sketch::ConstraintType::Horizontal,
        solidar::sketch::ConstraintType::Vertical,
        solidar::sketch::ConstraintType::Coincident,
        solidar::sketch::ConstraintType::PointOnLine,
        solidar::sketch::ConstraintType::Distance,
        solidar::sketch::ConstraintType::Length,
        solidar::sketch::ConstraintType::Radius,
        solidar::sketch::ConstraintType::Diameter,
        solidar::sketch::ConstraintType::Parallel,
        solidar::sketch::ConstraintType::Perpendicular,
        solidar::sketch::ConstraintType::Equal,
        solidar::sketch::ConstraintType::Angle,
        solidar::sketch::ConstraintType::DistanceX,
        solidar::sketch::ConstraintType::DistanceY,
        solidar::sketch::ConstraintType::PointOnCircle,
        solidar::sketch::ConstraintType::Tangent,
        solidar::sketch::ConstraintType::LineDistance,
        solidar::sketch::ConstraintType::Lock,
        solidar::sketch::ConstraintType::PointOnArc,
        solidar::sketch::ConstraintType::Midpoint,
        solidar::sketch::ConstraintType::PointOnXAxis,
        solidar::sketch::ConstraintType::PointOnYAxis};

    QJsonArray stableSketches;
    for (int type = 0; type < static_cast<int>(stableTypeKeys.size()); ++type) {
      auto constraint = makeConstraint(
          static_cast<qint64>(type + 1), type,
          QString::fromLatin1(stableTypeKeys[static_cast<std::size_t>(type)]));
      applyCurrentConstraintShape(&constraint, type);
      stableSketches.append(makeRootSketch(QJsonArray{constraint}));
    }
    auto stableRoot = makeV1Root(stableSketches);
    stableRoot["constraintTypeEncoding"] = QStringLiteral("stable-name-v1");
    const QString stablePath =
        writeRoot(QStringLiteral("stable-constraint-codec.solidar"), stableRoot);
    CHECK(!stablePath.isEmpty());
    auto stable = solidar::project::ProjectFile::stageLoad(stablePath);
    if (stable.kind != solidar::project::ProjectLoadKind::ValidV1)
      std::fprintf(stderr, "stable codec stage failed: %s\n",
                   stable.error.toUtf8().constData());
    CHECK(stable.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(stable.legacy.sketches.size() == stableTypes.size());
    for (std::size_t index = 0; index < stableTypes.size(); ++index) {
      CHECK(stable.legacy.sketches[index].geometry.constraints().size() == 1);
      CHECK(stable.legacy.sketches[index].geometry.constraints()[0].type ==
            stableTypes[index]);
    }

    {
      auto root = stableRoot;
      auto sketches = root.value("sketches").toArray();
      auto sketch = sketches[0].toObject();
      auto constraints = sketch.value("constraints").toArray();
      auto constraint = constraints[0].toObject();
      constraint["type"] = 999;
      constraint["typeKey"] = QStringLiteral("FutureConstraint");
      constraints[0] = constraint;
      sketch["constraints"] = constraints;
      sketches[0] = sketch;
      root["sketches"] = sketches;
      CHECK(expectUnsupported("unknown-stable-constraint.solidar", root));
    }
    {
      auto root = stableRoot;
      auto sketches = root.value("sketches").toArray();
      auto sketch = sketches[0].toObject();
      auto constraints = sketch.value("constraints").toArray();
      auto constraint = constraints[0].toObject();
      constraint["typeKey"] = QStringLiteral("Vertical");
      constraints[0] = constraint;
      sketch["constraints"] = constraints;
      sketches[0] = sketch;
      root["sketches"] = sketches;
      CHECK(expectInvalid("mismatched-stable-constraint.solidar", root));
    }
    {
      auto root = stableRoot;
      auto sketches = root.value("sketches").toArray();
      auto sketch = sketches[0].toObject();
      auto constraints = sketch.value("constraints").toArray();
      auto constraint = constraints[0].toObject();
      constraint.remove("typeKey");
      constraints[0] = constraint;
      sketch["constraints"] = constraints;
      sketches[0] = sketch;
      root["sketches"] = sketches;
      CHECK(expectInvalid("missing-stable-constraint-key.solidar", root));
    }
    {
      auto root = stableRoot;
      root["constraintTypeEncoding"] = QStringLiteral("future-name-v2");
      CHECK(expectUnsupported("unknown-constraint-encoding.solidar", root));
    }
    {
      auto root = stableRoot;
      root.remove("sketches");
      root.remove("extrusion");
      CHECK(expectInvalid("stable-root-cannot-use-early-defaults.solidar", root));
    }
    {
      auto root = stableRoot;
      root.remove("name");
      CHECK(expectInvalid("stable-root-missing-name.solidar", root));
      root = stableRoot;
      root.remove("createdAt");
      CHECK(expectInvalid("stable-root-missing-created-at.solidar", root));
    }
    {
      auto root = stableRoot;
      auto sketches = root.value("sketches").toArray();
      auto sketch = sketches[0].toObject();
      sketch.remove("arcs");
      sketches[0] = sketch;
      root["sketches"] = sketches;
      CHECK(expectInvalid("stable-root-missing-arcs.solidar", root));
    }
    {
      auto centerSketch = makeRootSketch(QJsonArray{});
      QJsonArray centeredLines;
      for (int index = 0; index < 4; ++index) {
        const std::array<std::array<double, 2>, 4> points{{
            {{0.0, 0.0}}, {{10.0, 0.0}}, {{10.0, 10.0}}, {{0.0, 10.0}}}};
        const auto& first = points[static_cast<std::size_t>(index)];
        const auto& second =
            points[static_cast<std::size_t>((index + 1) % 4)];
        centeredLines.append(
            QJsonObject{{"x1", first[0]}, {"y1", first[1]},
                        {"x2", second[0]}, {"y2", second[1]},
                        {"elementId", 77}, {"dashed", false}});
      }
      centerSketch["lines"] = centeredLines;
      centerSketch["centerNodeElementIds"] = QJsonArray{77};
      auto centerRoot = makeV1Root(QJsonArray{centerSketch});
      centerRoot["constraintTypeEncoding"] =
          QStringLiteral("stable-name-v1");
      const QString centerPath =
          writeRoot("valid-four-line-center.solidar", centerRoot);
      CHECK(!centerPath.isEmpty());
      const auto centered =
          solidar::project::ProjectFile::stageLoad(centerPath);
      CHECK(centered.kind == solidar::project::ProjectLoadKind::ValidV1);
      CHECK(centered.legacy.sketches[0]
                .geometry.hasElementCenterNode(77));

      centeredLines.removeLast();
      centerSketch["lines"] = centeredLines;
      centerRoot["sketches"] = QJsonArray{centerSketch};
      CHECK(expectInvalid("invalid-three-line-center.solidar", centerRoot));
    }
    {
      auto collapsing = makeConstraint(
          1, static_cast<int>(solidar::sketch::ConstraintType::PointOnXAxis),
          QStringLiteral("PointOnXAxis"));
      collapsing["secondPointLine"] = 0;
      collapsing["secondPointStart"] = false;
      auto sketch = makeRootSketch(QJsonArray{collapsing});
      sketch["lines"] =
          QJsonArray{QJsonObject{{"x1", 0.0}, {"y1", 0.0},
                                 {"x2", 0.0}, {"y2", 10.0},
                                 {"elementId", 1}, {"dashed", false}}};
      auto root = makeV1Root(QJsonArray{sketch});
      root["constraintTypeEncoding"] = QStringLiteral("stable-name-v1");
      CHECK(expectInvalid("solver-collapse-line.solidar", root));
    }
    {
      QJsonArray constraints;
      constexpr int kQuadraticConstraintCount = 1415;
      for (int index = 0; index < kQuadraticConstraintCount; ++index) {
        auto constraint = makeConstraint(
            index + 1,
            static_cast<int>(solidar::sketch::ConstraintType::Horizontal),
            QStringLiteral("Horizontal"));
        constraint["firstGeometryKind"] = QStringLiteral("line");
        constraint["firstGeometry"] = 0;
        constraints.append(constraint);
      }
      auto root = makeV1Root(QJsonArray{makeRootSketch(constraints)});
      root["constraintTypeEncoding"] = QStringLiteral("stable-name-v1");
      CHECK(expectInvalid("quadratic-solver-budget.solidar", root));
    }

    auto historicalConstraint = [&](qint64 id, int type) {
      auto constraint = makeConstraint(id, type, QString{});
      constraint.remove("firstPointElementCenter");
      constraint.remove("secondPointElementCenter");
      return constraint;
    };
    auto historicalSketch = [&](const QJsonArray& constraints) {
      auto sketch = makeRootSketch(constraints);
      sketch.remove("centerNodeElementIds");
      return sketch;
    };

    {
      auto distance = historicalConstraint(1, 3);
      distance["firstPointLine"] = 0;
      distance["secondPointLine"] = 1;
      distance["value"] = 5.0;
      auto length = historicalConstraint(2, 4);
      length["firstGeometryKind"] = QStringLiteral("line");
      length["firstGeometry"] = 0;
      length["value"] = 10.0;
      const auto root = makeV1Root(
          QJsonArray{historicalSketch(QJsonArray{distance, length})});
      const QString path = writeRoot("pre-point-on-line-v1.solidar", root);
      CHECK(!path.isEmpty());
      auto staged = solidar::project::ProjectFile::stageLoad(path);
      CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV1);
      const auto& constraints = staged.legacy.sketches[0].geometry.constraints();
      CHECK(constraints.size() == 2);
      CHECK(constraints[0].type == solidar::sketch::ConstraintType::Distance);
      CHECK(constraints[1].type == solidar::sketch::ConstraintType::Length);
    }
    {
      auto pointOnLine = historicalConstraint(1, 3);
      pointOnLine["firstGeometryKind"] = QStringLiteral("line");
      pointOnLine["firstGeometry"] = 0;
      pointOnLine["secondPointLine"] = 1;
      auto distanceY = historicalConstraint(2, 13);
      distanceY["firstPointLine"] = 0;
      distanceY["secondPointLine"] = 1;
      distanceY["value"] = 5.0;
      const auto root = makeV1Root(
          QJsonArray{historicalSketch(QJsonArray{pointOnLine, distanceY})});
      const QString path = writeRoot("point-on-line-gap-v1.solidar", root);
      CHECK(!path.isEmpty());
      auto staged = solidar::project::ProjectFile::stageLoad(path);
      CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV1);
      const auto& constraints = staged.legacy.sketches[0].geometry.constraints();
      CHECK(constraints.size() == 2);
      CHECK(constraints[0].type ==
            solidar::sketch::ConstraintType::PointOnLine);
      CHECK(constraints[1].type ==
            solidar::sketch::ConstraintType::DistanceY);
    }
    for (const int ambiguousType : {6, 8, 9, 10, 12}) {
      auto constraint = historicalConstraint(1, ambiguousType);
      if (ambiguousType == 6) {
        constraint["firstGeometryKind"] = QStringLiteral("circle");
        constraint["firstGeometry"] = 0;
        constraint["value"] = 2.0;
      } else if (ambiguousType == 8 || ambiguousType == 9 ||
                 ambiguousType == 10) {
        constraint["firstGeometryKind"] = QStringLiteral("line");
        constraint["firstGeometry"] = 0;
        constraint["secondGeometryKind"] = QStringLiteral("line");
        constraint["secondGeometry"] = 1;
        if (ambiguousType == 10) constraint["value"] = 45.0;
      } else {
        constraint["firstPointLine"] = 0;
        constraint["secondPointLine"] = 1;
        constraint["value"] = 5.0;
      }
      const auto root = makeV1Root(
          QJsonArray{historicalSketch(QJsonArray{constraint})});
      CHECK(expectUnsupported(
          QStringLiteral("ambiguous-v1-%1.solidar").arg(ambiguousType), root));
    }
    {
      auto horizontal = historicalConstraint(1, 0);
      horizontal["firstGeometryKind"] = QStringLiteral("line");
      horizontal["firstGeometry"] = 0;
      const auto root = makeV1Root(
          QJsonArray{historicalSketch(QJsonArray{horizontal})});
      const QString path = writeRoot("common-ordinal-v1.solidar", root);
      CHECK(!path.isEmpty());
      const auto staged = solidar::project::ProjectFile::stageLoad(path);
      CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV1);
      CHECK(staged.legacy.sketches[0].geometry.constraints()[0].type ==
            solidar::sketch::ConstraintType::Horizontal);
    }
    {
      auto lostPointOnLine = historicalConstraint(1, 3);
      lostPointOnLine["firstGeometryKind"] = QStringLiteral("line");
      lostPointOnLine["firstGeometry"] = 0;
      const auto root = makeV1Root(
          QJsonArray{historicalSketch(QJsonArray{lostPointOnLine})});
      CHECK(expectInvalid("lost-point-on-line-reference.solidar", root));
    }

    QJsonArray appendedSketches;
    for (int type = 16; type <= 21; ++type) {
      auto constraint = makeConstraint(type + 1, type, QString{});
      applyCurrentConstraintShape(&constraint, type);
      appendedSketches.append(makeRootSketch(QJsonArray{constraint}));
    }
    auto appendedV1Root = makeV1Root(appendedSketches);
    const QString appendedV1Path =
        writeRoot("unmarked-current-v1.solidar", appendedV1Root);
    CHECK(!appendedV1Path.isEmpty());
    auto appendedV1 =
        solidar::project::ProjectFile::stageLoad(appendedV1Path);
    CHECK(appendedV1.kind == solidar::project::ProjectLoadKind::ValidV1);
    CHECK(appendedV1.legacy.sketches.size() == 6);
    for (std::size_t index = 0; index < 6; ++index) {
      CHECK(appendedV1.legacy.sketches[index].geometry.constraints().size() ==
            1);
      CHECK(appendedV1.legacy.sketches[index]
                .geometry.constraints()[0].type == stableTypes[index + 16]);
    }

    auto appendedV2Root = makeV2Root(appendedV1Root);
    const QString appendedV2Path =
        writeRoot("unmarked-current-v2.solidar", appendedV2Root);
    CHECK(!appendedV2Path.isEmpty());
    auto appendedV2 =
        solidar::project::ProjectFile::stageLoad(appendedV2Path);
    CHECK(appendedV2.kind == solidar::project::ProjectLoadKind::ValidV2);
    CHECK(appendedV2.document.has_value());
    CHECK(appendedV2.document->sketches().size() == 6);
    for (std::size_t index = 0; index < 6; ++index)
      CHECK(appendedV2.document->sketches()[index]
                .geometry.constraints().size() == 1);

    {
      auto root = validRoot;
      root.remove("model");
      CHECK(expectInvalid("missing-model.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto sketches = model.value("sketches").toArray();
      auto first = sketches[0].toObject();
      first["id"] = 0;
      sketches[0] = first;
      model["sketches"] = sketches;
      root["model"] = model;
      CHECK(expectInvalid("zero-sketch-id.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto sketches = model.value("sketches").toArray();
      auto second = sketches[1].toObject();
      second["id"] = sketches[0].toObject().value("id");
      sketches[1] = second;
      model["sketches"] = sketches;
      root["model"] = model;
      CHECK(expectInvalid("duplicate-sketch-id.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      bodyObject["id"] = 0;
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("zero-body-id.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      bodies.append(bodies[0]);
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("duplicate-body-id.solidar", root));
    }

    auto mutateFirstFeature = [&](QJsonObject root,
                                  const std::function<void(QJsonObject&)>& mutate) {
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      auto features = bodyObject.value("features").toArray();
      auto feature = features[0].toObject();
      mutate(feature);
      features[0] = feature;
      bodyObject["features"] = features;
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      root["model"] = model;
      return root;
    };
    CHECK(expectInvalid("zero-feature-id.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["id"] = 0;
                        })));
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      auto features = bodyObject.value("features").toArray();
      features.append(features[0]);
      bodyObject["features"] = features;
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("duplicate-feature-id.solidar", root));
    }
    CHECK(expectInvalid("unknown-feature.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["type"] = QStringLiteral("FutureFeature");
                        })));
    CHECK(expectInvalid("invalid-enum.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["operation"] = 99;
                        })));
    CHECK(expectInvalid("nonfinite-number.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          // JSON has no NaN/Inf representation; null is how a
                          // non-finite producer is encoded by Qt.
                          feature["lengthMm"] = QJsonValue::Null;
                        })));
    CHECK(expectInvalid("out-of-range-number.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["lengthMm"] = 100001.0;
                        })));
    CHECK(expectInvalid("broken-reference.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["sketchId"] = 999999;
                        })));
    CHECK(expectInvalid("stable-extrude-missing-source-kind.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature.remove("sourceKind");
                        })));
    {
      auto root = validRoot;
      root.remove("name");
      CHECK(expectInvalid("stable-v2-missing-name.solidar", root));
    }
    {
      auto root = validRoot;
      auto sketches = root.value("sketches").toArray();
      auto first = sketches[0].toObject();
      first.remove("sketchId");
      sketches[0] = first;
      root["sketches"] = sketches;
      CHECK(expectInvalid("missing-geometry-sketch-id.solidar", root));

      root = validRoot;
      sketches = root.value("sketches").toArray();
      auto second = sketches[1].toObject();
      second["sketchId"] = sketches[0].toObject().value("sketchId");
      sketches[1] = second;
      root["sketches"] = sketches;
      CHECK(expectInvalid("duplicate-geometry-sketch-id.solidar", root));

      root = validRoot;
      sketches = root.value("sketches").toArray();
      const QJsonValue firstSavedSketch = sketches[0];
      sketches[0] = sketches[1];
      sketches[1] = firstSavedSketch;
      root["sketches"] = sketches;
      CHECK(expectInvalid("reordered-geometry-only.solidar", root));
    }
    {
      auto root = validRoot;
      auto sketches = root.value("sketches").toArray();
      auto first = sketches[0].toObject();
      first.remove("arcs");
      sketches[0] = first;
      root["sketches"] = sketches;
      CHECK(expectInvalid("stable-v2-missing-arcs.solidar", root));
    }
    {
      auto root = validRoot;
      auto sketches = root.value("sketches").toArray();
      auto first = sketches[0].toObject();
      auto dimensions = first.value("dimensions").toArray();
      auto pointDimension = dimensions[1].toObject();
      pointDimension.remove("firstOrigin");
      dimensions[1] = pointDimension;
      first["dimensions"] = dimensions;
      sketches[0] = first;
      root["sketches"] = sketches;
      CHECK(expectInvalid("partial-dimension-origin-group.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto sketches = model.value("sketches").toArray();
      auto first = sketches[0].toObject();
      first["xDirection"] = QJsonArray{2.0, 0.0, 0.0};
      sketches[0] = first;
      model["sketches"] = sketches;
      root["model"] = model;
      CHECK(expectInvalid("scaled-sketch-basis.solidar", root));

      root = validRoot;
      model = root.value("model").toObject();
      sketches = model.value("sketches").toArray();
      first = sketches[0].toObject();
      first["yDirection"] = QJsonArray{1.0, 1.0, 0.0};
      sketches[0] = first;
      model["sketches"] = sketches;
      root["model"] = model;
      CHECK(expectInvalid("skew-sketch-basis.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      bodyObject.remove("visible");
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("stable-v2-missing-visible.solidar", root));
    }
    {
      auto patternRoot = validRoot;
      auto model = patternRoot.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto bodyObject = bodies[0].toObject();
      auto features = bodyObject.value("features").toArray();
      auto pattern = features[2].toObject();
      pattern["type"] = QStringLiteral("LinearPattern");
      pattern["sourceBodyId"] = 0;
      pattern["sourceFeatureId"] = features[1].toObject().value("id");
      pattern["direction"] = 0;
      pattern["count"] = 3;
      pattern["spacingMm"] = 10.0;
      pattern["operation"] =
          static_cast<int>(solidar::PatternOperation::Join);
      features[2] = pattern;
      bodyObject["features"] = features;
      bodies[0] = bodyObject;
      model["bodies"] = bodies;
      patternRoot["model"] = model;

      auto missingOperation = patternRoot;
      auto missingModel = missingOperation.value("model").toObject();
      auto missingBodies = missingModel.value("bodies").toArray();
      auto missingBody = missingBodies[0].toObject();
      auto missingFeatures = missingBody.value("features").toArray();
      auto missingPattern = missingFeatures[2].toObject();
      missingPattern.remove("operation");
      missingFeatures[2] = missingPattern;
      missingBody["features"] = missingFeatures;
      missingBodies[0] = missingBody;
      missingModel["bodies"] = missingBodies;
      missingOperation["model"] = missingModel;
      CHECK(expectInvalid("stable-pattern-missing-operation.solidar",
                          missingOperation));

      auto missingSourceBody = patternRoot;
      missingModel = missingSourceBody.value("model").toObject();
      missingBodies = missingModel.value("bodies").toArray();
      missingBody = missingBodies[0].toObject();
      missingFeatures = missingBody.value("features").toArray();
      missingPattern = missingFeatures[2].toObject();
      missingPattern.remove("sourceBodyId");
      missingFeatures[2] = missingPattern;
      missingBody["features"] = missingFeatures;
      missingBodies[0] = missingBody;
      missingModel["bodies"] = missingBodies;
      missingSourceBody["model"] = missingModel;
      CHECK(expectInvalid("stable-pattern-missing-source-body.solidar",
                          missingSourceBody));
    }
    {
      auto root = mutateFirstFeature(
          validRoot, [](QJsonObject& feature) {
            feature["profileOverride"] = QJsonObject{
                {"lines", QJsonArray{QJsonObject{{"x1", 1.0}, {"y1", 1.0},
                                                  {"x2", 2.0}, {"y2", 1.0},
                                                  {"elementId", 1}}}},
                {"circles", QJsonArray{}}};
          });
      CHECK(expectInvalid("stable-profile-missing-arcs.solidar", root));
      root = mutateFirstFeature(
          validRoot, [](QJsonObject& feature) {
            feature["profileOverride"] = QJsonObject{
                {"lines", QJsonArray{QJsonObject{{"x1", 1.0}, {"y1", 1.0},
                                                  {"x2", 2.0}, {"y2", 1.0}}}},
                {"circles", QJsonArray{}}, {"arcs", QJsonArray{}}};
          });
      CHECK(expectInvalid("stable-profile-missing-element-id.solidar", root));
    }
    CHECK(expectInvalid("truncated-brep.solidar",
                        mutateFirstFeature(validRoot, [](QJsonObject& feature) {
                          feature["type"] = QStringLiteral("ImportedShape");
                          feature["brep"] = QString::fromLatin1(
                              QByteArray("truncated-brep").toBase64());
                        })));
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      QJsonArray oversized;
      for (qsizetype index = 0;
           index <= solidar::project::ProjectFile::kMaximumCollectionItems;
           ++index)
        oversized.append(QJsonValue::Null);
      model["bodies"] = oversized;
      root["model"] = model;
      CHECK(expectInvalid("oversized-array.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      QJsonArray tooManyBodies;
      for (qsizetype index = 0;
           index <= solidar::project::ProjectFile::kMaximumDocumentBodies;
           ++index)
        tooManyBodies.append(QJsonObject{});
      model["bodies"] = tooManyBodies;
      root["model"] = model;
      CHECK(expectInvalid("too-many-bodies.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      const auto featureTemplate =
          body.value("features").toArray()[0].toObject();
      QJsonArray tooManyFeatures;
      for (qsizetype index = 0;
           index <= solidar::project::ProjectFile::kMaximumDocumentFeatures;
           ++index)
        tooManyFeatures.append(featureTemplate);
      body["features"] = tooManyFeatures;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("too-many-features.solidar", root));
    }
    {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto fillet = features[2].toObject();
      CHECK(fillet.value("type").toString() == QStringLiteral("Fillet"));
      const auto edgeTemplate = fillet.value("edges").toArray()[0];
      QJsonArray tooManyEdges;
      for (qsizetype index = 0;
           index <=
           solidar::project::ProjectFile::kMaximumFeatureTopologyReferences;
           ++index)
        tooManyEdges.append(edgeTemplate);
      fillet["edges"] = tooManyEdges;
      features[2] = fillet;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("too-many-feature-topology-refs.solidar", root));
    }
    {
      // Exactly the document-wide allowance is consumed by face-supported
      // sketches. One additional face-source Extrude reference must be
      // rejected even though every individual feature stays below its cap.
      auto root = validRoot;
      auto model = root.value("model").toObject();
      const auto originalMetadata = model.value("sketches").toArray();
      QJsonObject faceSupport;
      for (const auto value : originalMetadata) {
        const auto support = value.toObject().value("support").toObject();
        if (support.value("type").toInt() ==
            static_cast<int>(solidar::SketchSupportType::Face)) {
          faceSupport = support;
          break;
        }
      }
      CHECK(!faceSupport.isEmpty());

      QJsonArray geometrySketches;
      QJsonArray metadataSketches;
      for (qsizetype index = 0;
           index <
           solidar::project::ProjectFile::kMaximumDocumentTopologyReferences;
           ++index) {
        const qint64 id = 10000 + index;
        geometrySketches.append(QJsonObject{
            {"sketchId", id},
            {"support", QStringLiteral("XY")},
            {"lines", QJsonArray{}},
            {"circles", QJsonArray{}},
            {"arcs", QJsonArray{}},
            {"dimensions", QJsonArray{}},
            {"constraints", QJsonArray{}},
            {"centerNodeElementIds", QJsonArray{}}});
        metadataSketches.append(QJsonObject{
            {"id", id},
            {"name", QStringLiteral("Supported %1").arg(index)},
            {"origin", QJsonArray{0.0, 0.0, 0.0}},
            {"xDirection", QJsonArray{1.0, 0.0, 0.0}},
            {"yDirection", QJsonArray{0.0, 1.0, 0.0}},
            {"support", faceSupport}});
      }
      root["sketches"] = geometrySketches;
      model["sketches"] = metadataSketches;

      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto extrude = features[0].toObject();
      CHECK(extrude.value("type").toString() == QStringLiteral("Extrude"));
      extrude["sourceKind"] = QStringLiteral("face");
      extrude["face"] = faceSupport;
      extrude.remove("sketchId");
      features[0] = extrude;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("document-topology-reference-budget.solidar",
                          root));
    }
    const auto patternRoot = [&](const QString& type) {
      auto root = validRoot;
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto pattern = features[2].toObject();
      pattern["type"] = type;
      pattern["sourceBodyId"] = 0;
      pattern["sourceFeatureId"] = features[1].toObject().value("id");
      pattern["count"] = solidar::kMinimumPatternCount;
      pattern["operation"] = static_cast<int>(solidar::PatternOperation::Join);
      if (type == QStringLiteral("LinearPattern")) {
        pattern["direction"] =
            static_cast<int>(solidar::PrincipalAxis::X);
        pattern["spacingMm"] = 10.0;
      } else {
        pattern["axis"] = static_cast<int>(solidar::PrincipalAxis::Z);
        pattern["angleDeg"] = 360.0;
      }
      features[2] = pattern;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      return root;
    };

    // Parametric patterns exist only in project version 2. Exercise both v2
    // dialects accepted by the reader: the current stable schema and the
    // historical schema that predates constraintTypeEncoding and typeKey.
    for (const bool stableSchema : {false, true}) {
      for (const auto& [type, parameter] : {
               std::pair{QStringLiteral("LinearPattern"),
                         QStringLiteral("spacingMm")},
               std::pair{QStringLiteral("CircularPattern"),
                         QStringLiteral("angleDeg")}}) {
        for (const double value : {0.009, 0.01}) {
          auto root = patternRoot(type);
          if (!stableSchema) {
            root.remove("constraintTypeEncoding");
            auto sketches = root.value("sketches").toArray();
            for (qsizetype sketchIndex = 0; sketchIndex < sketches.size();
                 ++sketchIndex) {
              auto sketch = sketches[sketchIndex].toObject();
              auto constraints = sketch.value("constraints").toArray();
              for (qsizetype constraintIndex = 0;
                   constraintIndex < constraints.size(); ++constraintIndex) {
                auto constraint = constraints[constraintIndex].toObject();
                constraint.remove("typeKey");
                constraints[constraintIndex] = constraint;
              }
              sketch["constraints"] = constraints;
              sketches[sketchIndex] = sketch;
            }
            root["sketches"] = sketches;
          }
          auto model = root.value("model").toObject();
          auto bodies = model.value("bodies").toArray();
          auto body = bodies[0].toObject();
          auto features = body.value("features").toArray();
          auto pattern = features[2].toObject();
          pattern[parameter] = value;
          features[2] = pattern;
          body["features"] = features;
          bodies[0] = body;
          model["bodies"] = bodies;
          root["model"] = model;

          const QString dialect = stableSchema ? QStringLiteral("stable")
                                               : QStringLiteral("historical");
          const QString name =
              QStringLiteral("%1-%2-%3-boundary-%4.solidar")
                  .arg(dialect, type, parameter)
                  .arg(value, 0, 'f', 3);
          if (value < solidar::kMinimumPatternParameter) {
            CHECK(expectInvalid(name, root));
          } else {
            const QString path = writeRoot(name, root);
            CHECK(!path.isEmpty());
            const auto staged =
                solidar::project::ProjectFile::stageLoad(path);
            CHECK(staged.kind == solidar::project::ProjectLoadKind::ValidV2);
            CHECK(staged.document.has_value());
            CHECK(staged.document->bodies().size() == 1);
            CHECK(staged.document->bodies()[0].features().size() == 3);
            CHECK(staged.document->bodies()[0].features()[2]->isValid());
          }
        }
      }
    }

    for (const QString& type : {QStringLiteral("LinearPattern"),
                                QStringLiteral("CircularPattern")}) {
      for (const int badCount :
           {0, 1, solidar::kMaximumPatternCount + 1,
            std::numeric_limits<int>::max()}) {
        auto root = patternRoot(type);
        auto model = root.value("model").toObject();
        auto bodies = model.value("bodies").toArray();
        auto body = bodies[0].toObject();
        auto features = body.value("features").toArray();
        auto pattern = features[2].toObject();
        pattern["count"] = badCount;
        features[2] = pattern;
        body["features"] = features;
        bodies[0] = body;
        model["bodies"] = bodies;
        root["model"] = model;
        CHECK(expectInvalid(
            QStringLiteral("bad-%1-count-%2.solidar").arg(type).arg(badCount),
            root));
      }
    }

    for (const auto& [type, enumField] : {
             std::pair{QStringLiteral("LinearPattern"),
                       QStringLiteral("direction")},
             std::pair{QStringLiteral("CircularPattern"),
                       QStringLiteral("axis")}}) {
      auto root = patternRoot(type);
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto pattern = features[2].toObject();
      pattern[enumField] = 99;
      features[2] = pattern;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid(
          QStringLiteral("bad-%1-%2.solidar").arg(type).arg(enumField), root));
    }

    {
      auto root = patternRoot(QStringLiteral("LinearPattern"));
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto pattern = features[2].toObject();
      pattern["operation"] = 99;
      features[2] = pattern;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid("bad-pattern-operation.solidar", root));
    }

    for (const auto& [type, parameter, value] : {
             std::tuple{QStringLiteral("LinearPattern"),
                        QStringLiteral("spacingMm"),
                        solidar::kMaximumPatternSpacingMm + 0.01},
             std::tuple{QStringLiteral("CircularPattern"),
                        QStringLiteral("angleDeg"),
                        solidar::kMaximumPatternAngleDeg + 0.01}}) {
      auto root = patternRoot(type);
      auto model = root.value("model").toObject();
      auto bodies = model.value("bodies").toArray();
      auto body = bodies[0].toObject();
      auto features = body.value("features").toArray();
      auto pattern = features[2].toObject();
      pattern[parameter] = value;
      features[2] = pattern;
      body["features"] = features;
      bodies[0] = body;
      model["bodies"] = bodies;
      root["model"] = model;
      CHECK(expectInvalid(QStringLiteral("bad-%1-limit.solidar").arg(parameter),
                          root));
    }

    // Staging is speculative: explicit IDs from a candidate file must not
    // advance process-global generators. Committing the same document must
    // reserve all three ID domains and retain the documented save headroom.
    {
      solidar::Document templateDocument;
      auto& templateSketch = templateDocument.addSketch("ID profile");
      const auto templateSketchId = templateSketch.id;
      templateSketch.geometry.addRectangle({0.0, 0.0}, {10.0, 10.0});
      auto& templateBody = templateDocument.addBody("ID body");
      templateBody.addFeature(std::make_unique<solidar::ExtrudeFeature>(
          templateSketchId, 5.0, "ID extrude"));
      CHECK(templateDocument.recompute());
      const QString templatePath =
          directory.filePath("id-reservation-template.solidar");
      CHECK(solidar::project::ProjectFile::saveDocument(
          templatePath, templateDocument, &error));
      QFile templateFile(templatePath);
      CHECK(templateFile.open(QIODevice::ReadOnly));
      auto highIdRoot =
          QJsonDocument::fromJson(templateFile.readAll()).object();

      constexpr qint64 kHighId =
          solidar::project::ProjectFile::kMaximumPersistedId - 10;
      auto geometrySketches = highIdRoot.value("sketches").toArray();
      auto geometrySketch = geometrySketches[0].toObject();
      geometrySketch["sketchId"] = kHighId;
      geometrySketches[0] = geometrySketch;
      highIdRoot["sketches"] = geometrySketches;
      auto highModel = highIdRoot.value("model").toObject();
      auto metadataSketches = highModel.value("sketches").toArray();
      auto metadataSketch = metadataSketches[0].toObject();
      metadataSketch["id"] = kHighId;
      metadataSketches[0] = metadataSketch;
      highModel["sketches"] = metadataSketches;
      auto highBodies = highModel.value("bodies").toArray();
      auto highBody = highBodies[0].toObject();
      highBody["id"] = kHighId;
      auto highFeatures = highBody.value("features").toArray();
      auto highFeature = highFeatures[0].toObject();
      highFeature["id"] = kHighId;
      highFeature["sketchId"] = kHighId;
      highFeatures[0] = highFeature;
      highBody["features"] = highFeatures;
      highBodies[0] = highBody;
      highModel["bodies"] = highBodies;
      highIdRoot["model"] = highModel;

      auto outOfRangeRoot = highIdRoot;
      auto outGeometrySketches = outOfRangeRoot.value("sketches").toArray();
      auto outGeometrySketch = outGeometrySketches[0].toObject();
      outGeometrySketch["sketchId"] =
          solidar::project::ProjectFile::kMaximumPersistedId + 1;
      outGeometrySketches[0] = outGeometrySketch;
      outOfRangeRoot["sketches"] = outGeometrySketches;
      CHECK(expectInvalid("persisted-id-over-headroom.solidar",
                          outOfRangeRoot));

      const QString highIdPath =
          writeRoot("high-id-staged.solidar", highIdRoot);
      CHECK(!highIdPath.isEmpty());
      solidar::Document beforeStage;
      const auto beforeSketchId = beforeStage.addSketch("Before").id;
      auto& beforeBody = beforeStage.addBody("Before");
      const auto beforeBodyId = beforeBody.id();
      auto beforeFeature = std::make_unique<solidar::ExtrudeFeature>(
          beforeSketchId, 1.0, "Before");
      const auto beforeFeatureId = beforeFeature->id();
      beforeBody.addFeature(std::move(beforeFeature));

      const auto stagedHighId =
          solidar::project::ProjectFile::stageLoad(highIdPath);
      CHECK(stagedHighId.kind ==
            solidar::project::ProjectLoadKind::ValidV2);
      CHECK(stagedHighId.document.has_value());

      solidar::Document afterStage;
      const auto afterSketchId = afterStage.addSketch("After").id;
      auto& afterBody = afterStage.addBody("After");
      const auto afterBodyId = afterBody.id();
      auto afterFeature = std::make_unique<solidar::ExtrudeFeature>(
          afterSketchId, 1.0, "After");
      const auto afterFeatureId = afterFeature->id();
      afterBody.addFeature(std::move(afterFeature));
      CHECK(afterSketchId == beforeSketchId + 1);
      CHECK(afterBodyId == beforeBodyId + 1);
      CHECK(afterFeatureId == beforeFeatureId + 1);

      solidar::Document committed;
      CHECK(solidar::project::ProjectFile::loadDocument(
          highIdPath, &committed, &error));
      auto& nextSketch = committed.addSketch("After high ID");
      const auto nextSketchId = nextSketch.id;
      nextSketch.geometry.addRectangle({20.0, 0.0}, {30.0, 10.0});
      auto& nextBody = committed.addBody("After high ID");
      const auto nextBodyId = nextBody.id();
      auto nextFeature = std::make_unique<solidar::ExtrudeFeature>(
          nextSketchId, 5.0, "After high ID");
      const auto nextFeatureId = nextFeature->id();
      nextBody.addFeature(std::move(nextFeature));
      CHECK(nextSketchId == static_cast<solidar::SketchId>(kHighId + 1));
      CHECK(nextBodyId == static_cast<solidar::BodyId>(kHighId + 1));
      CHECK(nextFeatureId == static_cast<solidar::FeatureId>(kHighId + 1));
      CHECK(committed.recompute());
      CHECK(solidar::project::ProjectFile::saveDocument(
          directory.filePath("high-id-edited.solidar"), committed, &error));
    }
  }

  QFile broken(directory.filePath("broken.solidar"));
  CHECK(broken.open(QIODevice::WriteOnly));
  broken.write("not json");
  broken.close();
  CHECK(!solidar::project::ProjectFile::validate(broken.fileName(), &error));
  return 0;
}
