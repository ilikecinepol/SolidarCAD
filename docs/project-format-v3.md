# Формат проекта `.solidar` v3

Версия 3 сохраняет параметрическую модель v2 без изменений её feature-части,
но заменяет представление данных Sketcher на типизированную схему
`typed-sketch-v1`. Поле корневого объекта `version` равно `3`. Reader v3
по-прежнему читает v1 и v2; reader v1/v2 не обязан читать v3.

## Идентичность геометрии

Каждый объект в `lines`, `circles`, `arcs` и `beziers` имеет положительное
целочисленное поле `id`. ID уникален в пределах sketch и сохраняется буквально
при save/load. Позиция объекта в JSON-массиве не является ссылкой.

## Point reference

Point reference — объект с обязательным discriminator `source` и ровно одним
набором полей:

| `source` | Поля |
|---|---|
| `datum-origin` | нет |
| `line-endpoint` | `geometryId`, `endpoint: start|end` |
| `circle-center` | `geometryId` |
| `arc-endpoint` | `geometryId`, `endpoint: start|end` |
| `bezier-point` | `geometryId`, `pointIndex: 0..3` |
| `composite-center` | `elementId` |

Лишние поля, неизвестный `source`, отсутствующий ID и несовместимый geometry
kind являются ошибкой загрузки.

## Constraint

Constraint хранится как оболочка:

```json
{
  "id": 42,
  "mode": "driving",
  "payload": {
    "type": 5,
    "typeKey": "Length",
    "geometryId": 17,
    "value": 25.0
  }
}
```

`id` положителен и уникален. `mode` принимает `driving`, `reference` или
`disabled`. Набор полей payload определяется `type`; поля других constraint
types запрещены. `type` и стабильный числовой `typeKey` должны согласовываться.

Для размерных driving/disabled constraints единственное authoritative число
находится в `payload.value`. У reference constraint поля `value` нет: значение
измеряется по текущей геометрии и не образует hard equation. Reference mode
разрешён только для размерных constraint types.

## Dimension annotation

Annotation не дублирует тип, geometry references или числовое значение:

```json
{
  "id": 9,
  "constraintId": 42,
  "offset": 4.0,
  "angle": 0.0
}
```

`constraintId` обязан указывать ровно на один существующий размерный
constraint. Один constraint не может иметь две annotations. Удаление и
переключение режима выполняются только по этой связи.

## Allocator cursors

Каждый sketch содержит обязательный объект `idAllocators` с точным набором
полей `nextElementId`, `nextGeometryId`, `nextConstraintId` и
`nextDimensionId`. Они сохраняют следующий свободный ID даже после удаления
объекта с максимальным ID. Поэтому save/load не переиспользует удалённые ID и
не меняет будущую последовательность вставок.

Все массивы `lines`, `circles`, `arcs`, `beziers`, `constraints`,
`dimensions`, `centerNodeElementIds` обязательны. Оболочка constraint, её
payload, point reference, annotation и `idAllocators` отклоняют лишние поля:
неизвестные данные не могут быть молча потеряны при следующем Save.

## Миграция v1/v2

Reader сначала декодирует историческую геометрию без её изменения, затем
строит связи annotations:

1. если историческая annotation однозначно совпадает с одним ещё не связанным
   размерным constraint, используется его существующий ID;
2. если совпадений нет или их несколько, угадывание запрещено — создаётся
   отдельный reference constraint со стабильным новым ID;
3. если вход уже содержит точный `constraintId`, он сохраняется буквально;
   stale или повторная связь отклоняется;
4. порядок geometry, constraints и annotations сохраняется.

Неизвестный type/schema, duplicate/stale ID, неверный geometry kind,
self-reference и недопустимое числовое значение приводят к fail-closed ошибке.
Открытый Document при этом не изменяется. Структурно валидный, но
переопределённый sketch загружается в исходной позе для диагностики и ремонта;
частичный результат equation solver не коммитится.
