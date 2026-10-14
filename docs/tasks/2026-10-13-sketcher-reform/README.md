# Реформирование Sketcher: порядок этапов и общие gates

Дата: 13 октября 2026 г.

## Назначение комплекта

Этот каталог содержит последовательные исполнимые задания для замены
процедурного `BasicSketchSolver` на транзакционный equation-based solver без
потери совместимости существующих проектов.

Инструкции выполняются строго по порядку. Следующий этап нельзя начинать, пока
в отчёте предыдущего этапа не доказаны все его критерии завершения.

## Текущий этап

Текущий разрешённый этап — **R0: атомарность и наблюдаемость Solver v1**:

- [01-r0-atomicity-and-observability.md](01-r0-atomicity-and-observability.md)

Этап R0 является отдельной завершённой поставкой. Подготовленные инструкции
R1–R4 не разрешают начинать их досрочно.

## Последовательность реформы

1. **R0 — containment:** неудачный solve ничего не коммитит; ошибки доходят до
   UI; автоматические ограничения не исчезают молча.
2. **R1 — model contract:** типизированные ссылки и ограничения, однозначная
   связь размера с constraint, версия persistence.
3. **R2 — equation kernel:** единые параметры, residuals и Jacobian для solver и
   diagnostics, только явная связность.
4. **R3 — solver и UX:** нелинейный component solver, rank diagnostics,
   временные drag-ограничения и точные состояния ошибки.
5. **R4 — migration:** shadow mode, corpus, performance gates, переключение и
   удаление Solver v1.

Инструкции:

- [02-r1-model-contract-and-persistence.md](02-r1-model-contract-and-persistence.md)
- [03-r2-equation-kernel.md](03-r2-equation-kernel.md)
- [04-r3-solver-diagnostics-and-drag.md](04-r3-solver-diagnostics-and-drag.md)
- [05-r4-shadow-rollout-and-cutover.md](05-r4-shadow-rollout-and-cutover.md)

## Неподвижные инварианты всей реформы

1. Неуспешная операция не меняет persisted semantic state.
2. Перестановка ограничений не должна менять статус и определённое решение.
3. Для недоопределённой системы выбор конкретной позы определяется предыдущим
   валидным состоянием и drag hint, а не порядком constraints.
4. Solver не вызывает mutators `Sketch`; он решает candidate parameter vector.
5. Solver и diagnostics используют одно описание уравнений.
6. Связность определяется явными constraints и composite ownership. Равенство
   координат не является topology.
7. `GeometryId`, `ConstraintId` и `DimensionId` остаются стабильными при
   insert/delete/Undo/Redo/save/load.
8. Ни одно старое ограничение не удаляется автоматически ради сходимости.
9. Нельзя ослаблять tolerance, пропускать неподдержанное ограничение или
   объявлять частично решённую геометрию успешной.
10. UI, Undo/Redo, selection/hover, active tool, solver cache и persistence не
    должны наблюдать половину транзакции.

## Общие правила выполнения

Перед каждым этапом:

1. Прочитать `AGENTS.md` и релевантные инструкции:
   `.opencode/skills/sketcher-safety/SKILL.md`,
   `.opencode/skills/regression-testing/SKILL.md`,
   `.opencode/skills/safe-change/SKILL.md`.
2. Зафиксировать `git status --short`; не изменять и не удалять пользовательские
   файлы.
3. Снять baseline указанными в инструкции тестами и benchmark.
4. Создать implementation map: authoritative state, observers, caches,
   Undo/Redo, persistence и все публичные mutation paths.
5. Выполнять небольшие reviewable изменения. Не совмещать этап с новыми
   Sketcher-инструментами или визуальным редизайном.

После изменения заголовков обязательна clean rebuild:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set VCPKG_ROOT=<repo>\vcpkg
set VCPKG_DEFAULT_TRIPLET=ci-x64-windows
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --preset ci
cmake --build --preset ci --clean-first
ctest --preset ci --output-on-failure
```

После успешного `ci` выполнить dev verifier:

```powershell
.\.opencode\scripts\verify-and-run.ps1
```

Verifier только сообщает ошибки и не исправляет код.

## Общий формат доказательства завершения

Для каждого этапа создать отчёт в `docs/reports/` со следующими разделами:

1. восстановленные инварианты и устранённые пользовательские риски;
2. выбранное решение и отклонённые альтернативы;
3. изменённые production/test/persistence файлы;
4. новые deterministic regressions и property tests;
5. targeted test results;
6. полный clean `ci`, dev verifier и ручной smoke;
7. benchmark baseline/after на одинаковых fixtures;
8. результат независимого review;
9. известные ограничения и разрешённый следующий этап;
10. расхождения с инструкцией.

Этап не завершён при красном gate, непроверенной миграции, недоказанном
rollback, HIGH-замечании reviewer или формулировке «обычно работает» вместо
воспроизводимого теста.
