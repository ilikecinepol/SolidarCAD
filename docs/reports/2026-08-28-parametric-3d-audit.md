# Аудит параметрической 3D-архитектуры

Дата: 28 августа 2026 г.
База: `main` / `fc80fe3`

## Текущая схема

`Document` владеет стабильными `SketchId`, коллекцией `DocumentSketch` и
упорядоченным списком `Body`. Каждый `Body` владеет линейной
`std::vector<std::unique_ptr<ShapeFeature>>`; порядок вектора является порядком
истории и одновременно порядком recompute.

`Feature` уже предоставляет общий фундамент состояния:

- `Dirty` — требуется перестройка;
- `Valid` — последняя перестройка успешна;
- `Error` — перестройка завершилась диагностируемой ошибкой.

`Error` семантически соответствует требуемому в ТЗ `Failed`: сообщение хранится
в `Feature::error()`, а `ShapeFeature` очищает старую форму перед неуспешной
перестройкой. Переименование enum само по себе ценности не добавляет; лучше
сохранить совместимость и усилить семантику/тесты.

`Body::rebuild` последовательно передаёт результат предыдущего Feature через
`RebuildContext::previousShape`. После первого dirty Feature все последующие
помечаются dirty. При ошибке дальнейшая перестройка прекращается, downstream
остаётся dirty. После каждого успешного Feature вызывается обновление
размещений прикреплённых эскизов.

`Document::rebuild` перестраивает Body в порядке документа и прекращает работу
на первой ошибке. API `recomputeFrom(featureId)` отсутствует.

## Зависимости операций

- `ExtrudeFeature` хранит `profileSketchId`, длину, operation и reversed.
- `PocketFeature` хранит `profileSketchId` и глубину; входная форма — предыдущий
  Feature того же Body.
- `FilletFeature` хранит список `EdgeReference` и радиус; базовая форма —
  предыдущий Feature.
- Sketch-on-Face представлен `DocumentSketch::support` с `FaceReference` на
  Body/Feature и `faceIndex`; placement вычисляется из формы owner Feature.

Явного графа upstream/downstream нет. Зависимости на предыдущую форму выводятся
из порядка Body, зависимости на Sketch и support хранятся по ID. Этого
достаточно для текущей линейной истории, но нет единого метода определить
первый затронутый Feature после изменения Sketch.

## Построение и ошибки OCCT

`ExtrudeFeature`, `PocketFeature`, `FilletFeature` и профильный builder уже
перехватывают `Standard_Failure` и преобразуют её в `markError`. В ряде мест
последний защитный слой использует общий catch с короткой диагностикой. Старый
shape очищается перед rebuild, поэтому stale geometry не выдаётся как валидная.

Остаётся проверить единообразие сообщений и то, что все boolean/build paths
имеют `Standard_Failure`, `std::exception` и последний диагностируемый слой.

## Изменение Sketch и dirty propagation

`sketch::Sketch` не знает о `Document` и не должен зависеть от него. Изменение
геометрии само по себе не уведомляет `Document`. UI вручную копирует геометрию
в `DocumentSketch`, ищет затронутый Extrude и вызывает `Body::markDirtyFrom`.
Таким образом, core API допускает изменение `document.sketches()` без dirty
propagation, а integration tests вынуждены вручную помечать историю.

Минимальное исправление: ввести document-level mutation/gateway
(`markSketchDirty`/`recomputeFromSketch`) и `recompute`/`recomputeFrom`, не
перенося CAD-логику в UI и не создавая DAG scheduler.

## Topology references

`FaceReference` и `EdgeReference` уже содержат `BodyId`, owner `FeatureId` и
legacy index, но это две разрозненные структуры. Разрешение edge находится в
`TopologyReferenceResolver`, а face placement — в `SketchPlacement`.

Минимальный foundation: общий `TopologyReference` с типом subshape,
`ownerBodyId`, `ownerFeatureId`, `legacyIndex` и резервом под signature/tag;
typed wrappers сохраняются для совместимости, но resolver и новая persistent
модель используют централизованное представление. Полное persistent naming в
scope не входит.

## Сериализация `.solidar`

`ProjectFile` format version 1 сериализует:

- box parameters;
- геометрию Sketch, constraints/dimensions и строковый support;
- только флаг `hasExtrusion` и индекс source sketch.

Реальные `Document`, `Body`, Feature IDs, Feature types/parameters/order,
SketchPlacement, FaceReference, Pocket и Fillet не сохраняются. `MainWindow`
после загрузки реконструирует лишь упрощённый Extrude. Это главный блокер
сценария Save → Load → Edit → Recompute.

Минимальное расширение: version 2 с полноценным `Document`-снимком, при этом
сохранить чтение version 1. Persistent payload должен оставаться Qt/Core DTO и
не содержать `TopoDS_Shape`; формы восстанавливаются через recompute.

## Слабые места

1. Нет document-level фиксации изменения Sketch и dirty propagation.
2. Нет `Document::recomputeFrom(FeatureId)` и structured recompute result.
3. Ошибка одного Body останавливает перестройку остальных Body.
4. `Body::resultShape` возвращает только последний Feature; при downstream
   failure доступ к последней валидной upstream форме возможен только через
   историю, что нужно явно учитывать в UI.
5. Face/edge index зависит от порядка обхода OCCT и может измениться после
   upstream rebuild.
6. Project format v1 не хранит параметрическую историю.
7. Compliance SBOM статичен и не содержит vcpkg/CMake как packages.
8. CI сразу запускает compliance test, но не генерирует SBOM и не сохраняет его
   как artifact.

## Минимальный план изменений

1. Сохранить существующий `FeatureState::Error` как эквивалент Failed, добавить
   явные helpers и тесты переходов; усилить Body/Document recompute API.
2. Добавить `Document::markSketchDirty`, `recompute` и `recomputeFrom`, определяя
   первый Feature по profile/support dependency и линейной истории.
3. Ввести общий `TopologyReference` и централизованный resolver с legacy index.
4. Добавить integration test полной цепочки и стресс-набор Fillet.
5. Добавить project format v2 для Document/Body/Feature history с обратным
   чтением v1; после load выполнять recompute.
6. Аудировать OCCT catches и диагностируемое downstream failure.
7. Добавить детерминированный stdlib-only SBOM generator, validation и CI steps.
8. Обновить архитектурную документацию и итоговый статус-отчёт.

## Планируемые затрагиваемые файлы

- `src/model/Feature.*`, `Body.*`, `Document.*`;
- `src/model/SketchPlacement.*`, `TopologyReferenceResolver.*`;
- `src/model/ExtrudeFeature.*`, `PocketFeature.*`, `FilletFeature.*`;
- `src/project/ProjectFile.*` и минимальная адаптация `MainWindow`;
- `tests/CMakeLists.txt`, существующие feature/project tests и новые chain tests;
- `scripts/`, `.github/workflows/build.yml`, `sbom/`;
- `docs/architecture.md`, `docs/modules.md`, compliance-файлы и отчёты.

Новые библиотеки, CAD-команды и UI redesign не требуются.
