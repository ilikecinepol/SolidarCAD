# R0 — атомарность и наблюдаемость Solver v1

Дата проверки: 13 октября 2026 г.

Статус поставки: **ЗАВЕРШЕНА**. Production-изменения, targeted tests, clean CI,
20 последовательных повторов, dev verifier, полный ручной smoke, benchmark и
независимый review прошли. Все критерии R0 доказаны; этап R1 разрешён.

## 1. Восстановленные инварианты и устранённые пользовательские риски

- Интерактивные mutation paths теперь решают candidate state внутри delta journal
  и публикуют его только после полного успешного solve/audit. Ошибка восстанавливает
  геометрию, constraints, dimensions, selection-related semantic state и solver
  cache.
- `solveStableComponent()` больше не публикует успешно решённые компоненты, если
  поздняя компонента, stale reference, unsupported constraint, non-finite или
  degenerate geometry делает всю операцию неуспешной.
- Полный `solveStable()` проверяет также constraints, которые не попали ни в одну
  связную компоненту, включая constraint без geometry references. Их ошибка
  обнаруживается до merge кандидатов.
- Автоматические coincident/axis constraints больше не исчезают молча: каждый
  `tryApplyConstraint()` проверяется, допустимая rank-redundant привязка сохраняется,
  а реальная ошибка отменяет всю команду создания примитива.
- UI различает duplicate/redundant, conflict, stale reference, unsupported,
  non-convergence и internal failure; в сообщении доступны связанные
  `ConstraintId`.
- После отказа следующий валидный edit, Undo/Redo и save/load продолжают работать;
  cache после rollback не содержит candidate state.

Таким образом, изменения восстанавливают инварианты реформы 1, 8, 9 и 10 для
охваченных публичных mutation paths, не вводя контрактов R1–R4.

## 2. Выбранное решение и отклонённые альтернативы

Выбрано containment-решение вокруг существующего `BasicSketchSolver`:

1. Solver вычисляет локальные кандидаты и собирает staged merge.
2. Общий preflight/audit проверяет ссылки, поддержку, residual, конечность и
   геометрическую вырожденность.
3. Merge выполняется один раз после успеха всех компонентов.
4. Публичный mutator владеет delta journal и возвращает типизированный результат.
5. Controller переводит model status в UI error, сохраняя исходный diagnostic
   payload.

Отклонены:

- покомпонентный немедленный commit — оставляет half-solved sketch;
- удаление старого constraint ради сходимости — нарушает постоянный инвариант;
- ослабление tolerance/игнорирование unsupported constraint — создаёт ложный
  success;
- откат только геометрии без solver cache — позволяет наблюдать candidate state
  после отказа;
- переход к типизированной модели ссылок/equation kernel — относится к R1/R2 и
  запрещён в R0.

### Implementation map

| Область | Authoritative state | Transaction owner | Наблюдатели/результат | Rollback/cache |
|---|---|---|---|---|
| `translateElement`, `translateSelection`, `translateLinesByIds` | `Sketch` geometry + constraints | публичный mutator | `SketchMutationResult` → controller → canvas | journal snapshot + solver cache restore |
| circle/arc/Bezier move и reshape, `translatePoint` | stable-ID geometry в `Sketch` | публичный mutator | типизированные status/diagnostic/related IDs | полный отказ без частичного publish |
| `setConstraintValue` | constraint value + candidate geometry | публичный mutator | `SketchMutationResult` | constraint, geometry и cache откатываются вместе |
| `tryApplyConstraint` | constraint list + candidate geometry | `Sketch::tryApplyConstraint` | `ConstraintApplyResult` с `SolveResult` и component | новый constraint и solve-кандидат не публикуются при ошибке |
| automatic snapping | primitive command outer transaction | `SketchCommandController` | каждая auto-constraint проверяется | любая ошибка откатывает примитив и все auto-constraints |
| full/component solve | geometry в `Sketch`; B-Rep здесь не участвует | solver staging, merge после audit | `SolveResult` | cache обновляется только после полного success |
| Undo/Redo, persistence | delta journal / `.solidar` semantic state | существующие owners | fingerprints и стабильные IDs | failed mutation не создаёт delta и не меняет save payload |

Публичные production-вызовы solver и `tryApplyConstraint` были перечислены через
`rg`; прямой индексный `translateCircle(size_t, ...)` также переведён на
транзакционный stable-ID path.

## 3. Изменённые production/test/persistence файлы

Production:

- `src/sketch/Sketch.h` — model-level result/status/diagnostic contracts;
- `src/sketch/Sketch.cpp` — transaction ownership, rollback, constraint and
  geometry validation;
- `src/sketch/SketchSolver.h`, `src/sketch/SketchSolver.cpp` — staged component
  solve, full preflight/audit, atomic merge and cache discipline;
- `src/ui/SketchCommandController.h`, `src/ui/SketchCommandController.cpp` —
  propagation of failures and atomic auto-constrain;
- `src/ui/SketchCanvas.cpp` — distinct Russian UI messages and related IDs.

Tests/persistence coverage:

- `tests/sketch_constraint_integrity_tests.cpp`;
- `tests/sketch_command_controller_tests.cpp`;
- `tests/project_file_tests.cpp`;
- `tests/project_switch_ui_tests.cpp`.

Формат `.solidar` и production persistence code не менялись; добавлены только
round-trip regressions существующего формата.

## 4. Новые deterministic regressions и property tests

- ранняя успешная и поздняя неуспешная solver component не дают partial commit;
- locked/non-convergent edit сохраняет fingerprint и исходную геометрию;
- orphan stable ID и constraint без references обнаруживаются до merge;
- invalid и unsupported получают разные diagnostic codes;
- NaN и zero-length geometry отклоняются;
- положительные circle/arc radius `1e-12` остаются допустимыми;
- micro-Bezier проверяется тем же predicate, что и persistence, и реально проходит
  save/load;
- rollback восстанавливает cache: повторный solve остаётся cache hit;
- duplicate result возвращает точный affected component;
- auto-snap failure откатывает новый primitive и journal, после чего валидная
  команда проходит;
- failed mutation → valid delta → Undo → Redo → save/load → continued edit;
- существующие permutation/property regressions solver продолжили проходить.

## 5. Targeted test results

Обязательный набор после финальных исправлений:

```text
ctest --preset ci -R "sketch_(regression|constraint_integrity|lock_constraint|command_controller|interaction_controller)_tests" --output-on-failure
5/5 passed
```

Дополнительные persistence/repeat проверки:

```text
ctest --preset ci -R "project_file_tests|sketch_(constraint_integrity|command_controller)_tests" --repeat until-fail:20 --output-on-failure
60/60 executions passed, 37.48 s
```

`editor_smoke_tests` и `rectangle_dimension_ui_tests`, затронутые automatic
constraints, также прошли после исправления обработки допустимого redundant snap.

## 6. Полный clean CI, dev verifier и ручной smoke

### Clean CI — PASS

- После изменений заголовков выполнен `cmake --build --preset ci --clean-first`:
  442/442 build steps успешно.
- После двух финальных локальных LOW-правок, не менявших заголовки, выполнен
  incremental build: 75/75 steps успешно.
- `ctest --preset ci --output-on-failure`: **79/79 passed**, 26.42 s.
- `git diff --check`: PASS.

### Dev verifier — PASS

Запущен точный `.opencode/scripts/verify-and-run.ps1`: configure и Debug build
успешны, CTest дал **79/79**, launch-smoke завершился маркером
`SOLIDAR_VERIFY_SUCCESS`. Исправленный `project_switch_ui_tests` входит в этот
успешный прогон; timeout не увеличивался и красный gate не скрывался.

### Ручной smoke — PASS

Повторный полный smoke выполнен в Release-приложении
`build/ci/src/solidar.exe` на временном ignored-проекте
`build/ci/r0-manual-smoke.solidar`:

1. Созданы rectangle, цепочка line/arc и две окружности.
2. Наложены и визуально подтверждены Coincident, H/V, Equal, Tangent и driving
   dimension `30.00 мм`.
3. Свободная окружность и ограниченная точка перетаскивались повторно; Equal,
   Coincident, H/V, Tangent и размер сохранялись, статус оставался корректным.
4. На окружность наложен Lock: DOF изменился с 13 до 10. Две попытки перенести
   её в разные позиции получили явный статус
   `Операция отклонена: конфликт ограничений`; положение и размер окружности на
   обоих снимках остались неизменными.
5. Сразу после отказов Undo снял последний успешный Lock (DOF 10 → 13), а Redo
   восстановил его (DOF 13 → 10). Следовательно, rejected drag не создал Undo
   entry и не повредил последующую историю.
6. Проект сохранён, приложение закрыто и тот же `.solidar` открыт заново.
   Геометрия, размер и ограничения восстановились; после reopen выполнено новое
   валидное перемещение незаблокированной окружности с сохранением Equal.
7. Эскиз завершён; из прямоугольного профиля создан Extrude длиной 25 мм. На
   верхней грани создан второй sketch с окружностью и выполнен Pocket длиной
   25 мм. Оба preview и итоговое тело визуально подтверждены.
8. Финальное сохранение успешно: индикатор `*` исчез из заголовка окна.

Smoke тем самым покрывает обязательную последовательность create → constrain →
valid drag → rejected drag/rollback → Undo/Redo → save/reopen → continued edit →
Extrude/Pocket.

## 7. Benchmark baseline/after

Одинаковые fixtures, 21 sample на строку; semantic hash совпал во всех 12 строках.
Артефакты сохранены в ignored build tree:
`build/ci/r0-sketch-solver-baseline.csv` и
`build/ci/r0-sketch-solver-after.csv`.

| Entities | Mode | Baseline p50, ms | After p50, ms |
|---:|---|---:|---:|
| 100 | localized drag | 0.0393 | 0.0445 |
| 100 | connected drag | 1.7837 | 2.0073 |
| 100 | unchanged solve | 0.0022 | 0.0025 |
| 100 | full diagnostics | 1.1633 | 1.3096 |
| 500 | localized drag | 0.2011 | 0.2037 |
| 500 | connected drag | 35.6323 | 35.7061 |
| 500 | unchanged solve | 0.0111 | 0.0111 |
| 500 | full diagnostics | 23.3379 | 24.5756 |
| 1000 | localized drag | 0.4412 | 0.3675 |
| 1000 | connected drag | 139.0783 | 137.7203 |
| 1000 | unchanged solve | 0.0223 | 0.0223 |
| 1000 | full diagnostics | 98.9184 | 99.2595 |

Наблюдаемая разница не является существенной регрессией на этих fixtures; самый
заметный относительный рост приходится на субмиллисекундные/малые случаи, а
connected/full workloads 500–1000 элементов остаются близки к baseline.
Benchmark выполнен до последней правки счётчика malformed constraints и явного
test assertion; эти две правки не меняют solver path успешных fixtures.

## 8. Результат независимого review

Независимый reviewer сначала обнаружил три HIGH-проблемы; после исправлений — ещё
две более узкие HIGH-проблемы: constraint без references мог миновать preflight,
а первоначальный Bezier degeneracy predicate расходился с persistence. Обе
устранены соответствующими regressions.

Финальный review подтвердил отсутствие HIGH. Два оставшихся LOW — счётчик всех
unvisited invalid constraints и явная проверка component для duplicate — также
исправлены; выполнен заключительный delta-review после этих правок.

## 9. Известные ограничения и разрешённый следующий этап

- Известная order dependence Solver v1 остаётся допустимым и явно
  задокументированным ограничением R0; её устранение не входит в этот этап.
- Временный smoke-проект сохранён только в ignored build tree и не входит в
  поставку.
- R0 по правилам комплекта **завершён**.
- Разрешённый следующий этап: **R1 — model contract**.

## 10. Расхождения с инструкцией

Неразрешённых расхождений нет. Benchmark был снят до двух заключительных узких
правок failure-only/test assertions; эти правки не затрагивают успешные benchmark
fixtures, а их semantic hashes остались подтверждены targeted и full CI.
