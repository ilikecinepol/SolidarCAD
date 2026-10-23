# Product document envelope и идентичность определений

Дата: 18 октября 2026 г.; уточнение persistence boundary — 19 октября 2026 г.;
exit D1 — 23 октября 2026 г.
Статус: решение принято (ветвь B); D1 завершён; standalone v4 отделён от
product envelope; A1a разблокирован и авторизован

## Контекст

Пользователь явно выбрал ветвь B для будущего формата изделий и компонентов.
23 октября 2026 года выполнен exit-критерий D1 и отдельно авторизован следующий
ограниченный шаг A1a. Это решение не активирует A2, A7 или product persistence.

Подэтап G0 ядра графа зависимостей закрыт 18 октября 2026 года: единый
`DocumentIndex` имеет детерминированные cross-body, Sketch и face-support edges,
структурный abort, affected-closure propagation и same-ID recovery; evidence
закреплено scheduler/removal/join/pattern regressions и согласовано с roadmap.
Это не закрывает полный этап Parametric Core 2.0. Persistent topology v2,
standalone v4 reader/writer и application gate завершены, включая зафиксированный
boolean split/merge contract и регрессии. Следующим разрешён только A1a;
production-типы изделия, A2, A7 и mates не активированы. Техническая возможность
показать статическую сборку не является разрешением на расширение scope.

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

Production reader открывает standalone v1-v4. Новый документ и открытый v1-v3
несут состояние V3; открытый v4 несёт V4. Обычные Save/Save As сохраняют это
явное состояние, а отдельный атомарный `Upgrade to v4` переводит V3 в V4.
Native V2 authoring/commit в V3 заблокирован, удаление последней V2-ссылки не
понижает V4, а v3 writer по-прежнему отвергает V2. Ни один из этих путей не создаёт
product envelope или external identity. До A2 запрещены persisted product
envelope, `ProjectId`, envelope schema metadata, conversion workflow,
persistence codec и activation product-envelope schema в application Open.

Этот запрет не блокирует последовательность A1 из исходной задачи. После
пройденного topology gate A1a добавляет Qt-free in-memory, non-production и
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
  `ProjectId`; завершённые P2/P3 проводят v4 через строгие staged
  decoder/materialization/application gates и атомарный writer. Save и Save As
  сохраняют явный standalone format state: открытый v4
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

Ветвь B требует будущей явной миграции product persistence и поэтапной смены
application ownership; текущий single-`Document` workflow остаётся фактическим
до следующих отдельно авторизованных шагов. Цена миграции принята ради одной
модели envelope/kind, честной linked identity и возможности subassemblies без
второй смены основы.

Этот ADR не вводит production-типы, resolver, occurrences, UI или mates. Он
также не переиспользует standalone v4 как envelope: product schema будет
отдельной версией выше 4. Завершение D1 разрешает только A1a; A2 и A7 не
активированы.

Graph prerequisite закрыт подэтапом G0. Исторический точный gate D1 для
разблокировки A1a состоял из всех трёх условий:

1. persistent topology v2 реализована end-to-end, включая v4 reader/writer,
   freeze boolean algebra и regressions для требуемых split/merge, symmetry и
   pattern provenance случаев;
2. на точном HEAD получено итоговое зелёное evidence согласованного build/test
   gate;
3. пользователь дал явную авторизацию начать A1a.

Все условия выполнены 23 октября 2026 года на `HEAD bdba8c9`: ключевые commits
`8043f70` (атомарный open и сохранение V4 state) и `bdba8c9` (format-gate native
V2 authoring); clean CI собрал 579/579 целей, CTest прошёл 108/108 тестов с
labels `unit` (57), `regression` (80), `topology` (28), `ui-smoke` (12) и
`compliance` (4). Локальный verifier прошёл 203/203 целей, 108/108 тестов и
GUI smoke 8 секунд. D1 завершён; A1a разблокирован и авторизован. A1b, A2 и A7
сохраняют собственные последовательные границы; остальные пункты Parametric
Core 2.0 этим evidence не объявляются завершёнными.
