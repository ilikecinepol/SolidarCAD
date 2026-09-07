# Part Design Stability Gate 2 — state audit

Date: 2026-09-07

## Findings

- `MainWindow` owns parallel state for the `Document`, `sketchHistory_`,
  `historySteps_`, `historyPosition_`, viewport presentation and five independent
  tool sessions. Apply and Cancel handlers previously repeated different subsets
  of `hide`, preview clearing, manipulator clearing and ribbon cleanup.
- `PartDesignToolController::activate` treated reopening the active tool as a
  no-op. If an Apply path completed the session but missed controller cleanup,
  the next click inherited stale selection and presentation state.
- `deactivate` cleared controller identity but did not invoke the registered
  presentation cleanup. Apply paths therefore depended on every handler
  remembering the same cleanup sequence.
- Menu, ribbon, docks and their signal connections are constructed in
  `MainWindow::buildUi`/`buildMenus`, not per tool opening. `ModelRibbon` creates
  each menu once in its constructor. No runtime double-connect path was found.
- `rebuildHistoryPanel` recreates history buttons and their connections, but the
  old child widgets are deleted with their layout items; these are not duplicate
  persistent connections. The authoritative feature sequence already comes from
  `Document` through `buildPartDesignHistory`.
- Typed contracts exist in `standardPartDesignToolDefinitions`, but type
  acceptance had no reusable predicate. Ad-hoc picking paths could consequently
  accept a Body where Plane/Axis/Feature was requested.
- Fillet, Chamfer, Shell, Draft and Revolve sessions correctly clear their own
  preview, inputs and errors in `cancel()`. The missing guarantee was the shared
  controller boundary between sessions and viewport/UI presentation.

## Changes made

- Reopening the same tool now cancels and clears its previous lifecycle before
  activation.
- Apply/deactivation now invokes the registered presentation cleanup centrally.
- Registrations remain keyed by tool kind, so repeated registration cannot
  duplicate a tool entry.
- Added an exact typed-selection acceptance predicate and regression checks for
  Mirror Plane and Circular Pattern Feature/Axis contracts.
- Added a twenty-cycle controller stress regression covering tool switches,
  reference re-selection, Cancel, stale state cleanup and duplicate guards.
- Development builds emit concise lifecycle/selection transitions to `std::clog`;
  Release builds compile the logging out.

## Remaining architectural risk

`sketchHistory_` still mirrors sketch data already present in `Document` for UI
compatibility. It should be removed incrementally only after project loading,
editing and viewport display paths consume `DocumentSketch` directly. Replacing
it wholesale in this stabilization gate would create unnecessary serialization
and editing risk.
