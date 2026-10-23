# Дорожная карта SolidarCAD

Актуальность: 23 октября 2026 года.

Этот документ — основной источник статуса этапов. `README.md` содержит краткое
резюме, `docs/release-checklist.md` — обязательный gate конкретного релизного
коммита, а датированные файлы в `docs/reports/` остаются историческими снимками.

## Текущая позиция

После автоматического RC gate 4 октября пользователь 18 октября явно
авторизовал components/product program как сознательное расширение scope до
публикации 0.1.0. Поэтому прежний RC больше не является финальным evidence для
расширенного `main`. Проверка 4 октября (`ci` 68/68, release ZIP/SHA/SPDX и
Windows/Ubuntu Actions для `859eefb`) остаётся историческим снимком, а не
текущим release approval. Публичный выпуск ещё не состоялся: changelog остаётся
`Unreleased`, тега `v0.1.0` нет. После закрытия авторизованного scope необходимо
повторить полный release checklist, clean CI/release packaging, полевой,
GPU/DPI, clean-machine и tag gates на новом финальном RC. Подробности прежней
проверки записаны в
`docs/reports/2026-10-04-mvp-release-readiness.md`.

| Этап | Статус | Критерий выхода |
| --- | --- | --- |
| Контур зависимостей и лицензирования | Завершён | Реестр, notices, лицензии, SBOM и compliance-тесты находятся в репозитории. |
| Параметрическое 3D и история | Завершён для MVP | Однодетальная история перестраивается, сохраняется и восстанавливается с устойчивыми ID и диагностикой. |
| Интерактивный Part Design и viewport | Завершён для MVP | Инструменты, preview, manipulators, picking, ruler и headless UI-smoke покрыты тестами. |
| Автоматическая готовность 0.1.0 | Scope расширен после прежнего RC; gate требуется повторить | Чистые `ci`/`release` сборки, 100% CTest и валидированный комплект ZIP/SHA/SPDX на финальном expanded-scope RC. |
| Полевое тестирование 0.1.0 | Текущий gate | Основной учебный сценарий проходится без падений, потери данных, неверной геометрии и вмешательства разработчика в файл проекта. |
| Ручная и публикационная готовность 0.1.0 | Текущий блокер | Пройдены полевой, GPU/DPI и clean-machine gates, зелёный CI тега, опубликован комплект 0.1.0. |

## 0.1.0 — Field Preview и выпуск MVP

Состав фиксируется на текущем однодетальном workflow: Sketch, Extrude, Pocket,
Revolve, Fillet, Chamfer, Shell, Draft, Mirror, Move, Linear Pattern, Circular
Pattern, редактируемая история, `.solidar`, STEP и STL.

До выпуска необходимо:

1. провести полевое тестирование основного учебного сценария: Sketch с
   ограничениями, Extrude/Pocket, редактирование истории, сохранение, повторное
   открытие и экспорт STEP/STL;
2. устранить подтверждённые падения, потерю данных, неверную геометрию и
   нарушения save/load, не расширяя функциональный состав;
3. создать чистый release-candidate commit на `main`;
4. выполнить автоматический gate из `docs/release-checklist.md` на этом commit;
5. выполнить ручной GPU/DPI gate и clean-machine проверку точного архива;
6. проверить Windows и Ubuntu GitHub Actions для тега;
7. поставить дату в changelog, создать `v0.1.0` и опубликовать ZIP, SHA-256,
   SPDX и release notes одним комплектом.

Правило scope freeze действовало для RC от 4 октября. Пользовательская
авторизация 18 октября была явным исключением и reset этого gate для
components/product program. Другие новые пользовательские функции в 0.1.0 не
добавляются без столь же явного пересмотра scope; весь release gate после
расширения выполняется заново.

## 0.1.x — полевая стабилизация

Приоритеты после выпуска:

- исправление повторяющихся затруднений из полевых тестов без изменения
  основного однодетального workflow;
- укрепление Undo/Redo, save/load, восстановления после неверного ввода и
  диагностики Sketch solver;
- сохранение совместимости существующих `.solidar` и добавление детерминированной
  регрессии для каждого подтверждённого дефекта;
- диагностика состава зависимостей, vulnerability scanning и процесс
  обновления зафиксированных Qt/OCCT/vcpkg версий;
- устранение предупреждений сборки без расширения функционального состава.

Критерий выхода: основные учебные сценарии проходятся без вмешательства
разработчика, а пользователь после Undo/Redo, сохранения и повторного открытия
может продолжить редактирование модели.

## 0.2 — Sketcher 2.0

Скетчер должен стать завершённым самостоятельным рабочим модулем до развития
производственных чертежей. Функциональный состав этапа:

- Polygon, Slot и Sketch Mirror;
- Trim, Extend и Offset;
- доведённая вспомогательная геометрия и устойчивое проецирование внешних рёбер;
- понятные состояния «недоопределён», «полностью определён» и «переопределён»;
- локализация конфликтующих ограничений и предсказуемое восстановление solver;
- полноценное редактирование driving/driven размеров и ограничений.

Text не блокирует `0.2.0`: преобразование шрифта в контуры планируется для
`0.2.x`, если полевые сценарии не поднимут его приоритет.

Надёжность мутаций является частью функционального состава. Создание,
изменение и удаление геометрии должны согласованно обновлять ограничения,
selection/hover, активный инструмент, solver, Undo/Redo и сериализацию.

Критерий выхода: сценарий «создание -> ограничения -> удаление -> Undo/Redo ->
сохранение -> открытие -> дальнейшее редактирование -> Extrude/Pocket» проходит
без падения, потери связей или молчаливого изменения геометрии.

## 0.3 — Parametric Core 2.0

- перевести загрузку, редактирование и viewport на `DocumentSketch` как единый
  источник истины и постепенно удалить параллельное состояние `sketchHistory_`;
- реализовать persistent topology v2 с provenance от операций и более богатыми
  семантическими именами для split/merge и повторяющейся геометрии;
- укрепить граф зависимостей, перестроение истории и диагностику ошибок;
- сохранить совместимость формата проекта либо выполнить явную версионированную
  миграцию.

Отдельный подэтап G0 «ядро графа зависимостей» закрыт 18 октября 2026 года:
единый `DocumentIndex` детерминированно обслуживает cross-body зависимости,
Sketch и face-support edges, структурный abort, affected closure и same-ID
recovery. Evidence закреплено в `parametric_history_tests`,
`feature_removal_tests`, `join_bodies_feature_tests` и
`pattern_persistence_tests`. Это не закрывает этап 0.3 целиком: persistent
topology v2 gate завершён, но DocumentSketch migration, Feature history reorder,
branching и suppression остаются открытыми.

Persistence gate P0 фиксирует
[`docs/project-format-v4.md`](project-format-v4.md): v4 — standalone
single-part topology-capable schema с обязательной отсортированной capability
`persistent-topology-v2` и строгими tagged V1/V2 references. Standalone v1-v3 не
меняются, mixed V1/V2 references разрешены, а V2 никогда не использует V1
fallback. Последовательность завершена 23 октября 2026 года; default для новых
и открытых v1-v3 документов остаётся V3, а конкретный writer выбирается явным
persistence state:

1. **P1 — carrier и consumers (завершён 21 октября 2026):** разрешённые
   in-memory producers и consumers, atomic rollback, boolean provenance,
   symmetry/pattern occurrence и terminal-output regressions закрыты;
2. **P2 — codec/staging (завершён 22 октября 2026):** строгий low-level decoder
   и staged validation/materialization были добавлены без регистрации v4 в
   `ProjectApplicationService`, UI open, autosave или recovery и без публикации
   application `Document`; application writer оставался запрещён до P3;
3. **P3 — writer/application gate (завершён 23 октября 2026):** каноническая
   атомарная запись v4 включена в application open; deterministic format-state
   matrix закреплена:
   v4 остаётся v4; v1-v3 и новые документы остаются v3 до явного upgrade;
   Save и Save As сохраняют state; native V2 authoring/commit в v3 запрещён до
   отдельной явной команды upgrade; удаление V2 не понижает формат.

В P1 уже заморожена и покрыта тестами boolean provenance algebra: правила
Generated/Modified/Retained/Merge, split/merge, canonical origins,
partial/complete coverage и collision policy. P3 сохранил эту алгебру без
переосмысления и подтвердил её save/load regressions. Исторический запрет на
writer до завершения P2/P3 снят только для явно выбранного V4 state. Ошибка Save
не меняет destination или format state.

Критерий выхода: сложная однодетальная модель выдерживает изменение размеров,
редактирование и перестановку допустимых шагов истории, сохранение и повторное
открытие без необходимости заново выбирать корректно сохранившиеся ссылки.

## 0.4 — производственный чертёж ЕСКД

Расширить независимый drawing-модуль до полного рабочего процесса: проверить
ячейки основной надписи, реализовать необходимые типы размеров, допуски и
обозначения поверхностей, поставить распространяемый ГОСТ-шрифт и добавить
эталонные PDF/print regression cases. Граница текущей реализации описана в
`docs/eskd-profile.md`.

ЕСКД начинается после Sketcher 2.0 и Parametric Core 2.0, чтобы ассоциативные
виды и размеры опирались на устойчивые геометрию, историю и топологические
ссылки.

## 0.5 — сборки

Вводить структуру изделия, экземпляры компонентов и сопряжения только после
стабилизации графа зависимостей и persistent topology v2. Формат сборок должен
ссылаться на устойчивые ID и не сериализовать B-Rep как источник истины.

18 октября 2026 года для D1 принята ветвь B: единый identity-bearing
product/component envelope. 23 октября D1 завершён; после него реализованы A1
foundation и ограниченный single-part persistence/UI срез A2. Решение и
границы миграции зафиксированы в
[`docs/decisions/2026-10-18-product-document-envelope.md`](decisions/2026-10-18-product-document-envelope.md).
Production editor теперь владеет одним `ProductDocument` с одним корневым
`PartDefinition` и вложенным существующим `Document`. Standalone V3/V4 остаются
identityless. Строгий V5 envelope имеет `ProjectId` и metadata; Open/Save
активны, явная «Преобразовать в документ изделия…» атомарно создаёт V5, а Save
As для V5 создаёт clone с новым `ProjectId`. Standalone Save As сохраняет V3/V4
без implicit identity. Stable model/topology IDs и корневой `DefinitionId`
сохраняются.

Historical v1 prism с legacy-extrusion marker в committed application state
безусловно fail-closed блокируется до chooser/prepare/writer/I/O (`89d3184`),
потому что текущая модель не может потерять его семантику. Service доверяет
переданному typed precondition, а production UI передаёт фактический marker.
Marker-absent v1-like carrier и авторитетные V2-V4 остаются допустимыми для
conversion. Прямой V5 fuzz scan и checked-in v1/v5 corpus seeds добавлены в
`022f97e`.

Следующие зависимости этого направления названы явно:

- **A2f:** Relocate/Move с сохранением `ProjectId`; до реализации нужна
  filesystem/replica transaction policy для canonical same-target/file-identity
  aliases (symlink, junction/reparse, hardlink), same-/cross-volume algorithms,
  overwrite, dirty/savepoint и external-modification, source-delete failure,
  rollback/crash recovery и two-replica state;
- **A2g:** semantics-preserving автоматическая миграция historical v1 prism;
- **A6a:** runtime enforcement конфликта одинакового `ExternalSourceKey` с
  различной revision/content hash.

Bounded single-part V5 slice реализован и покрыт CI regressions, но итоговый A2
evidence gate остаётся открытым до записанного Linux Clang non-MSVC fuzz run.
Umbrella A2 также открыт: multiple definitions, assemblies, occurrences, linked
resolver и mates не реализованы. A7 не активирован. Исходная датированная задача
остаётся неизменяемым историческим evidence и не заменяет актуальные ADR/format
docs.

## 0.6 — макросы и автоматизация

`src/model/macro` остаётся фундаментом, а не обещанной функцией ближайшего
релиза. Последовательность развития: семантический gateway операций, запись,
версионированный JSON и миграции, библиотека, затем UI. Макросы не должны
опережать укрепление solver, истории и persistent topology.

## Параллельный платформенный трек

- подписанный установщик Windows после стабилизации portable-пакета;
- ручная GPU/clean-machine проверка Linux, затем поддерживаемый бинарный пакет;
- воспроизводимое зеркало критических build inputs и регламент обновления
  зависимостей.

## Сквозные правила качества

- каждый подтверждённый дефект получает минимальную детерминированную
  регрессию;
- падение, потеря данных, неверная геометрия и нарушение save/load блокируют
  ближайший релиз;
- неудобство, не мешающее завершить основной сценарий, планируется в ближайший
  стабилизационный выпуск;
- новый этап не начинается до выполнения критерия выхода предыдущего этапа.

## Правило обновления

При смене этапа одновременно обновляются этот документ, краткий раздел Roadmap
в `README.md`, `CHANGELOG.md` и соответствующий release/status report. Старые
датированные отчёты не переписываются как текущие — они помечаются историческими.
