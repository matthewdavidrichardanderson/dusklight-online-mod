cmake_minimum_required(VERSION 3.26)
project(dusklight_online LANGUAGES CXX)

# Pin the SDK/ABI used for release builds. Compatibility with other builds is
# negotiated at runtime through versioned services and the GameService epoch.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "008a18c1428c1827795867a6645898a035b64a7b")
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")
add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)

# The native world-label renderer uses a private ImGui font atlas. No host
# ImGui internals are imported.
FetchContent_Declare(
    online_imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    URL_HASH SHA256=466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
FetchContent_GetProperties(online_imgui)
if (NOT online_imgui_POPULATED)
    FetchContent_Populate(online_imgui)
endif ()
add_library(dusklight_online_imgui STATIC
    "${online_imgui_SOURCE_DIR}/imgui.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${online_imgui_SOURCE_DIR}/imgui_widgets.cpp"
)
target_include_directories(dusklight_online_imgui PUBLIC "${online_imgui_SOURCE_DIR}")
target_compile_features(dusklight_online_imgui PUBLIC cxx_std_20)
set_target_properties(dusklight_online_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Remote Link's actor files and font resources are included with this project
# so a clean checkout has every input needed to configure the build. The rest
# of the mod compiles against the SDK selected above.
set(DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/dusklight_remote_link"
)

# Fail closed on every imported file so an explicitly supplied checkout cannot
# silently change Remote Link behavior or packaged resources.
function(dusklight_remote_link_require_sha256 relative_path expected_sha256)
    set(source_path "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/${relative_path}")
    if (NOT EXISTS "${source_path}")
        message(FATAL_ERROR "Pinned Remote Link source file is missing: ${relative_path}")
    endif ()
    file(SHA256 "${source_path}" actual_sha256)
    string(TOLOWER "${expected_sha256}" expected_sha256_lower)
    if (NOT actual_sha256 STREQUAL expected_sha256_lower)
        message(FATAL_ERROR
            "Bundled Remote Link source file has an unexpected hash: "
            "${relative_path} (got ${actual_sha256})")
    endif ()
endfunction()

dusklight_remote_link_require_sha256(
    "src/d/actor/d_a_remote_link.cpp"
    "8CEE4667C4D77F1E92C9791DA8DCC0E7A3BC0E8BDB043693AA6CEE76697F306A")
dusklight_remote_link_require_sha256(
    "include/d/actor/d_a_remote_link.h"
    "E15643ACF03D0A6578A0703184C7FA0C5D9C5AAB7A86F9065BEBEE647BA78506")
dusklight_remote_link_require_sha256(
    "src/dusk/multiplayer/remote_link_dummy.cpp"
    "F4C6F011D655509812B05EEE5AA2E2DB9E00276E7F740F36E31DEF30C9D69742")
dusklight_remote_link_require_sha256(
    "include/dusk/multiplayer/remote_link_dummy.hpp"
    "6A21876BAC92EF0C0386AE34389A7E7D0D6A7AD69BBD00661E7CF0148247C6A4")
dusklight_remote_link_require_sha256(
    "res/AlegreyaSC-Bold.ttf"
    "F0EDAF1F289B7EB1B94BA9290146D57E5B413364BCF723CD0196779FAA557AFA")
dusklight_remote_link_require_sha256(
    "res/Inter-Regular.ttf"
    "712FB3AE7568CC3812776EDA49922FF8BCA5521CB02C006DEC80040E9260C262")

set(DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/src")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/d/actor")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}")
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/include/d/actor/d_a_remote_link.h"
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/d/actor/d_a_remote_link.h"
    COPYONLY
)
set(DUSKLIGHT_ONLINE_GENERATED_RES_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/res")
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}")
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/res/AlegreyaSC-Bold.ttf"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/AlegreyaSC-Bold.ttf"
    COPYONLY
)
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/res/Inter-Regular.ttf"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/Inter-Regular.ttf"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/FONT_LICENSES.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/FONT_LICENSES.txt"
    COPYONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/THIRD_PARTY_LICENSES.txt"
    "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}/THIRD_PARTY_LICENSES.txt"
    COPYONLY
)
# The selected SDK does not expose Z2AudioMgr::seStartNoCull. Preserve the
# actor source apart from this asserted call-site adaptation.
file(READ
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/src/d/actor/d_a_remote_link.cpp"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
)
set(DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL "audioMgr->seStartNoCull(")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL}" DUSKLIGHT_ONLINE_REMOTE_NO_CULL_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_NO_CULL_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link no-cull call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_NO_CULL_CALL}"
    "dusklight_online::game::remote_audio_start_no_cull(audioMgr, "
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
# JASGlobalInstance<T> inline accessors bind to the consuming DLL's template
# static on Windows. Route Remote Link audio metadata/audience lookups through
# the exported Z2AudioMgr pointer instead of permitting null mod-local copies.
set(DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL
    "Z2GetSoundInfo()->getAudibleSwFull(i_soundId)")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL}"
    DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link sound-info call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_SOUND_INFO_CALL}"
    "dusklight_online::game::remote_audio_audible_params(i_soundId)"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
set(DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL "Z2GetAudience()")
string(FIND "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
    "${DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL}"
    DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_OFFSET)
if (DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned Remote Link audience call site changed; refusing a stale adaptation")
endif ()
string(REPLACE
    "${DUSKLIGHT_ONLINE_REMOTE_AUDIENCE_CALL}"
    "dusklight_online::game::remote_audio_audience()"
    DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
file(WRITE
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}/d_a_remote_link_impl.inc"
    "${DUSKLIGHT_ONLINE_REMOTE_LINK_SOURCE}"
)
file(MAKE_DIRECTORY "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/dusk/multiplayer")
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/include/dusk/multiplayer/remote_link_dummy.hpp"
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}/dusk/multiplayer/remote_link_dummy.hpp"
    COPYONLY
)
configure_file(
    "${DUSKLIGHT_REMOTE_LINK_SOURCE_ROOT}/src/dusk/multiplayer/remote_link_dummy.cpp"
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}/remote_link_dummy_impl.inc"
    COPYONLY
)

set(ONLINE_INVITE_SOURCES
    src/net/invite_code.cpp
)

set(ONLINE_CORE_SOURCES
    ${ONLINE_INVITE_SOURCES}
    src/net/transport.cpp
    src/net/udp_codec.cpp
)

set(ONLINE_GAME_SOURCES
    src/game/abi_compat.cpp
    src/game/audio_bridge.cpp
    src/game/bomb_bridge.cpp
    src/game/collectible_visual_bridge.cpp
    src/game/game_adapter.cpp
    src/game/local_pose.cpp
    src/game/protocol_router.cpp
    src/game/remote_pose.cpp
    src/game/remote_actor_bridge.cpp
    src/game/visual_bridge.cpp
    src/game/remote_link.cpp
    src/game/remote_link_dummy.cpp
)

add_mod(dusklight_online
    FEATURES game
    SOURCES
        src/mod.cpp
        src/online_app.cpp
        ${ONLINE_CORE_SOURCES}
        ${ONLINE_GAME_SOURCES}
    MOD_JSON mod.json
    RES_DIR "${DUSKLIGHT_ONLINE_GENERATED_RES_DIR}"
)

target_include_directories(dusklight_online PRIVATE
    include
    src
    "${DUSKLIGHT_ONLINE_GENERATED_INCLUDE_DIR}"
    "${DUSKLIGHT_ONLINE_GENERATED_SOURCE_DIR}"
    "${DUSKLIGHT_DIR}/src"
)
# Remote Link is supplied by this mod rather than mainline's profile table.
# Keep it on a private s16 ID and substitute the profile only for process IDs
# created by this mod. This avoids colliding with GAMEOVER or a future profile
# assigned a private value outside the host's process-name range.
target_compile_definitions(dusklight_online PRIVATE fpcNm_REMOTE_LINK_e=0x7FFE)
target_link_libraries(dusklight_online PRIVATE
    nlohmann_json::nlohmann_json
    libzstd_static
    dusklight_online_imgui
    ${CMAKE_DL_LIBS}
)
if (WIN32)
    target_link_libraries(dusklight_online PRIVATE ws2_32)
    # MSVC otherwise records the absolute build-machine PDB path in mod.dll's
    # PE debug directory. Keep distributed .dusk packages free of usernames
    # and local workspace paths while retaining a useful PDB basename.
    target_link_options(dusklight_online PRIVATE "/PDBALTPATH:dusklight_online.pdb")
endif ()

# The relay is intentionally a standalone/headless operator process. It uses
# the same invite-code source as the mod so endpoint codes cannot drift.
option(DUSKLIGHT_ONLINE_BUILD_RELAY "Build the desktop relay operator tools" ON)
if (DUSKLIGHT_ONLINE_BUILD_RELAY AND NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(dusklight_online_relay
        tools/multiplayer_relay/relay.cpp
        ${ONLINE_INVITE_SOURCES}
    )
    target_compile_features(dusklight_online_relay PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_relay PRIVATE include src)
    target_link_libraries(dusklight_online_relay PRIVATE nlohmann_json::nlohmann_json)

    if (WIN32)
        target_link_libraries(dusklight_online_relay PRIVATE ws2_32)
        add_executable(dusklight_online_relay_launcher WIN32
            tools/multiplayer_relay/launcher.cpp
            ${ONLINE_INVITE_SOURCES}
        )
        target_compile_features(dusklight_online_relay_launcher PRIVATE cxx_std_20)
        target_include_directories(dusklight_online_relay_launcher PRIVATE include src)
        target_link_libraries(dusklight_online_relay_launcher PRIVATE
            nlohmann_json::nlohmann_json
            ws2_32
        )
    endif ()
endif ()

option(DUSKLIGHT_ONLINE_BUILD_TESTS "Build standalone online core tests" ON)
if (DUSKLIGHT_ONLINE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(dusklight_online_invite_code_test
        tests/invite_code_test.cpp
        ${ONLINE_INVITE_SOURCES}
    )
    target_compile_features(dusklight_online_invite_code_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_invite_code_test PRIVATE include src)
    target_link_libraries(dusklight_online_invite_code_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME dusklight_online_invite_code COMMAND dusklight_online_invite_code_test)

    add_executable(dusklight_online_transport_test
        tests/transport_test.cpp
        ${ONLINE_CORE_SOURCES}
    )
    target_compile_features(dusklight_online_transport_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_transport_test PRIVATE include src)
    target_link_libraries(dusklight_online_transport_test PRIVATE
        nlohmann_json::nlohmann_json
        libzstd_static
    )
    if (WIN32)
        target_link_libraries(dusklight_online_transport_test PRIVATE ws2_32)
    endif ()
    add_test(NAME dusklight_online_transport COMMAND dusklight_online_transport_test)

    add_executable(dusklight_online_udp_codec_test
        tests/udp_codec_test.cpp
        src/net/udp_codec.cpp
    )
    target_compile_features(dusklight_online_udp_codec_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_udp_codec_test PRIVATE include src)
    target_link_libraries(dusklight_online_udp_codec_test PRIVATE
        nlohmann_json::nlohmann_json
        libzstd_static
    )
    add_test(NAME dusklight_online_udp_codec COMMAND dusklight_online_udp_codec_test)

    add_executable(dusklight_online_protocol_router_test
        tests/protocol_router_test.cpp
        src/game/protocol_router.cpp
    )
    target_compile_features(dusklight_online_protocol_router_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_protocol_router_test PRIVATE include src)
    target_link_libraries(dusklight_online_protocol_router_test PRIVATE
        nlohmann_json::nlohmann_json
    )
    add_test(NAME dusklight_online_protocol_router COMMAND dusklight_online_protocol_router_test)

    add_executable(dusklight_online_relay_transport_client_test
        tests/relay_transport_client_test.cpp
        ${ONLINE_CORE_SOURCES}
    )
    target_compile_features(dusklight_online_relay_transport_client_test PRIVATE cxx_std_20)
    target_include_directories(dusklight_online_relay_transport_client_test PRIVATE include src)
    target_link_libraries(dusklight_online_relay_transport_client_test PRIVATE
        nlohmann_json::nlohmann_json
        libzstd_static
    )
    if (WIN32)
        target_link_libraries(dusklight_online_relay_transport_client_test PRIVATE ws2_32)
    endif ()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if (Python3_Interpreter_FOUND AND TARGET dusklight_online_relay)
        add_test(
            NAME dusklight_online_relay_integration
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/multiplayer_relay/test_relay.py"
                --relay $<TARGET_FILE:dusklight_online_relay>
        )
        set_tests_properties(dusklight_online_relay_integration PROPERTIES TIMEOUT 30)
        add_test(
            NAME dusklight_online_relay_transport
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_relay_transport_test.py"
                $<TARGET_FILE:dusklight_online_relay>
                $<TARGET_FILE:dusklight_online_relay_transport_client_test>
        )
        set_tests_properties(dusklight_online_relay_transport PROPERTIES TIMEOUT 20)
    endif ()
endif ()
