# R1 — типизированный model contract и persistence

Дата проверки: 13 октября 2026 г.

Статус поставки: **ЗАВЕРШЕНА**. R0 atomicity regressions, обязательные R1
regressions, clean CI, dev verifier, ручной save/load/Undo/Redo smoke и
детерминированный persistence round-trip прошли. Semantic hashes benchmark
совпали; обнаруженное замедление connected-drag зафиксировано как известное
ограничение, поскольку R1 не задаёт performance threshold.

## 1. Восстановленные инварианты и устранённые пользовательские риски

- `PointReference` имеет ровно один tagged source: datum origin, line endpoint,
  circle center, arc endpoint, Bezier point или composite center. Переключение
  source заменяет payload, поэтому смешанное состояние невозможно.
- Constraint хранит stable `ConstraintId`, `ConstraintMode` и один из 22
  типизированных payloads. Не относящиеся к выбранному типу semantic-поля в
  payload отсутствуют.
- Driving value существует только в соответствующем constraint payload.
  Dimension annotation хранит placement и точный `ConstraintId`; её
  `valueMm` является только вычисляемой compatibility projection и не входит в
  linked persistence/fingerprint.
- Reference constraint не имеет driving value и не создаёт hard equation;
  Disabled constraint также исключён из solve.
- Удаление, изменение значения и переключение mode выполняются по точному
  `ConstraintId`; shape/type matching удалён из production mutation paths.
- Единая model-level validation boundary проверяет uniqueness/staleness IDs,
  geometry kinds, parameter ranges, self-reference, payload/mode compatibility
  и возвращает typed code вместе со связанными IDs.
- Публичный `addConstraint()` не принимает caller-supplied ID: persisted IDs
  восстанавливаются только через fail-closed `restoreConstraints()`. Обычное
  добавление всегда проходит schema validation и journal rollback.
- Annotation нельзя материализовать без существующего размерного
  `ConstraintId`; duplicate link/ID и non-dimensional target отклоняются без
  изменения semantic fingerprint.
- Persistence cursors принимаются только строго выше максимального live ID для
  element, geometry, constraint и dimension, поэтому malformed v3 не может
  повторно выдать уже используемый ID после следующей вставки.
- Persistence v3 хранит tagged references, typed payloads, modes и точные
  annotation links. Malformed/duplicate/stale данные отклоняются до замены
  открытого `Document`.
- Конфликтный, но структурно валидный sketch загружается для ремонта без
  частичного solve. R0 transaction/rollback contract сохранён.

## 2. Выбранное решение и отклонённые альтернативы

Выбрано разделение на три слоя:

1. Канонические tagged/variant сущности в Qt-free model.
2. Одна schema boundary (`SketchSchema`) для UI, loader и solver entry points.
3. Явный codec v3 с детерминированным migration adapter для v1/v2.

Legacy source syntax временно обслуживается compatibility views, которые
проецируют только активный tagged payload и не хранят параллельных semantic
значений. Это позволило перевести persistence и mutation paths без массового
одновременного переписывания всех исторических test fixtures.

Отклонены:

- поиск Dimension↔Constraint по типу/geometry/value — неоднозначен при двух
  похожих размерах;
- хранение `Dimension::valueMm` как второго authoritative value — допускает
  рассинхронизацию;
- best-effort чтение неизвестного v3 payload — нарушает fail-closed;
- угадывание ambiguous legacy dimension — migration policy переводит её в
  отдельный Reference constraint;
- silent change формата v2 — новая схема получила отдельную версию 3;
- solve во время migration — мог бы менять историческую геометрию и публиковать
  partial result.

### Implementation map

| Область | Authoritative state | Validation/observer | Undo/cache/persistence |
|---|---|---|---|
| Point references | один `PointReference::Payload` | `validatePointReferenceSchema` | stable geometry/element IDs |
| Constraints | `ConstraintPayload` + `ConstraintId` + mode | `validateConstraintSchema` | delta journal хранит payload, ID и mode |
| Dimensions | constraint payload value; annotation placement + `ConstraintId` | exact lookup `constraintById` | linked `valueMm` только projection |
| UI mutations | `SketchCommandController` вызывает exact-ID mutators | typed model result → UI | failed operation не создаёт delta |
| Solver/diagnostics | только Driving constraints | общая canonical model schema | R0 atomic candidate/rollback сохранён |
| Project load | staged decoded `Document` | `validateSketchSchema` до publish | v3 exact; v1/v2 migration deterministic |
| Fingerprints | tagged payload, IDs, mode, exact link | solver/semantic consumers | порядок и IDs сохраняются round-trip |

### Старая и новая схема

| Сущность | v1/v2 / прежний model | v3 / R1 model |
|---|---|---|
| Point reference | несколько nullable/zero полей (`lineId`, `circleId`, `arcId`, flags) | один tagged payload с одним source |
| Constraint | enum type + общий набор geometry/point/value fields | envelope `{id, mode, payload}`; payload типизирован |
| Driving value | дублировался в constraint и `Dimension::valueMm` | только `payload.drivingValue` |
| Annotation link | type/shape/value matching или отсутствующая связь | точный stable `constraintId` |
| Reference dimension | мог быть похож на driving constraint | Reference mode без persisted driving value/hard equation |
| Persistence version | 1/2, untyped sketch records | 3, `typed-sketch-v1`; v1/v2 readers сохранены |

## 3. Изменённые production/test/persistence файлы

Model и solver:

- `src/sketch/Sketch.h`, `src/sketch/Sketch.cpp`;
- `src/sketch/SketchSchema.h`, `src/sketch/SketchSchema.cpp`;
- `src/sketch/SketchSolver.h`, `src/sketch/SketchSolver.cpp`;
- `src/sketch/SketchConstraintDiagnostics.cpp`;
- `src/CMakeLists.txt`.

UI consumers:

- `src/ui/SketchCommandController.h/.cpp`;
- `src/ui/SketchCanvas.cpp`;
- `src/ui/SketchRenderer.cpp`;
- `src/ui/SketchHitSceneAdapter.cpp`.

Persistence и документация:

- `src/project/ProjectFile.cpp`;
- `docs/project-format-v3.md`.

Tests:

- новый `tests/sketch_schema_tests.cpp` и регистрация в
  `tests/CMakeLists.txt`;
- обновлены project, controller, constraint-integrity, rectangle-dimension,
  hit-test, project-switch, pattern-persistence и sketch-regression tests.

## 4. Новые deterministic regressions и property tests

- compile-time assertions для шести PointReference variants и 22 constraint
  payload types; constructor/assignment test доказывает замену source;
- schema matrix всех constraint types, valid/invalid geometry combinations,
  wrong kind, self-reference, invalid range и invalid mode;
- две сходные dimensions на одном component: mode/remove/value изменяют только
  точный `ConstraintId`;
- удаление geometry инвалидирует/удаляет только связанные constraints и
  annotations;
- Undo/Redo сохраняет exact IDs и modes;
- v1/v2 fixtures мигрируют детерминированно, v3 round-trip сохраняет IDs,
  order и semantic fingerprint;
- duplicate/stale IDs и malformed payload не меняют открытый `Document`;
- структурно валидный конфликт двух driving lengths сохраняется и загружается
  без частичного solve;
- deterministic 96-step insert/delete/reorder property sequence проверяет
  ссылочную целостность после каждой операции;
- существующие R0 atomicity regressions остаются зелёными.
- public-boundary regressions доказывают отказ для explicit constraint ID,
  stale reference, invalid mode, line-line tangent и unlinked annotation;
- malformed v3 fixtures отдельно проверяют каждый свободный allocator cursor,
  расположенный ниже максимального live ID;
- historical v1/v2 fixtures не содержат искусственных R1 reference
  constraints: loader сначала строит deterministic exact links во временном
  состоянии и только затем материализует annotations.

## 5. Targeted test results

Финальный targeted набор:

```text
project_file_tests
project_switch_ui_tests
sketch_regression_tests
sketch_command_controller_tests
sketch_rectangle_dimension_ui_tests
sketch_constraint_integrity_tests
sketch_schema_tests
7/7 passed
```

В полном clean CI эти же tests выполнялись вместе со всеми regression,
ui-smoke, compliance и topology suites.

## 6. Полный clean CI, dev verifier и ручной smoke

### Clean CI — PASS

- `cmake --build --preset ci --clean-first`: **447/447** build steps.
- `ctest --preset ci --output-on-failure`: **80/80 passed**, 23.86 s.
- `git diff --check`: PASS.

### Dev verifier — PASS

Точный `.opencode/scripts/verify-and-run.ps1` завершился успешно:
configure/build OK, **80/80 passed** (46.63 s), GUI launch-smoke OK,
`SOLIDAR_VERIFY_SUCCESS`.

### Повторный ручной smoke — PASS

В dev-приложении создан ignored-проект
`build/dev/r1-manual-smoke.solidar`:

1. Создан sketch на XY и прямоугольник.
2. Undo визуально удалил прямоугольник и вернул статус `Эскиз пуст`.
3. Redo восстановил четыре линии и статус `Эскиз недоопределён · DOF: 4`.
4. Sketch завершён, проект сохранён; UI подтвердил `Проект сохранён` и снял
   dirty marker.
5. Через Open тот же файл загружен повторно; дерево содержит `Эскиз 1`, а
   viewport показывает тот же прямоугольник.
6. Файл независимо проверен: `version = 3`, `schema = typed-sketch-v1`, четыре
   line records и 11 typed constraint envelopes.

## 7. Benchmark baseline/after

Baseline — принятый R0 after-файл; R1 after снят тем же executable/script и теми
же fixtures, 21 sample на строку. Semantic hash совпал во всех 12 строках.

| Entities | Mode | R0 p50, ms | R1 p50, ms | Delta |
|---:|---|---:|---:|---:|
| 100 | localized drag | 0.0445 | 0.0470 | +5.62% |
| 100 | connected drag | 2.0073 | 2.7811 | +38.55% |
| 100 | unchanged solve | 0.0025 | 0.0025 | 0.00% |
| 100 | full diagnostics | 1.3096 | 1.2401 | -5.31% |
| 500 | localized drag | 0.2037 | 0.1921 | -5.69% |
| 500 | connected drag | 35.7061 | 52.1643 | +46.09% |
| 500 | unchanged solve | 0.0111 | 0.0124 | +11.71% |
| 500 | full diagnostics | 24.5756 | 27.0050 | +9.89% |
| 1000 | localized drag | 0.3675 | 0.3704 | +0.79% |
| 1000 | connected drag | 137.7203 | 213.8773 | +55.30% |
| 1000 | unchanged solve | 0.0223 | 0.0253 | +13.45% |
| 1000 | full diagnostics | 99.2595 | 108.4295 | +9.24% |

Артефакты: `build/ci/r0-sketch-solver-after.csv` и
`build/ci/r1-sketch-solver-after.csv`.

R1 не задаёт performance threshold, поэтому это не красный формальный gate, но
connected-drag regression является реальным известным ограничением. Основная
причина — временный compatibility façade увеличил размер/copy cost canonical
constraint objects. Это нельзя скрывать при планировании R2/R4 performance work.

## 8. Результат независимого review

Финальный независимый verdict: **PASS; HIGH/MEDIUM findings не осталось**.

Reviewer повторно проверил working-tree diff и подтвердил:

- `addConstraint()` отклоняет explicit ID, валидирует schema до mutation и
  всегда использует journal/rollback;
- `storeDimension()` и `restoreConstraints()` fail-closed запрещают unlinked,
  duplicate и non-annotatable связи;
- legacy v1/v2 loader декодирует dimensions во временный массив и создаёт
  deterministic exact links до materialization;
- все четыре allocator cursors должны быть строго выше максимального live ID;
- `git diff --check` и семь targeted suites прошли.

Первоначальные три HIGH замечания тем самым закрыты; новых HIGH/MEDIUM reviewer
не обнаружил. Reviewer файлы не изменял.

## 9. Известные ограничения и разрешённый следующий этап

- Compatibility views старого source API ещё присутствуют, но не являются
  semantic storage. Их удаление требует отдельного `rg`-подтверждённого
  перевода оставшихся historical call sites.
- `Dimension::valueMm` физически сохранён как derived compatibility projection;
  для linked annotation он не persisted, не fingerprinted и не authoritative.
- Connected-drag benchmark замедлился на 38–55%; semantic hashes не изменились.
  До R4 cutover требуется вернуть performance к согласованному gate.
- Старые v1/v2 readers сохранены; старый reader не обязан читать v3.
- R2 не начинался. Следующий разрешённый этап после принятия этого отчёта:
  **R2 — equation kernel**.

## 10. Расхождения с инструкцией

- Канонические payloads полностью типизированы, но compatibility façade старого
  field-style API пока физически присутствует в оболочках `PointReference` и
  `Constraint`. Он не может хранить несколько sources/irrelevant semantic
  values, однако его удаление отложено до нулевого числа call sites.
- `Dimension::valueMm` не удалён из ABI, а превращён в неавторитетную projection.
  Требование об одном authoritative numeric value выполнено на уровне model,
  fingerprints и persistence.
- Benchmark имеет заметную регрессию, хотя R1 не определяет порога. Она явно
  зафиксирована, а не объявлена несущественной.
