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
set(CHAT_EMOJI_RESOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/emoji.qrc")
if(WIN32)
    # Use Qt's recommended Windows-compatible TrueType wrapper around the same
    # Noto bitmap glyphs. Keep the existing FreeType resource on Linux.
    set(CHAT_EMOJI_RESOURCE "${CMAKE_CURRENT_SOURCE_DIR}/resources/emoji-windows.qrc")
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|arm" OR
       (CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64"))
        message(FATAL_ERROR "Windows builds require x86_64 MSVC")
    endif()
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
    find_package(WebP CONFIG REQUIRED)
    set(CHAT_WEBP_TARGET WebP::webpdemux)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebpdemux)
    set(CHAT_WEBP_TARGET PkgConfig::WEBP)
endif()

add_library(chat-support STATIC
    src/chat/emote-catalog.cpp
    src/chat/emote-service.cpp
    src/chat/image-cache.cpp
    src/renderer/message-layout.cpp
)
set_target_properties(chat-support PROPERTIES AUTOMOC ON)
target_include_directories(chat-support PUBLIC src)
target_link_libraries(chat-support PUBLIC Qt6::Core Qt6::Gui Qt6::Network ${CHAT_WEBP_TARGET})
target_compile_definitions(chat-support PUBLIC QT_NO_KEYWORDS)

if(WIN32)
    set(UPDATER_PLATFORM src/updater/post-exit-windows.cpp)
else()
    set(UPDATER_PLATFORM src/updater/post-exit.cpp)
endif()
add_library(post-exit-support STATIC src/updater/post-exit-shared.cpp ${UPDATER_PLATFORM})
target_include_directories(post-exit-support PUBLIC src)
target_link_libraries(post-exit-support PUBLIC Qt6::Core ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(post-exit-support PUBLIC shell32 ole32 psapi)
    include(cmake/WindowsHelper.cmake)
else()
    add_executable(bokis-twitch-chat-updater src/updater/helper-main.cpp)
    target_link_libraries(bokis-twitch-chat-updater PRIVATE post-exit-support)
endif()

add_library(${PROJECT_NAME} MODULE)

target_sources(${PROJECT_NAME} PRIVATE
    src/plugin-main.cpp
    src/renderer/chat-source.cpp
    src/twitch/twitch-client.cpp
    src/updater/update-checker.cpp
    ${CHAT_EMOJI_RESOURCE}
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
if(WIN32)
    install(TARGETS ${PROJECT_NAME} bokis-twitch-chat-updater
        RUNTIME DESTINATION "bin/64bit" LIBRARY DESTINATION "bin/64bit")
    install(FILES $<TARGET_FILE:Qt6::WebSockets> DESTINATION "bin/64bit")
    install(FILES "${CMAKE_SOURCE_DIR}/resources/fonts/OFL.txt" DESTINATION "data/licenses"
        RENAME "NotoColorEmoji-OFL.txt")
else()
    install(TARGETS ${PROJECT_NAME} bokis-twitch-chat-updater
        LIBRARY DESTINATION "lib/obs-plugins" RUNTIME DESTINATION "lib/obs-plugins")
endif()

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
