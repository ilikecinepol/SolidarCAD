# Формат проекта SolidarCAD v7: плоская сборка со linked-деталями

V7 расширяет отдельный product envelope V6 новым вариантом payload детали и
обязательным authored-полем `name` у occurrence. Байты и schema V6 не
меняются. V7 пока используется через
disconnected codec API. A6b1 предоставляет явные cold Reload/Repair model и
application boundaries, но Open/Save в `MainWindow` остаются выключены до
A6b2.

Корень содержит ровно поля `format`, `version`, `capabilities`, `projectId`,
`rootDefinitionId`, `metadata`, `definitions`. `version` равна `7`, а
capabilities строго равны отсортированному набору `flat-assembly-v1`,
`linked-parts-v1`, `product-envelope-v1`. Неизвестные или лишние поля и
capabilities отклоняются.

Definitions строго отсортированы по уникальному положительному ID. Ровно одна
definition является корневой `assembly`; все остальные — локальные `part`.
Каждый occurrence обязан указывать на локальную part. Вложенная assembly и
target на assembly запрещены.

## Payload детали

Embedded-вариант не изменяет точный параметрический payload V4:

```json
{"payloadKind":"embedded-part-v4","data":{}}
```

Linked-вариант имеет ровно три поля:

```json
{
  "payloadKind": "linked-part-v1",
  "binding": {
    "projectId": "223e4567-e89b-42d3-a456-426614174001",
    "definitionId": 900,
    "relativePathHint": "library/part.solidar",
    "sha256": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
  },
  "lastValid": {}
}
```

`lastValid` — полный параметрический корень V4 и единственный материализованный
`Document`, принадлежащий `PartDefinition`. Binding не владеет вторым
Document/B-Rep. Внешний `binding.definitionId` не обязан совпадать с локальным
ID. SHA-256 — ровно 64 строчных hex-символа. Locator хранится в исходном UTF-8
виде, но обязан быть безопасным относительным путём по общей A6a policy:
запрещены absolute, UNC, device/ADS, NUL и выход `..`. Canonical path,
filesystem status, mesh, GPU и B-Rep не сериализуются.

Occurrence содержит ровно ID, target, rigid placement, `name`, `visible`,
`suppressed`, `grounded`. Definitions и occurrences кодируются канонически в
порядке ID. Encoder выполняет повторный strict scan своих байтов.

## Лимиты и материализация

- файл: общий лимит `ProjectFile` 64 MiB;
- definitions: 256; occurrences: 4096;
- JSON strings: 1 MiB UTF-8 суммарно;
- parametric payload cache: 48 MiB compact JSON суммарно;
- linked locator: 16 KiB UTF-8 суммарно;
- rebuild work: `ProjectFile::kMaximumDocumentRebuildWork`.

Scanner не выполняет I/O, resolver, OCCT или recompute. Materializer повторно
сканирует authored root, сравнивает candidate, затем в отключённом staging
материализует и recompute каждый cache. Только после полного успеха строится
`ProductDocument`; ошибка не публикует частичный продукт и не сдвигает
глобальные ID/cursors.
