# Пакет зависимостей для сборки Linux

`solidarcad-build-deps-ubuntu24.04-x64.tar.zst` — переносимый SDK для сборки
SolidarCAD на Ubuntu 24.04 x86_64. Он устраняет повторную загрузку полного Qt и
повторную компиляцию OCCT на каждой машине.

## Состав

- минимальный Qt 6.8.3 из архива `qtbase`;
- vcpkg на baseline из `vcpkg.json`, включая неглубокие Git metadata для
  manifest mode, но без временных build trees и downloads;
- read-only бинарный кэш vcpkg для OCCT 8.0.1 и его транзитивных зависимостей;
- `env.sh` с `CMAKE_PREFIX_PATH`, `VCPKG_ROOT`, triplet и источниками кэша;
- manifest с версиями и commit исходного дерева;
- лицензии, notices и SHA-256 всего SDK-архива.

Пакет не содержит исходный код SolidarCAD и не является пользовательским
дистрибутивом приложения.

## Создание в CI

Ubuntu job в `.github/workflows/build.yml` использует локальный бинарный кэш
vcpkg, а после успешной сборки запускает:

```bash
bash scripts/package_linux_build_deps.sh \
  "$QT_ROOT_DIR" \
  "$VCPKG_ROOT" \
  "$GITHUB_WORKSPACE/.cache/vcpkg-binaries" \
  "$GITHUB_WORKSPACE/build/solidarcad-build-deps-ubuntu24.04-x64.tar.zst"
```

Workflow публикует архив и файл `.sha256` как GitHub Actions artifact
`solidarcad-build-deps-ubuntu24.04-x64`.

## Установка

Поместите архив и одноимённый `.sha256` в один каталог и выполните из корня
исходного дерева:

```bash
bash scripts/install_linux_build_deps.sh \
  /путь/к/solidarcad-build-deps-ubuntu24.04-x64.tar.zst
source "$HOME/.local/share/solidarcad-build-deps/current/env.sh"
```

Установщик проверяет SHA-256 архива и распаковывает его в версионированный по
хешу каталог. Ссылка `current` переключается на установленную версию без
удаления предыдущего SDK.

## Сборка

После активации `env.sh` используются обычные воспроизводимые команды:

```bash
cmake --preset ci
cmake --build --preset ci --parallel "$(nproc)"
QT_QPA_PLATFORM=offscreen ctest --preset ci
```

vcpkg восстанавливает готовые пакеты из SDK. Если manifest или triplet
изменились, новые пакеты собираются один раз и сохраняются в пользовательский
кэш `~/.cache/solidarcad-vcpkg-binaries`.

## Локальное создание

Создать пакет можно и после локальной Ubuntu-сборки. Перед конфигурацией нужно
включить файловый binary cache vcpkg, а затем передать генератору корень Qt,
vcpkg, кэша и выходной путь. Генератор намеренно отказывается создавать пакет,
если Qt, bootstrapped vcpkg или binary cache отсутствуют.
