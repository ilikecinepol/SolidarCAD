# U7b — исходное положение компонентов

Дата: 2 ноября 2026 г.

Статус: задание для реализации.

## Контекст

Рабочая копия: `C:\Users\drmma\Documents\SolidarCAD`.

Перед работой полностью прочитать:

- `AGENTS.md`;
- `.opencode/skills/solidcad-architecture/SKILL.md`;
- `.opencode/skills/safe-change/SKILL.md`;
- `.opencode/skills/qt-safety/SKILL.md`;
- `.opencode/skills/regression-testing/SKILL.md`;
- `.opencode/skills/build-and-run/SKILL.md`;
- `.opencode/skills/git-hygiene/SKILL.md`;
- `docs/decisions/2026-10-31-unified-component-workspace.md`.

Сначала выполнить `git status`. Рабочее дерево содержит незакоммиченную
реализацию компонентов и сборок — сохранить её полностью. Не выполнять
`reset`, `clean`, checkout чужих файлов, commit или push.

## Цель

В группу «СБОРКИ» обычной `ModelRibbon` добавить два инструмента:

1. «Закрепить положение»;
2. «Вернуть исходное положение».

Они работают только в целевом unified component workspace
(`ComponentDefinition`). Legacy `assemblyReadOnlyRibbon` и старый
`AssemblyDefinition` UI не расширять.

## Семантический контракт

У `ComponentOccurrence` должны существовать две независимые позы:

- `placement` — текущее положение относительно непосредственного родителя;
- `homePlacement` — сохранённое исходное положение.

Правила:

- при создании occurrence: `homePlacement = placement`;
- при загрузке старого формата: `homePlacement = placement`;
- direct drag изменяет только `placement`;
- «Закрепить положение» выполняет `homePlacement = placement`;
- текущий `placement` при закреплении не меняется;
- после закрепления компонент остаётся перемещаемым;
- «Вернуть исходное положение» выполняет `placement = homePlacement`;
- восстанавливается полный rigid transform: translation и quaternion;
- `grounded`, visible, suppressed, IDs, имя, B-Rep и содержимое definition не
  изменяются;
- если `placement == homePlacement`, обе операции являются точным no-op и не
  создают историю;
- будущие solver/mate-команды смогут менять `placement`, но не должны
  автоматически менять `homePlacement`.

Важно: существующий `OccurrenceFlags::grounded` — отдельная 6-DOF фиксация
для решателя. Не переименовывать и не использовать его вместо
`homePlacement`.

## Модель и транзакции

Расширить `ComponentOccurrence` в `src/model/ProductDocument.*`:

- добавить `homePlacement()` и закрытое поле;
- сохранить старый конструктор или overload, устанавливающий home равным
  текущему placement;
- добавить закрытую мутацию home через `ComponentDefinition`;
- убедиться, что обычный `setPlacement()` не меняет home;
- все clone/materialization/extraction/duplicate-пути должны сохранять обе
  позы.

Добавить typed API, например:

- `ComponentHomePlacementOperation { AdoptCurrent, Restore }`;
- `ComponentHomePlacementRequest`;
- `ComponentHomePlacementTransaction`;
- typed status/result;
- `prepareComponentHomePlacement()`;
- `applyComponentHomePlacement()`;
- `revertComponentHomePlacement()`.

Request/transaction должны содержать точные stale guards:

- product incarnation;
- root definition;
- полный `OccurrencePath`;
- target и owner definition;
- expected owner state;
- expected current placement;
- expected home placement;
- before/after state IDs и placement values.

Любая ошибка, повторный вход, stale selection, successor/resource failure —
точный no-op с полным rollback.

Для вложенного occurrence меняется его parent-local transform. Не
преобразовывать home в world coordinates.

`Restore` должен отклоняться для grounded, suppressed и effectively hidden
occurrence теми же typed-причинами, что и обычное перемещение.
`AdoptCurrent` может работать с grounded occurrence, поскольку отображаемый
transform не меняется.

## История и представление

Расширить unified `ModelCommandHistory`, а не legacy
`AssemblyCommandHistory`:

- добавить `ModelCommandKind::ComponentHomePlacement`;
- отдельную bounded cold command;
- Apply/Undo/Redo для обеих операций;
- одна пользовательская операция — одна history entry;
- no-op не меняет revision, dirty/savepoint и redo tail.

Требуемая симметрия:

- Undo Adopt восстанавливает прежний home, не двигая компонент;
- Undo Restore возвращает прежний current placement;
- Redo повторяет операцию точно.

После операции сохранить:

- выбранный occurrence;
- hover;
- active component context;
- раскрытие дерева;
- камеру.

Adopt не должен менять transform, bounds или picking. Restore обновляет
transform, bounds и spatial consumers. Обе операции не должны запускать
`Document::recompute()`, remesh или повторный GPU upload source geometry.

## Persistence

`homePlacement` должен переживать Save/Open. Session-only реализация не
принимается.

V10/V11 и общий `StrictOccurrenceCodec` имеют строгую exact-field схему,
поэтому их байты и readers не изменять.

Добавить новый component envelope V12 на основе V11:

- обязательное occurrence-поле `homePlacement`;
- capability `component-home-placement-v1`;
- сохранить drawings и остальные возможности V11;
- отдельный version-specific occurrence codec;
- canonical translation/quaternion validation;
- encode → scan → validation materialization → encode fixed point;
- atomic writer;
- документация `docs/project-format-v12.md`;
- `ProjectPersistenceFormat::V12` и application routing.

Миграция:

- V10/V11 materialization: `homePlacement = placement`;
- первое изменение placement/home переводит writable component project в V12;
- после перехода не понижать формат;
- V10/V11 encoder должен fail-closed, если `homePlacement != placement`;
- неизвестные поля/capabilities, malformed/non-finite home и версия выше V12
  отклоняются;
- добавить минимальный и повреждённый V12 fuzz seed.

## Интерфейс

В `ModelRibbon`, в существующую группу «СБОРКИ», добавить две не-checkable
tile-кнопки:

- `componentSetHomePlacementCommand` — текст `Закрепить\nположение`;
- `componentRestoreHomePlacementCommand` — текст
  `Вернуть исходное\nположение`.

Сигналы:

- `setComponentHomePlacementRequested()`;
- `restoreComponentHomePlacementRequested()`.

Tooltip первой кнопки:

> Сохранить текущее положение как исходное. Компонент останется перемещаемым.

Tooltip второй:

> Вернуть выбранный компонент в сохранённое исходное положение.

Использовать theme-aware ресурсы и существующую систему иконок. Не добавлять
локальные цвета или `setStyleSheet`. На этом этапе не добавлять горячие клавиши
и контекстное меню.

Обе кнопки доступны только когда:

- открыт writable unified component project;
- выбран актуальный non-root occurrence;
- publication и полный `OccurrencePath` актуальны;
- нет modal dialog, активного Part Design tool, drag preview, history apply или
  project replacement;
- `placement != homePlacement`.

Для Restore дополнительно действуют grounded/suppressed/hidden guards.

Все условия собрать в одном методе вроде
`updateComponentHomePlacementAvailability()`. Обновлять его после:

- tree/viewport selection и clear;
- drag commit;
- Adopt/Restore;
- Undo/Redo;
- установки новой projection;
- Open/project switch;
- входа и выхода из component editing.

Команды `MainWindow` должны проходить через модельную транзакцию и историю, а
не менять occurrence напрямую.

Сообщения:

- «Текущее положение сохранено как исходное»;
- «Компонент возвращён в исходное положение».

## Обязательные сценарии

1. Создать компонент: обе кнопки disabled.
2. Переместить A → B: обе кнопки enabled.
3. Restore: компонент возвращается в A.
4. Undo возвращает B, Redo возвращает A.
5. Переместить A → C, выполнить Adopt.
6. Компонент остаётся перемещаемым.
7. Переместить C → D, выполнить Restore: результат C.
8. Save, Close, Open, переместить компонент и снова получить C через Restore.
9. Повторить model-level сценарий с ненулевым вращением.
10. Проверить nested occurrence.
11. Убедиться, что `grounded` и будущая mate-семантика не изменились.

## Тесты

Минимально расширить или добавить:

- `component_placement_tests`;
- `component_placement_history_tests`;
- `model_ribbon_tests`;
- `component_viewport_integration_tests`;
- `component_workspace_tests`;
- новый `product_envelope_v12_tests`;
- public-header compile test V12;
- routing/Save/Save As/project-switch regressions.

Проверить:

- stale guards и `Busy`;
- exact rollback на всех failure seams;
- no-op;
- Undo/Redo и redo-tail;
- savepoint/dirty;
- legacy migration home=current;
- V12 fixed point и atomic destination;
- nested local-vs-world поведение;
- отсутствие recompute/remesh/upload;
- сохранение selection/camera;
- UI на 100%, 125%, 150% и 200% DPI.

Использовать `CHECK`, не `assert`.

## Проверка

После изменения заголовков обязательно:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=C:\Users\drmma\Documents\SolidarCAD\vcpkg
set VCPKG_DEFAULT_TRIPLET=ci-x64-windows
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset ci
cmake --build --preset ci --clean-first
ctest --preset ci
```

Затем:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .opencode/scripts/verify-and-run.ps1
```

Провести независимый review. В финальном отчёте указать изменённые файлы,
результаты focused/full CI, ручной сценарий и все оставшиеся ограничения.
Соединения, mate UI и solver в эту задачу не добавлять.
