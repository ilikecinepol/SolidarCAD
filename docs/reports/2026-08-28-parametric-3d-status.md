# Статус стабилизации параметрического 3D — 28.08.2026

## Итог

Задания ТЗ A–L выполнены. Модель поддерживает каскадное перестроение истории
Sketch → Extrude → Sketch-on-Face → Pocket → Fillet, контролируемые состояния
Dirty/Valid/Error и сохранение/загрузку полной редактируемой цепочки в формате
проекта v2. Старые проекты v1 продолжают открываться через compatibility path.

## Реализовано

- архитектурный аудит зафиксирован в
  `docs/reports/2026-08-28-parametric-3d-audit.md`;
- `Document::replaceSketchGeometry`, `markSketchDirty`, `recompute` и
  `recomputeFrom` обеспечивают централизованную инвалидацию зависимостей;
- `TopologyReference` хранит BodyId, FeatureId, тип подформы, legacy index и
  резервные поля persistent tag/geometric signature;
- Fillet поддерживает одну/несколько граней, безопасно переживает ошибочный
  радиус и восстанавливается после корректировки параметра;
- project v2 сохраняет ID, параметры Extrude/Pocket/Fillet, размещение и
  привязку эскизов; B-Rep восстанавливается перестроением истории;
- границы вызовов OCCT возвращают диагностируемую ошибку или пустой resolver
  result, не пропуская исключения ядра в UI;
- `scripts/generate_sbom.py` воспроизводимо создаёт SPDX 2.3 direct-build SBOM;
- CTest проверяет актуальность checked-in SBOM, CI генерирует профиль с
  фактической версией Qt и публикует его как artifact;
- реестр зависимостей и third-party notices синхронизированы с CI-профилем.

## Автотесты

Добавлены/расширены:

- `document_tests` — состояния, dirty propagation и recomputeFrom;
- `parametric_feature_chain_tests` — реальная сквозная 3D-цепочка и изменение
  исходного эскиза;
- `fillet_feature_tests` — topology resolver, несколько рёбер, invalid radius,
  oversized radius и восстановление;
- `project_file_tests` — round-trip полной цепочки проекта v2 с проверкой ID,
  параметров, support reference и объёма;
- `compliance_artifacts_tests` — воспроизводимость SBOM и согласованность
  compliance-артефактов.

Локальный gate: полная Debug-сборка MSVC/Qt/OCCT успешна; CTest — 15/15 после
исправления локального поиска Python launcher в compliance-тесте.

## Оставшиеся ограничения

- `TopologyReference` пока разрешает подформу по legacy index. Поля для
  persistent naming заложены, но алгоритм сопоставления по истории OCCT ещё не
  реализован.
- SPDX описывает прямые build inputs. Полный release SBOM должен формироваться
  после сканирования staging и включать транзитивные компоненты и хэши файлов.
- Лицензия собственного кода проекта всё ещё требует решения правообладателя.
