# SolidarCAD — Extrude v2 Correction Pass after manual QA

## Контекст

Ветка: `codex/3d-input-and-selection`.

Текущий Extrude v2 в целом работает: native face extrusion строится через B-Rep/OCCT, вертикальные и наклонные плоские грани могут быть источником, zoom/orbit не сдвигают selection, non-planar face корректно отклоняется.

Но ручное QA выявило системные дефекты. Это одна correction pass поверх текущего Extrude v2. Не откатывать архитектуру `ExtrudeSource + FaceReference + FaceExtrudeBuilder + ExtrudeToolSession`.

Работать от текущего local worktree. Не делать reset/clean. Не трогать unrelated tools. Не commit/push без явного разрешения пользователя.

## 1. BLOCKER — стрелка показывает направление, противоположное геометрии

Симптом: пользователь тянет стрелку визуально вверх, а геометрия растёт вниз, и наоборот.

Причина: `computeManipulatorLayout()` выбирает `visualSign` из `{+1,-1}` ради экранного размещения handle/HUD. Для directional Extrude это недопустимо: экранная стрелка может быть нарисована против semantic direction, тогда как OCCT builder продолжает использовать настоящую face normal.

Требование:
`drawn arrow direction == semantic geometry direction`.

Расширить контракт layout/manipulator, например `allowVisualDirectionFlip`. Для Face Extrude установить `false`. HUD можно переставлять независимо от направления стрелки.

Добавить regression для +Z, +X и наклонной normal: layout не имеет права инвертировать extrusion semantics.

## 2. BLOCKER — Cut не работает и handle не проходит через ноль

Симптом:
- при включении `Вырезать` появляется `Face extrude Cut does not intersect the body`;
- при попытке тянуть внутрь тела handle останавливается на исходной поверхности.

Текущая проблема:
- `ExtrudeToolSession` хранит только positive distance (`minimum=0.01`);
- `reversed` — отдельный bool;
- `setLengthFromManipulator()` принимает только длину;
- checkbox `Вырезать` меняет только `Join <-> Cut`;
- `trySetLength()` откатывает новое значение, если preview invalid, поэтому нельзя пройти через временно invalid/no-op состояние.

Требование: манипулятор Extrude должен работать со signed drag value.

```text
signed > epsilon:
    length = abs(signed)
    reversed = false

signed < -epsilon:
    length = abs(signed)
    reversed = true

abs(signed) <= epsilon:
    temporary transition state
```

Панель может отображать положительную абсолютную длину.

Preview invalid не должен блокировать дальнейшее движение мыши. Session должна принимать parameter/direction state даже при временно invalid preview, показывать ошибку и позволять продолжить drag.

`direction/reversed` и `operation Join/Cut` остаются независимыми.

Tests:
- Cut top face inward succeeds;
- Cut outward gives no-op error, но drag остаётся editable;
- crossing zero flips reversed;
- Join outward works;
- invalid preview не откатывает signed drag state.

## 3. BLOCKER — после face extrusion грани перестают резолвиться

Симптом: после face extrusion при выборе другой плоской грани появляется:
`Topology face reference is ambiguous by semantic tag`.

Подтверждённая причина: `TopologyReferenceResolver` немедленно возвращает failure, если несколько кандидатов имеют один `persistentTag`. После boolean/extrude несколько coplanar faces могут получить одинаковый auto-tag (`planar:max-x`, `planar:max-y`, `planar:max-z`). При этом reference уже содержит `FaceSignature`, но resolver до неё не доходит.

Новый алгоритм:
```text
if tag gives 1 candidate:
    resolve

if tag gives >1 and signature exists:
    score only tagged candidates by saved FaceSignature
    if unique best candidate:
        resolve
    else:
        true ambiguity

if tag gives 0:
    try geometric signature over all candidates

legacy index:
    final fallback only
```

Semantic tag — strong hint, не абсолютная идентичность.

Tests:
- две coplanar max-axis faces с одним auto-tag, но разными centroid/area;
- signature выбирает правильную;
- настоящая симметричная неоднозначность остаётся ошибкой.

## 4. HIGH — лишние coplanar рёбра после Join

Симптом: после face extrusion на одной геометрически плоской поверхности остаётся старое seam edge.

Требование: после boolean выполнять OCCT same-domain unification, например `ShapeUpgrade_UnifySameDomain`, либо эквивалент.

Условия:
- объединять только genuinely same-domain faces/edges;
- не удалять реальные геометрические границы;
- после unification снова выполнить validity check;
- topology naming tests должны проходить.

Tests:
- box top face Join outward: старый coplanar seam исчезает;
- настоящий угол остаётся edge;
- hole/cylinder boundaries сохраняются.

## 5. BLOCKER — history marker сам прыгает назад

Симптом:
несколько операций → новый Sketch → marker внезапно уходит на 2–3-й шаг → Extrude не активируется → вручную вернуть marker в конец → применить feature → marker снова уходит назад.

Подтверждённая причина: в нескольких местах `MainWindow` используется legacy-формула:

```cpp
historyPosition_ =
    static_cast<int>(sketchHistory_.size()) +
    (hasExtrusion_ ? 1 : 0);
```

Она учитывает Sketches + один Extrude и не учитывает современную feature history.

Реальный источник истины уже есть:
```cpp
historySteps_ = buildPartDesignHistory(...)
```

Требование:
- один источник истины: `lastPosition = historySteps_.size()`;
- добавить helper вроде `moveHistoryToEnd()` / `isHistoryAtEnd()`;
- после успешного создания новой feature: rebuild history → marker = actual last position;
- после завершения нового Sketch marker остаётся в конце актуальной истории;
- убрать вычисление позиции через `sketchHistory_.size()`.

Explicit rollback должен оставаться предсказуемым: не прыгать самопроизвольно.

Tests:
- 1 sketch + 6 features + new sketch → marker остаётся на конце;
- apply ещё одной feature → marker на новом конце;
- repeated operations не двигают marker назад.

## 6. BLOCKER — после Save/Open показывается более ранний history state

Симптом: до сохранения модель полная, после открытия внешний вид откатывается/ломается.

Подтверждённая причина в `loadProject()`:
```cpp
historyPosition_ = sketchHistory_.size() + (hasExtrusion_ ? 1 : 0);
```
после чего вызываются `rebuildHistoryPanel()` и `applyHistoryPosition(historyPosition_)`.

Для современного Body это отображает промежуточный шаг.

Требование после load/recompute:
```text
history marker = actual end of buildPartDesignHistory
viewport = final Body result
```

Дополнительно проверить, не вызывает ли topology ambiguity реальный rebuild failure после load. Исправление пункта 3 должно покрыть этот случай.

Regression:
- Sketch → Extrude → Draft → Face Extrude → Cut;
- save/load;
- final bounds/volume/feature validity идентичны;
- marker в конце;
- viewport показывает final body.

## 7. HIGH — Sketch on newly-created planar face ошибочно считается curved

Симптом: на явно плоской грани после новых 3D операций `Создать эскиз` выдаёт:
`Sketch on curved surfaces is not supported yet`.

Подтверждённая архитектурная проблема:
`resolveFacePlacement(shape, TopologyReference)` делает `resolveFaceReference()`, а при любом resolution failure возвращает default `ResolvedFacePlacement{planar=false}`. UI любую `!planar` интерпретирует как curved surface.

Нужно различать:
- resolved planar face;
- resolved non-planar face;
- topology resolution failed.

Например расширить result:
```cpp
struct ResolvedFacePlacement {
    SketchPlacement placement;
    bool planar;
    bool resolved;
    std::string error;
};
```
или эквивалент.

После исправления topology resolver из пункта 3 planar faces новых Extrude должны снова корректно использоваться как Sketch support.

Tests:
- sketch on planar face created by Face Extrude;
- sketch on planar face after Draft;
- sketch on planar face after Cut;
- cylinder side → true non-planar rejection;
- topology ambiguity → topology error, а не "curved surface".

## 8. Русификация user-facing ошибок

Все ошибки, которые видит пользователь в status bar/tool panel/message box, должны быть на русском.

Сейчас видны строки:
- `Face extrude Cut does not intersect the body`
- `Topology face reference is ambiguous by semantic tag`
- `Face for extrusion must be planar`
- `Sketch on curved surfaces is not supported yet`

Предпочтительно:
```text
model/debug error = stable technical English or structured code
UI user message = Russian
```

Минимальные сообщения:
- `Выдавливание не пересекает тело.`
- `Выбранная грань не может быть однозначно определена.`
- `Для выдавливания выберите плоскую грань.`
- `Не удалось восстановить выбранную грань после изменения модели.`
- `Создание эскиза на криволинейной поверхности пока не поддерживается.`
- `Не удалось определить выбранную поверхность.`

Не показывать raw internal English, если ошибка известна приложению.

## 9. Что уже работает и не должно регрессировать

Сохранить:
- native `ExtrudeSource` Sketch/Face;
- persistent `FaceReference`;
- top/side/sloped planar face extrusion;
- zoom/orbit/pan stable selection;
- `ReplaceSource` native preview;
- Tab → on-canvas HUD;
- Enter → Apply;
- Draft/Shell текущий UX;
- non-planar face rejection;
- serialization backward compatibility старых Sketch Extrude;
- whole-body/face/edge selection behavior.

## 10. Manual QA после correction

1. Top face: arrow указывает ровно туда, куда растёт geometry.
2. Side face: горизонтальная стрелка и горизонтальный рост.
3. Sloped Draft face: arrow и extrusion используют одну normal.
4. Cut: наружу временно invalid, через ноль внутрь появляется cut preview, движение не замерзает.
5. Повторить face extrusion 3 раза: новые planar faces остаются selectable.
6. Join на coplanar surface: старый seam edge исчезает.
7. История: минимум 6 операций + новый sketch; marker не прыгает назад.
8. Save/Open: модель визуально идентична, marker в конце.
9. Sketch on new planar face: Sketcher открывается на правильной поверхности.
10. Cylinder side: русское сообщение о неподдерживаемой криволинейной поверхности.
11. Все известные пользовательские ошибки — на русском.

## 11. Regression tests required

Добавить tests для:
- directional manipulator no visual inversion;
- signed Extrude drag / reversed switching;
- invalid preview does not lock drag;
- duplicate semantic tag + signature disambiguation;
- same-domain face unification;
- history position after many features;
- history position after save/load;
- sketch-on-face resolution after Face Extrude;
- Russian user-facing error mapping where testable.

Run:
```powershell
git diff --check
cmake --build --preset dev --clean-first --parallel
ctest --preset dev
```
После этого GUI smoke.

## 12. Agent workflow

```text
explorer + regression-designer + occt-auditor
→ short correction contract
→ implementer
→ reviewer
→ one correction pass maximum
→ build-engineer/local-verifier
→ STOP
```

No additional feature work.
No Mirror/Pattern redesign.
No unrelated refactor.
No commit/push unless user explicitly authorizes it.
