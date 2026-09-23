# Fetches the latest published Dusklight release, or the revision selected by the caller.
#
# Inputs:
#   DUSKLIGHT_VERSION       optional git tag or commit SHA to fetch. When omitted, resolve the
#                           latest published non-prerelease tag at configure time.
# Outputs / knobs:
#   DUSKLIGHT_DIR           Dusklight checkout, default <source>/dusklight. Point it at an
#                           existing checkout (e.g. a Dusklight development tree) to skip
#                           fetching entirely.
#   DUSKLIGHT_REPOSITORY    git remote to fetch from
#   DUSKLIGHT_RELEASE_API   GitHub API endpoint for the latest published release

include_guard(GLOBAL)

set(DUSKLIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dusklight"
        CACHE PATH "Path to the Dusklight source tree")
set(DUSKLIGHT_REPOSITORY "https://github.com/TwilitRealm/dusklight.git"
        CACHE STRING "Dusklight git repository to fetch from")
set(DUSKLIGHT_RELEASE_API "https://api.github.com/repos/TwilitRealm/dusklight/releases/latest"
        CACHE STRING "API endpoint for the latest published Dusklight release")

function(_exec_git)
    list(JOIN ARGN " " _args_text)
    execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
            WORKING_DIRECTORY "${DUSKLIGHT_DIR}"
            RESULT_VARIABLE _result)
    if (NOT _result EQUAL 0)
        message(FATAL_ERROR "'git ${_args_text}' failed.\n"
                "Check that DUSKLIGHT_VERSION (${DUSKLIGHT_VERSION}) is a valid tag or commit "
                "in ${DUSKLIGHT_REPOSITORY} and that you are online.")
    endif ()
endfunction()

# A stamp file marks a checkout this module manages (and which version it holds). A checkout
# without one belongs to the user and is left untouched.
set(_dusklight_stamp "${DUSKLIGHT_DIR}/.stamp")

if (EXISTS "${DUSKLIGHT_DIR}/sdk/CMakeLists.txt" AND NOT EXISTS "${_dusklight_stamp}")
    message(STATUS "Dusklight: using existing checkout at ${DUSKLIGHT_DIR}")
else ()
    if (NOT DUSKLIGHT_VERSION)
        file(DOWNLOAD "${DUSKLIGHT_RELEASE_API}"
                "${CMAKE_BINARY_DIR}/dusklight-latest-release.json"
                STATUS _dusklight_release_status
                TLS_VERIFY ON
                HTTPHEADER "Accept: application/vnd.github+json"
                HTTPHEADER "User-Agent: dusklight-online-mod")
        list(GET _dusklight_release_status 0 _dusklight_release_result)
        if (NOT _dusklight_release_result EQUAL 0)
            message(FATAL_ERROR
                    "Dusklight: could not resolve the latest published release from ${DUSKLIGHT_RELEASE_API}: ${_dusklight_release_status}")
        endif ()
        file(READ "${CMAKE_BINARY_DIR}/dusklight-latest-release.json" _dusklight_release_json)
        string(JSON _dusklight_release_tag ERROR_VARIABLE _dusklight_release_error
                GET "${_dusklight_release_json}" tag_name)
        if (_dusklight_release_error OR
            NOT _dusklight_release_tag MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
            message(FATAL_ERROR
                    "Dusklight: latest published release did not contain a valid tag")
        endif ()
        set(DUSKLIGHT_VERSION "${_dusklight_release_tag}")
        message(STATUS "Dusklight: latest published release is ${DUSKLIGHT_VERSION}")
    endif ()

    set(_dusklight_fetched "")
    if (EXISTS "${_dusklight_stamp}")
        file(READ "${_dusklight_stamp}" _dusklight_fetched)
        string(STRIP "${_dusklight_fetched}" _dusklight_fetched)
    endif ()

    if (NOT _dusklight_fetched STREQUAL DUSKLIGHT_VERSION)
        find_package(Git QUIET REQUIRED)
        message(STATUS "Dusklight: fetching ${DUSKLIGHT_VERSION} into ${DUSKLIGHT_DIR}")
        file(MAKE_DIRECTORY "${DUSKLIGHT_DIR}")
        if (NOT EXISTS "${DUSKLIGHT_DIR}/.git")
            _exec_git(init --quiet)
            _exec_git(remote add origin "${DUSKLIGHT_REPOSITORY}")
        endif ()
        # FetchContent's GIT_SHALLOW falls back to a full clone for SHAs, so we fetch
        # manually instead.
        _exec_git(fetch --depth 1 "${DUSKLIGHT_REPOSITORY}" "${DUSKLIGHT_VERSION}")
        _exec_git(-c advice.detachedHead=false checkout --force FETCH_HEAD)
        if (EXISTS "${DUSKLIGHT_DIR}/.gitmodules")
            _exec_git(submodule update --init --depth 1 extern/aurora)
        endif ()
        file(WRITE "${_dusklight_stamp}" "${DUSKLIGHT_VERSION}\n")
    endif ()

    # Keep the stamp out of `git status` in the managed checkout.
    set(_dusklight_exclude "${DUSKLIGHT_DIR}/.git/info/exclude")
    set(_dusklight_exclude_text "")
    if (EXISTS "${_dusklight_exclude}")
        file(READ "${_dusklight_exclude}" _dusklight_exclude_text)
    endif ()
    if (NOT _dusklight_exclude_text MATCHES "(^|\n)/\\.stamp(\n|$)")
        file(APPEND "${_dusklight_exclude}" "/.stamp\n")
    endif ()

    # Shallow checkouts carry no tags for `git describe`; pin the SDK's version string instead.
    set(DUSK_VERSION_OVERRIDE "${DUSKLIGHT_VERSION}")
endif ()
