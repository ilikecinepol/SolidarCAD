# Macro foundation

The macro subsystem lives in `src/model/macro` and is a GUI-independent
orchestration layer over the normal document model. A macro is a declarative
sequence of semantic CAD actions; it never stores mouse events, screen
coordinates, widgets, OpenCascade shapes, callbacks, or raw pointers.

## Definition model

`MacroDefinition` is a reusable, versioned value object. It contains its own
`MacroId`, name, description, typed inputs, typed parameters, and an ordered
list of actions. IDs used by the definition (`MacroInputId`,
`MacroParameterId`, `MacroActionId`, and `MacroObjectId`) are symbolic and
local to the macro. They are not IDs copied from the document in which a macro
was authored.

`MacroInputDefinition` supports Sketch points, Sketch entities, Sketches,
faces, edges, and bodies. At apply time, `MacroInputBindings` maps each
symbolic input to a real model reference. `SketchPointReference` identifies a
semantic endpoint or center by `SketchId`, stable `sketch::GeometryId`, and
`SketchPointKind`. `resolveSketchPoint` resolves this reference to a local 2D
`sketch::Point` rather than world XYZ.

`MacroValue` currently supports `double`, `bool`, and `std::string`.
`MacroScalarExpression` is either a literal double or a parameter reference.
`MacroPointExpression` combines a Sketch-point input with local `(u, v)`
offsets. Consequently the same definition works on XY, XZ, YZ, and arbitrary
planar Sketch placements without knowing how the Sketch is oriented in 3D.

`MacroAction` is a `std::variant`. The initial action set is deliberately
small: `MacroCreateLineAction` and `MacroCreateCircleAction`. Adding future
actions extends this variant and the executor dispatch without changing the
container shape of `MacroDefinition`.

## Execution and instances

`MacroExecutionContext` owns the runtime mapping from symbolic inputs,
parameters, and macro-created object IDs to real document references.
`MacroExecutor` validates the format version and required bindings, resolves
parameter defaults and overrides, then executes actions in order. Every point
offset remains in the target Sketch's local coordinate system.

Application is transactional. The executor takes a deep `Document` snapshot
before the first action. An invalid action or exception restores that snapshot,
so a failed macro leaves no partial geometry.

On success, `MacroExecutionResult` contains a lightweight `MacroInstance` with
a fresh `MacroInstanceId`, the source `MacroId`, original input bindings, and
the real objects created by the run. The instance is not yet stored in
`Document` and does not provide parametric re-application; it reserves that
architectural option without making a macro a `ShapeFeature`.

All persistent definition fields are plain values, strings, vectors, enums,
and variants, and `formatVersion` starts at 1. JSON or another library format
can therefore be added later without changing the core ownership model.

## Existing semantic operations and future recording

There is currently no single document-wide command/event stream. Model
operations that a recorder can later intercept semantically include:

- `sketch::Sketch::addLine`, `addCircle`, rectangle helpers, and
  `addConstraint`;
- `Document::addSketch`;
- `Body::addFeature` for Extrude and Pocket creation;
- feature setters such as `ExtrudeFeature::setLengthMm`, `setOperation`, and
  `setReversed`, followed by the existing rebuild path.

The first recorder step should introduce a narrow model-operation gateway (or
observer around those mutation boundaries), translate successful operations
to macro-local object IDs and relative expressions, and ignore GUI events.
After that, add action variants for constraints and feature creation/editing,
JSON serialization with format migration, validation of complete definitions,
and a library/application UI. Face and edge bindings retain the project's
current index-based topological-reference limitation; persistent topology
naming is a separate concern.

## Intended flow

```text
Record -> semantic model operations -> MacroDefinition -> saved library

Select typed inputs -> MacroExecutor -> normal Sketch/Feature objects
                                      -> optional MacroInstance metadata
```
