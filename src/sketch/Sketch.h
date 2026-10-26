#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace solidar::sketch {

using GeometryId = std::uint64_t;
inline constexpr GeometryId kInvalidGeometryId = 0;

using ConstraintId = std::uint64_t;
inline constexpr ConstraintId kInvalidConstraintId = 0;

using DimensionId = std::uint64_t;
inline constexpr DimensionId kInvalidDimensionId = 0;

struct Point {
  double xMm{};
  double yMm{};
  bool operator==(const Point&) const = default;
};

struct Line {
  Point start;
  Point end;
  std::size_t elementId{};
  bool dashed{false};
};

struct Circle {
  Point center;
  double radiusMm{};
  bool dashed{false};
};

struct Arc {
  Point center;
  double radiusMm{};
  double startAngleRad{};
  double sweepAngleRad{};
  bool dashed{false};
};

// Cubic Bezier primitive. The endpoint/control-point order is stable and is
// also used by PointReference::bezierPoint.
struct Bezier {
  std::array<Point, 4> points{};
  bool dashed{false};
};

[[nodiscard]] Point bezierPointAt(const Bezier& bezier, double t) noexcept;
// Builds a C1-continuous interpolating curve through every supplied knot.
// Each returned cubic shares an endpoint with the next one. Fewer than two
// distinct knots do not form a curve and produce an empty result.
[[nodiscard]] std::vector<Bezier> interpolatingBezierChain(
    const std::vector<Point>& knots);

[[nodiscard]] Point arcStartPoint(const Arc& arc) noexcept;
[[nodiscard]] Point arcEndPoint(const Arc& arc) noexcept;

enum class Endpoint { Start, End };

struct DatumOriginReference {
  bool operator==(const DatumOriginReference&) const = default;
};
struct LineEndpointReference {
  GeometryId lineId{kInvalidGeometryId};
  Endpoint endpoint{Endpoint::Start};
  bool operator==(const LineEndpointReference&) const = default;
};
struct CircleCenterReference {
  GeometryId circleId{kInvalidGeometryId};
  bool operator==(const CircleCenterReference&) const = default;
};
struct ArcEndpointReference {
  GeometryId arcId{kInvalidGeometryId};
  Endpoint endpoint{Endpoint::Start};
  bool operator==(const ArcEndpointReference&) const = default;
};
struct BezierPointReference {
  GeometryId bezierId{kInvalidGeometryId};
  std::uint8_t pointIndex{};
  bool operator==(const BezierPointReference&) const = default;
};
struct CompositeCenterReference {
  // Composite element IDs are persistent model IDs, never vector indices.
  std::uint64_t elementId{};
  bool operator==(const CompositeCenterReference&) const = default;
};

// A point reference has one, and only one, active source.  The small public
// views retain source compatibility while consumers migrate; assigning a
// different view replaces the active tagged payload instead of accumulating
// mutually contradictory fields.
class PointReference {
 public:
  using Payload = std::variant<DatumOriginReference,
                               LineEndpointReference,
                               CircleCenterReference,
                               ArcEndpointReference,
                               BezierPointReference,
                               CompositeCenterReference>;

  enum class IdViewKind { Line, Circle, Arc, Bezier };
  class GeometryIdView {
   public:
    GeometryIdView(PointReference* owner, IdViewKind kind) noexcept
        : owner_(owner), kind_(kind) {}
    GeometryIdView& operator=(const GeometryIdView& other) noexcept {
      return *this = static_cast<GeometryId>(other);
    }
    GeometryIdView& operator=(GeometryId id) noexcept;
    operator GeometryId() const noexcept;

   private:
    PointReference* owner_{};
    IdViewKind kind_{};
  };

  enum class BoolViewKind { Start, Origin };
  class BoolView {
   public:
    BoolView(PointReference* owner, BoolViewKind kind) noexcept
        : owner_(owner), kind_(kind) {}
    BoolView& operator=(const BoolView& other) noexcept {
      return *this = static_cast<bool>(other);
    }
    BoolView& operator=(bool value) noexcept;
    operator bool() const noexcept;

   private:
    PointReference* owner_{};
    BoolViewKind kind_{};
  };

  class ElementIdView {
   public:
    explicit ElementIdView(PointReference* owner) noexcept : owner_(owner) {}
    ElementIdView& operator=(const ElementIdView& other) noexcept {
      return *this = static_cast<std::size_t>(other);
    }
    ElementIdView& operator=(std::size_t id) noexcept;
    operator std::size_t() const noexcept;

   private:
    PointReference* owner_{};
  };

  class BezierPointView {
   public:
    explicit BezierPointView(PointReference* owner) noexcept : owner_(owner) {}
    BezierPointView& operator=(const BezierPointView& other) noexcept {
      return *this = static_cast<std::uint8_t>(other);
    }
    BezierPointView& operator=(std::uint8_t index) noexcept;
    operator std::uint8_t() const noexcept;

   private:
    PointReference* owner_{};
  };

  PointReference() noexcept;
  PointReference(GeometryId lineId, bool start) noexcept;
  explicit PointReference(DatumOriginReference source) noexcept;
  explicit PointReference(LineEndpointReference source) noexcept;
  explicit PointReference(CircleCenterReference source) noexcept;
  explicit PointReference(ArcEndpointReference source) noexcept;
  explicit PointReference(BezierPointReference source) noexcept;
  explicit PointReference(CompositeCenterReference source) noexcept;
  PointReference(const PointReference& other) noexcept;
  PointReference(PointReference&& other) noexcept;
  PointReference& operator=(const PointReference& other) noexcept;
  PointReference& operator=(PointReference&& other) noexcept;

  [[nodiscard]] const Payload& payload() const noexcept { return payload_; }
  bool operator==(const PointReference& other) const noexcept {
    return payload_ == other.payload_;
  }

 private:
  Payload payload_{DatumOriginReference{}};

 public:
  GeometryIdView lineId{this, IdViewKind::Line};
  BoolView start{this, BoolViewKind::Start};
  GeometryIdView circleId{this, IdViewKind::Circle};
  ElementIdView elementCenterId{this};
  GeometryIdView arcId{this, IdViewKind::Arc};
  BoolView origin{this, BoolViewKind::Origin};
  GeometryIdView bezierId{this, IdViewKind::Bezier};
  BezierPointView bezierPoint{this};
};

enum class DimensionKind {
  LineLength,
  PointDistance,
  CircleDiameter,
  PointDistanceX,
  PointDistanceY,
  LineAngle,
  LineDistance,
  CircleRadius
};

struct Dimension {
  DimensionKind kind{DimensionKind::LineLength};
  GeometryId geometryId{kInvalidGeometryId};
  PointReference firstPoint{};
  PointReference secondPoint{};
  // Compatibility projection used only while decoding v1/v2 and by legacy
  // presentation callers.  For a linked annotation this is refreshed from
  // the exact constraint and is never an authoritative model value.
  double valueMm{};
  double offsetMm{4.0};
  double angleRad{};
  // Persistent annotation identity. Kept last so historical aggregate
  // initializers remain source-compatible.
  DimensionId id{kInvalidDimensionId};
  // Exact model relationship.  Legacy decoders populate this
  // deterministically; production code never discovers it by matching shape.
  ConstraintId constraintId{kInvalidConstraintId};
};

enum class ConstraintType {
  Horizontal,
  Vertical,
  Coincident,
  PointOnLine,
  Distance,
  Length,
  Radius,
  Diameter,
  Parallel,
  Perpendicular,
  Equal,
  Angle,
  DistanceX,
  DistanceY,
  PointOnCircle,
  Tangent,
  LineDistance,
  Lock,
  PointOnArc,
  Midpoint,
  PointOnXAxis,
  PointOnYAxis,
  Count};

enum class ConstraintMode { Driving, Reference, Disabled };

struct HorizontalConstraint { GeometryId lineId{kInvalidGeometryId}; };
struct VerticalConstraint { GeometryId lineId{kInvalidGeometryId}; };
struct CoincidentConstraint { PointReference first; PointReference second; };
struct PointOnLineConstraint { GeometryId lineId{kInvalidGeometryId}; PointReference point; };
struct DistanceConstraint { PointReference first; PointReference second; std::optional<double> drivingValue; };
struct LengthConstraint { GeometryId lineId{kInvalidGeometryId}; std::optional<double> drivingValue; };
struct RadiusConstraint { GeometryId circleId{kInvalidGeometryId}; std::optional<double> drivingValue; };
struct DiameterConstraint { GeometryId circleId{kInvalidGeometryId}; std::optional<double> drivingValue; };
struct ParallelConstraint { GeometryId firstLineId{kInvalidGeometryId}; GeometryId secondLineId{kInvalidGeometryId}; };
struct PerpendicularConstraint { GeometryId firstLineId{kInvalidGeometryId}; GeometryId secondLineId{kInvalidGeometryId}; };
struct EqualConstraint { GeometryId firstGeometryId{kInvalidGeometryId}; GeometryId secondGeometryId{kInvalidGeometryId}; };
struct AngleConstraint { GeometryId firstLineId{kInvalidGeometryId}; GeometryId secondLineId{kInvalidGeometryId}; std::optional<double> drivingValue; };
struct DistanceXConstraint { PointReference first; PointReference second; std::optional<double> drivingValue; };
struct DistanceYConstraint { PointReference first; PointReference second; std::optional<double> drivingValue; };
struct PointOnCircleConstraint { GeometryId circleId{kInvalidGeometryId}; PointReference point; };
struct TangentConstraint { GeometryId firstGeometryId{kInvalidGeometryId}; GeometryId secondGeometryId{kInvalidGeometryId}; };
struct LineDistanceConstraint { GeometryId firstLineId{kInvalidGeometryId}; GeometryId secondLineId{kInvalidGeometryId}; std::optional<double> drivingValue; };
struct LockConstraint { GeometryId geometryId{kInvalidGeometryId}; };
struct PointOnArcConstraint { GeometryId arcId{kInvalidGeometryId}; PointReference point; };
struct MidpointConstraint { GeometryId lineId{kInvalidGeometryId}; PointReference point; };
struct PointOnXAxisConstraint { PointReference point; };
struct PointOnYAxisConstraint { PointReference point; };
struct UnsupportedConstraint {
  ConstraintType rawType{ConstraintType::Count};
};

using ConstraintPayload = std::variant<
    HorizontalConstraint, VerticalConstraint, CoincidentConstraint,
    PointOnLineConstraint, DistanceConstraint, LengthConstraint,
    RadiusConstraint, DiameterConstraint, ParallelConstraint,
    PerpendicularConstraint, EqualConstraint, AngleConstraint,
    DistanceXConstraint, DistanceYConstraint, PointOnCircleConstraint,
    TangentConstraint, LineDistanceConstraint, LockConstraint,
    PointOnArcConstraint, MidpointConstraint, PointOnXAxisConstraint,
    PointOnYAxisConstraint, UnsupportedConstraint>;

static_assert(std::variant_size_v<ConstraintPayload> ==
              static_cast<std::size_t>(ConstraintType::Count) + 1,
              "Each production ConstraintType needs one payload; the final "
              "variant alternative is the fail-closed unsupported sentinel");

[[nodiscard]] constexpr bool isProductionConstraintType(
    ConstraintType type) noexcept {
  const auto ordinal = static_cast<int>(type);
  return ordinal >= static_cast<int>(ConstraintType::Horizontal) &&
         ordinal < static_cast<int>(ConstraintType::Count);
}

class Constraint {
 public:
  class TypeView {
   public:
    explicit TypeView(Constraint* owner) noexcept : owner_(owner) {}
    TypeView& operator=(const TypeView& other) noexcept {
      return *this = static_cast<ConstraintType>(other);
    }
    TypeView& operator=(ConstraintType type) noexcept;
    operator ConstraintType() const noexcept;
   private:
    Constraint* owner_{};
  };
  class GeometryView {
   public:
    GeometryView(Constraint* owner, bool first) noexcept
        : owner_(owner), first_(first) {}
    GeometryView& operator=(const GeometryView& other) noexcept {
      return *this = static_cast<GeometryId>(other);
    }
    GeometryView& operator=(GeometryId id) noexcept;
    operator GeometryId() const noexcept;
   private:
    Constraint* owner_{};
    bool first_{};
  };
  class PointView {
   public:
    class GeometryIdMember {
     public:
      GeometryIdMember(PointView* owner,
                       PointReference::IdViewKind kind) noexcept
          : owner_(owner), kind_(kind) {}
      GeometryIdMember& operator=(const GeometryIdMember& other) noexcept {
        return *this = static_cast<GeometryId>(other);
      }
      GeometryIdMember& operator=(GeometryId id) noexcept;
      operator GeometryId() const noexcept;
     private:
      PointView* owner_{};
      PointReference::IdViewKind kind_{};
    };
    class BoolMember {
     public:
      BoolMember(PointView* owner,
                 PointReference::BoolViewKind kind) noexcept
          : owner_(owner), kind_(kind) {}
      BoolMember& operator=(const BoolMember& other) noexcept {
        return *this = static_cast<bool>(other);
      }
      BoolMember& operator=(bool value) noexcept;
      operator bool() const noexcept;
     private:
      PointView* owner_{};
      PointReference::BoolViewKind kind_{};
    };
    class ElementIdMember {
     public:
      explicit ElementIdMember(PointView* owner) noexcept : owner_(owner) {}
      ElementIdMember& operator=(const ElementIdMember& other) noexcept {
        return *this = static_cast<std::size_t>(other);
      }
      ElementIdMember& operator=(std::size_t id) noexcept;
      operator std::size_t() const noexcept;
     private:
      PointView* owner_{};
    };
    class BezierPointMember {
     public:
      explicit BezierPointMember(PointView* owner) noexcept : owner_(owner) {}
      BezierPointMember& operator=(const BezierPointMember& other) noexcept {
        return *this = static_cast<std::uint8_t>(other);
      }
      BezierPointMember& operator=(std::uint8_t index) noexcept;
      operator std::uint8_t() const noexcept;
     private:
      PointView* owner_{};
    };

    PointView(Constraint* owner, bool first) noexcept
        : owner_(owner), first_(first) {}
    PointView& operator=(const PointView& other) noexcept {
      return *this = static_cast<PointReference>(other);
    }
    PointView& operator=(const PointReference& point) noexcept;
    operator PointReference() const noexcept;

    GeometryIdMember lineId{this, PointReference::IdViewKind::Line};
    BoolMember start{this, PointReference::BoolViewKind::Start};
    GeometryIdMember circleId{this, PointReference::IdViewKind::Circle};
    ElementIdMember elementCenterId{this};
    GeometryIdMember arcId{this, PointReference::IdViewKind::Arc};
    BoolMember origin{this, PointReference::BoolViewKind::Origin};
    GeometryIdMember bezierId{this, PointReference::IdViewKind::Bezier};
    BezierPointMember bezierPoint{this};
   private:
    Constraint* owner_{};
    bool first_{};
  };
  class ValueView {
   public:
    explicit ValueView(Constraint* owner) noexcept : owner_(owner) {}
    ValueView& operator=(const ValueView& other) noexcept {
      return *this = static_cast<double>(other);
    }
    ValueView& operator=(double value) noexcept;
    operator double() const noexcept;
   private:
    Constraint* owner_{};
  };

  Constraint() noexcept;
  explicit Constraint(ConstraintPayload payload,
                      ConstraintMode mode = ConstraintMode::Driving,
                      ConstraintId id = kInvalidConstraintId) noexcept;
  Constraint(const Constraint& other) noexcept;
  Constraint(Constraint&& other) noexcept;
  Constraint& operator=(const Constraint& other) noexcept;
  Constraint& operator=(Constraint&& other) noexcept;

  [[nodiscard]] const ConstraintPayload& payload() const noexcept {
    return payload_;
  }
  [[nodiscard]] ConstraintPayload& payload() noexcept { return payload_; }

  ConstraintId id{kInvalidConstraintId};
  ConstraintMode mode{ConstraintMode::Driving};

 private:
  ConstraintPayload payload_{HorizontalConstraint{}};

 public:
  TypeView type{this};
  GeometryView firstGeometry{this, true};
  GeometryView secondGeometry{this, false};
  PointView firstPoint{this, true};
  PointView secondPoint{this, false};
  ValueView value{this};
};

[[nodiscard]] constexpr bool participatesInConstraintSystem(
    const Constraint& constraint) noexcept {
  return constraint.mode == ConstraintMode::Driving;
}

enum class GeometryKind { Line, Circle, Arc, Bezier };

struct GeometryLocation {
  GeometryKind kind{GeometryKind::Line};
  std::size_t index{};
  bool operator==(const GeometryLocation&) const = default;
};

struct ConstraintComponent {
  std::vector<GeometryId> geometryIds;
  std::vector<ConstraintId> constraintIds;
};

enum class SolveStatus {
  SolvedFullyConstrained,
  SolvedUnderConstrained,
  Redundant,
  PartiallyRedundant,
  Conflicting,
  Malformed,
  Unsupported,
  NonConverged,
  BackendFailure,
};

// Stable solver projection shared by model mutation callers.  Keeping it in
// the Qt-free model header lets every interactive mutation report why it did
// (or did not) commit instead of forcing callers to infer success from a
// void/bool return value.
struct SolveResult {
  SolveStatus status{SolveStatus::SolvedUnderConstrained};
  std::size_t applied{};
  std::size_t unsupported{};
  std::size_t invalidReferences{};
  bool converged{true};
  std::size_t violatedConstraints{};
  double maxNormalizedResidual{};
  std::size_t passes{};
  std::size_t componentsVisited{};
  std::size_t geometriesVisited{};
  std::size_t constraintsVisited{};
  std::size_t lockedSnapshotSize{};
  std::size_t fullDiagnosticsCount{};
  std::size_t allocationCount{};
  std::size_t allocatedBytes{};
  std::size_t peakOwnedBytes{};
  std::size_t nonFiniteGeometry{};
  std::size_t degenerateGeometry{};
  bool deferredByActiveSolve{false};
  std::size_t equationRank{};
  std::size_t degreesOfFreedom{};
  bool rankComputed{true};
  std::vector<ConstraintId> conflictingConstraintIds;
  std::vector<ConstraintId> redundantConstraintIds;
  std::vector<ConstraintId> partiallyRedundantConstraintIds;
  std::vector<GeometryId> remainingDofGeometryIds;
};

enum class SketchMutationStatus {
  Applied,
  NoChange,
  InvalidReference,
  Unsupported,
  Conflicting,
  NonConverged,
  DeferredByActiveSolve,
  InternalError,
};

enum class SketchMutationDiagnosticCode {
  None,
  InvalidGeometryReference,
  UnsupportedConstraint,
  ConstraintResidualViolation,
  NonFiniteGeometry,
  DegenerateGeometry,
  SolverDidNotConverge,
  ActiveSolve,
  LockedGeometry,
  InternalInvariant,
};

struct SketchMutationResult {
  SketchMutationStatus status{SketchMutationStatus::InternalError};
  SketchMutationDiagnosticCode diagnosticCode{
      SketchMutationDiagnosticCode::InternalInvariant};
  SolveResult solveResult;
  ConstraintComponent component;
  std::vector<ConstraintId> relatedConstraintIds;

  [[nodiscard]] bool committed() const noexcept {
    return status == SketchMutationStatus::Applied ||
           status == SketchMutationStatus::NoChange;
  }
  operator bool() const noexcept { return committed(); }
};

enum class SketchDeletionStatus {
  Removed,
  Locked,
  Stale,
  NoChange,
};

struct SketchDeletionResult {
  SketchDeletionStatus status{SketchDeletionStatus::NoChange};
  std::vector<GeometryId> removedGeometryIds;

  [[nodiscard]] bool removed() const noexcept {
    return status == SketchDeletionStatus::Removed;
  }
};

enum class ConstraintApplyStatus {
  Accepted,
  Redundant,
  Conflicting,
  Unsupported,
  InvalidReference,
  SolverFailed,
};

struct ConstraintApplyResult {
  ConstraintApplyStatus status{ConstraintApplyStatus::SolverFailed};
  ConstraintId constraintId{kInvalidConstraintId};
  SketchMutationDiagnosticCode diagnosticCode{
      SketchMutationDiagnosticCode::InternalInvariant};
  SolveResult solveResult;
  ConstraintComponent component;
  std::vector<ConstraintId> relatedConstraintIds;
  std::string diagnostic;

  [[nodiscard]] bool accepted() const noexcept {
    return status == ConstraintApplyStatus::Accepted ||
           (status == ConstraintApplyStatus::Redundant &&
            constraintId != kInvalidConstraintId);
  }
};

template <typename T>
struct IndexedValueDelta {
  // Indices belong to the complete state on their respective side of the
  // transition.  Keeping both is essential when a single journal removes an
  // earlier entity and edits a later one: the entity's index is then
  // different while undoing the same delta.
  std::size_t beforeIndex{};
  std::size_t afterIndex{};
  std::optional<T> before;
  std::optional<T> after;
};

struct SketchDelta {
  std::vector<IndexedValueDelta<Line>> lines;
  std::vector<IndexedValueDelta<Circle>> circles;
  std::vector<IndexedValueDelta<Arc>> arcs;
  std::vector<IndexedValueDelta<Bezier>> beziers;
  std::vector<IndexedValueDelta<GeometryId>> lineIds;
  std::vector<IndexedValueDelta<GeometryId>> circleIds;
  std::vector<IndexedValueDelta<GeometryId>> arcIds;
  std::vector<IndexedValueDelta<GeometryId>> bezierIds;
  std::vector<IndexedValueDelta<Dimension>> dimensions;
  std::vector<IndexedValueDelta<Constraint>> constraints;
  std::vector<IndexedValueDelta<std::size_t>> centerNodeElementIds;
  std::size_t beforeNextElementId{};
  std::size_t afterNextElementId{};
  GeometryId beforeNextGeometryId{};
  GeometryId afterNextGeometryId{};
  ConstraintId beforeNextConstraintId{};
  ConstraintId afterNextConstraintId{};
  DimensionId beforeNextDimensionId{};
  DimensionId afterNextDimensionId{};
  std::uint64_t beforeSemanticFingerprint{};
  std::uint64_t afterSemanticFingerprint{};
  std::vector<ConstraintId> beforeInvalidConstraintIds;
  std::vector<ConstraintId> afterInvalidConstraintIds;
  std::size_t retainedBytes{};
  [[nodiscard]] bool empty() const noexcept;
};

class Sketch final {
 public:
  Sketch();
  Sketch(const Sketch& other);
  Sketch& operator=(const Sketch& other);
  Sketch(Sketch&&) noexcept = default;
  Sketch& operator=(Sketch&&) noexcept = default;

  // Starts a mutation journal without copying the Sketch. Mutators append
  // stable-ID/range before-images as they touch entities; finish materializes
  // a reversible compact delta.
  void beginDeltaJournal();
  [[nodiscard]] SketchDelta finishDeltaJournal();
  [[nodiscard]] SketchDelta cancelDeltaJournal();
  [[nodiscard]] bool deltaJournalActive() const noexcept;
  [[nodiscard]] std::size_t deltaJournalDepth() const noexcept;
  [[nodiscard]] bool rollbackDeltaJournalsToDepth(
      std::size_t depth) noexcept;
  static void resetFullCopyCountForTesting() noexcept;
  [[nodiscard]] static std::size_t fullCopyCountForTesting() noexcept;
  static void resetDeltaJournalBeginCountForTesting() noexcept;
  [[nodiscard]] static std::size_t deltaJournalBeginCountForTesting() noexcept;
  static void failNextNestedConstraintJournalForTesting() noexcept;

  void clear();
  SketchMutationResult setRectangle(double widthMm, double heightMm);
  SketchMutationResult addLine(Point start, Point end);
  SketchMutationResult addLine(Point start, Point end, std::size_t elementId);
  SketchMutationResult addRectangle(Point firstCorner, Point oppositeCorner);
  SketchMutationResult addRectangle(Point first, Point second, Point third,
                                    Point fourth);
  SketchMutationResult addCircle(Point center, double radiusMm);
  void addArc(Point center, double radiusMm, double startAngleRad,
              double sweepAngleRad, bool dashed = false);
  void addBezier(Point start, Point control1, Point control2, Point end,
                 bool dashed = false);
  void removeLine(std::size_t index);
  void removeCircle(std::size_t index);
  void removeArc(std::size_t index);
  void removeBezier(std::size_t index);
  void removeElement(std::size_t elementId);
  [[nodiscard]] SketchDeletionResult removeSelection(
      const std::vector<GeometryId>& geometryIds,
      const std::vector<std::size_t>& elementIds);
  [[nodiscard]] SketchMutationResult translateElement(
      std::size_t elementId, double dxMm, double dyMm);

  // Optional virtual center node owned by a composite CAD element.
  // The point is derived from the element geometry, so it automatically
  // follows translation, rotation and rectangle resizing.
  void markElementCenterNode(std::size_t elementId);
  [[nodiscard]] bool hasElementCenterNode(
      std::size_t elementId) const noexcept;
  [[nodiscard]] std::optional<Point> elementCenterPoint(
      std::size_t elementId) const noexcept;
  [[nodiscard]] const std::vector<std::size_t>&
  centerNodeElementIds() const noexcept;
  [[nodiscard]] SketchMutationResult translateSelection(
      const std::vector<std::size_t>& elementIds,
      const std::vector<GeometryId>& circleIds,
      const std::vector<GeometryId>& arcIds,
      const std::vector<GeometryId>& bezierIds, double dxMm, double dyMm);
  [[nodiscard]] SketchMutationResult translateSelection(
      const std::vector<std::size_t>& elementIds,
      const std::vector<GeometryId>& circleIds,
      const std::vector<GeometryId>& arcIds, double dxMm, double dyMm) {
    return translateSelection(elementIds, circleIds, arcIds, {}, dxMm, dyMm);
  }
  [[nodiscard]] SketchMutationResult translateLinesByIds(
      const std::vector<GeometryId>& lineIds, double dxMm, double dyMm);
  void setElementDashed(std::size_t elementId, bool dashed);
  void setLineDashedById(GeometryId id, bool dashed);
  void setCircleDashed(std::size_t index, bool dashed);
  [[nodiscard]] SketchMutationResult translateCircle(
      std::size_t index, double dxMm, double dyMm);
  void setCircleDashedById(GeometryId id, bool dashed);
  void setArcDashedById(GeometryId id, bool dashed);
  void setBezierDashedById(GeometryId id, bool dashed);
  [[nodiscard]] SketchMutationResult translateCircleById(
      GeometryId id, double dxMm, double dyMm);
  [[nodiscard]] SketchMutationResult translateArcById(
      GeometryId id, double dxMm, double dyMm);
  [[nodiscard]] SketchMutationResult translateBezierById(
      GeometryId id, double dxMm, double dyMm);
  [[nodiscard]] SketchMutationResult moveArcEndpointReshapeById(
      GeometryId id, bool start, Point target);
  [[nodiscard]] SketchMutationResult translatePoint(
      PointReference reference, double dxMm, double dyMm);

  [[nodiscard]] GeometryId lineId(std::size_t index) const noexcept;
  [[nodiscard]] GeometryId circleId(std::size_t index) const noexcept;
  [[nodiscard]] GeometryId arcId(std::size_t index) const noexcept;
  [[nodiscard]] GeometryId bezierId(std::size_t index) const noexcept;
  [[nodiscard]] std::optional<std::size_t> lineIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> circleIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> arcIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> bezierIndex(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<GeometryLocation> geometryLocation(
      GeometryId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> constraintIndex(
      ConstraintId id) const noexcept;
  [[nodiscard]] std::optional<std::size_t> dimensionIndex(
      DimensionId id) const noexcept;
  [[nodiscard]] std::optional<double> dimensionValue(
      std::size_t index) const noexcept;
  [[nodiscard]] ConstraintComponent connectedComponent(
      const std::vector<GeometryId>& seeds) const;
  [[nodiscard]] std::vector<ConstraintComponent> constraintComponents() const;
  [[nodiscard]] std::size_t ownedBytes() const noexcept;
  [[nodiscard]] std::size_t ownedAllocationBlocks() const noexcept;
  // Complete persisted identity.  Unlike solverFingerprint(), this includes
  // presentation and persistence-only fields and must never be used as the
  // solver cache key.
  [[nodiscard]] std::uint64_t semanticFingerprint() const noexcept;
  [[nodiscard]] bool semanticallyEqual(const Sketch& other) const noexcept;
  [[nodiscard]] std::uint64_t solverFingerprint() const noexcept;
  [[nodiscard]] static SketchDelta makeDelta(const Sketch& before,
                                             const Sketch& after);
  bool applyDelta(const SketchDelta& delta, bool forward);

  bool storeDimension(const Dimension& dimension);
  bool removeDimension(std::size_t index);
  void clearDimensions();
  SketchMutationResult setDimensionPlacement(std::size_t index,
                                             double offsetMm,
                                             double angleRad);
  bool setDimensionValue(std::size_t index, double valueMm);
  bool setDimensionConstraint(std::size_t index, ConstraintId constraintId);

  // Persistence boundary for v3 stable identities. Geometry values are
  // materialized first, then their persisted IDs replace generated IDs in one
  // validated operation before any references are decoded.
  [[nodiscard]] bool restoreGeometryIds(
      std::vector<GeometryId> lineIds,
      std::vector<GeometryId> circleIds,
      std::vector<GeometryId> arcIds,
      std::vector<GeometryId> bezierIds);
  [[nodiscard]] GeometryId nextGeometryId() const noexcept;
  [[nodiscard]] ConstraintId nextConstraintId() const noexcept;
  [[nodiscard]] DimensionId nextDimensionId() const noexcept;
  [[nodiscard]] std::size_t nextElementId() const noexcept;
  [[nodiscard]] bool restoreIdAllocators(std::size_t nextElementId,
                                         GeometryId nextGeometryId,
                                         ConstraintId nextConstraintId,
                                         DimensionId nextDimensionId);

  ConstraintId addConstraint(Constraint constraint);
  // Safe interactive boundary. The candidate is committed only when the
  // complete affected constraint component remains valid.
  [[nodiscard]] ConstraintApplyResult tryApplyConstraint(
      Constraint constraint, bool commitRedundant = false);
  // Persistence-only bulk restore: the caller validates the complete set,
  // then the solver runs once after every constraint is present.
  [[nodiscard]] bool restoreConstraints(std::vector<Constraint> constraints);
  [[nodiscard]] SketchMutationResult setConstraintValue(
      ConstraintId id, double value);
  [[nodiscard]] SketchMutationResult setConstraintMode(
      ConstraintId id, ConstraintMode mode,
      std::optional<double> drivingValue = std::nullopt);
  bool removeConstraint(ConstraintId id);
  void clearConstraints();
  [[nodiscard]] const std::vector<Constraint>& constraints() const noexcept;
  [[nodiscard]] bool isGeometryLocked(GeometryId id) const noexcept;
  [[nodiscard]] bool isElementLocked(std::size_t elementId) const noexcept;
  [[nodiscard]] bool isPointReferenceLocked(PointReference reference) const noexcept;
  void restoreLockedGeometryFrom(const Sketch& baseline);

  [[nodiscard]] double widthMm() const noexcept;
  [[nodiscard]] double heightMm() const noexcept;
  [[nodiscard]] const std::vector<Line>& lines() const noexcept;
  [[nodiscard]] const std::vector<Circle>& circles() const noexcept;
  [[nodiscard]] const std::vector<Arc>& arcs() const noexcept;
  [[nodiscard]] const std::vector<Bezier>& beziers() const noexcept;
  [[nodiscard]] const std::vector<Dimension>& dimensions() const;
  [[nodiscard]] std::optional<Point> referencedPoint(
      PointReference reference) const noexcept;
  [[nodiscard]] bool isClosed() const noexcept;

 private:
  friend class SketchSolver;
  friend class SketchTestAccess;
  void invalidateStructureIndexes() noexcept;
  void rebuildIdentityIndexes() const;
  void rebuildStructureIndexes() const;
  void updateBounds() noexcept;
  void journalCaptureGeometry(GeometryId id);
  void journalCapturePoint(PointReference reference);
  void journalCaptureComponents(const std::vector<GeometryId>& seeds);
  void journalCaptureConstraint(ConstraintId id);
  void journalCaptureDimension(std::size_t index);
  void journalCaptureAllDimensions();
  void journalCaptureCenters();
  void journalRecordAddedGeometry(GeometryId id, GeometryKind kind,
                                  std::size_t index);
  void journalRecordAddedConstraint(ConstraintId id, std::size_t index);
  void journalRecordAddedDimension(std::size_t index);
  [[nodiscard]] std::vector<ConstraintId>
  invalidReferenceConstraintIds() const;
  struct JournalLine {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Line> before;
  };
  struct JournalCircle {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Circle> before;
  };
  struct JournalArc {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Arc> before;
  };
  struct JournalBezier {
    GeometryId id{kInvalidGeometryId};
    std::size_t index{};
    std::optional<Bezier> before;
  };
  struct JournalConstraint {
    ConstraintId id{kInvalidConstraintId};
    std::size_t index{};
    std::optional<Constraint> before;
  };
  struct JournalDimension {
    std::size_t token{};
    std::optional<std::size_t> beforeIndex;
    std::optional<Dimension> before;
  };
  struct DeltaJournalState {
    std::vector<JournalLine> lines;
    std::vector<JournalCircle> circles;
    std::vector<JournalArc> arcs;
    std::vector<JournalBezier> beziers;
    std::vector<JournalConstraint> constraints;
    std::vector<JournalDimension> dimensions;
    // Stable, allocation-only transaction origin metadata. Geometry and
    // constraints already own persistent IDs; Dimensions receive journal-
    // local tokens so current vector indices may shift arbitrarily.
    std::vector<GeometryId> beforeLineIds;
    std::vector<GeometryId> beforeCircleIds;
    std::vector<GeometryId> beforeArcIds;
    std::vector<GeometryId> beforeBezierIds;
    std::vector<ConstraintId> beforeConstraintIds;
    std::vector<std::size_t> dimensionTokens;
    std::vector<std::vector<std::size_t>> parentDimensionTokensBefore;
    std::size_t nextDimensionToken{};
    std::size_t beforeDimensionCount{};
    std::optional<std::vector<std::size_t>> centersBefore;
    std::size_t beforeNextElementId{};
    GeometryId beforeNextGeometryId{};
    ConstraintId beforeNextConstraintId{};
    DimensionId beforeNextDimensionId{};
    std::uint64_t beforeSemanticFingerprint{};
    std::vector<ConstraintId> beforeInvalidConstraintIds;
    std::uint64_t beforeLastSolvedFingerprint{};
    bool beforeHasLastSolvedFingerprint{false};
    bool beforeLastSolveConverged{true};
    std::size_t beforeLastSolveViolatedConstraints{};
    double beforeLastSolveMaxNormalizedResidual{};
    std::size_t beforeLastSolveUnsupported{};
    std::size_t beforeLastSolveInvalidReferences{};
    SolveResult beforeLastSolveResult{};
  };
  double widthMm_{60.0};
  double heightMm_{40.0};
  std::vector<Line> lines_;
  std::vector<Circle> circles_;
  std::vector<Arc> arcs_;
  std::vector<Bezier> beziers_;
  std::vector<GeometryId> lineIds_;
  std::vector<GeometryId> circleIds_;
  std::vector<GeometryId> arcIds_;
  std::vector<GeometryId> bezierIds_;
  mutable std::vector<Dimension> dimensions_;
  std::vector<Constraint> constraints_;

  // Elements created by tools that expose a persistent virtual center node.
  // Currently used by RectangleMode::FromCenter.
  std::vector<std::size_t> centerNodeElementIds_;
  std::size_t nextElementId_{1};
  GeometryId nextGeometryId_{1};
  ConstraintId nextConstraintId_{1};
  DimensionId nextDimensionId_{1};
  mutable bool structureIndexesDirty_{true};
  mutable bool connectivityDirty_{true};
  mutable std::unordered_map<GeometryId, GeometryLocation> geometryIndex_;
  mutable std::unordered_map<ConstraintId, std::size_t> constraintIndex_;
  mutable std::unordered_map<GeometryId, std::vector<GeometryId>> connectivity_;
  mutable std::unordered_map<GeometryId, std::vector<ConstraintId>>
      geometryConstraints_;
  mutable std::uint64_t lastSolvedFingerprint_{};
  mutable bool hasLastSolvedFingerprint_{false};
  mutable bool lastSolveConverged_{true};
  mutable std::size_t lastSolveViolatedConstraints_{};
  mutable double lastSolveMaxNormalizedResidual_{};
  mutable std::size_t lastSolveUnsupported_{};
  mutable std::size_t lastSolveInvalidReferences_{};
  mutable SolveResult lastSolveResult_{};
  std::vector<DeltaJournalState> deltaJournals_;
};

}  // namespace solidar::sketch
