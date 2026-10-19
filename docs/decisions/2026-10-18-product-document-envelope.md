# Product document envelope и идентичность определений

Дата: 18 октября 2026 г.; уточнение persistence boundary — 19 октября 2026 г.
Статус: архитектурное решение принято (ветвь B); product envelope отделён от
standalone project format v4; D1 не завершён

## Контекст

Пользователь явно выбрал ветвь B для будущего формата изделий и компонентов.
Это решение фиксирует целевую модель владения и идентичности, но не разрешает
начинать A1 и не означает выполнение exit-критерия D1.

Подэтап G0 ядра графа зависимостей закрыт 18 октября 2026 года: единый
`DocumentIndex` имеет детерминированные cross-body, Sketch и face-support edges,
структурный abort, affected-closure propagation и same-ID recovery; evidence
закреплено scheduler/removal/join/pattern regressions и согласовано с roadmap.
Это не закрывает полный этап Parametric Core 2.0. In-memory carrier persistent
topology v2 и первые face-lineage producers/transform consumers уже существуют,
но persistence, полный boolean split/merge contract и reader/writer gates ещё
не завершены. Поэтому A1 и типы изделия, а также A7 и mates, запрещены до
прохождения оставшихся gates. Техническая возможность показать статическую
сборку не является разрешением на реализацию.

Текущее приложение по-прежнему владеет одним `Document`. Production-типа
изделия и перехода application ownership пока нет.

## Рассмотренные варианты

### Вариант A — отдельный assembly root рядом с part root

Сохранить текущий part-документ как самостоятельный корень и добавить
параллельный формат сборки со своими правилами ссылок. Честная реализация этого
варианта не обязана считать путь идентичностью: assembly может владеть стабильным
binding ID для вставленного standalone v3, а путь оставить только locator. Такой
binding, однако, идентифицирует вхождение для конкретной сборки, а не создаёт
долговечную внешнюю идентичность самого standalone-проекта.

### Вариант B — единый identity-bearing envelope

Любой новый product-документ имеет один envelope, реестр определений и явный
корень. Вид определения является данными внутри envelope, а не альтернативным
форматом файла. Это даёт одну модель для деталей, изделий и будущих
subassemblies и позволяет честно отделить идентичность связанного определения
от его расположения и ревизии.

Сравнение вариантов по dimensions D1:

| Dimension | Вариант A | Вариант B |
| --- | --- | --- |
| Пользовательская ценность | Быстрее даёт отдельную статическую сборку и не требует немедленной конвертации детали. | Даёт общую модель детали, изделия и будущей subassembly, включая устойчивую внешнюю идентичность после явной конвертации. |
| Начальная стоимость миграции | Ниже: standalone part остаётся отдельным типом root, assembly добавляется рядом. | Выше: нужны envelope, явная конвертация persistence и staged смена application ownership. |
| Linked identity | Assembly-owned binding ID честно идентифицирует импорт внутри одной сборки; путь остаётся locator. Для ссылки на тот же проект между сборками нужна дополнительная внешняя identity policy. | После explicit conversion `ExternalSourceKey` задаёт долговечную идентичность definition; linked resolution остаётся запрещённым до A6. Locator и revision отделены. |
| `Save` / `Save As` / `Relocate` | Для standalone продолжают действовать standalone semantics; assembly binding не превращает файл детали в identity-bearing source. Правила part и assembly остаются раздельными. | До конвертации действуют те же standalone semantics; после неё единые правила `ProjectId` различают Save, clone через Save As и Relocate. |
| Обратная совместимость | Standalone part читается без миграции; новый assembly root неизвестен старому reader. | Standalone part читается без миграции; новый envelope неизвестен старому reader. В обоих вариантах требуется явный `Unsupported`, без silent downgrade. |
| Число format bumps | Можно начать с одного assembly-format transition, но последующее объединение external identity/subassembly, вероятно, потребует ещё одного перехода. | Требует более дорогого первого envelope transition, но, вероятно, уменьшает число долгосрочных schema transitions благодаря одной модели root/kind. |
| Application ownership | Сохраняет два долгосрочных пути владения: single `Document` для part и отдельный owner для assembly. | Целевой единый `ProductDocument` становится owner для part и assembly после отдельной staged миграции. |

Вариант B выбран не потому, что A неизбежно использует path identity. Его
преимущество — единая долговечная external identity, естественная модель
subassembly, меньше долгосрочных dual paths и, вероятно, меньше последующих
schema transitions, а также единый application owner. Эти преимущества приняты
несмотря на более высокую начальную стоимость миграции.

## Решение

Принят вариант B: единый identity-bearing product/component envelope. Версия
схемы ортогональна виду корня и виду определения. Уточнение P0 резервирует v4
не для envelope, а для standalone single-part topology-capable schema из
`docs/project-format-v4.md`. Первая схема envelope поэтому получает отдельный
номер строго выше `4`; точный номер назначается её собственным persistence gate.

Целевая модель владения:

- `ProductDocument` владеет `ProjectId`, `DefinitionRegistry` и корневым
  `DefinitionId`;
- `PartDefinition` владеет существующим `Document`;
- `AssemblyDefinition` в будущем владеет occurrences;
- occurrence ссылается на definition и не клонирует его B-Rep.

Пространства идентификаторов имеют разные области действия:

- `ProjectId` идентифицирует проект;
- `DefinitionId` уникален внутри проекта и всегда интерпретируется вместе с
  `ProjectId`;
- `OccurrenceId` уникален внутри конкретного assembly definition;
- `OccurrencePath` — упорядоченный путь из `OccurrenceId` от корня сборки к
  вложенному occurrence, а не замена идентичности definition;
- `ExternalSourceKey = {ProjectId, DefinitionId}` разрешён только для источника,
  который сам несёт такую идентичность;
- `SourceRevision` или content hash описывает ревизию содержимого, но не
  идентичность; файловый путь является только locator.

Одинаковый `ExternalSourceKey` с разными `SourceRevision` или content hash
означает `IdentityConflict`. Resolver не должен молча выбирать один вариант по
пути, времени изменения или порядку загрузки.

## Миграция standalone v1-v4

Production writer сегодня остаётся v3 и single `Document`; reader сохраняет
standalone v1-v3. V4 определён отдельно как standalone single-part schema для
persistent topology v2, но его reader/writer ещё не реализованы. До A2
запрещены persisted product envelope, `ProjectId`, envelope schema metadata,
conversion workflow, persistence codec и activation product-envelope schema в
application Open.

Этот запрет не блокирует последовательность A1 из исходной задачи. После
прохождения topology gate A1a добавляет Qt-free in-memory, non-production и
non-persisted foundation — project-local `DefinitionId`,
`DefinitionRegistry`, root ID и `PartDefinition`, единолично владеющую
существующим `Document`. Затем A1b1 может провести тот же единственный
in-memory carrier через staged-границу `ProjectApplicationService`, а A1b2 —
перевести committed editor ownership на него. Все три шага остаются
non-persisted: не добавляют `ProjectId` или envelope metadata, не меняют
standalone codec/writer и не создают второй authoritative state.

Carrier A1 не является product envelope. После A1b обычный New/Open может
владеть standalone `Document` через этот in-memory root carrier, но не создаёт
ни transient, ни persisted product envelope и не активирует envelope schema.
A1a, A1b1 и A1b2 выполняются до A2, последовательно и каждый в собственной
границе с отдельной авторизацией.

Правила этого ADR явно заменяют раннюю формулировку A2 в строках 403–407
`docs/tasks/2026-10-18-components-and-assemblies.md`, где обычные Save/Save As
могли быть истолкованы как implicit identity migration. Вложение задачи
остаётся неизменяемым историческим evidence; при будущей реализации A2
приоритет имеет этот ADR. Единственный способ создать product envelope из
standalone v1-v4 — явная команда conversion, описанная ниже.

В будущем только explicit Convert workflow сможет staged и атомарно обернуть
standalone v1-v4 в памяти в envelope с одним корневым `PartDefinition` и
выделить candidate `ProjectId` и root `DefinitionId`. До успешного conversion
commit эти candidate IDs являются только transient conversion state: они не
сохраняются и не считаются persisted external identity. Обычный Open не создаёт
даже transient product envelope.

Единственный trigger первой записи envelope для открытого standalone v1-v4 —
явная будущая команда `Convert to Product Document` / «Преобразовать в документ
изделия». Она атомарно пишет envelope в выбранную пользователем цель и не
перезаписывает source неявно. Standalone-файл остаётся читаемым и неизменным, если
пользователь явно не выбрал его как conversion target с перезаписью. Скрытый
sidecar не создаётся.

До conversion:

- обычный `Save` в ту же цель и `Save As` не создают envelope и не записывают
  `ProjectId`; P2
  декодирует v4 только в low-level staged tests, не содержит v4 writer и не
  допускает payload до application open/Save/Save As/autosave/recovery; после
  P3 Save и Save As сохраняют явный standalone format state: открытый v4
  остаётся v4, а новый или открытый v1-v3 остаётся v3 до отдельного явного
  standalone upgrade, без content-driven upgrade/downgrade; standalone upgrade
  v1-v3 -> v4 не создаёт product envelope;
- standalone `Relocate` не имеет гарантии сохранения идентичности проекта;
- использовать standalone payload внутри product/assembly можно только явной
  операцией import/copy в новый project-local embedded `PartDefinition`, которым
  владеет целевой `ProductDocument`/assembly;
- такой embedded `PartDefinition` является новым определением целевого проекта:
  он не является source binding или link, не сохраняет идентичность исходного
  standalone-файла и не создаёт `ExternalSourceKey`;
- linked identity, binding к standalone source и reload/repair semantics
  запрещены до explicit conversion исходного standalone-файла в
  identity-bearing envelope и
  утверждения A6 resolver policy;
- путь к standalone source допускается только как provenance/locator metadata и
  никогда не является идентичностью.

После conversion:

- обычный `Save` в ту же цель сохраняет `ProjectId`;
- `Save As` по умолчанию является clone/fork и создаёт новый `ProjectId`;
- будущая явная операция `Relocate`/`Move` сохраняет `ProjectId`;
- project-local `DefinitionId` могут сохраниться при clone, потому что новый
  `ProjectId` задаёт новую область их идентичности.

После conversion обычная файловая copy сохраняет persisted `ProjectId` и
создаёт replica того же проекта с ещё одним locator; это не clone. `Relocate`
перемещает тот же проект и сохраняет его identity, а clone создаёт независимо
развивающийся проект с новым `ProjectId`. UI и конфликтная политика для
одновременных replica остаются отдельной будущей работой.

## Embedded и linked definitions

Будущий начальный product envelope сможет владеть embedded definitions. До
conversion standalone payload может попасть туда только как явно импортированный
или скопированный новый project-local embedded `PartDefinition`; связи с
исходным standalone source у него нет. Linked definitions, source binding и
reload/repair откладываются до explicit conversion источника и A6 identity и
resolver policy. Standalone v1-v4 не становится linked definition и не получает
`ExternalSourceKey` посредством файлового пути: путь остаётся только
provenance/locator metadata.

## Совместимость и сериализация

Envelope-aware loader должен сохранить поддержку standalone v1, v2, v3 и v4.
Явная конвертация v1-v4 обязана сохранять существующие stable IDs и полную
topology provenance без понижения V2 references до V1. Envelope новой схемы для
reader без её поддержки должен классифицироваться как `Unsupported` и никогда
не понижаться молча до standalone part. Миграция будет выполняться staged и атомарно
через seams A0: ошибка не публикует частично построенный product-документ, не
изменяет source и не изменяет destination.

Derived и transformed B-Rep, mesh и GPU-данные не сериализуются и
перестраиваются из авторитетных параметров. Существующее исключение
`ImportedShape`, где импортированная геометрия является авторитетным входом,
сохраняется.

## Последствия и границы rollout

Ветвь B требует явной миграции persistence и поэтапной смены application
ownership; текущий single-`Document` workflow остаётся фактическим до отдельной
авторизованной реализации. Цена миграции принята ради одной модели envelope/kind,
честной linked identity и возможности subassemblies без второй смены основы.

Этот ADR не вводит production-типы, resolver, occurrences, UI или mates. Он
также не переиспользует standalone v4 как envelope: product schema будет
отдельной версией выше 4. A7 mates не начинается до завершения persistent
topology v2 persistence и её регрессий.

Graph prerequisite закрыт подэтапом G0. Оставшийся точный gate для разблокировки
A1 состоит из всех трёх условий:

1. persistent topology v2 реализована end-to-end, включая v4 reader/writer,
   freeze boolean algebra и regressions для требуемых split/merge, symmetry и
   pattern provenance случаев;
2. на точном HEAD получено итоговое зелёное evidence согласованного build/test
   gate;
3. пользователь дал явную авторизацию начать A1.

До выполнения всех условий D1 exit не достигнут, A1 остаётся заблокированным.
Статическая assembly feasibility сама по себе ни один из этих gates не заменяет.
