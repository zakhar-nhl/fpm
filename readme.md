[ X _ X ] FPM - v.0.2.0-Alpha x86_64 | NON HUMAN LINUX

Hello and Welcome to the FPM Package Manager Repository!

Features of FPM:
* C++17, standard library only (zero external C++ dependencies)
* CMake build system
* SLAG-AR package format (Simple Lightweight Archive & Generated-manifest Architecture)
* Mirror-based repository updates with automatic mirror selection
* SHA256-based integrity verification

(Recommended) Build via CMake:

1 cd ~/fpm

2 cmake -B build

3 cmake --build build

Fast build without CMake:

mkdir -p build
g++ -std=c++17 -Iinclude \
    src/main.cpp src/package.cpp src/logger.cpp src/database.cpp \
    src/slag.cpp src/mirror_selector.cpp src/ui.cpp src/cleaner.cpp \
    src/suggest.cpp src/fuzzy_search.cpp src/json.cpp src/i18n.cpp -o build/fpm

Installation into the system:

sudo cmake --install build

main commands:

USAGE:
  fpm [OPTION] [PACKAGE...]

CORE OPERATIONS:
  -i,   install <pkg...>    Install specified package(s)
  -r,   remove <pkg...>     Remove package(s), keeping configuration files
  -p,   purge <pkg...>      Completely remove package(s) and configurations
  -s,   search <query>      Search for packages by name (exact, prefix, substring)
  -fs,  fsearch <query>     Deep search: name, description and fuzzy matches
  -qs,  qsearch <query>     Search installed packages only
  -info info <pkg>          Display detailed package information
  -l,   list [query]        List all currently installed packages (filter by query)
  -pkg  <file.fpm>          Install local .fpm package file

SYNCHRONIZATION & MAINTENANCE:
  -upd, update              Update local repository database from mirrors
  -upg, upgrade             Upgrade all installed packages to latest version
  -su,  sysup               Execute full system update (update + upgrade)
  -ar,  autoremove          Remove orphaned dependencies
  -o,   orphans             List orphaned dependencies (alias -orphans)
  -c,   clean               Clear cached download archives
  -cmd, cleandisk           CleanMyDisk: cache + temporary files
  -v,   verify <pkg>        Verify integrity of installed package files (alias -verify)

BUILD & DEVELOPMENT:
  -b,   build <dir>         Build FPM package from recipe directory
  -repo <dir> [--base URL]  Generate an FPM repository index from .fpm packages

SYSTEM & DEBUG:
  -ms,  mselect             Mirror selector (probe candidate pool, pick active mirror)
  -am,  add-mirror <URL>    Add a mirror to the candidate pool (--active -> active mirror)
  -dm,  del-mirror <URL>    Delete a user-added mirror from the pool and active list
  -rm,  reset-mirror        Reset active mirror to the default fallback (https://geo.mirror.pkgbuild.com/)
  -n,   news                Show FPM release notes for this version
  -vr,  version             Display FPM version and build architecture
  -h,   help                Display this help message

EXAMPLES:
  fpm -su                Full system update
  fpm -i gcc git         Install multiple packages
  fpm -p nano            Purge package with configuration files
  fpm -b ./mypkg/        Build local package recipe
  fpm -am https://mirror.example.org/mirror/   Add a candidate mirror
  fpm -ms                                      Pick the active mirror interactively

Directory Layout:
  /var/lib/fpm/local/<pkg>/   Installed package metadata and manifest
  /var/cache/fpm/repo/        Downloaded repository databases
  /var/cache/fpm/packages/    Downloaded .fpm packages
  /etc/fpm/mirrors.list       Active mirror(s) used by -upd / -i
  /etc/fpm/candidates.list    Mirror candidate pool used by -ms (100+ mirrors)
  etc/mirrors.list            Repo-local fallback mirror list

Mirror configuration:
  -upd and -i use the ACTIVE mirror(s) in mirrors.list. -ms probes the full
  candidate pool (candidates.list), ranks mirrors by latency + repository
  support, and rewrites mirrors.list to the single mirror you pick.
  -am <URL> adds a URL to candidates.list; with --active it goes straight
  into mirrors.list. Both commands verify reachability first.
  Config is per-name-space: a "$HOME/fpm/etc" directory owns its own
  mirrors.list + candidates.list; otherwise the system /etc/fpm files are
  used. The reserved fallback mirror (https://geo.mirror.pkgbuild.com/) is
  hardcoded in the binary and is ALWAYS tried when every active mirror fails
  (404/500/timeout/broken index), so an update can never brick the system.

Repository backend:
  To publish a repository, place .fpm packages in a directory and run:
    fpm -repo /srv/fpm/repo --base https://repo.example.com/fpm
  This writes packages.db (canonical FPM text index) and packages.json.
  Serve that directory over HTTP/HTTPS and add its URL to mirrors.list.
  A mirror only counts as working when its index downloads and passes FPM
  validation. As of v0.2.0 no public production FPM repository exists, so
  no real public mirrors are configured yet - Add only published FPM
  repository mirrors to etc/mirrors.list, never random distro mirrors.

  -------------------------------------------------------------------------------

Привет и добро пожаловать в репозиторий пакетного менеджера FPM!

Особенности FPM:
* C++17, только стандартная библиотека (ноль внешних C++ зависимостей)
* Система сборки CMake
* Формат пакетов SLAG-AR
* Обновление репозиториев через зеркала с автоматическим выбором сервера
* Проверка целостности на основе SHA256

(Рекомендуется) Сборка через CMake:

1 cd ~/fpm

2 cmake -B build

3 cmake --build build

Быстрая сборка без CMake:

mkdir -p build
g++ -std=c++17 -Iinclude \
    src/main.cpp src/package.cpp src/logger.cpp src/database.cpp \
    src/slag.cpp src/mirror_selector.cpp src/ui.cpp src/cleaner.cpp \
    src/suggest.cpp src/fuzzy_search.cpp src/json.cpp src/i18n.cpp -o build/fpm

Установка в систему:

sudo cmake --install build

Основные команды:

ИСПОЛЬЗОВАНИЕ:
  fpm [ОПЦИЯ] [ПАКЕТ...]

ОСНОВНЫЕ ОПЕРАЦИИ:
  -i,   install <pkg>    Установить указанный пакет(ы)
  -r,   remove <pkg>     Удалить пакет(ы), сохранив конфигурационные файлы
  -p,   purge <pkg>      Полностью удалить пакет(ы) и их конфигурации
-s,   search <query>   Поиск пакетов по имени (точное, префикс, подстрока)
  -fs,  fsearch <query>  Глубокий поиск: имя, описание и нечеткие совпадения
  -qs,  qsearch <query>  Поиск только среди установленных пакетов
  -info info <pkg>      Показать подробную информацию о пакете
  -l,   list [query]    Список установленных пакетов (фильтр по запросу)
  -pkg  <file.fpm>       Установить локальный файл пакета .fpm

СИНХРОНИЗАЦИЯ И ОБСЛУЖИВАНИЕ:
  -upd, update           Обновить локальную базу данных репозиториев
  -upg, upgrade          Обновить все установленные пакеты до последней версии
  -su,  sysup            Выполнить полное обновление системы (update + upgrade)
  -ar,  autoremove       Удалить неиспользуемые (Осиротевшие) зависимости
  -o,   orphans          Показать неиспользуемые (Осиротевшие) зависимости (алиас -orphans)
  -c,   clean            Очистить кэш загруженных архивов
  -cmd, cleandisk        Очистить кэш и временные файлы
  -v,   verify <pkg>     Проверить целостность файлов установленного пакета (алиас -verify)

СБОРКА И РАЗРАБОТКА:
  -b,   build <dir>       Собрать пакет FPM из директории рецепта
  -repo <dir> [--base URL]  Сгенерировать индекс репозитория FPM из .fpm пакетов

СИСТЕМА И ОТЛАДКА:
  -ms,  mselect           Выбор активного зеркала (проверка пула candidates.list, замер задержки)
  -am,  add-mirror <URL>  Добавить зеркало в пул кандидатов (--active -> активное зеркало)
  -dm,  del-mirror <URL>  Удалить добавленное вами зеркало из пула и активного списка
  -rm,  reset-mirror      Вернуть активное зеркало на дефолтный фолбэк (https://geo.mirror.pkgbuild.com/)
  -n,   news              Показать релиз-ноты FPM для этой версии
  -vr,  version          Показать версию FPM и архитектуру сборки
  -h,   help             Показать это справочное сообщение

ПРИМЕРЫ:
  fpm -su                Полное обновление системы
  fpm -i gcc git         Установка нескольких пакетов
  fpm -p nano            Полное удаление пакета вместе с конфигурацией
  fpm -b ./mypkg/        Сборка пакета из локального рецепта