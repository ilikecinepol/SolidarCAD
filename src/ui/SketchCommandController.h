#pragma once

#include "sketch/Sketch.h"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace solidar {

enum class SketchCommandError {
  None,
  InvalidInput,
  StaleReference,
  Duplicate,
  Conflict,
  Unsupported,
  SolverFailed,
  TransactionMismatch,
  MutationRejected,
  InternalFailure,
};

struct SketchCommandEffects {
  bool committedRenderSceneDirty{};
  bool geometryChanged{};
  bool constraintsChanged{};
  bool dimensionsChanged{};
  bool selectionMayBeStale{};
  bool diagnosticsRequired{};
};

struct SketchCommandResult {
  bool accepted{};
  SketchCommandError error{SketchCommandError::None};
  sketch::SketchDelta delta;
  SketchCommandEffects effects;
  std::vector<sketch::GeometryId> changedGeometryIds;
  std::vector<sketch::ConstraintId> changedConstraintIds;
  std::vector<sketch::DimensionId> changedDimensionIds;
  std::optional<sketch::ConstraintApplyResult> constraintApplyResult;
};

struct SketchTransactionToken {
  std::uint64_t serial{};
  std::uint64_t sketchGeneration{};
  std::uint64_t initialFingerprint{};
  std::uintptr_t sketchIdentity{};
  std::size_t journalDepthBefore{};

  [[nodiscard]] bool valid() const noexcept { return serial != 0; }
  friend bool operator==(const SketchTransactionToken&,
                         const SketchTransactionToken&) = default;
};

struct SetSketchRectangleCommand {
  double widthMm{};
  double heightMm{};
};
struct ClearSketchCommand {};
struct AddLineCommand {
  sketch::Point start;
  sketch::Point end;
  std::optional<std::size_t> elementId;
};
struct AddRectangleCommand {
  sketch::Point first;
  sketch::Point second;
  std::optional<sketch::Point> third;
  std::optional<sketch::Point> fourth;
  bool markCenter{};
};
struct AddCircleCommand {
  sketch::Point center;
  double radiusMm{};
  bool dashed{};
};
struct AddArcCommand {
  sketch::Point center;
  double radiusMm{};
  double startAngleRad{};
  double sweepAngleRad{};
  bool dashed{};
};
struct AddBezierCommand {
  sketch::Point start;
  sketch::Point control1;
  sketch::Point control2;
  sketch::Point end;
  bool dashed{};
};
struct ProjectedLineSegment {
  sketch::Point first;
  sketch::Point second;
};
struct ProjectLineChain {
  std::vector<ProjectedLineSegment> segments;
};
struct ProjectCircle {
  sketch::Point center;
  double radiusMm{};
};
struct ProjectArc {
  sketch::Point center;
  double radiusMm{};
  double startAngleRad{};
  double sweepAngleRad{};
};
struct ProjectGeometryCommand {
  std::variant<ProjectLineChain, ProjectCircle, ProjectArc> geometry;
};
using SketchPrimitiveCommand =
    std::variant<AddLineCommand, AddRectangleCommand, AddCircleCommand,
                 AddArcCommand, AddBezierCommand>;
struct AddPrimitiveBatchCommand {
  std::vector<SketchPrimitiveCommand> primitives;
};
struct RemoveGeometryCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
};
struct RemoveElementCommand {
  std::size_t elementId{};
};
struct DeleteSelectionCommand {
  std::vector<sketch::GeometryId> geometryIds;
  std::vector<std::size_t> elementIds;
};
struct AddConstraintCommand {
  sketch::Constraint constraint;
  bool bestEffort{};
};
enum class SketchPointBindingKind {
  Coincident,
  Midpoint,
  PointOnLine,
  PointOnCircle,
  PointOnArc,
  XAxis,
  YAxis,
  Origin,
};
struct BindPointCommand {
  sketch::PointReference movingPoint;
  SketchPointBindingKind kind{SketchPointBindingKind::Coincident};
  sketch::PointReference targetPoint;
  sketch::GeometryId targetGeometry{sketch::kInvalidGeometryId};
};
struct AutoConstrainNewGeometryCommand {
  std::vector<sketch::GeometryId> newGeometryIds;
  double toleranceMm{};
};
struct RemoveConstraintCommand {
  sketch::ConstraintId id{sketch::kInvalidConstraintId};
};
struct SetConstraintValueCommand {
  sketch::ConstraintId id{sketch::kInvalidConstraintId};
  double value{};
};
struct StoreDimensionCommand {
  sketch::Dimension dimension;
};
struct UpsertDrivingDimensionCommand {
  sketch::Dimension dimension;
  sketch::Constraint constraint;
  std::optional<sketch::DimensionId> existingDimensionId;
  bool ensureParallel{};
};
struct RemoveDimensionCommand {
  sketch::DimensionId id{sketch::kInvalidDimensionId};
};
struct SetDimensionValueCommand {
  sketch::DimensionId id{sketch::kInvalidDimensionId};
  double value{};
};
struct SetDimensionPlacementCommand {
  sketch::DimensionId id{sketch::kInvalidDimensionId};
  double offsetMm{};
  double angleRad{};
};
struct SetDimensionDrivingCommand {
  sketch::DimensionId id{sketch::kInvalidDimensionId};
  bool driving{};
};
struct SetLineDashedCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  bool dashed{};
};
struct SetCircleDashedCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  bool dashed{};
};
struct SetArcDashedCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  bool dashed{};
};
struct SetBezierDashedCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  bool dashed{};
};
struct SetElementDashedCommand {
  std::size_t elementId{};
  bool dashed{};
};
struct SetSelectionDashedCommand {
  std::vector<sketch::GeometryId> lineIds;
  std::vector<std::size_t> elementIds;
  std::vector<sketch::GeometryId> circleIds;
  std::vector<sketch::GeometryId> arcIds;
  bool dashed{};
  std::vector<sketch::GeometryId> bezierIds;
};
struct TranslatePointCommand {
  sketch::PointReference point;
  double dxMm{};
  double dyMm{};
};
struct MoveArcEndpointCommand {
  sketch::GeometryId arcId{sketch::kInvalidGeometryId};
  bool start{};
  sketch::Point target;
};
struct TranslateLinesCommand {
  std::vector<sketch::GeometryId> lineIds;
  double dxMm{};
  double dyMm{};
};
struct TranslateSelectionCommand {
  std::vector<std::size_t> elementIds;
  std::vector<sketch::GeometryId> circleIds;
  std::vector<sketch::GeometryId> arcIds;
  double dxMm{};
  double dyMm{};
  std::vector<sketch::GeometryId> bezierIds;
};
struct TranslateCircleCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  double dxMm{};
  double dyMm{};
};
struct TranslateArcCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  double dxMm{};
  double dyMm{};
};
struct TranslateBezierCommand {
  sketch::GeometryId id{sketch::kInvalidGeometryId};
  double dxMm{};
  double dyMm{};
};
struct ApplySketchDeltaCommand {
  const sketch::SketchDelta* delta{};
  bool forward{};
};
struct MirrorGeometryCommand {
  sketch::GeometryId axisId{sketch::kInvalidGeometryId};
  std::vector<sketch::GeometryId> sourceIds;
};
struct TrimGeometryCommand {
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  double firstParameter{};
  double secondParameter{};
  bool fullGeometry{};
};

using SketchLiveCommand =
    std::variant<SetDimensionPlacementCommand, TranslatePointCommand,
                 MoveArcEndpointCommand, TranslateLinesCommand,
                 TranslateSelectionCommand, TranslateCircleCommand,
                 TranslateArcCommand, TranslateBezierCommand>;

using SketchCommand = std::variant<
    SetSketchRectangleCommand, ClearSketchCommand, AddLineCommand,
    AddRectangleCommand, AddCircleCommand, AddArcCommand, AddBezierCommand,
    ProjectGeometryCommand, AddPrimitiveBatchCommand,
    RemoveGeometryCommand, RemoveElementCommand, DeleteSelectionCommand,
    AddConstraintCommand, BindPointCommand, AutoConstrainNewGeometryCommand,
    RemoveConstraintCommand, SetConstraintValueCommand, StoreDimensionCommand,
    UpsertDrivingDimensionCommand,
    RemoveDimensionCommand, SetDimensionValueCommand,
    SetDimensionPlacementCommand, SetDimensionDrivingCommand,
    SetLineDashedCommand,
    SetCircleDashedCommand, SetArcDashedCommand, SetBezierDashedCommand,
    SetElementDashedCommand,
    TranslatePointCommand, SetSelectionDashedCommand,
    MoveArcEndpointCommand, TranslateLinesCommand, TranslateSelectionCommand,
    TranslateCircleCommand, TranslateArcCommand, TranslateBezierCommand,
    ApplySketchDeltaCommand,
    MirrorGeometryCommand, TrimGeometryCommand>;

class SketchCommandController final {
 public:
  [[nodiscard]] SketchCommandResult execute(sketch::Sketch& sketch,
                                            const SketchCommand& command) const;
  [[nodiscard]] SketchCommandResult executeInTransaction(
      sketch::Sketch& sketch, SketchTransactionToken token,
      std::uint64_t sketchGeneration, const SketchLiveCommand& command);

  [[nodiscard]] std::optional<SketchTransactionToken> beginTransaction(
      sketch::Sketch& sketch, std::uint64_t sketchGeneration);
  [[nodiscard]] std::optional<sketch::SketchDelta> finishTransaction(
      sketch::Sketch& sketch, SketchTransactionToken token,
      std::uint64_t sketchGeneration);
  [[nodiscard]] bool cancelTransaction(sketch::Sketch& sketch,
                                       SketchTransactionToken token,
                                       std::uint64_t sketchGeneration) noexcept;
  [[nodiscard]] bool hasActiveTransaction() const noexcept;
  [[nodiscard]] bool invalidateTransactions(
      sketch::Sketch& sketch, std::uint64_t sketchGeneration) noexcept;

  [[nodiscard]] bool replaceSketch(sketch::Sketch& target,
                                   const sketch::Sketch& replacement,
                                   std::uint64_t currentGeneration,
                                   std::uint64_t replacementGeneration);

 private:
  struct ActiveTransaction {
    SketchTransactionToken token;
  };

  [[nodiscard]] bool matches(const sketch::Sketch& sketch,
                             SketchTransactionToken token,
                             std::uint64_t generation) const noexcept;

  std::optional<ActiveTransaction> activeTransaction_;
  std::uint64_t nextTransactionSerial_{1};
};

}  // namespace solidar
