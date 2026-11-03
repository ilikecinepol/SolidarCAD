# Формат проекта V12

V12 расширяет строгий component envelope V11 сохранённым исходным положением
экземпляров компонентов. Корневой объект и определения остаются такими же,
как в V11; версия равна `12`, а отсортированный набор capabilities содержит:

- `component-definitions-v1`;
- `component-drawings-v1`;
- `component-home-placement-v1`;
- `nested-components-v1`;
- `product-envelope-v1`.

Каждый объект `occurrences[]` обязан содержать ровно поля
`occurrenceId`, `targetDefinitionId`, `placement`, `homePlacement`, `name`,
`visible`, `suppressed`, `grounded`. `placement` и `homePlacement` — независимые
parent-local rigid transforms с полями `translationMm[3]` и
`quaternionWxyz[4]`. Числа конечны, translation и quaternion записаны в
канонической форме; отрицательный ноль запрещён в обеих массивах.

V10/V11 при загрузке мигрируют `homePlacement = placement`. Их writers не
записывают V12-поле и отклоняют модель, если две позы различаются. Первое
изменение текущего или исходного положения переводит проект в V12; дальнейшее
сохранение не понижает формат.

Writer V12 выполняет encode → strict scan → validation-only materialization →
encode fixed point, повторяет scan/materialization/encode для проверки
стабильности и только затем атомарно заменяет файл назначения.
