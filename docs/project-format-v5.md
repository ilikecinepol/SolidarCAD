# Product Envelope `.solidar` v5

Статус: контракт и отключённый scanner реализованы в микрогейте A2a.
Production-dispatch, materialization, writer и UI в A2a намеренно не
подключены. Обычные Open/Save/Save As продолжают работать только с уже
поддерживаемыми standalone-форматами.

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

## Identity и следующие микрогейты

`ProductDocument` является единственным владельцем опционального `ProjectId`:
у standalone-документа identity отсутствует. Persistence state не дублирует
identity. A2a не генерирует UUID; он только валидирует persisted identity и
предоставляет узкий `noexcept` commit уже подготовленного валидного ID для
будущего post-write commit.

- A2b добавит disconnected encoder/writer и атомарную публикацию байтов.
- A2c добавит materialization и явную конвертацию standalone → product с
  инъецируемой генерацией identity.
- A2d подключит product-aware Open/Save/Save As и lifecycle/UI только после
  прохождения предыдущих независимых гейтов.
