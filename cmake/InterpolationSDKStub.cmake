# The official downloadable Android/Apple SDK stubs predate d34226ad's
# out-of-line animation methods and J3DFrameCtrl vtable. Regenerate a link-only
# stub from all existing exports plus those upstream symbols. No stub is bundled
# with the mod, and undefined-symbol checks stay enabled.
if (NOT PROJECT_IS_TOP_LEVEL OR DUSK_GAME_EXE OR NOT (APPLE OR ANDROID))
    return()
endif ()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${DUSKLIGHT_DIR}/cmake/SymbolManifest.cmake")
ensure_symgen(TRUE)
_mod_download_link_stub(_original_interp_stub)
set(_interp_symbols "${CMAKE_CURRENT_LIST_DIR}/interpolation_symbols.txt")
set(_interp_script "${CMAKE_CURRENT_LIST_DIR}/refresh_interpolation_stub.py")
set(_interp_exports "${CMAKE_CURRENT_BINARY_DIR}/interpolation_sdk_exports.exp")
get_filename_component(_interp_stub_name "${_original_interp_stub}" NAME)
set(_interp_stub "${CMAKE_CURRENT_BINARY_DIR}/interpolation-${_interp_stub_name}")

if (APPLE)
    set(_interp_format macho)
    if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(_interp_platform ios)
    else ()
        set(_interp_platform macos)
    endif ()
    set(_interp_args --platform "${_interp_platform}")
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND _interp_args --min-os "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif ()
    if (CMAKE_OSX_ARCHITECTURES)
        set(_interp_archs ${CMAKE_OSX_ARCHITECTURES})
    else ()
        set(_interp_archs "${CMAKE_SYSTEM_PROCESSOR}")
    endif ()
    foreach (_arch IN LISTS _interp_archs)
        list(APPEND _interp_args --arch "${_arch}")
    endforeach ()
    get_target_property(_interp_link_options dusklight_online LINK_OPTIONS)
    list(FIND _interp_link_options "${_original_interp_stub}" _interp_loader_index)
    if (_interp_loader_index EQUAL -1)
        message(FATAL_ERROR "Cannot find the SDK bundle loader to update")
    endif ()
    list(REMOVE_AT _interp_link_options ${_interp_loader_index})
    list(INSERT _interp_link_options ${_interp_loader_index} "${_interp_stub}")
    set_property(TARGET dusklight_online PROPERTY LINK_OPTIONS "${_interp_link_options}")
else ()
    set(_interp_format elf)
    set(_interp_args --arch "${CMAKE_SYSTEM_PROCESSOR}" --soname libmain.so)
    get_target_property(_interp_libraries dusklight_online LINK_LIBRARIES)
    list(FIND _interp_libraries "${_original_interp_stub}" _interp_library_index)
    if (_interp_library_index EQUAL -1)
        message(FATAL_ERROR "Cannot find the SDK library to update")
    endif ()
    list(REMOVE_AT _interp_libraries ${_interp_library_index})
    list(INSERT _interp_libraries ${_interp_library_index} "${_interp_stub}")
    set_property(TARGET dusklight_online PROPERTY LINK_LIBRARIES "${_interp_libraries}")
endif ()

add_custom_command(
    OUTPUT "${_interp_stub}"
    COMMAND "${Python3_EXECUTABLE}" "${_interp_script}"
        --input "${_original_interp_stub}" --symbols "${_interp_symbols}"
        --output "${_interp_exports}"
    COMMAND "${SYMGEN_EXE}" stub -f "${_interp_format}" "${_interp_exports}"
        -o "${_interp_stub}" ${_interp_args}
    COMMAND "${Python3_EXECUTABLE}" "${_interp_script}"
        --input "${_original_interp_stub}" --symbols "${_interp_symbols}"
        --output "${_interp_exports}" --verify "${_interp_stub}"
    DEPENDS "${_original_interp_stub}" "${_interp_symbols}" "${_interp_script}" symgen
    BYPRODUCTS "${_interp_exports}"
    VERBATIM
)
add_custom_target(dusklight_online_interpolation_stub DEPENDS "${_interp_stub}")
add_dependencies(dusklight_online dusklight_online_interpolation_stub)
set_property(TARGET dusklight_online APPEND PROPERTY LINK_DEPENDS "${_interp_stub}")
