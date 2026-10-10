# Этап 6: Sketch solver и Undo/Redo

Дата измерения: 2026-10-08. Preset: `ci`, Release, MSVC, Qt 6.8.3,
OpenCASCADE 8.0.1. Тайминги и память являются report-only; приёмочные gates
основаны на детерминированных счётчиках работы.

## Что изменено

- `GeometryId` и `ConstraintId` разрешаются ленивыми индексами O(1).
- Детерминированный граф constraints учитывает прямые geometry-поля, line /
  circle / arc `PointReference`, а также `elementCenterId` со всеми членами
  composite element. Datum origin остаётся fixed anchor и не соединяет разные
  компоненты. Existing line-endpoint/circle-center junctions в пределах
  `1e-7` также являются явными рёбрами графа, потому что legacy mutators
  перемещают такой CAD vertex как кластер. Identity indexes и
  coordinate-dependent connectivity имеют отдельные dirty flags, поэтому
  move-away/reconnect не оставляет stale component cache.
- Interactive mutators передают dirty geometry seeds. Solver строит компактный
  sub-sketch только соответствующего connected component и сливает результат
  обратно по стабильным ID.
- Locked snapshot содержит только locked geometry текущего компонента, а не
  полный `Sketch` на каждом pass.
- Повторный unchanged solve использует отдельный solver fingerprint и возвращает тот
  же convergence/residual status с нулём passes.
- Полный Jacobian DOF/conflict audit в `SketchCanvas` coalesced таймером и
  явно flush-ится на release; model остаётся Qt-free.
- History entry MainWindow теперь non-null value type; callback application
  защищён RAII для `applyingUndo_` и rollback при exception, STEP import очищает
  старую историю, selection/history position сохраняются stable refs. Sketch
  history ограничена 32 MiB/100 steps; model history — 128 MiB/100 steps.
- Part Design history сохраняет body metadata и cold feature-level slices;
  добавленные/удалённые Body также cold и не удерживают OCCT Shape/Topology.
  Imported B-Rep хранится в контролируемом байтовом архиве и материализуется
  заново при apply. Полные `Document` не захватываются callbacks.
- `SketchCanvas` пишет typed mutation journal и хранит reversible
  per-container `SketchDelta`; тестовая instrumentation подтверждает ноль
  полных копий Sketch для длинной серии обычных операций. Journal хранит
  transaction-origin ID order и dimension tokens, поэтому удаления A+C/C+A,
  mixed line/circle/arc, constraints, center nodes и first/middle/last
  dimensions восстанавливают исходные индексы независимо от порядка мутаций.
- `SketchDelta` применяется через полностью staged vectors: removals сортируются
  по убыванию source index, insertions — по возрастанию target index, а source и
  target counters/fingerprints проверяются до первого swap. Historical orphan
  constraints сохраняют явную provenance, поэтому delete/cancel/Undo обратимы,
  но новый orphan нельзя внести поддельной delta.
- Persisted semantic fingerprint отделён от solver cache key и включает
  dashed/element IDs, circle/arc presentation, все `PointReference`, dimensions,
  center nodes и next-ID counters. На уровне Document дополнительно сравниваются
  placement и support; неизменённый `Finish Sketch` не создаёт history command.
- Join visibility вычисляется как shared-consumer relation. Edit/remove/cascade
  освобождают operand только после исчезновения последнего surviving Join;
  Cancel, два Undo/Redo и project round-trip сохраняют тот же результат.
  Edit presentation временно показывает только exclusive operand; Body,
  который одновременно consumed другим Join, не попадает в viewport scene.
- Edit dispatcher сохраняет raw mutually-exclusive selection и stable history
  location до tool-specific selection. Cancel/modal reject восстанавливают их
  без model/history/modified mutation; Accept сохраняет true pre-tool state на
  Undo и финальное committed state на Redo. Dispatch guard также завершает
  transaction на каждом unsupported/no-op/invalid-source/not-at-end/exception
  выходе; следующий unrelated tool не наследует stale snapshot. Это проверено
  для всех 15 matrix variants с sentinel Body/Edge/Face selection, включая
  invalid topology preview и последующий реальный Cancel.
- Supported edit с Accept без изменения persisted параметров распознаётся как
  empty `DocumentDelta`: Undo/Redo entry не создаётся, исходные raw selection,
  stable history position, extrusion source и modified flag восстанавливаются,
  а все sessions/preview/manipulators/pick state полностью завершаются. Этот
  no-op проверен через production handlers для всех 15 matrix variants с
  чередованием Body/Edge/Face sentinel и исходного modified `true/false`;
  последующий editor открывается и отменяется штатно. Полная persisted-definition
  проверка сравнивает канонический JSON byte-for-byte и исключает только
  top-level `createdAt`: это генерируемое при каждом export время, а не состояние
  `Document`.
- Component solve сохраняет persisted constraint vector order. Regression с
  raw unsolved `[id=20 Equal(B,C), id=10 Equal(A,C)]` запускает full и component
  solve из двух идентичных состояний без предварительного solve и отличает
  правильную ветвь (20 mm) от ошибочной ID-sort ветви (10 mm). V1/V2 load и
  round-trip сохраняют тот же vector order и результат.
- Coordinate junction regression покрывает line endpoint/circle center на
  `0.5e-7`, транзитивную line-circle-line цепочку, узел вне tolerance,
  move-away/reconnect с invalidation connectivity cache и точный delta
  Undo/Redo. Production mutators перемещают весь связанный кластер, а component
  metrics посещают ровно ожидаемые geometry.

## Benchmark

Baseline: `build/ci/stage6-sketch-solver-baseline.csv`, SHA-256
`0EB55EDCCC291D38F716B5553EBDB56A96D5B7FAC2254B688335046CA9121420`.
Optimized: `build/ci/stage6-sketch-solver-optimized.csv`, SHA-256
`06B47454CD2C03A2121191E733400B0A68EB02CE2AAD8183EA36450BD509F8E8`.

| entities | localized p95, ms | unchanged p95, ms | full diagnostics p95, ms | connected p95, ms |
|---:|---:|---:|---:|---:|
| 100 | 0.0876 | 0.0078 | 4.7702 | 10.4000 |
| 500 | 1.0569 | 0.0151 | 41.1507 | 129.4102 |
| 1000 | 1.2407 | 0.0396 | 145.7387 | 402.7794 |

Для 1000 independent entities frozen historical orchestration имеет p95
full-snapshot/full-solve стоимость порядка сотен миллисекунд. После component
scoping/cache финальный correction-cycle rerun после изменения connectivity
даёт для localized drag и unchanged solve p95 1.2407 ms и 0.0396 ms
соответственно. Connected workload закономерно остаётся
O(N²) внутри существующей sequential math: этап не заменяет математический
движок.

Baseline реализован изолированным frozen historical orchestration внутри того
же benchmark binary: полный Sketch snapshot и full solve. Обе реализации
получают идентичные fixtures, samples и CSV schema; `connected_drag` в обоих
случаях — одна цепочка из N geometry и 2N-1 constraints.
Все 12 baseline/optimized пар имеют одинаковый `semantic_hash`.

Детерминированные gates optimized matrix:

- localized: `components/geometries/constraints = 1/1/1`;
- connected: `1/N/(2N-1)`;
- unchanged: `passes = components = geometries = constraints = 0`;
- full diagnostics: `full_diagnostics_count = 1`.

`allocation_count/allocated_bytes/peak_owned_bytes` снимаются scoped-перехватом
полного набора `operator new/new[]` (обычные, sized, aligned и nothrow варианты)
в отдельном benchmark executable. Поэтому это реальные события/байты выделения
и пик одновременно живых allocations именно внутри измеряемой операции, а не
оценка по непустым векторам. Перехват не входит в production binary. Process
RSS не используется как gate из-за фоновых allocations Qt/OCCT и
нестабильности между запусками.

## Известное ограничение

Существующая sequential solver math всё ещё квадратична для одного большого
connected component. Полная замена движка не входила в этап 6; отдельный design
proposal нужен, если реальные эскизы регулярно образуют компоненты порядка
1000 constraints.

UI-тесты отдельно покрывают реальные direct Sketch/Face Extrude, Move и removal
handlers. Production history matrix использует реальные ToolSession/accept,
history edit-dispatch и remove handlers для Sketch Extrude, Face Extrude,
Revolve, Pocket, Fillet, Chamfer, Shell, Draft, Join, Mirror, Move,
Linear/Circular Pattern (Join и NewBody), затем выполняет create/edit/remove и
два Undo/Redo цикла для каждого типа с проверкой полного concrete payload,
валидного B-Rep и topology index, разрешения persistent references, exact
selection/stable history location и полного teardown всех sessions/preview.
Expected edit payload/B-Rep строится независимым test oracle через явные setters
и отдельный recompute, а не копируется из production result.

## Проверка correction cycle

- `cmake --build --preset ci --clean-first`: 396/396 targets.
- Targeted Part Design/UI/Sketch/persistence matrix: 34/34 passed.
- После обнаруженного CI timing failure диагностический JSON diff подтвердил,
  что единственным расхождением unchanged-Accept snapshots был заново
  сгенерированный `createdAt`. После канонизации этого metadata field
  `project_switch_ui_tests` прошёл 20/20 последовательных повторов и полный
  CTest prefix 1–23 прошёл 23/23; persisted model payload/B-Rep assertions не
  ослаблялись.
- Persistence/history дополнительно: 6/6 passed (`project_file`,
  `project_lifecycle`, STEP, parametric/pattern history).
- Qt UI tests запускались через CTest или с точным PowerShell-присваиванием
  `$env:QT_QPA_PLATFORM='offscreen'`. В `cmd.exe` нужна форма
  `set "QT_QPA_PLATFORM=offscreen"`: вариант `set QT_QPA_PLATFORM=offscreen &&`
  добавляет хвостовой пробел и приводит к ожидаемому Qt `qFatal`, потому что
  плагина с именем `offscreen ` не существует.
- CAD-memory tests используют compounds по 120 solids: cold history освобождает
  исходные Shape/Topology weak references, budget детерминированно вытесняет
  FIFO entries, oversized command откатывается и не превышает лимит.
- `git diff --check`: passed. Пользовательский `Новый проект.solidar`:
  SHA-256 `9999E1BB846E914F1E206BD4011048382FCEF58BCCC31B035C72463D95690BC4`
  (без изменений).
