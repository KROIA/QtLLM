#pragma once

#include "UnitTest.h"
#include "QtLLM.h"
#include <QObject>
#include <QString>

// Prompt injection: application code placing a prompt into the conversation
// so it reads exactly as if the user had typed it.
class TST_ChatDockWidget : public UnitTest::Test
{
    TEST_CLASS(TST_ChatDockWidget)
public:
    TST_ChatDockWidget()
        : Test("TST_ChatDockWidget")
    {
        ADD_TEST(TST_ChatDockWidget::testSubmitPromptEmitsMessageSent);
        ADD_TEST(TST_ChatDockWidget::testSubmitPromptRejectsEmptyText);
        ADD_TEST(TST_ChatDockWidget::testSubmitPromptRefusedWhileLoading);
        ADD_TEST(TST_ChatDockWidget::testSetInputTextDoesNotSend);
    }

private:

    TEST_FUNCTION(testSubmitPromptEmitsMessageSent)
    {
        TEST_START;

        QtLLM::ChatDockWidget dock;
        QString sentText;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent,
                         [&sentText](const QString& text) { sentText = text; });

        const bool accepted = dock.submitPrompt(QStringLiteral("summarise the log"));

        TEST_ASSERT_TRUE(accepted);
        TEST_ASSERT_EQUAL(sentText, QString("summarise the log"));
    }


    TEST_FUNCTION(testSubmitPromptRejectsEmptyText)
    {
        TEST_START;

        QtLLM::ChatDockWidget dock;
        int emissions = 0;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent,
                         [&emissions](const QString&) { ++emissions; });

        const bool accepted = dock.submitPrompt(QStringLiteral("   "));

        TEST_ASSERT_FALSE(accepted);
        TEST_ASSERT_EQUAL(emissions, 0);
    }


    TEST_FUNCTION(testSubmitPromptRefusedWhileLoading)
    {
        TEST_START;

        QtLLM::ChatDockWidget dock;
        int emissions = 0;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent,
                         [&emissions](const QString&) { ++emissions; });

        dock.setLoading(true);
        const bool accepted = dock.submitPrompt(QStringLiteral("too soon"));

        TEST_ASSERT_FALSE(accepted);
        TEST_ASSERT_EQUAL(emissions, 0);
    }


    TEST_FUNCTION(testSetInputTextDoesNotSend)
    {
        TEST_START;

        QtLLM::ChatDockWidget dock;
        int emissions = 0;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent,
                         [&emissions](const QString&) { ++emissions; });

        dock.setInputText(QStringLiteral("a suggested draft"));

        TEST_ASSERT_EQUAL(emissions, 0);
        TEST_ASSERT_EQUAL(dock.inputText(), QString("a suggested draft"));
    }

};

TEST_INSTANTIATE(TST_ChatDockWidget);
