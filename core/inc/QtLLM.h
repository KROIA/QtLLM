// @file QtLLM.h
// @brief Main public header for the library.
//
// Include this single header to access the entire public API.
// Add your own public headers inside USER_SECTION 2 so that
// consumers only need `#include "QtLLM.h"`.
#pragma once

/// USER_SECTION_START 1

/// USER_SECTION_END

#include "QtLLM_info.h"

/// USER_SECTION_START 2

#include "UsageStats.h"
#include "Pricing.h"
#include "Tool.h"
#include "ToolResult.h"
#include "Client.h"
#include "OllamaManager.h"
#include "ChatDockWidget.h"
#include "InterviewWidget.h"
#include "InterviewTool.h"
#include "BuiltinTools.h"
#include "SettingsDialog.h"

/// USER_SECTION_END