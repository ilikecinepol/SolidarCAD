# Задача для Codex — компоненты и последующие сборки SolidarCAD

Дата: 18 октября 2026 г.

Статус: зонтичная программа работ. При первом запуске разрешён только этап
**A0a**. Номера будущих форматов и названия новых production types не считаются
утверждёнными до ADR D1.

## Как выполнять задание

Не начинай с изменения кода. Сначала прочитай `AGENTS.md`, актуальные
`docs/roadmap.md`, `docs/architecture.md`, `docs/persistent-topology.md`,
`docs/project-format-v3.md`, `docs/testing.md` и релевантные инструкции:

- `.opencode/skills/solidcad-architecture/SKILL.md`;
- `.opencode/skills/safe-change/SKILL.md`;
- `.opencode/skills/regression-testing/SKILL.md`;
- `.opencode/skills/persistent-topology/SKILL.md`;
- `.opencode/skills/occt-safety/SKILL.md`;
- `.opencode/skills/qt-safety/SKILL.md`;
- `.opencode/skills/build-and-run/SKILL.md`.

Для каждого этапа `solidcad-lead` сначала формирует отдельный bounded
implementation contract. Обязательные независимые read-only исследования:

- `solidcad-explorer` — фактическая карта затронутого кода, ownership, IDs,
  persistence, cache и lifecycle;
- `solidcad-regression-designer` — минимальные deterministic regressions,
  hand-authored fixtures и отрицательные случаи.

Профильных аудиторов подключать только по реальному scope этапа:

- `solidcad-occt-auditor` — B-Rep sharing, OCCT locations/transforms,
  topology resolution, mates или STEP/XDE;
- `solidcad-qt-auditor` — QObject ownership, re-entrancy, tool teardown,
  project replacement, Viewport/QOpenGLWidget или UI lifecycle;
- `solidcad-build-engineer` — CMake/toolchain/dependencies/build scripts,
  расхождение local/CI либо диагностика build failure. В этой программе он
  также является read-only runner обязательного post-review `ci` gate, потому
  что lead не имеет shell permission, implementer ограничен targeted checks, а
  local verifier может запускать только штатный dev-скрипт. Обычный header
  change сам по себе не разрешает build engineer менять scripts.

Продуктовый код этапа изменяет один `solidcad-implementer`. Затем выполняется
независимый `solidcad-reviewer`. BLOCKER/HIGH допускает один точечный цикл
исправления тем же implementer и повторный review. После PASS выполняются
финальные gates из раздела «Проверка».

После каждого этапа остановись, создай отчёт с доказательствами и запроси
явное разрешение на следующий этап. Не объединяй соседние этапы в один diff,
даже если их удобно реализовать вместе.

## Исходное состояние

SolidarCAD — C++20, Qt 6.8.3, OpenCASCADE 8.0.1, CMake + vcpkg.

- `Document` моделирует одну деталь, владеет Sketch/ReferenceImage и несколькими
  `Body`.
- `Body` — линейная Part Design history, а не компонент.
- `Document::rebuild()` строит зависимости Feature/Sketch, поддерживает
  cross-body dependencies и локальные стабильные IDs.
- `ShapeFeature` хранит transient B-Rep, shape revision и topology index.
- Canonical writer пишет `.solidar` v3, но `ProjectLoadKind::ValidV2` смешивает
  успешность загрузки, поколение параметрической модели и реальную версию
  файла.
- `MainWindow` и application boundary ориентированы на один `Document`.
- `Viewport::setBodyShapes()` показывает несколько Body, но presentation,
  picking и cache identity не имеют occurrence scope или rigid placement.
- `MoveFeature` преобразует/копирует B-Rep внутри истории детали. Это не
  сборочное размещение.
- Persistent topology v1 имеет документированные ограничения split/merge,
  symmetry и pattern-generated geometry.
- Roadmap ставит product structure/occurrences/mates после стабилизации
  dependency graph и persistent topology v2.

Перед каждым этапом повторно проверь `git status --short`. На момент подготовки
задания существовали пользовательские untracked пути
`rectangle-dimension-ui-wmRcqj/` и `Новый проект.solidar`. Не изменяй, не
удаляй и не добавляй в индекс их или другие unrelated файлы.

## Пользовательский результат всей программы

1. Существующая параметрическая деталь используется как определение компонента.
2. Одна definition вставляется в сборку несколькими независимыми occurrences.
3. Occurrences имеют ручное rigid-размещение, visibility, suppression и
   grounding.
4. Затем добавляются linked-компоненты и подсборки.
5. После persistent topology v2 добавляются устойчивые сопряжения.
6. Save/load, Undo/Redo, viewport selection и export сохраняют структуру, а
   B-Rep остаётся производным transient state.

Сквозной сценарий после всей программы:

1. открыть существующую v3-деталь как component source;
2. вставить её дважды в новую сборку;
3. независимо разместить и выбирать оба экземпляра;
4. изменить параметр общей definition и обновить оба occurrence без смены их
   IDs и placements;
5. зафиксировать один occurrence и наложить сопряжение на второй;
6. сохранить, открыть заново и продолжить редактирование;
7. после topology change либо корректно re-resolve mate, либо получить
   детерминированный `Broken`, но не молча привязаться к другой грани.

Если ADR D1 утвердит новый standalone component envelope, сценарий дополнится
явной миграцией v3 в next component schema. До ADR нельзя заранее называть её
v4. Аналогично next assembly schema нельзя заранее называть v5.

## Архитектурная цель после ADR D1

Подсборка должна быть тем же видом definition, на который способен ссылаться
occurrence. Целевая форма модели:

```text
ProductDocument
├── ProjectId                         # после persistence-этапа
├── DefinitionRegistry
│   ├── PartDefinition : DefinitionId
│   │   └── существующий Document
│   └── AssemblyDefinition : DefinitionId
│       └── ComponentOccurrence[] -> DefinitionId
└── RootDefinitionId

ComponentOccurrence
├── OccurrenceId
├── target DefinitionId               # part или subassembly
├── authored RigidTransform3d
└── visibility / suppression / grounded
```

Ранний static-assembly этап использует только root `AssemblyDefinition`,
которая ссылается на `PartDefinition`, и ограничивает `OccurrencePath` глубиной
1. Ссылки на `AssemblyDefinition`, nesting и cycle detection включаются только
на A6c. Не добавляй фиктивную recursive validation в плоский этап.

Конкретные имена `ProductDocument`, `PartDefinition` и `DefinitionRegistry`
могут быть изменены только принятым ADR; семантические границы обязательны.

### Пространства идентичности

Не смешивать следующие значения:

- `ProjectId` — identity сохранённого product/file document;
- `DefinitionId` — стабильный project-local ID записи definition;
- `OccurrenceId` — стабильный ID экземпляра внутри assembly definition;
- `OccurrencePath` — последовательность occurrence IDs от root;
- `ExternalSourceKey` — identity linked source по принятому D1 contract. Для
  identity-bearing envelope это обычно `{ProjectId, DefinitionId}`. Если v3
  остаётся source без такого envelope, D1 обязан выбрать честный assembly-owned
  binding ID либо отложить links до явной identity-bearing schema; скрытый
  sidecar и притворно глобальный path-ID запрещены;
- `SourceRevision`/content hash — версия содержимого, но не identity;
- path — только relocatable locator hint.

Для schema, содержащей `ProjectId`, обычный Save в тот же target сохраняет его,
Save As по умолчанию создаёт новый, а явная будущая команда Relocate/Move может
сохранить. Project-local `DefinitionId` может остаться тем же, потому что scoped
новым `ProjectId`. Legacy v3 без identity-bearing envelope не должен заявлять
эти гарантии. Два источника с одинаковым `ExternalSourceKey`, но разным
`SourceRevision`/hash, являются `IdentityConflict`; resolver не выбирает один
молча.

## Неподвижные инварианты

1. `Body` не переименовывается и не используется как component definition.
2. `MoveFeature`, Mirror/Pattern occurrences и `JoinBodiesFeature` не подменяют
   assembly occurrence или mate.
3. Part Design boolean/dependency не пересекает definition boundary. Сборка —
   композиция, а не boolean history.
4. Derived/transformed B-Rep, mesh и GPU buffers не сериализуются как источник
   истины. Существующий authoritative archive `ImportedShapeFeature` остаётся
   отдельным явным исключением.
5. Selection/mate не хранит raw pointer, `TopoDS_Face`, OCCT ordinal, tree
   index или flattened face/edge offset как persistent identity.
6. Visibility — presentation state. Suppression исключает occurrence из
   solve/export/BOM semantics. Grounded — единственное authoritative boolean
   property occurrence: ручное перемещение запрещено, а будущий solver считает
   его 6 DOF фиксированными. `Ground/Fix` не дублируется записью mate registry.
7. Failed load/reload/solve/preview не меняет committed product, ID cursors,
   Undo stack, selection или последнюю валидную scene publication.
8. Definition edit обновляет все occurrences. Duplicate occurrence не копирует
   `Document`/B-Rep. «Сделать независимым» — отдельная будущая clone/remap
   операция.
9. Placement-only edit не вызывает `Document` recompute или OCCT meshing.
10. B-Rep и tessellation разделяются по source mesh key, а не по occurrence.
    Один CPU/GPU source разрешён на уникальный definition-scoped key,
    включающий как минимум non-persistent product/snapshot epoch, definition,
    Body/Feature, shape revision/identity и meshing quality/parameters. Epoch
    запрещает reuse между двумя открытыми проектами с совпавшими локальными
    IDs/revisions, но разной геометрией. Multi-Body definition законно имеет
    несколько meshes/uploads; N occurrences не умножают их число.
11. Смена проекта соблюдает порядок:

```text
invalidate queued preview
-> cancel active sessions while old sources are alive
-> clear manipulators, hover, selection and callbacks
-> atomically replace committed product state
-> publish/reset viewport scene
-> rebuild tree, properties and diagnostics
```

12. OCCT rebuild/preview не распараллеливается без отдельного доказанного
    thread-safety/lifetime design.
13. UI не владеет authoritative assembly state; renderer/picking получают
    immutable scene snapshots.

## Запрещённые псевдорешения

- хранить component/occurrence ID только в UI item;
- клонировать или transform-ить B-Rep на occurrence;
- использовать `MoveFeature` как assembly placement;
- делать Body/Feature IDs глобальными через remap при вставке;
- сохранять flattened compound и пытаться восстановить из него assembly;
- разрешать mate через raw face/edge index при наличии persistent reference;
- молча пропускать missing/outdated/cyclic/conflicting linked source;
- частично публиковать placements после solver failure;
- наращивать assembly handlers в `MainWindow.cpp` или assembly rendering в
  `Viewport::paintGL` вместо controller/scene boundaries;
- ослаблять validation, удалять failing test или генерировать все migration
  fixtures тем же writer, который они должны проверять.

## Этапы поставки

### A0a — точная таксономия load result

Это единственный этап, разрешённый при первом запуске.

Scope:

1. Построить карту `ProjectFile::create/save/saveDocument/load/loadDocument/
   stageLoad/validate`, `ProjectApplicationService::stageOpen` и test/fuzz
   callers.
2. Разделить в результате загрузки:
   - `Valid/Invalid/Unsupported`;
   - фактический `sourceVersion`;
   - фактический materialized payload kind.
3. Сохранить compatibility facade для текущих part-only callers без silent
   downgrade будущего неизвестного project kind.
4. Не перемещать encoder/decoder functions, не менять JSON/version/UI и не
   выполнять общий cleanup `ProjectFile.cpp`.
5. Добавить hand-authored v1/v2/v3 и malformed/unsupported fixtures.

Exit checklist:

- [ ] v1/v2/v3 сообщают точный `sourceVersion` и materialized kind;
- [ ] malformed и unsupported различимы;
- [ ] rejected load не меняет destination `Document` и ID generators;
- [ ] current writer выдаёт тот же v3 semantic payload;
- [ ] diff не содержит component/assembly types или codec code motion;
- [ ] targeted tests, clean `ci`, verifier и launch smoke зелёные;
- [ ] reviewer не оставил HIGH; опубликован отчёт A0a.

### A0b — узкий encoder seam

Начинать только после принятого A0a.

Scope: выделить только запись текущего part payload в узкий internal API.
Decoder остаётся на месте. Feature codec registry остаётся единственным
authoritative списком. Если read-only map показывает, что extraction потребует
переноса decoder или дублирования registry, остановиться и предложить меньший
seam вместо массового code motion.

Exit checklist:

- [ ] on-disk v3 semantic fields, IDs и порядок semantic arrays/history не
  изменены; JSON object-key order и byte ordering не становятся contract;
- [ ] oracle сравнивает persisted semantics, rebuilt validity/bounds/volume и
  topology-resolution outcomes с tolerance, но не B-Rep bytes/pointers;
- [ ] decoder production diff отсутствует;
- [ ] negative/resource-limit regressions сохранены;
- [ ] clean `ci`, verifier, smoke, review и отчёт A0b зелёные.

### A0c1 — decode/validation seam без materialization

Начинать только после принятого A0b.

Scope: выделить разбор текущего part payload и structural validation в узкий
internal API/DTO. Materialization `Document`, recompute и application commit
остаются на месте. Сохранить resource limits и fail-closed errors. Если для
этого нужен массовый перенос materialization либо второй registry, остановиться
и добавить только internal facade, не расширяя diff.

Exit checklist:

- [ ] один encoder/decoder registry, без второго feature-type switch;
- [ ] hand-authored v1/v2/v3 и fuzz corpus дают прежние parsed/error results;
- [ ] structural limits и malformed/unsupported classification сохранены;
- [ ] materialization/recompute production code не переносился;
- [ ] текущий application по-прежнему работает с одним `Document`;
- [ ] clean `ci`, verifier, smoke, review и отчёт A0c1 зелёные.

### A0c2 — staged materialization seam

Начинать только после принятого A0c1.

Scope: отделить materialize/recompute текущего part DTO, сохранив speculative ID
reservation pause и атомарный commit. Product envelope и новая schema не входят
в scope. Если explorer map показывает массовый cross-subsystem code motion,
остановиться и предложить более узкий facade/дополнительный микрошаг.

Exit checklist:

- [ ] late materialize/rebuild failure ничего не публикует и не двигает cursors;
- [ ] v1/v2/v3 semantic IDs, topology refs и geometry oracle не изменены;
- [ ] decode и materialization не дублируют validation/registry;
- [ ] текущий application по-прежнему владеет одним `Document`;
- [ ] clean `ci`, verifier, smoke, review и отчёт A0c2 зелёные.

### D1 — обязательный ADR и roadmap gate

Это отдельный read-only/design deliverable после A0c2 и до любых product types.
Lead обязан:

1. Сверить фактическое завершение dependency-graph milestone и persistent
   topology v2 с roadmap, отчётами и зелёными tests. При расхождении сообщить
   blocker и не начинать A1.
2. Подготовить ADR с двумя честными вариантами:
   - существующий v3 `Document` остаётся standalone component source, а новая
     schema впервые появляется у assembly;
   - вводится unified product/component envelope с явной миграцией v3.
3. Для каждого варианта оценить пользовательскую ценность, migration cost,
   linked identity, Save/Save As/Relocate, backward compatibility, число format
   bumps и влияние на application ownership.
4. Развести `ProjectId`, `DefinitionId`, `OccurrenceId`, `ExternalSourceKey` и
   `SourceRevision`; отдельно определить identity legacy v3 source без скрытого
   sidecar и утвердить collision policy.
5. Утвердить schema numbering отдельно от `kind=part|assembly`.

Exit checklist:

- [ ] roadmap prerequisites подтверждены доказательствами;
- [ ] ADR выбирает ровно один вариант, без гибридной скрытой миграции;
- [ ] identity/copy/relocation/conflict policy однозначна;
- [ ] пользователь явно принял ADR и разрешил A1a;
- [ ] production code в D1 не менялся.

### A1a — headless definition ownership model

Добавить Qt-free model foundation: typed project-local `DefinitionId`, registry,
root ID и `PartDefinition`, которая единолично владеет существующим `Document`.
На этом этапе foundation не инстанцируется production application и не создаёт
второй authoritative state. `AssemblyDefinition`, occurrence, transform,
persistence UUID и Undo delta не добавлять.

Exit checklist:

- [ ] ownership выражен `unique`-семантикой; B-Rep/Document не копируются;
- [ ] duplicate/missing/root ID validation детерминирована;
- [ ] public model headers Qt-free;
- [ ] новые types пока не включены в production New/Open/Save path;
- [ ] model tests, clean `ci`, verifier, smoke, review и отчёт A1a зелёные.

### A1b1 — staged application carrier seam

Ввести carrier из A1a только на границе `ProjectApplicationService` и staged
result. До commit carrier единолично владеет root `Document`; существующий
editor commit может извлечь его move-операцией. `MainWindow` ownership и
прямые editor callers пока не мигрировать. Никакого одновременного второго
owner или shadow metadata.

Exit checklist:

- [ ] staged result имеет один owning path до `Document`;
- [ ] commit выполняет move, а rejected stage ничего не публикует;
- [ ] `MainWindow` production ownership в этом этапе не менялось;
- [ ] v1-v3 save/load и весь part UI семантически неизменны;
- [ ] service/lifecycle tests, clean `ci`, verifier, smoke, review и отчёт A1b1
  зелёные.

### A1b2 — editor owner migration

После принятого A1b1 перевести committed editor state на один root carrier.
Старые `document()` и part-only entry points могут временно возвращать только
non-owning view на root `Document`; отдельный owner, proxy-document, второй Undo
history или shadow metadata запрещены.

Перед implementation explorer обязан посчитать прямые `MainWindow::document_`
callers и затронутые tool/session/history boundaries. Если миграция не
помещается в bounded editor-owner contract без массового изменения независимых
tools, остановиться и предложить дополнительные механические микрошаги; не
создавать временный второй authoritative state.

Exit checklist:

- [ ] в committed editor state ровно один owning path до `Document`;
- [ ] New/Open/Close/failed Open соблюдают прежнюю атомарность и teardown;
- [ ] compatibility views доказаны non-owning и не переживают carrier;
- [ ] v1-v3 save/load, history, active sessions и part UI не изменили semantics;
- [ ] lifecycle/UI tests, clean `ci`, verifier, smoke, review и отчёт A1b2
  зелёные.

### A2 — утверждённый ADR component-source/persistence contract

Реализовать только вариант, принятый в D1.

Если v3 остаётся component source, не создавать искусственный component format:
использовать codec A0b/A0c2, формализовать source descriptor и доказать, что
embedded assembly definition может хранить part payload без повторного writer.
Legacy v3 не получает скрытый `ProjectId`. Реализовать ровно тот link-identity
вариант, который D1 выбрал для legacy source: assembly-owned binding либо
явное откладывание linked mode до identity-bearing schema.

Если принят unified envelope, добавить next component schema с `ProjectId`,
root `DefinitionId`, metadata и payload discriminator. v1-v3 мигрировать в
памяти без remap локальных IDs; исходный файл не переписывать до явного Save.
Save сохраняет identity, Save As создаёт новый `ProjectId`, явный Relocate
сохраняет его.

Для новой schema обязателен strict exact-field contract: неизвестная более
новая version/capability — `Unsupported`; extra/malformed field в заявленной
поддерживаемой schema — `Invalid`. Нельзя принять поле, отбросить и resave.

Общий exit checklist:

- [ ] реализована только выбранная ADR-ветка;
- [ ] staging несёт всю metadata до application commit без shadow state;
- [ ] legacy IDs/topology refs не remap-ятся;
- [ ] B-Rep не попал в JSON; rebuild идёт из параметров;
- [ ] property/fuzz/migration/lifecycle tests, clean `ci`, verifier, review и
  отчёт A2 зелёные.

Дополнительно для ветки «v3 остаётся source»:

- [ ] v3 bytes/semantic schema и обычные Save/Save As не получили выдуманный
  `ProjectId` или sidecar;
- [ ] embedded part payload использует один codec;
- [ ] D1 legacy binding policy проверена либо linked mode явно заблокирован до
  отдельной identity-bearing schema.

Дополнительно для ветки unified envelope:

- [ ] exact-field/capability/UUID validation покрыта hand-authored fixtures;
- [ ] Save сохраняет `ProjectId`, Save As создаёт новый, Relocate сохраняет;
- [ ] same-key/different-revision collision отклоняется детерминированно.

### A3a — headless flat static-assembly domain

Добавить validated `RigidTransform3d`, `AssemblyDefinition`, `OccurrenceId`,
`OccurrencePath` и flat occurrences. В этом этапе occurrence может ссылаться
только на `PartDefinition`, path depth равна 1, nesting/cycles отсутствуют.
Добавить authored placement, visibility, suppression и authoritative grounded
property. Никаких persistence, UI или mates.

Утвердить orphan policy до codec A3b: удаление последнего occurrence не удаляет
definition. Unreferenced definitions остаются валидными registry entries и
сериализуются детерминированно; их очистка возможна только будущей явной
`RemoveUnusedDefinition` transaction, не входящей в эту программу.

Exit checklist:

- [ ] finite rigid transform запрещает scale/shear/reflection и имеет
  deterministic canonicalization;
- [ ] два occurrences разделяют одну `PartDefinition`, но имеют разные IDs,
  flags и placements;
- [ ] duplicate/dangling IDs, invalid transforms и depth > 1 отклоняются
  атомарно;
- [ ] unreferenced definition валидна и не удаляется implicit mutation;
- [ ] placement/flags не меняют `Document` semantic fingerprint;
- [ ] headless tests, clean `ci`, verifier, review и отчёт A3a зелёные.

### A3b — flat assembly codec и next assembly schema

Добавить schema для embedded part definitions и flat occurrences, используя
codec A0 без дублирования feature/sketch logic. Decode выполняется минимум в
два прохода: ID/refs/limits, затем materialize/recompute. Ввести aggregate
limits по definitions, occurrences, строкам, bytes и rebuild work. Writer не
создаёт поля mates/nesting; непустое неизвестное semantic content не игнорирует.
Hand-authored fixture с более новой version либо явно заявленной неизвестной
capability/discriminator даёт `Unsupported`. Extra/malformed `mates` или
`nesting` внутри заявленной flat schema даёт `Invalid`.

Exit checklist:

- [ ] definition payload записан один раз для нескольких occurrences;
- [ ] save/load сохраняет IDs/order/flags/transforms и sharing;
- [ ] unreferenced registry definition round-trip сохраняется до явной future
  cleanup transaction;
- [ ] invalid/dangling/duplicate/oversized input отклоняется до commit;
- [ ] `mates`/`nesting`/unknown capability fixtures дают точно заданный
  `Invalid`/`Unsupported`, никогда silent drop;
- [ ] late recompute failure отклоняет весь staged product;
- [ ] derived occurrence B-Rep/mesh отсутствует в JSON;
- [ ] persistence/fuzz tests, clean `ci`, verifier, review и отчёт A3b зелёные.

### A3c — assembly-aware service staging без editor commit

Расширить `ProjectApplicationService` только до возврата validated staged
product variant. Part-only API явно отклоняет assembly. Реальный commit assembly
в `MainWindow` откладывается до read-only workspace A5a: editor Open на этом
этапе сообщает «тип проекта пока не поддержан редактором» и сохраняет текущий
part state без изменений. Tree, ribbon и viewport assembly не показывают.

Exit checklist:

- [ ] service успешно возвращает полностью validated staged assembly;
- [ ] failed stage не меняет ID cursors и текущий application state;
- [ ] попытка editor Open assembly не меняет active part/path/dirty/history,
  selection или sessions;
- [ ] silent downgrade assembly до root part невозможен;
- [ ] assembly не становится active editor project до A5a;
- [ ] application-service/lifecycle tests, clean `ci`, verifier, review и
  отчёт A3c зелёные.

### A4a1 — pure immutable presentation builder

Создать UI-free pure `AssemblyPresentationBuilder`, который синхронно строит
immutable scene snapshot с разделёнными source mesh key и occurrence
presentation identity. Он не владеет publication generation, callbacks,
event-loop lifetime, renderer или UI.

Exit checklist:

- [ ] snapshot содержит product epoch, occurrence path, source key, world
  transform и flags;
- [ ] invalid source возвращает failure без partial snapshot;
- [ ] одинаковый input детерминированно даёт семантически тот же snapshot;
- [ ] builder tests, clean `ci`, verifier, review и отчёт A4a1 зелёные.

### A4a2 — scene publication coordinator

Добавить отдельный coordinator, атомарно публикующий только complete A4a1
snapshot. Он владеет generation token и lifetime rules: re-entrant consumer и
delayed callback старой сцены не могут наблюдать или вернуть stale state.
Renderer, picking, tree и пользовательские commands ещё не менять.

Exit checklist:

- [ ] publication меняет active snapshot одной commit-операцией;
- [ ] failed build сохраняет прежнюю valid publication;
- [ ] stale generation/delayed callback детерминированно игнорируется;
- [ ] coordinator tests, clean `ci`, verifier, review и отчёт A4a2 зелёные.

### A4b — renderer instancing и cache lifetime

Научить renderer принимать A4a2 snapshots и рисовать instance transforms.
Вынести код из монолитного `Viewport::paintGL`, не добавляя assembly UI.

Exit checklist:

- [ ] N occurrences используют по одному CPU/GPU resource на уникальный source
  mesh key, а не один на всю multi-Body definition;
- [ ] два проекта с одинаковыми local IDs/revisions, но разной geometry и
  product epoch не переиспользуют старый CPU/GPU source;
- [ ] placement-only update не tessellate/upload source mesh заново;
- [ ] independent occurrence highlight не загрязняет shared source;
- [ ] last-user pruning и context teardown не оставляют stale GPU state;
- [ ] renderer tests, real-GPU smoke, clean `ci`, verifier, review и отчёт A4b
  зелёные.

### A4c1 — transform-aware bounds и Fit

Добавить world bounds occurrence instances и Fit-to-view. Picking, rectangle,
snap и ruler не менять.

Exit checklist:

- [ ] transforms корректно входят в aggregate bounds;
- [ ] hidden/suppressed исключены из bounds по утверждённой semantics;
- [ ] placement update меняет bounds/Fit, но не source mesh;
- [ ] bounds/Fit tests, GPU smoke, clean `ci`, verifier, review и отчёт A4c1
  зелёные.

### A4c2 — ray picking, occlusion и instance highlight

Добавить face/edge ray picking с frontmost occlusion. Hit возвращает
`OccurrencePath` плюс локальную body/feature/topology identity; flattened
global ordinal запрещён. Highlight адресуется occurrence, а не shared mesh.

Exit checklist:

- [ ] одинаковая local face двух occurrences различается по path;
- [ ] overlapping instances выбирают frontmost occurrence;
- [ ] highlight одного occurrence не подсвечивает другой;
- [ ] hidden/suppressed не участвуют в picking;
- [ ] ray/occlusion tests, GPU smoke, clean `ci`, verifier, review и отчёт A4c2
  зелёные.

### A4c3 — rectangle selection, snap и ruler

Отдельно перевести remaining spatial consumers на occurrence-scoped identities
и world transforms.

Exit checklist:

- [ ] rectangle selection возвращает occurrence-scoped keys;
- [ ] snap/ruler world coordinates согласованы с transformed render;
- [ ] placement update инвалидирует только spatial state;
- [ ] hidden/suppressed не участвуют в rectangle/snap/ruler;
- [ ] spatial tests, GPU smoke, clean `ci`, verifier, review и отчёт A4c3
  зелёные.

### A5a — read-only assembly workspace

Добавить typed tree model, read-only assembly workspace/ribbon state и
occurrence selection поверх A3/A4. Не добавлять команды модификации. В assembly
mode Part Design timeline скрыт; authoritative model остаётся вне UI. Это
первый этап, где validated staged assembly из A3c атомарно коммитится как active
editor project; до готовности complete read-only scene commit запрещён.

Exit checklist:

- [ ] assembly открывается и отображает definitions/occurrences без mutation;
- [ ] first editor commit assembly выполняет полный teardown старого part и
  публикует complete read-only workspace атомарно;
- [ ] tree↔viewport selection использует стабильные IDs;
- [ ] part↔assembly project switch очищает stale state;
- [ ] UI smoke/DPI, clean `ci`, verifier, review и отчёт A5a зелёные.

### A5b — Insert и placement transaction

Добавить только New Assembly, Insert embedded component, ghost placement,
move/rotate и Apply/Cancel как одну typed transaction. Один Escape отменяет
nested selection, второй — tool. Undo/Redo покрывает только этот завершённый
contract.

Exit checklist:

- [ ] Apply создаёт ровно одну undo command; Cancel восстанавливает точное
  исходное state;
- [ ] preview не меняет committed model и безопасно переживает failed Open;
- [ ] controller `cancelActive()` и session `cancel()` вызываются до замены
  source;
- [ ] tool/history/UI smoke, clean `ci`, verifier, review и отчёт A5b зелёные.

### A5c0 — occurrence command/delta foundation

Добавить typed command boundary и bounded cold model deltas без новых
пользовательских commands. History не хранит B-Rep/GPU snapshots. Failure до
commit не меняет model или history depth.

Exit checklist:

- [ ] apply/revert пустого test command детерминированы и атомарны;
- [ ] delta адресует stable IDs и не владеет transient geometry;
- [ ] failed apply не создаёт history entry;
- [ ] history foundation tests, clean `ci`, verifier, review и отчёт A5c0
  зелёные.

### A5c1–A5c6 — occurrence commands микрошагами

Каждая строка ниже — отдельная поставка с отдельным разрешением, contract,
review и отчётом; не объединять их в один diff:

1. A5c1 — rename;
2. A5c2 — visibility + temporary isolate presentation;
3. A5c3 — ground/un-ground;
4. A5c4 — suppression/unsuppression;
5. A5c5 — duplicate: новый `OccurrenceId` ссылается на тот же `DefinitionId`,
   не клонируя `Document`/B-Rep;
6. A5c6 — remove/undo: удаление последнего occurrence сохраняет orphan
   definition согласно A3a policy.

#### A5c1 — Rename occurrence

Rename хранит authored UTF-8 имя на occurrence, не меняя definition, IDs,
placement, flags, `Document` или B-Rep. Empty legacy/new name отображается как
детерминированный `Экземпляр <OccurrenceId>`. Apply/Undo/Redo используют typed
delta с exact before/after guard; successor перестраивает только immutable tree
и сохраняет viewport publication/epoch/GPU cache. V6 encoder fail-closed для
непустого authored name, пока schema явно не расширена.

A5c1 exit checklist:

- [x] strict UTF-8 validation и exact model Apply/Revert покрыты focused tests;
- [x] history, Insert→Rename chain, resource/tree failure rollback покрыты;
- [x] ribbon/context/F2, modal lifecycle и replacement cancellation покрыты;
- [x] tree-only successor сохраняет session/publication/epoch/selection и не
  вызывает geometry upload;
- [x] clean `ci`/`dev` focused builds и tests завершены;
- [ ] native GPU evidence, verifier/review и отдельный отчёт завершены.

#### A5c2 — Visibility + temporary isolate

Visibility меняет только persistent authored `visible` bit через typed delta и
отдельный same-epoch visibility successor. Shared definition sources
переиспользуются без remesh/upload, пока остаётся видимый пользователь;
последний Hide может законно prune-ить ресурс, а Show — reacquire его.
Temporary isolate фильтрует immutable scene/bounds/spatial/picking внутри той
же session/publication, не меняет ProductDocument/history/dirty/persistence и
никогда не раскрывает authored hidden/suppressed/body-hidden геометрию.

A5c2 implementer checklist:

- [x] exact visibility model delta, rollback и unified history покрыты;
- [x] same-epoch fresh visibility publication и cache reuse реализованы;
- [x] presentation-only isolate/show-all сохраняет base sources/publication;
- [x] ribbon/context/Space/Escape и semantic tree states реализованы;
- [x] focused model/session/tree/UI/classifier/headless tests добавлены;
- [x] clean `ci`/`dev` focused builds/tests завершены;
- [ ] native GPU, review и отдельный отчёт завершены.

#### A5c3 — Ground / un-ground occurrence

Ground меняет только persistent authored `grounded` bit через typed delta.
Grounded occurrence отклоняет committed placement, а reentrant placement во
время occurrence transaction получает `Busy`. Same-epoch grounding successor
готовит fresh publication/tree до commit и переиспользует mesh/topology/GPU
sources без изменения transforms, bounds, picking membership или camera.

A5c3 implementer checklist:

- [x] exact grounded-bit Apply/Revert и unified history Undo/Redo реализованы;
- [x] grounded placement guard и transaction Busy покрыты;
- [x] same-epoch fresh grounding publication и zero-remesh reuse реализованы;
- [x] ribbon/context/Ctrl+G, dynamic text и isolate disable реализованы;
- [x] focused model/session/tree/UI/classifier/headless tests добавлены;
- [x] clean `ci`/`dev` focused gates завершены;
- [ ] native GPU, review и отдельный отчёт завершены.

#### A5c4 — Suppress / unsuppress occurrence

Suppress меняет только persistent authored `suppressed` bit через typed delta.
Distinct same-epoch suppression successor готовит fresh publication/tree до
commit, исключает occurrence из bounds/picking/spatial consumers и сохраняет
semantic tree selection без viewport highlight. Shared effective source user
не допускает remesh/upload/prune; последний effective user допускает prune и
последующий reacquire. `Shift+Space` не пересекается с visibility `Space`.

A5c4 implementer checklist:

- [x] exact suppressed-bit Apply/Revert и unified history Undo/Redo реализованы;
- [x] whole-flags transaction Busy и exact rollback покрыты;
- [x] same-epoch fresh suppression publication/spatial exclusion реализованы;
- [x] shared-source reuse и last-user prune/reacquire разделены;
- [x] ribbon/context/Shift+Space, dynamic text и isolate disable реализованы;
- [x] focused model/session/UI/classifier/headless tests добавлены;
- [x] clean `ci`/`dev` focused gates завершены;
- [ ] native GPU, review и отдельный отчёт завершены.

#### A5c5 — Duplicate occurrence

Duplicate — отдельная structural cold command, потому что операция добавляет и
удаляет map node, а не меняет поле существующего occurrence. Новый ID —
минимальный свободный положительный ID в пределах 4096; Undo извлекает только
точно созданный node, Redo восстанавливает тот же ID. Target, placement, flags
и authored name копируются без offset/suffix, а Part `Document`/B-Rep и
definition-local render source остаются общими.

A5c5 implementer checklist:

- [x] cold structural command и shared mutation Busy guard реализованы;
- [x] lowest-free allocator, exact node Apply/Extract и stable-ID Redo покрыты;
- [x] same-epoch fresh duplicate projection и zero-remesh/upload reuse готовы;
- [x] ribbon/context/Ctrl+D, selection Apply/Undo/Redo и isolate disable готовы;
- [x] V6 one-definition/two-occurrence и named fail-closed покрыты;
- [x] focused model/session/UI/classifier/headless tests добавлены;
- [ ] native GPU, review и отдельный отчёт завершены.

#### A5c6 — Remove occurrence / Undo

Remove удаляет только occurrence node и сохраняет definition, её `Document` и
B-Rep. Undo восстанавливает тот же стабильный `OccurrenceId`, placement,
flags и authored name; collision или failure оставляют model/history и
публикацию без изменений. Последний occurrence может быть удалён: пустой
assembly root валиден, а definition остаётся в дереве с orphan-пометкой.

A5c6 implementer checklist:

- [x] cold remove/restore transaction и shared mutation Busy guard реализованы;
- [x] exact node rollback, stable-ID Undo/Redo и collision покрыты;
- [x] same-epoch fresh remove projection и shared-source reuse готовы;
- [x] ribbon/context/tree-scoped Delete и Part Delete non-collision готовы;
- [x] orphan/empty-root V6 roundtrip и tree feedback покрыты;
- [x] focused model/session/UI/classifier/headless tests добавлены;
- [ ] native GPU, review и отдельный отчёт завершены.

Общий exit checklist каждого микрошага:

- [ ] команда меняет только заявленное поле/отношение;
- [ ] Apply/Undo/Redo симметричны; failure не меняет model/history depth;
- [ ] visibility/suppression/grounded semantics не смешаны;
- [ ] scoped command/UI/lifecycle tests, clean `ci`, verifier, review и
  отдельный отчёт зелёные.

### A5d — edit-definition context

Добавить safe same-window context switch с breadcrumb. Multi-window manager не
входит в scope. Изменение definition обновляет все occurrences, сохраняя их IDs
и placements.

Definition edit коммитится только после успешного recompute. При failure вся
edit transaction откатывается: authoritative parameters/history, Undo stack и
все occurrences остаются на предыдущей valid revision/presentation; Save и
export продолжают видеть предыдущий committed state. Committed invalid
definition в этом этапе запрещена.

Exit checklist:

- [x] переход и возврат не создают второго owner/Undo history;
- [x] recompute общей definition публикуется всем occurrences атомарно;
- [x] late recompute failure полностью откатывает edit и не создаёт Undo entry;
- [x] active tool/project switch teardown безопасен;
- [ ] context/lifecycle/UI tests, clean `ci`, verifier, review и отчёт A5d
  зелёные.

### A6a — read-only single-level linked resolver

Начинать только если принятая ветка D1/A2 уже даёт явный
`ExternalSourceKey`. Если legacy-v3 ветка отложила linked identity, сначала
нужны отдельный ADR и identity-bearing schema; path/hash нельзя повысить до
глобальной identity молча.

Добавить явный source descriptor `{ExternalSourceKey, relativePath hint,
SourceRevision/hash}` и staged resolver одного уровня. Никаких reload/repair,
nesting, сети или автоматического disk scan.

Cross-platform path/security policy:

- сначала выполняется platform-neutral lexical normalization `.`/`..` и
  separators отдельно от отображаемого hint; lexical escape из любого
  allowlisted root отклоняется даже для missing path, который ещё нельзя
  filesystem-canonicalize;
- существующий target затем canonicalize-ится с разрешением symlink/reparse
  points и повторно проверяется против allowlisted roots;
- Windows comparison case-insensitive и учитывает drive/UNC/reparse semantics;
  POSIX comparison case-sensitive и учитывает symlink/inode identity;
- default allowlist содержит project root; пользовательские component libraries
  возможны только как явно настроенные дополнительные roots. Absolute/UNC path
  вне allowlist запрещён, но сам формат не запрещает будущую явную library
  policy;
- junction/symlink/reparse loops отслеживаются по canonical target/file
  identity и ограничиваются depth/work budget;
- одинаковый `ExternalSourceKey` с разным hash — `IdentityConflict`, а не
  last-writer-wins.

Exit checklist:

- [ ] valid relative link разрешается без неявной записи source file;
- [ ] missing lexical-safe path остаётся `Missing`, а path escape отклоняется;
- [ ] Windows/POSIX case, symlink/reparse, outdated и identity conflict имеют
  deterministic platform-aware tests/diagnostics;
- [ ] resolver соблюдает byte/file/depth/work budgets;
- [ ] no-network/no-scan доказаны tests;
- [ ] resolver/security tests, clean `ci`, verifier, review и отчёт A6a зелёные.

### A6b0 — strict linked-assembly schema microgate

До lifecycle-команд закрепить отдельный V7 envelope и Qt-free model binding.
V7 сохраняет flat assembly с embedded либо linked PartDefinition; linked
вариант содержит внешний `ExternalSourceKey`, исходный relocatable path hint,
accepted SHA-256 и обязательный параметрический `lastValid` cache. Scanner,
encoder и materializer disconnected и не выполняют resolver/I/O. V6 остаётся
байт-в-байт и schema-совместимым. Этот microgate не включает Open/Save UI,
Reload/Repair/history/export или nesting.

Exit checklist:

- [x] model binding Qt-free и не владеет вторым Document/B-Rep;
- [x] strict flat V7 codec сохраняет все authored occurrence fields;
- [x] linked locator/hash/cache валидируются bounded и fail closed;
- [x] materialization staged, recompute-checked и atomic без I/O;
- [x] focused V6/V7/model/A6a tests и clean-first build зелёные;
- [x] full CI и review A6b0 зелёные; единственный verifier остановился на
  Windows file lock от оставленного smoke-процесса, после точечного завершения
  процесса recovery dev build, 140/140 CTest и отдельный 8-second GUI smoke
  прошли без повторного verifier.

### A6b1 — reload/repair model + cold history microgate

Добавить явные Reload/Repair Link без UI/Open/Save route. Reload staged,
повторно разрешает topology references и атомарно обновляет shared definition
для всех occurrences. Repair меняет только relocatable locator после точной
проверки external identity. Обе команды cold и Undo/Redo не обращаются к FS.

Exit checklist:

- [x] reload failure сохраняет последнюю valid revision/presentation;
- [x] repair не меняет identity/hash/cache молча;
- [x] resolver/lifecycle boundary не имеет write/scan/network capability;
- [x] cold Undo/Redo после missing/changed source не обращается к FS;
- [x] focused lifecycle/topology/link tests зелёные;
- [x] clean-first focused `ci`, independent review и отчёт A6b1 зелёные;
  финальный монолитный verifier не повторялся implementer-ом и остаётся
  ответственностью финального runner-а.

### A6b2 — editor persistence, diagnostics и write policy

Подключить V7 Open/Save/Save As и UI-команды Reload/Repair. Save assembly не
сохраняет грязные component files каскадно без отдельной команды/транзакции.
Missing link может оставаться диагностируемо открытым с last-valid cache, но
блокирует полный export.

Exit checklist:

- [x] Open/Save/Save As V7 сохраняют linked binding/cache атомарно;
- [x] Save As alias metadata/file-identity/canonicalization errors fail-closed;
- [x] dirty linked source не записывается implicit cascade;
- [x] Missing/Outdated diagnostics и Reload/Repair UI lifecycle точны;
- [x] полный export preflight fail-closed для Missing/Outdated; реальные
  assembly STEP/STL writers/actions остаются отложены до A9;
- [ ] UI/lifecycle/export tests, clean `ci`, verifier, review и отчёт A6b2
  зелёные.

### A6c0 — Qt-free nested graph + disconnected V8 microgate

Разрешить occurrence ссылаться на `AssemblyDefinition`, включить recursive
`OccurrencePath`, dependency DAG, cycle detection и aggregate budgets. Это
первый этап, где общая variant-модель подсборок реально активируется.

Exit checklist:

- [x] deterministic world-transform traversal и полный path для нескольких
  уровней, shared DAG разрешён;
- [x] direct/indirect/orphan cycles и central depth/work/leaf/source budgets
  fail closed;
- [x] atomic batch construction поддерживает forward refs, flat incremental
  authoring остаётся fail-closed;
- [x] strict disconnected V8 codec/materializer добавлен без изменения V6/V7;
- [ ] focused clean `ci`, independent review и отчёт A6c0 зелёные.

### A6c1a — recursive presentation/tree/session identity

Подключить recursive presentation/tree/selection/picking и linked lifecycle по
полному occurrence path, не подключая V8 к editor Open/Save.

Exit checklist:

- [x] common graph snapshot содержит intermediate/terminal visits и строится
  один раз на publication;
- [x] presentation/tree/session/picking/link reload используют полный
  occurrence path;
- [x] root-only mutation actions явно остаются depth-1, terminal isolation
  поддержан, intermediate isolation отключён;
- [x] history учитывает path vectors и восстанавливает nested selection;
- [ ] focused clean `ci`, independent review и отчёт A6c1a зелёные.

### A6c1b — V8 editor lifecycle + native nested GPU gate

Подключить V8 Open/Save/Save As через editor/application service, добавить
финальный real-driver multi-DPR nested GPU marker и доказать retirement/
publication lifecycle без изменения V6/V7 маршрутов.

Exit checklist:

- [x] V8 editor Open/Save/Save As атомарны и V6/V7 не регрессировали в
  focused gate;
- [x] V7/V8 сохраняют application-owned linked diagnostics на `lastValid`,
  missing-leaf alias проверяется через physical parent + filesystem-aware
  Unicode/case suffix без lexical `.`/`..` collapse и drive-relative paths, а
  full-product export preflight fail-closed охватывает orphan/suppressed links;
- [x] conjunctive native nested shared-source GPU marker и отрицательная
  per-field матрица добавлены; финальный DPR matrix остаётся runner gate;
- [ ] full clean `ci`, verifier, review и итоговый отчёт A6c1 зелёные.

### A7a — headless mate kernel: grounded + planar coincident

Начинать только после повторного доказательства persistent topology v2 на
реальных target references. Создать typed mate registry и отдельный 6-DOF
solver только для planar coincident с aligned/opposed и offset. Grounded —
occurrence property, которую solver трактует как fixed 6 DOF; отдельного Ground
mate нет. Qt/UI/persistence не добавлять.

Exit checklist:

- [x] candidate placements публикуются для связной component атомарно;
- [x] late failure не публикует ранние transforms;
- [x] success/underconstrained/redundant/overconstrained/ambiguous/missing/
  broken различимы и truthful DOF/residuals доступны;
- [x] localized seed/component, full ID/insertion/endpoint permutation,
  finite/exact-runtime-budget, candidate staleness/rollback,
  authoritative ambiguous/reversed, large-lever rank и real non-planar
  fixtures зелёные; candidate seal не имеет test reseal backdoor;
- [ ] solver tests, clean `ci`, verifier, review и отчёт A7a зелёные.

### A7b — mate persistence для поддержанного ядра

Сохранить typed occurrence-scoped topology/datum references и solver options
только для A7a. Unknown mate type/capability — `Unsupported`; invalid reference
— `Invalid`/`Broken` по утверждённому contract, но не silent drop.
Persisted state содержит только authored mate type/parameters/references/options.
Residuals, DOF, conflict set и diagnostic status являются derived state,
обязательно пересчитываются при staged load/solve и не сериализуются как
authoritative data.

Exit checklist:

- [ ] save/load сохраняет occurrence path, local persistent ref и authored
  options, а diagnostics после load воспроизводимо пересчитаны;
- [ ] JSON не содержит authoritative residual/DOF/conflict cache;
- [ ] placement-only edit не инвалидирует local topology identity;
- [ ] corrupt/unknown input не меняет committed assembly;
- [ ] codec/property tests, clean `ci`, verifier, review и отчёт A7b зелёные.

### A7c — расширение mate families микрошагами

Каждый пункт ниже является отдельной поставкой с отдельным разрешением,
implementation contract, review и отчётом:

1. parallel/perpendicular;
2. concentric для datum/cylindrical axes;
3. Distance после стабилизации предыдущих типов;
4. Angle после стабилизации Distance.

Для каждого микрошага обязательны permutation, redundant/conflict,
degenerate/non-finite, localized solve, rollback и save/load regressions.

Exit checklist каждого микрошага A7c:

- [ ] добавлена ровно одна relation family из списка;
- [ ] прежние families и их diagnostics не изменили semantics;
- [ ] solve остаётся localized и атомарным, UI отсутствует;
- [ ] codec capability и round-trip расширены без silent fallback;
- [ ] solver/codec tests, clean `ci`, verifier, review и отдельный отчёт зелёные.

#### A7c1 — planar parallel/perpendicular

- [x] единый tagged registry хранит Coincident/Parallel/Perpendicular с общей
  owner-local ID, revision/fingerprint и option seal моделью;
- [x] mixed localized solver реализует rank-2 Parallel без plane translation и
  rank-1 Perpendicular с deterministic collinear fallback;
- [x] V9 условно объявляет `assembly-planar-angular-mates-v1`, использует
  точные disjoint schemas и сохраняет coincident-only bytes;
- [x] deterministic solver/codec regressions и literal fuzz seeds расширены;
- [ ] clean focused gate, independent review и финальный отчёт A7c1 зелёные.

#### A7c2 — axis concentric

- [x] единый tagged registry содержит `AxisConcentricMate` с owner-relative
  path и datum X/Y/Z либо authoritative V2 cylindrical-face axis;
- [x] общий `solveAssemblyMates` даёт unoriented rank-4 projection без axial
  slide/twist, а совместимый `solvePlanarMates` включает все relation kinds;
- [x] V9 условно объявляет `assembly-axis-concentric-mates-v1`, строго кодирует
  обе axis-reference alternatives и сохраняет старые capability/bytes;
- [x] focused solver/V9 tests покрывают real topology, permutations, exact work,
  fixed point и materialization rollback; UI и Distance/Angle не входят.
- [x] review hardening: total endpoint view без terminate, full nested/topology
  candidate dependency seal, mixed rank/conflict matrix, independent A7c2
  datum+cylinder byte oracle и real cylinder/datum fixed point.

#### A7c3 — symmetric point distance

- [x] unified registry содержит `PointDistanceMate` с datum Origin либо
  authoritative V2 circular-edge center и strictly-positive unsigned distance;
- [x] localized solver добавляет rank-1 translation-only projection без
  rotation/fallback axis; numerical coincidence даёт typed retained
  `DegenerateDirection/Broken`;
- [x] V9 условно объявляет `assembly-point-distance-mates-v1`, строго кодирует
  disjoint point schemas и сохраняет прежние byte/capability combinations;
- [x] focused solver/V9 matrices покрывают permutations, conflict/redundancy,
  mixed concentric, exact work, stale rollback, nested fixed point и fuzz seeds;
  UI и Angle не входят.

#### A7c4 — planar angle

- [x] unified registry append-only содержит `PlanarAngleMate` с oriented V2
  planar endpoints, aligned/opposed branch и unsigned acute `angleRad`;
- [x] localized solver добавляет rank-1 pivot-preserving cone projection без
  translation/world-axis fallback; collinear unsatisfied relation даёт typed
  retained `DegenerateDirection/Broken`;
- [x] shared planar resolver metered по authoritative face count, принимает
  только FORWARD/REVERSED и сохраняет resource boundary;
- [x] V9 условно объявляет отдельный `assembly-planar-angle-mates-v1`, строго
  кодирует exact schema и все 16 capability combinations без изменения старых
  byte oracles;
- [x] focused solver/V9/writer matrices покрывают exact work, nested fixed
  point, seven-seam rollback и valid/corrupt/unknown fuzz seeds; UI отсутствует.
- [x] correction gate фиксирует model/V9 angular tolerance floor `2^-42`,
  satisfied identity/fixed point, arbitrary-3D literal pose oracle, rigid-frame
  covariance, mixed exact placements, 3-node/localized graphs и bad_alloc
  propagation через topology resolver.

### A8a — базовый Mate tool

Добавить typed two-stage selection, compatible filter, aligned/opposed preview,
Apply/Cancel и одну Undo transaction для уже поддержанного planar mate. Failed
preview не заменяет последнюю valid presentation.

Exit checklist:

- [ ] selection никогда не теряет occurrence scope: pick хранит exact current
  publication, полный `OccurrencePath`, terminal `DefinitionId` и V2 face;
- [ ] Apply/Cancel/Undo/Redo симметричны через одну append-only cold command и
  атомарный registry+placement transaction;
- [ ] teardown безопасен при Open/project switch, failed Open сохраняет tool,
  а quiescence блокирует Reload и другие assembly mutations;
- [ ] UI smoke/DPI, clean `ci`, verifier, review и отчёт A8a зелёные.

Текущее focused dev evidence включает clean-first model/editor build,
allocation-failure seams, sealed transaction, headless session/history,
viewport source-reuse и frozen picking при candidate preview, V7/V8 Apply-only
promotion и lifecycle UI scenarios. Native workspace harness теперь исполняет
typed two-stage flow, strict filter, aligned/opposed и retained failed preview,
Apply/Cancel/Escape, одну Undo/Redo и teardown/reentry; `A8A_GPU_SMOKE=PASS`
получен на фактическом AMD/OpenGL context при 150%. Layout harness напрямую
запрашивает четыре logical window size и проверяет реальные mate widgets,
отдельно печатая OS-accepted size и native DPR; synthetic DPR не моделируется.
Preview oracle использует transformed triangle candidate scene, а не две
координаты committed picking. Однако ручная матрица
100/125/150/200% не заменена. Checklist остаётся открытым до review, полного
`ci`, verifier и ручного native Windows GPU/DPI подтверждения.

### A8b — mate management и diagnostics UI

Добавить edit/reselect/remove, русскую диагностику, tree/property presentation
и UI для типов, принятых в A7c. Broken mate остаётся в документе с точным code
и явной командой повторного выбора.

Exit checklist:

- [ ] status в model/tree/properties согласован;
- [ ] reselect не меняет mate до Apply;
- [ ] missing/broken/ambiguous не скрываются;
- [ ] UI/lifecycle/regression tests, clean `ci`, verifier, review и отчёт A8b
  зелёные.

### A9a — STL assembly export

Начинать только после принятого A8b и отдельного export contract.

Flatten выполнять только на output boundary с occurrence transforms, не меняя
source shapes. Suppressed пропускается, visibility не влияет на semantic
export. Любой unresolved unsuppressed occurrence блокирует весь export и не
повреждает прежний destination file.

Exit checklist:

- [ ] все unsuppressed leaf occurrences экспортированы с world transforms;
- [ ] source shapes/revisions не изменились;
- [ ] unresolved input не создаёт partial output и не портит старый файл;
- [ ] export tests, clean `ci`, verifier, review и отчёт A9a зелёные.

### A9b — STEP/XDE assembly export

Начинать только после принятого A9a и отдельного OCCT/XDE audit.

Использовать XDE/CAF (`STEPCAFControl`, XCAF shape tool) для names, reused
definitions, hierarchy и locations. Текущий `STEPControl_Writer` остаётся part
compatibility path и не называется assembly export.

Exit checklist:

- [ ] XDE round-trip сохраняет names, hierarchy, reused definitions и locations;
- [ ] part-only STEP path не изменил прежнее поведение;
- [ ] unresolved input и write failure атомарны;
- [ ] OCCT/XDE tests, clean `ci`, verifier, review и отчёт A9b зелёные.

### A9c — large-assembly measurements

Начинать только после принятого A9b; performance changes не включать до
измеренного baseline.

Добавить opt-in benchmark 100/1000 occurrences: scene publication, placement
update, picking, memory и CPU/GPU source counts. Сначала зафиксировать baseline
и deterministic work counters; wall-clock gate вводить только на стабильном
стенде.

Exit checklist:

- [ ] сценарии и hardware/software context воспроизводимо описаны;
- [ ] CPU/GPU source counts подтверждают sharing contract;
- [ ] baseline и after приложены без flaky wall-clock assertion;
- [ ] opt-in benchmark не замедляет обычный test suite;
- [ ] clean `ci`, verifier, review и отчёт A9c зелёные.

Для A9a/A9b/A9c обязательны отдельные разрешения. Не объединять export и
performance в один diff.

Non-goals всей программы до отдельного задания: flexible/kinematic
subassemblies, contact/collision, interference analysis, configurations,
exploded view, полноценный BOM, in-context cross-component Part Design,
scale/reflection occurrences и автоматическое сетевое обновление.

## Regression matrix по этапам

Gate текущего этапа включает только строки его этапа, все уже принятые строки
предыдущих этапов и полный существующий regression suite. Будущие строки не
являются условием завершения раннего этапа.

| Этап | Обязательные deterministic regressions |
|---|---|
| A0a | source versions v1/v2/v3; malformed vs unsupported; rejected load не меняет destination/cursors |
| A0b | hand-authored fixture encode; semantic IDs/order/fields; rebuilt validity/bounds/volume/topology outcomes с tolerance |
| A0c1 | parsed/error equivalence; limits; fuzz corpus; materialization не перенесена |
| A0c2 | late materialize/recompute rollback; cursors; один feature registry |
| A1a | unique ownership; duplicate/missing/root IDs; Qt-free public headers |
| A1b1 | staged carrier move/rollback; existing editor owner unchanged |
| A1b2 | New/Open/Close/failed Open; single editor owner; legacy part workflow |
| A2 | выбранная ADR branch; legacy binding либо envelope Save/Save As/Relocate/exact fields; metadata staging |
| A3a | two occurrences/one definition; orphan retention; transforms/flags; invalid IDs/path |
| A3b | shared payload once; `mates`/`nesting` Invalid; capability Unsupported; late rebuild rollback |
| A3c | service staging; part-only rejection; editor Open assembly leaves active part unchanged |
| A4a1 | deterministic pure snapshot; invalid build без partial result |
| A4a2 | atomic publication; generation и delayed callback lifetime |
| A4b | shared source counts; cross-project epoch isolation; placement without remesh/upload; pruning |
| A4c1 | transformed bounds/Fit; hidden/suppressed policy |
| A4c2 | frontmost ray/occlusion; same local face in two paths; independent highlight |
| A4c3 | occurrence-scoped rectangle selection/snap/ruler |
| A5a | tree↔viewport selection; read-only open; project switching |
| A5b | Insert/placement Apply/Cancel/Escape; one Undo transaction; failed staging |
| A5c0 | typed delta apply/revert/failure; no transient geometry snapshots |
| A5c1–6 | отдельная command matrix; same-definition duplicate; orphan-preserving remove/undo |
| A5d | shared edit success; failed recompute full rollback; IDs/placements preserved |
| A6a | missing lexical path; Windows/POSIX case; symlink/reparse loop; conflict/escape/budgets/no network |
| A6b | reload/repair success/failure; topology re-resolution; no implicit cascade save |
| A6c | nested transforms/paths; direct/indirect cycles; depth/work limits |
| A7a | planar fixtures; DOF/conflict statuses; permutation; late rollback; degenerate refs |
| A7b | authored mate round-trip; diagnostics recomputed; unknown type; corrupt refs |
| A7c | отдельная матрица для каждой новой relation family |
| A8 | tool lifecycle; selection filters; preview; Apply/Cancel/Undo; Broken reselect |
| A9a | transformed STL instances; source immutability; atomic output failure |
| A9b | STEP names/hierarchy/reuse/locations round-trip |
| A9c | 100/1000 occurrence counters и baseline без flaky wall-clock assertion |

Все tests используют репозиторный `CHECK`, а не raw `assert`, и не зависят от
timing или OCCT enumeration order. Migration fixtures создаются вручную, а не
только текущим writer.

## Проверка каждого этапа

Implementer запускает targeted tests своего этапа во время работы. После PASS
review lead назначает `solidcad-build-engineer` read-only CI runner: тот один
раз выполняет reproducible `ci` gate и возвращает штатный
`BUILD ENVIRONMENT: PASS|FAIL` с командами/категорией. В этом режиме build
engineer не меняет product/test files; script-layer edit разрешён только после
отдельно подтверждённой infrastructure причины. Lead сам shell не запускает.

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<repo>\vcpkg
set VCPKG_DEFAULT_TRIPLET=ci-x64-windows
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset ci
cmake --build --preset ci --clean-first
ctest --preset ci --output-on-failure
```

Если headers не менялись, `--clean-first` можно опустить только при явном
подтверждении lead. Label runs (`unit`, `regression`, `topology`, `ui-smoke`,
`compliance`) применяются как targeted диагностика, а не как пять обязательных
повторов уже полного suite.

После успешного `ci` lead вызывает `solidcad-local-verifier`, который в обычном
happy path ровно один раз запускает свой штатный dev contract:

```powershell
.\.opencode\scripts\verify-and-run.ps1
```

Verifier только сообщает ошибки и не редактирует код. Не дублировать этот
скрипт другим агентом. Failure маршрутизируется строго по agent policy:

- TOOLCHAIN/CONFIGURE/DEPENDENCY/NETWORK/STALE_BUILD — один диагностический
  pass `solidcad-build-engineer`, затем один повтор затронутого gate;
- COMPILE/LINK/TEST/RUNTIME из product code — focused correction implementer
  разрешена только если contract ещё не израсходовал единственный correction
  pass; затем обязательны повторный review, `ci` и verifier;
- если correction pass уже использован, второй review остаётся BLOCKED либо
  причина не классифицирована безопасно — STOP и отчёт пользователю, без
  третьего цикла и без ослабления tests.

Для A4b и всех последующих viewport/UI этапов дополнительно нужен manual
real-GPU smoke по scope этапа. Для UI: DPI 100/125/150/200%, Apply/Cancel,
project switch и Undo/Redo. Для renderer: shared source counts, transform,
selection/highlight, Fit/orbit/zoom и resource teardown.

Это human-owned gate. После автоматических PASS lead выдаёт пользователю точный
checklist и запрашивает подтверждение результата; штатный verifier launch не
подменяет интерактивную проверку. Без подтверждения пользователя статус этапа
`MANUAL GPU PENDING`: этап не объявляется завершённым и следующий этап не
начинается. Агент не выдумывает результат. Если пользователь отдельно даёт
доступ к GUI-capable runner, его evidence должно быть приложено явно.

## Финальный отчёт после каждого этапа

### A7b0 — strict disconnected V9 mate persistence

A7b0 owns the model option seal and the independent V9
scan/encode/materialize boundary. It includes child-first registry restoration,
component solve/application, retained semantic failures, hard-failure rollback,
runtime diagnostics and V7/V8 fail-closed behavior. It does not route V9
through MainWindow or ProjectApplicationService and does not add an atomic V9
writer or fuzz entry points.

### A7b1 — editor publication and hostile-input integration

A7b1 is the explicit follow-up for Open/Save/Save As format routing, atomic
writer policy/failure injection, application publication and switch rollback,
fuzz dispatch/corpus, and the corresponding editor tests. A7b0 completion must
not be reported as A7b1 completion.

A7b1 implementation uses exact ProductV9/Future routing, a two-pass
ValidationOnly fixed-point writer, explicit V9 promotion state, and an
application-owned runtime diagnostics seal. V7/V8 fail closed on both authored
mates and custom authoritative options. Save, Save As and first Save preserve
the selected V7/V8/V9 format and reuse the common alias/no-cascade policy; no
real assembly exporter is introduced.

После всех доступных gates `solidcad-lead` публикует в чате датированный
authoritative completion report и указывает:

1. пользовательский результат и восстановленные инварианты;
2. read-only audits и точный implementation contract;
3. выбранное решение и отклонённые альтернативы;
4. изменённые production/test/persistence/build файлы;
5. новые deterministic regressions и hand-authored fixtures;
6. targeted tests, полный `ci`, verifier и manual smoke по scope;
7. performance baseline/counters для performance-sensitive этапов;
8. результат независимого review и fix cycle;
9. ограничения, расхождения и следующий безопасный этап;
10. подтверждение сохранности unrelated файлов и отсутствия commit/push.

Не выполнять дополнительный вызов implementer только ради записи результатов
review/CI/verifier в `docs/reports/`: это создало бы непроверенный write-pass
после финального gate. Если пользователь отдельно просит сохранить отчёт в
репозитории, это новый documentation-only contract после завершения этапа; он
не меняет product/test/build files и проверяется отдельным diff review.

Этап не завершён при красном gate, непроверенном header rebuild, миграции без
независимого oracle, частично опубликованном state, HIGH замечании reviewer,
неустранённом topology/lifetime риске или невоспроизводимом доказательстве.

## Git

Не выполнять commit, push, merge, rebase, reset, clean и не менять ветку без
явного запроса пользователя. Не удалять и не форматировать unrelated файлы.
