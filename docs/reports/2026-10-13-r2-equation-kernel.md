# R2 — единый equation kernel: отчёт завершения

Дата проверки: 13 октября 2026 г.

## 1. Восстановленные инварианты и устранённые риски

- Для каждой constraint component строится отдельный immutable `EquationSystem` с каноническими parameters, residual rows, Jacobian и provenance до `ConstraintId`.
- Kernel находится в `solidar_model`, не зависит от Qt, `SketchCanvas` или OCCT и не вызывает mutators `Sketch`. Входом служит `const Sketch&`, результатом — собственный snapshot. Для R3 предусмотрена отдельная перегрузка assembly с candidate vector в порядке `variableIndex`; она создаёт новый layout, сохраняет fixed values и не изменяет исходный `Sketch`.
- Связность определяется только typed constraints и shared composite ownership. Близость координат не создаёт edge; origin не является общей topology-вершиной.
- Порядок parameters задаётся `(GeometryId, ParameterRole)`, порядок constraints/equations — `ConstraintId` и scalar row index. Порядок записей persistence не влияет на component membership, equations, sparsity, rank или provenance.
- Lock не реализован восстановлением старого snapshot: все параметры locked geometry помечаются fixed и получают нулевые Jacobian columns. Shared composite ownership соединяет component, но не добавляет скрытых equations.
- Неизвестная комбинация ссылок, вырожденная геометрия и non-finite input не маскируются успешным solve и не дают `NaN`/`Inf`: они представлены typed status.
- Production `SketchConstraintDiagnostics` переведён на тот же `EquationSystem`; повторной реализации constraint formulas в diagnostics больше нет. Solver v1 оставлен включённым согласно границам R2, а публичный Qt-free snapshot готов для Solver v2 в R3.

## 2. Выбранное решение и отклонённые альтернативы

Выбраны пять явных границ: `ConstraintGraph`, `ParameterLayout`, `ConstraintEquationRegistry`, `EquationSystem`, `EquationDiagnostics`. Реализация собрана в одном translation unit `EquationKernel.cpp`; небольшие публичные headers соответствующих границ включают единый контракт `EquationKernel.h`. Это сохраняет обозримый публичный API без циклических зависимостей и без дублирования внутренних numeric helpers.

Jacobian вычисляется forward-mode dual-number AD тем же evaluator, который выдаёт residual. Для каждой variable column evaluator повторно запускается только для constraint, реально ссылающегося на geometry этой колонки. `ParameterLayout::withVariableValues()` и `ConstraintEquationRegistry::assemble(..., candidate)` позволяют оценивать residual/Jacobian на пробном векторе без мутации model state; неверная длина candidate отвергается. Выбрана читаемая dense row-major snapshot-форма; sparse operation count публикуется отдельно. Sparse storage и nonlinear solve отложены до R3.

Отклонены:

- повторное использование формул Solver v1 — сохранило бы procedural mutation model и две расходящиеся математические реализации;
- finite difference в production — он оставлен только независимым тестовым oracle;
- coordinate-proximity graph edges — они нарушают явную topology и делают результат зависимым от позы;
- скрытые equations для composite geometry — они закрепляли бы текущую форму вместо typed model semantics;
- одно абсолютное epsilon для всех размеров — оно не масштабируется от микронных до метровых эскизов;
- silent fallback к Solver v1 для неподдержанной комбинации — вместо него возвращается `UnsupportedCombination`.

### Parameterization

| Geometry | Semantic parameters | Count |
|---|---|---:|
| Line | start.x, start.y, end.x, end.y | 4 |
| Circle | center.x, center.y, radius | 3 |
| Arc | center.x, center.y, radius, start-angle, sweep-angle | 5 |
| Cubic Bezier | P0.x/y, P1.x/y, P2.x/y, P3.x/y | 8 |

Provenance каждого параметра — stable `GeometryId` плюс `ParameterRole`. Fixed/locked параметры присутствуют в layout, но не получают `variableIndex`.

### Equation table

Обозначения: `d(a,b)` — Euclidean distance, `crossN`/`dotN` — cross/dot нормализованных направлений, `sdist` — signed point-line distance, `arcDist` — расстояние до конечной дуги с учётом span.

| Constraint | Rows | Residual / допустимые комбинации | Unit / degeneracy |
|---|---:|---|---|
| Horizontal | 1 | `end.y - start.y`, Line | length; zero-length line → degenerate |
| Vertical | 1 | `end.x - start.x`, Line | length; zero-length line → degenerate |
| Coincident | 2 | `p2.x-p1.x`, `p2.y-p1.y`, любые typed point refs | length |
| PointOnLine | 2 | `sdist(point,line)`, distance outside finite segment | length; zero line → degenerate |
| Distance | 1 | `d(p1,p2)-target` | length; target > 0; coincident points → degenerate |
| Length | 1 | `d(start,end)-target`, Line | length; zero line → degenerate |
| Radius | 1 | `radius-target`, Circle | length; radius/target > 0 |
| Diameter | 1 | `2*radius-target`, Circle | length; radius/target > 0 |
| Parallel | 1 | `crossN(dir1,dir2)`, Line-Line | angle/dimensionless; zero direction → degenerate |
| Perpendicular | 1 | `dotN(dir1,dir2)`, Line-Line | angle/dimensionless; zero direction → degenerate |
| Equal | 1 | `length1-length2` или `radius1-radius2` | length; Line-Line, Circle-Circle; прочее unsupported |
| Angle | 1 | `acos(dotN)-targetRadians`, Line-Line | radians; target strictly in `(0,180)` degrees; `dotN=±1` при ненулевой ошибке → degenerate |
| DistanceX | 1 | `abs(p2.x-p1.x)-target` | length; target > 0; zero separation → degenerate |
| DistanceY | 1 | `abs(p2.y-p1.y)-target` | length; target > 0; zero separation → degenerate |
| PointOnCircle | 1 | `d(center,point)-radius` | length; zero radius или point=center → degenerate |
| Tangent | 1–3 | Line-Radial: finite-segment distance (+ arc-span); Radial-Radial: external/internal distance (+ arc spans); Line-Bezier: signed endpoint-line distance + finite-segment outside row + tangent cross; Radial-Bezier: endpoint radius + tangent dot; Bezier-Bezier: endpoint x/y coincidence + tangent cross | mixed length/angle; zero directions/radii/ambiguous center → degenerate; other pairs unsupported |
| LineDistance | 1 | `abs(sdist(line2.start,line1))-target`, Line-Line | length; target > 0; zero base line или zero signed distance → degenerate |
| Lock | 0 | fixed parameter descriptors, без restoration rows | dimensionless policy |
| PointOnArc | 1 | `arcDist(point,arc)` | length; zero radius/invalid span или point=center → degenerate |
| Midpoint | 2 | `point - (start+end)/2`, Line + point | length |
| PointOnXAxis | 1 | `point.y` | length; origin — fixed datum, не graph node |
| PointOnYAxis | 1 | `point.x` | length; origin — fixed datum, не graph node |

### Нормализация и tolerances

- `sketchScale = max(1e-9, 2 * max_i(distance(anchor_i, centroid) + intrinsicRadius_i))`: line endpoints и Bezier control points дают point anchors, circles/arcs — center anchors с intrinsic radius. Эта O(n) оценка инвариантна к rigid transform и линейно масштабируется.
- Length row: `normalizedResidual = residual / sketchScale`; acceptance tolerance хранится как `max(1e-9 mm, sketchScale * 1e-8)`.
- Angle/dimensionless row: scale `1`, tolerance `1e-8`; angle target переводится из градусов model contract в radians внутри registry.
- Geometry degeneracy threshold для нулевой длины/радиуса: `1e-14` в domain units; squared direction threshold: `1e-28`.
- Rank tolerance: `epsilon(double) * max(rows, cols) * ||J||∞ * 64`, где `J` уже нормализован и содержит только variable columns.
- Последняя finite guard заменяет non-finite residual/derivative на конечную sentinel row и status `NonFiniteInput`; invalid rows имеют нулевой Jacobian.
- Statuses: `Valid`, `MalformedReference`, `UnsupportedCombination`, `DegenerateGeometry`, `NonFiniteInput`; итоговый status component — наиболее строгий из row statuses.

## 3. Изменённые production/test/persistence файлы

Production:

- `src/sketch/EquationKernel.h`, `src/sketch/EquationKernel.cpp`;
- `src/sketch/ConstraintGraph.h`, `ParameterLayout.h`, `ConstraintEquationRegistry.h`, `EquationSystem.h`, `EquationDiagnostics.h` — публичные module-boundary headers;
- `src/sketch/SketchConstraintDiagnostics.cpp/.h` — adapter на kernel;
- `src/CMakeLists.txt` — включение kernel в `solidar_model`.

Tests/benchmarks:

- `tests/sketch_equation_kernel_tests.cpp`;
- `tests/model_public_headers_compile_tests.cpp`;
- `tests/CMakeLists.txt`;
- `benchmarks/equation_kernel_benchmark.cpp`;
- `benchmarks/CMakeLists.txt`.

Persistence-файлы в R2 не менялись. Все существующие dirty R0/R1 и пользовательские файлы сохранены.

## 4. Новые deterministic regressions и property tests

- Registry coverage проверяет ровно все 22 production `ConstraintType` без дубликатов.
- Complete fixture содержит exact-zero rows всех production types; каждая row, Jacobian entry и semantic hash finite, provenance сохранён. Отдельный fixture проверяет заранее вычисленный ненулевой raw residual каждого из 22 типов (Lock — fixed/no-row contract).
- Forward AD каждого equation type сравнивается с независимым central finite-difference oracle; отдельно покрыты все 9 поддержанных unordered Tangent-комбинаций. Exact line–Bezier tangent проверяет ненулевой positional Jacobian и rank, чтобы симметричный finite-difference oracle не мог скрыть нулевую производную.
- Все 6 permutations трёх constraints сравниваются по semantic hash, residual/equation order, sparsity pattern, rank и provenance; большой mixed fixture отдельно проверяется в прямом/обратном persisted order.
- Coordinate-near lines (`5e-10` apart) остаются разными components; shared composite ownership объединяет component без скрытых rows.
- Malformed references, unsupported combinations, zero-length/radius и non-finite inputs дают typed status без crash/NaN/Inf. Singular-but-common candidates (zero Distance/DistanceX/DistanceY, parallel input для ненулевого Angle, point=center для Circle/Arc, center-on-line Tangent и zero LineDistance) дают `DegenerateGeometry`, а не ложный `Valid` с нулевым gradient.
- Candidate-vector regression меняет trial value, проверяет новый residual/Jacobian, неизменность `Sketch`/base hash и отказ на неверной длине vector.
- Seeded rigid translation/rotation/scale property (`seed=5904354`, печатается всегда) проверяет согласованную инвариантность Distance, Length, Parallel, Perpendicular, Angle, Equal, LineDistance и circle Equal/Tangent.
- Deterministic CI gate фиксирует operation counts `(2 components, 8 parameters, 4 equations, 8 non-zero Jacobian entries)` и golden semantic hashes `15507668604071813813`, `12033787661441475554`.

## 5. Targeted test results

До R2 снят baseline R0/R1: 7/7 targeted tests зелёные.

После финальных production-изменений Debug targeted suite: 8/8, 130.49 s:

- `project_file_tests`;
- `project_switch_ui_tests`;
- `sketch_regression_tests`;
- `sketch_command_controller_tests`;
- `sketch_rectangle_dimension_ui_tests`;
- `sketch_constraint_integrity_tests`;
- `sketch_schema_tests`;
- `sketch_equation_kernel_tests`.

После финальных singular-state и line–Bezier Jacobian исправлений `sketch_equation_kernel_tests` отдельно пройден в Debug (1/1, 0.14 s) и Release. Полный чистый Release `ctest` на окончательном коде: 81/81, 26.06 s.

## 6. Полный clean CI, dev verifier и ручной smoke

- `cmake --preset ci` — успешно.
- После изменения headers: `cmake --build --preset ci --clean-first` — успешно, 456 build steps.
- `ctest --preset ci --output-on-failure` на окончательном коде — 81/81, 26.06 s.
- `.opencode/scripts/verify-and-run.ps1` на окончательном коде — configure/build/81 tests/launch-smoke успешно: 81/81, 63.93 s; `SOLIDAR_GUI_SMOKE_OK PID=44988 SECONDS=8`; `SOLIDAR_VERIFY_SUCCESS`. Запущенный verifier процесс после проверки закрыт.
- Ручной smoke в dev build: создан временный `.solidar`, открыт Sketcher на XY, построен горизонтальный отрезок, получен точный UI status `Эскиз недоопределён · DOF: 3`, sketch завершён со status `Эскиз завершён — модель перестроена`, проект сохранён без dirty marker. Приложение закрыто; созданный временный файл удалён.

## 7. Benchmark baseline/after

Оба запуска выполнены Release `ci` на одной машине. Артефакты находятся в ignored build tree:

- baseline Solver v1: `build/ci/r2-equation-kernel-baseline.csv`;
- R2 equation kernel: `build/ci/r2-equation-kernel-after.csv`.

Baseline снят до появления kernel и измеряет старые solver/diagnostics workloads; поэтому его timing нельзя выдавать за apples-to-apples ускорение assembly. Он фиксирует исходную стоимость системы. Новый benchmark изолирует именно graph + equation assembly и покрывает обязательные one/many-component размеры.

| Workload | Geometry | Components | Assembly ms | Params | Eq | J nnz | Allocations | Peak bytes | Owned bytes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| independent H/V/Length | 100 | 100 | 0.3093 | 400 | 200 | 400 | 4,224 | 42,513 | 42,400 |
| independent H/V/Length | 500 | 500 | 1.8284 | 2,000 | 1,000 | 2,000 | 21,028 | 209,896 | 212,000 |
| independent H/V/Length | 1000 | 1000 | 4.6516 | 4,000 | 2,000 | 4,000 | 42,029 | 391,082 | 424,000 |
| Coincident/Equal chain | 100 | 1 | 1.4979 | 400 | 297 | 792 | 4,083 | 2,371,883 | 1,430,496 |
| Coincident/Equal chain | 500 | 1 | 18.9582 | 2,000 | 1,497 | 3,992 | 20,111 | 58,707,846 | 35,266,696 |
| Coincident/Equal chain | 1000 | 1 | 68.8037 | 4,000 | 2,997 | 7,992 | 40,123 | 175,790,518 | 105,621,648 |
| rectangle + dimensions | 100 | 25 | 0.2067 | 400 | 150 | 300 | 2,620 | 21,247 | 58,600 |
| line/circle/arc tangency | 99 | 33 | 0.2303 | 396 | 99 | 528 | 2,562 | 19,403 | 40,392 |
| Bezier endpoint/tangent | 100 | 50 | 0.3067 | 600 | 150 | 300 | 4,072 | 22,556 | 60,400 |

Semantic hashes записаны в CSV для каждой строки; для финального Bezier workload hash — `12271130241155142716`. Timing остаётся report-only; CI фиксирует exact operation counts и hashes. Наблюдаемая цена dense AD для connected chain (68.80 ms и 105.6 MiB owned при 1000 geometry) зафиксирована как вход для sparse work в R3, но не ослабляет корректность R2.

## 8. Независимый review

Независимый reviewer проверил production, tests и gates. Первичные HIGH были устранены до приёмки:

- добавлен candidate-vector API без мутации `Sketch`;
- finite Tangent domains доведены до segment/arc semantics;
- singular states перестали возвращать `Valid` с нулевым Jacobian;
- line–Bezier positional residual заменён на дифференцируемый `signedLineDistance + segmentOutside`, добавлен exact-tangent rank regression.

Финальный verdict reviewer: **PASS; HIGH/MEDIUM по production и tests нет**. Единственное MEDIUM относилось к устаревшему тексту этого отчёта (формула line–Bezier, benchmark, candidate/singular coverage); оно закрыто настоящей синхронизацией отчёта с финальным кодом и CSV. Reviewer также независимо подтвердил `git diff --check`, targeted Release test, полный Release 81/81 и benchmark.

## 9. Известные ограничения и разрешённый следующий этап

- `EquationSystem` хранит dense Jacobian для простоты аудита; large connected components имеют заметный memory footprint. Это не correctness gate R2 и должно быть адресовано sparse representation/solver strategy в R3.
- Branch selection для internal/external tangency и ближайшего Bezier endpoint канонична для данного candidate state, но её нелинейная стратегия сходимости относится к R3.
- Solver v1 намеренно не переключён и не удалён. Он продолжает работать под R0 transaction containment; R2 kernel уже является единым источником equations для diagnostics и готовым input для будущего solver.
- UX conflict localization, drag constraints и nonlinear component solving не входят в R2.

Следующий разрешённый этап: **R3 — solver, diagnostics и drag**. Review закрыт без HIGH, все R2 gates зелёные; R3 разрешён, но в этой поставке не начат.

## 10. Расхождения с инструкцией

- Физически пять публичных module boundaries представлены отдельными headers, но их declarations собраны в одном `EquationKernel.h`, а implementation — в одном `EquationKernel.cpp`. Ответственности и API разделены; математическая реализация намеренно не размножена по translation units.
- Baseline до R2 измеряет Solver v1 и старую diagnostics, потому что equation kernel ещё не существовал. Поэтому benchmark report содержит обе серии и явно не заявляет прямое timing-сравнение.
- Отклонений, ослабляющих tolerance, пропускающих production constraint type, разрешающих partial success или меняющих persistence, нет.
