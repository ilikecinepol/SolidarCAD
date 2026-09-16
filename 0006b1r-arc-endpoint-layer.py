from __future__ import annotations

import argparse
from pathlib import Path
import sys

ROOT = Path.cwd()
SKETCH = ROOT / 'src/sketch/Sketch.cpp'
CANVAS = ROOT / 'src/ui/SketchCanvas.cpp'
HEADER = ROOT / 'src/sketch/Sketch.h'


def load_preserve(path: Path):
    raw = path.read_bytes()
    if raw.startswith(b'\xef\xbb\xbf'):
        bom = b'\xef\xbb\xbf'
        raw = raw[3:]
    else:
        bom = b''
    newline = '\r\n' if b'\r\n' in raw else '\n'
    text = raw.decode('utf-8').replace('\r\n', '\n')
    return text, newline, bom


def save_preserve(path: Path, text: str, newline: str, bom: bytes):
    data = text.replace('\n', newline).encode('utf-8')
    path.write_bytes(bom + data)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected exactly 1 match, found {count}')
    return text.replace(old, new, 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='validate only; do not write files')
    args = parser.parse_args()

    for path in (SKETCH, CANVAS, HEADER):
        if not path.exists():
            print(f'ERROR: missing {path}', file=sys.stderr)
            return 2

    header, _, _ = load_preserve(HEADER)
    sketch, sketch_nl, sketch_bom = load_preserve(SKETCH)
    canvas, canvas_nl, canvas_bom = load_preserve(CANVAS)

    # Require the exact PointOnArc layer from 0006b2a first.
    if 'PointOnArc};' not in header and 'PointOnArc\n};' not in header and 'PointOnArc};' not in header.replace(' ', ''):
        if 'PointOnArc' not in header:
            print('ERROR: PointOnArc is not present in Sketch.h; apply 0006b2a first.', file=sys.stderr)
            return 3
    if 'bool Sketch::setPointOnArc(' not in sketch:
        print('ERROR: setPointOnArc() is not present in Sketch.cpp; apply 0006b2a first.', file=sys.stderr)
        return 3

    # Refuse a double-application.
    if 'bool moveArcEndpointRigid(Arc& arc, bool start, Point target)' in sketch:
        print('Arc endpoint layer is already present; nothing to do.')
        return 0

    try:
        # ------------------------------------------------------------------
        # Sketch.cpp: rigid endpoint movement and PointReference identity.
        # ------------------------------------------------------------------
        old = '''Point arcEndPoint(const Arc& arc) noexcept {
  const double angle = arc.startAngleRad + arc.sweepAngleRad;
  return {arc.center.xMm + arc.radiusMm * std::cos(angle),
          arc.center.yMm + arc.radiusMm * std::sin(angle)};
}

Sketch::Sketch() { clear(); }
'''
        new = '''Point arcEndPoint(const Arc& arc) noexcept {
  const double angle = arc.startAngleRad + arc.sweepAngleRad;
  return {arc.center.xMm + arc.radiusMm * std::cos(angle),
          arc.center.yMm + arc.radiusMm * std::sin(angle)};
}

namespace {

// Keep an Arc rigid when one of its endpoint references has to move.
// Endpoint constraints translate the whole Arc instead of rebuilding its
// radius/start/sweep inside the sequential solver.
bool moveArcEndpointRigid(Arc& arc, bool start, Point target) {
  const Point current = start ? arcStartPoint(arc) : arcEndPoint(arc);
  const double dx = target.xMm - current.xMm;
  const double dy = target.yMm - current.yMm;
  if (std::abs(dx) <= 1e-12 && std::abs(dy) <= 1e-12) return true;
  arc.center.xMm += dx;
  arc.center.yMm += dy;
  return true;
}

}  // namespace

Sketch::Sketch() { clear(); }
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp helper')

        old = '''        if (a.circleId != kInvalidGeometryId ||
            b.circleId != kInvalidGeometryId)
          return a.circleId != kInvalidGeometryId &&
                 a.circleId == b.circleId;

        return a.lineId == b.lineId &&
               a.start == b.start;
'''
        new = '''        if (a.circleId != kInvalidGeometryId ||
            b.circleId != kInvalidGeometryId)
          return a.circleId != kInvalidGeometryId &&
                 a.circleId == b.circleId;

        if (a.arcId != kInvalidGeometryId ||
            b.arcId != kInvalidGeometryId)
          return a.arcId != kInvalidGeometryId &&
                 b.arcId != kInvalidGeometryId &&
                 a.arcId == b.arcId &&
                 a.start == b.start;

        return a.lineId == b.lineId &&
               a.start == b.start;
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp coincident identity')

        old = '''  // Coinciding both ends of one line would collapse it.
  if (firstReference.elementCenterId == 0 &&
      secondReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      secondReference.circleId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId &&
      firstReference.lineId == secondReference.lineId)
    return false;

  const Point target = *first;
  const Point oldSecond = *second;
'''
        new = '''  // Coinciding both ends of one line would collapse it.
  if (firstReference.elementCenterId == 0 &&
      secondReference.elementCenterId == 0 &&
      firstReference.circleId == kInvalidGeometryId &&
      secondReference.circleId == kInvalidGeometryId &&
      firstReference.arcId == kInvalidGeometryId &&
      secondReference.arcId == kInvalidGeometryId &&
      firstReference.lineId != kInvalidGeometryId &&
      firstReference.lineId == secondReference.lineId)
    return false;

  // Never collapse both endpoints of one Arc onto each other.
  if (firstReference.arcId != kInvalidGeometryId &&
      firstReference.arcId == secondReference.arcId &&
      firstReference.start != secondReference.start)
    return false;

  const Point target = *first;
  const Point oldSecond = *second;

  if (std::hypot(target.xMm - oldSecond.xMm,
                 target.yMm - oldSecond.yMm) <= 1e-9)
    return true;
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp coincident guards')

        old = '''  if (secondReference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(secondReference.circleId);
    if (!index) return false;
    circles_[*index].center = target;
  } else {
'''
        new = '''  if (secondReference.circleId != kInvalidGeometryId) {
    const auto index = circleIndex(secondReference.circleId);
    if (!index) return false;
    circles_[*index].center = target;
  } else if (secondReference.arcId != kInvalidGeometryId) {
    const auto index = arcIndex(secondReference.arcId);
    if (!index) return false;
    if (!moveArcEndpointRigid(arcs_[*index], secondReference.start, target))
      return false;
  } else {
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp coincident arc move')

        old = '''  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto circle = circleIndex(pointReference.circleId);
    if (!circle) return false;
    circles_[*circle].center = target;
  } else {
'''
        new = '''  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto circle = circleIndex(pointReference.circleId);
    if (!circle) return false;
    circles_[*circle].center = target;
  } else if (pointReference.arcId != kInvalidGeometryId) {
    const auto arc = arcIndex(pointReference.arcId);
    if (!arc) return false;
    if (!moveArcEndpointRigid(arcs_[*arc], pointReference.start, target))
      return false;
  } else {
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp PointOnLine arc endpoint')

        old = '''  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto movingCircle =
        circleIndex(pointReference.circleId);
    if (!movingCircle) return false;

    circles_[*movingCircle].center = target;
  } else {
'''
        new = '''  } else if (pointReference.circleId != kInvalidGeometryId) {
    const auto movingCircle =
        circleIndex(pointReference.circleId);
    if (!movingCircle) return false;

    circles_[*movingCircle].center = target;
  } else if (pointReference.arcId != kInvalidGeometryId) {
    const auto movingArc = arcIndex(pointReference.arcId);
    if (!movingArc) return false;
    if (!moveArcEndpointRigid(arcs_[*movingArc], pointReference.start, target))
      return false;
  } else {
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp PointOnCircle arc endpoint')

        old = '''        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.lineId == kInvalidGeometryId ||
'''
        new = '''        if (first.circleId != kInvalidGeometryId ||
            second.circleId != kInvalidGeometryId) {
          return first.circleId != kInvalidGeometryId &&
                 second.circleId != kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.arcId != kInvalidGeometryId ||
            second.arcId != kInvalidGeometryId) {
          return first.arcId != kInvalidGeometryId &&
                 second.arcId != kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
        }

        if (first.lineId == kInvalidGeometryId ||
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp translatePoint identity')

        old = '''      circles_[*index].center.xMm += dxMm;
      circles_[*index].center.yMm += dyMm;
      continue;
    }

    const auto index =
        lineIndex(pointReference.lineId);
'''
        new = '''      circles_[*index].center.xMm += dxMm;
      circles_[*index].center.yMm += dyMm;
      continue;
    }

    if (pointReference.arcId != kInvalidGeometryId) {
      const auto index = arcIndex(pointReference.arcId);
      if (!index) continue;
      const auto current = referencedPoint(pointReference);
      if (!current) continue;
      const Point target{current->xMm + dxMm,
                         current->yMm + dyMm};
      (void)moveArcEndpointRigid(arcs_[*index], pointReference.start, target);
      continue;
    }

    const auto index =
        lineIndex(pointReference.lineId);
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp translatePoint arc move')

        old = '''    if (pointReference.elementCenterId != 0 ||
        pointReference.circleId !=
            kInvalidGeometryId ||
        pointReference.lineId ==
            kInvalidGeometryId)
'''
        new = '''    if (pointReference.elementCenterId != 0 ||
        pointReference.circleId !=
            kInvalidGeometryId ||
        pointReference.arcId !=
            kInvalidGeometryId ||
        pointReference.lineId ==
            kInvalidGeometryId)
'''
        sketch = replace_once(sketch, old, new, 'Sketch.cpp movedLineIds arc exclusion')

        # ------------------------------------------------------------------
        # SketchCanvas.cpp: make Arc endpoints participate in the same
        # automatic Coincident/PointOnLine/PointOnCircle persistence layer.
        # ------------------------------------------------------------------
        old = '''void autoCoincidentNewGeometry(
    sketch::Sketch& sketch,
    std::size_t oldLineCount,
    std::size_t oldCircleCount,
    double toleranceMm) {
'''
        new = '''void autoCoincidentNewGeometry(
    sketch::Sketch& sketch,
    std::size_t oldLineCount,
    std::size_t oldCircleCount,
    std::size_t oldArcCount,
    double toleranceMm) {
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas helper signature')

        old = '''        if (first.circleId != sketch::kInvalidGeometryId ||
            second.circleId != sketch::kInvalidGeometryId) {
          return first.circleId != sketch::kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
'''
        new = '''        if (first.circleId != sketch::kInvalidGeometryId ||
            second.circleId != sketch::kInvalidGeometryId) {
          return first.circleId != sketch::kInvalidGeometryId &&
                 first.circleId == second.circleId;
        }

        if (first.arcId != sketch::kInvalidGeometryId ||
            second.arcId != sketch::kInvalidGeometryId) {
          return first.arcId != sketch::kInvalidGeometryId &&
                 second.arcId != sketch::kInvalidGeometryId &&
                 first.arcId == second.arcId &&
                 first.start == second.start;
        }

        return first.lineId == second.lineId &&
               first.start == second.start;
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas reference identity')

        old = '''  for (std::size_t index = 0;
       index < std::min(oldCircleCount, sketch.circles().size());
       ++index) {
    const auto id = sketch.circleId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addOldReference(center);
  }

  // Existing virtual centers of composite elements are CAD points too.
'''
        new = '''  for (std::size_t index = 0;
       index < std::min(oldCircleCount, sketch.circles().size());
       ++index) {
    const auto id = sketch.circleId(index);
    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addOldReference(center);
  }

  for (std::size_t index = 0;
       index < std::min(oldArcCount, sketch.arcs().size());
       ++index) {
    const auto id = sketch.arcId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    sketch::PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addOldReference(endpoint);
    endpoint.start = false;
    addOldReference(endpoint);
  }

  // Existing virtual centers of composite elements are CAD points too.
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas old arc endpoints')

        old = '''  for (std::size_t index = oldCircleCount;
       index < sketch.circles().size();
       ++index) {
    const auto id = sketch.circleId(index);

    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addNewReference(center);
  }

  // Newly created center-based rectangles expose a virtual center node.
'''
        new = '''  for (std::size_t index = oldCircleCount;
       index < sketch.circles().size();
       ++index) {
    const auto id = sketch.circleId(index);

    if (id == sketch::kInvalidGeometryId)
      continue;

    sketch::PointReference center;
    center.circleId = id;
    addNewReference(center);
  }

  for (std::size_t index = oldArcCount;
       index < sketch.arcs().size(); ++index) {
    const auto id = sketch.arcId(index);
    if (id == sketch::kInvalidGeometryId) continue;

    sketch::PointReference endpoint;
    endpoint.arcId = id;
    endpoint.start = true;
    addNewReference(endpoint);
    endpoint.start = false;
    addNewReference(endpoint);
  }

  // Newly created center-based rectangles expose a virtual center node.
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas new arc endpoints')

        old = '''  }
}

}  // namespace
void SketchCanvas::commitPoint(sketch::Point point) {
'''
        new = '''  }
}

// Compatibility overload for existing Line/Rectangle/Circle creation paths.
// No Arc is new in those calls, so the current Arc count is the old count.
void autoCoincidentNewGeometry(
    sketch::Sketch& sketch,
    std::size_t oldLineCount,
    std::size_t oldCircleCount,
    double toleranceMm) {
  autoCoincidentNewGeometry(sketch, oldLineCount, oldCircleCount,
                            sketch.arcs().size(), toleranceMm);
}

}  // namespace
void SketchCanvas::commitPoint(sketch::Point point) {
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas compatibility overload')

        old = '''  pushUndoState();
  sketch_.addArc(arc->center,
                 arc->radiusMm,
                 arc->startAngleRad,
                 arc->sweepAngleRad,
                 arc->dashed);

  arcPoints_.clear();
'''
        new = '''  pushUndoState();
  const std::size_t oldLineCount = sketch_.lines().size();
  const std::size_t oldCircleCount = sketch_.circles().size();
  const std::size_t oldArcCount = sketch_.arcs().size();

  sketch_.addArc(arc->center,
                 arc->radiusMm,
                 arc->startAngleRad,
                 arc->sweepAngleRad,
                 arc->dashed);

  autoCoincidentNewGeometry(
      sketch_, oldLineCount, oldCircleCount, oldArcCount,
      8.0 / std::max(0.001, pixelsPerMm_));

  arcPoints_.clear();
'''
        canvas = replace_once(canvas, old, new, 'SketchCanvas arc creation persistence')

    except RuntimeError as exc:
        print(f'CHECK FAILED: {exc}', file=sys.stderr)
        print('No files were modified.', file=sys.stderr)
        return 4

    print('CHECK PASSED: current tree matches c856a7e5 + 0006b2a endpoint-layer expectations.')
    print('Would modify: src/sketch/Sketch.cpp, src/ui/SketchCanvas.cpp')

    if args.check:
        return 0

    save_preserve(SKETCH, sketch, sketch_nl, sketch_bom)
    save_preserve(CANVAS, canvas, canvas_nl, canvas_bom)
    print('APPLIED: Arc endpoint Coincident / PointOnLine / PointOnCircle persistence layer.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
