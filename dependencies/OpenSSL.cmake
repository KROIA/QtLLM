## description: OpenSSL 3.x / 4.x — used directly for RSA sign/verify (EVP API).
## No longer piggy-backs on Qt5's bundled 1.1.x runtime.
##
## Search order for the install root (first hit wins):
##   1. -DOPENSSL_ROOT_DIR=... on the CMake command line
##   2. Qt's bundled OpenSSL v3 (installed via the Qt Maintenance Tool)
##   3. The Shining Light "Win64 OpenSSL v3.x" package under Program Files
##   4. System defaults (Linux/macOS)

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    set(LIB_NAME OpenSSL)
    set(LIB_MACRO_NAME OPEN_SSL)

    if(WIN32 AND NOT DEFINED OPENSSL_ROOT_DIR)
        set(_OPENSSL_CANDIDATE_ROOTS
            "C:/Qt/Tools/OpenSSLv3/Win_x64"
            "C:/Program Files/OpenSSL-Win64"
            "C:/Program Files/OpenSSL-Win64-v3"
            "C:/OpenSSL-Win64"
            "C:/OpenSSL"
        )
        foreach(_root ${_OPENSSL_CANDIDATE_ROOTS})
            if(EXISTS "${_root}/include/openssl/opensslv.h")
                set(OPENSSL_ROOT_DIR "${_root}")
                break()
            endif()
        endforeach()
    endif()

    # Require 3.0 or newer. This also accepts any future 4.x release.
    find_package(${LIB_NAME} 3.0 REQUIRED COMPONENTS SSL Crypto)

    message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")
    message(STATUS "  root:        ${OPENSSL_ROOT_DIR}")
    message(STATUS "  include dir: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "  ssl lib:     ${OPENSSL_SSL_LIBRARY}")
    message(STATUS "  crypto lib:  ${OPENSSL_CRYPTO_LIBRARY}")

    # Propagate imported targets + include dir to every build profile.
    list(APPEND DEPS_FOR_SHARED_LIB         OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND DEPS_FOR_STATIC_LIB         OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND DEPS_FOR_STATIC_PROFILE_LIB OpenSSL::SSL OpenSSL::Crypto)

    set(${LIBRARY_MACRO_NAME}  "${${LIBRARY_MACRO_NAME}};${LIB_MACRO_NAME}"        PARENT_SCOPE)
    set(${SHARED_LIB}          "${${SHARED_LIB}};${DEPS_FOR_SHARED_LIB}"           PARENT_SCOPE)
    set(${STATIC_LIB}          "${${STATIC_LIB}};${DEPS_FOR_STATIC_LIB}"           PARENT_SCOPE)
    set(${STATIC_PROFILE_LIB}  "${${STATIC_PROFILE_LIB}};${DEPS_FOR_STATIC_PROFILE_LIB}" PARENT_SCOPE)
    set(${INCLUDE_PATHS}       "${${INCLUDE_PATHS}};${OPENSSL_INCLUDE_DIR}"        PARENT_SCOPE)

    # --- Runtime DLL deployment (Windows only) ------------------------------
    # find_package returns import libraries (.lib) but not the runtime DLLs.
    # Locate them next to the import libs and copy them into every build-config
    # output folder plus the install tree.
    if(WIN32 AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        get_filename_component(_ssl_lib_dir    "${OPENSSL_SSL_LIBRARY}"    DIRECTORY)
        get_filename_component(_crypto_lib_dir "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
        set(_SSL_DLL_HINTS
            "${_ssl_lib_dir}"
            "${_ssl_lib_dir}/../bin"
            "${_crypto_lib_dir}"
            "${_crypto_lib_dir}/../bin")
        if(OPENSSL_ROOT_DIR)
            list(APPEND _SSL_DLL_HINTS "${OPENSSL_ROOT_DIR}/bin")
        endif()
        list(REMOVE_DUPLICATES _SSL_DLL_HINTS)

        # OpenSSL Windows DLLs: libssl-<major>-x64.dll, libcrypto-<major>-x64.dll.
        # Glob for the major-version wildcard so this works for 3.x today and 4.x later.
        set(_FOUND_DLLS "")
        foreach(_hint ${_SSL_DLL_HINTS})
            if(IS_DIRECTORY "${_hint}")
                file(GLOB _matches
                    "${_hint}/libssl-*-x64.dll"
                    "${_hint}/libcrypto-*-x64.dll")
                list(APPEND _FOUND_DLLS ${_matches})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _FOUND_DLLS)

        if(_FOUND_DLLS)
            message(STATUS "OpenSSL runtime DLLs for deployment:")
            foreach(_dll ${_FOUND_DLLS})
                message(STATUS "  ${_dll}")
            endforeach()

            install(FILES ${_FOUND_DLLS}
                    DESTINATION "${RELATIVE_INSTALL_BIN_FOLDER}")

            # Derive a genex-free base for the runtime output directory.
            # CMAKE_RUNTIME_OUTPUT_DIRECTORY contains $<CONFIG> which file(COPY)
            # will not evaluate.
            if(DEFINED RUNTIME_OUTPUT_DIRECTORY AND RUNTIME_OUTPUT_DIRECTORY)
                set(_out_base "${RUNTIME_OUTPUT_DIRECTORY}")
            else()
                set(_out_base "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
                string(REGEX REPLACE "/?\\$<CONFIG(URATION)?>/?$" "" _out_base "${_out_base}")
            endif()
            if(NOT _out_base OR _out_base MATCHES "\\$<")
                set(_out_base "${CMAKE_BINARY_DIR}")
            endif()

            if(CMAKE_CONFIGURATION_TYPES)
                set(_cfgs ${CMAKE_CONFIGURATION_TYPES})
            elseif(CMAKE_BUILD_TYPE)
                set(_cfgs ${CMAKE_BUILD_TYPE})
            else()
                set(_cfgs Debug Release RelWithDebInfo MinSizeRel)
            endif()

            foreach(_cfg ${_cfgs})
                set(_dest "${_out_base}/${_cfg}")
                file(MAKE_DIRECTORY "${_dest}")
                file(COPY ${_FOUND_DLLS} DESTINATION "${_dest}")
            endforeach()

            file(MAKE_DIRECTORY "${_out_base}")
            file(COPY ${_FOUND_DLLS} DESTINATION "${_out_base}")
        else()
            message(WARNING
                "OpenSSL import libraries found but no runtime DLLs "
                "(libssl-*-x64.dll / libcrypto-*-x64.dll) were located next to them. "
                "Executables will fail to start until the DLLs are on PATH or copied "
                "next to the .exe manually.")
        endif()
    endif()
endfunction()

dep(DEPENDENCY_NAME_MACRO
    DEPENDENCIES_FOR_SHARED_LIB
    DEPENDENCIES_FOR_STATIC_LIB
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB
    DEPENDENCIES_INCLUDE_PATHS)
