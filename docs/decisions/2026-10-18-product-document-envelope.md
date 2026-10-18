# Product document envelope и идентичность определений

Дата: 18 октября 2026 г.
Статус: архитектурное решение принято (ветвь B); D1 не завершён, реализация A1
заблокирована невыполненными prerequisites

## Контекст

Пользователь явно выбрал ветвь B для будущего формата изделий и компонентов.
Это решение фиксирует целевую модель владения и идентичности, но не разрешает
начинать A1 и не означает выполнение exit-критерия D1.

Граф зависимостей является кандидатом с завершённой реализацией: есть
детерминированный cross-body graph и тесты. Формальное закрытие milestone и
согласование его статуса с roadmap ещё не выполнены. Persistent topology
остаётся на уровне v1; v2 отсутствует. Split/merge сейчас завершаются отказом,
симметричная геометрия неоднозначна, а provenance для геометрии, созданной
pattern-операциями, отсутствует. Поэтому A1 и типы изделия, а также A7 и mates,
запрещены до прохождения зафиксированных ниже gates. Техническая возможность
показать статическую сборку не является разрешением на реализацию.

Текущее приложение по-прежнему владеет одним `Document`. Production-типа
изделия и перехода application ownership пока нет.

## Рассмотренные варианты

### Вариант A — отдельный assembly root рядом с part root

Сохранить текущий part-документ как самостоятельный корень и добавить
параллельный формат сборки со своими правилами ссылок. Честная реализация этого
варианта не обязана считать путь идентичностью: assembly может владеть стабильным
binding ID для вставленного legacy v3, а путь оставить только locator. Такой
binding, однако, идентифицирует вхождение для конкретной сборки, а не создаёт
долговечную внешнюю идентичность самого legacy-проекта.

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
| Начальная стоимость миграции | Ниже: legacy part остаётся отдельным типом root, assembly добавляется рядом. | Выше: нужны envelope, явная конвертация persistence и staged смена application ownership. |
| Linked identity | Assembly-owned binding ID честно идентифицирует импорт внутри одной сборки; путь остаётся locator. Для ссылки на тот же проект между сборками нужна дополнительная внешняя identity policy. | После explicit conversion `ExternalSourceKey` задаёт долговечную идентичность definition; linked resolution остаётся запрещённым до A6. Locator и revision отделены. |
| `Save` / `Save As` / `Relocate` | Для legacy продолжают действовать legacy semantics; assembly binding не превращает файл детали в identity-bearing source. Правила part и assembly остаются раздельными. | До конвертации действуют те же legacy semantics; после неё единые правила `ProjectId` различают Save, clone через Save As и Relocate. |
| Обратная совместимость | Legacy part читается без миграции; новый assembly root неизвестен старому reader. | Legacy part читается без миграции; новый envelope неизвестен старому reader. В обоих вариантах требуется явный `Unsupported`, без silent downgrade. |
| Число format bumps | Можно начать с одного assembly-format transition, но последующее объединение external identity/subassembly, вероятно, потребует ещё одного перехода. | Требует более дорогого первого envelope transition, но, вероятно, уменьшает число долгосрочных schema transitions благодаря одной модели root/kind. |
| Application ownership | Сохраняет два долгосрочных пути владения: single `Document` для part и отдельный owner для assembly. | Целевой единый `ProductDocument` становится owner для part и assembly после отдельной staged миграции. |

Вариант B выбран не потому, что A неизбежно использует path identity. Его
преимущество — единая долговечная external identity, естественная модель
subassembly, меньше долгосрочных dual paths и, вероятно, меньше последующих
schema transitions, а также единый application owner. Эти преимущества приняты
несмотря на более высокую начальную стоимость миграции.

## Решение

Принят вариант B: единый identity-bearing product/component envelope. Версия
схемы ортогональна виду корня и виду определения. Номер следующей схемы этим ADR
не назначается.

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

## Миграция legacy v1-v3

Production сегодня остаётся v1-v3 и single `Document`. Envelope, `ProjectId`,
`DefinitionRegistry` и conversion workflow ещё не реализованы и не должны
появляться до отдельно разрешённого A2. A2 будет реализовывать правила этого
ADR, а не заново решать их.

В будущем envelope-aware open сможет staged и атомарно обернуть legacy v1-v3 в
памяти в envelope с одним корневым `PartDefinition` и выделить candidate
`ProjectId` и root `DefinitionId`. До успешного explicit conversion commit эти
candidate IDs являются только transient migration state: они не сохраняются и
не считаются persisted external identity.

Единственный trigger первой записи envelope для открытого legacy-проекта —
явная будущая команда `Convert to Product Document` / «Преобразовать в документ
изделия». Она атомарно пишет envelope в выбранную пользователем цель и не
перезаписывает source неявно. Legacy-файл остаётся читаемым и неизменным, если
пользователь явно не выбрал его как conversion target с перезаписью. Скрытый
sidecar не создаётся.

До conversion:

- обычный `Save` в ту же цель и `Save As` сохраняют legacy v3 semantics и не
  обещают и не записывают `ProjectId`;
- legacy `Relocate` не имеет гарантии сохранения идентичности проекта;
- использовать legacy payload внутри product/assembly можно только явной
  операцией import/copy в новый project-local embedded `PartDefinition`, которым
  владеет целевой `ProductDocument`/assembly;
- такой embedded `PartDefinition` является новым определением целевого проекта:
  он не является source binding или link, не сохраняет идентичность исходного
  legacy-файла и не создаёт `ExternalSourceKey`;
- linked identity, binding к legacy source и reload/repair semantics запрещены
  до explicit conversion исходного legacy-файла в identity-bearing envelope и
  утверждения A6 resolver policy;
- путь к legacy source допускается только как provenance/locator metadata и
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
conversion legacy payload может попасть туда только как явно импортированный
или скопированный новый project-local embedded `PartDefinition`; связи с
исходным legacy source у него нет. Linked definitions, source binding и
reload/repair откладываются до explicit conversion источника и A6 identity и
resolver policy. Legacy v3 не становится linked definition и не получает
`ExternalSourceKey` посредством файлового пути: путь остаётся только
provenance/locator metadata.

## Совместимость и сериализация

Envelope-aware loader должен сохранить поддержку v1, v2 и v3. Envelope новой
схемы для reader без её поддержки должен классифицироваться как `Unsupported`
и никогда не понижаться
молча до legacy part. Миграция будет выполняться staged и атомарно через seams A0:
ошибка не публикует частично построенный product-документ и не изменяет
destination.

Derived и transformed B-Rep, mesh и GPU-данные не сериализуются и
перестраиваются из авторитетных параметров. Существующее исключение
`ImportedShape`, где импортированная геометрия является авторитетным входом,
сохраняется.

## Последствия и границы rollout

Ветвь B требует явной миграции persistence и поэтапной смены application
ownership; текущий single-`Document` workflow остаётся фактическим до отдельной
авторизованной реализации. Цена миграции принята ради одной модели envelope/kind,
честной linked identity и возможности subassemblies без второй смены основы.

Этот ADR не вводит production-типы, формат, resolver, occurrences, UI, mates или
номер схемы. A7 mates не начинается до реализации persistent topology v2 и её
регрессий.

Точный gate для разблокировки A1 состоит из всех четырёх условий:

1. формально согласован и закрыт milestone графа зависимостей, включая статус в
   roadmap;
2. persistent topology v2 реализована и покрыта regressions для требуемых
   split/merge, symmetry и pattern provenance случаев;
3. на точном HEAD получено зелёное evidence согласованного build/test gate;
4. пользователь дал явную авторизацию начать A1.

До выполнения всех условий D1 exit не достигнут, A1 остаётся заблокированным.
Статическая assembly feasibility сама по себе ни один из этих gates не заменяет.
