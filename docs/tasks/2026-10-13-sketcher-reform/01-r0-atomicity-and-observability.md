# R0 — атомарность и наблюдаемость Solver v1

## Цель

До замены математического движка сделать текущие пользовательские мутации
fail-closed: неуспешный или частично выполненный solve не изменяет committed
Sketch, а причина отказа доходит до UI и тестов.

Этот документ описывает **текущий этап реформирования**. Только после его полного
завершения разрешён R1.

## Исходный риск

`BasicSketchSolver::solveStableComponent()` переносит local geometry обратно в
`Sketch` независимо от `SolveResult::converged`. Интерактивные translate/move
пути часто отбрасывают `SolveResult`, а некоторые auto-constraints игнорируют
`ConstraintApplyResult`. Поэтому неудачный drag или snap может оставить
нарушенные ограничения либо создать визуально привязанную, но семантически
свободную геометрию.

## Не входит в R0

- новый nonlinear solver;
- изменение набора constraint types;
- новый формат `.solidar`;
- удаление известной order dependence Solver v1;
- визуальный редизайн SketchCanvas;
- оптимизация, не требуемая для атомарности.

## Шаг 1. Зафиксировать карту mutation paths

1. Перечислить все вызовы `solveStable`, `solveStableComponent` и прямого
   `solve` из production-кода.
2. Для каждого вызова отметить:
   - кто владеет транзакцией;
   - какие geometry/constraints/dimensions могут измениться;
   - используется ли результат solve;
   - кто выполняет rollback;
   - какой UI status получает пользователь.
3. Отдельно проверить translate point/line/selection/circle/arc/Bezier, reshape
   Arc endpoint, изменение constraint value и auto-constrain после создания.
4. Приложить таблицу к отчёту R0. Нельзя исправлять только известные вызовы и
   оставлять непроверенные пути.

## Шаг 2. Ввести единый результат мутации

1. Определить model-level результат, который отличает как минимум:
   `Applied`, `NoChange`, `InvalidReference`, `Unsupported`, `Conflicting`,
   `NonConverged`, `DeferredByActiveSolve`, `InternalError`.
2. Результат должен содержать:
   - `SolveResult` или его стабильную публичную проекцию;
   - затронутую component;
   - связанные `ConstraintId` при наличии;
   - диагностический code, а не только строку.
3. Обновить публичные интерактивные mutators так, чтобы caller не мог принять
   `void` за доказательство успеха.
4. Текст локализации формируется в UI, model остаётся Qt-free.

## Шаг 3. Сделать component solve атомарным

1. Решать только candidate state затронутой компоненты.
2. Считать solve успешным только если одновременно:
   - нет invalid references;
   - нет unsupported constraints;
   - solve не deferred;
   - все active hard constraints компоненты проходят residual audit;
   - результат не содержит `NaN`/`Inf` и недопустимой геометрии.
3. Under-constrained component с нулевыми residuals является успешной.
4. Переносить candidate geometry в committed `Sketch` только после полной
   проверки результата.
5. При любом другом статусе committed geometry, constraints, dimensions,
   bounds, solver fingerprint и next-ID counters должны остаться неизменными.
6. Не использовать «исправление» через удаление constraints или ослабление
   tolerance.

## Шаг 4. Провести атомарность через command layer

1. `SketchCommandController` обязан отклонять mutation command при неуспешном
   solve.
2. Вложенный delta journal полностью откатывается до исходной глубины.
3. Не создавать Undo entry для rejected/no-op операции.
4. После отказа следующая несвязанная операция должна выполняться штатно.
5. Selection/hover/active tool не должны ссылаться на candidate-only объекты.
6. Render и diagnostics caches инвалидируются только после commit; rollback не
   должен публиковать промежуточную сцену.

## Шаг 5. Устранить молчаливые auto-constraint failures

1. Запретить `(void)tryApplyConstraint(...)` в production auto-constrain paths.
2. Если preview обещал semantic snap, но соответствующий constraint не принят,
   отклонить всю команду создания primitive и восстановить состояние до неё.
3. Если relation является только визуальной подсказкой и не должна сохраняться,
   она не может выглядеть как подтверждённый snap.
4. UI показывает различимые сообщения для duplicate/redundant/conflict,
   malformed reference и non-convergence.
5. В сообщении о конфликте предоставить связанные constraint IDs для выбора в
   панели, если model их определил.

## Шаг 6. Добавить deterministic regressions

Добавить тесты через существующие targets, прежде всего
`sketch_constraint_integrity_tests`, `sketch_command_controller_tests` и
`sketch_interaction_controller_tests`:

1. несовместимый drag возвращает отказ и сохраняет semantic fingerprint;
2. candidate component не сливается при `NonConverged`;
3. failed drag → valid drag работает без stale journal/cache;
4. rejected auto-snap не оставляет geometry, constraint или Undo entry;
5. failed mutation → Undo/Redo → save/load → дальнейшее редактирование;
6. invalid reference и unsupported constraint имеют разные codes;
7. under-constrained, но удовлетворённая component успешно коммитится;
8. `NaN`, `Inf`, zero-length/zero-radius boundary не меняют модель;
9. после каждого отказа `deltaJournalDepth()` возвращается к исходному значению.

Каждый regression обязан падать на старом поведении или содержать независимую
проверку, доказывающую отсутствие ложноположительного теста.

## Шаг 7. Проверка

Минимальный targeted gate:

```bat
cmake --build --preset ci --clean-first
ctest --preset ci -R "sketch_(regression|constraint_integrity|lock_constraint|command_controller|interaction_controller)_tests" --output-on-failure
```

После targeted gate выполнить полный `ctest --preset ci`, dev verifier и ручной
smoke:

1. создать rectangle и цепочку line/arc/circle;
2. наложить Coincident, H/V, Equal, Tangent и driving dimensions;
3. несколько раз перетащить свободную и ограниченную точки;
4. выполнить заведомо невозможный drag;
5. проверить неизменность geometry после отказа;
6. Undo/Redo, save, reopen и продолжить редактирование;
7. завершить Sketch и выполнить Extrude/Pocket.

## Параметры завершения R0

R0 завершён только когда одновременно выполнено всё:

- [ ] все production solve call sites присутствуют в implementation map;
- [ ] ни один интерактивный mutator не отбрасывает неуспешный solve result;
- [ ] local component merge выполняется только после подтверждённого успеха;
- [ ] любой failed solve сохраняет полный semantic fingerprint;
- [ ] rejected command не создаёт Undo entry и не оставляет открытый journal;
- [ ] auto-constraint failure не является молчаливым и атомарно откатывает
      promised snap/primitive;
- [ ] UI различает conflict, non-convergence, invalid reference и unsupported;
- [ ] старые `.solidar` загружаются без миграции формата и без потери данных;
- [ ] новые deterministic regressions проходят минимум 20 последовательных
      повторов без flake;
- [ ] targeted и полный clean `ci` зелёные;
- [ ] dev verifier и ручной smoke зелёные;
- [ ] независимый reviewer не оставил HIGH findings;
- [ ] создан отчёт R0 с доказательствами каждого пункта.

Известная order dependence Solver v1 допускается после R0 только как явно
задокументированное ограничение. Любая частичная мутация после отказа — блокер
завершения этапа.
