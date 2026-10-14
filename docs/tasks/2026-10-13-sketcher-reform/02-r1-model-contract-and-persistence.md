# R1 — типизированный model contract и persistence

## Условие старта

Начинать R1 можно только после принятого отчёта R0. Атомарность нельзя временно
сломать ради миграции типов.

## Цель

Сделать геометрию, ограничения и размерные annotations однозначными данными,
которые невозможно сконструировать в противоречивом состоянии. Удалить поиск
связи `Dimension ↔ Constraint` по совпадению типа и geometry references.

## Не входит в R1

- переключение production solve на Solver v2;
- новая математика constraints;
- добавление новых типов геометрии или инструментов;
- удаление чтения старых `.solidar`.

## Шаг 1. Описать канонические сущности

1. Ввести tagged point reference с ровно одним источником:
   - datum origin;
   - line endpoint;
   - circle center;
   - arc endpoint;
   - Bezier endpoint/control point;
   - composite element center.
2. Невозможно представить одновременно line endpoint и circle center.
3. Все ссылки используют стабильные IDs, не vector indices.
4. Ввести типизированные payloads constraints. Поля, не относящиеся к типу,
   физически отсутствуют, а не заполняются нулями.
5. Общая оболочка constraint хранит `ConstraintId`, mode и payload.
6. Mode как минимум различает `Driving`, `Reference` и `Disabled` там, где это
   семантически применимо.

## Шаг 2. Сделать одно authoritative значение размера

1. Driving dimension хранит числовое значение только в constraint payload.
2. Presentation annotation хранит placement/offset и точный `ConstraintId`.
3. Reference dimension хранит измеряемую ссылку и presentation metadata, но не
   создаёт hard equation.
4. Удаление annotation не может удалить другой похожий constraint.
5. Переключение Driving ↔ Reference изменяет ровно одну известную связь.
6. `Dimension::valueMm` перестаёт быть параллельным источником истины.

## Шаг 3. Централизовать schema validation

1. Создать единственную model-level validation boundary.
2. Она проверяет существование IDs, совместимость geometry kinds, допустимый
   диапазон параметров, отсутствие self-reference и uniqueness ID.
3. UI, project loader и solver не поддерживают собственные расходящиеся копии
   schema switch.
4. Validation возвращает typed error code и связанные IDs.
5. Неизвестный persisted type отвергается диагностируемо; известный конфликтный
   sketch может быть открыт для ремонта без частичного solve.

## Шаг 4. Версионировать persistence

1. Подготовить отдельное описание формата, если новая схема требует `.solidar`
   v3. Молчаливо менять v2 запрещено.
2. Сохранить чтение v1/v2.
3. При migration:
   - каждому constraint и annotation назначить стабильную связь;
   - неоднозначность не угадывать;
   - ambiguous historical dimension переводить в reference либо выдавать
     диагностируемую ошибку согласно заранее зафиксированной policy;
   - не менять геометрию только ради успешной миграции.
4. Повторный save/load не должен менять IDs, order или semantic fingerprint.
5. Старый reader не обязан читать v3, но новый reader обязан детерминированно
   читать все поддерживаемые старые версии.

## Шаг 5. Перевести потребителей

Перевести и проверить:

1. Sketch mutators и constraint graph;
2. `SketchCommandController`;
3. `SketchCanvas`, renderer и constraint panel;
4. hit testing и selection;
5. Undo/Redo delta journal;
6. project codecs;
7. semantic и solver fingerprints;
8. diagnostics и benchmark fixtures.

Старые compatibility helpers удалять только после `rg`, подтверждающего ноль
production call sites.

## Шаг 6. Тесты

Обязательны:

1. compile-time/constructor tests, исключающие смешанную point reference;
2. schema matrix для каждого constraint type и всех допустимых/недопустимых
   geometry combinations;
3. две похожие dimensions на одной component: удаление/переключение одной не
   затрагивает другую;
4. delete geometry удаляет или диагностируемо инвалидирует ровно связанные
   constraints/annotations;
5. Undo/Redo сохраняет IDs и modes;
6. v1/v2 migration fixtures и v3 round-trip;
7. malformed/duplicate/stale IDs не меняют открытый Document;
8. save/load конфликтного, но структурно валидного sketch;
9. property test для случайных insert/delete/reorder с проверкой ссылочной
   целостности.

## Параметры завершения R1

- [ ] невозможно создать PointReference с несколькими источниками;
- [ ] каждый constraint имеет типизированный payload;
- [ ] validation logic существует в одном model-level registry/boundary;
- [ ] driving dimension имеет ровно одно authoritative numeric value;
- [ ] annotation ссылается на точный `ConstraintId`, без heuristic matching;
- [ ] переключение/удаление dimension затрагивает ровно ожидаемый constraint;
- [ ] v1/v2 читаются, новая версия round-trip стабильна;
- [ ] malformed migration fail-closed и не меняет открытый проект;
- [ ] R0 atomicity regressions продолжают проходить;
- [ ] clean `ci`, dev verifier, manual save/load/Undo/Redo smoke зелёные;
- [ ] независимый reviewer не оставил HIGH findings;
- [ ] отчёт R1 содержит таблицу старой и новой схемы и доказательство миграции.
