# Upstream 83f50329 adds these virtual methods, but the published Windows
# SDK import stub predates them. Supplement it with ordinary imports of
# upstream's implementations; no game code or fork library is included.
set(_line_import_def "${CMAKE_CURRENT_LIST_DIR}/line_interpolation.def")
set(_line_import_lib "${CMAKE_CURRENT_BINARY_DIR}/line_interpolation_imports.lib")
if (CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(_line_import_machine arm64)
else ()
    set(_line_import_machine x64)
endif ()
add_custom_command(
    OUTPUT "${_line_import_lib}"
    COMMAND "${CMAKE_AR}" /nologo "/def:${_line_import_def}"
        "/machine:${_line_import_machine}" /name:dusklight.exe
        "/out:${_line_import_lib}"
    DEPENDS "${_line_import_def}"
    VERBATIM
)
add_custom_target(dusklight_online_line_imports DEPENDS "${_line_import_lib}")
add_dependencies(dusklight_online dusklight_online_line_imports)
target_link_libraries(dusklight_online PRIVATE "${_line_import_lib}")
