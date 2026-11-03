#include "model/PlanarMateSolver.h"

#include "model/Body.h"
#include "model/FeatureTopologySnapshot.h"
#include "model/TopologyReferenceResolver.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <tuple>
#include <utility>

namespace solidar {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;

class WorkLimitExceeded final {};

class WorkBudget final {
 public:
  explicit WorkBudget(std::uint64_t limit, std::uint64_t* published) noexcept
      : limit_(limit), published_(published) {}

  void consume(std::uint64_t units = 1) {
    if (units > limit_ - used_) {
      attempted_ = used_ == std::numeric_limits<std::uint64_t>::max()
                       ? used_
                       : used_ + 1;
      *published_ = attempted_;
      throw WorkLimitExceeded{};
    }
    used_ += units;
    attempted_ = used_;
    *published_ = attempted_;
  }

  [[nodiscard]] std::uint64_t used() const noexcept { return attempted_; }

 private:
  std::uint64_t limit_{};
  std::uint64_t used_{};
  std::uint64_t attempted_{};
  std::uint64_t* published_{};
};

std::uint64_t boundedAdd(std::uint64_t left, std::uint64_t right) noexcept {
  return right > std::numeric_limits<std::uint64_t>::max() - left
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

std::uint64_t boundedMultiply(std::uint64_t left,
                              std::uint64_t right) noexcept {
  return left != 0 &&
                 right > std::numeric_limits<std::uint64_t>::max() / left
             ? std::numeric_limits<std::uint64_t>::max()
             : left * right;
}

void prechargeQuadraticSort(WorkBudget& budget, std::size_t count,
                            std::uint64_t comparisonWork) {
  const auto n = static_cast<std::uint64_t>(count);
  budget.consume(boundedMultiply(boundedMultiply(n, n), comparisonWork));
}

struct PlaneFrame final {
  Vec3 point{};
  Vec3 normal{0.0, 0.0, 1.0};
};

struct AxisFrame final {
  Vec3 point{};
  Vec3 direction{0.0, 0.0, 1.0};
};

struct Pose final {
  Vec3 translation{};
  Quat rotation{1.0, 0.0, 0.0, 0.0};
};

struct ResolvedEndpoint final {
  OccurrenceId directOccurrenceId;
  PlaneFrame planeLocalToDirect;
  AxisFrame axisLocalToDirect;
  Vec3 pointLocalToDirect{};
  std::vector<MateOccurrenceDependencySeal> occurrenceDependencies;
  std::optional<MateDefinitionDependencySeal> definitionDependency;
};

struct ResolvedMate final {
  const AssemblyMate* relation{};
  ResolvedEndpoint first;
  ResolvedEndpoint second;
  std::uint64_t semanticWork{};
};

PlanarMateOrientation relationOrientation(
    const AssemblyMate& relation) noexcept {
  if (const auto* coincident = std::get_if<PlanarCoincidentMate>(&relation))
    return coincident->orientation();
  if (const auto* parallel = std::get_if<PlanarParallelMate>(&relation))
    return parallel->orientation();
  return std::get<PlanarAngleMate>(relation).orientation();
}

double relationOffset(const AssemblyMate& relation) noexcept {
  const auto* coincident = std::get_if<PlanarCoincidentMate>(&relation);
  return coincident ? coincident->offsetMm() : 0.0;
}

struct Node final {
  OccurrenceId id;
  const ComponentOccurrence* occurrence{};
  Pose original;
  Pose solved;
};

bool finite(double value) noexcept { return std::isfinite(value); }

bool validOptions(const MateSolverOptions& options) noexcept {
  return options.isValid();
}

std::uint64_t referenceSemanticWork(const MateEndpoint& endpoint,
                                    WorkBudget& budget) {
  budget.consume();
  std::uint64_t work = boundedAdd(16, endpoint.occurrencePath.depth());
  const auto* reference = endpoint.face.v2();
  if (!reference) return work;
  work = boundedAdd(work, reference->provenance.localName.size());
  work = boundedAdd(work, 8);
  for (const auto& origin : reference->provenance.origins) {
    budget.consume();
    work = boundedAdd(work, boundedAdd(8, origin.localName.size()));
  }
  return work;
}

std::uint64_t referenceSemanticWork(const MateAxisEndpoint& endpoint,
                                    WorkBudget& budget) {
  budget.consume();
  std::uint64_t work = boundedAdd(8, endpoint.occurrencePath.depth());
  const auto* cylinder =
      std::get_if<CylindricalFaceAxis>(&endpoint.reference);
  if (!cylinder) return boundedAdd(work, 1);
  const auto* reference = cylinder->face.v2();
  if (!reference) return work;
  work = boundedAdd(work, reference->provenance.localName.size());
  work = boundedAdd(work, 8);
  for (const auto& origin : reference->provenance.origins) {
    budget.consume();
    work = boundedAdd(work, boundedAdd(8, origin.localName.size()));
  }
  return work;
}

std::uint64_t referenceSemanticWork(const MatePointEndpoint& endpoint,
                                    WorkBudget& budget) {
  budget.consume();
  std::uint64_t work = boundedAdd(8, endpoint.occurrencePath.depth());
  const auto* circle = std::get_if<CircularEdgeCenter>(&endpoint.reference);
  if (!circle) return boundedAdd(work, 1);
  const auto* reference = circle->edge.v2();
  if (!reference) return work;
  work = boundedAdd(work, reference->provenance.localName.size());
  work = boundedAdd(work, 8);
  for (const auto& origin : reference->provenance.origins) {
    budget.consume();
    work = boundedAdd(work, boundedAdd(8, origin.localName.size()));
  }
  return work;
}

std::uint64_t relationSemanticWork(const AssemblyMate& relation,
                                   WorkBudget& budget) {
  if (const auto* axis = std::get_if<AxisConcentricMate>(&relation))
    return boundedAdd(referenceSemanticWork(axis->first(), budget),
                      referenceSemanticWork(axis->second(), budget));
  if (const auto* distance = std::get_if<PointDistanceMate>(&relation))
    return boundedAdd(referenceSemanticWork(distance->first(), budget),
                      referenceSemanticWork(distance->second(), budget));
  return boundedAdd(referenceSemanticWork(*assemblyPlanarMateFirst(relation), budget),
                    referenceSemanticWork(*assemblyPlanarMateSecond(relation),
                                          budget));
}

Vec3 add(Vec3 a, const Vec3& b) noexcept {
  for (std::size_t i = 0; i < 3; ++i) a[i] += b[i];
  return a;
}

Vec3 subtract(Vec3 a, const Vec3& b) noexcept {
  for (std::size_t i = 0; i < 3; ++i) a[i] -= b[i];
  return a;
}

Vec3 scale(Vec3 value, double factor) noexcept {
  for (auto& component : value) component *= factor;
  return value;
}

double dot(const Vec3& a, const Vec3& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
  return {a[1] * b[2] - a[2] * b[1],
          a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

double norm(const Vec3& value) noexcept {
  return std::sqrt(std::max(0.0, dot(value, value)));
}

double robustNorm(const Vec3& value) noexcept {
  return std::hypot(value[0], value[1], value[2]);
}

bool normalize(Vec3& value, WorkBudget* budget = nullptr) {
  if (budget) budget->consume(4);
  const double length = norm(value);
  if (!finite(length) || length <= 1.0e-15) return false;
  if (budget) budget->consume(3);
  value = scale(value, 1.0 / length);
  return finite(value[0]) && finite(value[1]) && finite(value[2]);
}

std::optional<Vec3> pointDirection(const Vec3& first, const Vec3& second,
                                   WorkBudget& budget) {
  budget.consume(12);
  Vec3 direction = subtract(second, first);
  const double length = robustNorm(direction);
  if (!finite(length) || !(length > 0.0)) return std::nullopt;
  direction = scale(direction, 1.0 / length);
  if (!finite(direction[0]) || !finite(direction[1]) ||
      !finite(direction[2]))
    return std::nullopt;
  const double normalizedLength = robustNorm(direction);
  if (!finite(normalizedLength) || !(normalizedLength > 0.0))
    return std::nullopt;
  return direction;
}

std::optional<Vec3> angleTangent(Vec3 moving, Vec3 effectiveFixed,
                                 WorkBudget& budget) {
  budget.consume(20);
  const auto scaleFreeUnit = [](Vec3* value) {
    const double length = robustNorm(*value);
    if (!finite(length) || !(length > 0.0)) return false;
    *value = scale(*value, 1.0 / length);
    if (!finite((*value)[0]) || !finite((*value)[1]) ||
        !finite((*value)[2]))
      return false;
    const double normalizedLength = robustNorm(*value);
    return finite(normalizedLength) && normalizedLength > 0.0;
  };
  if (!scaleFreeUnit(&moving) || !scaleFreeUnit(&effectiveFixed))
    return std::nullopt;
  Vec3 tangent = subtract(
      moving, scale(effectiveFixed, dot(moving, effectiveFixed)));
  if (!scaleFreeUnit(&tangent)) return std::nullopt;
  return tangent;
}

Quat multiply(const Quat& a, const Quat& b) noexcept {
  return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
          a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
          a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
          a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Vec3 rotate(const Quat& quaternion, const Vec3& value) noexcept {
  const Vec3 qv{quaternion[1], quaternion[2], quaternion[3]};
  const Vec3 twice = scale(cross(qv, value), 2.0);
  return add(add(value, scale(twice, quaternion[0])), cross(qv, twice));
}

bool degeneratePointDirection(const ResolvedMate& mate,
                              const std::map<OccurrenceId, Node>& nodes,
                              WorkBudget& budget) {
  budget.consume(8);
  const auto firstFound = nodes.find(mate.first.directOccurrenceId);
  const auto secondFound = nodes.find(mate.second.directOccurrenceId);
  if (firstFound == nodes.end() || secondFound == nodes.end()) return true;
  const auto* firstNode = &firstFound->second;
  const auto* secondNode = &secondFound->second;
  const Vec3 firstPoint = add(
      rotate(firstNode->solved.rotation, mate.first.pointLocalToDirect),
      firstNode->solved.translation);
  const Vec3 secondPoint = add(
      rotate(secondNode->solved.rotation, mate.second.pointLocalToDirect),
      secondNode->solved.translation);
  return !pointDirection(firstPoint, secondPoint, budget);
}

bool degenerateAngleDirection(const ResolvedMate& mate,
                              const std::map<OccurrenceId, Node>& nodes,
                              WorkBudget& budget) {
  budget.consume(12);
  const auto firstFound = nodes.find(mate.first.directOccurrenceId);
  const auto secondFound = nodes.find(mate.second.directOccurrenceId);
  if (firstFound == nodes.end() || secondFound == nodes.end()) return true;
  if (&firstFound->second == &secondFound->second) return false;
  if (firstFound->second.occurrence->flags().grounded &&
      secondFound->second.occurrence->flags().grounded)
    return false;
  const auto& angle = std::get<PlanarAngleMate>(*mate.relation);
  const double factor = angle.orientation() == PlanarMateOrientation::Aligned
                            ? 1.0
                            : -1.0;
  const Vec3 first = rotate(firstFound->second.solved.rotation,
                            mate.first.planeLocalToDirect.normal);
  const Vec3 second = rotate(secondFound->second.solved.rotation,
                             mate.second.planeLocalToDirect.normal);
  return !angleTangent(second, scale(first, factor), budget);
}

bool canonicalize(Pose& pose) noexcept {
  const auto created = RigidTransform3d::create(pose.translation, pose.rotation);
  if (!created) return false;
  pose.translation = created.transform->translationMm();
  pose.rotation = created.transform->quaternionWxyz();
  return true;
}

Pose poseOf(const RigidTransform3d& transform) noexcept {
  return {transform.translationMm(), transform.quaternionWxyz()};
}

RigidTransform3d transformOf(const Pose& pose) noexcept {
  const auto created = RigidTransform3d::create(pose.translation, pose.rotation);
  return created ? *created.transform : RigidTransform3d::identity();
}

PlaneFrame transformPlane(const Pose& pose, const PlaneFrame& plane) noexcept {
  return {add(rotate(pose.rotation, plane.point), pose.translation),
          rotate(pose.rotation, plane.normal)};
}

AxisFrame transformAxis(const Pose& pose, const AxisFrame& axis) noexcept {
  return {add(rotate(pose.rotation, axis.point), pose.translation),
          rotate(pose.rotation, axis.direction)};
}

void canonicalizeDirection(Vec3& direction) noexcept {
  for (const double component : direction) {
    if (std::abs(component) <= 1.0e-15) continue;
    if (component < 0.0) direction = scale(direction, -1.0);
    break;
  }
}

Vec3 canonicalPerpendicularBasis(Vec3 direction) noexcept {
  canonicalizeDirection(direction);
  const Vec3 basis = std::abs(direction[0]) <= std::abs(direction[1]) &&
                             std::abs(direction[0]) <= std::abs(direction[2])
                         ? Vec3{1.0, 0.0, 0.0}
                         : (std::abs(direction[1]) <= std::abs(direction[2])
                                ? Vec3{0.0, 1.0, 0.0}
                                : Vec3{0.0, 0.0, 1.0});
  Vec3 tangent = cross(direction, basis);
  const double length = norm(tangent);
  if (length > 1.0e-15) tangent = scale(tangent, 1.0 / length);
  canonicalizeDirection(tangent);
  return tangent;
}

Quat shortestRotation(Vec3 from, Vec3 to, double maximumAngle,
                      WorkBudget& budget) {
  if (!normalize(from, &budget) || !normalize(to, &budget))
    return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0};
  const double cosine = std::clamp(dot(from, to), -1.0, 1.0);
  double angle = std::acos(cosine);
  if (angle <= 1.0e-15) return {1.0, 0.0, 0.0, 0.0};
  Vec3 axis = cross(from, to);
  if (!normalize(axis, &budget)) {
    const Vec3 basis = std::abs(from[0]) <= std::abs(from[1]) &&
                               std::abs(from[0]) <= std::abs(from[2])
                           ? Vec3{1.0, 0.0, 0.0}
                           : (std::abs(from[1]) <= std::abs(from[2])
                                  ? Vec3{0.0, 1.0, 0.0}
                                  : Vec3{0.0, 0.0, 1.0});
    axis = cross(from, basis);
    if (!normalize(axis, &budget))
      return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0};
  }
  angle = std::min(angle, maximumAngle);
  const double half = angle * 0.5;
  const double sine = std::sin(half);
  return {std::cos(half), axis[0] * sine, axis[1] * sine,
          axis[2] * sine};
}

Quat angleConeRotation(Vec3 from, Vec3 to, double maximumAngle,
                       WorkBudget& budget) {
  budget.consume(20);
  const auto scaleFreeUnit = [](Vec3* value) {
    const double length = robustNorm(*value);
    if (!finite(length) || !(length > 0.0)) return false;
    *value = scale(*value, 1.0 / length);
    return finite((*value)[0]) && finite((*value)[1]) &&
           finite((*value)[2]) && robustNorm(*value) > 0.0;
  };
  if (!scaleFreeUnit(&from) || !scaleFreeUnit(&to))
    return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0};
  Vec3 axis = cross(from, to);
  const double sine = robustNorm(axis);
  const double cosine = std::clamp(dot(from, to), -1.0, 1.0);
  if (finite(sine) && sine == 0.0 && cosine > 0.0)
    return {1.0, 0.0, 0.0, 0.0};
  if (!finite(sine) || !(sine > 0.0) || !scaleFreeUnit(&axis))
    return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0};
  const double angle = std::min(std::atan2(sine, cosine), maximumAngle);
  if (!finite(angle) || !(angle > 0.0))
    return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0};
  const double half = angle * 0.5;
  const double halfSine = std::sin(half);
  return {std::cos(half), axis[0] * halfSine, axis[1] * halfSine,
          axis[2] * halfSine};
}

MateSolveStatus statusForEndpointFailure(MateEndpointFailure failure) noexcept {
  switch (failure) {
    case MateEndpointFailure::Ambiguous:
      return MateSolveStatus::Ambiguous;
    case MateEndpointFailure::Missing:
    case MateEndpointFailure::BodyMissing:
    case MateEndpointFailure::FeatureMissing:
    case MateEndpointFailure::TopologyUnavailable:
      return MateSolveStatus::Missing;
    case MateEndpointFailure::PathMissing:
    case MateEndpointFailure::TargetIsNotPart:
    case MateEndpointFailure::Suppressed:
    case MateEndpointFailure::NonPlanar:
    case MateEndpointFailure::NonCylindrical:
    case MateEndpointFailure::NonCircular:
    case MateEndpointFailure::TopologyEvaluationFailed:
    case MateEndpointFailure::InvalidDatumAxis:
    case MateEndpointFailure::InvalidDatumPoint:
    case MateEndpointFailure::Degenerate:
    case MateEndpointFailure::DegenerateDirection:
    case MateEndpointFailure::InvalidFaceOrientation:
      return MateSolveStatus::Broken;
    case MateEndpointFailure::NonAuthoritativeTopology:
      return MateSolveStatus::Stale;
    case MateEndpointFailure::V1Unsupported:
    case MateEndpointFailure::InvalidOwner:
    case MateEndpointFailure::Unexpected:
    case MateEndpointFailure::None:
      return MateSolveStatus::InvalidInput;
  }
  return MateSolveStatus::InvalidInput;
}

struct ResolvedOccurrencePath final {
  OccurrenceId directOccurrenceId;
  const PartDefinition* terminalPart{};
  Pose tailPose;
  std::vector<MateOccurrenceDependencySeal> dependencies;
};

MateEndpointFailure resolveOccurrencePath(
    const ProductDocument& product, const AssemblyDefinition& owner,
    const OccurrencePath& path, ResolvedOccurrencePath& output,
    WorkBudget& budget) {
  if (path.entries().empty()) return MateEndpointFailure::PathMissing;
  const AssemblyDefinition* assembly = &owner;
  RigidTransform3d tail = RigidTransform3d::identity();
  for (std::size_t index = 0; index < path.depth(); ++index) {
    budget.consume();
    const auto* occurrence = assembly->find(path.entries()[index]);
    if (!occurrence) return MateEndpointFailure::PathMissing;
    output.dependencies.push_back(
        {assembly->id(), occurrence->id(), occurrence->targetDefinitionId(),
         occurrence->placement(), occurrence->flags(),
         occurrence->displayNameUtf8()});
    if (occurrence->flags().suppressed) return MateEndpointFailure::Suppressed;
    if (index == 0) output.directOccurrenceId = occurrence->id();
    if (index > 0) {
      const auto composed =
          RigidTransform3d::compose(tail, occurrence->placement());
      if (!composed) return MateEndpointFailure::Unexpected;
      tail = *composed.transform;
    }
    const auto* terminalPart = product.definitions().findPart(
        occurrence->targetDefinitionId());
    const auto* nested = product.definitions().findAssembly(
        occurrence->targetDefinitionId());
    const bool last = index + 1 == path.depth();
    if (last) {
      if (!terminalPart) return MateEndpointFailure::TargetIsNotPart;
      output.terminalPart = terminalPart;
    } else {
      if (!nested) return MateEndpointFailure::PathMissing;
      assembly = nested;
    }
  }
  if (!output.terminalPart) return MateEndpointFailure::TargetIsNotPart;
  output.tailPose = poseOf(tail);
  return MateEndpointFailure::None;
}

MateEndpointFailure resolveEndpoint(const ProductDocument& product,
                                    const AssemblyDefinition& owner,
                                    const MateEndpoint& endpoint,
                                    ResolvedEndpoint& output,
                                    WorkBudget& budget) {
  budget.consume();
  const auto* reference = endpoint.face.v2();
  if (!reference) return MateEndpointFailure::V1Unsupported;
  if (!reference->hasValidOwner() ||
      reference->provenance.kind != TopologyKind::Face)
    return MateEndpointFailure::InvalidOwner;
  ResolvedOccurrencePath path;
  const auto pathFailure = resolveOccurrencePath(
      product, owner, endpoint.occurrencePath, path, budget);
  if (pathFailure != MateEndpointFailure::None) return pathFailure;
  output.directOccurrenceId = path.directOccurrenceId;
  output.occurrenceDependencies = std::move(path.dependencies);
  output.definitionDependency = MateDefinitionDependencySeal{
      path.terminalPart->id(), path.terminalPart->stateId(), std::nullopt};

  const Body* body = path.terminalPart->document().findBody(reference->bodyId);
  if (!body) return MateEndpointFailure::BodyMissing;
  if (!path.terminalPart->document().findFeature(
          reference->bodyId, reference->provenance.featureId))
    return MateEndpointFailure::FeatureMissing;
  const auto snapshot = body->topologySnapshot(reference->provenance.featureId);
  if (!snapshot || !snapshot->topologyIndex())
    return MateEndpointFailure::TopologyUnavailable;
  if (!snapshot->v2Authoritative())
    return MateEndpointFailure::NonAuthoritativeTopology;
  output.definitionDependency->topology = MateTopologyDependencySeal{
      snapshot->bodyId(), snapshot->featureId(), snapshot->revision(),
      snapshot->topologyIndex() != nullptr, snapshot->v2Authoritative()};
  budget.consume(snapshot->topologyIndex()->faceCount());
  const auto resolution = snapshot->topologyIndex()->resolveFace(*reference);
  if (!resolution) {
    switch (resolution.failure) {
      case TopologyResolutionFailure::Missing:
        return MateEndpointFailure::Missing;
      case TopologyResolutionFailure::Ambiguous:
        return MateEndpointFailure::Ambiguous;
      case TopologyResolutionFailure::InvalidReference:
        return MateEndpointFailure::InvalidOwner;
      default:
        return MateEndpointFailure::Unexpected;
    }
  }
  if (!resolution.subshape || resolution.subshape->IsNull())
    return MateEndpointFailure::TopologyEvaluationFailed;
  PlaneFrame local;
  try {
    const auto orientation = resolution.subshape->Orientation();
    if (orientation != TopAbs_FORWARD && orientation != TopAbs_REVERSED)
      return MateEndpointFailure::InvalidFaceOrientation;
    BRepAdaptor_Surface adaptor(*resolution.subshape, true);
    if (adaptor.GetType() != GeomAbs_Plane)
      return MateEndpointFailure::NonPlanar;
    const gp_Pln plane = adaptor.Plane();
    local.point = {plane.Location().X(), plane.Location().Y(),
                   plane.Location().Z()};
    local.normal = {plane.Axis().Direction().X(),
                    plane.Axis().Direction().Y(),
                    plane.Axis().Direction().Z()};
    if (orientation == TopAbs_REVERSED)
      local.normal = scale(local.normal, -1.0);
    if (!finite(local.point[0]) || !finite(local.point[1]) ||
        !finite(local.point[2]))
      return MateEndpointFailure::Degenerate;
    if (!normalize(local.normal, &budget))
      return MateEndpointFailure::Degenerate;
  } catch (const Standard_Failure&) {
    return MateEndpointFailure::TopologyEvaluationFailed;
  }
  output.planeLocalToDirect = transformPlane(path.tailPose, local);
  if (!finite(output.planeLocalToDirect.point[0]) ||
      !finite(output.planeLocalToDirect.point[1]) ||
      !finite(output.planeLocalToDirect.point[2]))
    return MateEndpointFailure::Degenerate;
  if (!normalize(output.planeLocalToDirect.normal, &budget))
    return MateEndpointFailure::Degenerate;
  return MateEndpointFailure::None;
}

MateEndpointFailure resolveAxisEndpoint(
    const ProductDocument& product, const AssemblyDefinition& owner,
    const MateAxisEndpoint& endpoint, ResolvedEndpoint& output,
    WorkBudget& budget) {
  budget.consume();
  ResolvedOccurrencePath path;
  const auto pathFailure = resolveOccurrencePath(
      product, owner, endpoint.occurrencePath, path, budget);
  if (pathFailure != MateEndpointFailure::None) return pathFailure;
  output.directOccurrenceId = path.directOccurrenceId;
  output.occurrenceDependencies = std::move(path.dependencies);
  output.definitionDependency = MateDefinitionDependencySeal{
      path.terminalPart->id(), path.terminalPart->stateId(), std::nullopt};

  AxisFrame local;
  if (const auto* datum = std::get_if<LocalDatumAxis>(&endpoint.reference)) {
    if (!isValidLocalDatumAxis(*datum))
      return MateEndpointFailure::InvalidDatumAxis;
    local.point = {0.0, 0.0, 0.0};
    local.direction = *datum == LocalDatumAxis::X
                          ? Vec3{1.0, 0.0, 0.0}
                          : (*datum == LocalDatumAxis::Y
                                 ? Vec3{0.0, 1.0, 0.0}
                                 : Vec3{0.0, 0.0, 1.0});
  } else {
    const auto& face =
        std::get<CylindricalFaceAxis>(endpoint.reference).face;
    const auto* reference = face.v2();
    if (!reference) return MateEndpointFailure::V1Unsupported;
    if (!reference->hasValidOwner() ||
        reference->provenance.kind != TopologyKind::Face)
      return MateEndpointFailure::InvalidOwner;
    const Body* body =
        path.terminalPart->document().findBody(reference->bodyId);
    if (!body) return MateEndpointFailure::BodyMissing;
    if (!path.terminalPart->document().findFeature(
            reference->bodyId, reference->provenance.featureId))
      return MateEndpointFailure::FeatureMissing;
    const auto snapshot =
        body->topologySnapshot(reference->provenance.featureId);
    if (!snapshot || !snapshot->topologyIndex())
      return MateEndpointFailure::TopologyUnavailable;
    if (!snapshot->v2Authoritative())
      return MateEndpointFailure::NonAuthoritativeTopology;
    output.definitionDependency->topology = MateTopologyDependencySeal{
        snapshot->bodyId(), snapshot->featureId(), snapshot->revision(),
        snapshot->topologyIndex() != nullptr, snapshot->v2Authoritative()};
    budget.consume(snapshot->topologyIndex()->faceCount());
    const auto resolution = snapshot->topologyIndex()->resolveFace(*reference);
    if (!resolution) {
      switch (resolution.failure) {
        case TopologyResolutionFailure::Missing:
          return MateEndpointFailure::Missing;
        case TopologyResolutionFailure::Ambiguous:
          return MateEndpointFailure::Ambiguous;
        case TopologyResolutionFailure::InvalidReference:
          return MateEndpointFailure::InvalidOwner;
        default:
          return MateEndpointFailure::Unexpected;
      }
    }
    if (!resolution.subshape || resolution.subshape->IsNull())
      return MateEndpointFailure::TopologyEvaluationFailed;
    try {
      BRepAdaptor_Surface adaptor(*resolution.subshape, true);
      if (adaptor.GetType() != GeomAbs_Cylinder)
        return MateEndpointFailure::NonCylindrical;
      const auto cylinder = adaptor.Cylinder();
      if (!finite(cylinder.Radius()) || cylinder.Radius() <= 0.0)
        return MateEndpointFailure::Degenerate;
      local.point = {cylinder.Location().X(), cylinder.Location().Y(),
                     cylinder.Location().Z()};
      local.direction = {cylinder.Axis().Direction().X(),
                         cylinder.Axis().Direction().Y(),
                         cylinder.Axis().Direction().Z()};
    } catch (const Standard_Failure&) {
      return MateEndpointFailure::TopologyEvaluationFailed;
    }
    if (!finite(local.point[0]) || !finite(local.point[1]) ||
        !finite(local.point[2]))
      return MateEndpointFailure::Degenerate;
    if (!normalize(local.direction, &budget))
      return MateEndpointFailure::Degenerate;
    canonicalizeDirection(local.direction);
    local.point = subtract(
        local.point, scale(local.direction, dot(local.point, local.direction)));
  }
  if (!normalize(local.direction, &budget))
    return MateEndpointFailure::Degenerate;
  canonicalizeDirection(local.direction);
  output.axisLocalToDirect = transformAxis(path.tailPose, local);
  if (!finite(output.axisLocalToDirect.point[0]) ||
      !finite(output.axisLocalToDirect.point[1]) ||
      !finite(output.axisLocalToDirect.point[2]) ||
      !normalize(output.axisLocalToDirect.direction, &budget))
    return MateEndpointFailure::Degenerate;
  canonicalizeDirection(output.axisLocalToDirect.direction);
  return MateEndpointFailure::None;
}

MateEndpointFailure resolvePointEndpoint(
    const ProductDocument& product, const AssemblyDefinition& owner,
    const MatePointEndpoint& endpoint, ResolvedEndpoint& output,
    WorkBudget& budget) {
  budget.consume();
  ResolvedOccurrencePath path;
  const auto pathFailure = resolveOccurrencePath(
      product, owner, endpoint.occurrencePath, path, budget);
  if (pathFailure != MateEndpointFailure::None) return pathFailure;
  output.directOccurrenceId = path.directOccurrenceId;
  output.occurrenceDependencies = std::move(path.dependencies);
  output.definitionDependency = MateDefinitionDependencySeal{
      path.terminalPart->id(), path.terminalPart->stateId(), std::nullopt};

  Vec3 local{};
  if (const auto* datum = std::get_if<LocalDatumPoint>(&endpoint.reference)) {
    if (!isValidLocalDatumPoint(*datum))
      return MateEndpointFailure::InvalidDatumPoint;
  } else {
    const auto& edge =
        std::get<CircularEdgeCenter>(endpoint.reference).edge;
    const auto* reference = edge.v2();
    if (!reference) return MateEndpointFailure::V1Unsupported;
    if (!reference->hasValidOwner() ||
        reference->provenance.kind != TopologyKind::Edge)
      return MateEndpointFailure::InvalidOwner;
    const Body* body =
        path.terminalPart->document().findBody(reference->bodyId);
    if (!body) return MateEndpointFailure::BodyMissing;
    if (!path.terminalPart->document().findFeature(
            reference->bodyId, reference->provenance.featureId))
      return MateEndpointFailure::FeatureMissing;
    const auto snapshot =
        body->topologySnapshot(reference->provenance.featureId);
    if (!snapshot || !snapshot->topologyIndex())
      return MateEndpointFailure::TopologyUnavailable;
    if (!snapshot->v2Authoritative())
      return MateEndpointFailure::NonAuthoritativeTopology;
    output.definitionDependency->topology = MateTopologyDependencySeal{
        snapshot->bodyId(), snapshot->featureId(), snapshot->revision(),
        snapshot->topologyIndex() != nullptr, snapshot->v2Authoritative()};
    budget.consume(snapshot->topologyIndex()->edgeCount());
    const auto resolution = snapshot->topologyIndex()->resolveEdge(*reference);
    if (!resolution) {
      switch (resolution.failure) {
        case TopologyResolutionFailure::Missing:
          return MateEndpointFailure::Missing;
        case TopologyResolutionFailure::Ambiguous:
          return MateEndpointFailure::Ambiguous;
        case TopologyResolutionFailure::InvalidReference:
          return MateEndpointFailure::InvalidOwner;
        default:
          return MateEndpointFailure::Unexpected;
      }
    }
    if (!resolution.subshape || resolution.subshape->IsNull())
      return MateEndpointFailure::TopologyEvaluationFailed;
    try {
      BRepAdaptor_Curve adaptor(*resolution.subshape);
      if (adaptor.GetType() != GeomAbs_Circle)
        return MateEndpointFailure::NonCircular;
      const auto circle = adaptor.Circle();
      if (!finite(circle.Radius()) || circle.Radius() <= 0.0)
        return MateEndpointFailure::Degenerate;
      local = {circle.Location().X(), circle.Location().Y(),
               circle.Location().Z()};
      if (!finite(local[0]) || !finite(local[1]) || !finite(local[2]))
        return MateEndpointFailure::Degenerate;
    } catch (const Standard_Failure&) {
      return MateEndpointFailure::TopologyEvaluationFailed;
    }
  }
  output.pointLocalToDirect =
      add(rotate(path.tailPose.rotation, local), path.tailPose.translation);
  if (!finite(output.pointLocalToDirect[0]) ||
      !finite(output.pointLocalToDirect[1]) ||
      !finite(output.pointLocalToDirect[2]))
    return MateEndpointFailure::Degenerate;
  return MateEndpointFailure::None;
}

Node* node(std::map<OccurrenceId, Node>& nodes, OccurrenceId id) noexcept {
  const auto found = nodes.find(id);
  return found == nodes.end() ? nullptr : &found->second;
}

const Node* node(const std::map<OccurrenceId, Node>& nodes,
                 OccurrenceId id) noexcept {
  const auto found = nodes.find(id);
  return found == nodes.end() ? nullptr : &found->second;
}

MateResidual residual(const ResolvedMate& mate,
                      const std::map<OccurrenceId, Node>& nodes,
                      WorkBudget& budget) {
  budget.consume();
  const auto kind = assemblyMateKind(*mate.relation);
  if (kind == AssemblyMateKind::PointDistance) {
    const Vec3 first = add(
        rotate(node(nodes, mate.first.directOccurrenceId)->solved.rotation,
               mate.first.pointLocalToDirect),
        node(nodes, mate.first.directOccurrenceId)->solved.translation);
    const Vec3 second = add(
        rotate(node(nodes, mate.second.directOccurrenceId)->solved.rotation,
               mate.second.pointLocalToDirect),
        node(nodes, mate.second.directOccurrenceId)->solved.translation);
    const double actual = robustNorm(subtract(second, first));
    const double target = std::get<PointDistanceMate>(*mate.relation).distanceMm();
    return {assemblyMateId(*mate.relation), std::abs(actual - target), 0.0};
  }
  if (kind == AssemblyMateKind::AxisConcentric) {
    auto first = transformAxis(
        node(nodes, mate.first.directOccurrenceId)->solved,
        mate.first.axisLocalToDirect);
    auto second = transformAxis(
        node(nodes, mate.second.directOccurrenceId)->solved,
        mate.second.axisLocalToDirect);
    if (!normalize(first.direction, &budget) ||
        !normalize(second.direction, &budget))
      return {assemblyMateId(*mate.relation),
              std::numeric_limits<double>::quiet_NaN(),
              std::numeric_limits<double>::quiet_NaN()};
    const double cosine =
        std::clamp(std::abs(dot(first.direction, second.direction)), 0.0, 1.0);
    const Vec3 directionCross = cross(first.direction, second.direction);
    const double crossLength = norm(directionCross);
    const Vec3 delta = subtract(second.point, first.point);
    const double distance =
        crossLength <= 1.0e-12
            ? norm(cross(delta, first.direction))
            : std::abs(dot(delta, scale(directionCross, 1.0 / crossLength)));
    return {assemblyMateId(*mate.relation), distance, std::acos(cosine)};
  }
  const PlaneFrame first =
      transformPlane(node(nodes, mate.first.directOccurrenceId)->solved,
                     mate.first.planeLocalToDirect);
  const PlaneFrame second =
      transformPlane(node(nodes, mate.second.directOccurrenceId)->solved,
                     mate.second.planeLocalToDirect);
  if (kind == AssemblyMateKind::PlanarAngle) {
    const auto& angle = std::get<PlanarAngleMate>(*mate.relation);
    const double factor = angle.orientation() == PlanarMateOrientation::Aligned
                              ? 1.0
                              : -1.0;
    const Vec3 effectiveFirst = scale(first.normal, factor);
    const double cosine =
        std::clamp(dot(second.normal, effectiveFirst), -1.0, 1.0);
    const double sine = robustNorm(cross(second.normal, effectiveFirst));
    const double actual = std::atan2(sine, cosine);
    return {angle.id(), 0.0, std::abs(actual - angle.angleRad())};
  }
  if (kind == AssemblyMateKind::PlanarPerpendicular) {
    const double cosine =
        std::clamp(dot(first.normal, second.normal), -1.0, 1.0);
    return {assemblyMateId(*mate.relation), 0.0,
            std::asin(std::clamp(std::abs(cosine), 0.0, 1.0))};
  }
  const double factor = relationOrientation(*mate.relation) ==
                                PlanarMateOrientation::Aligned
                            ? 1.0
                            : -1.0;
  const double cosine =
      std::clamp(dot(second.normal, scale(first.normal, factor)), -1.0, 1.0);
  const double linear = kind == AssemblyMateKind::PlanarCoincident
                            ? std::abs(dot(subtract(second.point, first.point),
                                           first.normal) -
                                       relationOffset(*mate.relation))
                            : 0.0;
  return {assemblyMateId(*mate.relation), linear, std::acos(cosine)};
}

Vec3 perpendicularTarget(Vec3 movingNormal, Vec3 fixedNormal,
                         WorkBudget& budget) {
  Vec3 projected = subtract(
      movingNormal, scale(fixedNormal, dot(movingNormal, fixedNormal)));
  if (normalize(projected, &budget)) return projected;
  Vec3 canonicalFixed = fixedNormal;
  for (const double component : canonicalFixed) {
    if (std::abs(component) <= 1.0e-15) continue;
    if (component < 0.0) canonicalFixed = scale(canonicalFixed, -1.0);
    break;
  }
  const Vec3 basis = std::abs(canonicalFixed[0]) <=
                                 std::abs(canonicalFixed[1]) &&
                             std::abs(canonicalFixed[0]) <=
                                 std::abs(canonicalFixed[2])
                         ? Vec3{1.0, 0.0, 0.0}
                         : (std::abs(canonicalFixed[1]) <=
                                    std::abs(canonicalFixed[2])
                                ? Vec3{0.0, 1.0, 0.0}
                                : Vec3{0.0, 0.0, 1.0});
  projected = cross(canonicalFixed, basis);
  if (!normalize(projected, &budget))
    return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
  for (const double component : projected) {
    if (std::abs(component) <= 1.0e-15) continue;
    if (component < 0.0) projected = scale(projected, -1.0);
    break;
  }
  if (dot(movingNormal, canonicalFixed) < 0.0)
    projected = scale(projected, -1.0);
  return projected;
}

bool projectMate(const ResolvedMate& mate, std::map<OccurrenceId, Node>& nodes,
                 const MateSolverOptions& options, WorkBudget& budget) {
  budget.consume();
  Node* firstNode = node(nodes, mate.first.directOccurrenceId);
  Node* secondNode = node(nodes, mate.second.directOccurrenceId);
  if (!firstNode || !secondNode) return false;
  if (firstNode == secondNode) return true;
  const bool firstGrounded = firstNode->occurrence->flags().grounded;
  const bool secondGrounded = secondNode->occurrence->flags().grounded;
  if (firstGrounded && secondGrounded) return true;

  const auto kind = assemblyMateKind(*mate.relation);
  bool moveSecond = false;
  if (firstGrounded)
    moveSecond = true;
  else if (secondGrounded)
    moveSecond = false;
  else
    moveSecond = firstNode->id < secondNode->id;

  if (kind == AssemblyMateKind::PointDistance) {
    const Vec3 first = add(rotate(firstNode->solved.rotation,
                                  mate.first.pointLocalToDirect),
                           firstNode->solved.translation);
    const Vec3 second = add(rotate(secondNode->solved.rotation,
                                   mate.second.pointLocalToDirect),
                            secondNode->solved.translation);
    const Vec3 delta = subtract(second, first);
    const double actual = robustNorm(delta);
    auto direction = pointDirection(first, second, budget);
    if (!finite(actual) || !direction) return false;
    const double target =
        std::get<PointDistanceMate>(*mate.relation).distanceMm();
    double correction = target - actual;
    correction = std::clamp(correction, -options.maximumTranslationStepMm,
                            options.maximumTranslationStepMm);
    Node* moving = moveSecond ? secondNode : firstNode;
    if (!moveSecond) *direction = scale(*direction, -1.0);
    moving->solved.translation =
        add(moving->solved.translation, scale(*direction, correction));
    return canonicalize(moving->solved);
  }

  if (kind == AssemblyMateKind::AxisConcentric) {
    AxisFrame first =
        transformAxis(firstNode->solved, mate.first.axisLocalToDirect);
    AxisFrame second =
        transformAxis(secondNode->solved, mate.second.axisLocalToDirect);
    Node* moving = moveSecond ? secondNode : firstNode;
    const AxisFrame movingLocal = moveSecond ? mate.second.axisLocalToDirect
                                             : mate.first.axisLocalToDirect;
    const Vec3 movingDirection =
        moveSecond ? second.direction : first.direction;
    Vec3 targetDirection = moveSecond ? first.direction : second.direction;
    if (dot(movingDirection, targetDirection) < 0.0)
      targetDirection = scale(targetDirection, -1.0);
    const Vec3 anchor = moveSecond ? second.point : first.point;
    const Quat delta = shortestRotation(movingDirection, targetDirection,
                                        options.maximumAngularStepRad, budget);
    moving->solved.rotation = multiply(delta, moving->solved.rotation);
    moving->solved.translation =
        subtract(anchor,
                 rotate(moving->solved.rotation, movingLocal.point));
    if (!canonicalize(moving->solved)) return false;
    first = transformAxis(firstNode->solved, mate.first.axisLocalToDirect);
    second = transformAxis(secondNode->solved, mate.second.axisLocalToDirect);
    Vec3 fixedDirection = moveSecond ? first.direction : second.direction;
    if (!normalize(fixedDirection, &budget)) return false;
    const Vec3 movingPoint = moveSecond ? second.point : first.point;
    const Vec3 fixedPoint = moveSecond ? first.point : second.point;
    const Vec3 toFixed = subtract(fixedPoint, movingPoint);
    Vec3 correction = subtract(
        toFixed, scale(fixedDirection, dot(toFixed, fixedDirection)));
    const double correctionLength = norm(correction);
    if (!finite(correctionLength)) return false;
    if (correctionLength > options.maximumTranslationStepMm)
      correction = scale(correction,
                         options.maximumTranslationStepMm / correctionLength);
    moving->solved.translation = add(moving->solved.translation, correction);
    return canonicalize(moving->solved);
  }

  PlaneFrame first =
      transformPlane(firstNode->solved, mate.first.planeLocalToDirect);
  PlaneFrame second =
      transformPlane(secondNode->solved, mate.second.planeLocalToDirect);
  Node* moving = moveSecond ? secondNode : firstNode;
  const PlaneFrame movingLocal =
      moveSecond ? mate.second.planeLocalToDirect
                 : mate.first.planeLocalToDirect;
  const Vec3 movingNormal = moveSecond ? second.normal : first.normal;
  const Vec3 fixedNormal = moveSecond ? first.normal : second.normal;
  Vec3 targetNormal{};
  if (kind == AssemblyMateKind::PlanarPerpendicular) {
    targetNormal = perpendicularTarget(movingNormal, fixedNormal, budget);
  } else if (kind == AssemblyMateKind::PlanarAngle) {
    const auto& angle = std::get<PlanarAngleMate>(*mate.relation);
    const double factor = angle.orientation() == PlanarMateOrientation::Aligned
                              ? 1.0
                              : -1.0;
    const Vec3 effectiveFixed = scale(fixedNormal, factor);
    budget.consume(12);
    const double currentAngle = std::atan2(
        robustNorm(cross(movingNormal, effectiveFixed)),
        std::clamp(dot(movingNormal, effectiveFixed), -1.0, 1.0));
    if (finite(currentAngle) &&
        std::abs(currentAngle - angle.angleRad()) <=
            options.angularToleranceRad)
      return true;
    const auto tangent = angleTangent(movingNormal, effectiveFixed, budget);
    if (!tangent) return false;
    targetNormal = add(scale(effectiveFixed, std::cos(angle.angleRad())),
                       scale(*tangent, std::sin(angle.angleRad())));
    if (!normalize(targetNormal, &budget)) return false;
  } else {
    const double factor = relationOrientation(*mate.relation) ==
                                  PlanarMateOrientation::Aligned
                              ? 1.0
                              : -1.0;
    targetNormal = scale(fixedNormal, factor);
  }
  const Vec3 pivot = moveSecond ? second.point : first.point;
  const Quat delta =
      kind == AssemblyMateKind::PlanarAngle
          ? angleConeRotation(movingNormal, targetNormal,
                              options.maximumAngularStepRad, budget)
          : shortestRotation(movingNormal, targetNormal,
                             options.maximumAngularStepRad, budget);
  moving->solved.rotation = multiply(delta, moving->solved.rotation);
  moving->solved.translation =
      subtract(pivot, rotate(moving->solved.rotation, movingLocal.point));
  if (!canonicalize(moving->solved)) return false;

  if (kind != AssemblyMateKind::PlanarCoincident) return true;

  first = transformPlane(firstNode->solved, mate.first.planeLocalToDirect);
  second =
      transformPlane(secondNode->solved, mate.second.planeLocalToDirect);
  const double distance = dot(subtract(second.point, first.point), first.normal);
  const double offset = relationOffset(*mate.relation);
  double correction = moveSecond ? offset - distance : distance - offset;
  correction = std::clamp(correction, -options.maximumTranslationStepMm,
                          options.maximumTranslationStepMm);
  moving->solved.translation =
      add(moving->solved.translation, scale(first.normal, correction));
  return canonicalize(moving->solved);
}

std::vector<double> constraintVector(
    const std::vector<ResolvedMate>& mates,
    const std::map<OccurrenceId, Node>& nodes,
    const MateSolverOptions& options, WorkBudget& budget) {
  std::vector<double> values;
  values.reserve(mates.size() * 5);
  for (const auto& mate : mates) {
    budget.consume();
    const auto first =
        (assemblyMateKind(*mate.relation) == AssemblyMateKind::AxisConcentric ||
         assemblyMateKind(*mate.relation) == AssemblyMateKind::PointDistance)
            ? PlaneFrame{}
            : transformPlane(
                  node(nodes, mate.first.directOccurrenceId)->solved,
                  mate.first.planeLocalToDirect);
    const auto second =
        (assemblyMateKind(*mate.relation) == AssemblyMateKind::AxisConcentric ||
         assemblyMateKind(*mate.relation) == AssemblyMateKind::PointDistance)
            ? PlaneFrame{}
            : transformPlane(
                  node(nodes, mate.second.directOccurrenceId)->solved,
                  mate.second.planeLocalToDirect);
    const auto kind = assemblyMateKind(*mate.relation);
    if (kind == AssemblyMateKind::PointDistance) {
      const Vec3 firstPoint = add(
          rotate(node(nodes, mate.first.directOccurrenceId)->solved.rotation,
                 mate.first.pointLocalToDirect),
          node(nodes, mate.first.directOccurrenceId)->solved.translation);
      const Vec3 secondPoint = add(
          rotate(node(nodes, mate.second.directOccurrenceId)->solved.rotation,
                 mate.second.pointLocalToDirect),
          node(nodes, mate.second.directOccurrenceId)->solved.translation);
      values.push_back(
          (robustNorm(subtract(secondPoint, firstPoint)) -
           std::get<PointDistanceMate>(*mate.relation).distanceMm()) /
          options.linearToleranceMm);
      continue;
    }
    if (kind == AssemblyMateKind::AxisConcentric) {
      auto firstAxis = transformAxis(
          node(nodes, mate.first.directOccurrenceId)->solved,
          mate.first.axisLocalToDirect);
      auto secondAxis = transformAxis(
          node(nodes, mate.second.directOccurrenceId)->solved,
          mate.second.axisLocalToDirect);
      if (!normalize(firstAxis.direction, &budget) ||
          !normalize(secondAxis.direction, &budget)) {
        values.push_back(std::numeric_limits<double>::quiet_NaN());
        continue;
      }
      if (dot(firstAxis.direction, secondAxis.direction) < 0.0)
        secondAxis.direction = scale(secondAxis.direction, -1.0);
      const Vec3 angular = cross(secondAxis.direction, firstAxis.direction);
      for (const double component : angular)
        values.push_back(component / options.angularToleranceRad);
      values.push_back((dot(secondAxis.direction, firstAxis.direction) - 1.0) /
                       options.angularToleranceRad);
      const Vec3 tangent1 = canonicalPerpendicularBasis(firstAxis.direction);
      const Vec3 tangent2 = cross(firstAxis.direction, tangent1);
      const Vec3 displacement = subtract(secondAxis.point, firstAxis.point);
      values.push_back(dot(displacement, tangent1) /
                       options.linearToleranceMm);
      values.push_back(dot(displacement, tangent2) /
                       options.linearToleranceMm);
      continue;
    }
    if (kind == AssemblyMateKind::PlanarAngle) {
      const auto& angle = std::get<PlanarAngleMate>(*mate.relation);
      const double factor =
          angle.orientation() == PlanarMateOrientation::Aligned ? 1.0 : -1.0;
      const Vec3 effectiveFirst = scale(first.normal, factor);
      const double cosine =
          std::clamp(dot(second.normal, effectiveFirst), -1.0, 1.0);
      const double sine = robustNorm(cross(second.normal, effectiveFirst));
      values.push_back((std::atan2(sine, cosine) - angle.angleRad()) /
                       options.angularToleranceRad);
      continue;
    }
    if (kind == AssemblyMateKind::PlanarPerpendicular) {
      values.push_back(dot(second.normal, first.normal) /
                       options.angularToleranceRad);
      continue;
    }
    const double factor = relationOrientation(*mate.relation) ==
                                  PlanarMateOrientation::Aligned
                              ? 1.0
                              : -1.0;
    const Vec3 angular = cross(second.normal, scale(first.normal, factor));
    for (const double component : angular)
      values.push_back(component / options.angularToleranceRad);
    values.push_back(
        (dot(second.normal, scale(first.normal, factor)) - 1.0) /
        options.angularToleranceRad);
    if (kind == AssemblyMateKind::PlanarCoincident)
      values.push_back(
          (dot(subtract(second.point, first.point), first.normal) -
           relationOffset(*mate.relation)) /
          options.linearToleranceMm);
  }
  return values;
}

enum class RankStatus : std::uint8_t { Ranked, NonFinite, ResourceFailure };

struct RankResult final {
  RankStatus status{RankStatus::Ranked};
  std::size_t rank{};
};

RankResult matrixRank(const std::vector<std::vector<double>>& input,
                      double tolerance, WorkBudget& budget) {
  std::vector<std::vector<double>> matrix;
  try {
    matrix = input;
  } catch (const std::bad_alloc&) {
    return {RankStatus::ResourceFailure, 0};
  }
  if (matrix.empty() || matrix.front().empty()) return {};
  const std::size_t rows = matrix.size();
  const std::size_t columns = matrix.front().size();
  constexpr double noiseFloor =
      64.0 * std::numeric_limits<double>::epsilon();
  std::vector<bool> activeRows(rows, true);
  for (std::size_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
    auto& row = matrix[rowIndex];
    if (row.size() != columns) return {RankStatus::NonFinite, 0};
    double rowMagnitude = 0.0;
    for (const double value : row) {
      budget.consume();
      if (!finite(value)) return {RankStatus::NonFinite, 0};
      rowMagnitude = std::max(rowMagnitude, std::abs(value));
    }
    activeRows[rowIndex] = rowMagnitude > noiseFloor;
  }

  // The equations are already scaled by their authored angular/linear
  // tolerances. Reject numerical-noise rows before any normalization, then
  // scale columns so a >1e10 rotational lever arm cannot hide a unit
  // translational derivative.
  for (std::size_t column = 0; column < columns; ++column) {
    double columnMagnitude = 0.0;
    for (std::size_t row = 0; row < rows; ++row) {
      budget.consume();
      if (activeRows[row])
        columnMagnitude =
            std::max(columnMagnitude, std::abs(matrix[row][column]));
    }
    if (columnMagnitude <= noiseFloor) {
      for (std::size_t row = 0; row < rows; ++row) {
        budget.consume();
        matrix[row][column] = 0.0;
      }
      continue;
    }
    for (std::size_t row = 0; row < rows; ++row) {
      budget.consume();
      matrix[row][column] /= columnMagnitude;
      if (!finite(matrix[row][column]))
        return {RankStatus::NonFinite, 0};
    }
  }
  for (std::size_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
    auto& row = matrix[rowIndex];
    double rowMagnitude = 0.0;
    for (const double value : row) {
      budget.consume();
      rowMagnitude = std::max(rowMagnitude, std::abs(value));
    }
    if (!activeRows[rowIndex] || rowMagnitude <= noiseFloor) {
      for (double& value : row) {
        budget.consume();
        value = 0.0;
      }
    } else {
      for (double& value : row) {
        budget.consume();
        value /= rowMagnitude;
        if (!finite(value)) return {RankStatus::NonFinite, 0};
      }
    }
  }
  std::size_t rank = 0;
  for (std::size_t column = 0; column < columns && rank < rows; ++column) {
    budget.consume();
    std::size_t pivot = rank;
    for (std::size_t row = rank + 1; row < rows; ++row) {
      budget.consume();
      if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
        pivot = row;
    }
    if (std::abs(matrix[pivot][column]) <= tolerance) continue;
    std::swap(matrix[pivot], matrix[rank]);
    const double divisor = matrix[rank][column];
    if (!finite(divisor) || divisor == 0.0)
      return {RankStatus::NonFinite, 0};
    for (std::size_t c = column; c < columns; ++c) {
      budget.consume();
      matrix[rank][c] /= divisor;
      if (!finite(matrix[rank][c])) return {RankStatus::NonFinite, 0};
    }
    for (std::size_t row = 0; row < rows; ++row) {
      budget.consume();
      if (row == rank) continue;
      const double factor = matrix[row][column];
      if (!finite(factor)) return {RankStatus::NonFinite, 0};
      if (std::abs(factor) <= tolerance) continue;
      for (std::size_t c = column; c < columns; ++c) {
        budget.consume();
        matrix[row][c] -= factor * matrix[rank][c];
        if (!finite(matrix[row][c])) return {RankStatus::NonFinite, 0};
      }
    }
    ++rank;
  }
  return {RankStatus::Ranked, rank};
}

std::vector<std::vector<double>> numericJacobian(
    const std::vector<ResolvedMate>& mates,
    std::map<OccurrenceId, Node>& nodes,
    const std::vector<OccurrenceId>& freeNodes,
    const MateSolverOptions& options, WorkBudget& budget) {
  const auto baseline = constraintVector(mates, nodes, options, budget);
  std::vector<std::vector<double>> matrix(
      baseline.size(), std::vector<double>(freeNodes.size() * 6, 0.0));
  constexpr double angularStep = 1.0e-6;
  for (std::size_t nodeIndex = 0; nodeIndex < freeNodes.size(); ++nodeIndex) {
    budget.consume();
    Node* current = node(nodes, freeNodes[nodeIndex]);
    const Pose saved = current->solved;
    for (std::size_t axis = 0; axis < 6; ++axis) {
      budget.consume();
      current->solved = saved;
      const double step =
          axis < 3
              ? std::max(1.0e-7,
                         std::abs(saved.translation[axis]) * 1.0e-8)
              : angularStep;
      const auto perturb = [&](double signedStep) {
        current->solved = saved;
        if (axis < 3) {
          current->solved.translation[axis] += signedStep;
          return;
        }
        Vec3 rotationAxis{};
        rotationAxis[axis - 3] = 1.0;
        const double half = signedStep * 0.5;
        const Quat delta{std::cos(half), rotationAxis[0] * std::sin(half),
                         rotationAxis[1] * std::sin(half),
                         rotationAxis[2] * std::sin(half)};
        current->solved.rotation = multiply(delta, current->solved.rotation);
      };
      perturb(step);
      const auto positive = constraintVector(mates, nodes, options, budget);
      perturb(-step);
      const auto negative = constraintVector(mates, nodes, options, budget);
      for (std::size_t row = 0; row < baseline.size(); ++row) {
        budget.consume();
        matrix[row][nodeIndex * 6 + axis] =
            (positive[row] - negative[row]) / (2.0 * step);
        if (!finite(matrix[row][nodeIndex * 6 + axis]))
          matrix[row][nodeIndex * 6 + axis] =
              std::numeric_limits<double>::quiet_NaN();
      }
    }
    current->solved = saved;
  }
  return matrix;
}

std::vector<MateId> connectedMateIds(const MateRegistry& registry,
                                     const MateSolveRequest& request,
                                     WorkBudget& budget) {
  std::set<MateId> selected;
  std::set<OccurrenceId> reached;
  if (request.seedImmediateOccurrenceId)
    reached.insert(*request.seedImmediateOccurrenceId);
  if (request.seedMateId) {
    const auto* seed = registry.findMate(*request.seedMateId);
    if (!seed) return {};
    selected.insert(*request.seedMateId);
    reached.insert(assemblyMateFirstOccurrencePath(*seed).entries().front());
    reached.insert(assemblyMateSecondOccurrencePath(*seed).entries().front());
  }
  bool changed = true;
  while (changed) {
    budget.consume();
    changed = false;
    for (const auto& [id, relation] : registry.relations()) {
      budget.consume();
      const auto first =
          assemblyMateFirstOccurrencePath(relation).entries().front();
      const auto second =
          assemblyMateSecondOccurrencePath(relation).entries().front();
      if (!reached.contains(first) && !reached.contains(second)) continue;
      if (selected.insert(id).second) changed = true;
      if (reached.insert(first).second) changed = true;
      if (reached.insert(second).second) changed = true;
    }
  }
  return {selected.begin(), selected.end()};
}

bool occurrenceKeyLess(const std::optional<TopologyOccurrenceKey>& left,
                       const std::optional<TopologyOccurrenceKey>& right) {
  if (left.has_value() != right.has_value()) return !left.has_value();
  if (!left) return false;
  return std::tie(left->featureId, left->slot) <
         std::tie(right->featureId, right->slot);
}

bool originLess(const TopologyProvenanceOrigin& left,
                const TopologyProvenanceOrigin& right) {
  const auto leftCore = std::tie(left.featureId, left.kind, left.operation,
                                 left.role, left.localName);
  const auto rightCore = std::tie(right.featureId, right.kind, right.operation,
                                  right.role, right.localName);
  if (leftCore != rightCore) return leftCore < rightCore;
  return occurrenceKeyLess(left.occurrence, right.occurrence);
}

bool provenanceLess(const TopologyProvenanceKey& left,
                    const TopologyProvenanceKey& right) {
  const auto leftCore = std::tie(left.featureId, left.kind, left.operation,
                                 left.role, left.localName);
  const auto rightCore = std::tie(right.featureId, right.kind, right.operation,
                                  right.role, right.localName);
  if (leftCore != rightCore) return leftCore < rightCore;
  if (left.origins != right.origins)
    return std::lexicographical_compare(left.origins.begin(), left.origins.end(),
                                        right.origins.begin(), right.origins.end(),
                                        originLess);
  return occurrenceKeyLess(left.occurrence, right.occurrence);
}

int endpointFailurePriority(MateEndpointFailure failure) noexcept {
  switch (statusForEndpointFailure(failure)) {
    case MateSolveStatus::InvalidInput:
      return 0;
    case MateSolveStatus::Ambiguous:
      return 1;
    case MateSolveStatus::Missing:
      return 2;
    case MateSolveStatus::Broken:
      return 3;
    case MateSolveStatus::Stale:
      return 4;
    default:
      return 5;
  }
}

bool endpointSemanticLess(const MateEndpoint& left,
                          const MateEndpoint& right) noexcept {
  if (left.occurrencePath.entries() != right.occurrencePath.entries())
    return std::lexicographical_compare(
        left.occurrencePath.entries().begin(),
        left.occurrencePath.entries().end(),
        right.occurrencePath.entries().begin(),
        right.occurrencePath.entries().end());
  const auto* leftV2 = left.face.v2();
  const auto* rightV2 = right.face.v2();
  if (!leftV2 || !rightV2) return leftV2 == nullptr && rightV2 != nullptr;
  if (leftV2->bodyId != rightV2->bodyId)
    return leftV2->bodyId < rightV2->bodyId;
  return provenanceLess(leftV2->provenance, rightV2->provenance);
}

bool axisReferenceSemanticLess(const MateAxisReference& left,
                               const MateAxisReference& right) noexcept {
  if (left.index() != right.index()) return left.index() < right.index();
  if (const auto* leftDatum = std::get_if<LocalDatumAxis>(&left))
    return *leftDatum < std::get<LocalDatumAxis>(right);
  const auto& leftFace = std::get<CylindricalFaceAxis>(left).face;
  const auto& rightFace = std::get<CylindricalFaceAxis>(right).face;
  const auto* leftV2 = leftFace.v2();
  const auto* rightV2 = rightFace.v2();
  if (!leftV2 || !rightV2) return leftV2 == nullptr && rightV2 != nullptr;
  if (leftV2->bodyId != rightV2->bodyId)
    return leftV2->bodyId < rightV2->bodyId;
  return provenanceLess(leftV2->provenance, rightV2->provenance);
}

bool axisEndpointSemanticLess(const MateAxisEndpoint& left,
                              const MateAxisEndpoint& right) noexcept {
  if (left.occurrencePath.entries() != right.occurrencePath.entries())
    return std::lexicographical_compare(
        left.occurrencePath.entries().begin(),
        left.occurrencePath.entries().end(),
        right.occurrencePath.entries().begin(),
        right.occurrencePath.entries().end());
  return axisReferenceSemanticLess(left.reference, right.reference);
}

bool pointReferenceSemanticLess(const MatePointReference& left,
                                const MatePointReference& right) noexcept {
  if (left.index() != right.index()) return left.index() < right.index();
  if (const auto* leftDatum = std::get_if<LocalDatumPoint>(&left))
    return *leftDatum < std::get<LocalDatumPoint>(right);
  const auto& leftEdge = std::get<CircularEdgeCenter>(left).edge;
  const auto& rightEdge = std::get<CircularEdgeCenter>(right).edge;
  const auto* leftV2 = leftEdge.v2();
  const auto* rightV2 = rightEdge.v2();
  if (!leftV2 || !rightV2) return leftV2 == nullptr && rightV2 != nullptr;
  if (leftV2->bodyId != rightV2->bodyId)
    return leftV2->bodyId < rightV2->bodyId;
  return provenanceLess(leftV2->provenance, rightV2->provenance);
}

bool pointEndpointSemanticLess(const MatePointEndpoint& left,
                               const MatePointEndpoint& right) noexcept {
  if (left.occurrencePath.entries() != right.occurrencePath.entries())
    return std::lexicographical_compare(
        left.occurrencePath.entries().begin(),
        left.occurrencePath.entries().end(),
        right.occurrencePath.entries().begin(),
        right.occurrencePath.entries().end());
  return pointReferenceSemanticLess(left.reference, right.reference);
}

bool resolvedMateSemanticLess(const ResolvedMate& left,
                              const ResolvedMate& right) noexcept {
  const auto leftKind = assemblyMateKind(*left.relation);
  const auto rightKind = assemblyMateKind(*right.relation);
  const bool leftAxis = leftKind == AssemblyMateKind::AxisConcentric;
  const bool rightAxis = rightKind == AssemblyMateKind::AxisConcentric;
  const bool leftPoint = leftKind == AssemblyMateKind::PointDistance;
  const bool rightPoint = rightKind == AssemblyMateKind::PointDistance;
  // A concentric relation is the stronger orientation+transverse primitive.
  // Rank it before planar rows so a same-axis Parallel relation is diagnosed
  // as the redundant authored relation, independent of MateId/insertion.
  const int leftFamily = leftAxis ? 0 : (leftPoint ? 2 : 1);
  const int rightFamily = rightAxis ? 0 : (rightPoint ? 2 : 1);
  if (leftFamily != rightFamily) return leftFamily < rightFamily;
  if (leftAxis) {
    const auto& leftMate = std::get<AxisConcentricMate>(*left.relation);
    const auto& rightMate = std::get<AxisConcentricMate>(*right.relation);
    const MateAxisEndpoint* leftFirst = &leftMate.first();
    const MateAxisEndpoint* leftSecond = &leftMate.second();
    const MateAxisEndpoint* rightFirst = &rightMate.first();
    const MateAxisEndpoint* rightSecond = &rightMate.second();
    if (axisEndpointSemanticLess(*leftSecond, *leftFirst))
      std::swap(leftFirst, leftSecond);
    if (axisEndpointSemanticLess(*rightSecond, *rightFirst))
      std::swap(rightFirst, rightSecond);
    if (axisEndpointSemanticLess(*leftFirst, *rightFirst)) return true;
    if (axisEndpointSemanticLess(*rightFirst, *leftFirst)) return false;
    if (axisEndpointSemanticLess(*leftSecond, *rightSecond)) return true;
    if (axisEndpointSemanticLess(*rightSecond, *leftSecond)) return false;
    return assemblyMateId(*left.relation) < assemblyMateId(*right.relation);
  }
  if (leftPoint) {
    const auto& leftMate = std::get<PointDistanceMate>(*left.relation);
    const auto& rightMate = std::get<PointDistanceMate>(*right.relation);
    const MatePointEndpoint* leftFirst = &leftMate.first();
    const MatePointEndpoint* leftSecond = &leftMate.second();
    const MatePointEndpoint* rightFirst = &rightMate.first();
    const MatePointEndpoint* rightSecond = &rightMate.second();
    if (pointEndpointSemanticLess(*leftSecond, *leftFirst))
      std::swap(leftFirst, leftSecond);
    if (pointEndpointSemanticLess(*rightSecond, *rightFirst))
      std::swap(rightFirst, rightSecond);
    if (pointEndpointSemanticLess(*leftFirst, *rightFirst)) return true;
    if (pointEndpointSemanticLess(*rightFirst, *leftFirst)) return false;
    if (pointEndpointSemanticLess(*leftSecond, *rightSecond)) return true;
    if (pointEndpointSemanticLess(*rightSecond, *leftSecond)) return false;
    const auto leftDistance = std::bit_cast<std::uint64_t>(leftMate.distanceMm());
    const auto rightDistance =
        std::bit_cast<std::uint64_t>(rightMate.distanceMm());
    if (leftDistance != rightDistance) return leftDistance < rightDistance;
    return assemblyMateId(*left.relation) < assemblyMateId(*right.relation);
  }
  const auto canonicalEndpoints = [](const AssemblyMate& relation) {
    const MateEndpoint* first = assemblyPlanarMateFirst(relation);
    const MateEndpoint* second = assemblyPlanarMateSecond(relation);
    if (assemblyMateKind(relation) != AssemblyMateKind::PlanarCoincident &&
        endpointSemanticLess(*second, *first))
      std::swap(first, second);
    return std::pair{first, second};
  };
  const auto leftEndpoints = canonicalEndpoints(*left.relation);
  const auto rightEndpoints = canonicalEndpoints(*right.relation);
  if (endpointSemanticLess(*leftEndpoints.first, *rightEndpoints.first))
    return true;
  if (endpointSemanticLess(*rightEndpoints.first, *leftEndpoints.first))
    return false;
  if (endpointSemanticLess(*leftEndpoints.second, *rightEndpoints.second))
    return true;
  if (endpointSemanticLess(*rightEndpoints.second, *leftEndpoints.second))
    return false;
  if (leftKind != rightKind) return leftKind < rightKind;
  if (leftKind != AssemblyMateKind::PlanarPerpendicular &&
      relationOrientation(*left.relation) !=
          relationOrientation(*right.relation))
    return relationOrientation(*left.relation) <
           relationOrientation(*right.relation);
  if (leftKind == AssemblyMateKind::PlanarAngle) {
    const auto leftAngle = std::bit_cast<std::uint64_t>(
        std::get<PlanarAngleMate>(*left.relation).angleRad());
    const auto rightAngle = std::bit_cast<std::uint64_t>(
        std::get<PlanarAngleMate>(*right.relation).angleRad());
    if (leftAngle != rightAngle) return leftAngle < rightAngle;
  }
  const auto leftOffset =
      std::bit_cast<std::uint64_t>(relationOffset(*left.relation));
  const auto rightOffset =
      std::bit_cast<std::uint64_t>(relationOffset(*right.relation));
  if (leftOffset != rightOffset) return leftOffset < rightOffset;
  return assemblyMateId(*left.relation) < assemblyMateId(*right.relation);
}

}  // namespace

MatePlacementCandidate::MatePlacementCandidate(
    MatePlacementCandidateAddress address, MateSolveRequest seed,
    std::vector<MateId> componentMateIds,
    std::vector<MatePlacementChange> changes,
    std::vector<MateOccurrenceDependencySeal> occurrenceDependencies,
    std::vector<MateDefinitionDependencySeal> definitionDependencies) noexcept
    : address_(address),
      seed_(std::move(seed)),
      componentMateIds_(std::move(componentMateIds)),
      changes_(std::move(changes)),
      occurrenceDependencies_(std::move(occurrenceDependencies)),
      definitionDependencies_(std::move(definitionDependencies)) {
  integrityFingerprint_ = computeIntegrityFingerprint();
}

std::uint64_t MatePlacementCandidate::computeIntegrityFingerprint() const
    noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto add = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= static_cast<unsigned char>(value >> (byte * 8));
      hash *= 1099511628211ULL;
    }
  };
  const auto addString = [&add](const std::string& value) {
    add(value.size());
    for (const unsigned char byte : value) add(byte);
  };
  add(address_.productIncarnation.value());
  add(address_.rootDefinitionId.value());
  add(address_.ownerAssemblyId.value());
  add(address_.mateRegistryRevision);
  add(address_.mateRegistryFingerprint);
  add(seed_.seedMateId ? seed_.seedMateId->value() : 0);
  add(seed_.seedImmediateOccurrenceId
          ? seed_.seedImmediateOccurrenceId->value()
          : 0);
  add(componentMateIds_.size());
  for (const auto id : componentMateIds_) add(id.value());
  add(changes_.size());
  for (const auto& change : changes_) {
    add(change.before.occurrenceId.value());
    add(change.before.targetDefinitionId.value());
    add(change.before.flags.visible);
    add(change.before.flags.suppressed);
    add(change.before.flags.grounded);
    for (const double value : change.before.placement.translationMm())
      add(std::bit_cast<std::uint64_t>(value));
    for (const double value : change.before.placement.quaternionWxyz())
      add(std::bit_cast<std::uint64_t>(value));
    for (const double value : change.afterPlacement.translationMm())
      add(std::bit_cast<std::uint64_t>(value));
    for (const double value : change.afterPlacement.quaternionWxyz())
      add(std::bit_cast<std::uint64_t>(value));
  }
  add(occurrenceDependencies_.size());
  for (const auto& dependency : occurrenceDependencies_) {
    add(dependency.ownerAssemblyId.value());
    add(dependency.occurrenceId.value());
    add(dependency.targetDefinitionId.value());
    add(dependency.flags.visible);
    add(dependency.flags.suppressed);
    add(dependency.flags.grounded);
    for (const double value : dependency.placement.translationMm())
      add(std::bit_cast<std::uint64_t>(value));
    for (const double value : dependency.placement.quaternionWxyz())
      add(std::bit_cast<std::uint64_t>(value));
    addString(dependency.displayNameUtf8);
  }
  add(definitionDependencies_.size());
  for (const auto& dependency : definitionDependencies_) {
    add(dependency.definitionId.value());
    add(dependency.stateId.value());
    add(dependency.topology.has_value());
    if (!dependency.topology) continue;
    add(dependency.topology->bodyId);
    add(dependency.topology->featureId);
    add(dependency.topology->shapeRevision);
    add(dependency.topology->topologyAvailable);
    add(dependency.topology->v2Authoritative);
  }
  return hash;
}

bool MatePlacementCandidate::integrityValid() const noexcept {
  return integrityFingerprint_ == computeIntegrityFingerprint();
}

PlanarMateAuthoringTransaction::PlanarMateAuthoringTransaction(
    DefinitionId ownerAssemblyId, PlanarCoincidentMate relation,
    MateRegistry beforeRegistry, MateRegistry afterRegistry,
    MatePlacementCandidate candidate) noexcept
    : ownerAssemblyId_(ownerAssemblyId),
      relation_(std::move(relation)),
      beforeRegistry_(std::move(beforeRegistry)),
      afterRegistry_(std::move(afterRegistry)),
      candidate_(std::move(candidate)) {
  integrityFingerprint_ = computeIntegrityFingerprint();
}

std::uint64_t
PlanarMateAuthoringTransaction::computeIntegrityFingerprint() const noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto add = [&hash](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      hash ^= static_cast<unsigned char>(value >> (byte * 8));
      hash *= 1099511628211ULL;
    }
  };
  add(ownerAssemblyId_.value());
  add(relation_.id().value());
  add(beforeRegistry_.revision());
  add(beforeRegistry_.fingerprint());
  add(afterRegistry_.revision());
  add(afterRegistry_.fingerprint());
  add(candidate_.integrityFingerprint_);
  return hash;
}

bool PlanarMateAuthoringTransaction::integrityValid() const noexcept {
  return candidate_.integrityValid() &&
         integrityFingerprint_ == computeIntegrityFingerprint();
}

namespace {

struct CandidateParts final {
  MatePlacementCandidateAddress address;
  MateSolveRequest seed;
  std::vector<MateId> componentMateIds;
  std::vector<MatePlacementChange> changes;
  std::vector<MateOccurrenceDependencySeal> occurrenceDependencies;
  std::vector<MateDefinitionDependencySeal> definitionDependencies;
};

MateSolveResult solvePlanarMatesCore(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateSolveRequest& request, const MateSolverOptions& options,
    std::optional<CandidateParts>& candidateParts,
    const MateRegistry* registryOverlay = nullptr) noexcept {
  MateSolveResult result;
  WorkBudget budget(options.maximumWorkUnits,
                    &result.diagnostics.requiredWorkUnits);
  try {
    if (!validOptions(options) || !ownerAssemblyId.isValid() ||
        !request.isValid())
      return result;
    const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
    if (!owner) {
      result.status = MateSolveStatus::Missing;
      return result;
    }
    const auto& registry = registryOverlay ? *registryOverlay
                                           : owner->mateRegistry();
    if (registry.size() == 0) {
      result.status = MateSolveStatus::InvalidInput;
      return result;
    }
    // Component identity is diagnostic state, not a successful-candidate
    // detail. Derive it before every status that can follow registry lookup,
    // including Busy, ResourceFailure and semantic endpoint failures.
    std::uint64_t selectionWork = 0;
    WorkBudget selectionBudget(1'000'000'000ULL, &selectionWork);
    result.componentMateIds =
        connectedMateIds(registry, request, selectionBudget);
    if (result.componentMateIds.empty()) {
      result.status = MateSolveStatus::Missing;
      return result;
    }
    if (options != registry.options()) {
      result.status = MateSolveStatus::InvalidInput;
      return result;
    }
    if (product.mutationTransactionActive()) {
      result.status = MateSolveStatus::Busy;
      return result;
    }
    // Covers ordered-set comparisons in connected-component expansion.
    prechargeQuadraticSort(budget, registry.size(), 32);
    budget.consume(boundedMultiply(registry.size(), 4));
    const auto& componentMateIds = result.componentMateIds;
    if (componentMateIds.size() > options.maximumMates) {
      result.status = MateSolveStatus::ResourceFailure;
      return result;
    }

    std::vector<ResolvedMate> mates;
    mates.reserve(componentMateIds.size());
    std::map<OccurrenceId, Node> nodes;
    std::vector<MateEndpointDiagnostic> endpointFailures;
    endpointFailures.reserve(componentMateIds.size() * 2);
    std::uint64_t maximumSemanticWork = 1;
    for (const auto mateId : componentMateIds) {
      budget.consume();
      const auto& relation = *registry.findMate(mateId);
      if (!isValidAssemblyMate(relation, options.maximumAbsoluteOffsetMm,
                               options.angularToleranceRad)) {
        endpointFailures.push_back(
            {mateId, false, MateEndpointFailure::InvalidOwner});
        continue;
      }
      ResolvedMate resolved{&relation};
      bool relationResolved = true;
      for (int endpointIndex = 0; endpointIndex < 2; ++endpointIndex) {
        auto& output = endpointIndex == 0 ? resolved.first : resolved.second;
        MateEndpointFailure failure = MateEndpointFailure::Unexpected;
        if (const auto* axis =
                std::get_if<AxisConcentricMate>(&relation)) {
          const auto& endpoint =
              endpointIndex == 0 ? axis->first() : axis->second();
          const auto endpointWork = referenceSemanticWork(endpoint, budget);
          budget.consume(endpointWork);
          resolved.semanticWork =
              boundedAdd(resolved.semanticWork, endpointWork);
          failure =
              resolveAxisEndpoint(product, *owner, endpoint, output, budget);
        } else if (const auto* distance =
                       std::get_if<PointDistanceMate>(&relation)) {
          const auto& endpoint =
              endpointIndex == 0 ? distance->first() : distance->second();
          const auto endpointWork = referenceSemanticWork(endpoint, budget);
          budget.consume(endpointWork);
          resolved.semanticWork =
              boundedAdd(resolved.semanticWork, endpointWork);
          failure =
              resolvePointEndpoint(product, *owner, endpoint, output, budget);
        } else {
          const auto& endpoint =
              endpointIndex == 0 ? *assemblyPlanarMateFirst(relation)
                                 : *assemblyPlanarMateSecond(relation);
          const auto endpointWork = referenceSemanticWork(endpoint, budget);
          budget.consume(endpointWork);
          resolved.semanticWork =
              boundedAdd(resolved.semanticWork, endpointWork);
          failure = resolveEndpoint(product, *owner, endpoint, output, budget);
        }
        if (failure != MateEndpointFailure::None) {
          endpointFailures.push_back(
              {mateId, endpointIndex == 1, failure});
          relationResolved = false;
          continue;
        }
        const auto* occurrence = owner->find(output.directOccurrenceId);
        if (!occurrence) {
          result.status = MateSolveStatus::Broken;
          return result;
        }
        if (!nodes.contains(occurrence->id())) {
          const Pose pose = poseOf(occurrence->placement());
          nodes.emplace(occurrence->id(),
                        Node{occurrence->id(), occurrence, pose, pose});
        }
      }
      if (relationResolved) {
        maximumSemanticWork =
            std::max(maximumSemanticWork, resolved.semanticWork);
        mates.push_back(std::move(resolved));
      }
    }
    if (!endpointFailures.empty()) {
      prechargeQuadraticSort(budget, endpointFailures.size(), 8);
      std::sort(endpointFailures.begin(), endpointFailures.end(),
                [](const auto& left, const auto& right) {
                  const int leftPriority = endpointFailurePriority(left.failure);
                  const int rightPriority = endpointFailurePriority(right.failure);
                  if (leftPriority != rightPriority)
                    return leftPriority < rightPriority;
                  if (left.mateId != right.mateId)
                    return left.mateId < right.mateId;
                  return left.secondEndpoint < right.secondEndpoint;
                });
      result.diagnostics.endpointFailure = endpointFailures.front();
      result.status = statusForEndpointFailure(
          result.diagnostics.endpointFailure->failure);
      return result;
    }
    for (const auto& mate : mates) {
      const auto kind = assemblyMateKind(*mate.relation);
      if (kind != AssemblyMateKind::PointDistance &&
          kind != AssemblyMateKind::PlanarAngle)
        continue;
      budget.consume();
      const bool degenerate =
          kind == AssemblyMateKind::PointDistance
              ? degeneratePointDirection(mate, nodes, budget)
              : degenerateAngleDirection(mate, nodes, budget);
      if (degenerate) {
        result.diagnostics.relationFailure = MateRelationDiagnostic{
            assemblyMateId(*mate.relation),
            MateEndpointFailure::DegenerateDirection};
        result.status = MateSolveStatus::Broken;
        return result;
      }
    }
    prechargeQuadraticSort(budget, mates.size(), maximumSemanticWork);
    std::sort(mates.begin(), mates.end(), resolvedMateSemanticLess);
    if (nodes.size() > options.maximumNodes) {
      result.status = MateSolveStatus::ResourceFailure;
      return result;
    }

    bool converged = false;
    for (std::size_t iteration = 0; iteration < options.maximumIterations;
         ++iteration) {
      budget.consume();
      for (const auto& mate : mates) {
        const auto kind = assemblyMateKind(*mate.relation);
        const bool degenerate =
            kind == AssemblyMateKind::PointDistance
                ? degeneratePointDirection(mate, nodes, budget)
                : (kind == AssemblyMateKind::PlanarAngle
                       ? degenerateAngleDirection(mate, nodes, budget)
                       : false);
        if (degenerate) {
          result.diagnostics.relationFailure = MateRelationDiagnostic{
              assemblyMateId(*mate.relation),
              MateEndpointFailure::DegenerateDirection};
          result.status = MateSolveStatus::Broken;
          return result;
        }
        if (!projectMate(mate, nodes, options, budget)) {
          result.status = MateSolveStatus::NonConvergent;
          return result;
        }
      }
      // A later relation in the same sweep can move an anchor that an earlier
      // point-distance relation just projected.  Revalidate the whole point
      // family before residual/convergence so a rounded exact coincidence can
      // never be published as a candidate.
      for (const auto& mate : mates) {
        const auto kind = assemblyMateKind(*mate.relation);
        if (kind != AssemblyMateKind::PointDistance &&
            kind != AssemblyMateKind::PlanarAngle)
          continue;
        budget.consume();
        const bool degenerate =
            kind == AssemblyMateKind::PointDistance
                ? degeneratePointDirection(mate, nodes, budget)
                : degenerateAngleDirection(mate, nodes, budget);
        if (degenerate) {
          result.diagnostics.relationFailure = MateRelationDiagnostic{
              assemblyMateId(*mate.relation),
              MateEndpointFailure::DegenerateDirection};
          result.status = MateSolveStatus::Broken;
          return result;
        }
      }
      double maxLinear = 0.0;
      double maxAngular = 0.0;
      for (const auto& mate : mates) {
        const auto value = residual(mate, nodes, budget);
        if (!finite(value.linearMm) || !finite(value.angularRad)) {
          result.status = MateSolveStatus::NonConvergent;
          return result;
        }
        maxLinear = std::max(maxLinear, value.linearMm);
        maxAngular = std::max(maxAngular, value.angularRad);
      }
      if (maxLinear <= options.linearToleranceMm &&
          maxAngular <= options.angularToleranceRad) {
        converged = true;
        break;
      }
    }

    for (const auto& mate : mates) {
      const auto kind = assemblyMateKind(*mate.relation);
      if (kind != AssemblyMateKind::PointDistance &&
          kind != AssemblyMateKind::PlanarAngle)
        continue;
      budget.consume();
      const bool degenerate =
          kind == AssemblyMateKind::PointDistance
              ? degeneratePointDirection(mate, nodes, budget)
              : degenerateAngleDirection(mate, nodes, budget);
      if (degenerate) {
        result.diagnostics.relationFailure = MateRelationDiagnostic{
            assemblyMateId(*mate.relation),
            MateEndpointFailure::DegenerateDirection};
        result.status = MateSolveStatus::Broken;
        return result;
      }
    }

    result.diagnostics.perMateResiduals.reserve(mates.size());
    for (const auto& mate : mates) {
      const auto value = residual(mate, nodes, budget);
      if (!finite(value.linearMm) || !finite(value.angularRad)) {
        result.status = MateSolveStatus::NonConvergent;
        return result;
      }
      result.diagnostics.maximumLinearResidualMm = std::max(
          result.diagnostics.maximumLinearResidualMm, value.linearMm);
      result.diagnostics.maximumAngularResidualRad = std::max(
          result.diagnostics.maximumAngularResidualRad, value.angularRad);
      result.diagnostics.perMateResiduals.push_back(value);
      if (value.linearMm > options.linearToleranceMm ||
          value.angularRad > options.angularToleranceRad)
        result.diagnostics.conflictMateIds.push_back(value.mateId);
    }
    std::vector<OccurrenceId> freeNodes;
    for (const auto& [id, current] : nodes)
      if ((budget.consume(), !current.occurrence->flags().grounded))
        freeNodes.push_back(id);
    if (!converged) {
      bool immovableConflict = false;
      for (const auto& mate : mates) {
        budget.consume();
        const auto* first = node(nodes, mate.first.directOccurrenceId);
        const auto* second = node(nodes, mate.second.directOccurrenceId);
        if (first == second || (first->occurrence->flags().grounded &&
                                second->occurrence->flags().grounded)) {
          immovableConflict = true;
          break;
        }
      }
      auto jacobian =
          numericJacobian(mates, nodes, freeNodes, options, budget);
      const auto values = constraintVector(mates, nodes, options, budget);
      const auto rank = matrixRank(jacobian, options.rankTolerance, budget);
      if (rank.status == RankStatus::ResourceFailure) {
        result.status = MateSolveStatus::ResourceFailure;
        return result;
      }
      if (rank.status == RankStatus::NonFinite) {
        result.status = MateSolveStatus::NonConvergent;
        return result;
      }
      for (std::size_t row = 0; row < jacobian.size(); ++row) {
        budget.consume();
        jacobian[row].push_back(-values[row]);
      }
      const auto augmentedRank =
          matrixRank(std::move(jacobian), options.rankTolerance, budget);
      if (augmentedRank.status == RankStatus::ResourceFailure) {
        result.status = MateSolveStatus::ResourceFailure;
        return result;
      }
      if (augmentedRank.status == RankStatus::NonFinite) {
        result.status = MateSolveStatus::NonConvergent;
        return result;
      }
      result.status = immovableConflict || augmentedRank.rank > rank.rank
                          ? MateSolveStatus::Overconstrained
                          : MateSolveStatus::NonConvergent;
      if (result.status == MateSolveStatus::Overconstrained) {
        result.diagnostics.conflictMateIds.clear();
        for (const auto& mate : mates) {
          budget.consume();
          result.diagnostics.conflictMateIds.push_back(
              assemblyMateId(*mate.relation));
        }
        prechargeQuadraticSort(
            budget, result.diagnostics.conflictMateIds.size(), 2);
        std::sort(result.diagnostics.conflictMateIds.begin(),
                  result.diagnostics.conflictMateIds.end());
      }
      return result;
    }

    result.diagnostics.totalDegreesOfFreedom = nodes.size() * 6;
    const auto jacobian =
        numericJacobian(mates, nodes, freeNodes, options, budget);
    const auto fullRank = matrixRank(jacobian, options.rankTolerance, budget);
    if (fullRank.status == RankStatus::ResourceFailure) {
      result.status = MateSolveStatus::ResourceFailure;
      return result;
    }
    if (fullRank.status == RankStatus::NonFinite) {
      result.status = MateSolveStatus::NonConvergent;
      return result;
    }
    result.diagnostics.rank = fullRank.rank;
    const std::size_t freeDof = freeNodes.size() * 6;
    result.diagnostics.remainingDegreesOfFreedom =
        result.diagnostics.rank <= freeDof
            ? freeDof - result.diagnostics.rank
            : 0;

    std::size_t previousRank = 0;
    for (std::size_t count = 1; count <= mates.size(); ++count) {
      budget.consume();
      budget.consume(count);  // prefix materialization
      std::vector<ResolvedMate> prefix(mates.begin(), mates.begin() + count);
      const auto prefixRank = matrixRank(
          numericJacobian(prefix, nodes, freeNodes, options, budget),
          options.rankTolerance, budget);
      if (prefixRank.status == RankStatus::ResourceFailure) {
        result.status = MateSolveStatus::ResourceFailure;
        return result;
      }
      if (prefixRank.status == RankStatus::NonFinite) {
        result.status = MateSolveStatus::NonConvergent;
        return result;
      }
      if (prefixRank.rank == previousRank)
        result.diagnostics.redundantMateIds.push_back(
            assemblyMateId(*prefix.back().relation));
      previousRank = prefixRank.rank;
    }
    prechargeQuadraticSort(budget,
                           result.diagnostics.perMateResiduals.size(), 2);
    std::sort(result.diagnostics.perMateResiduals.begin(),
              result.diagnostics.perMateResiduals.end(),
              [](const MateResidual& left, const MateResidual& right) {
                return left.mateId < right.mateId;
              });
    prechargeQuadraticSort(budget,
                           result.diagnostics.redundantMateIds.size(), 2);
    std::sort(result.diagnostics.redundantMateIds.begin(),
              result.diagnostics.redundantMateIds.end());
    prechargeQuadraticSort(budget,
                           result.diagnostics.conflictMateIds.size(), 2);
    std::sort(result.diagnostics.conflictMateIds.begin(),
              result.diagnostics.conflictMateIds.end());

    std::vector<MatePlacementChange> changes;
    budget.consume(boundedMultiply(nodes.size(), 32));
    changes.reserve(nodes.size());
    for (const auto& [id, current] : nodes) {
      static_cast<void>(id);
      changes.push_back(
          {{current.occurrence->id(), current.occurrence->targetDefinitionId(),
            current.occurrence->placement(), current.occurrence->flags()},
           transformOf(current.solved)});
    }
    std::vector<MateOccurrenceDependencySeal> occurrenceDependencies;
    std::vector<MateDefinitionDependencySeal> definitionDependencies;
    for (const auto& mate : mates) {
      for (const auto* endpoint : {&mate.first, &mate.second}) {
        budget.consume(endpoint->occurrenceDependencies.size() + 1);
        occurrenceDependencies.insert(
            occurrenceDependencies.end(),
            endpoint->occurrenceDependencies.begin(),
            endpoint->occurrenceDependencies.end());
        if (endpoint->definitionDependency)
          definitionDependencies.push_back(*endpoint->definitionDependency);
      }
    }
    const auto occurrenceLess = [](const auto& left, const auto& right) {
      return std::tie(left.ownerAssemblyId, left.occurrenceId) <
             std::tie(right.ownerAssemblyId, right.occurrenceId);
    };
    const auto definitionKey = [](const auto& value) {
      return std::tuple{
          value.definitionId, value.topology.has_value(),
          value.topology ? value.topology->bodyId : kInvalidBodyId,
          value.topology ? value.topology->featureId : kInvalidFeatureId};
    };
    prechargeQuadraticSort(budget, occurrenceDependencies.size(), 2);
    std::sort(occurrenceDependencies.begin(), occurrenceDependencies.end(),
              occurrenceLess);
    occurrenceDependencies.erase(
        std::unique(occurrenceDependencies.begin(),
                    occurrenceDependencies.end()),
        occurrenceDependencies.end());
    prechargeQuadraticSort(budget, definitionDependencies.size(), 2);
    std::sort(definitionDependencies.begin(), definitionDependencies.end(),
              [&definitionKey](const auto& left, const auto& right) {
                return definitionKey(left) < definitionKey(right);
              });
    definitionDependencies.erase(
        std::unique(definitionDependencies.begin(),
                    definitionDependencies.end()),
        definitionDependencies.end());
    if (!result.diagnostics.redundantMateIds.empty())
      result.status = MateSolveStatus::Redundant;
    else if (result.diagnostics.remainingDegreesOfFreedom == 0)
      result.status = MateSolveStatus::Solved;
    else
      result.status = MateSolveStatus::Underconstrained;
    std::uint64_t registryFingerprintWork = 8;
    for (const auto& [id, relation] : registry.relations()) {
      static_cast<void>(id);
      budget.consume();
      registryFingerprintWork = boundedAdd(
          registryFingerprintWork, relationSemanticWork(relation, budget));
    }
    budget.consume(registryFingerprintWork);
    budget.consume(boundedAdd(
        boundedMultiply(componentMateIds.size(), 2),
        boundedAdd(boundedMultiply(changes.size(), 24),
                   boundedAdd(boundedMultiply(occurrenceDependencies.size(),
                                              24),
                              boundedMultiply(definitionDependencies.size(),
                                              12)))));
    const auto registryFingerprint = registry.fingerprint();
    candidateParts.emplace(CandidateParts{
        MatePlacementCandidateAddress{product.incarnation(),
                                      product.rootDefinitionId(),
                                      ownerAssemblyId, registry.revision(),
                                      registryFingerprint},
        request, componentMateIds, std::move(changes),
        std::move(occurrenceDependencies),
        std::move(definitionDependencies)});
    return result;
  } catch (const std::bad_alloc&) {
    result.status = MateSolveStatus::ResourceFailure;
  } catch (const WorkLimitExceeded&) {
    result.status = MateSolveStatus::ResourceFailure;
  } catch (const Standard_Failure&) {
    result.status = MateSolveStatus::Broken;
  } catch (...) {
    result.status = MateSolveStatus::InvalidInput;
  }
  result.candidate.reset();
  return result;
}

}  // namespace

MateSolveResult solveAssemblyMates(const ProductDocument& product,
                                   DefinitionId ownerAssemblyId,
                                   const MateSolveRequest& request,
                                   const MateSolverOptions& options) noexcept {
  std::optional<CandidateParts> candidateParts;
  auto result = solvePlanarMatesCore(product, ownerAssemblyId, request, options,
                                     candidateParts);
  if (!candidateParts) return result;
  try {
    MatePlacementCandidate sealed(
        candidateParts->address, std::move(candidateParts->seed),
        std::move(candidateParts->componentMateIds),
        std::move(candidateParts->changes),
        std::move(candidateParts->occurrenceDependencies),
        std::move(candidateParts->definitionDependencies));
    result.candidate = std::move(sealed);
  } catch (...) {
    result.status = MateSolveStatus::ResourceFailure;
    result.candidate.reset();
  }
  return result;
}

PlanarMateEndpointClassification classifyPlanarMateEndpoint(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    const MateEndpoint& endpoint) noexcept {
  PlanarMateEndpointClassification result;
  try {
    const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
    if (!owner) {
      result.status = MateSolveStatus::Missing;
      result.failure = MateEndpointFailure::PathMissing;
      return result;
    }
    std::uint64_t work{};
    WorkBudget budget(owner->mateRegistry().options().maximumWorkUnits, &work);
    ResolvedEndpoint resolved;
    result.failure = resolveEndpoint(product, *owner, endpoint, resolved, budget);
    result.status = result.failure == MateEndpointFailure::None
                        ? MateSolveStatus::Solved
                        : statusForEndpointFailure(result.failure);
    if (resolved.definitionDependency)
      result.terminalDefinitionId =
          resolved.definitionDependency->definitionId;
  } catch (const std::bad_alloc&) {
    result.status = MateSolveStatus::ResourceFailure;
    result.failure = MateEndpointFailure::Unexpected;
  } catch (const WorkLimitExceeded&) {
    result.status = MateSolveStatus::ResourceFailure;
    result.failure = MateEndpointFailure::Unexpected;
  } catch (const Standard_Failure&) {
    result.status = MateSolveStatus::Broken;
    result.failure = MateEndpointFailure::TopologyEvaluationFailed;
  } catch (...) {
    result.status = MateSolveStatus::InvalidInput;
    result.failure = MateEndpointFailure::Unexpected;
  }
  return result;
}

PlanarMateAuthoringPrepareResult preparePlanarMateAuthoring(
    const ProductDocument& product, DefinitionId ownerAssemblyId,
    PlanarCoincidentMate relation) noexcept {
  PlanarMateAuthoringPrepareResult prepared;
  try {
    const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
    if (!owner) {
      prepared.status = MateSolveStatus::Missing;
      return prepared;
    }
    MateRegistry staged = owner->mateRegistry();
    const auto inserted = staged.insert(relation);
    if (inserted != MateRegistryMutationStatus::Inserted) {
      prepared.status = inserted == MateRegistryMutationStatus::ResourceFailure ||
                                inserted == MateRegistryMutationStatus::BudgetExceeded
                            ? MateSolveStatus::ResourceFailure
                            : MateSolveStatus::InvalidInput;
      return prepared;
    }
    std::optional<CandidateParts> candidateParts;
    auto solved = solvePlanarMatesCore(
        product, ownerAssemblyId,
        MateSolveRequest{relation.id(), std::nullopt}, staged.options(),
        candidateParts, &staged);
    prepared.status = solved.status;
    prepared.diagnostics = std::move(solved.diagnostics);
    if (!candidateParts) return prepared;
    MatePlacementCandidate candidate(
        candidateParts->address, std::move(candidateParts->seed),
        std::move(candidateParts->componentMateIds),
        std::move(candidateParts->changes),
        std::move(candidateParts->occurrenceDependencies),
        std::move(candidateParts->definitionDependencies));
    prepared.transaction = PlanarMateAuthoringTransaction(
        ownerAssemblyId, std::move(relation), owner->mateRegistry(),
        std::move(staged), std::move(candidate));
  } catch (const std::bad_alloc&) {
    prepared.status = MateSolveStatus::ResourceFailure;
    prepared.transaction.reset();
  } catch (...) {
    prepared.status = MateSolveStatus::InvalidInput;
    prepared.transaction.reset();
  }
  return prepared;
}

MateSolveResult solveAssemblyMates(const ProductDocument& product,
                                   DefinitionId ownerAssemblyId,
                                   const MateSolveRequest& request) noexcept {
  const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
  if (!owner) {
    MateSolveResult result;
    result.status = MateSolveStatus::Missing;
    return result;
  }
  return solveAssemblyMates(product, ownerAssemblyId, request,
                            owner->mateRegistry().options());
}

MateSolveResult solveAssemblyMates(const ProductDocument& product,
                                   DefinitionId ownerAssemblyId,
                                   const MateSolverOptions& options) noexcept {
  const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
  if (!owner || owner->mateRegistry().relations().empty()) {
    MateSolveResult result;
    result.status = owner ? MateSolveStatus::InvalidInput
                          : MateSolveStatus::Missing;
    return result;
  }
  return solveAssemblyMates(
      product, ownerAssemblyId,
      MateSolveRequest{owner->mateRegistry().relations().begin()->first,
                       std::nullopt},
      options);
}

MateSolveResult solveAssemblyMates(const ProductDocument& product,
                                   DefinitionId ownerAssemblyId) noexcept {
  const auto* owner = product.definitions().findAssembly(ownerAssemblyId);
  if (!owner || owner->mateRegistry().relations().empty()) {
    MateSolveResult result;
    result.status = owner ? MateSolveStatus::InvalidInput
                          : MateSolveStatus::Missing;
    return result;
  }
  return solveAssemblyMates(
      product, ownerAssemblyId,
      MateSolveRequest{owner->mateRegistry().relations().begin()->first,
                       std::nullopt},
      owner->mateRegistry().options());
}

MateSolveResult solvePlanarMates(const ProductDocument& product,
                                 DefinitionId ownerAssemblyId,
                                 const MateSolveRequest& request,
                                 const MateSolverOptions& options) noexcept {
  return solveAssemblyMates(product, ownerAssemblyId, request, options);
}

MateSolveResult solvePlanarMates(const ProductDocument& product,
                                 DefinitionId ownerAssemblyId,
                                 const MateSolveRequest& request) noexcept {
  return solveAssemblyMates(product, ownerAssemblyId, request);
}

MateSolveResult solvePlanarMates(const ProductDocument& product,
                                 DefinitionId ownerAssemblyId,
                                 const MateSolverOptions& options) noexcept {
  return solveAssemblyMates(product, ownerAssemblyId, options);
}

MateSolveResult solvePlanarMates(const ProductDocument& product,
                                 DefinitionId ownerAssemblyId) noexcept {
  return solveAssemblyMates(product, ownerAssemblyId);
}

}  // namespace solidar
