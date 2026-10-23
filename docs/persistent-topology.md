# Persistent topology v1/v2

SolidarCAD поддерживает два разных контракта топологических ссылок. V1 ищет
геометрию по семантическим тегам и сигнатурам. V2 использует точный provenance
ключ, опубликованный поддержанным producer-ом. В standalone-документе v4
разрешена смесь обоих вариантов, но каждый вариант всегда разрешается только
своим resolver path.

## V1: геометрическая идентичность

V1-ссылка хранит `BodyId`, `FeatureId`, kind Face/Edge, опциональный
семантический тег, геометрическую сигнатуру и legacy index. Планарная грань
может получить тег наподобие `planar:max-z`. Face signature содержит тип
поверхности, площадь, центр, ориентированную нормаль, bounds и параметры
цилиндра; Edge signature — тип кривой, длину, midpoint, tangent, bounds и
параметры окружности.

`TopologyReferenceResolver` применяет порядок:

1. единственное совпадение семантического тега;
2. единственное лучшее совпадение геометрической сигнатуры;
3. legacy index, только если persistent evidence отсутствует;
4. явный `Missing` или `Ambiguous` diagnostic.

Централизованная tolerance policy допускает 35% диагонали target shape для
нормализованной позиции, 35% относительного изменения площади, длины или
радиуса, cosine направления не ниже 0.996 и считает оценки с разницей менее
0.01 неоднозначными. Resolver выполняет один линейный B-Rep traversal и не
использует tessellation, pointer identity или whole-shape hash.

Если tag/signature присутствует, но безопасного совпадения нет, V1 не
откатывается к index. Старые файлы без persistent evidence сохраняют прежний
index fallback. Ограничения V1 остаются прежними: произвольные split/merge,
сильные topology changes и симметричные геометрически неразличимые кандидаты
могут завершиться `Missing` или `Ambiguous`.

## V2: точный provenance

`FaceReference` и `EdgeReference` являются discriminated carriers V1/V2.
`TopologyReferenceV2` хранит owning `BodyId` и полный typed
`TopologyProvenanceKey`: producer `FeatureId`, kind, operation, semantic role,
локальное имя, immediate origins и опциональный authored occurrence slot.
Origins не рекурсивны. Pattern slot принадлежит producer-у и не является
assembly occurrence identity.

V2 resolver выполняет exact lookup полного ключа в `TopologyLineage` owning
feature. При `Missing`, `Ambiguous`, owner mismatch или отсутствии lineage
переход к V1 tag/signature/index запрещён. Factory V2 также не выводит identity
из raw ordinal: она возвращает ссылку только для уже опубликованного exact
binding.

### Поддержанные producers P1

Lineage публикуется только следующими строгими путями:

- single-profile sketch Extrude NewBody;
- strict sketch Join/Cut и V2 face-source Join/Cut Extrude;
- single-profile Revolve NewBody и strict Revolve Join/Cut;
- strict Pocket;
- JoinBodies, когда оба inputs имеют `CompleteFaces`;
- Move, Mirror, LinearPattern и CircularPattern от source с `CompleteFaces`.

Imported, profile-override и partial-lineage пути, а также неподдержанные
multi-region/annular sketch-profile пути вне перечисленных single-profile
producers сохраняют V1 behavior и не изобретают V2. Это ограничение не
относится ко всем annular solids: например, перечисленные выше поддержанные
пути Revolve публикуют V2. Ошибка capture, collision, неполное evidence или
неподдержанный split завершается до публикации current shape/lineage/index и
сохраняет last-valid state.

Для face lineage `CompleteFaces` означает полное покрытие канонических граней,
а не всех рёбер. Edge V2 публикуется только для уникального замкнутого
двухгранного manifold edge: пара immediate identities соседних output faces
должна быть уникальна во всём result. Seam, open, non-manifold, degenerate и
edges с повторяющейся face pair намеренно отсутствуют и дают `Missing` без V1
fallback.

### Consumers и terminal outputs

Exact V2 разрешают:

- Sketch support на Face;
- face-source Extrude;
- face-plane Mirror;
- Shell removed faces;
- Draft drafted faces, body-face neutral plane и rotation edge;
- Fillet и Chamfer edge lists.

Fillet, Chamfer, Shell и Draft в P1 являются consumer-only terminal outputs.
Они строят валидный B-Rep, но не публикуют lineage собственного результата.
Их V1 factories продолжают работать, а Face/Edge V2 factories исчерпывающе
возвращают `Missing`. Это явная capability boundary, а не попытка восстановить
identity по порядку OCCT subshapes.

## Persistence boundary

Production writer v3 атомарно отвергает любой V2 carrier и не понижает его до
V1. Контракт standalone v4 описан в `docs/project-format-v4.md`: P1-P3 закрыли
in-memory carrier, producers, consumers, rollback, reader/writer и application
gate. Новый или открытый v1-v3 документ остаётся в состоянии V3; открытый v4 —
в V4; явный `Upgrade to v4` атомарно переводит V3 в V4. Writer выбирается
текущим application persistence state, а не содержимым: native V2
authoring/commit в V3 заблокирован, удаление последней V2-ссылки не понижает V4.
Derived B-Rep и runtime lineage никогда не сериализуются; после загрузки они
перестраиваются из параметров и provenance.
