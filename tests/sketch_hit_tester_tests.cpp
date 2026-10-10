#include "TestAssertions.h"

#include "ui/SketchHitSceneAdapter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace solidar::sketch {

class SketchTestAccess {
 public:
  static void installConstraints(Sketch& sketch,
                                 std::vector<Constraint> constraints) {
    sketch.constraints_ = std::move(constraints);
    sketch.invalidateStructureIndexes();
  }
};

}  // namespace solidar::sketch

using namespace solidar;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

SketchPickFilter onlyEntities() {
  SketchPickFilter filter;
  filter.points = false;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  return filter;
}

SketchPickFilter onlyPoints() {
  SketchPickFilter filter;
  filter.entities = false;
  filter.datums = false;
  filter.projections = false;
  filter.dimensions = false;
  return filter;
}

SketchPickCandidate lineCandidate(sketch::GeometryId id, double y,
                                  double tolerance, int priority,
                                  std::size_t order) {
  return {SketchPickEntityRef{SketchPickEntityKind::Line, id},
          {{{0.0, y}, {10.0, y}}}, std::nullopt, tolerance, priority,
          order, true, true};
}

void toleranceAndTieBreaksAreExplicit() {
  SketchHitScene scene;
  scene.candidates.push_back(lineCandidate(1, 0.0, 5.0, 20, 0));
  auto hit = SketchHitTester::pick(scene, {5.0, 5.0}, onlyEntities());
  CHECK(!hit.has_value());
  hit = SketchHitTester::pick(scene, {5.0, 4.9999}, onlyEntities());
  CHECK(hit.has_value());
  CHECK(hit->entity() && hit->entity()->geometryId == 1);
  CHECK(std::abs(hit->distancePx - 4.9999) < 1e-9);
  CHECK(!SketchHitTester::pick(scene, {5.0, 5.0001}, onlyEntities()));

  // Priority is the primary policy, then distance, then stable order.
  scene.candidates.push_back(lineCandidate(2, 4.0, 8.0, 10, 9));
  hit = SketchHitTester::pick(scene, {5.0, 0.1}, onlyEntities());
  CHECK(hit && hit->entity()->geometryId == 2);
  scene.candidates.push_back(lineCandidate(3, 1.0, 8.0, 10, 8));
  hit = SketchHitTester::pick(scene, {5.0, 0.1}, onlyEntities());
  CHECK(hit && hit->entity()->geometryId == 3);
  scene.candidates.push_back(lineCandidate(4, 1.0, 8.0, 10, 7));
  hit = SketchHitTester::pick(scene, {5.0, 0.1}, onlyEntities());
  CHECK(hit && hit->entity()->geometryId == 4);
}

void adapterUsesStableReferencesForRepresentativeGeometry() {
  sketch::Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0}, 17);
  sketch.addCircle({20.0, 10.0}, 5.0);
  sketch.addArc({30.0, 0.0}, 4.0, 0.0, 1.5707963267948966);
  const auto lineId = sketch.lineId(0);
  const auto circleId = sketch.circleId(0);
  const auto arcId = sketch.arcId(0);

  SketchHitSceneOptions options;
  options.origin = SketchHitPoint{0.0, 0.0};
  options.xAxis = SketchScreenSegment{{-100.0, 0.0}, {100.0, 0.0}};
  options.yAxis = SketchScreenSegment{{0.0, -100.0}, {0.0, 100.0}};
  const auto scene = SketchHitSceneAdapter::build(
      sketch, [](sketch::Point point) {
        return SketchHitPoint{point.xMm, point.yMm};
      }, options);

  auto hit = SketchHitTester::pick(scene, {5.0, 1.0}, onlyEntities());
  CHECK(hit && hit->entity());
  CHECK(hit->entity()->kind == SketchPickEntityKind::Line);
  CHECK(hit->entity()->geometryId == lineId);
  CHECK(hit->entity()->elementId == 17);

  hit = SketchHitTester::pick(scene, {25.0, 10.0}, onlyEntities());
  CHECK(hit && hit->entity()->kind == SketchPickEntityKind::Circle);
  CHECK(hit->entity()->geometryId == circleId);

  hit = SketchHitTester::pick(scene, {32.8, 2.8}, onlyEntities());
  CHECK(hit && hit->entity()->kind == SketchPickEntityKind::Arc);
  CHECK(hit->entity()->geometryId == arcId);

  auto pointHit = SketchHitTester::pick(scene, {0.0, 0.0}, onlyPoints());
  CHECK(pointHit && pointHit->point());
  CHECK(pointHit->point()->kind == SketchPickPointKind::LineEndpoint);
  CHECK(pointHit->point()->reference.lineId == lineId);

  pointHit = SketchHitTester::pick(scene, {20.0, 10.0}, onlyPoints());
  CHECK(pointHit && pointHit->point()->kind ==
                        SketchPickPointKind::CircleCenter);
  CHECK(pointHit->point()->reference.circleId == circleId);

  pointHit = SketchHitTester::pick(scene, {34.0, 0.0}, onlyPoints());
  CHECK(pointHit && pointHit->point()->kind ==
                        SketchPickPointKind::ArcEndpoint);
  CHECK(pointHit->point()->reference.arcId == arcId);

  auto midpointOnly = onlyPoints();
  midpointOnly.lineEndpoints = false;
  midpointOnly.circleCenters = false;
  midpointOnly.arcEndpoints = false;
  midpointOnly.elementCenters = false;
  pointHit = SketchHitTester::pick(scene, {5.0, 0.0}, midpointOnly);
  CHECK(pointHit && pointHit->point()->kind ==
                        SketchPickPointKind::LineMidpoint);
  CHECK(pointHit->point()->carrierId == lineId);
}

void filtersAndInvalidReferencesFailClosed() {
  SketchHitScene scene;
  auto construction = lineCandidate(1, 0.0, 9.0, 20, 0);
  std::get<SketchPickEntityRef>(construction.target).construction = true;
  scene.candidates.push_back(construction);
  auto projected = lineCandidate(2, 0.0, 9.0, 20, 1);
  std::get<SketchPickEntityRef>(projected.target).projected = true;
  scene.candidates.push_back(projected);
  auto hidden = lineCandidate(3, 0.0, 9.0, 20, 2);
  hidden.visible = false;
  scene.candidates.push_back(hidden);
  scene.candidates.push_back(lineCandidate(sketch::kInvalidGeometryId, 0.0,
                                           9.0, 0, 3));
  auto nonFinite = lineCandidate(4, 0.0, 9.0, 0, 4);
  nonFinite.segments.front().first.x =
      std::numeric_limits<double>::quiet_NaN();
  scene.candidates.push_back(nonFinite);

  auto filter = onlyEntities();
  filter.includeConstruction = false;
  filter.includeProjected = false;
  CHECK(!SketchHitTester::pick(scene, {5.0, 0.0}, filter));
  filter.includeProjected = true;
  auto hit = SketchHitTester::pick(scene, {5.0, 0.0}, filter);
  CHECK(hit && hit->entity()->geometryId == 2);

  SketchHitScene transient;
  transient.candidates.push_back(
      {SketchProjectionEdgeToken{SketchProjectionSource::ReferenceBody, 0,
                                 0, 0},
       {{{0.0, 0.0}, {10.0, 0.0}}}, std::nullopt, 9.0, 0, 0, true, true});
  SketchPickFilter projectionOnly{};
  projectionOnly.entities = projectionOnly.points =
      projectionOnly.datums = projectionOnly.dimensions = false;
  CHECK(!SketchHitTester::pick(transient, {5.0, 0.0}, projectionOnly));

  transient.candidates.clear();
  transient.candidates.push_back(
      {SketchProjectionEdgeToken{SketchProjectionSource::ReferenceBody, 42,
                                 7, 0},
       {{{0.0, 0.0}, {10.0, 0.0}}}, std::nullopt, 9.0, 0, 0, true, true});
  hit = SketchHitTester::pick(transient, {5.0, 0.0}, projectionOnly);
  CHECK(hit && hit->projection());
  CHECK(hit->projection()->meshRevision == 42);
  CHECK(hit->projection()->edgeSlot == 7);
}

void adapterDerivesProjectedStateFromDashedLockContract() {
  sketch::Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0}, 42);
  const auto projectedId = sketch.lineId(0);
  sketch.setLineDashedById(projectedId, true);
  // Solver locking propagates through a composite element. Projection
  // identity must not: only the exact Lock.firstGeometry is projected.
  sketch.addLine({0.0, 0.0}, {10.0, 0.0}, 42);
  const auto compositeSiblingId = sketch.lineId(1);
  sketch.setLineDashedById(compositeSiblingId, true);
  sketch.addLine({0.0, 10.0}, {10.0, 10.0});
  const auto constructionId = sketch.lineId(2);
  sketch.setLineDashedById(constructionId, true);
  sketch::Constraint lock;
  lock.type = sketch::ConstraintType::Lock;
  lock.firstGeometry = projectedId;
  CHECK(sketch.addConstraint(lock) != sketch::kInvalidConstraintId);
  const auto scene = SketchHitSceneAdapter::build(
      sketch, [](sketch::Point point) {
        return SketchHitPoint{point.xMm, point.yMm};
      });

  const auto candidateFor = [&scene](sketch::GeometryId id) {
    return std::find_if(scene.candidates.begin(), scene.candidates.end(),
                        [id](const SketchPickCandidate& candidate) {
                          const auto* entity =
                              std::get_if<SketchPickEntityRef>(&candidate.target);
                          return entity && entity->geometryId == id;
                        });
  };
  const auto projected = candidateFor(projectedId);
  const auto sibling = candidateFor(compositeSiblingId);
  const auto construction = candidateFor(constructionId);
  CHECK(projected != scene.candidates.end());
  CHECK(sibling != scene.candidates.end());
  CHECK(construction != scene.candidates.end());
  CHECK(std::get<SketchPickEntityRef>(projected->target).projected);
  CHECK(!std::get<SketchPickEntityRef>(projected->target).construction);
  CHECK(!std::get<SketchPickEntityRef>(sibling->target).projected);
  CHECK(std::get<SketchPickEntityRef>(sibling->target).construction);
  CHECK(!std::get<SketchPickEntityRef>(construction->target).projected);
  CHECK(std::get<SketchPickEntityRef>(construction->target).construction);

  auto filter = onlyEntities();
  filter.includeConstruction = false;
  filter.includeProjected = true;
  SketchHitScene projectedOnly{{*projected}};
  auto hit = SketchHitTester::pick(projectedOnly, {5.0, 0.0}, filter);
  CHECK(hit && hit->entity() && hit->entity()->geometryId == projectedId);
  filter.includeConstruction = true;
  filter.includeProjected = false;
  CHECK(!SketchHitTester::pick(projectedOnly, {5.0, 0.0}, filter));

  SketchHitScene constructionOnly{{*construction}};
  hit = SketchHitTester::pick(constructionOnly, {5.0, 10.0}, filter);
  CHECK(hit && hit->entity() && hit->entity()->geometryId == constructionId);
  filter.includeConstruction = false;
  filter.includeProjected = true;
  CHECK(!SketchHitTester::pick(constructionOnly, {5.0, 10.0}, filter));

  const auto classificationFor = [](const sketch::Sketch& geometry,
                                    sketch::GeometryId id) {
    const auto classifiedScene = SketchHitSceneAdapter::build(
        geometry, [](sketch::Point point) {
          return SketchHitPoint{point.xMm, point.yMm};
        });
    for (const auto& candidate : classifiedScene.candidates) {
      if (const auto* entity =
              std::get_if<SketchPickEntityRef>(&candidate.target);
          entity && entity->geometryId == id)
        return std::optional<SketchPickEntityRef>{*entity};
    }
    return std::optional<SketchPickEntityRef>{};
  };

  {
    sketch::Sketch malformed;
    malformed.addLine({0.0, 0.0}, {10.0, 0.0});
    malformed.addLine({0.0, 5.0}, {10.0, 5.0});
    const auto first = malformed.lineId(0);
    malformed.setLineDashedById(first, true);
    sketch::Constraint twoGeometryLock;
    twoGeometryLock.id = 101;
    twoGeometryLock.type = sketch::ConstraintType::Lock;
    twoGeometryLock.firstGeometry = first;
    twoGeometryLock.secondGeometry = malformed.lineId(1);
    sketch::SketchTestAccess::installConstraints(
        malformed, {twoGeometryLock});
    const auto classified = classificationFor(malformed, first);
    CHECK(classified && !classified->projected && classified->construction);
  }
  {
    sketch::Sketch malformed;
    malformed.addLine({0.0, 0.0}, {10.0, 0.0});
    malformed.addLine({0.0, 5.0}, {10.0, 5.0});
    const auto first = malformed.lineId(0);
    malformed.setLineDashedById(first, true);
    sketch::Constraint pointLock;
    pointLock.id = 102;
    pointLock.type = sketch::ConstraintType::Lock;
    pointLock.firstGeometry = first;
    pointLock.firstPoint = {malformed.lineId(1), true};
    sketch::SketchTestAccess::installConstraints(malformed, {pointLock});
    auto classified = classificationFor(malformed, first);
    CHECK(classified && !classified->projected && classified->construction);
    pointLock.firstPoint = {};
    pointLock.secondPoint.origin = true;
    sketch::SketchTestAccess::installConstraints(malformed, {pointLock});
    classified = classificationFor(malformed, first);
    CHECK(classified && !classified->projected && classified->construction);
  }
  {
    sketch::Sketch malformed;
    malformed.addLine({0.0, 0.0}, {10.0, 0.0});
    const auto first = malformed.lineId(0);
    malformed.setLineDashedById(first, true);
    sketch::Constraint nonfiniteLock;
    nonfiniteLock.id = 103;
    nonfiniteLock.type = sketch::ConstraintType::Lock;
    nonfiniteLock.firstGeometry = first;
    nonfiniteLock.value = std::numeric_limits<double>::infinity();
    sketch::SketchTestAccess::installConstraints(
        malformed, {nonfiniteLock});
    const auto classified = classificationFor(malformed, first);
    CHECK(classified && !classified->projected && classified->construction);
  }
  {
    sketch::Sketch malformed;
    malformed.addLine({0.0, 0.0}, {10.0, 0.0});
    const auto first = malformed.lineId(0);
    malformed.setLineDashedById(first, true);
    sketch::Constraint zeroIdLock;
    zeroIdLock.type = sketch::ConstraintType::Lock;
    zeroIdLock.firstGeometry = first;
    sketch::SketchTestAccess::installConstraints(malformed, {zeroIdLock});
    const auto classified = classificationFor(malformed, first);
    CHECK(classified && !classified->projected && classified->construction);
  }
}

void dimensionAndDatumTokensRemainTypedAndTransient() {
  sketch::PointReference first{11, true};
  sketch::PointReference second{11, false};
  SketchHitScene scene;
  scene.candidates.push_back(
      {SketchDimensionToken{3, 41, SketchDimensionHitKind::Geometry,
                            sketch::DimensionKind::PointDistance,
                            sketch::kInvalidGeometryId, first, second},
       {{{0.0, 0.0}, {10.0, 0.0}}}, std::nullopt, 8.0, 0, 0, true, true});
  scene.candidates.push_back(
      {SketchPickDatumRef{SketchPickDatumKind::Origin, {0.0, 0.0}}, {},
       SketchHitPoint{0.0, 0.0}, 8.0, 1, 1, true, true});

  SketchPickFilter dimensionsOnly{};
  dimensionsOnly.entities = dimensionsOnly.points =
      dimensionsOnly.datums = dimensionsOnly.projections = false;
  auto hit = SketchHitTester::pick(scene, {5.0, 0.0}, dimensionsOnly);
  CHECK(hit && hit->dimension());
  CHECK(hit->dimension()->transientSlot == 3);
  CHECK(hit->dimension()->dimensionId == 41);
  CHECK(hit->dimension()->firstPoint.lineId == 11);

  SketchPickFilter datumsOnly{};
  datumsOnly.entities = datumsOnly.points = datumsOnly.projections =
      datumsOnly.dimensions = false;
  hit = SketchHitTester::pick(scene, {0.0, 0.0}, datumsOnly);
  CHECK(hit && hit->datum());
  CHECK(hit->datum()->kind == SketchPickDatumKind::Origin);

  SketchHitScene axes;
  axes.candidates.push_back(
      {SketchPickDatumRef{SketchPickDatumKind::XAxis, {0.0, 0.0}},
       {{{-20.0, 0.0}, {20.0, 0.0}}}, std::nullopt, 7.0, 2, 0,
       true, true});
  hit = SketchHitTester::pick(axes, {15.0, 1.0}, datumsOnly);
  CHECK(hit && hit->datum());
  CHECK(hit->datum()->kind == SketchPickDatumKind::XAxis);

  scene.candidates.front().target = SketchDimensionToken{
      3, sketch::kInvalidDimensionId, SketchDimensionHitKind::Geometry,
      sketch::DimensionKind::PointDistance};
  CHECK(!SketchHitTester::pick(scene, {5.0, 0.0}, dimensionsOnly));
}

void originWinsCoincidentEndpointIndependentOfAppendOrder() {
  sketch::Sketch sketch;
  sketch.addLine({0.0, 0.0}, {10.0, 0.0});
  SketchHitSceneOptions options;
  options.origin = SketchHitPoint{0.0, 0.0};
  const auto scene = SketchHitSceneAdapter::build(
      sketch, [](sketch::Point point) {
        return SketchHitPoint{point.xMm, point.yMm};
      }, options);
  auto filter = onlyPoints();
  filter.datums = true;
  const auto hit = SketchHitTester::pick(scene, {0.0, 0.0}, filter);
  CHECK(hit && hit->datum());
  CHECK(hit->datum()->kind == SketchPickDatumKind::Origin);
}

void boxSelectionUsesExactSegmentIntersection() {
  SketchHitScene scene;
  scene.candidates.push_back(lineCandidate(1, 5.0, 9.0, 20, 0));
  scene.candidates.push_back(lineCandidate(2, 20.0, 9.0, 20, 1));
  const auto hits = SketchHitTester::pickInBox(
      scene, {3.0, 3.0}, {7.0, 7.0}, onlyEntities());
  CHECK(hits.size() == 1);
  CHECK(std::get<SketchPickEntityRef>(hits.front()).geometryId == 1);

  sketch::Sketch curves;
  curves.addCircle({0.0, 0.0}, 100.0);
  curves.addArc({200.0, 0.0}, 100.0, 0.0,
                1.5707963267948966);
  const auto circleId = curves.circleId(0);
  const auto arcId = curves.arcId(0);
  const auto curveScene = SketchHitSceneAdapter::build(
      curves, [](sketch::Point point) {
        return SketchHitPoint{point.xMm, point.yMm};
      });
  const auto centerHits = SketchHitTester::pickInBox(
      curveScene, {-1.0, -1.0}, {1.0, 1.0}, onlyEntities());
  CHECK(std::any_of(centerHits.begin(), centerHits.end(), [circleId](const auto& target) {
    const auto* entity = std::get_if<SketchPickEntityRef>(&target);
    return entity && entity->geometryId == circleId;
  }));
  // The box is inside the arc's screen bounding rectangle but does not cross
  // the arc itself. Legacy Canvas selection treated this as a hit.
  const auto arcBoundsHits = SketchHitTester::pickInBox(
      curveScene, {240.0, 40.0}, {250.0, 50.0}, onlyEntities());
  CHECK(std::any_of(arcBoundsHits.begin(), arcBoundsHits.end(), [arcId](const auto& target) {
    const auto* entity = std::get_if<SketchPickEntityRef>(&target);
    return entity && entity->geometryId == arcId;
  }));

  SketchHitScene boundaryScene;
  for (const auto [kind, id] : {
           std::pair{SketchPickEntityKind::Circle, sketch::GeometryId{91}},
           std::pair{SketchPickEntityKind::Arc, sketch::GeometryId{92}}}) {
    SketchPickCandidate candidate;
    candidate.target = SketchPickEntityRef{kind, id};
    candidate.boxHitPolicy = SketchBoxHitPolicy::CurveBoundsOrCenter;
    candidate.selectionBounds = SketchScreenBox{{0.0, 0.0}, {10.0, 10.0}};
    candidate.selectionCenter = SketchHitPoint{100.0, 100.0};
    boundaryScene.candidates.push_back(candidate);
  }
  const auto exactTouch = SketchHitTester::pickInBox(
      boundaryScene, {10.0, 2.0}, {12.0, 4.0}, onlyEntities());
  CHECK(exactTouch.empty());
  const auto nearOverlap = SketchHitTester::pickInBox(
      boundaryScene, {9.999, 2.0}, {12.0, 4.0}, onlyEntities());
  CHECK(nearOverlap.size() == 2);
}

}  // namespace

int main() {
  toleranceAndTieBreaksAreExplicit();
  adapterUsesStableReferencesForRepresentativeGeometry();
  filtersAndInvalidReferencesFailClosed();
  adapterDerivesProjectedStateFromDashedLockContract();
  dimensionAndDatumTokensRemainTypedAndTransient();
  originWinsCoincidentEndpointIndependentOfAppendOrder();
  boxSelectionUsesExactSegmentIntersection();
  std::cout << "sketch_hit_tester_tests passed\n";
  return EXIT_SUCCESS;
}
