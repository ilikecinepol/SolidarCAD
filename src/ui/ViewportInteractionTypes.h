#pragma once

#include <QString>
#include <variant>

#include "model/Feature.h"
#include "model/SketchPlacement.h"
#include "sketch/Sketch.h"

namespace solidar {

enum class ViewportCancelReason {
  ActiveTool,
  SketchPlaneSelection,
  ImagePlaneSelection,
  NestedReselection
};

enum class BasePlane { XY, XZ, YZ };

enum class LegacySolidFace {
  InitialCap,
  EndCap,
  Bottom,
  Top,
  Front,
  Right,
  Back,
  Left
};

struct DatumPlanePick {
  BasePlane plane{BasePlane::XY};
  SketchPlacement placement{SketchPlacement::xy()};
};

struct BodyFacePick {
  FaceReference face;
};

struct LegacySolidFacePick {
  LegacySolidFace face{LegacySolidFace::InitialCap};
  SketchPlacement placement{SketchPlacement::xy()};
  Vector3d outwardNormal{};
  sketch::Sketch geometry;
};

struct SketchPlanePick {
  std::variant<DatumPlanePick, BodyFacePick, LegacySolidFacePick> source;
  QString presentationLabel;
  // Eye direction (from the model towards the camera) at the click. Sketcher
  // uses only its hemisphere so the picked face remains on the viewer's side;
  // it never changes the persisted SketchPlacement or feature direction.
  Vector3d viewDirection{};
};

struct SketchRegionPick {
  SketchId sketchId{kInvalidSketchId};
  SketchPlacement placement{SketchPlacement::xy()};
  sketch::Sketch geometry;
};

struct ExtrusionSourcePick {
  std::variant<SketchRegionPick, BodyFacePick, LegacySolidFacePick> source;
  QString presentationLabel;
};

}  // namespace solidar

Q_DECLARE_METATYPE(solidar::ViewportCancelReason)
Q_DECLARE_METATYPE(solidar::BasePlane)
Q_DECLARE_METATYPE(solidar::DatumPlanePick)
Q_DECLARE_METATYPE(solidar::BodyFacePick)
Q_DECLARE_METATYPE(solidar::LegacySolidFace)
Q_DECLARE_METATYPE(solidar::LegacySolidFacePick)
Q_DECLARE_METATYPE(solidar::SketchPlanePick)
Q_DECLARE_METATYPE(solidar::SketchRegionPick)
Q_DECLARE_METATYPE(solidar::ExtrusionSourcePick)
