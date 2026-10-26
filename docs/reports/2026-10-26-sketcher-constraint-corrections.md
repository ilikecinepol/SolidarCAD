# Исправление ограничений Sketcher — 2026-10-26

## Контекст и границы

- Рабочая копия: отдельный managed worktree для ветки исправлений.
- Ветка: `codex/sketcher-constraint-fixes`.
- Исходный HEAD: `be36d750c5210604071b6d642d114667caf9c375` (`main` на момент начала). Реализация подготовлена в отдельной ветке `codex/sketcher-constraint-fixes`; commit/push выполняются отдельным явным запросом после завершения проверок.
- Основная dirty working copy не изменялась. Из неё только прочитано содержимое отсутствующего в managed worktree verifier-скрипта.
- Не затрагивались компоненты/сборки, ProductEnvelope V5/V6, assembly UI, assembly persistence и формат файла проекта.
- До реализации прочитаны `AGENTS.md` и релевантные инструкции `.opencode/skills` для Sketcher/solver, safe change, regression testing, Qt safety и build-and-run.

Базовый аудит выполнялся на committed HEAD в отдельном Debug-каталоге с `/Z7 /Ob0 /Od /RTC1`; исходный focused-набор из 12 Sketcher/model/UI/persistence тестов проходил. Для каждого этапа новый oracle сначала воспроизводил соответствующий дефект на неизменённом production-коде, затем тот же тест переводился в зелёное состояние минимальным bounded diff. После каждого этапа выполнялась обязательная focused-матрица и независимое ревью; переход к следующему этапу происходил только после `APPROVE` без P1/P2.

## S1 — Disabled и Reference

### Finding

Уравнения уже исключали non-driving ограничения, но conflict/redundancy preflight и граф solver-компонент продолжали учитывать `Disabled` и `Reference`. Отключённый `Horizontal` мог блокировать `Vertical`, а non-driving связь искажала local solve, DOF и выбор якорей.

### Решение и инварианты

- Введён единый `participatesInConstraintSystem`.
- Один предикат используется при emission уравнений, validation/preflight, построении компонент, localized solve/drag, DOF/diagnostics и anchor selection.
- `Disabled`/`Reference` остаются authoritative сущностями с прежними ID, сериализацией и UI mode, но не участвуют в математике и связности.
- Переключение `Driving -> Disabled/Reference -> Driving` сохраняет точный Undo/Redo и tri-state UI.

### Regression oracle

`nonDrivingConstraintsDoNotParticipateInSolverTopology` проверяет acceptance `Horizontal -> Disabled -> Vertical`, отсутствие equation, раздельные компоненты, локальный drag, DOF и model-level Undo/Redo. Save/load mode и participation semantics проверяются отдельным oracle в `project_file_tests`; дополнительный UI-oracle находится в `sketch_render_cache_ui_tests`.

## S2 — конечность публичных mutation API

### Finding

`addLine`, `addRectangle`, `addCircle` и `setDimensionPlacement` принимали `NaN`, `+Inf` или `-Inf` до изменения authoritative model.

### Решение и инварианты

- Все затронутые публичные границы проверяют каждый числовой аргумент до выделения ID, записи в registry/history, dirty state и UI effects.
- Возвращается typed fail-closed результат; нет clamp/нормализации нечислового значения.
- Поведение Arc/Bezier не изменено.

### Regression oracle

`publicMutationBoundariesRejectNonFiniteAtomically` перебирает `NaN`, `+Inf` и `-Inf` для каждого аргумента и сверяет ID allocator, геометрию, ограничения, размеры и placements до/после отказа.

## S3 — атомарное удаление Locked-геометрии

### Finding

Модель могла оставить заблокированную геометрию, тогда как command/UI сообщали success, очищали selection и могли создать пустой Undo; смешанный batch удалял только незаблокированную часть.

### Решение и инварианты

- Введён typed результат `Removed/Locked/Stale/NoChange`.
- Batch сначала полностью preflight-ится. Любой `Locked` или stale ID отклоняет весь batch без частичного изменения.
- Command effects содержат только реально изменённые ID.
- Rejected Delete/Cut/Trim не очищает selection, не меняет dirty state и не создаёт Undo entry; UI показывает точную русскую диагностику.
- Успешное удаление остаётся одной атомарной history-командой с точным Undo/Redo.

### Regression oracle

- `typedBatchDeletionPreflightsWholeSelection` — model-level locked/mixed/unlocked/stale/Undo/Redo.
- `lockedBatchDeletionIsAtomicAndTruthful`, `lockedAndNoOpMutationsReportTruthfully`, `trimRejectsEveryLockedGeometryKindAtomically` — command-layer results/effects.
- UI-блок в `sketch_rectangle_dimension_ui_tests` — Delete/Ctrl+X/Trim, selection, diagnostics, dirty state и history.

## S4 — duplicate/redundancy и неизвестные enum

### Finding

Legacy `addConstraint` обходил часть rank/redundancy policy; переставленный `Tangent` не канонизировался; неизвестный enum мог тихо получить `Horizontal` payload/registry rule.

### Решение и инварианты

- Legacy `addConstraint` безопасно сужен, но сохраняет более дешёвый transactional solve: unsupported type, malformed schema и точный `Driving`-дубль отклоняются до выделения ID. Полная rank/redundancy classification остаётся ответственностью интерактивной границы `tryApplyConstraint`.
- Одинаковые `Reference`/`Disabled` могут получать разные ID как осознанные non-driving annotations; duplicate reject применяется, только когда оба эквивалентных ограничения участвуют в системе (`Driving`).
- `Tangent` включён в симметричную канонизацию аргументов.
- Неизвестный enum сохраняется неизвестным и возвращает `Invalid/Unsupported`; lookup registry nullable и больше не падает на первое `Horizontal`-правило.
- Exhaustive compile/runtime oracle требует явного обновления switch/registry при добавлении нового типа.
- Stable codec и round-trip всех 22 существующих типов сохранены.

### Regression oracle

`duplicatePolicyAndUnknownTypesFailClosed`, equation-registry exhaustive test и существующий 22-type round-trip в `project_file_tests`.

## S5 — сингулярные старты Angle и Tangent

### Finding

Angle на точно parallel/antiparallel линиях и Tangent при линии через центр окружности не давали solver первого шага и возвращали ложный `Degenerate/Conflict`.

### Математический контракт

- Несингулярный Angle сохраняет исторический `acos` residual без изменения tolerance, DOF или diagnostics.
- Только когда `sine.value == 0.0` и `abs(abs(cosine.value) - 1.0) <= 1e-14`, сохраняется прежнее значение `acos` residual, а производная детерминированно заимствуется из ориентированной `atan2`-ветви; signed zero нормализован.
- Для Tangent точная нулевая signed-distance получает детерминированное направление производной, выбранное из stable IDs и канонического направления линии.
- Случайный jitter и скрытый success отсутствуют.
- Zero-length, zero-radius и non-finite остаются typed `Degenerate/NonFinite`.
- Late failure полностью откатывает pose/history; permutation, localized solve, drag/release, Undo/Redo и reload не меняют выбранную ветвь.

### Regression oracle

- `singularAngleAndTangentStartsAreDeterministic` покрывает permutation, parallel/antiparallel, aligned/opposed, free/partially fixed, redundant/overconstrained, localized solve, rollback, model-level Undo/Redo и drag/release.
- Отдельный v3 save/load oracle в `project_file_tests` сверяет точный semantic fingerprint выбранной детерминированной ветви после round-trip.
- `nearAntiparallelAnglePreservesAcosResidual` доказывает неизменность близкого, но несингулярного residual.
- Независимый численный review принят без P1/P2/P3.

## S6 — Radius end-to-end

### Finding

`Radius` существовал в schema/kernel/codec, но не мог владеть dimension annotation и не имел пользовательской команды; circle AutoDimension создавал только Diameter.

### Решение и инварианты

- `Radius` разрешён как владелец annotation.
- В Sketch ribbon добавлена явная команда `Радиус`; существующий AutoDimension окружности по-прежнему создаёт `Диаметр`.
- Radius и Diameter имеют независимые русские подписи, hit-test, placement, edit и renderer path.
- Creation/edit/delete, Undo/Redo, reload и project switch работают без изменения версии формата.
- V3 и V4 validation/writer paths принимают существующий stable Radius codec; неизвестные значения по-прежнему fail-closed.

### Regression oracle

- `radiusConstraintCanOwnAnAnnotation`.
- Radius UI-блок в `sketch_rectangle_dimension_ui_tests`: explicit creation, Diameter default, edit, delete, Undo/Redo, offset/angle placement, hit/double-click после reload и project switch.
- `project_format_v4_legacy_sketch_validator_tests`: hand-authored v4 Radius annotation.
- `project_file_tests`: фактический v4 writer/load round-trip Radius.

Красно-зелёные дополнительные дефекты этапа: Radius label hit сначала игнорировал `offsetMm`, а v4 typed-sketch gate не включал Radius. Оба случая получили отдельные failing oracle до исправления.

## Дополнительная тестовая коррекция

`sketch_renderer_tests` по-прежнему проверяет 10 000 примитивов и те же render-cache assertions, но тестовый fixture теперь заполняет friend-доступом контейнеры и один раз пересчитывает bounds. Ранее публичный `addLine()` вызывал полный `updateBounds()` после каждого добавления, превращая подготовку Debug-теста в O(n²) и зависание более четырёх минут. Production-код, Debug flags и проверяемый renderer path не изменены; тест проходит за 0,89 с.

## Проверки

### Этапные проверки

После S1–S6 выполнялись focused build/test с `--clean-first` после изменений заголовков и минимальный набор:

`sketch_schema_tests`, `sketch_equation_kernel_tests`, `sketch_nonlinear_solver_tests`, `sketch_constraint_integrity_tests`, `sketch_regression_tests`, `sketch_lock_constraint_tests`, `sketch_command_controller_tests`, `sketch_rectangle_dimension_ui_tests`, `sketch_interaction_controller_tests`, `sketch_render_cache_ui_tests`, `project_switch_ui_tests`, `project_file_tests`.

Каждая этапная матрица завершилась без падений. S6 дополнительно независимо перепроверен: 6/6 focused tests зелёные.

### Dev verifier

Штатного `.opencode/scripts/verify-and-run.ps1` в managed worktree нет. Содержимое скрипта из основной копии выполнено в памяти с единственной заменой вычисления `repoRoot` на текущий managed worktree; файл основной копии не изменялся.

Команда окружения использовала локальную установку Qt 6.8.3 и OCCT из существующего CI vcpkg install tree.

Результат:

- configure `dev`: PASS;
- build `dev`: PASS;
- Debug CTest: **117/117 PASS**, 230,39 с;
- labels в verifier summary: compliance 4, regression 89, topology 33, ui-smoke 12, unit 66;
- 8-секундный запуск `build\dev\src\solidar.exe`: `SOLIDAR_GUI_SMOKE_OK`;
- итог: `SOLIDAR_VERIFY_SUCCESS`.

### Clean CI

Окружение: MSVC Build Tools 2022 `vcvars64.bat`, repository-local vcpkg, `VCPKG_DEFAULT_TRIPLET=ci-x64-windows`, Qt 6.8.3 для MSVC 2022 x64.

Команды:

```bat
cmake --preset ci
cmake --build --preset ci --clean-first -- -j 4
ctest --preset ci --output-on-failure
ctest --preset ci -L unit --output-on-failure
ctest --preset ci -L regression --output-on-failure
ctest --preset ci -L ui-smoke --output-on-failure
ctest --preset ci -L compliance --output-on-failure
ctest --preset ci -L topology --output-on-failure
```

Окончательный прогон после всех production/test/report изменений:

- configure `ci`: PASS;
- clean-first build: **609/609** целей;
- полный CTest: **117/117 PASS**, 52,46 с;
- `unit`: **66/66 PASS**, 16,13 с;
- `regression`: **89/89 PASS**, 40,46 с;
- `ui-smoke`: **12/12 PASS**, 18,76 с;
- `compliance`: **4/4 PASS**, 0,45 с;
- `topology`: **33/33 PASS**, 12,60 с.

Первый более ранний принудительно `offscreen` прогон дал единственное environment-only падение `viewport_renderer_tests` на `QOpenGLContext::create()`; тот же тест, окончательный полный прогон и label-прогоны в штатной Windows-среде прошли.

### GUI smoke

В живой Debug-сборке подтверждено:

- после отключения `Горизонтально` DOF вернулся с 3 до 4; `Вертикально` было принято, DOF стал 3;
- Delete выбранной Lock-линии показал `Удаление отклонено: геометрия заблокирована ограничением «Замок»`; линия осталась видимой и выделенной;
- явная команда `Радиус` создала `R 20.00 мм`; double-click edit изменил размер и геометрию до `R 18.00 мм`.

Живой coordinate-smoke singular Angle/Tangent не завершён: слой Windows automation потерял точную привязку окна из-за параллельно активного пользовательского приложения с несохранёнными данными. Рискованных кликов по чужому окну не выполнялось. Этот сценарий закрыт воспроизводимыми model/UI-harness тестами (`singularAngleAndTangentStartsAreDeterministic`, drag/release и command/UI matrix), но это явно не выдается за четвёртое ручное GUI-наблюдение.

## Независимое ревью и severity

Каждый bounded этап S1–S6 принят независимым ревьюером без открытых P1/P2; S5 прошёл отдельный численный review. Финальное ревью всего совокупного production/test/report diff завершилось вердиктом `APPROVE`: блокирующих P0/P1/P2 нет. Ревьюер независимо повторил четыре критических focused-теста (`lock`, `renderer`, `constraint_integrity`, `equation_kernel`): 4/4 PASS.

Текущий статус findings:

- P0: нет;
- P1: нет;
- P2: нет;
- P3: остаётся неблокирующая неоднозначность публичного legacy API `addConstraint`: он допускает валидную rank-redundancy, тогда как интерактивный `tryApplyConstraint` по умолчанию её откатывает. Structural builders зависят от текущего поведения; рекомендуемое продолжение — сделать legacy API private/internal либо явно документировать его structural-only контракт;
- process evidence: остаётся описанное ограничение живого singular Angle/Tangent coordinate-smoke, покрыто автоматизированными oracle, но не считается закрытым ручным наблюдением.

## Открытые риски

- Сингулярная ветвь намеренно активируется только на точной математической сингулярности. Численно близкие конфигурации продолжают старый `acos` path; это защищено regression oracle, но требует сохранения при будущих изменениях solver.
- V4 использует существующий codec без format bump. Любое будущее расширение `ConstraintType` обязано обновить exhaustive registry/codec oracle.
- OpenGL UI-тесты зависят от доступного desktop context и не должны принудительно запускаться с `QT_QPA_PLATFORM=offscreen`.

## Намеренно вне scope

- Изменение solver tolerances, случайный jitter и общий численный рефакторинг.
- Новая persisted schema/version.
- Изменение семантики Arc/Bezier.
- Расширение `MainWindow.cpp`.
- Компоненты/сборки, ProductEnvelope V5/V6, assembly UI/persistence.
- Merge в `main`, разрешение пересечений с параллельной assembly/V6-работой и любые изменения её scope.
