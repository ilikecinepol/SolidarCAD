# SolidarCAD — Extrude Visual Cleanup

## Цель

Исправить визуализацию Face Extrude после внедрения Extrude v2.

Сейчас при выдавливании B-Rep грани одновременно могут отображаться новый native OCCT preview и старый legacy QPainter preview. Из-за этого появляются огромные красные/синие полупрозрачные призмы или цилиндры, лишняя длинная стрелка и отрицательное значение в старом поле ввода.

## Root cause

В `Viewport::paintGL()` legacy preview всё ещё рисуется при `extrusionManipulatorVisible_` и использует:

- `selectedExtrusionPolygon_`
- `selectedExtrusionPolygons_`
- `selectedExtrusionPaths_`
- `extrusionPreviewLengthMm_`
- `extrusionManipulatorAnchor_`
- `extrusionLengthEditor_`

Face Extrude уже использует современный путь:

- `ExtrudeToolSession`
- `LinearToolManipulator`
- `ToolParameterHud`
- `setToolPreviewShape()`
- `ToolPreviewPresentation::ReplaceSource`

То есть смешаны два поколения визуализации.

## Требования

### 1. Face Extrude — только native preview

Для face-based Extrude использовать только:

```text
FaceReference
→ FaceExtrudeBuilder
→ TopoDS_Shape preview
→ BodyRenderMesh
→ ViewportRenderer
→ ReplaceSource
```

Legacy screen-space preview поверх него запрещён.

### 2. Sketch Extrude не ломать

Legacy path можно оставить только для старого sketch-based Extrude, если он ещё нужен.

Разделение должно быть явным:

```text
Sketch Extrude → legacy path
Face Extrude → modern ToolSession path only
```

### 3. При запуске Face Extrude

Обязательно скрывать legacy UI:

```cpp
hideExtrusionManipulator();
extrusionLengthEditor_->hide();
```

и очищать legacy caches:

```cpp
selectedExtrusionPolygon_.clear();
selectedExtrusionPolygons_.clear();
selectedExtrusionPaths_.clear();
selectedExtrusionRegionSketches_.clear();
selectedExtrusionSketch_.clear();
extrusionHoverPolygon_.clear();
extrusionHoverPath_ = {};
```

Желательно вынести это в helper:

```cpp
void Viewport::clearLegacyExtrusionPreview();
```

### 4. Guard в `paintGL()`

Даже если legacy state случайно остался активным, Face Extrude не должен получить второй preview.

Добавить явный predicate, например концептуально:

```cpp
if (extrusionManipulatorVisible_ && !modernFaceExtrudePreviewActive) {
    drawLegacyExtrusionPreview();
}
```

Лучше использовать явный active-tool/session state, если он доступен.

### 5. HUD

Для Face Extrude на экране должен быть только `ToolParameterHud`.

Показывать абсолютную длину:

```text
131,66 mm
```

а не `-131,66 mm`.

Знак остаётся внутренним состоянием `reversed`.

### 6. Стрелка

Должна быть только одна современная синяя `LinearToolManipulator`.

Никакой второй legacy-стрелки.

### 7. Cut preview

При Cut внутрь тела:

- не рисовать красный полупрозрачный режущий объём;
- показывать именно будущий B-Rep result с удалённым материалом;
- красный использовать только для error/invalid state.

### 8. Invalid preview

Если preview invalid:

- не возвращаться к legacy preview;
- modern manipulator остаётся доступным;
- исходное тело остаётся нормального цвета;
- показывается русское сообщение об ошибке;
- drag можно продолжить.

## Manual QA

1. Face Extrude Join наружу:
   - одна синяя стрелка;
   - один HUD;
   - preview = будущее тело;
   - нет полупрозрачной legacy-призмы.

2. Face Extrude Cut внутрь:
   - одна синяя стрелка;
   - HUD показывает положительный модуль длины;
   - preview показывает полость;
   - нет красного цилиндра.

3. Перетащить через ноль:
   - никакого дублирования;
   - остаётся один modern control.

4. Invalid Cut наружу:
   - исходное тело видно нормально;
   - нет красной гигантской геометрии;
   - есть русское сообщение;
   - drag назад внутрь продолжает работать.

5. Cancel:
   - не остаётся legacy arrow;
   - не остаётся старого spinbox;
   - не остаётся screen-space polygon/path.

6. Apply:
   - никаких transient overlay элементов.

7. Проверить обычный Sketch Extrude:
   - его workflow не регрессировал.

## Regression tests

Добавить минимально:

- entering Face Extrude clears legacy extrusion UI state;
- modern face preview suppresses legacy extrusion paint path;
- directional Face Extrude HUD получает absolute magnitude;
- cancel Face Extrude очищает legacy state.

Если `paintGL()` сложно тестировать напрямую — вынести predicate/helper и тестировать его отдельно.

## Scope

Не трогать:

- topology naming;
- history;
- `ShapeUpgrade_UnifySameDomain`;
- Mirror/Patterns;
- Draft/Shell;
- сериализацию;
- boolean semantics.

Это только visual routing cleanup между legacy Sketch Extrude и modern Face Extrude.

## Workflow

```text
explorer
→ implementer
→ reviewer
→ build/test
→ GUI smoke
→ STOP
```

Без commit/push без отдельного разрешения пользователя.
