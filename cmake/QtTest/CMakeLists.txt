# OBS ships Qt without QtTest. Build only the matching upstream testlib module;
# never rebuild/replace OBS's Core, GUI or Network libraries to enable our tests.
cmake_minimum_required(VERSION 3.28)
if(NOT EXISTS "${QT_BASE_SOURCE}/src/testlib/CMakeLists.txt")
    message(FATAL_ERROR "QT_BASE_SOURCE must point to the pinned qtbase source")
endif()
include("${QT_BASE_SOURCE}/.cmake.conf")
project(BokisQtTest VERSION ${QT_REPO_MODULE_VERSION} LANGUAGES C CXX)
find_package(Qt6 ${PROJECT_VERSION} CONFIG REQUIRED COMPONENTS BuildInternals Core Gui)
qt_internal_project_setup()
qt_build_repo_begin()
add_subdirectory("${QT_BASE_SOURCE}/src/testlib" testlib)
qt_build_repo_post_process()
qt_build_repo_end()
