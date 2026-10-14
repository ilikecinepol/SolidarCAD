# R2 — единый equation kernel

## Условие старта

R1 принят, persistence и typed model contract стабильны. Production по-прежнему
может использовать атомарно ограждённый Solver v1.

## Цель

Построить независимый от UI и `Sketch` mutators слой, который для каждой
constraint component детерминированно создаёт parameter vector, residual
equations и Jacobian. Один и тот же registry обязан использоваться будущим
solver и diagnostics.

## Не входит в R2

- включение Solver v2 для пользовательских изменений;
- удаление Solver v1;
- окончательная UX conflict localization;
- оптимизация до потери читаемости или корректности.

## Шаг 1. Создать модульные границы

В `solidar_model` выделить компоненты с одной ответственностью:

1. `ConstraintGraph` — явная связность geometry/parameters/constraints;
2. `ParameterLayout` — стабильное отображение domain parameters в vector;
3. `ConstraintEquationRegistry` — residuals и Jacobian rows;
4. `EquationSystem` — immutable snapshot конкретной component;
5. `EquationDiagnostics` — residual/rank input без UI типов.

`SketchCanvas`, Qt и OCCT не должны быть зависимостями equation kernel.

## Шаг 2. Определить параметры геометрии

1. Зафиксировать минимальный parameter set для Line, Circle, Arc и Bezier.
2. Для каждого параметра хранить stable provenance: geometry ID и semantic role.
3. Datum и locked/external geometry представлены fixed parameters, а не
   последующим восстановлением snapshot.
4. Composite elements не должны получать скрытые уравнения из текущей формы.
   Их инварианты задаются явными internal constraints либо собственной
   документированной parameterization.
5. Degenerate geometry обрабатывается как typed malformed/unsupported state, а
   не как деление на почти ноль.

## Шаг 3. Построить явный граф

1. Ребро графа возникает только из typed constraint, shared composite ownership
   или явного external/datum dependency.
2. Близкие координаты без `Coincident` не соединяют components.
3. Origin является fixed datum и не склеивает независимые components.
4. Component membership и parameter ordering детерминированы stable IDs/roles,
   но математический результат не зависит от persisted constraint order.
5. Добавить linear-time или близкую к ней обработку chains равенств через
   union-find/reduction, если это не меняет semantics.

## Шаг 4. Реализовать equation registry

Для каждого поддерживаемого constraint type определить:

1. число scalar equations;
2. нормализованный residual;
3. Jacobian rows — аналитические либо через выбранный и документированный
   automatic differentiation механизм;
4. допустимые geometry/reference combinations;
5. scale/tolerance и единицы;
6. поведение на degeneracy;
7. provenance каждой строки до `ConstraintId`.

Покрыть все constraint types, которые доступны production UI на момент старта
R2. Нельзя считать этап завершённым при silent fallback к Solver v1 для одного
из типов внутри одной component.

## Шаг 5. Нормализация и численная устойчивость

1. Линейные residuals нормализовать относительно документированного sketch
   scale, угловые — в радианах с отдельной шкалой.
2. Не использовать одно абсолютное epsilon для координат от микронов до метров.
3. Jacobian rank tolerance связывать с нормой матрицы и machine precision.
4. Все вычисления проверять на finite values.
5. Canonical equation assembly не должен зависеть от iteration order
   `unordered_map`.

## Шаг 6. Независимые тестовые оракулы

1. Для каждого equation type — fixtures с exact zero и известным residual.
2. Аналитический/AD Jacobian сравнить с central finite-difference oracle на
   невырожденных точках.
3. Проверить rigid translation/rotation invariance для применимых constraints.
4. Проверить scale invariance в согласованном диапазоне размеров.
5. Для всех permutations constraints сравнить canonical residual vector,
   sparsity pattern и rank.
6. Проверить components до/после близкого, но не совпадающего положения точек.
7. Проверить singular/degenerate cases без crash/NaN.
8. Добавить seeded property tests; seed печатается при отказе.

## Шаг 7. Benchmark kernel

Добавить report-only benchmark минимум для:

- независимых H/V/Length components;
- длинной Coincident/Equal chain;
- rectangle с dimensions;
- mixed line/circle/arc tangency;
- Bezier endpoint/tangent constraints;
- 100/500/1000 geometry в одной и многих components.

Измерять assembly time, equation/parameter count, non-zero Jacobian entries,
allocations и peak owned bytes. Timing не становится обычным CI gate; CI
проверяет детерминированные operation counts и semantic hashes.

## Параметры завершения R2

- [ ] equation kernel Qt-free и не вызывает mutators `Sketch`;
- [ ] все production constraint types представлены единым registry;
- [ ] solver и diagnostics смогут потреблять один `EquationSystem` без
      повторной реализации формул;
- [ ] graph не содержит coordinate-proximity edges;
- [ ] parameter/equation ordering детерминирован;
- [ ] permutation tests дают одинаковые equations, rank и provenance;
- [ ] Jacobian каждого constraint проходит независимый numerical oracle;
- [ ] malformed/degenerate inputs не создают NaN/Inf и имеют typed status;
- [ ] benchmark содержит все обязательные workloads и baseline report;
- [ ] R0/R1 regressions, clean `ci` и dev verifier зелёные;
- [ ] независимый reviewer не оставил HIGH findings;
- [ ] отчёт R2 содержит equation table и численные tolerances.
