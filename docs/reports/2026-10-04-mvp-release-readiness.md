# Готовность Windows x64 0.1.0 MVP — 04.10.2026

## Итог

Предрелизная автоматическая проверка успешна. Функциональный состав MVP и
release pipeline готовы к финальному release-candidate проходу. Выпуск пока
заблокирован обязательными ручными GPU/DPI и clean-machine проверками, после
которых необходимо повторить gate на точном релизном commit и теге.

Базовая ревизия исходного кода: `859eefbd17c0fb0c1be105a62cdd9c0d07b03d06`.
Во время этой проверки рабочее дерево содержало только документирование
актуального статуса и устранение устаревших утверждений; эти изменения ещё не
являются отдельным commit.

## Локальный автоматический gate

В окружении MSVC 2022, Qt 6.8.3, vcpkg triplet `ci-x64-windows` и OCCT 8.0.1:

- `cmake --preset ci` — успешно;
- `cmake --build --preset ci` — успешно;
- `ctest --preset ci` — 68/68 тестов успешно;
- `cmake --preset release` и `cmake --build --preset release` — успешно;
- `cmake --install build/release` — успешно;
- CPack и `scripts/finalize_release.py` — успешно.

Сводка CTest: 41 regression, 22 unit, 11 ui-smoke, 2 topology и 2 compliance
теста. Метки пересекаются, поэтому их сумма больше общего числа тестов.

Создан и проверен комплект:

- `SolidarCAD-0.1.0-Windows-AMD64.zip`;
- `SolidarCAD-0.1.0-Windows-AMD64.zip.sha256`;
- `SolidarCAD-0.1.0-Windows-AMD64.spdx.json`.

SHA-256 архива этой проверки:
`be3479936974c77567d165c59fda65345f0a5fa259225d5f688b513eb477b8d6`.

Этот хэш является предрелизным доказательством и не должен публиковаться как
финальный: после создания точного release-candidate commit архив необходимо
пересобрать и проверить заново.

## GitHub Actions

Run `37139763821` для базовой ревизии `859eefb` завершён успешно 3 октября
2026 года. Успешны обе matrix jobs:

- `windows-latest`, `ci-x64-windows`;
- `ubuntu-24.04`, `ci-x64-linux`.

Финальный критерий требует такого же успешного результата для точного
release-candidate commit и тега `v0.1.0`.

## Оставшиеся блокеры

1. Ручной GPU/DPI gate из `docs/testing.md`.
2. Clean-machine проверка точного финального архива.
3. После ручной приёмки: commit документации, повторный автоматический gate,
   дата в `CHANGELOG.md`, тег `v0.1.0`, GitHub Actions тега и публикация единого
   комплекта ZIP/SHA/SPDX/release notes.

Пункты 3 не выполняются заранее: `docs/release-checklist.md` запрещает выпуск,
пока оба ручных gate не закрыты.
