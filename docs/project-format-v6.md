# Формат проекта SolidarCAD v6: плоская статическая сборка

Версия 6 является отдельным конвертом изделия. Версия 5 закрыта и не
расширяется. V6 сохраняет только параметрические определения деталей и
ссылочную структуру плоской сборки; B-Rep и полигональные сетки в occurrence
не сохраняются.

## Корневой объект

Корень содержит ровно семь полей:

```json
{
  "format": "solidar-project",
  "version": 6,
  "capabilities": ["flat-assembly-v1", "product-envelope-v1"],
  "projectId": "123e4567-e89b-42d3-a456-426614174000",
  "rootDefinitionId": 100,
  "metadata": {"name": "assembly", "createdAt": "..."},
  "definitions": []
}
```

Capabilities, definitions и occurrences записываются в строго возрастающем
порядке и не содержат дублей. Обязательны `flat-assembly-v1` и
`product-envelope-v1`. Неизвестная, но структурно корректная capability
классифицируется как `Unsupported`.

`definitions` содержит ровно одно корневое определение `assembly`; остальные
элементы имеют вид:

```json
{
  "definitionId": 1,
  "definitionKind": "part",
  "payload": {"payloadKind": "embedded-part-v4", "data": {}}
}
```

`data` — полный корень существующего формата детали v4. `name` и `createdAt`
каждой вложенной детали обязаны совпадать с внешним `metadata`. Каждое
определение детали записывается один раз, в том числе неиспользуемое. Несколько
occurrence могут ссылаться на один `definitionId`.

Корневое определение сборки имеет вид:

```json
{
  "definitionId": 100,
  "definitionKind": "assembly",
  "occurrences": [{
    "occurrenceId": 11,
    "targetDefinitionId": 1,
    "placement": {
      "translationMm": [0, 0, 0],
      "quaternionWxyz": [1, 0, 0, 0]
    },
    "visible": true,
    "suppressed": false,
    "grounded": true
  }]
}
```

Placement состоит только из конечного переноса и ненулевого quaternion.
Quaternion нормализуется и канонизируется через `RigidTransform3d`; scale,
shear и reflection не представимы. Цель occurrence обязана быть существующим
`part`. Вложенные сборки, mates и любые дополнительные поля запрещены.

Канонизация quaternion использует сетку компонентов `2^-48` и фиксированное
представление: `C(C(q)) == C(q)` и `C(-q) == C(q)` побитно. Побитное равенство
также гарантируется для точно представимого умножения всего quaternion на
степень двойки, если оно не вызывает overflow или underflow. Нули записываются
как `+0`, а норма отличается от единицы не более чем на обычную погрешность
binary64. Сетка и запас fixed-point дают угловую ошибку порядка `1e-13` rad.
Математически близкие лучи, утратившие точную коллинеарность из-за независимого
IEEE-754 округления, не обязаны иметь побитно одинаковую каноническую запись.

## Лимиты

- размер файла: не более 64 MiB;
- definitions: не более 256;
- occurrences суммарно: не более 4096;
- UTF-8 байты всех JSON-строк, включая имена полей: не более 1 MiB;
- compact JSON всех `embedded-part-v4.data`: не более 48 MiB;
- сумма уже вычисленного v4 `rebuildWork`: не более
  `ProjectFile::kMaximumDocumentRebuildWork`.

Все суммы проверяются с защитой от переполнения до создания model candidate.
Scanner не материализует `Document`, не вызывает OCCT и не выполняет
recompute.

## Классификация

- версия ниже 6 или другой формат: `NotApplicable`;
- версия выше 6: `Unsupported`;
- неизвестная capability, kind, payload discriminator либо поддерживаемая
  структура с неизвестной семантикой вложенного v4: `Unsupported` с candidate;
- ошибка структуры, лимита, ID, ссылки, placement или метаданных: `Invalid`;
- полностью известный и корректный документ: `Candidate`.

Известная структурная ошибка имеет приоритет над отложенной неизвестной
семантикой. Materializer повторно сканирует `authoredRoot`, сравнивает полный
candidate и только затем локально материализует и recompute всех деталей в
порядке `DefinitionId`. Публикация `ProductDocument` выполняется только после
успеха всех деталей; ошибка не изменяет глобальные генераторы ID.
