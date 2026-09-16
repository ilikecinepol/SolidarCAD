# SolidarCAD — локальный фикс: автоматическая Projection грани при создании Sketch on Face

## Цель

Изменить поведение создания эскиза на выбранной плоской грани 3D-тела.

### Новое ожидаемое поведение

Когда пользователь:

1. выбирает грань существующего тела;
2. создаёт на этой грани новый Sketch;

контур **именно выбранной грани** должен автоматически добавляться в новый эскиз как обычная проекционная/reference geometry.

То есть пользователь сразу должен иметь в Sketcher:

- внешний контур выбранной грани;
- внутренние контуры/отверстия этой грани, если они есть;
- доступные endpoints/segments для snapping и dimensions;
- locked/dashed поведение, идентичное ручному инструменту Projection.

Ручной инструмент **Projection** после этого нужен только для геометрии, которая **не относится к support face текущего эскиза**:
- соседних граней;
- других рёбер тела;
- геометрии других элементов модели.

---

# 1. Работа только локально

Создать отдельную локальную ветку от актуального `main`.

```powershell
git switch main
git pull --ff-only origin main
git switch -c fix/auto-project-support-face
```

## Запрещено

- `git push`
- merge в `main`
- force push
- изменение удалённых веток

До ручной проверки пользователем исправление должно оставаться локальным.

---

# 2. Текущее устройство, которое нужно переиспользовать

Не строить новый параллельный механизм Projection.

В текущем коде уже есть необходимая инфраструктура.

## MainWindow

При создании Sketch on Face:

- определяется `currentSketchFaceReference_`;
- разрешается placement выбранной плоской грани;
- вызывается `configureSketchEditContext()`.

`configureSketchEditContext()` передаёт в `SketchCanvas`:

- placement;
- support shape;
- support face reference.

## SketchCanvas

`SketchCanvas::setSketchEditContext()` уже:

- строит `referenceBodyMesh_` для тела;
- resolve'ит support face;
- строит отдельный `referenceFaceMesh_` именно для выбранной грани;
- использует `referenceFaceMesh_` для отображения границы выбранной грани.

Таким образом, **`referenceFaceMesh_.edges()` является естественным источником для автоматической Projection support face**.

Также уже существует:

```cpp
bool SketchCanvas::projectReferenceEdge(std::size_t edgeVectorIndex)
```

Этот путь уже умеет:

- переводить 3D edge points в координаты Sketch через `referencePlacement_.toLocal(...)`;
- разбивать tessellated edge на segments;
- не добавлять уже существующую геометрию повторно;
- объединять сегменты одного ребра в один projected element;
- помечать projection как dashed;
- добавлять `ConstraintType::Lock`;
- откатывать projected element, если Lock не принят.

Эту логику необходимо **переиспользовать**, а не дублировать.

---

# 3. Главное архитектурное изменение

Сейчас `projectReferenceEdge()` одновременно выполняет две разные задачи:

1. преобразует `RenderEdge` в projected Sketch geometry;
2. оформляет пользовательское действие:
   - `pushUndoState()`;
   - UI-сообщения.

Для автоматической projection support face это неудобно.

## Требуется

Выделить общий внутренний helper, который умеет добавить в Sketch один `RenderEdge`.

Пример направления, имя можно выбрать лучше:

```cpp
bool SketchCanvas::appendProjectedEdge(
    const RenderEdge& edge,
    bool recordUndo,
    bool reportStatus);
```

или разделить ещё чище:

```cpp
bool SketchCanvas::appendProjectedEdge(const RenderEdge& edge);
```

а пользовательский wrapper оставить таким:

```cpp
bool SketchCanvas::projectReferenceEdge(std::size_t edgeVectorIndex) {
    ...
    pushUndoState();
    return appendProjectedEdge(referenceBodyMesh_.edges()[edgeVectorIndex]);
}
```

Точный API выбрать по текущему стилю кода.

## Важно

Не копировать большой блок из `projectReferenceEdge()` во второй метод.

Должен существовать **один authoritative path**, который создаёт projected geometry.

---

# 4. Автоматическая Projection support face

После успешного resolve выбранной face в:

```cpp
SketchCanvas::setSketchEditContext(...)
```

и после:

```cpp
referenceFaceMesh_.rebuild(*resolved.subshape);
```

для **нового пустого Sketch on Face** автоматически добавить:

```cpp
referenceFaceMesh_.edges()
```

в Sketch как projected geometry.

Каждое topological/render edge должно пройти через тот же общий helper, который используется ручной Projection.

---

# 5. Что именно проектировать

Проектировать **только boundary выбранной support face**.

Не проектировать автоматически:

- все видимые рёбра тела;
- соседние рёбра;
- задние рёбра;
- silhouette edges;
- рёбра других граней;
- весь `referenceBodyMesh_`.

Источником auto-projection должен быть:

```cpp
referenceFaceMesh_.edges()
```

а не:

```cpp
referenceBodyMesh_.edges()
```

Это принципиальное требование.

---

# 6. Внутренние контуры

Если выбранная грань имеет:

- отверстие;
- карман;
- несколько внутренних wires;

они также принадлежат выбранной грани и должны автоматически попасть в Sketch как Projection.

То есть нужно пройти **все** `referenceFaceMesh_.edges()`, а не только внешний контур.

---

# 7. Поведение projected geometry

Автоматически добавленная геометрия должна вести себя **точно как ручная Projection**.

Для каждого projected element:

- `dashed == true`;
- geometry locked;
- нельзя случайно переместить/resize;
- endpoints доступны для Coincident/Distance/DistanceX/DistanceY;
- snapping работает;
- projected geometry не становится обычной editable geometry;
- solver не должен считать её свободной.

Не вводить новый тип geometry специально для auto-projection в рамках этого локального фикса.

---

# 8. Undo semantics

Автоматическая Projection support face — это **исходное состояние нового Sketch**, а не пользовательская операция.

Поэтому при входе в новый Sketch on Face:

- не создавать отдельный Undo entry на каждое boundary edge;
- первый `Ctrl+Z` после того, как пользователь нарисовал свой первый объект, должен отменять именно пользовательский объект/операцию;
- он не должен по одному удалять автоматически добавленные рёбра support face.

Лучший вариант для текущего локального фикса:

> добавить auto-projection без вызова `pushUndoState()`.

Ручной Projection должен сохранить существующее Undo-поведение.

---

# 9. Повторное редактирование существующего Sketch

Это критический regression-case.

`setSketchEditContext()` используется не только при создании нового Sketch, но и при редактировании face-supported sketch.

Нельзя при каждом открытии существующего Sketch добавлять boundary projection заново.

Проверить текущий порядок:

```text
configureSketchEditContext()
loadSketch(existingGeometry)
```

Если auto-projection создаётся до `loadSketch()`, а `loadSketch()` полностью заменяет `sketch_`, временная projection не должна оставлять:

- Undo entries;
- duplicated geometry;
- stale selection;
- stale constraint state.

Предпочтительно сделать условие создания auto-projection явным, а не полагаться только на последующий overwrite.

Например:

- передавать через `SketchEditContext` признак нового sketch;
- либо использовать валидный/невалидный `sketchId`;
- либо отдельный параметр `autoProjectSupportFace`.

Выбрать минимальное решение, согласованное с существующей архитектурой.

### Требование

При повторном Edit Sketch:

- число projected elements не увеличивается;
- Lock constraints не дублируются;
- geometry не меняется;
- сохранённые пользовательские constraints остаются теми же.

---

# 10. Обычные datum-plane sketches

Для Sketch, созданного на:

- XY;
- XZ;
- YZ;

нового поведения быть не должно.

Auto-projection включается только для:

```text
SketchSupportType::Face
```

или эквивалентного текущего face-supported context.

---

# 11. Ручной Projection после auto-projection

После создания face sketch пользователь может включить инструмент Projection.

Если он кликает ребро, которое уже автоматически спроецировано:

- дубликат не создаётся;
- существующий duplicate guard должен продолжить работать;
- Sketch не должен получать второй Lock или второй совпадающий dashed element.

Если пользователь кликает другое ребро тела, которое не относится к support face:

- оно проецируется как сейчас;
- Undo работает как сейчас.

---

# 12. Не менять геометрическую точность в этой задаче

Текущий `projectReferenceEdge()` работает через sampled `RenderEdge.points` и переводит их в набор line segments.

В рамках этого фикса **не переписывать Projection на OCCT curve primitives**.

Это отдельное улучшение.

Сейчас auto-projection должна иметь ту же геометрическую точность и representation, что и существующая ручная Projection.

---

# 13. Тесты

Перед реализацией добавить/найти тест, который краснеет на текущем `main`.

Минимально нужны следующие случаи.

## Test A — rectangular planar face

Создать тело с прямоугольной плоской гранью.

Создать новый Sketch на этой грани.

Проверить:

- boundary автоматически присутствует в `Sketch`;
- присутствуют 4 projected edges/elements в ожидаемой форме;
- они dashed;
- они locked;
- пользователь не запускал Tool::Projection вручную.

---

## Test B — face with inner wire

Создать planar face с отверстием/внутренним контуром.

Новый Sketch on Face должен автоматически получить:

- внешний boundary;
- внутренний boundary.

---

## Test C — datum plane

Создать Sketch на XY/XZ/YZ.

Проверить:

- никаких auto-projected body edges не добавлено.

---

## Test D — reopen/edit

1. Создать Sketch on Face.
2. Убедиться, что support boundary спроецирован.
3. Добавить пользовательскую geometry/constraint.
4. Завершить Sketch.
5. Снова открыть Edit Sketch.

Проверить:

- projected geometry не продублировалась;
- количество constraints не выросло;
- пользовательская geometry сохранилась.

---

## Test E — manual duplicate

После автоматической Projection попытаться вручную спроецировать одно из тех же support edges.

Ожидание:

- duplicate geometry не появляется.

---

## Test F — external edge

В face-supported Sketch вручную спроецировать другое ребро тела, не принадлежащее support face.

Ожидание:

- оно успешно добавляется;
- dashed + locked;
- manual Projection остаётся undoable.

---

# 14. Ручная проверка

После build запустить свежий `solidar.exe`.

## Сценарий 1

1. Создать прямоугольник.
2. Extrude.
3. Выбрать верхнюю грань.
4. Create Sketch.

### PASS

Сразу после входа в Sketcher:

- контур верхней грани уже виден как Projection;
- его не нужно проецировать вручную;
- можно сразу ставить размеры/привязки от его endpoints.

---

## Сценарий 2

На боковой грани создать новый Sketch.

### PASS

Именно boundary боковой грани автоматически projected.

---

## Сценарий 3

Создать отверстие/внутренний контур на face, если текущий функционал позволяет.

Создать новый Sketch на полученной face.

### PASS

Внешний и внутренний boundary доступны как Projection.

---

## Сценарий 4

Внутри face-sketch включить Projection и выбрать соседнее ребро тела.

### PASS

Внешнее ребро добавляется вручную.

---

## Сценарий 5

Закрыть Sketch → снова Edit Sketch.

### PASS

Нет второго набора projected edges.

---

# 15. Scope

Ожидаемый основной scope:

```text
src/ui/SketchCanvas.cpp
src/ui/SketchCanvas.h
```

Допустимо минимально изменить:

```text
src/ui/MainWindow.cpp
src/ui/MainWindow.h
```

или структуру `SketchEditContext`, если это нужно, чтобы явно различать:

```text
new face sketch
existing face sketch edit
```

Тестовые файлы — по фактической существующей структуре тестов.

Не менять:

- Sketch solver без необходимости;
- topology naming;
- FaceReference semantics;
- ProjectFile serialization;
- Extrude;
- viewport picking;
- unrelated UI.

---

# 16. Regression gate

После реализации:

```powershell
git diff --check
```

Далее штатный:

```text
configure
build
targeted tests
full CTest
fresh solidar.exe smoke
```

Если полный CTest имеет известный baseline failure, отдельно показать:

- что targeted tests PASS;
- что новый failure не появился;
- состояние того же теста на исходном `main`, если это необходимо для доказательства.

---

# 17. Никакого push

После успешной локальной проверки:

```powershell
git status --short --branch
git diff --stat
git diff --check
```

Вернуть отчёт пользователю.

**Не делать commit/push/merge без отдельной команды пользователя.**

---

# 18. Критерии готовности

Исправление считается готовым к ручной проверке пользователя, если:

- [ ] новый Sketch on Face автоматически содержит boundary support face;
- [ ] internal wires также projected;
- [ ] auto projection dashed;
- [ ] auto projection locked;
- [ ] Projection не создаёт отдельные Undo steps при входе в Sketch;
- [ ] редактирование существующего Sketch не создаёт duplicates;
- [ ] datum-plane Sketch не изменился;
- [ ] manual Projection для внешних edges работает;
- [ ] manual Projection уже auto-projected edge не создаёт duplicate;
- [ ] targeted regression tests PASS;
- [ ] build PASS;
- [ ] свежий GUI запускается;
- [ ] никакого push не сделано.

---

# 19. Финальный отчёт агента

```markdown
## Result
PASS / FAIL / PARTIAL

## Branch
...

## Root implementation point
...

## Changed files
- ...

## Auto-projection behavior
...

## Undo behavior
...

## Existing-sketch edit behavior
...

## Tests added
- ...

## Targeted tests
...

## Full CTest
...

## Build
...

## Fresh executable
...

## Manual smoke
...

## Diff audit
...

## Remaining risks
...

## Push status
NOT PUSHED
```
