## description: simple profiler for applications

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    # Define the git repository and tag to download from
    set(LIB_NAME easy_profiler)
    set(LIB_MACRO_NAME EASY_PROFILER_LIBRARY_AVAILABLE)
    set(GIT_REPO https://github.com/yse/easy_profiler.git)
    set(GIT_TAG v2.1.0)

    set(EASY_PROFILER_NO_SAMPLES True)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build easy_profiler as static library.")

    # Allow old CMakeLists.txt to work: suppress the version-too-old error by telling CMake
    # to interpret cmake_minimum_required(VERSION 3.5) as if it was cmake_minimum_required(VERSION 3.20)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

    # easy_profiler's GUI (profiler_gui) is the only part of easy_profiler that
    # uses Qt, and it is Qt5-only: it calls find_package(Qt5Widgets) directly.
    # Previously this block forced QT_MAJOR_VERSION back to 5 and re-ran
    # QtLocator so the GUI could still be built inside a Qt6 parent project.
    # That leaves two Qt majors in one CMake tree, and AUTOGEN can then compute
    # rcc options for one major while resolving the rcc binary from the other,
    # which fails with "rcc: Unknown option 'no-zstd'".
    # Build the GUI only when the parent project is on Qt5; skip it otherwise.
    # The easy_profiler core library itself needs no Qt, so the profiling build
    # profile is unaffected.
    # CACHE ... FORCE is required because easy_profiler declares this variable
    # with option(), and whether option() defers to a plain variable depends on
    # policy CMP0077, which its cmake_minimum_required(VERSION 3.5) leaves OLD.
    if(QT_MAJOR_VERSION EQUAL 5)
        set(EASY_PROFILER_NO_GUI OFF CACHE BOOL "Skip easy_profiler's Qt5-only GUI application" FORCE)
    else()
        set(EASY_PROFILER_NO_GUI ON CACHE BOOL "Skip easy_profiler's Qt5-only GUI application" FORCE)
    endif()

    # Add this library to the specific profiles of this project
    list(APPEND SHARED_LIB_DEPENDENCY "")
    list(APPEND STATIC_LIB_DEPENDENCY "")
    list(APPEND STATIC_PROFILE_LIB_DEPENDENCY ${LIB_NAME}) # only use for static profiling profile

    downloadExternalLibrary()

    set(EASY_PROFILER_IS_AVAILABLE ON PARENT_SCOPE)


    # Deploy the Profiler GUI. Only exists when it was actually built, i.e. when
    # the parent project is on Qt5.
    if(QT_ENABLE AND QT_DEPLOY AND NOT EASY_PROFILER_NO_GUI)
        windeployqt(profiler_gui ${INSTALL_BIN_PATH})
    endif()


    set_target_properties(${LIB_NAME} PROPERTIES CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RUNTIME_OUTPUT_DIRECTORY})
    set_target_properties(${LIB_NAME} PROPERTIES CMAKE_LIBRARY_OUTPUT_DIRECTORY ${RUNTIME_OUTPUT_DIRECTORY})
    set_target_properties(${LIB_NAME} PROPERTIES CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${RUNTIME_OUTPUT_DIRECTORY})
    set_target_properties(${LIB_NAME} PROPERTIES DEBUG_POSTFIX ${DEBUG_POSTFIX_STR})
    target_compile_definitions(${LIB_NAME} PUBLIC EASY_PROFILER_STATIC)
endfunction()

dep(DEPENDENCY_NAME_MACRO 
    DEPENDENCIES_FOR_SHARED_LIB 
    DEPENDENCIES_FOR_STATIC_LIB 
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB 
    DEPENDENCIES_INCLUDE_PATHS)