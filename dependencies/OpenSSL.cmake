## description: OpenSSL is a cryptographic library that enables an open source implementation of Transport Layer Security (TLS) and Secure Sockets Layer (SSL) protocols.

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    # Define the git repository and tag to download from
    set(LIB_NAME OpenSSL)
	set(LIB_MACRO_NAME OPEN_SSL)

    # Install the library using the QT Maintenance Tool
    # Change the path to the installation directory of the library
    set(OPENSSL_ROOT_DIR "C:/Qt/Tools/OpenSSLv3/Win_x64")
    set(OPENSSL_MSVC_STATIC_RT True)
    set(OPENSSL_USE_STATIC_LIBS  True)
    find_package(${LIB_NAME} REQUIRED COMPONENTS SSL Crypto)

    # Add this library to the specific profiles of this project
    list(APPEND DEPS_FOR_SHARED_LIB OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND DEPS_FOR_STATIC_LIB OpenSSL::SSL OpenSSL::Crypto)
    list(APPEND DEPS_FOR_STATIC_PROFILE_LIB OpenSSL::SSL OpenSSL::Crypto)

	set(${LIBRARY_MACRO_NAME} "${${LIBRARY_MACRO_NAME}};${LIB_MACRO_NAME}" PARENT_SCOPE)
    set(${SHARED_LIB} "${${SHARED_LIB}};${DEPS_FOR_SHARED_LIB}" PARENT_SCOPE)
    set(${STATIC_LIB} "${${STATIC_LIB}};${DEPS_FOR_STATIC_LIB}" PARENT_SCOPE)
    set(${STATIC_PROFILE_LIB} "${${STATIC_PROFILE_LIB}};${DEPS_FOR_STATIC_PROFILE_LIB}" PARENT_SCOPE)
endfunction()

dep(DEPENDENCY_NAME_MACRO 
    DEPENDENCIES_FOR_SHARED_LIB 
    DEPENDENCIES_FOR_STATIC_LIB 
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB 
    DEPENDENCIES_INCLUDE_PATHS)