# Формат проекта V11

V11 расширяет строгий component-envelope V10 сохранением чертежей компонентов.
Маршрутизация приложения в этот этап не входит: codec используется напрямую.

Корневой объект сохраняет поля V10, `version` равен `11`, а точный
отсортированный список capabilities равен:

- `component-definitions-v1`
- `component-drawings-v1`
- `nested-components-v1`
- `product-envelope-v1`

Каждое component definition обязательно содержит `drawings` в дополнение к
V10-полям. Порядок массива является авторским порядком чертежей определения.

```json
{
  "drawingId": 1,
  "name": "Основной вид",
  "paper": "A4",
  "orientation": "portrait",
  "scale": 1,
  "sourceBodyIds": [10, 20]
}
```

`drawingId` — положительный persisted ID, уникальный внутри определения.
`name` — непустая ограниченная UTF-8 строка. Единственное значение `paper` —
`A4`; orientation — `portrait` или `landscape`. Scale должен быть конечным,
положительным, каноническим и не может быть `-0`. `sourceBodyIds` строго
возрастает, не содержит повторов и ссылается только на тела собственного
embedded V4 Document. Пустой массив означает все тела.

Scanner применяет общие V4/V10 бюджеты и отдельные совокупные бюджеты числа
чертежей, ссылок на тела, строк и drawing-work ко всем определениям, включая
недостижимые. Неизвестные поля, capabilities и enum-значения отклоняются.

Materializer повторно сканирует компактное authored JSON, сравнивает полный
candidate, материализует и recompute-ит каждый V4 Document, после чего строит
`ComponentDefinition` с точной копией чертежей. Validation-only не резервирует
глобальные ID; publication фиксирует ровно одну incarnation.

Atomic writer выполняет encode → scan → validation materialization → encode
до fixed point, повторяет цикл для stable point и только затем открывает
транзакцию назначения. Ошибка подготовки не изменяет destination.
