# Проверка продуктовых утверждений — 28.09.2026

Локальная база: `d994b2b` (Fix student testing regressions). Публичный HEAD на момент проверки через GitHub API: `1d88a9d25d1f0fd609e05040727ae9daf25d1ac0`. Прочитан публичный README по этому SHA: базовое описание согласуется с локальным README. Репозиторий не обновлялся и пользовательские изменения не перезаписывались.

GitHub API `/repos/ilikecinepol/SolidarCAD/releases` вернул `[]`. Поэтому версия, дата, размер, хэш и download URL пусты. Требования Windows 10/11, RAM/GPU не подтверждены готовым дистрибутивом и не заявлены.

| Утверждение | Источник в основном репозитории |
|---|---|
| Линии, дуги, окружности, геометрические и размерные ограничения | `src/sketch/Sketch.h`, `Sketch.cpp`, `SketchSolver.cpp` |
| Привязки и проецирование | `src/ui/SketchCanvas.cpp`: constructionSnapAt, Tool::Projection |
| Extrude, Pocket, Fillet, Chamfer, Revolve | README; `src/model/*Feature.cpp`, `docs/persistent-topology.md` |
| История и каскадный пересчёт | `src/model/Document.cpp`: recompute/recomputeFrom; README |
| Локальный .solidar с параметрами и историей | `src/project/ProjectFile.cpp`; README |
| Топологические ссылки | `docs/persistent-topology.md`; `TopologyReferenceResolver.cpp`. Семантические теги + геометрические сигнатуры, не гарантия сохранения любых split/merge. Маркетинговая гарантия persistent naming не добавлена. |
| A4, PDF и печать | `docs/eskd-profile.md`, README; полное соответствие ГОСТ не заявляется |
| STL / STEP | `src/io/StlExporter.cpp`, `StepExchange.cpp`; подключены в `src/ui/MainWindow.cpp` |
| Undo/redo | `src/ui/MainWindow.cpp`: undoLastAction/redoLastAction; SketchCanvas |
| C++20, Qt 6, OCCT 8.0.1, CMake | README, CMakeLists.txt |
| Официальный графический ресурс | `src/assets/solidarity-3d-logo.png`, используется `src/home/HomeWindow.cpp` |

## Как интерпретируется roadmap

Основной источник — раздел Roadmap README. Он частично устарел: OCCT, линии/дуги/окружности, picking, STEP/STL и undo/redo уже присутствуют. На сайте статусы уточнены по коду, а не скопированы как обещания будущей реализации. Completed относится только к базовой геометрии, а не ко всему Sketcher или готовности продукта к production. Упаковка MVP остаётся Planned согласно release-разделу README. AI API, collaboration, cloud и дата 1.0 не имеют подтверждённого официального плана и не включены.

Отчёт `docs/reports/2026-08-28-parametric-3d-status.md` полезен исторически, но его ограничение legacy-only topology устарело относительно `docs/persistent-topology.md`. Архитектурное утверждение об отсутствии OCCT в model тоже не используется.

Сайт описывает наличие реализации, не сертификацию или результаты нового полного CAD-регрессионного прогона. Лицензия собственного кода не выбрана: не добавлены вымышленные условия использования.
