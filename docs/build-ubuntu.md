# Сборка и установка SolidarCAD из исходников на Ubuntu

Инструкция рассчитана на **Ubuntu 24.04 LTS x86_64**. Это та же версия
Ubuntu, на которой проект проверяется в GitHub Actions. Используется
воспроизводимый профиль `ci`: Qt 6.8.3 устанавливается отдельно, а
Open CASCADE Technology 8.0.1 собирается из закреплённого manifest-файла
через vcpkg.

> Для обычной установки используйте готовый `.deb`, как описано ниже. Остальная
> часть документа нужна разработчикам, которые собирают проект из исходников.
> Более новые версии Ubuntu могут работать, но пока не входят в проверяемую
> конфигурацию проекта.

## Установка готового пакета

Скачайте `solidarcad_0.1.0-1_amd64.deb` из артефакта Ubuntu-сборки или со
страницы релиза и выполните в каталоге загрузки одну команду:

```bash
sudo apt install ./solidarcad_0.1.0-1_amd64.deb
```

После установки приложение доступно в меню рабочего стола и командой
`solidar`. Файлы `*.solidar` регистрируются как проекты SolidarCAD. Удаление:

```bash
sudo apt remove solidarcad
```

Пакет предназначен для Ubuntu 24.04 x86_64. Он включает Qt 6.8.3 и остальные
несистемные runtime-зависимости в `/opt/solidarcad`; `apt` автоматически
устанавливает требуемые системные графические библиотеки.

## 1. Установите системные зависимости

Откройте терминал и выполните:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git curl ca-certificates \
  zip unzip tar zstd pkg-config \
  python3 python3-venv \
  autoconf autoconf-archive automake libtool \
  libgl1-mesa-dev libxkbcommon-x11-0 libxcb-cursor0
```

Проверьте архитектуру и версии основных инструментов:

```bash
grep '^PRETTY_NAME=' /etc/os-release
uname -m
cmake --version
g++ --version
ninja --version
```

Ожидаются Ubuntu 24.04 LTS, архитектура `x86_64`, CMake 3.24 или новее и
компилятор с поддержкой C++20. В Ubuntu 24.04 штатный GCC 13 подходит. Если
система сообщает другую версию Ubuntu, продолжить можно, но такая конфигурация
не проверяется в CI проекта.

## 2. Получите исходный код

```bash
git clone https://github.com/ilikecinepol/SolidarCAD.git
cd SolidarCAD
```

Все следующие команды, если явно не указано иное, выполняются из корня
репозитория.

## Быстрый путь: готовый пакет зависимостей

Успешная Ubuntu-сборка в GitHub Actions создаёт артефакт
`solidarcad-build-deps-ubuntu24.04-x64`. Он содержит минимальный Qt 6.8.3,
закреплённый vcpkg и бинарный кэш уже собранного OCCT 8.0.1. Скачайте из
артефакта файлы `.tar.zst` и `.tar.zst.sha256` в один каталог, затем выполните:

```bash
bash scripts/install_linux_build_deps.sh \
  "$HOME/Загрузки/solidarcad-build-deps-ubuntu24.04-x64.tar.zst"
source "$HOME/.local/share/solidarcad-build-deps/current/env.sh"
```

После этого переходите сразу к разделу «Соберите проект». Профиль `ci`
восстановит OCCT из локального бинарного кэша вместо повторной загрузки и
компиляции. Системные пакеты из раздела 1 всё равно должны быть установлены.

Если готового артефакта ещё нет, выполните ручные шаги 3–5 ниже. Они нужны
один раз; после первой успешной CI-сборки пакет создаётся автоматически.

## 3. Установите Qt 6.8.3

Пакет `qt6-base-dev` из Ubuntu 24.04 содержит Qt 6.4.2 и не подходит: проект
требует Qt не ниже 6.5, а профили `ci` и `release` проверяют точную версию
6.8.3. Установите Qt в домашний каталог с помощью `aqtinstall`:

```bash
python3 -m venv "$HOME/.local/share/solidarcad-aqt"
source "$HOME/.local/share/solidarcad-aqt/bin/activate"
python -m pip install --upgrade pip aqtinstall
```

В некоторых сетях `download.qt.io` недоступен. Одного параметра `--base`
недостаточно: aqtinstall продолжает запрашивать контрольные суммы на основном
сервере. Создайте отдельную конфигурацию с двумя зеркалами из официального
списка Qt:

```bash
mkdir -p "$HOME/.config"
tee "$HOME/.config/solidarcad-aqt.ini" >/dev/null <<'EOF'
[aqt]
baseurl: https://mirror.accum.se/mirror/qt.io/qtproject

[requests]
connection_timeout: 10
response_timeout: 60
max_retries_on_connection_error: 5
retry_backoff: 0.5
max_retries_on_checksum_error: 5
max_retries_to_retrieve_hash: 5
hash_algorithm: sha1
INSECURE_NOT_FOR_PRODUCTION_ignore_hash: False

[mirrors]
trusted_mirrors:
    https://mirror.accum.se/mirror/qt.io/qtproject
    https://www.nic.funet.fi/pub/mirrors/download.qt-project.org
fallbacks:
    https://www.nic.funet.fi/pub/mirrors/download.qt-project.org
EOF
```

Затем установите Qt:

```bash
aqt -c "$HOME/.config/solidarcad-aqt.ini" \
  install-qt linux desktop 6.8.3 linux_gcc_64 \
  --outputdir "$HOME/Qt" \
  --archives qtbase

deactivate
```

`linux_gcc_64` — имя архитектуры в репозитории Qt. После распаковки aqtinstall
создаёт для неё каталог `gcc_64`, поэтому путь в следующих командах указан как
`$HOME/Qt/6.8.3/gcc_64`.

Зеркала Qt публикуют рядом с архивами контрольные суммы SHA-1. Приведённая
конфигурация использует их для проверки целостности и явно оставляет
`INSECURE_NOT_FOR_PRODUCTION_ignore_hash: False`. SHA-1 является устаревшим и
более слабым алгоритмом, чем SHA-256, поэтому при доступном `download.qt.io`
предпочтительна стандартная конфигурация aqtinstall. В сети, где основной
сервер заблокирован, этот вариант лучше полного отключения проверки хешей.

Проверьте установленную версию:

```bash
"$HOME/Qt/6.8.3/gcc_64/bin/qmake" -query QT_VERSION
```

Команда должна вывести `6.8.3`.

## 4. Установите закреплённый vcpkg

Версия vcpkg должна совпадать с `builtin-baseline` из `vcpkg.json`:

```bash
git clone https://github.com/microsoft/vcpkg.git vcpkg
git -C vcpkg checkout --detach 00c5775211f45cd08b37fce0484b4cb940e422ab
./vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

OCCT отдельно устанавливать не нужно: при конфигурации vcpkg соберёт
Open CASCADE 8.0.1 согласно `vcpkg.json` и `triplets/ci-x64-linux.cmake`.

## 5. Настройте окружение

В текущем терминале выполните:

```bash
export SOLIDAR_QT_ROOT="$HOME/Qt/6.8.3/gcc_64"
export CMAKE_PREFIX_PATH="$SOLIDAR_QT_ROOT"
export VCPKG_ROOT="$PWD/vcpkg"
export VCPKG_DEFAULT_TRIPLET=ci-x64-linux
export SOLIDAR_VCPKG_WRITE_CACHE="$HOME/.cache/solidarcad-vcpkg-binaries"
mkdir -p "$SOLIDAR_VCPKG_WRITE_CACHE"
export VCPKG_BINARY_SOURCES="clear;files,$SOLIDAR_VCPKG_WRITE_CACHE,readwrite"
```

Эти переменные нужно задавать снова после открытия нового терминала. Чтобы
не повторять команды вручную, их можно добавить в `~/.bashrc`, заменив
`$PWD/vcpkg` абсолютным путём к каталогу `vcpkg` внутри SolidarCAD.

## 6. Соберите проект

```bash
cmake --preset ci
cmake --build --preset ci --parallel "$(nproc)"
```

Первая конфигурация собирает OCCT и его зависимости, поэтому может занять
десятки минут. Последующие сборки используют уже подготовленные файлы vcpkg.
Если системе не хватает оперативной памяти, ограничьте параллелизм, например:

```bash
cmake --build --preset ci --parallel 2
```

## 7. Запустите тесты

```bash
QT_QPA_PLATFORM=offscreen ctest --preset ci
```

Все тесты должны завершиться успешно. Для повторного запуска одного теста:

```bash
QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build/ci -R '<имя-теста>' -V
```

## 8. Запустите SolidarCAD

```bash
./build/ci/src/solidar
```

Открыть существующий проект можно сразу из командной строки:

```bash
./build/ci/src/solidar /путь/к/модели.solidar
```

Если в сеансе Wayland окно не открывается или некорректно работает 3D-вид,
запустите приложение через XWayland:

```bash
QT_QPA_PLATFORM=xcb ./build/ci/src/solidar
```

## 9. Выполните локальную установку

Установить собранный исполняемый файл в домашний каталог можно так:

```bash
cmake --install build/ci --prefix "$HOME/.local/opt/SolidarCAD"
```

Qt установлен вне системного пути, поэтому запускайте локальную установку с
путём к его библиотекам:

```bash
LD_LIBRARY_PATH="$SOLIDAR_QT_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$HOME/.local/opt/SolidarCAD/bin/solidar"
```

Для обычной разработки проще запускать `build/ci/src/solidar`: CMake уже
задаёт для него пути к библиотекам сборочного дерева. Каталог, созданный
`cmake --install`, не является автономным переносимым Linux-пакетом.

## Диагностика типичных проблем

### CMake нашёл другую версию Qt

Убедитесь, что переменная указывает именно на Qt 6.8.3, затем выполните
чистую повторную конфигурацию:

```bash
export CMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/gcc_64"
cmake --preset ci --fresh
```

### aqtinstall не находит `qt_base`

Проверьте, что в команде установки указано именно `linux_gcc_64`:

```bash
aqt -c "$HOME/.config/solidarcad-aqt.ini" \
  install-qt linux desktop 6.8.3 linux_gcc_64 \
  --outputdir "$HOME/Qt" \
  --archives qtbase
```

Для Qt 6.8.3 значение `gcc_64` нельзя использовать как имя загружаемой
архитектуры: в репозитории пакет называется `linux_gcc_64`. При этом каталог
установленного комплекта по-прежнему называется `gcc_64`.

### Qt сообщает об ошибке плагина `xcb`

Установите основные зависимости плагина:

```bash
sudo apt install -y \
  libx11-xcb1 libxcb1 libxcb-cursor0 libxcb-icccm4 \
  libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
  libxcb-render-util0 libxcb-shape0 libxcb-shm0 libxcb-sync1 \
  libxcb-util1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb1 \
  libxkbcommon-x11-0
```

Проверить отсутствующие динамические библиотеки можно командой:

```bash
ldd build/ci/src/solidar | grep 'not found'
```

### Тесты не видят графический дисплей

Запускайте их с `QT_QPA_PLATFORM=offscreen`, как показано выше. Для запуска
самого приложения нужен активный графический сеанс Ubuntu.

### `download.qt.io` или выбранное зеркало недоступно

Откройте `$HOME/.config/solidarcad-aqt.ini` и поменяйте местами шведский и
финский адреса в `baseurl`, `trusted_mirrors` и `fallbacks`. Затем повторите:

```bash
source "$HOME/.local/share/solidarcad-aqt/bin/activate"
aqt -c "$HOME/.config/solidarcad-aqt.ini" \
  install-qt linux desktop 6.8.3 linux_gcc_64 \
  --outputdir "$HOME/Qt" \
  --archives qtbase

deactivate
```

Предупреждение `Failed to download checksum ... Updates.xml` само по себе ещё
не означает неудачную установку. Ориентируйтесь на итоговую строку `ERROR` и
проверку версии через `qmake`. Фатальная ошибка проверки архива `.7z` означает,
что ни одно доверенное зеркало не отдало его `.sha1`-файл. Не используйте
`--UNSAFE-ignore-hash`: отключать проверку загружаемых архивов не следует.

### Загрузка исходников vcpkg прерывается

Повторите команду после проверки доступа к интернету, прокси и свободного
места. Незавершённая первая конфигурация CMake обычно может быть запущена
повторно после восстановления соединения.

## Источники и актуальные ограничения

- [Qt 6.8: поддерживаемые платформы](https://doc.qt.io/qt-6.8/supported-platforms.html)
- [Qt for Linux/X11: требования](https://doc.qt.io/qt-6.8/linux.html)
- [Документация aqtinstall](https://aqtinstall.readthedocs.io/en/stable/)
- [Настройка зеркал aqtinstall](https://aqtinstall.readthedocs.io/en/stable/configuration.html)
- [Официальный список зеркал Qt](https://download.qt.io/static/mirrorlist/)
- [Требования vcpkg к Linux-хосту](https://learn.microsoft.com/vcpkg/concepts/supported-hosts)
- [Профили сборки SolidarCAD](../CMakePresets.json)
- [Manifest зависимостей SolidarCAD](../vcpkg.json)
