---
name: qt-safety
description: Qt lifetime, signals/slots, event-loop, widget and QOpenGLWidget safety rules for SolidarCAD.
---

# Qt safety rules

For QObject/widget/controller changes:
- identify QObject parent ownership and non-QObject ownership separately;
- disconnect or invalidate callbacks when their target state is replaced;
- do not capture raw pointers in delayed/queued callbacks unless lifetime is guaranteed;
- account for re-entrant signal handling when model mutation emits notifications;
- avoid emitting state-change signals while related caches are inconsistent;
- clear hover/selection/tool state when documents, bodies, sketches or features switch;
- QOpenGLWidget/viewport interaction state must be valid across show/hide/reset/project changes;
- UI indexes and references must not assume model containers remain unchanged after callbacks.

When a signal can cause mutation of the source object, reason explicitly about re-entrancy and destruction order.
