## description: Qt library that lets users and programs script a C++ application in JavaScript: the app defines the callable methods, scripts run sandboxed on their own thread, with checks, limits, dry run, call stepping and a built-in editor.
##
## QtLLM uses it for QtLLM::Scripting: the model writes one script instead of
## making a tool call per item. The dependency is optional:
##
##   QTLLM_SCRIPTING=ON  (default)  ScriptHost is fetched and linked (it brings
##                                  the Qt module Qml); SH_LIBRARY_AVAILABLE is
##                                  defined and QtLLM::Scripting exists.
##   QTLLM_SCRIPTING=OFF            nothing is fetched, no Qml. Scripting.h
##                                  declares nothing; QTLLM_HAS_SCRIPTING is 0.
##
##   cmake --preset x64-Debug -DQTLLM_SCRIPTING=OFF
##
## To build against a local checkout instead of the repository:
##
##   cmake --preset x64-Debug -DFETCHCONTENT_SOURCE_DIR_SCRIPTHOST="C:/path/to/ScriptHost"

option(QTLLM_SCRIPTING "Build QtLLM with scripting (QtLLM::Scripting); fetches the ScriptHost library" ON)

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    # Define the git repository and tag to download from
    set(LIB_NAME ScriptHost)
    set(LIB_MACRO_NAME SH_LIBRARY_AVAILABLE)
    set(GIT_REPO https://github.com/KROIA/ScriptHost.git)
    set(GIT_TAG main)
	set(NO_EXAMPLES True)
	set(NO_UNITTESTS True)
	set(ADDITIONAL_INCLUDE_PATHS )

	downloadStandardLibrary()
endfunction()

if(QTLLM_SCRIPTING)
    dep(DEPENDENCY_NAME_MACRO
        DEPENDENCIES_FOR_SHARED_LIB
        DEPENDENCIES_FOR_STATIC_LIB
        DEPENDENCIES_FOR_STATIC_PROFILE_LIB
        DEPENDENCIES_INCLUDE_PATHS)
    message(STATUS "QtLLM scripting: ON (ScriptHost)")
else()
    message(STATUS "QtLLM scripting: OFF")
endif()
