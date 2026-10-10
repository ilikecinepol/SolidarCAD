#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "sketch/Sketch.h"

namespace solidar {

// UI-independent tool identity used by the Sketch interaction state machine.
// SketchCanvas keeps its public Qt-facing Tool enum and maps it at the seam.
enum class SketchInteractionTool {
  Select,
  Line,
  Rectangle,
  Circle,
  Arc,
  Bezier,
  Projection,
  AutoDimension,
  LockConstraint,
  OrthogonalConstraint,
  CoincidentConstraint,
  PerpendicularConstraint,
  ParallelConstraint,
  EqualConstraint,
  TangentConstraint,
  Mirror,
  Trim
};

enum class SketchGeometryOperandKind {
  Line,
  Rectangle,
  Circle,
  Arc,
  Bezier,
  Point,
  XAxis,
  YAxis
};

struct SketchGeometryOperand {
  SketchGeometryOperandKind kind{SketchGeometryOperandKind::Line};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  std::size_t elementId{};
};

struct SketchLineEndpointDrag {
  sketch::GeometryId lineId{sketch::kInvalidGeometryId};
  bool start{};
};
struct SketchArcEndpointDrag {
  sketch::GeometryId arcId{sketch::kInvalidGeometryId};
  bool start{};
};
struct SketchCircleCenterDrag {
  sketch::GeometryId circleId{sketch::kInvalidGeometryId};
};
struct SketchElementCenterDrag {
  std::size_t elementId{};
};
struct SketchBezierPointDrag {
  sketch::GeometryId bezierId{sketch::kInvalidGeometryId};
  std::uint8_t pointIndex{};
};
using SketchPointDragTarget =
    std::variant<std::monostate, SketchLineEndpointDrag,
                 SketchArcEndpointDrag, SketchCircleCenterDrag,
                 SketchElementCenterDrag, SketchBezierPointDrag>;

enum class SketchAutoDimensionTarget {
  None,
  Line,
  Circle,
  Angle,
  Points,
  LineDistance
};
enum class SketchPointDimensionMode { Aligned, X, Y };
enum class SketchDatumReference { XAxis, YAxis, Origin };

struct SketchAutoDimensionState {
  SketchAutoDimensionTarget target{SketchAutoDimensionTarget::None};
  std::optional<sketch::GeometryId> geometryId;
  std::optional<sketch::PointReference> firstPoint;
  std::optional<sketch::PointReference> secondPoint;
  std::optional<SketchDatumReference> firstDatum;
  bool datumModeFixed{};
  std::optional<double> offsetMm;
  std::optional<double> angleRad;
  SketchPointDimensionMode pointMode{SketchPointDimensionMode::Aligned};
  std::optional<sketch::GeometryId> directLineId;
  std::optional<sketch::GeometryId> angleFirstLine;
  std::optional<sketch::GeometryId> angleSecondLine;
  std::optional<sketch::GeometryId> distanceFirstLine;
  std::optional<sketch::GeometryId> distanceSecondLine;
};

// Semantic identity for a dimension interaction. Dimensions do not yet own a
// persistent model ID, so the controller must never retain a vector slot.
// Canvas resolves this key against the current sketch immediately before use.
struct SketchDimensionReference {
  sketch::DimensionId id{sketch::kInvalidDimensionId};
};

struct SketchDimensionInteractionState {
  std::optional<SketchDimensionReference> selected;
  std::optional<SketchDimensionReference> draggingLine;
  std::optional<SketchDimensionReference> draggingLabel;
  std::optional<SketchDimensionReference> editing;
  std::vector<double> labelAlongMm;
  std::vector<double> labelOffsetMm;
};

struct SketchConstraintInteractionState {
  std::optional<sketch::GeometryId> pointOnLineCarrier;
  std::optional<sketch::GeometryId> pointOnCircleCarrier;
  std::optional<sketch::GeometryId> perpendicularFirstLine;
  std::optional<sketch::GeometryId> parallelFirstLine;
  std::optional<SketchGeometryOperand> equalFirst;
  std::optional<SketchGeometryOperand> tangentFirst;
  std::optional<SketchGeometryOperand> hoverOperand;
  std::optional<sketch::Point> hoverPoint;
  std::optional<sketch::PointReference> coincidentFirstPoint;
};

struct SketchLineCreationState {
  std::optional<sketch::GeometryId> startLineCarrier;
  std::optional<sketch::GeometryId> endLineCarrier;
  std::optional<sketch::GeometryId> startMidpointCarrier;
  std::optional<sketch::GeometryId> endMidpointCarrier;
  std::optional<sketch::GeometryId> startArcCarrier;
  std::optional<sketch::GeometryId> endArcCarrier;
};

struct SketchCreationGestureState {
  std::optional<sketch::Point> anchor;
  std::vector<sketch::Point> rectanglePoints;
  std::vector<sketch::Point> circlePoints;
  std::vector<sketch::Point> arcPoints;
  std::vector<sketch::Point> bezierPoints;
  std::vector<sketch::GeometryId> circleGuideIds;
};

enum class SketchMirrorGeometryKind { Line, Circle, Arc, Bezier };
struct SketchMirrorGeometryRef {
  SketchMirrorGeometryKind kind{SketchMirrorGeometryKind::Line};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
};
struct SketchMirrorInteractionState {
  std::vector<SketchMirrorGeometryRef> source;
};

enum class SketchTrimGeometryKind { Line, Circle, Arc, Bezier };
struct SketchTrimPreview {
  SketchTrimGeometryKind kind{SketchTrimGeometryKind::Line};
  sketch::GeometryId geometryId{sketch::kInvalidGeometryId};
  double firstParameter{};
  double secondParameter{1.0};
  bool fullGeometry{};
};
struct SketchTrimInteractionState {
  std::optional<SketchTrimPreview> preview;
};

struct SketchArcInteractionState {
  std::optional<double> chordAngleRad;
  std::optional<double> sagittaSign;
  bool dimensionKeyboardEdit{};
};

struct SketchCameraGestureState {
  enum class Kind { None, Pan, Orbit };
  enum class Button { None, Middle, Right };

  Kind kind{Kind::None};
  Button button{Button::None};
  double lastX{};
  double lastY{};
  bool moved{};
  double panX{};
  double panY{};
};

struct SketchScreenPoint {
  double x{};
  double y{};
};

struct SketchSelectionBoxState {
  bool active{};
  SketchScreenPoint start;
  SketchScreenPoint current;
  bool additive{};
};

struct SketchDragInteractionState {
  bool active{};
  sketch::Point current;
};

struct SketchToolState {
  SketchInteractionTool tool{SketchInteractionTool::Select};
  SketchPointDragTarget pointDrag;
  SketchDimensionInteractionState dimension;
  SketchAutoDimensionState autoDimension;
  SketchConstraintInteractionState constraint;
  SketchLineCreationState lineCreation;
  SketchCreationGestureState creation;
  SketchMirrorInteractionState mirror;
  SketchTrimInteractionState trim;
  SketchArcInteractionState arc;
  SketchCameraGestureState camera;
  SketchSelectionBoxState selectionBox;
  SketchDragInteractionState drag;
  bool twoTangentRadiusPreviewActive{};
};

class SketchInteractionController final {
 public:
  [[nodiscard]] const SketchToolState& snapshot() const noexcept;
  [[nodiscard]] SketchInteractionTool tool() const noexcept;
  [[nodiscard]] bool hasActiveGesture() const noexcept;

  void switchTool(SketchInteractionTool tool) noexcept;
  void resetForProject() noexcept;
  void cancelGesture() noexcept;

  // Typed commands are the only mutation seam. SketchCanvas owns event and
  // geometry decisions, while this controller owns every partial interaction
  // and enforces complete cancel/reset transitions.
  void beginCreation(SketchCreationGestureState creation = {}) noexcept;
  void updateCreation(SketchCreationGestureState creation) noexcept;
  void setCreationAnchor(std::optional<sketch::Point> anchor) noexcept;
  void appendRectanglePoint(sketch::Point point);
  void clearRectanglePoints() noexcept;
  void appendCirclePoint(sketch::Point point);
  void clearCirclePoints() noexcept;
  void appendArcPoint(sketch::Point point);
  void clearArcPoints() noexcept;
  void appendBezierPoint(sketch::Point point);
  void clearBezierPoints() noexcept;
  void appendCircleGuide(sketch::GeometryId lineId);
  void clearCircleGuides() noexcept;
  void updateLineCreation(SketchLineCreationState creation) noexcept;
  void updateArcCreation(SketchArcInteractionState arc) noexcept;
  void setTwoTangentPreview(bool active) noexcept;
  void completeCreation() noexcept;
  void cancelCreation() noexcept;

  void beginAutoDimension(SketchAutoDimensionState automatic) noexcept;
  void updateAutoDimension(SketchAutoDimensionState automatic) noexcept;
  void completeAutoDimension() noexcept;
  void cancelAutoDimension() noexcept;

  void beginDimensionEdit(SketchDimensionReference dimension) noexcept;
  void updateDimension(SketchDimensionInteractionState dimension) noexcept;
  void selectDimension(
      std::optional<SketchDimensionReference> dimension) noexcept;
  void beginDimensionLabelDrag(SketchDimensionReference dimension) noexcept;
  void beginDimensionLineDrag(SketchDimensionReference dimension) noexcept;
  void endDimensionLabelDrag() noexcept;
  void endDimensionLineDrag() noexcept;
  void clearDimensionLabels() noexcept;
  void setDimensionLabelPosition(std::size_t index, double alongMm,
                                 double offsetMm);
  void eraseDimensionLabel(std::size_t index) noexcept;
  void appendDimensionLabel(double alongMm, double offsetMm);
  void completeDimensionInteraction() noexcept;
  void cancelDimensionInteraction() noexcept;

  void beginConstraintOperands(
      SketchConstraintInteractionState constraint) noexcept;
  void updateConstraintOperands(
      SketchConstraintInteractionState constraint) noexcept;
  void setPointOnLineCarrier(
      std::optional<sketch::GeometryId> carrier) noexcept;
  void setPointOnCircleCarrier(
      std::optional<sketch::GeometryId> carrier) noexcept;
  void setPerpendicularFirstLine(
      std::optional<sketch::GeometryId> line) noexcept;
  void setParallelFirstLine(
      std::optional<sketch::GeometryId> line) noexcept;
  void setEqualFirst(std::optional<SketchGeometryOperand> operand) noexcept;
  void setTangentFirst(std::optional<SketchGeometryOperand> operand) noexcept;
  void setCoincidentFirstPoint(
      std::optional<sketch::PointReference> point) noexcept;
  void completeConstraintOperands() noexcept;
  void cancelConstraintOperands() noexcept;

  void updateMirror(SketchMirrorInteractionState mirror) noexcept;
  void updateTrim(SketchTrimInteractionState trim) noexcept;

  void beginPointDrag(SketchPointDragTarget target,
                      sketch::Point current = {}) noexcept;
  void updatePointDrag(sketch::Point current) noexcept;
  void completePointDrag() noexcept;
  void cancelPointDrag() noexcept;

  void beginSelectionBox(SketchScreenPoint start, bool additive) noexcept;
  void updateSelectionBox(SketchScreenPoint current) noexcept;
  void completeSelectionBox() noexcept;
  void cancelSelectionBox() noexcept;

  void beginCameraGesture(SketchCameraGestureState::Kind kind,
                          SketchCameraGestureState::Button button,
                          double x, double y) noexcept;
  void updateCameraGesture(double x, double y, bool moved) noexcept;
  void panCameraBy(double dx, double dy, double x, double y) noexcept;
  void setCameraPan(double x, double y) noexcept;
  void endCameraGesture() noexcept;

 private:
  static void resetTransient(SketchToolState& state) noexcept;

  SketchToolState state_;
};

}  // namespace solidar
