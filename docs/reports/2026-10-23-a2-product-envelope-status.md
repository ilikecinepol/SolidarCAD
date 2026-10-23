# A2 product envelope — статус реализованного ограниченного среза

Дата проверки: 23 октября 2026 г.

Проверенный HEAD: `022f97e`

Статус: **single-part V5 slice реализован и покрыт CI regressions; итоговый A2
evidence gate открыт до записанного Linux Clang non-MSVC fuzz run; umbrella A2
также открыт**.

## Реализованная граница

Редактор владеет одним `ProductDocument`: его единственный корневой
`PartDefinition` владеет существующим `Document`. Standalone V3/V4 остаётся
identityless, а строгий product envelope v5 несёт атомарный bundle `ProjectId`
и metadata. В production включены:

- staged Open и materialization single-part V5;
- Save V5 с сохранением identity;
- явная атомарная conversion допустимого standalone V3/V4 в выбранную цель V5;
- standalone Save As без implicit migration и V5 Save As clone с новым
  `ProjectId`;
- fail-closed rollback при decode/encode/write/flush/sync/replace failures без
  замены live graph и без расходования model ID cursors.

Conversion и clone сохраняют root `DefinitionId`, вложенные
Document/Body/Feature/Sketch IDs, topology references и live graph. Путь
остаётся locator. Save сохраняет identity; Save As clone меняет identity.
Обычная файловая копия является replica, а не clone.

## История реализации и evidence

| Commit | Подтверждённая граница |
| --- | --- |
| `3577f39` | Типизированные product identity и V5 envelope contract. |
| `8ac54db`, `3d18a09` | Общая атомарная публикация и disconnected V5 encoder/writer. |
| `b450ca6`, `e6b8196`, `715f2e1` | Authoritative metadata, materialization и подготовленная conversion boundary. |
| `db87fad`, `7e0dfa6` | Application persistence и product-aware editor Open/Save ownership. |
| `6d18d5b`, `417f07c`, `65721b0` | Noexcept conversion commit, atomic service и explicit UI conversion. |
| `4e29eef`, `fa1369d` | Atomic clone/fork service и product-aware Save As. |
| `89d3184` | Historical v1 prism с активным legacy extrusion marker отклоняется до chooser/writer, исключая подтверждённую потерю семантики. |
| `022f97e` | Fuzzer напрямую сканирует каждый input как V5 и использует checked-in minimal V1/V5 seeds. |

Последний полный CI evidence для этого среза: clean build **623/623**, CTest
**117/117**. Локальный verifier прошёл **117/117** и GUI smoke. Датированные
ранние отчёты сохраняют evidence своих HEAD и не переписываются этим снимком.

Linux non-MSVC fuzz preset на этом MSVC host **не запускался**: используемый
libFuzzer driver намеренно поддерживается Linux Clang non-MSVC профилем. Его
канонический V5 seed прочитан и проверен детерминированным unit test на этом
HEAD; это не подменяет отдельный Linux fuzz run и оставляет итоговый A2 evidence
gate открытым.

## Влияние на release scope

Components/product program от 18 октября был явным пользовательским решением
расширить scope до выпуска 0.1.0. Поэтому автоматический RC evidence от 4
октября остаётся историческим и не является финальным для расширенного `main`.
После завершения авторизованного scope необходимо заново выполнить полный
release checklist, CI/release packaging, field, GPU/DPI, clean-machine и tag
gates на новом финальном RC.

## Совместимость conversion

| Источник | Текущая политика |
| --- | --- |
| Marker-absent v1-like carrier | Допустим при остальных штатных preconditions. |
| Авторитетный standalone V2/V3 | Допустим как явное V3 persistence state. |
| Standalone V4 | Допустим с сохранением V1/V2 topology variants и provenance. |
| Historical v1 prism с legacy extrusion marker в committed application state | Безусловный fail-closed до chooser/prepare/writer/I/O. Service доверяет переданному typed precondition; production UI передаёт фактический marker. |
| Уже identity-bearing V5 | Повторная conversion запрещена; Save или Save As clone используются явно. |

## Открытые границы

- **A2f — Relocate/Move:** нужна утверждённая filesystem/replica transaction
  policy. Операция должна сохранить `ProjectId` и определить canonical
  same-target/file-identity aliases (symlink, junction/reparse point, hardlink),
  same- и cross-volume algorithms, overwrite, dirty/savepoint и
  external-modification policy, source-delete failure после publication,
  rollback/crash recovery и явное two-replica state; текущий Save As является
  clone, не relocate.
- **A2g — historical v1 prism migration:** требуется semantics-preserving
  преобразование legacy extrusion. До него fail-closed обязателен; синтез
  непроверенной Body/Extrude истории запрещён.
- **A6a — identity conflict enforcement:** нормативный инвариант одинакового
  `ExternalSourceKey` с разной `SourceRevision`/content hash остаётся
  `IdentityConflict`, но runtime resolver/enforcement ещё не реализован.
- Multiple definitions, `AssemblyDefinition`, occurrences, linked definitions,
  reload/repair и mates не входят в текущий V5 production slice.

Следовательно, текущие evidence подтверждают реализацию и CI regression
coverage описанной single-part границы, но не финальное закрытие A2. До
записанного Linux Clang non-MSVC fuzz run итоговый A2 evidence gate открыт;
umbrella A2 и этап сборок в целом также не завершены.
