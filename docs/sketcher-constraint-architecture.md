# Архитектура ограничений Sketcher

## Production Solver v2

Единственный production-путь проходит через `SketchSolver` и
`productionSketchSolver()`. Последовательный mutator-based Solver v1 и его
автоматический fallback удалены. Backend получает immutable snapshot и
возвращает candidate parameter vector; публикация выполняется одной
транзакцией только после residual/schema/geometry audit всей затронутой
компоненты.

Связность задают только явные constraints и composite ownership. Совпадение
координат не создаёт topology и не распространяет drag; snapping обязан
материализовать `Coincident` constraint.

## Транзакционный контракт

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

Diagnostics включает линии, окружности, дуги и Bezier, считает две скалярные строки для
`Midpoint` и публикует global DOF вместе со списком component-level DOF. UI
показывает fully-constrained только как статус всего эскиза; достоверный
per-geometry nullspace state пока не заявляется.

Все hard constraints равноправны. Тип ограничения и порядок кликов не задают
приоритет.

## Поток данных

```text
Sketch
→ ConstraintGraph
→ ParameterLayout
→ ConstraintEquationRegistry
→ ComponentSolver
→ SolveReport
→ SketchPresentationState
```

Residuals, forward-AD Jacobian, rank/DOF и conflict diagnostics собираются из
одного `ConstraintEquationRegistry`. Constraint vector сначала канонизируется
по стабильным ID, поэтому его persisted-порядок не выбирает ветвь решения.
Для недоопределённой системы ближайшая поза задаётся последним валидным
candidate и transient drag hints.
