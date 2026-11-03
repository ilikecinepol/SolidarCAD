# Единое компонентное рабочее пространство

Дата: 31 октября 2026 г.

Статус: пользователь принял отказ от отдельного режима сборки и единое рабочее
пространство как целевое направление. Детальные технические решения ниже
(включая move-семантику первого инструмента) являются рабочим архитектурным
контрактом и проверяются поэтапно. Production-реализация поставляется отдельными
ограниченными этапами.

## Контекст

Ручная проверка A8a показала, что отдельная команда «Новая сборка», отдельная
лента и пустой `AssemblyDefinition` не соответствуют ожидаемому сценарию.
Пользователь создаёт тела в обычном проекте и ожидает превратить выбранное тело
в компонент в том же проекте. Компонент должен иметь собственное содержимое
проекта и дочерние компоненты. Размещение компонента меняется прямым
перетаскиванием. Инструмент соединения относится к последующей работе.

Текущий `Viewport` взаимоисключающе показывает part-сцену или assembly-сцену.
Поэтому скрыть отдельную ленту, не меняя модель и presentation boundary,
недостаточно: результатом снова станет пустой кадр или потеря part-инструментов.

## Решение

Целевой новый тип определения — `ComponentDefinition`:

```text
ProductDocument
└── root ComponentDefinition
    ├── Document
    │   ├── Sketch[]
    │   ├── ReferenceImage[]
    │   └── Body[]
    └── ComponentOccurrence[] -> ComponentDefinition
```

Каждый `ComponentDefinition` имеет стабильный `DefinitionId`, единолично
владеет своим `Document` и содержит дочерние occurrences. Occurrence сохраняет
стабильный `OccurrenceId`, rigid placement и presentation flags. `Body` не
становится definition.

Существующие `PartDefinition`, `AssemblyDefinition` и форматы V5–V9 остаются
строгим compatibility path. Их точные схемы не расширяются. Единый компонентный
граф получает новую версию envelope после отдельного persistence-контракта.
Скрытая пара `PartDefinition + AssemblyDefinition` не используется как
долгосрочная модель компонента.

## «Создать компонент из тела»

Команда доступна в новой группе «СБОРКИ» обычной Model ribbon и требует явно
выбранный `BodyId`. Она перемещает тело, а не копирует его:

1. проверяет dependency boundary;
2. вне committed state готовит родительский `Document` после извлечения и новый
   `Document` компонента;
3. переносит выбранное тело и эксклюзивные используемые им эскизы с сохранением
   `BodyId`, `FeatureId`, `SketchId` и persistent topology;
4. recompute обоих кандидатов должен завершиться успешно;
5. атомарно добавляет definition и occurrence с единичным placement;
6. публикует модель, дерево, сцену и одну Undo-команду только после полной
   подготовки.

В первом срезе любая зависимость через границу definition отклоняется как
`CrossDefinitionDependency`: cross-body feature, общий эскиз, face support или
topology reference на объект с другой стороны. Ошибка является точным no-op и
не расходует ID/history cursor.

`ReferenceImage` пока остаётся у родителя: текущая модель не связывает
изображение с конкретным телом. `DrawingSheetView` является производным
представлением, а не сохраняемой коллекцией чертежей; секция «Чертежи» не должна
притворяться реализованным владением до отдельной persistent-модели. Оба пробела
закрываются обязательными этапами этой программы, а не бессрочным backlog.

Move выбран как безопасная семантика первого среза: исходное тело сохраняет
идентичность и оказывается ровно в одном definition. Copy/clone требует
отдельных правил новых ID, topology references и владения зависимостями и не
подменяет первый инструмент неявно.

## Единые presentation и дерево

Один immutable project snapshot содержит собственные тела корневого компонента
с identity transform и тела descendant occurrences с composed transforms.
Mesh/B-Rep остаются definition-local и разделяются между occurrences.

Typed presentation address различает содержимое корня и descendants и включает
`DefinitionId`, optional полный `OccurrencePath`, kind узла и локальный model
ID. Невалидный фиктивный `OccurrenceId` для корня запрещён.

Отдельные `featureTree_` и `AssemblyTreeModel` заменяются единым
`ComponentTreeModel`. Каждый component occurrence раскрывает секции «Тела»,
«Эскизы», «Изображения» и «Компоненты». Секция «Чертежи» добавляется только
после появления authoritative drawing model. QModelIndex и widget pointers не
переживают перестроение; selection восстанавливается только по typed address.

## Прямое перетаскивание

Direct drag реализуется отдельной `ComponentDragSession`, а не существующим
`draggingBody_`:

- до порога 3 px действие остаётся кликом;
- mouse move меняет только transient preview;
- перемещение первого этапа идёт в camera-facing plane через точку захвата;
- release выполняет одну placement-команду и одну запись Undo;
- Escape, потеря mouse grab, смена проекта, stale publication и любая ошибка
  возвращают exact committed scene;
- grounded, suppressed, hidden и unresolved occurrence не перемещаются;
- placement-only edit не вызывает recompute, remesh или повторный GPU upload.

## Этапы поставки

1. **U1 — headless component model.** `ComponentDefinition`, registry/graph,
   recursive validation, typed failures и rollback. Без Qt, persistence и UI.
2. **U2 — extraction transaction.** Атомарное перемещение независимого тела и
   эксклюзивных эскизов в новый component; одна cold Undo/Redo command.
3. **U3 — persistence.** Новый strict envelope, hand-authored fixtures,
   staged materialization и compatibility/migration policy для V3–V9. Сначала
   поставляется U3a (codec/scanner/materializer/atomic writer без
   пользовательского open/save routing). V10 routing включается только после
   U4, когда единое представление уже умеет показывать component-root; это
   запрещает промежуточное открытие корректного файла в пустой viewport.
4. **U4 — unified presentation/tree.** Root content + descendants в одной
   publication, общий picking и `ComponentTreeModel`.
5. **U5 — обычная лента.** Группа «СБОРКИ», команда «Создать компонент из
   тела»; отдельные New Assembly/ribbon удаляются из нового пользовательского
   пути после готовности compatibility open.
6. **U6 — active component authoring.** Активация root или вложенного
   `ComponentDefinition` как текущего контекста редактирования; обычные Model,
   Sketch и Image tools работают с его `Document`. Breadcrumb/typed context
   позволяет вернуться к родителю; cross-definition references остаются
   запрещены, а смена контекста отменяет активные tool sessions до публикации.
7. **U7 — component drag.** Preview, Apply-on-release, Cancel/Escape,
   Undo/Redo и GPU/DPI manual gate.
8. **U8 — image ownership.** Явная metadata принадлежности изображения
   definition, перенос/создание/редактирование изображений в активном
   компоненте и strict persistence.
9. **U9 — persistent drawings.** Authoritative drawing collection в model,
   принадлежность component definition, persistence и работа обычных drawing
   tools в активном компоненте.

Каждый этап имеет отдельные targeted regressions, независимый review, clean CI
и verifier. U4–U9 дополнительно требуют real-GPU/UI smoke там, где применимо.
Нельзя объединять
модель, формат, дерево и drag в один непроверяемый diff.

## Отложено

- инструмент «Соединение» и UI сопряжений;
- cross-definition Part Design зависимости;
- copy/«сделать независимым»;
- collision/contact и кинематика.

## Последствия для прежнего A8a

Автоматические доказательства старого Mate tool сохраняются как историческое
evidence, но manual gate не пройден: пользователь отверг отдельное assembly
workspace. A8b и A9 по прежней UI-ветке не продолжаются. Headless occurrence,
renderer, topology и mate kernel могут быть переиспользованы после адаптации к
единому component graph; их наличие само по себе не активирует старый UI.
