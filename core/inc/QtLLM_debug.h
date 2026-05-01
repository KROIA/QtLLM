// @file LibraryName_debug.h
// @brief Debug console output, profiling macros, and library-wide Logger.
//
// This header provides three facilities:
//   1. Console macros that print to stdout in Debug builds and compile to
//      nothing in Release builds.
//   2. Profiling macros that wrap easy_profiler when the profiling build
//      target is selected, and compile to nothing otherwise.
//   3. A library-wide Logger singleton (available when the Logger dependency
//      is present).
//
// @see EasyProfilerIntegration.md for profiling usage.
// @see LoggerIntegration.md for logging usage.
#pragma once
#include "QtLLM_global.h"

/// USER_SECTION_START 14

/// USER_SECTION_END

// The Logger library is automatically included when the Logger dependency .cmake file is present.
#if LOGGER_LIBRARY_AVAILABLE == 1
	#include "Logger.h"
#endif

/// USER_SECTION_START 1

/// USER_SECTION_END

// ---------------------------------------------------------------------------
// Console output macros (Debug builds only)
// ---------------------------------------------------------------------------
#ifdef NDEBUG
	#define QLLM_CONSOLE(msg)
	#define QLLM_CONSOLE_FUNCTION(msg)
#else
	#include <iostream>

	#define QLLM_DEBUG
	#define QLLM_CONSOLE_STREAM std::cout

	// Print msg to stdout.
	#define QLLM_CONSOLE(msg) QLLM_CONSOLE_STREAM << msg;
	// Print the current function signature followed by msg to stdout.
	#define QLLM_CONSOLE_FUNCTION(msg) QLLM_CONSOLE_STREAM << __PRETTY_FUNCTION__ << " " << msg;
#endif

/// USER_SECTION_START 2

/// USER_SECTION_END

// ---------------------------------------------------------------------------
// Profiling macros (wrap easy_profiler; no-ops when profiling is disabled)
// ---------------------------------------------------------------------------
#ifdef QLLM_PROFILING
	#include "easy/profiler.h"
	#include <easy/arbitrary_value.h>

	// Scoped profiling block with an explicit color value.
	#define QLLM_PROFILING_BLOCK_C(text, color) EASY_BLOCK(text, color)
	// Non-scoped profiling block — must be closed with PROFILING_END_BLOCK.
	#define QLLM_PROFILING_NONSCOPED_BLOCK_C(text, color) EASY_NONSCOPED_BLOCK(text, color)
	// End a non-scoped profiling block.
	#define QLLM_PROFILING_END_BLOCK EASY_END_BLOCK
	// Profile the enclosing function with an explicit color value.
	#define QLLM_PROFILING_FUNCTION_C(color) EASY_FUNCTION(color)
	// Scoped profiling block using a color stage name (e.g. Cyan500).
	#define QLLM_PROFILING_BLOCK(text, colorStage) QLLM_PROFILING_BLOCK_C(text,profiler::colors::  colorStage)
	// Non-scoped profiling block using a color stage name.
	#define QLLM_PROFILING_NONSCOPED_BLOCK(text, colorStage) QLLM_PROFILING_NONSCOPED_BLOCK_C(text,profiler::colors::  colorStage)
	// Profile the enclosing function using a color stage name.
	#define QLLM_PROFILING_FUNCTION(colorStage) QLLM_PROFILING_FUNCTION_C(profiler::colors:: colorStage)
	// Assign a name to the current thread in the profiler output.
	#define QLLM_PROFILING_THREAD(name) EASY_THREAD(name)

	// Capture a named scalar value in the profiler timeline.
	#define QLLM_PROFILING_VALUE(name, value) EASY_VALUE(name, value)
	// Capture a named text string in the profiler timeline.
	#define QLLM_PROFILING_TEXT(name, value) EASY_TEXT(name, value)

#else
	#define QLLM_PROFILING_BLOCK_C(text, color)
	#define QLLM_PROFILING_NONSCOPED_BLOCK_C(text, color)
	#define QLLM_PROFILING_END_BLOCK
	#define QLLM_PROFILING_FUNCTION_C(color)
	#define QLLM_PROFILING_BLOCK(text, colorStage)
	#define QLLM_PROFILING_NONSCOPED_BLOCK(text, colorStage)
	#define QLLM_PROFILING_FUNCTION(colorStage)
	#define QLLM_PROFILING_THREAD(name)

	#define QLLM_PROFILING_VALUE(name, value)
	#define QLLM_PROFILING_TEXT(name, value)
#endif

// Token-pasting helper used to build color stage identifiers (e.g. Cyan500).
#define CONCAT_SYMBOLS_IMPL(x, y) x##y
#define CONCAT_SYMBOLS(x, y) CONCAT_SYMBOLS_IMPL(x, y)

// ---------------------------------------------------------------------------
// Color stage suffixes for profiling blocks.
// Use these with the PROFILING_BLOCK / PROFILING_FUNCTION macros to visually
// distinguish call depths in the easy_profiler timeline.
// ---------------------------------------------------------------------------
#define QLLM_COLOR_STAGE_1 50
#define QLLM_COLOR_STAGE_2 100
#define QLLM_COLOR_STAGE_3 200
#define QLLM_COLOR_STAGE_4 300
#define QLLM_COLOR_STAGE_5 400
#define QLLM_COLOR_STAGE_6 500
#define QLLM_COLOR_STAGE_7 600
#define QLLM_COLOR_STAGE_8 700
#define QLLM_COLOR_STAGE_9 800
#define QLLM_COLOR_STAGE_10 900
#define QLLM_COLOR_STAGE_11 A100
#define QLLM_COLOR_STAGE_12 A200
#define QLLM_COLOR_STAGE_13 A400
#define QLLM_COLOR_STAGE_14 A700

namespace QtLLM
{
/// USER_SECTION_START 4

/// USER_SECTION_END

	// Controls the easy_profiler runtime.
	// Call start() before the code you want to profile and stop() afterwards.
	// The profiler is only functional in the static-profile build target;
	// in other targets the methods are no-ops.
	class QT_LLM_API Profiler
	{
	public:
		/// USER_SECTION_START 5

		/// USER_SECTION_END

		// Enable profiling data collection.
		static void start();
		// Stop profiling and write results to "profile.prof".
		static void stop();
		// Stop profiling and write results to the given profilerOutputFile.
		static void stop(const char* profilerOutputFile);

		/// USER_SECTION_START 6

		/// USER_SECTION_END
	};

/// USER_SECTION_START 7

/// USER_SECTION_END


#if LOGGER_LIBRARY_AVAILABLE == 1
	// Library-wide logger singleton backed by a Log::LogObject.
	//
	// All static methods delegate to a single internal LogObject whose name
	// defaults to the library name. Use this class for log messages that
	// belong to the library as a whole. For per-class logging, create your
	// own Log::LogObject and set its parent to Logger::getID().
	class QT_LLM_API Logger
	{
		/// USER_SECTION_START 8

		/// USER_SECTION_END
		Logger();
		static Logger& instance();
		public:
		/// USER_SECTION_START 9

		/// USER_SECTION_END

		static void setEnabled(bool enable);
		static bool isEnabled();
		static void setName(const std::string& name);
		static std::string getName();
		static void setColor(const Log::Color& col);
		static Log::Color getColor();
		static Log::DateTime getCreationDateTime();
		static Log::LoggerID getID();
		static void setParentID(Log::LoggerID parentID);
		static Log::LoggerID getParentID();

		static void log(const Log::Message& msg);

		static void log(const std::string& msg);
		static void log(const std::string& msg, Log::Level level);
		static void log(const std::string& msg, Log::Level level, const Log::Color& col);

		static void logTrace(const std::string& msg);
		static void logDebug(const std::string& msg);
		static void logInfo(const std::string& msg);
		static void logWarning(const std::string& msg);
		static void logError(const std::string& msg);
		static void logCustom(const std::string& msg);

		/// USER_SECTION_START 10

		/// USER_SECTION_END

		private:
		Log::LogObject m_logObject;

		/// USER_SECTION_START 11

		/// USER_SECTION_END
	};
/// USER_SECTION_START 12

/// USER_SECTION_END
#endif
/// USER_SECTION_START 13

/// USER_SECTION_END
}


// ---------------------------------------------------------------------------
// Default "General" profiling section.
// Copy this block into USER_SECTION 3 to create additional profiling sections
// with a different COLORBASE (see EasyProfilerIntegration.md).
// ---------------------------------------------------------------------------
#define QLLM_GENERAL_PROFILING_COLORBASE Cyan
#define QLLM_GENERAL_PROFILING_BLOCK_C(text, color) QLLM_PROFILING_BLOCK_C(text, color)
#define QLLM_GENERAL_PROFILING_NONSCOPED_BLOCK_C(text, color) QLLM_PROFILING_NONSCOPED_BLOCK_C(text, color)
#define QLLM_GENERAL_PROFILING_END_BLOCK QLLM_PROFILING_END_BLOCK;
#define QLLM_GENERAL_PROFILING_FUNCTION_C(color) QLLM_PROFILING_FUNCTION_C(color)
#define QLLM_GENERAL_PROFILING_BLOCK(text, colorStage) QLLM_PROFILING_BLOCK(text, CONCAT_SYMBOLS(QLLM_GENERAL_PROFILING_COLORBASE, colorStage))
#define QLLM_GENERAL_PROFILING_NONSCOPED_BLOCK(text, colorStage) QLLM_PROFILING_NONSCOPED_BLOCK(text, CONCAT_SYMBOLS(QLLM_GENERAL_PROFILING_COLORBASE, colorStage))
#define QLLM_GENERAL_PROFILING_FUNCTION(colorStage) QLLM_PROFILING_FUNCTION(CONCAT_SYMBOLS(QLLM_GENERAL_PROFILING_COLORBASE, colorStage))
#define QLLM_GENERAL_PROFILING_VALUE(name, value) QLLM_PROFILING_VALUE(name, value)
#define QLLM_GENERAL_PROFILING_TEXT(name, value) QLLM_PROFILING_TEXT(name, value)


/// USER_SECTION_START 3

/// USER_SECTION_END