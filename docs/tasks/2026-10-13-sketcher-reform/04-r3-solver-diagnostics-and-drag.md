# R3 — nonlinear solver, diagnostics и drag

## Условие старта

R2 принят: equation kernel покрывает весь production constraint vocabulary и
доказан permutation/Jacobian tests.

## Цель

Решать каждую component как единую систему уравнений, диагностировать rank и
причину отказа, а интерактивное перетаскивание выражать временными hints вместо
прямой деформации committed geometry.

## Шаг 1. Зафиксировать backend decision

1. Реализовать `ISketchSolver`/эквивалентную границу между equation system и
   backend.
2. Провести изолированный spike минимум для двух вариантов:
   - PlaneGCS за адаптером;
   - собственный тонкий backend поверх поддерживаемой linear algebra/nonlinear
     optimization библиотеки.
3. Не писать собственные QR/SVD/DogLeg primitives без отдельного доказательства
   необходимости.
4. В decision record сравнить:
   - покрытие текущих constraints;
   - лицензирование и способ поставки;
   - Windows/Linux build;
   - conflict/rank diagnostics;
   - determinism;
   - latency и memory;
   - стоимость сопровождения.
5. Новую прямую зависимость добавить в `vcpkg.json`, `DEPENDENCIES.md`, notices
   и SBOM policy только после принятого решения.

## Шаг 2. Реализовать solve transaction

1. Вход: immutable equation system, previous valid solution и optional hints.
2. Выход: candidate parameter vector и typed `SolveReport`.
3. Production state обновляется только при успешной полной валидации report.
4. Non-converged/conflicting/malformed result оставляет model неизменной.
5. Исключение backend не выходит из model boundary.
6. Warm start использует последнее валидное решение component.

## Шаг 3. Устойчиво решать недоопределённые системы

1. У constraint order нет приоритета.
2. Среди допустимых решений выбирать ближайшее к previous valid state.
3. Drag hint влияет только на свободные DOF и не может нарушить hard
   constraints.
4. Global rigid-body gauge фиксируется детерминированно либо регуляризуется к
   предыдущему состоянию.
5. Не создавать искусственные persisted Lock constraints для выбора позы.

## Шаг 4. Ввести временные drag constraints

1. Mouse press фиксирует reference solution и target identity.
2. Mouse move создаёт transient positional hints, не входящие в persistence,
   Undo или permanent graph.
3. Hard constraints имеют приоритет перед hints.
4. Если cursor target недостижим, solver возвращает ближайшее допустимое
   положение либо оставляет последний валидный candidate; hard constraints не
   нарушаются.
5. Mouse release коммитит один итоговый delta.
6. Escape/cancel/document replacement уничтожает transient state до замены
   `Document`.

## Шаг 5. Разделить состояния диагностики

`SolveReport` обязан различать:

- `SolvedFullyConstrained`;
- `SolvedUnderConstrained` с числом DOF;
- `Redundant`;
- `PartiallyRedundant`;
- `Conflicting`;
- `Malformed`;
- `Unsupported`;
- `NonConverged`;
- `BackendFailure`.

Для report хранить component IDs, residual norm, iterations, rank, DOF,
constraint provenance и candidate conflicting/redundant IDs.

## Шаг 6. Rank и conflict localization

1. Использовать rank-revealing QR/SVD выбранного backend.
2. Redundancy определяется зависимостью equations, а не сравнением только
   `rankAfter <= rankBefore` на произвольной позе.
3. Conflict отличается от non-convergence: высокий residual после исчерпания
   итераций сам по себе ещё не доказывает несовместимость.
4. Для небольшой/средней component построить минимальный либо локально
   минимальный conflict set повторным исключением constraints.
5. Для большой component допускается bounded coarse set, но UI и report обязаны
   явно пометить его как неполный.
6. Определить geometry с remaining DOF для UI highlighting.

## Шаг 7. UI presentation

1. Показывать русские состояния без потери machine-readable error code.
2. Позволить выбрать conflicting/redundant constraints из сообщения.
3. Поддержать явное Driving/Reference/Disabled переключение.
4. Не перекрашивать весь sketch как conflicting, если report локализует одну
   component.
5. Expensive full diagnostics выполнять debounce/on-release; lightweight solve
   остаётся синхронным только в пределах подтверждённого latency budget.

## Шаг 8. Тесты и benchmarks

Обязательны:

1. все permutations одного определённого набора дают одинаковую geometry;
2. under-constrained permutations дают одинаковый nearest-previous solution;
3. conflict, redundancy и non-convergence различаются;
4. drag не создаёт permanent constraints и не меняет hard residuals;
5. impossible drag → release/cancel/Undo/Redo не повреждает модель;
6. disconnected components не решаются повторно;
7. save/load до и после solve даёт одинаковый canonical result;
8. differential fixtures против PlaneGCS/другого принятого oracle, если
   применимо;
9. seeded randomized small systems с проверкой invariants;
10. повторяемые benchmark workloads из R2.

## Параметры завершения R3

- [ ] production-capable backend выбран доказательным decision record;
- [ ] solver решает единую component equation system, а не type passes;
- [ ] order permutation не меняет status и canonical/nearest solution;
- [ ] failed solve никогда не меняет persisted semantic fingerprint;
- [ ] drag использует только transient hints и сохраняет hard constraints;
- [ ] report различает все перечисленные состояния;
- [ ] rank/DOF и constraint provenance доступны model/UI;
- [ ] conflict localization возвращает проверяемый набор IDs;
- [ ] ни один текущий constraint type не падает обратно на sequential math;
- [ ] interactive fixture до 100 geometry укладывается в p95 16,7 мс на
      зафиксированном reference host либо этап содержит принятый blocking report;
- [ ] p95 для 500-geometry connected workload не хуже R2 baseline более чем на
      20%, если correctness не требует документированного исключения;
- [ ] clean `ci`, dev verifier и ручной drag/save/load smoke зелёные;
- [ ] независимый reviewer не оставил HIGH findings;
- [ ] отчёт R3 содержит accuracy и performance baseline/after.
