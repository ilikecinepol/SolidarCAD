# Задача для Codex — устранение результатов аудита кода

Дата: 7 октября 2026 г.

## Режим выполнения

Выполни задачу через полный SolidarCAD AI-team pipeline.

Не начинай с изменения кода. Сначала определи затронутые подсистемы и запусти
подходящие независимые read-only аудиты параллельно:

- `solidcad-explorer` — карта текущей реализации и зависимостей;
- `solidcad-regression-designer` — минимальные падающие до исправления тесты;
- `solidcad-occt-auditor` — B-Rep, rebuild, persistent topology и OCCT safety;
- `solidcad-qt-auditor` — UI state, Viewport, event loop и lifetime;
- `solidcad-sketcher-crash-hunter` — Sketch, solver и Undo/Redo для
  соответствующего этапа.

После получения результатов `solidcad-lead` должен сформировать отдельный
implementation contract для текущего этапа. Продуктовый код изменяет только
один `solidcad-implementer`. После реализации обязательны независимый
`solidcad-reviewer`, при необходимости `solidcad-build-engineer`, а последним —
`solidcad-local-verifier`.

Не выполнять все этапы одним гигантским diff. Каждый этап должен завершаться
сборкой, тестами, review и кратким отчётом до начала следующего. Если review
находит BLOCKER/HIGH, разрешён один точечный цикл исправления через того же
implementer с повторной проверкой.

Не считать задачу выполненной без успешных build + tests + GUI smoke launch.

## Контекст

SolidarCAD — C++20, Qt 6.8.3, OpenCASCADE 8.0.1, CMake и vcpkg. Основные слои:

- `solidar_model` — `Document`, `Body`, `Feature`, Sketch и solver;
- `solidar_project` — `.solidar`, STEP/STL и persistence;
- `solidar_sketch_ui`, `solidar_drawing_ui`, `solidar_viewport3d` — UI-модули;
- `solidar_editor` — `MainWindow` и координация инструментов;
- `solidar_home` и `solidar` — оболочка приложения.

На момент подготовки задания актуальная `ci`-сборка проходила полностью:
69/69 тестов. В рабочем дереве находился пользовательский untracked-файл
`Новый проект.solidar`; его нельзя изменять, удалять, добавлять в индекс или
использовать как тестовый fixture.

Аудит выявил три release-blocking риска корректности, несколько
алгоритмических bottleneck и накопившуюся перегрузку UI/application-слоя.
Точки входа, указанные ниже, являются ориентирами для исследования, а не
разрешением на локальный симптоматический патч.

## Цель

Устранить риски потери данных, падения на повреждённых проектах и нарушения
графа зависимостей; убрать подтверждённые алгоритмические bottleneck;
сократить число параллельных источников истины; укрепить тестовую и
архитектурную границы проекта.

После выполнения пользователь должен иметь возможность безопасно:

1. открывать проекты v1 и v2;
2. получать диагностируемый отказ на повреждённом v2 без изменения текущего
   документа;
3. удалять Sketch/Feature/Body без висячих cross-body зависимостей;
4. редактировать длинную историю и сложные эскизы без лишних глобальных
   перестроений;
5. работать с крупной B-Rep-моделью без полного CPU picking и полного remesh на
   каждое движение мыши;
6. продолжать использовать Undo/Redo, save/load и persistent topology без
   регрессий.

## Подтверждённое текущее неправильное поведение

### 1. Повреждённый v2 может молча открыться как legacy

- `src/project/ProjectFile.cpp:554-556`: `validate()` проверяет только legacy
  `load()` и не валидирует параметрическую модель v2.
- `src/ui/MainWindow.cpp:469-475`: ошибка `loadDocument()` сохраняется в
  `modelError`, но не используется.
- `src/ui/MainWindow.cpp:512-539`: любой отказ v2 трактуется как отсутствие
  параметрической истории; создаётся пустой `Document`, восстанавливаются лишь
  legacy box/sketches, после чего загрузка возвращает успех.

Последующее сохранение может затереть историю повреждённого проекта.

### 2. Повреждённый проект способен выбросить исключение через UI boundary

- `src/project/ProjectFile.cpp:798-980`: unchecked преобразования ID, enum и
  числовых параметров, недостаточная проверка ссылочной целостности.
- `src/model/Document.cpp:30-37,86-91`: duplicate SketchId/BodyId бросают
  `std::invalid_argument`.
- `src/model/Body.cpp:51-55`: отсутствует проверка уникальности FeatureId.
- На публичной границе загрузки нет общей защиты от стандартных и OCCT
  исключений.

### 3. Cascade delete неполно учитывает cross-body зависимости

- `src/model/Document.cpp:165-180,205-220`: удаление Feature строит только
  локальный suffix выбранного Body.
- `src/model/Document.cpp:183-202,223-240`: удаление Sketch может собрать
  зависимости нескольких Body, но применяет удаление фактически к одному.

Под риском `JoinBodies`, NewBody-паттерны и Feature, зависящие от Sketch на
удалённой грани.

### 4. Recompute выполняет повторные глобальные обходы

- `src/model/Document.cpp:113-148`: dirty propagation повторно сканирует все
  Feature и весь dirty-set до fixpoint, затем rebuild идёт в порядке хранения
  Body, а не по явному DAG.
- `src/model/Body.cpp:74-109`: `updateSketchPlacements()` вызывается после
  каждой валидной Feature, включая clean.
- `src/model/Document.cpp:349-377`: placement update снова обходит Sketch,
  Body и Feature и выполняет topology resolution.

### 5. Persistent topology повторно индексирует одну и ту же Shape

- `src/model/TopologyReferenceResolver.cpp:155-189`: candidates и signatures
  строятся заново, дедупликация имеет квадратичное поведение.
- `resolveFaceReference()`/`resolveEdgeReference()` повторяют эту работу для
  каждой ссылки.
- Единственный semantic-tag candidate может быть принят без проверки
  сохранённой signature.
- Документированный tolerance и константа в коде расходятся.

### 6. Picking и preview блокируют GUI-поток

- `src/ui/Viewport.cpp:4628-4693`: на каждом mouse move проецируются все
  треугольники, а для каждого сегмента ребра снова перебираются треугольники:
  `O(segments * triangles)`.
- `src/ui/ViewportPicking.cpp:269-343`: rectangle selection содержит
  `O(triangles^2)` и `O(segments * triangles)` пути.
- Fillet/Chamfer/Shell/Pattern preview синхронно запускают OCCT и полный remesh
  на последовательных drag events.
- `src/ui/ViewportRenderer.cpp` отображает не более первых 32 highlights, хотя
  selection model допускает больше.

### 7. Sketch solver и Undo плохо масштабируются

- `src/sketch/SketchSolver.cpp:1906-1969`: `solveStable()` допускает до 16
  проходов, копирует весь Sketch и повторно запускает диагностику.
- Диагностика строит плотный Jacobian и выполняет dense rank elimination.
- `MainWindow` и `SketchCanvas` хранят до 100 полных snapshot через Document,
  Sketch и захваченные closures.
- Часть Part Design действий имеет только undo callback и не поддерживает
  симметричный redo.

### 8. UI/application-слой перегружен и содержит дублирующее состояние

- `src/ui/SketchCanvas.cpp` — около 14 200 строк;
- `src/ui/Viewport.cpp` — около 6 300 строк;
- `src/ui/MainWindow.cpp` — около 6 000 строк;
- `src/sketch/Sketch.cpp` — около 5 400 строк.

`MainWindow` одновременно управляет файлами, UI, инструментами, Document,
Undo/Redo и историей. Рядом с `Document` хранятся `sketchHistory_`,
`hasExtrusion_`, `sketchCount_` и legacy Viewport state. Управляющие события и
плоскости местами кодируются специальными или локализованными строками.

### 9. Сериализация и публичная модель нарушают расширяемость и инкапсуляцию

- `src/project/ProjectFile.cpp:596-741` — длинная save-цепочка `dynamic_cast`;
- `src/project/ProjectFile.cpp:823-980` — зеркальная load-цепочка по строковому
  type;
- `src/model/Document.h:46,55` возвращает наружу mutable-контейнеры;
- `src/model/SolidFeature.h` протаскивает `QString` и legacy UI semantics в
  заявленный Qt-free model layer.

### 10. Тема и тестовая инфраструктура неоднородны

- В `SketchCanvas.cpp` и `Viewport.cpp` используются многочисленные прямые
  `QColor(...)`, включая фиксированный светлый фон, вместо `ThemeColors`.
- В 18 тестовых файлах остаётся около 675 raw `assert()`.
- CMake-регистрация тестов, labels и Windows runtime environment дублируется и
  уже пропускает отдельные новые targets.
- Нет sanitizer/fuzz/performance профиля и общего timeout для CTest.

## Обязательные правила и invariants

Перед каждым этапом прочитать `AGENTS.md` и релевантные инструкции из
`.opencode/skills/*`. В частности соблюдать `safe-change`, `occt-safety`,
`persistent-topology`, `qt-safety`, `regression-testing`,
`part-design-interactions`, `sketcher-safety` и `git-hygiene` по затронутым
подсистемам.

Во всех этапах обязательны следующие invariants:

- `Document` остаётся единственным авторитетным источником committed-модели;
- B-Rep перестраивается из параметров и не становится источником истины в
  `.solidar`; существующий импортированный B-Rep остаётся допустимым
  специальным случаем;
- stable ID не меняются без versioned migration;
- persistent topology не заменяется индексом ребра или грани;
- ambiguity detection не ослабляется;
- teardown всегда вызывает controller `cancelActive()` и session `cancel()` до
  замены `Document`;
- failed preview не уничтожает последнее committed valid state;
- hover не превращается в selection;
- Cancel возвращает исходное состояние;
- Undo/Redo, history scrubbing, save/load и project switching сохраняют
  согласованную геометрию;
- UI остаётся русским; внутренние управляющие протоколы не зависят от
  локализованного текста;
- цвета идут через `ThemeColors` или явно оформленные семантические CAD-роли;
- новые тесты используют `CHECK`, а не `assert`;
- изменения заголовков проверяются через `--clean-first`;
- нельзя удалять/ослаблять failing tests или подавлять ошибки silent catch;
- нельзя добавлять commit, push, merge, rebase, reset или clean без отдельного
  запроса пользователя;
- существующие пользовательские и unrelated изменения сохраняются.

## Порядок реализации

### Этап 0. Baseline и implementation map

Read-only этап.

1. Зафиксировать HEAD, статус рабочего дерева, версии toolchain и текущий
   результат `ci`.
2. Построить карту ownership/data flow для ProjectFile, Document/Body/Feature,
   ToolSession, Viewport и Sketch.
3. Создать воспроизводимые измерительные сценарии:
   - загрузка `.solidar` по фазам read/parse/build/solve/recompute;
   - clean, leaf-edit и root-edit recompute для истории 10/100/1000 Feature;
   - topology resolution для 10/100/1000 ссылок;
   - hover и marquee на 10k/100k/1M triangles;
   - drag/solve на Sketch из 100/500/1000 сущностей.
4. Зафиксировать p50/p95/p99, число OCCT build/mesh/topology calls, peak memory
   и allocations там, где это практически измеримо.

Не использовать нестабильный wall-clock threshold как единственный CI gate.
Для автотестов предпочитать детерминированные счётчики и проверки сложности;
timings хранить в отчёте.

Критерий выхода: есть baseline, минимальные regression-сценарии и отдельный
implementation contract для этапа 1.

### Этап 1. Безопасная и однократная загрузка `.solidar`

1. Ввести явное различение результатов загрузки: корректный v1, корректный v2
   и invalid/unsupported project. Конкретный тип API определить после аудита,
   но boolean не должен смешивать legacy и ошибку.
2. Для v2 запретить silent fallback в legacy при любой ошибке model.
3. Не менять текущий Document, project path, modified state, ToolSession или UI
   до полного успешного staging нового проекта.
4. Разбирать JSON один раз в валидируемое промежуточное представление.
5. Проверить обязательные поля, допустимые enum, конечность/range чисел,
   уникальность и ненулевое значение ID, целостность ссылок, array counts и
   документированные resource limits.
6. FeatureId должен быть уникальным в требуемой области документа.
7. Добавить exception boundary, переводящую `Standard_Failure`, стандартные и
   неизвестные исключения в диагностируемую русскую ошибку. Исключения нельзя
   просто подавлять.
8. Сохранить атомарную запись через `QSaveFile` и совместимость существующих
   корректных v1/v2.
9. Bulk-load Sketch geometry/constraints должен выполнять solver только после
   сборки согласованного Sketch, а не после каждого отдельного constraint.

Критерии приёмки:

- валидный v1 продолжает загружаться;
- валидный v2 проходит round-trip с теми же ID, ссылками и историей;
- повреждённый v2 отклоняется и не меняет открытый проект;
- повторное Save после отказа не может перезаписать повреждённый файл;
- duplicate/zero ID, invalid enum, NaN/Inf/range, missing field, broken
  reference, oversized input и неизвестный Feature type дают контролируемый
  отказ;
- UI-путь выполняет одно чтение и один JSON parse;
- каждый отказ покрыт детерминированной регрессией.

### Этап 2. Полный граф зависимостей и корректное удаление

1. Построить явное представление зависимостей Sketch/Body/Feature с reverse
   edges.
2. Исправить `planFeatureRemoval`, `planSketchRemoval` и применение plan так,
   чтобы вычислялся полный transitive closure по всем Body.
3. План удаления должен хранить отдельные диапазоны/наборы для каждого Body и
   применяться атомарно после полной проверки.
4. Учесть direct Feature dependencies, Sketch dependencies и Sketch support на
   удаляемой грани.
5. Добавить cycle detection и понятную диагностику.
6. Выполнять rebuild в детерминированном топологическом порядке, а не полагаться
   на порядок Body в JSON/vector.
7. Начать закрывать mutable-container API `Document`: оставить const
   ranges/views, а изменения проводить через валидируемые commands/methods.

Критерии приёмки:

- Feature/Sketch/Body removal не оставляет dangling references;
- покрыты JoinBodies, Linear/Circular Pattern NewBody, sketch-on-face и
  несколько зависимых Body;
- удаление либо успешно применяет полный план, либо не меняет Document;
- цикл диагностируется без hang/stack overflow;
- результат rebuild не зависит от допустимой перестановки Body в файле;
- существующие stable ID и topology references сохраняются.

### Этап 3. OCCT exception firewall и доменные лимиты

1. Определить единый exception firewall для Feature rebuild, ProjectFile,
   BodyRenderMesh и STEP/STL adapters.
2. Проверить Move, Mirror, Linear/Circular Pattern и другие Feature без полной
   защиты `Standard_Failure`/`BRepCheck`.
3. Применять двухфазную схему `compute -> validate -> commit`: не публиковать
   частично построенную или невалидную Shape/Mesh.
4. На уровне domain model ввести единые limits, включая верхний предел pattern
   count, согласованный с UI. Loader не может обходить эти ограничения.
5. Согласовать last-valid semantics: UI preview failure сохраняет committed
   состояние; ошибка document rebuild имеет явный статус и диагностику.

Критерии приёмки:

- OCCT-исключение не выходит в Qt event loop и не завершает процесс;
- невалидный результат не заменяет последнее валидное committed состояние;
- count `0`, `1`, выше лимита и `INT_MAX` проверены для паттернов;
- STL/mesh failure оставляет понятную ошибку и не создаёт ложный успешный
  результат;
- exception tests не зависят от текста внутренних сообщений OCCT.

### Этап 4. Incremental recompute и persistent topology

1. Добавить проверяемые индексы `BodyId`, `FeatureId`, `SketchId` и reverse
   dependencies. Индексы не должны стать вторым несогласованным источником
   истины.
2. Заменить repeated full scans на dirty queue в topological order.
3. Обновлять placement только у Sketch, привязанных к реально изменившейся
   Feature/Shape revision.
4. Создать immutable/cached `TopologyIndex` на Shape revision с уникальными
   face/edge maps, bounds, signatures и semantic tags.
5. Разрешать набор ссылок batch-операцией без повторного индексирования Shape.
6. Если сохранена signature, semantic tag обязан пройти проверку совместимости;
   ambiguity нельзя разрешать выбором первого кандидата.
7. Согласовать tolerance в документации и коде через тесты, а не произвольным
   изменением константы.

Критерии приёмки:

- clean recompute не запускает OCCT rebuild и глобальные topology-обходы;
- signatures одной Shape revision строятся один раз;
- leaf edit не перестраивает независимые ветви;
- cycle/error diagnostics остаются детерминированными;
- persistent-topology тесты проходят для parameter edit, save/load, split,
  merge, ambiguity и legacy reference;
- benchmark не регрессирует на малой модели и показывает улучшение на большой.

### Этап 5. Picking, preview и mesh scalability

До выбора решения измерить baseline из этапа 0. Допустимы GPU ID/depth picking,
BVH/AABB или другой доказуемо корректный spatial index. Не навязывать технологию
до аудита OpenGL/Qt/OCCT lifetime.

1. Убрать вложенный полный перебор edge-segment × triangles из mouse-move path.
2. Убрать `O(triangles^2)` из marquee selection.
3. Кэшировать projection/picking data по camera revision и mesh revision.
4. Coalesce mouse events и preview updates не чаще одного раза на кадр.
5. Для drag использовать облегчённый preview; точный OCCT rebuild и normal mesh
   выполнять на release/accept либо по контролируемому debounce.
6. Отбрасывать stale preview по generation ID/cancellation.
7. Не переносить OCCT в worker, пока не доказаны immutable inputs,
   thread-safety, lifetime и запрет публикации устаревшего результата.
8. Устранить ограничение первых 32 highlights либо согласовать единый доменный
   предел выбора и явно отразить его в UX.
9. Перейти к per-body/revision mesh cache и уменьшить дублирование indexed mesh
   и развёрнутых CPU triangles; picking acceleration не должен требовать второй
   полной копии геометрии.

Критерии приёмки:

- hover, selection и marquee корректны при перекрытии, occlusion и скрытых
  гранях;
- выбор более 32 поддерживаемых элементов полностью визуализируется;
- drag не вызывает полный mesh rebuild на каждое входное mouse event;
- в mouse-move пути отсутствует полный nested scan всех сегментов и
  треугольников;
- имеются deterministic correctness tests и отдельный opt-in benchmark;
- в отчёте приведены p50/p95/p99 и peak memory до/после на одинаковой модели;
- целевой ориентир: p95 hover/drag 16,7–33 мс, без отдельной синхронной UI-фазы
  дольше 50 мс. Если железо не позволяет достичь ориентира, представить
  измерения и конкретный оставшийся bottleneck, не подменяя результат.

### Этап 6. Sketch solver и симметричный Undo/Redo

1. Добавить индексы GeometryId/ConstraintId и граф связности constraints.
2. Решать только затронутые connected components.
3. Не копировать весь Sketch на каждый внутренний pass; snapshot locked geometry
   должен иметь минимально необходимый scope.
4. Отделить solver math, mutation commands и expensive diagnostics.
5. Выполнять полную DOF/conflict diagnostics по debounce/on-release либо в
   доказуемо безопасном worker, а не безусловно на каждый drag event.
6. Заменить полные Document/Sketch snapshots на command/delta undo или
   structural sharing с периодическими checkpoint и лимитом по памяти.
7. Все поддерживаемые Part Design операции должны иметь симметричные Undo и
   Redo и восстанавливать committed model, selection и history position.

Полная замена математического solver сторонним или новым движком не является
обязательной частью этого этапа. Если локальная перестройка недостаточна,
подготовить отдельный доказательный design proposal, не маскируя проблему.

Критерии приёмки:

- существующие solver tolerance не ослаблены;
- constraint integrity сохраняется после delete/undo/redo/save/load;
- повторный solve без изменения входа детерминирован;
- drag не запускает полный solve/diagnostics для независимых компонентов;
- Undo/Redo симметричен для всех текущих инструментов Part Design;
- benchmark 100/500/1000 сущностей показывает время, passes, allocations и
  память до/после.

### Этап 7. Декомпозиция application/UI и persistence

Это последовательность малых behavior-preserving extraction, а не перепись.

1. Оставить `MainWindow` composition root; вынести application services для
   project load/save, commands/undo и Part Design coordination.
2. Сделать `Document` единственным committed source of truth. UI хранит ID и
   revisioned view/cache, а не независимые копии геометрии и feature state.
3. Удалять `sketchHistory_`, `hasExtrusion_`, legacy extrusion state и другие
   зеркала только после переключения всех потребителей и regression coverage.
4. Разделить `SketchCanvas` на renderer, hit testing, tool state/commands и
   interaction controller.
5. Разделить `Viewport` на rendering passes, picking и tool interaction.
6. Заменить служебные строки (`__cancel_*__`, labels плоскостей, поиск текста
   ошибки) typed events, enums/references и error codes.
7. Ввести стабильный `FeatureKind` и registry codec/factory в
   `solidar_project`; UI descriptor/editor registry разместить в
   `solidar_editor`. Model Feature не должен знать Qt JSON или widgets.
8. Удалить Qt-тип из `SolidFeature`/model compatibility API или перенести
   legacy contract в подходящий верхний слой.
9. Закрыть прямой mutable-доступ к контейнерам `Document` после миграции
   вызывающего кода.

Критерии приёмки:

- каждый извлечённый компонент имеет одну явную ответственность;
- нет параллельных authoritative state между UI и `Document`;
- добавление Feature требует регистрации codec/factory, а не изменения двух
  центральных type chains;
- model target не требует Qt types;
- пользовательские сценарии до и после extraction эквивалентны;
- не появляется обратная зависимость model/project на editor/UI;
- старый путь удаляется только после перевода всех потребителей.

### Этап 8. Theme и тестовая инфраструктура

1. Перенести UI colors в `ThemeColors` и семантические роли для axes,
   selection, preview, error и diagnostics.
2. Проверить light/dark/system без фиксированного светлого background.
3. Создать общий test helper и заменить raw `assert` на `CHECK` либо другой
   всегда активный диагностируемый механизм проекта.
4. Централизовать создание test target, labels, timeout и Windows Qt/OCCT
   runtime environment одной CMake-функцией.
5. Убедиться, что каждый test target зарегистрирован через эту функцию.
6. Добавить поддерживаемый sanitizer job там, где он совместим с Qt/OCCT;
   конкретный внешний blocker документировать, а не скрывать.
7. Добавить malformed-input/property tests и opt-in fuzz target для project
   decoder. Fuzzing не должен писать за пределы временного каталога.
8. Добавить opt-in performance targets; timing thresholds не включать в обычный
   CI без стабилизации среды.

Критерии приёмки:

- light/dark/system проходят UI smoke;
- в UI paint code нет произвольных theme-dependent color literals;
- в тестах нет raw `assert`;
- все tests получают одинаковые labels/runtime/timeout правила;
- malformed-input tests не падают и не меняют открытый Document;
- sanitizer/static-analysis/fuzz конфигурация документирована и
  воспроизводима.

## Non-goals

- Не добавлять новые CAD-инструменты или пользовательские функции.
- Не расширять сборки, drawing, макросы или формат изделия.
- Не выполнять визуальный редизайн интерфейса.
- Не вводить формат `.solidar` v3 без доказанной необходимости, отдельной
  migration specification и тестов совместимости.
- Не заменять persistent topology индексами обхода.
- Не переписывать весь Sketch solver без отдельного design review.
- Не добавлять сторонние зависимости только ради уменьшения объёма кода.
- Не использовать сокращение LOC или число новых классов как критерий успеха.
- Не выполнять commit/push/release.

## Запрещённые псевдоисправления

- `if (!ptr) return` без восстановления нарушенного invariant;
- fallback повреждённого v2 в legacy;
- catch без диагностики и без теста на исходный failure path;
- silent skip неизвестной Feature или битой ссылки;
- удаление/ослабление failing test;
- увеличение tolerance только ради прохождения теста;
- отключение preview, selection, Undo/Redo или persistence;
- фоновый OCCT/OpenGL-код без доказанного lifetime и cancellation;
- сохранение одновременно старого и нового authoritative state без явного
  срока удаления legacy path.

## Regression requirements

Минимальный обязательный набор новых сценариев:

1. v1 load и v2 round-trip.
2. Corrupt v2: отсутствующий model, duplicate ID, zero ID, unknown Feature,
   invalid enum, NaN/Inf/range, broken reference, truncated BRep и oversized
   arrays.
3. Failed load не меняет Document, active tool, project path, modified flag и
   не допускает случайного overwrite исходного файла.
4. Multi-body cascade: JoinBodies, NewBody pattern и sketch-on-face через два и
   более Body.
5. Reordered body serialization и dependency cycle.
6. OCCT exception/invalid shape для Move, Mirror, Patterns, mesh и STL.
7. Persistent references после upstream parameter edit, save/load и
   ambiguity.
8. Picking при occlusion, overlapping faces/edges, marquee и более 32
   highlights.
9. Preview cancellation/stale result после drag, tool switch и project switch.
10. Sketch constraint delete/undo/redo/save/load и independent component solve.
11. Part Design Undo/Redo для всех существующих инструментов.
12. Light/dark/system smoke для SketchCanvas и Viewport.

Каждый подтверждённый дефект должен иметь тест, падающий до исправления и
проходящий после него. Если тест нельзя безопасно сделать красным в текущем
дереве, reviewer должен получить доказательство исходного failure path.

## Проверка каждого этапа

Сначала запускать узкие тесты изменённой подсистемы. Затем обязательный полный
gate в MSVC developer environment:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<repo>\vcpkg
set VCPKG_DEFAULT_TRIPLET=ci-x64-windows
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset ci
cmake --build --preset ci --clean-first
ctest --preset ci --output-on-failure
```

`--clean-first` обязателен после изменения любого header. Для полностью
source-only этапа можно использовать обычный incremental build после того, как
Build Engineer подтвердит отсутствие ABI/header риска.

После успешного `ci` запустить `.opencode/scripts/verify-and-run.ps1` для
configure/build/CTest/launch smoke на `dev`. Verifier только сообщает ошибки и
не исправляет продуктовый код.

Финальный ручной smoke:

1. создать Sketch с constraints;
2. выполнить Extrude и Pocket;
3. добавить Fillet/Chamfer и один Pattern;
4. изменить ранний параметр истории и recompute;
5. Undo/Redo, history scrub, save, close, reopen и продолжить редактирование;
6. переключить проект при активном preview и проверить teardown;
7. импортировать STEP и проверить hover/marquee;
8. экспортировать STEP/STL;
9. открыть повреждённый v2 и убедиться, что текущий проект не изменился;
10. повторить ключевой UI-smoke в light и dark theme.

## Definition of Done

Задача завершена только когда:

- исправлены все три release-blocking дефекта: v2 fallback, небезопасная
  десериализация и неполный multi-body cascade;
- ни одно исключение из project/OCCT boundaries не выходит в Qt event loop;
- dependency graph имеет cycle diagnostics и детерминированный rebuild order;
- clean recompute и topology resolution не выполняют лишние глобальные обходы;
- mouse-move picking не содержит полного вложенного перебора всей mesh;
- preview coalesced/cancellable и не публикует stale result;
- solver и Undo/Redo имеют документированное улучшение времени/памяти;
- `Document` является единственным committed source of truth;
- persistence расширяется через registry/factory, а не зеркальные центральные
  type chains;
- model API не протаскивает Qt UI types;
- theme-dependent цвета централизованы;
- raw `assert` удалены из тестов, test registration централизована;
- все старые и новые тесты проходят на `ci`;
- clean build после header changes и GUI smoke успешны;
- performance report содержит baseline/after на одинаковых сценариях;
- пользовательские/unrelated файлы не изменены;
- commit и push не выполнялись.

## Формат итогового отчёта

Для каждого этапа указать:

1. исправленный пользовательский риск и восстановленный invariant;
2. implementation contract и фактически выбранное решение;
3. изменённые файлы;
4. добавленные regression/benchmark tests;
5. результаты targeted tests, полного `ci` и GUI smoke;
6. baseline/after для времени, количества rebuild и памяти;
7. результаты независимого review;
8. известные ограничения и следующий безопасный шаг.

Отдельно перечислить любые расхождения с этим заданием. Не объявлять этап или
всю задачу завершёнными при красном gate, непроверенном header rebuild,
неустранённом HIGH замечании reviewer или недоказанной совместимости v1/v2.
