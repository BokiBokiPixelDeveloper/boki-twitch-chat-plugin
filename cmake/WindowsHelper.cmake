# Embed the complete worker runtime in the updater EXE. No Qt DLL is required
# beside the installed helper or on PATH when the temporary runner starts.
enable_language(RC)
add_executable(bokis-updater-worker src/updater/helper-windows-main.cpp)
target_link_libraries(bokis-updater-worker PRIVATE post-exit-support)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
set(helper_runtime "$<TARGET_FILE:Qt6::Core>" ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS})
set(helper_rc "#pragma code_page(65001)\n101 RCDATA \"$<TARGET_FILE:bokis-updater-worker>\"\n")
set(helper_header "#pragma once\nstruct HelperResource { int id; const wchar_t *name; };\nstatic constexpr HelperResource helperResources[] = {{101, L\"bokis-updater-worker.exe\"},\n")
set(resource_id 102)
foreach(runtime IN LISTS helper_runtime)
    if(runtime MATCHES "^\\$<")
        set(runtime_name "$<TARGET_FILE_NAME:Qt6::Core>")
    else()
        get_filename_component(runtime_name "${runtime}" NAME)
    endif()
    string(APPEND helper_rc "${resource_id} RCDATA \"${runtime}\"\n")
    string(APPEND helper_header "{${resource_id}, L\"${runtime_name}\"},\n")
    math(EXPR resource_id "${resource_id} + 1")
endforeach()
string(APPEND helper_header "};\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc.in" CONTENT "${helper_rc}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.hpp" CONTENT "${helper_header}")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc"
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc"
    DEPENDS bokis-updater-worker "$<TARGET_FILE:bokis-updater-worker>" ${helper_runtime}
        "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc.in"
    VERBATIM)
add_executable(bokis-twitch-chat-updater
    src/updater/helper-bootstrap-windows.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>/helper-resources.rc")
target_include_directories(bokis-twitch-chat-updater PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/helper-$<CONFIG>")
set_property(TARGET bokis-twitch-chat-updater PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_dependencies(bokis-twitch-chat-updater bokis-updater-worker)
