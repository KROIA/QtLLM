#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QSet>


// Tests for the predefined out-of-the-box tools (BuiltinTools enum +
// registrar): name mapping, registration surface and UI metadata.
class TST_BuiltinTools : public UnitTest::Test
{
    TEST_CLASS(TST_BuiltinTools)
public:
    TST_BuiltinTools()
        : Test("TST_BuiltinTools")
    {
        ADD_TEST(TST_BuiltinTools::testToolNamesUniqueAndNonEmpty);
        ADD_TEST(TST_BuiltinTools::testToolNameMapping);
        ADD_TEST(TST_BuiltinTools::testRegisterAll);
        ADD_TEST(TST_BuiltinTools::testRegisterSingle);
        ADD_TEST(TST_BuiltinTools::testMetadataPresent);
        ADD_TEST(TST_BuiltinTools::testEnableDisableBuiltin);
        ADD_TEST(TST_BuiltinTools::testNullClientSafe);
        ADD_TEST(TST_BuiltinTools::testRepeatingTaskRegistersThree);
    }

private:

    TEST_FUNCTION(testToolNamesUniqueAndNonEmpty)
    {
        TEST_START;

        const QList<QtLLM::BuiltinTool> all = QtLLM::BuiltinTools::allTools();
        TEST_ASSERT(all.size() >= 12);

        QSet<QString> names;
        for (QtLLM::BuiltinTool tool : all) {
            const QString name = QtLLM::BuiltinTools::toolName(tool);
            TEST_ASSERT(!name.isEmpty());
            TEST_ASSERT(!names.contains(name));
            names.insert(name);
        }
    }


    TEST_FUNCTION(testToolNameMapping)
    {
        TEST_START;

        TEST_COMPARE(QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::AskUserQuestion),
                     QString("ask_user_question"));
        TEST_COMPARE(QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::FileDialog),
                     QString("file_dialog"));
        TEST_COMPARE(QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::CurrentDateTime),
                     QString("current_datetime"));
        TEST_COMPARE(QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::WriteTextFile),
                     QString("write_text_file"));

        // Must match the name the InterviewTool registers under
        TEST_COMPARE(QtLLM::BuiltinTools::toolName(QtLLM::BuiltinTool::AskUserQuestion),
                     QtLLM::InterviewTool::name());
    }


    TEST_FUNCTION(testRegisterAll)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::BuiltinTools::registerTools(&client, QtLLM::BuiltinTools::allTools());

        // RepeatingTask registers 3 tools (start/cancel/list) -> +2
        const QStringList names = client.toolNames();
        TEST_COMPARE(names.size(), QtLLM::BuiltinTools::allTools().size() + 2);
        for (QtLLM::BuiltinTool tool : QtLLM::BuiltinTools::allTools())
            TEST_ASSERT(names.contains(QtLLM::BuiltinTools::toolName(tool)));
        TEST_ASSERT(names.contains("cancel_repeating_task"));
        TEST_ASSERT(names.contains("list_repeating_tasks"));

        // All enabled by default
        TEST_COMPARE(client.enabledToolNames().size(), names.size());
    }


    TEST_FUNCTION(testRegisterSingle)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::BuiltinTools::registerTool(&client, QtLLM::BuiltinTool::FileDialog);

        TEST_COMPARE(client.toolNames().size(), 1);
        TEST_COMPARE(client.toolNames()[0], QString("file_dialog"));
    }


    TEST_FUNCTION(testMetadataPresent)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::BuiltinTools::registerTool(&client, QtLLM::BuiltinTool::FileDialog);
        QtLLM::BuiltinTools::registerTool(&client, QtLLM::BuiltinTool::ListDirectory);

        for (const QtLLM::Tool& tool : client.registeredTools()) {
            if (tool.name() == "file_dialog") {
                TEST_COMPARE(tool.title(), QString("File dialog"));
                TEST_COMPARE(tool.group(), QString("Dialogs"));
                TEST_ASSERT(!tool.statusText().isEmpty());
            } else {
                TEST_COMPARE(tool.name(), QString("list_directory"));
                TEST_COMPARE(tool.group(), QString("Filesystem"));
            }
            TEST_ASSERT(!tool.description().isEmpty());
        }
    }


    TEST_FUNCTION(testEnableDisableBuiltin)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::BuiltinTools::registerTools(&client,
            { QtLLM::BuiltinTool::ReadTextFile, QtLLM::BuiltinTool::WriteTextFile });

        client.setToolEnabled("write_text_file", false);
        TEST_ASSERT(!client.isToolEnabled("write_text_file"));
        TEST_ASSERT(client.isToolEnabled("read_text_file"));
        TEST_COMPARE(client.enabledToolNames().size(), 1);
    }


    TEST_FUNCTION(testNullClientSafe)
    {
        TEST_START;

        // Must not crash
        QtLLM::BuiltinTools::registerTool(nullptr, QtLLM::BuiltinTool::FileDialog);
        QtLLM::BuiltinTools::registerTools(nullptr, QtLLM::BuiltinTools::allTools());
        TEST_ASSERT(true);
    }


    TEST_FUNCTION(testRepeatingTaskRegistersThree)
    {
        TEST_START;

        QtLLM::Client client("fake_key");
        QtLLM::BuiltinTools::registerTool(&client, QtLLM::BuiltinTool::RepeatingTask);

        const QStringList names = client.toolNames();
        TEST_COMPARE(names.size(), 3);
        TEST_ASSERT(names.contains("start_repeating_task"));
        TEST_ASSERT(names.contains("cancel_repeating_task"));
        TEST_ASSERT(names.contains("list_repeating_tasks"));
    }
};

TEST_INSTANTIATE(TST_BuiltinTools);
