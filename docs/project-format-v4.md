# Формат проекта `.solidar` v4

Статус: P0-P3 завершены 23 октября 2026 года; standalone v4 доступен через
production reader, атомарный writer и application format-state gate.

Версия 4 — самостоятельная схема однодетального параметрического документа с
поддержкой persistent topology v2. Она не является product/component envelope,
не вводит `ProjectId`, `DefinitionId`, occurrences или assembly root. Будущий
identity-bearing envelope получает отдельную версию схемы строго выше `4`.
Как и v1-v3, v4 не сериализует derived B-Rep или runtime lineage index: они
детерминированно перестраиваются из feature parameters и history provenance.

Default для новых и открытых v1-v3 документов остаётся V3. Writer выбирается
явным `ProjectPersistenceState`: открытый V4 остаётся V4, а наличие in-memory
lineage само по себе не меняет формат.

## P1 runtime authoring scope

На P1 exact lineage публикуют только строгие production-пути:

- single-profile Extrude NewBody, strict sketch/face Join и Cut;
- single-profile Revolve NewBody и strict Join/Cut;
- strict Pocket;
- JoinBodies при `CompleteFaces` у обоих inputs;
- Move, Mirror, LinearPattern и CircularPattern от `CompleteFaces` source.

Edge key существует только для уникальной замкнутой двухгранной manifold
границы. Seam, open, non-manifold, degenerate и повторяющиеся face-pair edges
дают runtime `Missing`. Sketch support, face Extrude, face-plane Mirror, Shell,
Draft, Fillet и Chamfer потребляют exact V2. Fillet, Chamfer, Shell и Draft в
P1 остаются consumer-only terminal outputs и не публикуют lineage результата.

Этот runtime producer allowlist не является structural allowlist decoder-а.
Структурно валидная V2 reference может указывать на существующий
Fillet/Chamfer/Shell/Draft feature; завершённый P2 декодирует её как обычный V2
carrier. После recompute отсутствие соответствующего exact key даёт runtime
`Missing`, а не structural `Invalid`. Decoder не выводит поддержку authoring из
типа owning feature и не понижает такой carrier до V1.

## Корень и capability

Корень v4 закрытый и содержит ровно десять обязательных ключей: `format`,
`version`, `capabilities`, `constraintTypeEncoding`, `name`, `createdAt`,
`document`, `sketches`, `extrusion`, `model`. Дополнительные ключи запрещены.
`format` равен `solidar-project`, `version` равен `4`,
`constraintTypeEncoding` равен `stable-name-v1`; `name` и `createdAt` сохраняют
точную семантику v3. `document` содержит ровно `widthMm`, `heightMm`,
`extrusionMm`; `model` — ровно `sketches`, `bodies`, `referenceImages`.
`sketches`, `extrusion` и все non-topology subfields `model` сохраняют точную
v3-схему; topology embeddings заменяются только по закрытому списку ниже.
Новый `capabilities` обязателен:

```json
{
  "version": 4,
  "capabilities": ["persistent-topology-v2"]
}
```

Показанный объект сокращён: остальные поля корня — те же обязательные поля
однодетального payload, что и в v3.

`capabilities` имеет следующие правила:

- это обязательный массив от 1 до 16 непустых строк, каждая длиной не более
  64 байт UTF-8;
- значения уникальны и отсортированы по возрастанию байтов UTF-8;
- `persistent-topology-v2` обязательно присутствует;
- каждая capability является обязательной для корректной интерпретации файла,
  а не информационной подсказкой.

Отсутствующий массив, отсутствие `persistent-topology-v2`, повтор, неверный
порядок или нестроковый элемент делают v4 структурно `Invalid`. Синтаксически
корректная неизвестная capability означает `Unsupported`: reader не открывает
документ частично и не удаляет неизвестные данные при следующем Save.

## Tagged topology reference

Каждое место схемы, которое хранит face/edge reference, в v4 содержит ровно
один из двух tagged variants. Обязательный discriminator называется
`referenceVersion` и в этой версии принимает только строки `V1` или `V2`.
Неявный выбор варианта по наличию отдельных полей запрещён. В одном
v4-документе разрешена любая смесь V1- и V2-ссылок.

Ожидаемый потребителем kind (face или edge) обязан совпадать с `kind` ссылки.
Все ID — положительные JSON integers в существующем диапазоне persisted ID;
`legacyIndex` и `slot` — неотрицательные exact JSON integers. Объекты ниже
закрытые: поля сверх перечисленных запрещены.

### Variant V1

V1 сохраняет существующую модель owner/tag/signature/index без изменения
resolver behavior:

```json
{
  "referenceVersion": "V1",
  "bodyId": 12,
  "featureId": 34,
  "kind": "Face",
  "legacyIndex": 0,
  "persistentTag": "planar:max-z",
  "signature": {
    "surfaceType": 1,
    "area": 200.0,
    "centroid": [5.0, 10.0, 20.0],
    "normal": [0.0, 0.0, 1.0],
    "bounds": {
      "minimum": [0.0, 0.0, 20.0],
      "maximum": [10.0, 20.0, 20.0]
    },
    "radius": 0.0,
    "axis": [0.0, 0.0, 0.0]
  }
}
```

Точный набор обязательных полей V1: `referenceVersion`, `bodyId`, `featureId`,
`kind`, `legacyIndex`. `persistentTag` и `signature` опциональны и при
отсутствии не записываются. `persistentTag` не может быть пустым, если поле
присутствует. `signature` обязана соответствовать `kind`:

- Face: ровно `surfaceType`, `area`, `centroid`, `normal`, `bounds`, `radius`,
  `axis`;
- Edge: ровно `curveType`, `length`, `midpoint`, `tangent`, `bounds`, `radius`,
  `center`.

`bounds` содержит ровно `minimum` и `maximum`; каждая точка/направление — массив
ровно из трёх конечных чисел. Числовые enum values, диапазоны, нормализация
направлений и tolerance policy остаются контрактом V1. `surfaceType` принимает
`Unknown = 0`, `Plane = 1`, `Cylinder = 2`; `curveType` принимает `Unknown = 0`,
`Line = 1`, `Circle = 2`, `Ellipse = 3`, `Other = 4`. V1 resolver сохраняет
порядок tag -> signature -> legacy index только при отсутствии persistent
evidence, описанный в `docs/persistent-topology.md`.

### Variant V2

V2 содержит только owner body и точный typed provenance key:

```json
{
  "referenceVersion": "V2",
  "bodyId": 12,
  "provenance": {
    "featureId": 34,
    "kind": "Face",
    "operation": "Generated",
    "role": "Side",
    "localName": "side:geometry:7",
    "origins": [
      {
        "featureId": 21,
        "kind": "Face",
        "operation": "Seed",
        "role": "SketchBoundary",
        "localName": "geometry:7"
      }
    ]
  }
}
```

Точный набор полей V2 reference: `referenceVersion`, `bodyId`, `provenance`.
`provenance` содержит ровно обязательные поля `featureId`, `kind`, `operation`,
`role`, `localName`, `origins` и опциональное `instanceKey`. Origin содержит
ровно `featureId`, `kind`, `operation`, `role`, `localName` и опциональное
`instanceKey`; origin не имеет собственного `origins` и поэтому ancestry всегда
ограничена одним непосредственным уровнем.

`instanceKey`, если присутствует, содержит ровно:

```json
{
  "featureId": 34,
  "slot": 2
}
```

Его `featureId` обязан совпадать с `featureId` содержащего key/origin.
`instanceKey` — authored slot внутри topology producer; это не assembly
`OccurrenceId`, не `OccurrencePath` и не часть будущей product identity.
Пустой `localName`, нулевой ID или owner mismatch делают ссылку `Invalid`.
Top-level `provenance.featureId` обязан принадлежать именно body, указанному в
`bodyId`. `featureId` каждого immediate origin является document-global:
соответствующая feature обязана существовать в любом body загружаемого
standalone document, а cross-body origin разрешён. `bodyId` не применяется к
origins и не заставляет их принадлежать owning body.

Допустимые строковые enum tokens:

| Поле | Значения |
| --- | --- |
| `kind` | `Face`, `Edge` |
| `operation` | `Seed`, `Generated`, `Modified`, `Retained`, `Merge` |
| `role` | `SketchBoundary`, `Cap`, `Side`, `Original`, `Reflected`, `PatternInstance`, `BooleanResult`, `Custom` |

`origins` — set-like evidence. Writer канонизирует его полной
сортировкой по tuple `featureId`, `kind`, `operation`, `role`, UTF-8 bytes
`localName`, presence `instanceKey`, `instanceKey.featureId`, `instanceKey.slot`.
Enum order совпадает с порядком tokens в таблице, отсутствие instanceKey идёт
раньше присутствия. Writer удаляет только точные дубликаты; reader требует уже
канонического порядка без повторов. Два origin, отличающиеся operation, role
или instanceKey, различны. Authored slot внутри ключа не переставляется.

V2 resolver выполняет только exact lookup полного provenance key внутри
lineage указанного owning feature. V2 object намеренно не содержит
`legacyIndex`, `persistentTag` или `signature`; при `Missing` или `Ambiguous`
переход к V1 resolver запрещён. Конвертация malformed V2 object в V1 также
запрещена.

## Reference-bearing embeddings

Reference всегда вложен целиком в закрытый embedding wrapper и никогда не
смешивает свои поля с полями feature. Обычный wrapper содержит ровно одно поле
`reference`, значением которого является полный V1/V2 object из предыдущего
раздела. Элементы reference-массивов являются такими же wrappers, а не голыми
reference objects. В v4 разрешены ровно следующие embeddings:

| Путь | Cardinality | Ожидаемый `kind` |
| --- | --- | --- |
| `model.sketches[*].support.reference` при face support | `support` содержит ровно прежний `type` и новый `reference` | `Face` |
| `model.bodies[*].features[*].face.reference` для face-source Extrude | поле `face` — один обязательный wrapper | `Face` |
| `model.bodies[*].features[*].edges[*].reference` для Fillet | `edges` — непустой список wrappers | `Edge` |
| `model.bodies[*].features[*].edges[*].reference` для Chamfer | `edges` — непустой список wrappers | `Edge` |
| `model.bodies[*].features[*].planeFace.reference` для face-plane Mirror | поле `planeFace` — один обязательный wrapper для этого plane kind, отсутствует для datum plane | `Face` |
| `model.bodies[*].features[*].removedFaces[*].reference` для Shell | `removedFaces` — непустой список wrappers | `Face` |
| `model.bodies[*].features[*].neutralPlaneFace.reference` для face-neutral Draft | поле `neutralPlaneFace` — один обязательный wrapper для этого neutral-plane kind | `Face` |
| `model.bodies[*].features[*].rotationEdge.reference` для Draft | поле `rotationEdge` — ноль или один wrapper | `Edge` |
| `model.bodies[*].features[*].draftedFaces[*].reference` для Draft | `draftedFaces` — непустой список wrappers | `Face` |

Для face support существующий `support` сам служит embedding wrapper: V1/V2
поля больше не лежат рядом с `type`, а находятся только в `reference`. Для
datum support `support` содержит ровно прежний `type`; поле `reference`
запрещено. Во всех feature sites внешний wrapper содержит ровно `reference`.

Остальные поля каждого feature, массивов `model.bodies`, `model.sketches` и
legacy projection `sketches`/`extrusion` остаются точной v3-схемой. Topology
reference в любом ином поле, inline V1/V2 fields рядом с feature fields и
hybrid object с полями обоих variants являются `Invalid`.

## Смешанные ссылки и сохранение

V4 не требует переписывать каждую существующую ссылку в V2. Структурно валидная
authored V2 reference сохраняется буквально даже тогда, когда текущий recompute
даёт `Missing` или `Ambiguous`. Writer не опрашивает текущую lineage, чтобы
выбрать variant, понизить V2 до V1 или удалить ссылку. Реализованный writer P3
сохраняет следующие обязательства:

- сохранять V1/V2 variant и все identity fields буквально при load/save, если
  пользователь не совершил явную операцию, создающую или заменяющую ссылку;
- при создании новой ссылки брать variant из явного результата authoring API,
  а не выводить его из текущего успеха resolver;
- никогда не синтезировать V2 из V1 signature/index и не понижать V2 до V1.

Смешанный документ обязан разрешать каждую ссылку её собственным resolver path.
Ошибка одной ссылки не меняет variant соседних ссылок.

## Проверка, лимиты и классификация

V4 наследует без расширения следующие точные ограничения v3:

- файл — не более 67108864 байт (64 MiB), B-Rep base64 field — не более
  67108864 characters;
- persisted ID — `1..9007199253740991`, прочие неотрицательные exact JSON
  integers — `0..9007199254740991`;
- bounded string — не более 16384 UTF-16 code units, общий collection — не
  более 100000 элементов;
- один sketch — не более 10000 geometry items, 5000 constraints и 2000000
  единиц solve complexity;
- весь document — не более 50000 sketch geometry items, 20000 constraints,
  5000000 единиц sketch solve complexity, 4096 sketches, 256 bodies, 4096
  features и 10000 единиц aggregate rebuild work;
- model length/offset fields сохраняют v3 bound 100000 mm с прежним знаком и
  lower-bound каждого конкретного поля; pattern count остаётся `2..100`,
  spacing `0.01..100000` mm, angle `0.01..360` degrees;
- все floating-point values обязаны быть конечными, а signature bounds и
  direction/tolerance invariants остаются точным контрактом v3.

Дополнительные точные v4 limits:

- не более 1024 topology references в одном feature object;
- не более 4096 topology references суммарно в document, включая sketch
  supports и все feature sites;
- не более 256 immediate origins в одной V2 reference;
- не более 65536 provenance records суммарно в document; top-level
  `provenance` и каждый origin считаются отдельными records;
- `localName` — не более 16384 байт UTF-8;
- сумма UTF-8 bytes всех `localName` top-level key и origins — не более
  256 KiB на reference и не более 8 MiB на document;
- `capabilities` — не более 16 элементов, каждый не более 64 байт UTF-8;
- `slot` — JSON exact integer в диапазоне `0..9007199254740991`.

Origins не рекурсивны: origin с полем `origins` всегда `Invalid`. Лимиты
проверяются до аллокации итогового `Document`; отдельные безлимитные аллокаторы
для v4 запрещены.

До создания `Document` reader проверяет:

1. exact root schema и capability contract;
2. discriminator и exact field set каждого variant;
3. ID ranges, known enum tokens, kind compatibility и instanceKey ownership;
4. canonical sort/dedup origins;
5. существование `bodyId`, принадлежность top-level `provenance.featureId`
   именно этому body и document-global существование каждого origin
   `featureId` (cross-body origins разрешены);
6. все остальные cross-reference и numerical invariants v3.

Классификация результата:

| Случай | Результат |
| --- | --- |
| Валидный standalone v1-v3 | Существующий standalone reader; семантика не меняется |
| Валидный v4 со всеми известными capabilities | Строгий staged decode/materialize/recompute; application open с V4 допускает только primary failures `SketchSupport`/`Feature` с кодом `TopologyReferenceMissing`/`TopologyReferenceAmbiguous`, прочие отклоняются |
| Версия выше поддерживаемой или синтаксически корректная строка неизвестной capability, `referenceVersion`, `kind`, `operation` или `role` | `Unsupported` |
| Неверный JSON type, отсутствующее/лишнее поле, hybrid V1/V2 object, duplicate/stale ID, превышение лимита или неканонические origins | `Invalid` |
| Известный `kind`, не совпадающий с ожидаемым kind embedding site | `Invalid` |
| Exact V2 key отсутствует или неоднозначен после recompute | Документ структурно загружен, ссылка имеет runtime-состояние `Missing`/`Ambiguous`; V1 fallback отсутствует |

Для известного `referenceVersion` (`V1` или `V2`) structural defect имеет
приоритет над неизвестным внутренним строковым token: лишнее поле, неверный
тип, диапазон или превышение budget дают `Invalid`, даже если `kind`,
`operation` или `role` одновременно неизвестен. Если иных дефектов нет,
неизвестный внутренний token даёт `Unsupported`. Корректная непустая bounded
строка неизвестного `referenceVersion` считается opaque future variant и даёт
`Unsupported` без применения к её остальным полям схемы V1/V2; отсутствующий,
нестроковый, пустой или oversized discriminator остаётся `Invalid`.

`Unsupported` и `Invalid` не публикуют частичный `Document`, не меняют уже
открытый документ и не разрешают Save поверх входного файла.

## Совместимость и rollout

- Reader v4 обязан продолжать читать v1, v2 и v3 прежними путями, без
  обязательной миграции и без изменения их байтов на обычном open.
- Reader v1-v3 не обязан читать v4 и должен отклонить неизвестную версию.
- **P1 (завершён 21 октября 2026):** in-memory carrier и все разрешённые
  consumers/producers, включая
  fail-closed lifecycle regressions и terminal-output negative matrix.
- **P2 (завершён 22 октября 2026):** low-level decoder/structural codec staging
  и его тесты; encoder/writer в границу P2 не входил. Историческая граница P2
  не регистрировала V4 decoder в
  `ProjectApplicationService`, UI open, recent/recovery или ином
  application-open path и не создавала доступный приложению `Document`; до P3
  application-пути записи V4 оставались запрещены.
- **P3 (завершён 23 октября 2026):** готовый атомарный writer и V4 codec включены
  в application open, deterministic save/load matrix и native-authoring gate
  закреплены commits `8043f70` и `bdba8c9`. P1 ранее заморозил boolean provenance
  algebra: неизменяемые правила `Generated`, `Modified`, `Retained` и `Merge`,
  N-to-1 merge, 1-to-N split, canonical sort/dedup origins, уникальные authored
  local names либо `Ambiguous`, отсутствие разрешаемого key для deleted inputs,
  а также partial/complete coverage и collision policy. P3 сохранил эту
  алгебру и подтвердил её save/load regressions без переосмысления. Изменение
  алгебры после выпуска v4 потребует новой capability/schema, а не тихой смены
  смысла сохранённых keys.

Завершённый P3 не меняет default writer на V4: default остаётся V3 для новых и
открытых v1-v3 документов. Конкретный V3/V4 writer выбирается явным persistence
state, а не автоматически вследствие наличия V2 carrier.

Текущий документ несёт явное persistence format state, независимое от
текущего содержимого:

- открытый v4 остаётся v4 при Save и Save As, даже если все V2 references были
  удалены;
- открытые v1-v3 и новые документы остаются v3 при Save и Save As;
- отдельная явная атомарная команда `Upgrade to v4` переводит standalone state
  V3 -> V4 и не создаёт product envelope или external identity;
- authoring/commit native V2 reference в состоянии v3 запрещён и требует, чтобы
  пользователь сначала выполнил отдельную явную команду upgrade; создание
  V2-ссылки само по себе не меняет format state, а V2 carrier никогда не
  записывается в v3;
- удаление последней V2 reference не выполняет content-driven downgrade.

Любой Save пишет и fsync-проверяет временный payload, затем атомарно заменяет
destination. Ошибка encode/write/flush/replace сохраняет прежний файл и прежнее
persistence format state; частичный v4 не публикуется.

## Будущий product envelope

Product/component envelope из ADR
`docs/decisions/2026-10-18-product-document-envelope.md` не является v4. Его
первая схема получает номер выше `4` и отдельный capability/reader/writer gate.
Будущая `Convert to Product Document` — отдельная операция, не `Upgrade to v4`.
Она переводит standalone v1-v4 в envelope схемы выше 4 и обязана сохранить все
существующие Body/Feature/Sketch/Geometry и прочие stable IDs, variant каждой
topology reference и полную V2 provenance; файловый путь не становится
identity. Ошибка оставляет исходный standalone-файл неизменным и не публикует
частичный envelope. Политики Save/Save As/Relocate, replica и identity conflict
для envelope этим standalone gate не изменены и остаются будущей работой A2+.
