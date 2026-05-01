## description: Deploys OpenSSL 1.1.x DLLs required by Qt5 Network for HTTPS at runtime.
## Qt 5.15.x requires exactly libssl-1_1-x64.dll and libcrypto-1_1-x64.dll.
## No compile-time linkage — Qt Network handles SSL internally.

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)

    ## Qt5 looks for these exact DLL names at runtime — no other version will work.
    set(_SSL_DLL_NAME    "libssl-1_1-x64.dll")
    set(_CRYPTO_DLL_NAME "libcrypto-1_1-x64.dll")

    ## Common install locations for the Shining Light "Win64 OpenSSL v1.1.1w" package
    set(_OPENSSL_SEARCH_DIRS
        "C:/Program Files/OpenSSL-Win64/bin"
        "C:/Program Files/OpenSSL-Win64-v1.1.1/bin"
        "C:/OpenSSL-Win64/bin"
        "C:/OpenSSL/bin"
    )

    set(_FOUND_SSL    "")
    set(_FOUND_CRYPTO "")

    foreach(_dir ${_OPENSSL_SEARCH_DIRS})
        if(EXISTS "${_dir}/${_SSL_DLL_NAME}" AND NOT _FOUND_SSL)
            set(_FOUND_SSL    "${_dir}/${_SSL_DLL_NAME}")
            set(_FOUND_CRYPTO "${_dir}/${_CRYPTO_DLL_NAME}")
        endif()
    endforeach()

    if(_FOUND_SSL AND EXISTS "${_FOUND_CRYPTO}")
        message(STATUS "Found OpenSSL 1.1.x DLLs for Qt5 Network SSL deployment:")
        message(STATUS "  ${_FOUND_SSL}")
        message(STATUS "  ${_FOUND_CRYPTO}")

        set(_ALL_DLLS "${_FOUND_SSL}" "${_FOUND_CRYPTO}")

        if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
            ## Install DLLs alongside the executables
            install(FILES ${_ALL_DLLS}
                    DESTINATION "${RELATIVE_INSTALL_BIN_FOLDER}")

            ## Copy to build output directories at configure time so executables
            ## launched directly from the build tree (not via install) also find them.
            ## Visual Studio multi-config puts binaries under build/Debug and build/Release.
            foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
                set(_dest "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_cfg}")
                file(MAKE_DIRECTORY "${_dest}")
                file(COPY ${_ALL_DLLS} DESTINATION "${_dest}")
            endforeach()

            ## Also copy to the base output directory for single-config generators (Ninja)
            file(COPY ${_ALL_DLLS} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        endif()

    else()
        message(WARNING
            "OpenSSL 1.1.x DLLs not found — Qt5 Network HTTPS will fail at runtime.\n"
            "Qt 5.15.x requires: ${_SSL_DLL_NAME} and ${_CRYPTO_DLL_NAME}\n"
            "The currently installed OpenSSL 3.x (libssl-4-x64.dll) is NOT compatible with Qt5.\n"
            "Install 'Win64 OpenSSL v1.1.1w Light' from Shining Light Productions.\n"
            "Default install path: C:/Program Files/OpenSSL-Win64/\n"
            "After installing, delete the CMake cache and reconfigure.")
    endif()

    ## No compile-time libraries added — Qt Network links SSL internally.
    set(${LIBRARY_MACRO_NAME} "${${LIBRARY_MACRO_NAME}}" PARENT_SCOPE)
    set(${SHARED_LIB}         "${${SHARED_LIB}}"         PARENT_SCOPE)
    set(${STATIC_LIB}         "${${STATIC_LIB}}"         PARENT_SCOPE)
    set(${STATIC_PROFILE_LIB} "${${STATIC_PROFILE_LIB}}" PARENT_SCOPE)

endfunction()

dep(DEPENDENCY_NAME_MACRO
    DEPENDENCIES_FOR_SHARED_LIB
    DEPENDENCIES_FOR_STATIC_LIB
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB
    DEPENDENCIES_INCLUDE_PATHS)


# Below is a helper batch script to silently install the required OpenSSL 1.1.1w Light package on Windows.
#[[
@echo off
setlocal

:: Check for administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo This script must be run as Administrator.
    pause
    exit /b 1
)

set INSTALLER_URL=https://slproweb.com/download/Win64OpenSSL_Light-1_1_1w.exe
set INSTALLER_PATH=%TEMP%\Win64OpenSSL_Light-1_1_1w.exe
set INSTALL_DIR=C:\Program Files\OpenSSL-Win64

:: Remove existing OpenSSL installation if present
if exist "%INSTALL_DIR%\unins000.exe" (
    echo Removing existing OpenSSL installation...
    "%INSTALL_DIR%\unins000.exe" /verysilent /suppressmsgboxes /norestart
)

:: Download installer
echo Downloading Win64 OpenSSL v1.1.1w Light...
curl -L -o "%INSTALLER_PATH%" "%INSTALLER_URL%"
if %errorlevel% neq 0 (
    echo Download failed. Exiting.
    exit /b 1
)

:: Install silently
echo Installing OpenSSL 1.1.1w...
"%INSTALLER_PATH%" /verysilent /suppressmsgboxes /norestart /sp-
if %errorlevel% neq 0 (
    echo Installation failed.
    exit /b 1
)

:: Verify installation
if exist "%INSTALL_DIR%\bin\openssl.exe" (
    echo Successfully installed:
    "%INSTALL_DIR%\bin\openssl.exe" version
) else (
    echo Installation may have failed - openssl.exe not found.
    exit /b 1
)

:: Clean up installer
del /f "%INSTALLER_PATH%"
echo Done.

endlocal
pause

]]