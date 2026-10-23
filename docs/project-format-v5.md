# Product Envelope `.solidar` v5

Статус на 23 октября 2026 года: строгий scanner, materialization, encoder,
атомарный writer и production application/UI paths включены. Реализованы Open,
Save, явная conversion standalone V3/V4 и Save As для standalone и single-part
V5. Формат пока не описывает occurrences или assembly definition; umbrella A2
этим ограниченным срезом не закрыт.

Версия 5 — первый identity-bearing product envelope. В A2a она содержит ровно
одно корневое определение. Количество `definitions`, отличное от одного,
является `Invalid`. В единственном структурно корректном definition неизвестный
`definitionKind` (включая будущий assembly kind) даёт `Unsupported`. Несколько
definitions, occurrences и полноценная assembly schema относятся к A3b и в
A2a не открываются частично и не отбрасываются.

## Закрытая схема A2a

Корневой объект содержит ровно семь полей:

```json
{
  "format": "solidar-project",
  "version": 5,
  "capabilities": ["product-envelope-v1"],
  "projectId": "123e4567-e89b-42d3-a456-426614174000",
  "rootDefinitionId": 17,
  "metadata": {
    "name": "Изделие",
    "createdAt": "2026-10-23T00:00:00Z"
  },
  "definitions": [
    {
      "definitionId": 17,
      "definitionKind": "part",
      "payload": {
        "payloadKind": "embedded-part-v4",
        "data": {
          "format": "solidar-project",
          "version": 4,
          "capabilities": ["persistent-topology-v2"]
        }
      }
    }
  ]
}
```

Объект `data` в примере сокращён: в файле это полный закрытый корень
standalone v4 со всеми десятью полями. Он проходит без изменений через
существующую цепочку v4 envelope → payload → model validation. Product layer
не имеет второго feature/topology codec, не переназначает ID и не сериализует
derived B-Rep. Единственное уже существующее исключение — авторитетный `brep`
у `ImportedShape` внутри standalone v4.

Поля `name` и `createdAt` внешнего `metadata` обязаны побайтно совпадать с
одноимёнными полями вложенного v4. В single-part схеме v5 у них нет двух разных
семантик: расхождение считается `Invalid`, а encoder получает один явный набор
metadata и использует его на обоих уровнях.

`ProjectId` — 128 бит, представленные канонической UUID-строкой: только
строчные hex-цифры, группы `8-4-4-4-12`, без скобок. Nil UUID запрещён.
`DefinitionId` и `rootDefinitionId` — положительные точные JSON integers в
существующем persisted-ID диапазоне; в A2a они обязаны совпадать.

`capabilities` — непустой ограниченный массив непустых строк, строго
отсортированный и уникальный по байтам UTF-8. `product-envelope-v1`
обязательна. Неизвестная capability, `definitionKind`, `payloadKind` или
будущая семантика корректного nested v4 дают `Unsupported`, не частичное
открытие. Любая структурная ошибка известной v5 имеет приоритет и даёт
`Invalid`. Чужой format и версии 1–4 дают `NotApplicable`; целочисленная версия
выше 5 даёт `Unsupported`; malformed/non-integer version даёт `Invalid`.

## Identity и активный workflow

`ProductDocument` является единственным владельцем атомарного опционального
identity bundle: `ProjectId` и envelope metadata (`name`, `createdAt`) либо
присутствуют вместе, либо вместе отсутствуют у standalone-документа. Metadata
хранятся в model как неизменяемые по смыслу UTF-8 байты без нормализации и
локализации; persistence state identity не дублирует. Фабрика валидирует
persisted ID до переноса registry. Узкий `noexcept` post-write commit принимает
весь bundle только после успешной атомарной публикации: invalid/nil ID и любая
stale/repeated попытка ничего не меняют. Save As/clone кодирует явные output
identity и metadata и меняет identity активного carrier только после публикации.

| Текущее состояние | Команда | Результат |
| --- | --- | --- |
| Standalone V3/V4, один валидный root, без v1 legacy-prism marker | «Преобразовать в документ изделия…» | Выбранная цель получает V5 с новым `ProjectId`; source по умолчанию не перезаписывается, активный editor становится V5. |
| Standalone V3/V4 | «Сохранить как…» | Записывается тот же standalone format без identity; V3 остаётся V3, V4 остаётся V4. |
| Product V5 | Save | Перезаписывается текущий locator с теми же `ProjectId`, metadata и root ID. |
| Product V5 | «Сохранить как…» | Создаётся clone с новым `ProjectId` и metadata цели; root/local IDs и вложенный `Document` сохраняются. |
| Product V5 | Повторная conversion | Недоступна и не выполняет запись. |
| Historical v1 prism с marker в committed application state | Conversion | Безусловный fail-closed до chooser/prepare/writer/I/O; semantics-preserving migration отложена в A2g. |

Conversion разрешена для marker-absent v1-like carrier и авторитетных V2-V4,
которые application представляет явным standalone V3/V4 persistence state.
Любой путь проверяет coherent identity/root state, отсутствие активной
transient-операции и неизменность editor snapshot после файлового диалога.
Ошибка encode/write/flush/sync/replace оставляет source, destination и активный
carrier без изменений.

- A2b0/A2b1 добавили общий format-neutral atomic publication и disconnected V5
  encoder/writer; A2c материализовал V5 и подготовил атомарную conversion;
  A2d включил Open/Save, explicit Convert и product-aware Save As clone.
- A2e1 (`89d3184`) закрыл подтверждённую потерю historical v1 extrusion:
  marker в committed application state безусловно блокирует conversion до
  chooser/prepare/writer/I/O. Service доверяет переданному typed precondition;
  production UI передаёт фактический marker текущего state.
- A2e2 (`022f97e`) вызывает `scanProductEnvelopeV5(QByteArrayView)` напрямую
  для каждого fuzz input до legacy file path. Runner кладёт в mutable corpus
  checked-in `minimal-v1.solidar` и канонический `minimal-v5.solidar`; crash,
  UB и hang являются oracle. Linux non-MSVC fuzz preset остаётся отдельным
  запуском, не заменяемым MSVC unit test.

## Открытые границы

- **A2f — Relocate/Move:** операция с сохранением `ProjectId` не реализована;
  сначала нужна filesystem/replica transaction policy. Она обязана определить
  canonical same-target/file-identity aliases (symlink, junction/reparse point,
  hardlink), same- и cross-volume algorithms, overwrite, dirty/savepoint и
  external-modification policy, source-delete failure после publication,
  rollback/crash recovery и явное two-replica state. Save As не является
  relocate и всегда создаёт новый identity для V5.
- **A2g — historical v1 prism migration:** marker-bearing legacy extrusion
  пока только fail-closed; автоматическая миграция обязана сохранить семантику,
  а не синтезировать непроверенную историю.
- **A6a — identity conflict enforcement:** правило same
  `ExternalSourceKey`/different revision уже нормативно, но runtime resolver и
  conflict enforcement ещё не реализованы.

Multiple definitions, assembly roots, occurrences, linked definitions и mates
не являются частью текущей production V5 schema.

Реализация и CI regressions покрывают этот bounded slice, но итоговый A2
evidence gate остаётся открытым до записанного Linux Clang non-MSVC fuzz run.
Umbrella A2 также остаётся открытым.
