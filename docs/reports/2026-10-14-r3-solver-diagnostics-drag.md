# R3 — nonlinear solver, diagnostics и drag: отчёт о завершении

Дата проверки: 14 октября 2026 г.
Статус: **PASS; R3 готов к приёмке**

## 1. Восстановленные инварианты и устранённые пользовательские риски

- Solver получает immutable `EquationSystem`, предыдущее валидное решение и
  transient hints; результатом является отдельный candidate vector и
  machine-readable `SolveReport`.
- Authoritative persisted state остаётся в `Sketch`. Публикация candidate
  выполняется только после успешной полной проверки residuals. `Malformed`,
  `Unsupported`, `NonConverged`, `Conflicting` и `BackendFailure` не публикуют
  частичную геометрию и не меняют semantic fingerprint.
- `BasicSketchSolver::solveStableComponent()` является транзакционной границей:
  solver не вызывает mutators `Sketch`, а observers, render cache и Undo/Redo
  видят только принятый итог.
- Constraint order больше не задаёт приоритет. Стабильные IDs определяют layout
  и provenance, а для недоопределённых систем поза регуляризуется к previous
  valid candidate и drag hint.
- Drag не создаёт persisted `Lock`: press фиксирует reference solution, move
  задаёт временный hint, release завершает ровно один delta journal. Escape,
  смена инструмента и замена документа уничтожают transient state и возвращают
  press-time semantic state.
- Hard constraints всегда имеют приоритет над hint. Недостижимый target не
  нарушает normalized residual и не оставляет промежуточный candidate.
- Solver и diagnostics используют один R2 equation registry/Jacobian. Явная
  graph connectivity сохраняется; совпадение координат не становится topology.
- Отчёт различает `SolvedFullyConstrained`, `SolvedUnderConstrained`,
  `Redundant`, `PartiallyRedundant`, `Conflicting`, `Malformed`, `Unsupported`,
  `NonConverged` и `BackendFailure`; содержит component IDs, residual,
  iterations, rank/DOF, provenance и диагностические stable IDs.

## 2. Выбранное решение и отклонённые альтернативы

Принят `OcctSketchSolver` за границей `ISketchSolver`. Нелинейная коррекция
использует поставляемый проектом OCCT `math_SVD`, rank/nullspace —
`MathLin::SVD`. Residuals и forward-AD Jacobian предоставляет R2
`EquationKernel`; отдельной геометрической математики и собственных QR/SVD/
DogLeg primitives нет.

Изолированный PlaneGCS spike прошёл 22/22 теста за 0,30 с. Первая Windows-сборка
заняла 4,7 мин и добавила 36 транзитивных пакетов. PlaneGCS отклонён для
production: адаптер всё равно должен был бы обеспечивать stable IDs,
provenance, deterministic storage и bounded conflict localization, а LGPL,
Boost/Eigen, notices и SBOM увеличивали бы поставку. OCCT уже входит в Windows и
Linux dependency closure, поэтому `vcpkg.json`, notices и SBOM policy в R3 не
менялись.

Полное обоснование: `docs/decisions/2026-10-13-r3-solver-backend.md`.

## 3. Изменённые production, test и persistence файлы

Production R3:

- `src/sketch/NonlinearSketchSolver.h`,
  `src/sketch/NonlinearSketchSolver.cpp` — solver boundary, LM/SVD, typed report,
  rank/DOF, hints и conflict localization;
- `src/sketch/SketchSolver.h`, `src/sketch/SketchSolver.cpp` — component
  transaction, warm start, publish/rollback, cache и benchmark drag path;
- `src/sketch/EquationKernel.h`, `src/sketch/EquationKernel.cpp`,
  `src/sketch/ParameterLayout.h` — candidate evaluation и корректное исключение
  disabled/probed constraints, включая освобождение параметров `Lock`;
- `src/sketch/Sketch.h`, `src/sketch/Sketch.cpp`,
  `src/sketch/SketchConstraintDiagnostics.h`,
  `src/sketch/SketchConstraintDiagnostics.cpp` — typed statuses, diagnostics и
  stable provenance;
- `src/ui/SketchCanvas.h`, `src/ui/SketchCanvas.cpp`,
  `src/ui/SketchCommandController.h`, `src/ui/SketchCommandController.cpp`,
  `src/ui/SketchHitSceneAdapter.cpp`,
  `src/ui/SketchInteractionController.h`,
  `src/ui/SketchInteractionController.cpp`, `src/ui/SketchRenderer.cpp`,
  `src/ui/MainWindow.cpp` — transient drag, cancellation, локальное выделение,
  русские сообщения и Driving/Reference/Disabled actions;
- `src/CMakeLists.txt` — включение nonlinear solver в production target.

Tests/benchmarks/docs R3:

- `tests/sketch_nonlinear_solver_tests.cpp`;
- `tests/sketch_render_cache_ui_tests.cpp`,
  `tests/sketch_interaction_controller_tests.cpp`,
  `tests/sketch_command_controller_tests.cpp`,
  `tests/sketch_constraint_integrity_tests.cpp`,
  `tests/sketch_rectangle_dimension_ui_tests.cpp` и
  `tests/sketch_regression_tests.cpp`;
- `tests/CMakeLists.txt`;
- `benchmarks/stage6_sketch_solver_benchmark.cpp`,
  `benchmarks/run_stage6_sketch_solver_benchmark.ps1`,
  `benchmarks/CMakeLists.txt`;
- `docs/decisions/2026-10-13-r3-solver-backend.md` и настоящий отчёт.

Persistence schema в R3 не менялась: используется принятый в R1 format v3.
`src/project/ProjectFile.cpp`, `docs/project-format-v3.md` и persistence tests,
видимые в общем worktree, относятся к уже принятому R1 и не являются новым
изменением R3. Пользовательские untracked файлы не удалялись и не изменялись.

## 4. Новые deterministic regressions и property tests

`sketch_nonlinear_solver_tests` проверяет:

- immutable solve и отдельный validated candidate;
- nearest-previous решение недоопределённой системы независимо от порядка;
- hint только в nullspace hard constraints и восстановление последнего
  принятого candidate при неудачной коррекции;
- typed redundancy/conflict и отсутствие переклассификации `NonConverged` в
  `Conflicting`;
- локально минимальный conflict set для нескольких независимых конфликтов;
- bounded localization для component больше 64 constraints с
  `conflictSetComplete=false` и без вымышленных IDs;
- disabled/probed `Lock`, solved-candidate diagnostics и владельцев remaining
  DOF;
- симметричный `Equal` drag без operand locks;
- malformed containment;
- все permutations определённого набора;
- explicit previous candidate и impossible hint;
- seeded randomized small systems с проверкой residual/fingerprint;
- disconnected components и typed solve cache без повторного solve.

`sketch_render_cache_ui_tests` дополнительно доказывает один release delta,
Undo/Redo, отсутствие permanent drag constraints и rollback незавершённого drag
по Escape и смене инструмента. Существующие project/persistence regressions
доказывают canonical save/load до и после solve.

## 5. Targeted test results

Release targeted gate — **7/7 PASS**:

- `project_file_tests`;
- `sketch_regression_tests`;
- `sketch_render_cache_ui_tests`;
- `sketch_rectangle_dimension_ui_tests`;
- `sketch_constraint_integrity_tests`;
- `sketch_equation_kernel_tests`;
- `sketch_nonlinear_solver_tests`.

Reviewer после финальной правки benchmark path независимо повторил два
ключевых Release tests: **2/2 PASS**.

## 6. Полный clean CI, dev verifier и ручной smoke

- `cmake --preset ci` — PASS. При первом запуске vcpkg запросил сетевую
  загрузку CMake 4.4.2; повтор с разрешённым доступом завершился успешно.
- `cmake --build --preset ci --clean-first` — PASS, 461 build step. После
  последней `.cpp`-правки benchmark helper выполнена успешная incremental
  сборка; заголовки после clean build не менялись.
- `ctest --preset ci --output-on-failure` — **82/82 PASS**, 27,51 с.
- `.opencode/scripts/verify-and-run.ps1` — `SOLIDAR_CONFIGURE_OK`,
  `SOLIDAR_BUILD_OK`, **82/82 PASS** за 89,24 с,
  `SOLIDAR_GUI_SMOKE_OK` (8 с), `SOLIDAR_VERIFY_SUCCESS`.
- Ручной smoke на копии `build/dev/r3-manual-smoke.solidar`: проект открыт,
  editor показал локализованное русское состояние `Избыточные ограничения` с
  IDs 1–11; corner drag переместил допустимую позу при сохранении размеров;
  Undo одним действием восстановил исходную геометрию, Redo повторил drag;
  завершение sketch перестроило модель; Ctrl+S снял dirty marker; повторное
  открытие сохранённой копии восстановило ту же геометрию без ошибки. Исходный
  fixture не изменялся.

Manual UI smoke выполнен через отдельную тестовую копию; приложение оставлено
открытым для дополнительной пользовательской проверки.

## 7. Benchmark baseline/after на одинаковых fixtures

Reference host: текущая Windows/MSVC Release `ci`-сборка. p95 указан в
миллисекундах. Каждый timed R3 sample включает press journal, preview mutation,
component solve и ровно один release delta.

| Geometry | Workload | R2 baseline p95 | R3 p95 | Historical path в том же run | Semantic hash | Status/residual |
|---:|---|---:|---:|---:|---:|---|
| 100 | connected drag | 9,3880 | **4,9133** | 7,2497 | `17240525979305364551` | success / 0 |
| 500 | connected drag | 97,6476 | **83,3953** | 115,8475 | `9281624794748141543` | success / 0 |

100-geometry gate 16,7 мс выполнен с запасом. Для 500 geometry R3 быстрее R2
baseline примерно на 14,6%, то есть не приближается к допустимому порогу +20%.

В исходном R2 fixture наклонная цепочка один раз нормализовалась Solver v1 до
таймера. R3 сразу строит получившуюся canonical horizontal pose, чтобы не
включать запрещённый sequential pre-solve в новый workload. Совпадающие
semantic hashes подтверждают идентичность фактически измеряемых fixtures.

## 8. Результат независимого review

Финальный verdict независимого reviewer:

> PASS. HIGH/MEDIUM замечаний не осталось; benchmark теперь честно измеряет
> R3 drag-path.

Reviewer проверил journal flow, получение previous candidate из press-time
state, tangent-hint fast path, охват таймера, совпадение semantic hashes и
`git diff --check`. Его независимые результаты: 100 geometry — 5,1445 мс
против historical 6,9572 мс; 500 geometry — 80,4866 мс против historical
113,8558 мс и R2 baseline 97,6476 мс; residual 0, те же hashes. HIGH и MEDIUM
замечаний нет.

## 9. Известные ограничения и разрешённый следующий этап

- Equation/Jacobian и SVD остаются dense внутри большой connected component;
  tangent fast path исключает полный SVD только когда hint уже лежит на hard
  manifold. Это не нарушает текущие gates, но sparse backend остаётся
  направлением масштабирования.
- Lightweight drag намеренно может вернуть `rankComputed=false`; точный rank
  считается debounced/on-release full diagnostics.
- Для component больше 64 constraints conflict localization намеренно bounded:
  report помечается неполным и не публикует непроверенные IDs.
- PlaneGCS использован только как изолированный spike/oracle и не поставляется.

После приёмки этого отчёта разрешён только **R4 — shadow rollout и cutover**.
Работы R4 в рамках R3 не начинались.

## 10. Расхождения с инструкцией

- Для benchmark R3 canonical horizontal fixture строится непосредственно,
  вместо obsolete pre-timer Solver-v1 normalization из R2. Это осознанное
  расхождение устраняет sequential solver из нового измерения; semantic hashes
  совпадают с R2 timed fixture.
- Новая dependency, notices и SBOM запись не добавлялись, поскольку выбран уже
  поставляемый OCCT TKMath.
- PlaneGCS остался изолированным differential spike. Production regressions
  используют общие equation invariants и не требуют включения PlaneGCS в CI.
- Остальных расхождений нет; все параметры завершения R3 доказаны.
