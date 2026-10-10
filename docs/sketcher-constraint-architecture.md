# Архитектура ограничений Sketcher

## Solver v1

Текущий `BasicSketchSolver` — последовательный mutator-based solver. Он
обрабатывает типы ограничений проходами и не решает единую систему уравнений.
Поэтому порядок ограничений в persisted vector пока может менять результат.
Это известное ограничение Solver v1, а не контракт CAD.

Неявное соединение геометрии по совпадающим координатам сохранено только для
совместимости. Новые функции не должны считать его публичной моделью topology;
snapping должен постепенно переходить к явным `Coincident` constraints.

## Контракт P0

Интерактивное добавление проходит через `Sketch::tryApplyConstraint()` и
возвращает `ConstraintApplyResult` со статусом `Accepted`, `Redundant`,
`Conflicting`, `Unsupported`, `InvalidReference` или `SolverFailed`.

Preflight выполняется во вложенном delta-journal для затронутой компоненты.
Constraint и изменения геометрии обычно коммитятся только для `Accepted`.
Точный дубликат возвращает `Redundant` без изменения модели. Для нового
driving dimension допускается явный opt-in `commitRedundant`: это сохраняет
rank-dependent, но валидное уравнение и возвращает `Redundant` с назначенным
`ConstraintId`. Любой непринятый результат атомарно восстанавливает прежний
semantic fingerprint. Старые ограничения не удаляются автоматически: явная
замена остаётся будущей пользовательской операцией.

Diagnostics включает линии, окружности и дуги, считает две скалярные строки для
`Midpoint` и публикует global DOF вместе со списком component-level DOF. UI
показывает fully-constrained только как статус всего эскиза; достоверный
per-geometry nullspace state пока не заявляется.

Все hard constraints равноправны. Тип ограничения и порядок кликов не задают
приоритет.

## Цель Solver v2

```text
Sketch
→ ConstraintGraph
→ ParameterLayout
→ ConstraintEquationRegistry
→ ComponentSolver
→ SolveReport
→ SketchPresentationState
```

Solver v2 должен убрать зависимость от порядка, заменить раздельные type-pass
эвристики единой системой, сделать explicit connectivity основой графа и уметь
выдавать доказуемое состояние отдельных geometry. P0 пока не строит minimal
conflict set: `relatedConstraintIds` содержит coarse набор из затронутой
компоненты или очевидную конфликтующую связь.
