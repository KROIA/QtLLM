## description: The OpenSSL runtime Qt needs for https (Claude, OpenAI-compatible servers): copies its two DLLs next to the programs. Nothing is linked.
##
## QtLLM does not call OpenSSL itself. Qt's network module loads it at run
## time, and without it every https request fails ("TLS initialization
## failed"). Which OpenSSL that is depends on the Qt version:
##
##   Qt 5      OpenSSL 1.1.1    libssl-1_1-x64.dll, libcrypto-1_1-x64.dll
##   Qt 6      OpenSSL 3.x      libssl-3-x64.dll,   libcrypto-3-x64.dll
##
## The pair is copied into the build output folder and installed into bin/ of
## the project that is being built - so also of an application that has QtLLM
## as a dependency. Without that an application works on the developer's PC,
## where some program put OpenSSL on PATH, and nowhere else.
##
## Where the DLLs are looked for, first hit wins:
##   1. -DQTLLM_OPENSSL_DIR=<folder that holds the two DLLs>
##   2. Qt's OpenSSL toolkit (Qt Maintenance Tool > Developer and Designer
##      Tools): <Qt>/Tools/OpenSSL for 1.1.1, <Qt>/Tools/OpenSSLv3 for 3.x
##   3. The Shining Light "Win64 OpenSSL" packages under Program Files
##   4. Git for Windows (C:/Program Files/Git/mingw64/bin)
##   5. PATH
##
## -DQTLLM_DEPLOY_OPENSSL=OFF switches the copying off, for an application
## that ships OpenSSL its own way.
##
## Other platforms: nothing is done, Qt uses the OpenSSL of the system.

option(QTLLM_DEPLOY_OPENSSL "Copy the OpenSSL DLLs Qt needs for https next to the programs" ON)
set(QTLLM_OPENSSL_DIR "" CACHE PATH "Folder with the OpenSSL DLLs for Qt's https; empty: they are looked for")

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    if(NOT WIN32 OR NOT QTLLM_DEPLOY_OPENSSL)
        return()
    endif()

    # ponytail: Qt 6.0 to 6.4 still loaded OpenSSL 1.1.1. For such a Qt the
    # 3.x pair is copied and https stays off; choose by Qt minor version
    # should anybody build with one.
    if(QT_MAJOR_VERSION EQUAL 5)
        set(_series "1_1")
        set(_toolkit "OpenSSL")
    else()
        set(_series "3")
        set(_toolkit "OpenSSLv3")
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch "-x64")
        set(_toolkit_arch "Win_x64")
    else()
        set(_arch "")
        set(_toolkit_arch "Win_x86")
    endif()
    set(_ssl    "libssl-${_series}${_arch}.dll")
    set(_crypto "libcrypto-${_series}${_arch}.dll")

    set(_dirs "")
    if(QTLLM_OPENSSL_DIR)
        list(APPEND _dirs "${QTLLM_OPENSSL_DIR}" "${QTLLM_OPENSSL_DIR}/bin")
    endif()
    if(QT_INSTALL_BASE)
        list(APPEND _dirs "${QT_INSTALL_BASE}/Tools/${_toolkit}/${_toolkit_arch}/bin")
    endif()
    list(APPEND _dirs
        "C:/Qt/Tools/${_toolkit}/${_toolkit_arch}/bin"
        "C:/Program Files/OpenSSL-Win64/bin"
        "C:/Program Files/OpenSSL-Win64"
        "C:/Program Files/OpenSSL-Win64-v3/bin"
        "C:/Program Files (x86)/OpenSSL-Win32/bin"
        "C:/Program Files (x86)/OpenSSL-Win32"
        "C:/OpenSSL-Win64/bin"
        "C:/OpenSSL-Win64"
        "C:/OpenSSL/bin"
        # Git for Windows brings a self-contained OpenSSL (1.1.1 in older
        # versions, 3.x in newer ones), and is on about every developer's PC.
        "C:/Program Files/Git/mingw64/bin")
    list(LENGTH _dirs _known_dirs)
    file(TO_CMAKE_PATH "$ENV{PATH}" _path_dirs)
    list(APPEND _dirs ${_path_dirs})

    set(_found "")
    set(_index 0)
    foreach(_dir ${_dirs})
        if(EXISTS "${_dir}/${_ssl}" AND EXISTS "${_dir}/${_crypto}")
            set(_found "${_dir}")
            break()
        endif()
        math(EXPR _index "${_index} + 1")
    endforeach()

    if(NOT _found)
        message(WARNING
            "QtLLM: ${_ssl} and ${_crypto} were not found, so they are not copied next to "
            "the programs. Qt ${QT_MAJOR_VERSION} needs them for https: without them requests "
            "to Claude and to other https servers fail on a PC that does not have them on "
            "PATH. Install the OpenSSL toolkit with the Qt Maintenance Tool, or pass "
            "-DQTLLM_OPENSSL_DIR=<folder that holds the two DLLs>.")
        return()
    endif()

    if(_index LESS _known_dirs)
        message(STATUS "QtLLM: OpenSSL for Qt ${QT_MAJOR_VERSION} https from ${_found}")
    else()
        message(STATUS "QtLLM: OpenSSL for Qt ${QT_MAJOR_VERSION} https from ${_found} (found on PATH; "
                       "set QTLLM_OPENSSL_DIR to choose another copy)")
    endif()
    set(_dlls "${_found}/${_ssl}" "${_found}/${_crypto}")

    # Into the install tree of the project being built. As a dependency that
    # is the application's, the install prefix being one for the whole build.
    install(FILES ${_dlls} DESTINATION "${RELATIVE_INSTALL_BIN_FOLDER}")

    # And next to the programs in the build tree. CMAKE_RUNTIME_OUTPUT_DIRECTORY
    # ends in $<CONFIG>, which file(COPY) does not evaluate, hence the base
    # folder the template keeps in RUNTIME_OUTPUT_DIRECTORY.
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
        set(_configs ${CMAKE_CONFIGURATION_TYPES})
    elseif(CMAKE_BUILD_TYPE)
        set(_configs ${CMAKE_BUILD_TYPE})
    else()
        set(_configs Debug Release RelWithDebInfo MinSizeRel)
    endif()
    foreach(_config ${_configs})
        file(MAKE_DIRECTORY "${_out_base}/${_config}")
        file(COPY ${_dlls} DESTINATION "${_out_base}/${_config}")
    endforeach()
endfunction()

dep(DEPENDENCY_NAME_MACRO
    DEPENDENCIES_FOR_SHARED_LIB
    DEPENDENCIES_FOR_STATIC_LIB
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB
    DEPENDENCIES_INCLUDE_PATHS)
