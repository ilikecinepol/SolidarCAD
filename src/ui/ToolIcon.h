#pragma once

#include <QIcon>

namespace solidar {

// Canonical icon catalogue for the Model and Sketcher ribbons. Keeping the
// geometry in one painter guarantees the same stroke, palette and rendering
// quality for every tool, including menus and the compact history view.
enum class ToolIconKind {
  CreateSketch,
  Extrude,
  Pocket,
  Revolve,
  Fillet,
  Chamfer,
  Move,
  Ruler,
  Shell,
  Draft,
  Mirror,
  LinearPattern,
  CircularPattern,
  Line,
  Rectangle,
  Circle,
  Arc,
  Projection,
  Polygon,
  Slot,
  Text,
  SketchMirror,
  Delete,
  Clear,
  AutoDimension,
  OrthogonalConstraint,
  CoincidentConstraint,
  PerpendicularConstraint,
  ParallelConstraint,
  EqualConstraint,
  TangentConstraint,
  LockConstraint,
  CircleCenterRadius,
  CircleTwoPoints,
  CircleThreePoints,
  CircleThreeTangents,
  CircleTwoTangentsRadius,
  RectangleTwoPoints,
  RectangleThreePoints,
  RectangleFromCenter
};

[[nodiscard]] QIcon toolIcon(ToolIconKind kind);

}  // namespace solidar
