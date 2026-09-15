cmake_minimum_required(VERSION 3.28...3.31)

project(bokis-twitch-chat-plugin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_FRONTEND_API "Use obs-frontend-api" OFF)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_TESTS "Build unit tests" OFF)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BOKIS_TWITCH_CHAT_VERSION)
string(STRIP "${BOKIS_TWITCH_CHAT_VERSION}" BOKIS_TWITCH_CHAT_VERSION)

find_package(libobs REQUIRED)
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Network WebSockets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebpdemux)

add_library(chat-support STATIC
    src/chat/emote-catalog.cpp
    src/chat/emote-service.cpp
    src/chat/image-cache.cpp
    src/renderer/message-layout.cpp
)
set_target_properties(chat-support PROPERTIES AUTOMOC ON)
target_include_directories(chat-support PUBLIC src)
target_link_libraries(chat-support PUBLIC Qt6::Core Qt6::Gui Qt6::Network PkgConfig::WEBP)
target_compile_definitions(chat-support PUBLIC QT_NO_KEYWORDS)

add_library(post-exit-support STATIC src/updater/post-exit.cpp)
target_include_directories(post-exit-support PUBLIC src)
target_link_libraries(post-exit-support PUBLIC Qt6::Core ${CMAKE_DL_LIBS})
add_executable(bokis-twitch-chat-updater src/updater/helper-main.cpp)
target_link_libraries(bokis-twitch-chat-updater PRIVATE post-exit-support)

add_library(${PROJECT_NAME} MODULE)

target_sources(${PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/renderer/chat-source.cpp
    src/twitch/twitch-client.cpp
    src/updater/update-checker.cpp
    resources/emoji.qrc
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    chat-support
    post-exit-support
    OBS::libobs
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::WebSockets
)

if(ENABLE_FRONTEND_API)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    QT_NO_KEYWORDS
    BOKIS_TWITCH_CHAT_VERSION="${BOKIS_TWITCH_CHAT_VERSION}"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "bokis-twitch-chat-plugin"
    AUTOMOC ON
    AUTORCC ON
)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} bokis-twitch-chat-updater
    LIBRARY DESTINATION "lib/obs-plugins"
    RUNTIME DESTINATION "lib/obs-plugins"
)

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
