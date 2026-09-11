[ X _ X ] FPM - v.0.1.0-Alpha x86_64 | NON HUMAN LINUX

Hello and Welcome to the FPM Package Manager Repository!

Features of FPM:
C++ 17 
CMake
Convenient syntax
Flexible Command System
And others

(Recommended) Build via CMake:

1 cd ~/fpm

2 cmake -B build

3 cmake --build build

Fast build via CMake:

mkdir -p build
g++ -std=c++17 -Iinclude src/main.cpp src/package.cpp src/logger.cpp src/database.cpp -o build/fpm

Installation into the system:

sudo cmake --install build

main commands:

USAGE:
  fpm [OPTION] [PACKAGE...]

CORE OPERATIONS:
  -i,   install <pkg>    Install specified package(s)
  -r,   remove <pkg>     Remove package(s), keeping configuration files
  -p,   purge <pkg>      Completely remove package(s) and configurations
  -s,   search <query>   Search for package in remote repositories
  -info info <pkg>       Display detailed package information
  -l,   list             List all currently installed packages

SYNCHRONIZATION & MAINTENANCE:
  -upd, update           Update local repository database
  -upg, upgrade          Upgrade all installed packages to latest version
  -su,  sysup            Execute full system update (update + upgrade)
  -ar,  autoremove       Remove orphaned dependencies
  -c,   clean            Clear cached download archives
  -verify <pkg>          Verify integrity of installed package files

BUILD & DEVELOPMENT:
  -b,   build <dir>      Build FPM package from recipe directory
  -pkg  <file.fpm>       Install local .fpm package file

SYSTEM & DEBUG:
  -v,   version          Display FPM version and build architecture
  -h,   help             Display this help message

EXAMPLES:
  fpm -su                Full system update
  fpm -i gcc git         Install multiple packages
  fpm -p nano            Purge package with configuration files
  fpm -b ./mypkg/        Build local package recipe
  
  
  -------------------------------------------------------------------------------
  
Привет и добро пожаловать в репозиторий пакетного менеджера FPM!

Особенности FPM:
C++ 17 
CMake
Удобный синтаксис
Гибкая система команд
И многое другое

(Рекомендуется) Сборка через CMake:

1 cd ~/fpm

2 cmake -B build

3 cmake --build build

Быстрая сборка через CMake:

mkdir -p build
g++ -std=c++17 -Iinclude src/main.cpp src/package.cpp src/logger.cpp src/database.cpp -o build/fpm

Установка в систему:

sudo cmake --install build

Основные команды:

ИСПОЛЬЗОВАНИЕ:
  fpm [ОПЦИЯ] [ПАКЕТ...]

ОСНОВНЫЕ ОПЕРАЦИИ:
  -i,   install <pkg>    Установить указанный пакет(ы)
  -r,   remove <pkg>     Удалить пакет(ы), сохранив конфигурационные файлы
  -p,   purge <pkg>      Полностью удалить пакет(ы) и их конфигурации
  -s,   search <query>   Поиск пакета в удаленных репозиториях
  -info info <pkg>       Показать подробную информацию о пакете
  -l,   list             Вывести список всех установленных пакетов

СИНХРОНИЗАЦИЯ И ОБСЛУЖИВАНИЕ:
  -upd, update           Обновить локальную базу данных репозиториев
  -upg, upgrade          Обновить все установленные пакеты до последней версии
  -su,  sysup            Выполнить полное обновление системы (update + upgrade)
  -ar,  autoremove       Удалить неиспользуемые (Осиротевшие) зависимости
  -c,   clean            Очистить кэш загруженных архивов
  -verify <pkg>          Проверить целостность файлов установленного пакета

СБОРКА И РАЗРАБОТКА:
  -b,   build <dir>      Собрать пакет FPM из директории рецепта
  -pkg  <file.fpm>       Установить локальный файл пакета .fpm

СИСТЕМА И ОТЛАДКА:
  -v,   version          Показать версию FPM и архитектуру сборки
  -h,   help             Показать это справочное сообщение

ПРИМЕРЫ:
  fpm -su                Полное обновление системы
  fpm -i gcc git         Установка нескольких пакетов
  fpm -p nano            Полное удаление пакета вместе с конфигурацией
  fpm -b ./mypkg/        Сборка пакета из локального рецепта
