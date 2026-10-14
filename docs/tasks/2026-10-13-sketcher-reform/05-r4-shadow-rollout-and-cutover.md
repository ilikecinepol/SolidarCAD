# R4 — shadow rollout, migration и отключение Solver v1

## Условие старта

R3 принят. Solver v2 покрывает весь текущий constraint vocabulary и прошёл
детерминированные correctness/diagnostics/drag gates.

## Цель

Доказать совместимость на реальных проектах, безопасно переключить production на
Solver v2 и удалить sequential solver только после отсутствия скрытых
потребителей.

## Шаг 1. Собрать versioned corpus

Corpus должен включать:

1. минимальные fixtures каждого constraint type;
2. все найденные historical sketch regressions;
3. rectangle/slot/polygon/mirror/trim-подобные реальные цепочки;
4. line/circle/arc/Bezier tangency;
5. under-, fully- и over-constrained sketches;
6. v1/v2 и новый persistence format;
7. delete/Undo/Redo/save/load/edit-again sequences;
8. крупные independent и connected workloads;
9. обезличенные реальные пользовательские проекты при наличии разрешения.

Каждый fixture имеет ожидаемый load status, solve class, constraint integrity и
canonical geometry либо явно допускаемый nearest-state contract.

## Шаг 2. Включить non-mutating shadow mode

1. Production продолжает коммитить только выбранный active solver.
2. Shadow solver получает immutable snapshot и не может менять Document, Undo,
   selection, caches или modified flag.
3. Сравнивать:
   - solve status;
   - satisfied constraint IDs;
   - residual norm;
   - rank/DOF;
   - canonical geometry;
   - iterations и latency.
4. Логи не содержат пользовательские пути/имена без необходимости.
5. Каждое расхождение получает воспроизводимый fixture и классификацию:
   V1 defect, V2 defect, intentional contract change или tolerance issue.

## Шаг 3. Разобрать intentional differences

1. Order-dependent результат V1 не является compatibility oracle.
2. Для under-constrained sketch использовать nearest-previous contract.
3. Более точная диагностика V2 допустима, если geometry и hard constraints
   корректны.
4. Нельзя принимать отличие только потому, что V2 «выглядит разумнее»; оно
   должно следовать документированному invariant и отдельному regression test.
5. Persistence migration не должна менять геометрию при первом открытии без
   явной необходимости ремонта malformed data.

## Шаг 4. Переключить active solver

1. Переключение выполняется одной adapter/config boundary, без разветвлений по
   UI tools.
2. Сначала включить V2 в tests и developer builds.
3. Затем включить V2 по умолчанию в production build.
4. Временный fallback на V1 допускается только как явный диагностируемый режим,
   никогда автоматически после частично выполненного V2 solve.
5. Failed V2 solve не должен запускать V1 над уже изменённым candidate state.

## Шаг 5. Удалить Solver v1

Удаление разрешено только после прохождения cutover gates:

1. удалить sequential type passes и V1-only mutators;
2. удалить coordinate-proximity connectivity compatibility;
3. удалить duplicated diagnostic equations;
4. удалить V1 benchmark path после сохранения итогового baseline report;
5. выполнить `rg` по старым API и доказать отсутствие production call sites;
6. сохранить file-format readers и migrations, если они ещё нужны.

## Шаг 6. Полный correction cycle

1. Clean configure/build/full CTest на `ci`.
2. Не менее 20 повторов наиболее чувствительных sketch/UI regressions.
3. Dev verifier с launch smoke.
4. Ручной сценарий:
   - создать sketch с mixed constraints;
   - drag свободной и связанной geometry;
   - получить и устранить conflict;
   - переключить driving/reference;
   - удалить geometry и выполнить два Undo/Redo;
   - save, close, reopen, продолжить edit;
   - Extrude/Pocket и редактирование исходного sketch.
5. Windows и Linux CI должны использовать один mathematical contract.
6. Проверить light/dark только для читаемости новых diagnostic states, без
   визуального redesign.

## Шаг 7. Финальный performance report

На одном reference host сравнить V1 baseline и V2:

- p50/p95/p99 solve и full diagnostics;
- iterations, components/parameters/equations visited;
- allocations и peak memory;
- 100/500/1000 geometry;
- independent, connected, tangency и Bezier workloads;
- drag cadence и on-release diagnostics.

Обязательные границы:

1. типичный interactive workload до 100 geometry: p95 не выше 16,7 мс;
2. ни одна синхронная UI-фаза не превышает 50 мс на reference fixture;
3. unchanged solve не пересобирает equation system без invalidation;
4. independent edit не посещает несвязанную component;
5. для крупных workloads без жёсткого timing gate публикуется измерение и
   конкретный bottleneck, а не декларация «достаточно быстро».

## Параметры завершения R4

- [ ] corpus покрывает все constraint types, historical bugs и persistence
      versions;
- [ ] каждое shadow mismatch закрыто regression или принятым design decision;
- [ ] Solver v2 является единственным production active solver;
- [ ] автоматического fallback после failed/partial solve нет;
- [ ] sequential solver и duplicated diagnostics удалены;
- [ ] `rg` подтверждает отсутствие V1 production call sites;
- [ ] existing projects открываются без молчаливой геометрической миграции;
- [ ] permutation, atomicity, drag, Undo/Redo и round-trip gates зелёные;
- [ ] performance boundaries выполнены и опубликованы;
- [ ] полный clean `ci`, 20x sensitive regressions, dev verifier и manual smoke
      зелёные;
- [ ] Windows/Linux результаты согласованы;
- [ ] независимый reviewer не оставил HIGH findings;
- [ ] финальный отчёт перечисляет удалённый legacy code и оставшиеся риски.

После выполнения этих пунктов реформа solver считается завершённой. Новые
Sketcher tools и дальнейшая декомпозиция `SketchCanvas` планируются отдельными
задачами и не используются для расширения границ R4.
