## description: Qt library that lets users and programs script a C++ application in JavaScript: the app defines the callable methods, scripts run sandboxed on their own threads, several at once, with timers, checks, limits, dry run and call stepping. The engine needs only Qt Core + Qml; the optional ScriptHostWidgets library adds the editor, debugger and scripts window.
##
## QtLLM uses it for QtLLM::Scripting: the model writes one script instead of
## making a tool call per item. The dependency is optional:
##
##   QTLLM_SCRIPTING=ON  (default)  ScriptHost and ScriptHostWidgets are
##                                  fetched and linked (they bring the Qt
##                                  module Qml); SH_LIBRARY_AVAILABLE is
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
	# Two libraries since ScriptHost 2.0: the engine (ScriptHost) and the
	# widgets (ScriptHostWidgets: Scripts tab, editor, approval dialog).
	# QtLLM uses both.
	set(ADDITIONAL_SHARED_LIB_DEPENDENCIES ScriptHostWidgets_shared)
	set(ADDITIONAL_STATIC_LIB_DEPENDENCIES ScriptHostWidgets_static)
	set(ADDITIONAL_STATIC_PROFILE_LIB_DEPENDENCIES
		"$<IF:$<TARGET_EXISTS:ScriptHostWidgets_static_profile>,ScriptHostWidgets_static_profile,ScriptHostWidgets_static>")

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
