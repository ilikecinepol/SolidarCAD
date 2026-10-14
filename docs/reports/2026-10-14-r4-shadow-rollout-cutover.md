# R4 — shadow rollout, migration и отключение Solver v1

Дата проверки: 14 октября 2026 г.
Статус: **Windows PASS; внешний Linux gate ожидает запуска CI**

## 1. Восстановленные инварианты и устранённые пользовательские риски

- Equation-based `SketchSolver` является единственным production solver.
  Автоматического fallback и повторного solve через старую реализацию нет.
- Неуспешный solve публикует только typed diagnostic и не меняет committed
  geometry, constraints, Undo/Redo, selection, cache или modified state.
- Последовательные V1 mutators удалены из публичного API и реализации.
  Compliance gate запрещает их повторное появление.
- Связность строится только из explicit constraints и composite ownership.
  Совпадающие или почти совпадающие координаты не соединяют components.
- `restoreConstraints()` валидирует disposable candidate, но сохраняет
  serialized pose. Первое открытие валидного v1/v2 проекта больше не является
  скрытой геометрической миграцией. Candidate с non-finite/degenerate geometry
  отклоняется атомарно.
- Tangency/Bezier performance строки теперь измеряют настоящий
  `analyzeConstraintSystem()`, а не только graph/equation assembly.

## 2. Выбранное решение и отклонённые альтернативы

Cutover выполнен одной границей `SketchSolver`: UI и model mutations не имеют
ветвлений по solver version. После pre-cutover comparison удалены
`BasicSketchSolver`, historical benchmark branch, sequential type passes и
процедурные setters горизонтальности, параллельности, расстояний, совпадения и
касательности.

Отклонены:

- автоматический fallback на V1 — он скрывал бы дефекты V2 и мог работать над
  уже изменённым candidate;
- solve-on-load — даже успешная коррекция молча меняет persisted geometry;
- coordinate-proximity topology — результат зависел бы от tolerance и позы;
- assembly-only строки под именем diagnostics — они не измеряют rank/DOF и
  conflict analysis;
- ослабление solver tolerance ради benchmark — fixtures вместо этого удержаны
  в локальном диапазоне координат.

## 3. Изменённые production, test и persistence файлы

Production и persistence:

- `src/sketch/Sketch.h`, `src/sketch/Sketch.cpp`;
- `src/sketch/SketchSolver.h`, `src/sketch/SketchSolver.cpp`;
- `src/sketch/SketchConstraintDiagnostics.h`,
  `src/sketch/SketchConstraintDiagnostics.cpp`;
- `src/project/ProjectFile.cpp`;
- `src/CMakeLists.txt`.

Tests, corpus и benchmark:

- `tests/sketch_solver_cutover_compliance_tests.cmake`,
  `tests/fixtures/sketch-solver-corpus/manifest.json`;
- `tests/project_file_tests.cpp`, `tests/sketch_constraint_integrity_tests.cpp`,
  `tests/sketch_renderer_tests.cpp`, `tests/sketch_lock_constraint_tests.cpp`,
  `tests/sketch_regression_tests.cpp`, `tests/part_design_regression_tests.cpp`;
- `benchmarks/stage6_sketch_solver_benchmark.cpp`,
  `benchmarks/run_stage6_sketch_solver_benchmark.ps1`,
  `benchmarks/README.md`, `benchmarks/CMakeLists.txt`;
- `docs/sketcher-constraint-architecture.md` и данные этого отчёта.

Схема project format не повышалась: v1/v2 readers и format-v3 typed identity
сохранены. Изменена только семантика materialization: load не коммитит solve.

## 4. Новые deterministic regressions и property tests

- v1 и v2 fixtures с линией `(0,0) -> (10,2)` и `Horizontal` доказывают, что
  load сохраняет все четыре serialized coordinates и stable constraint ID;
- malformed fixture, чей solve может только схлопнуть линию, остаётся
  fail-closed через disposable candidate validation;
- equal-coordinate line/circle/chain fixtures доказывают отсутствие неявной
  topology и локальность edit;
- `rectangleSlotPolygonChainsStayExplicit` покрывает composite rectangle,
  slot-like и polygon chains как три независимых explicit components;
- corpus manifest парсится как JSON, каждая запись связана с существующим test
  source и воспроизводимым fixture token;
- compliance scan запрещает Solver-v1 symbols и все удалённые procedural
  mutators;
- benchmark проверяет стабильный полный semantic fingerprint между samples,
  status/residual, счетчики локальности и реальный full diagnostics call.

## 5. Targeted test results

После финальных исправлений:

- `project_file_tests`, `sketch_constraint_integrity_tests`,
  `sketch_renderer_tests` — **3/3 PASS**;
- полный sensitive набор (`sketch_renderer`, rectangle/dimension UI,
  constraint integrity, schema, equation kernel, nonlinear solver, cutover
  compliance) — **7/7 PASS**, каждый тест выполнен 20 раз без отказа;
- `git diff --check` — PASS;
- cutover compliance — PASS, V1 production symbols не найдены.

## 6. Полный clean CI, dev verifier и ручной smoke

- `cmake --preset ci` — PASS;
- `cmake --build --preset ci --clean-first` — PASS, 461 build step;
- финальная incremental сборка после `.cpp`/test правок — PASS;
- `ctest --preset ci --output-on-failure` — **83/83 PASS**, 27,33 с;
- dev verifier — configure/build PASS, **83/83 PASS**, GUI launch smoke 8 с,
  `SOLIDAR_VERIFY_SUCCESS`;
- ручной smoke на `build/dev/r3-manual-smoke.solidar`: mixed/redundant
  constraints, свободный и связанный drag, rejected conflict с неизменной
  geometry, Driving/Reference, delete и два Undo/Redo, save/close/reopen/edit,
  Extrude/Pocket и изменение source sketch; light/dark diagnostic states
  читаемы. Исходные пользовательские файлы не изменялись.

Windows Release tests нужно запускать без принудительного
`QT_QPA_PLATFORM=offscreen`: software OpenGL context тестируется штатным Qt
Windows backend. С принудительным offscreen `viewport_renderer_tests` ожидаемо
не может создать context; это не конфигурация preset `ci`.

## 7. Benchmark baseline/after на одинаковых fixtures

Reference host: Windows, MSVC Release `ci`, 21 sample. Pre-cutover V1/V2
comparison сохранён в
`docs/reports/data/2026-10-14-r4-shadow-comparison.csv`; status, residual и
semantic hashes совпали во всех 12 сравнимых строках.

Финальный production report:
`docs/reports/data/2026-10-14-r4-production-benchmark.csv`.

| Geometry | Workload | p95, ms | p99, ms | Result |
|---:|---|---:|---:|---|
| 100 | localized drag | 0,0775 | 0,1248 | PASS |
| 100 | connected drag | 4,6725 | 4,9378 | PASS |
| 100 | unchanged solve | 0,0047 | 0,0049 | cache hit, 0 iterations |
| 100 | full diagnostics | 0,9236 | 0,9245 | PASS |
| 100 | all-curve tangency diagnostics | 1,2372 | 1,2588 | PASS |
| 100 | Bezier-heavy diagnostics | 1,6069 | 1,6453 | PASS |
| 1000 | connected drag | 372,8771 | 373,2545 | published, no hard gate |
| 1000 | full diagnostics | 12,4985 | 12,5224 | published |

Границы 16,7 мс p95 и 50 мс synchronous phase выполнены для всех
100-geometry reference fixtures. Localized edit посещает ровно одну component,
geometry и constraint; unchanged solve не собирает equation system повторно.
Известный bottleneck 500/1000 connected drag — dense LM/SVD одной большой
component; измерение опубликовано, timing gate для этих размеров не заявлен.

## 8. Результат независимого review

Первый независимый review завершился FAIL и нашёл четыре HIGH: оставшиеся
public V1 mutators, solve-on-load, assembly-only tangency/Bezier benchmark и
renderer fixture с отвергнутой несвязанной dimension. Все замечания исправлены
отдельными regressions и compliance gates.

Повторный независимый verdict: **PASS; HIGH/MEDIUM замечаний нет**. Reviewer
подтвердил физическое удаление V1 API, disposable persistence validation,
точное сохранение v1/v2 pose, реальные all-curve diagnostics workloads,
полный semantic fingerprint, benchmark thresholds, typed renderer annotation,
JSON corpus gate, **7/7 targeted**, **83/83 full CTest**, 18/18 benchmark rows
и чистый `git diff --check`.

## 9. Известные ограничения и разрешённый следующий этап

- Dense solve большой connected component остаётся главным performance
  bottleneck.
- Pre-cutover shadow CSV не содержал отдельные columns satisfied-ID/rank/DOF;
  эти contracts покрыты versioned equation/diagnostics regressions, но не
  ретроспективным V1 timing CSV. Повторно запустить удалённый V1 нельзя без его
  восстановления, что нарушило бы cutover.
- Локальная Windows машина не предоставляет WSL (`Wsl/E_ACCESSDENIED`). Workflow
  `.github/workflows/build.yml` использует один `ci` preset на Windows и
  Ubuntu 24.04, но результат текущего uncommitted worktree на Linux требует
  внешнего CI запуска после разрешённого push.

До зелёного Linux CI дальнейший этап не разрешён. После него solver reform
R0–R4 завершена; новые Sketcher tools и
декомпозиция `SketchCanvas` остаются отдельной работой.

## 10. Расхождения с инструкцией

- Анонимизированных пользовательских проектов в corpus нет: разрешение на их
  копирование не предоставлялось. Использованы только синтетические и
  historical regression fixtures.
- Shadow comparison до удаления V1 зафиксировал status/residual/hash/latency и
  work counters, но не отдельные satisfied-ID/rank/DOF columns. Эти свойства
  доказаны deterministic tests текущего mathematical contract.
- Linux gate нельзя выполнить на данном host; он честно оставлен внешним
  blocking gate, а не отмечен как PASS.
- Иных ослаблений tolerance, пропусков unsupported constraints или частично
  успешных solve нет.
