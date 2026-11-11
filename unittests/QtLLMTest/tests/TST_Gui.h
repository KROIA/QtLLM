#pragma once

// GUI tests: the library's widgets driven the way a user drives them - by
// clicking and typing - through the UnitTest GUI driver. Widgets are found by
// objectName ("qtllm_...") or visible text, never by position.
//
// Compiled only when the UnitTest dependency provides the driver (see
// unittests/QtLLMTest/CMakeLists.txt).
#ifdef UNITTEST_GUI_ENABLED

#include "UnitTest.h"
#include "UnitTest_Gui.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>

class TST_Gui : public UnitTest::Test
{
    TEST_CLASS(TST_Gui)
public:
    TST_Gui()
        : Test("TST_Gui")
    {
        ADD_TEST(TST_Gui::testSettingsProviderFieldsFollowCombo);
        ADD_TEST(TST_Gui::testSettingsFoundryEndpointIsCompleted);
        ADD_TEST(TST_Gui::testSettingsRestoringProviderKeepsModel);
        ADD_TEST(TST_Gui::testSettingsApplySwitchesClientProvider);
        ADD_TEST(TST_Gui::testSettingsToolsTabTogglesTool);
        ADD_TEST(TST_Gui::testChatTypeAndSend);
        ADD_TEST(TST_Gui::testChatCancelButtonCancelsTheTurn);
        ADD_TEST(TST_Gui::testChatClearButtonClearsConversation);
        ADD_TEST(TST_Gui::testChatStreamsIntoOneBubble);
        ADD_TEST(TST_Gui::testChatCompactButton);
        ADD_TEST(TST_Gui::testSettingsContextPolicyAndEffort);
        ADD_TEST(TST_Gui::testInterviewCardAnswer);
        ADD_TEST(TST_Gui::testMessageBoxToolAnsweredByUser);
    }

private:
    // Shorthand, so a failed lookup reads as a failed assertion rather than a crash.
    template<class T>
    static T* find(const char* name, QWidget* root)
    {
        return UnitTest::Gui::find<T>(QString::fromLatin1(name), root);
    }

    static bool visible(QWidget* widget) { return widget && widget->isVisible(); }

    // ------------------------------------------------------------ SettingsDialog

    TEST_FUNCTION(testSettingsProviderFieldsFollowCombo)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::SettingsDialog dlg;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));
        auto* combo = find<QComboBox>("qtllm_providerCombo", &dlg);
        TEST_ASSERT_NOT_NULL(combo);

        // Default: Anthropic API key.
        TEST_ASSERT(dlg.provider() == QtLLM::SettingsDialog::Provider::Claude);
        TEST_ASSERT(visible(find<QLineEdit>("qtllm_apiKeyEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_ollamaUrlEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_foundryKeyEdit", &dlg)));

        TEST_ASSERT(UnitTest::Gui::selectComboText(combo, "Ollama (local models)"));
        TEST_ASSERT(dlg.provider() == QtLLM::SettingsDialog::Provider::Ollama);
        TEST_ASSERT(visible(find<QLineEdit>("qtllm_ollamaUrlEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_apiKeyEdit", &dlg)));
        TEST_COMPARE(dlg.model(), QString("llama3.2:latest"));

        TEST_ASSERT(UnitTest::Gui::selectComboText(combo, "Microsoft Foundry (Claude)"));
        TEST_ASSERT(dlg.provider() == QtLLM::SettingsDialog::Provider::Foundry);
        TEST_ASSERT(visible(find<QLineEdit>("qtllm_foundryKeyEdit", &dlg)));
        TEST_ASSERT(visible(find<QLineEdit>("qtllm_foundryUrlEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_apiKeyEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_ollamaUrlEdit", &dlg)));
        TEST_COMPARE(dlg.model(), QtLLM::Client::defaultModel(QtLLM::Provider::Claude));

        TEST_ASSERT(UnitTest::Gui::selectComboText(combo, "Claude subscription (Claude Code login)"));
        TEST_ASSERT(dlg.provider() == QtLLM::SettingsDialog::Provider::ClaudeSubscription);
        auto* status = find<QLabel>("qtllm_subscriptionStatus", &dlg);
        TEST_ASSERT(visible(status));
        TEST_ASSERT(status && !status->text().isEmpty());   // installed / not installed / checking
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_foundryKeyEdit", &dlg)));
        TEST_ASSERT(!visible(find<QLineEdit>("qtllm_apiKeyEdit", &dlg)));
        TEST_COMPARE(dlg.model(), QString("sonnet"));
    }

    TEST_FUNCTION(testSettingsFoundryEndpointIsCompleted)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::SettingsDialog dlg;
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));
        TEST_ASSERT(UnitTest::Gui::selectComboText(find<QComboBox>("qtllm_providerCombo", &dlg),
                                                   "Microsoft Foundry (Claude)"));

        auto* url = find<QLineEdit>("qtllm_foundryUrlEdit", &dlg);
        auto* key = find<QLineEdit>("qtllm_foundryKeyEdit", &dlg);
        TEST_ASSERT(UnitTest::Gui::type(key, "secret-key"));
        TEST_COMPARE(dlg.foundryApiKey(), QString("secret-key"));
        TEST_ASSERT(key && key->echoMode() == QLineEdit::Password);

        // A bare resource URL, as shown in the Foundry portal.
        TEST_ASSERT(UnitTest::Gui::type(url, "https://res.services.ai.azure.com/anthropic/"));
        TEST_COMPARE(dlg.foundryEndpointUrl(),
                     QString("https://res.services.ai.azure.com/anthropic/v1/messages"));

        // A complete endpoint is taken as typed.
        TEST_ASSERT(UnitTest::Gui::clearAndType(url, "https://gw.example/v1/messages"));
        TEST_COMPARE(dlg.foundryEndpointUrl(), QString("https://gw.example/v1/messages"));
    }

    // An application restores its saved state with setModel() + setProvider().
    // Selecting the provider that way must not reset the model to the
    // provider's default, as a user changing the combo does.
    TEST_FUNCTION(testSettingsRestoringProviderKeepsModel)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::SettingsDialog dlg;
        dlg.setModel("my-saved-model");
        dlg.setProvider(QtLLM::SettingsDialog::Provider::Ollama);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));

        TEST_COMPARE(dlg.model(), QString("my-saved-model"));
        TEST_ASSERT(visible(find<QLineEdit>("qtllm_ollamaUrlEdit", &dlg)));
    }

    TEST_FUNCTION(testSettingsApplySwitchesClientProvider)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QtLLM::SettingsDialog dlg;
        dlg.setClient(&client);
        QObject::connect(&dlg, &QtLLM::SettingsDialog::settingsApplied, [&]() { dlg.applyTo(&client); });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));

        TEST_ASSERT(UnitTest::Gui::selectComboText(find<QComboBox>("qtllm_providerCombo", &dlg),
                                                   "Claude subscription (Claude Code login)"));
        auto* prompt = find<QTextEdit>("qtllm_systemPromptEdit", &dlg);
        TEST_ASSERT(UnitTest::Gui::type(prompt, "Be brief."));
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyButton", &dlg)));

        TEST_ASSERT(client.provider() == QtLLM::Provider::ClaudeCode);
        TEST_COMPARE(client.model(), QString("sonnet"));
        TEST_COMPARE(client.exportConversation()["systemPrompt"].toString(), QString("Be brief."));

        // Shown with show(), it is a window beside the application: Apply
        // leaves it open, and it can be applied again.
        TEST_ASSERT(dlg.isVisible());
        TEST_ASSERT(UnitTest::Gui::type(prompt, " Very."));
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyButton", &dlg)));
        TEST_ASSERT(client.exportConversation()["systemPrompt"].toString().endsWith("Very."));
        TEST_ASSERT(dlg.isVisible());
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_cancelButton", &dlg)));
        TEST_ASSERT(UnitTest::Gui::waitForClosed(&dlg));

        // Modal too, Apply leaves it open; "Apply and close" applies and
        // accepts it.
        dlg.open();
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyButton", &dlg)));
        TEST_ASSERT(dlg.isVisible());
        TEST_ASSERT(UnitTest::Gui::type(prompt, " Now."));
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyCloseButton", &dlg)));
        TEST_ASSERT(UnitTest::Gui::waitForClosed(&dlg));
        TEST_COMPARE(dlg.result(), int(QDialog::Accepted));
        TEST_ASSERT(client.exportConversation()["systemPrompt"].toString().endsWith("Now."));
    }

    TEST_FUNCTION(testSettingsToolsTabTogglesTool)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QtLLM::Tool alpha, beta;
        alpha.setName("alpha").setTitle("Alpha tool").setGroup("Demo").setDescription("first");
        beta.setName("beta").setDescription("second");
        client.registerTool(alpha, [](const QJsonObject&) { return QJsonObject(); });
        client.registerTool(beta, [](const QJsonObject&) { return QJsonObject(); });

        QtLLM::SettingsDialog dlg;
        dlg.setClient(&client);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));
        TEST_ASSERT(UnitTest::Gui::selectTab(find<QTabWidget>("qtllm_settingsTabs", &dlg), "Tools"));

        auto* alphaCheck = find<QCheckBox>("qtllm_tool_alpha", &dlg);
        auto* betaCheck  = find<QCheckBox>("qtllm_tool_beta", &dlg);
        TEST_ASSERT_NOT_NULL(alphaCheck);
        TEST_ASSERT_NOT_NULL(betaCheck);
        TEST_ASSERT(alphaCheck && alphaCheck->isChecked());
        TEST_ASSERT(alphaCheck && alphaCheck->text().contains("Alpha tool"));

        TEST_ASSERT(UnitTest::Gui::setChecked(alphaCheck, false));
        // Nothing changes until Apply.
        TEST_ASSERT(client.isToolEnabled("alpha"));

        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyButton", &dlg)));
        TEST_ASSERT(!client.isToolEnabled("alpha"));
        TEST_ASSERT(client.isToolEnabled("beta"));
        TEST_COMPARE(client.enabledToolNames(), QStringList{"beta"});
    }

    // ------------------------------------------------------------ ChatDockWidget

    TEST_FUNCTION(testChatTypeAndSend)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::ChatDockWidget dock;
        QStringList sent;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent,
                         [&](const QString& text) { sent.append(text); });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        auto* input = find<QTextEdit>("qtllm_chatInput", &dock);
        auto* send  = find<QPushButton>("qtllm_sendButton", &dock);
        TEST_ASSERT(UnitTest::Gui::type(input, "rename the window"));
        TEST_ASSERT(UnitTest::Gui::click(send));

        TEST_COMPARE(sent, QStringList{"rename the window"});
        TEST_COMPARE(dock.inputText(), QString());   // the field is emptied for the next prompt

        // An empty field sends nothing.
        TEST_ASSERT(UnitTest::Gui::click(send));
        TEST_COMPARE(sent.size(), 1);
    }

    // The Cancel button appears after a few seconds of waiting; clicking it
    // must stop the request, not just hide the spinner.
    TEST_FUNCTION(testChatCancelButtonCancelsTheTurn)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        FakeApi api;
        api.silent = true;
        QtLLM::Client client("key", api.url());
        int finished = 0, responses = 0;
        QObject::connect(&client, &QtLLM::Client::requestFinished, [&]() { ++finished; });
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString&) { ++responses; });

        QtLLM::ChatDockWidget dock;
        dock.setClient(&client);
        QObject::connect(&dock, &QtLLM::ChatDockWidget::messageSent, [&](const QString& text) {
            dock.setLoading(true);
            client.sendPrompt(text);
        });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        TEST_ASSERT(UnitTest::Gui::type(find<QTextEdit>("qtllm_chatInput", &dock), "hello"));
        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_sendButton", &dock)));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return api.messageRequests == 1; }, 3000));

        auto* cancel = find<QPushButton>("qtllm_cancelButton", &dock);
        TEST_ASSERT(!visible(cancel));                       // not offered immediately
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return visible(cancel); }, 8000));
        TEST_ASSERT(UnitTest::Gui::click(cancel));

        TEST_COMPARE(finished, 1);
        TEST_COMPARE(responses, 0);
        TEST_ASSERT(client.conversationHistory().isEmpty());
        TEST_ASSERT(!visible(cancel));
        TEST_ASSERT(!visible(find<QLabel>("qtllm_loadingLabel", &dock)));
    }

    TEST_FUNCTION(testChatClearButtonClearsConversation)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("hi there"));
        QtLLM::Client client("key", api.url());
        QtLLM::ChatDockWidget dock;
        dock.setClient(&client);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        client.sendPrompt("hello");
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return client.conversationHistory().size() == 2; }, 5000));

        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_clearButton", &dock)));
        TEST_ASSERT(client.conversationHistory().isEmpty());
        TEST_ASSERT(UnitTest::Gui::findWidgetByText("Context cleared", &dock) != nullptr);
    }

    // The answer grows in one bubble while it streams, and the application's
    // usual addAssistantMessage() on responseReady completes that same bubble.
    TEST_FUNCTION(testChatStreamsIntoOneBubble)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        FakeApi api;
        api.splitWrites = true;
        api.replies.enqueue(FakeApi::sse("end_turn", QJsonArray{
            QJsonObject{{"type", "text"}, {"text", "streamed **answer** text"}}}));
        QtLLM::Client client("key", api.url());
        QtLLM::ChatDockWidget dock;
        dock.setClient(&client);
        QObject::connect(&client, &QtLLM::Client::responseReady,
                         [&](const QString& text) { dock.addAssistantMessage(text); });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        client.sendPrompt("hi");
        // While streaming, a bubble with that name exists ...
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() {
            return UnitTest::Gui::findWidget("qtllm_streamingBubble", &dock) != nullptr
                || client.conversationHistory().size() == 2;
        }, 5000));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return client.conversationHistory().size() == 2; }, 5000));

        // ... and afterwards exactly one bubble holds the answer.
        TEST_ASSERT(UnitTest::Gui::findWidget("qtllm_streamingBubble", &dock) == nullptr);
        int bubbles = 0;
        for (QLabel* label : dock.findChildren<QLabel*>()) {
            if (label->text().contains("streamed") && label->text().contains("answer"))
                ++bubbles;
        }
        TEST_COMPARE(bubbles, 1);
    }

    TEST_FUNCTION(testChatCompactButton)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("an answer"));
        api.replies.enqueue(FakeApi::textReply("SUMMARY"));
        QtLLM::Client client("key", api.url());
        QtLLM::ChatDockWidget dock;
        dock.setClient(&client);
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        auto* compact = find<QPushButton>("qtllm_compactButton", &dock);
        TEST_ASSERT_NOT_NULL(compact);
        TEST_ASSERT(compact && !compact->icon().isNull());
        TEST_ASSERT(compact && !compact->toolTip().isEmpty());

        // Not while a turn is running.
        dock.setLoading(true);
        TEST_ASSERT(compact && !compact->isEnabled());
        dock.setLoading(false);
        TEST_ASSERT(compact && compact->isEnabled());

        // Nothing to compact yet: a hint, no request.
        TEST_ASSERT(UnitTest::Gui::click(compact));
        TEST_COMPARE(api.messageRequests, 0);
        TEST_ASSERT(find<QLabel>("qtllm_statusLabel", &dock)->text().contains("Nothing to compact"));

        client.sendPrompt("a question");
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return client.conversationHistory().size() == 2; }, 5000));

        TEST_ASSERT(UnitTest::Gui::click(compact));
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() {
            return UnitTest::Gui::findWidgetByText("Context compacted", &dock) != nullptr
                || !dock.findChildren<QLabel*>().isEmpty() && [&]() {
                       for (QLabel* label : dock.findChildren<QLabel*>())
                           if (label->text().startsWith("Context compacted"))
                               return true;
                       return false;
                   }();
        }, 5000));
        TEST_COMPARE(client.conversationHistory().size(), 1);
        TEST_ASSERT(compact && compact->isEnabled());
    }

    // The Context tab's limit, the automatic switch, the strategy and the
    // effort level reach the client on Apply.
    TEST_FUNCTION(testSettingsContextPolicyAndEffort)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        QtLLM::SettingsDialog dlg;
        dlg.setClient(&client);
        dlg.setProvider(QtLLM::SettingsDialog::Provider::Ollama);
        dlg.setOllamaUrl("http://127.0.0.1:9");
        QObject::connect(&dlg, &QtLLM::SettingsDialog::settingsApplied, [&]() { dlg.applyTo(&client); });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dlg));

        TEST_ASSERT(UnitTest::Gui::selectComboText(find<QComboBox>("qtllm_effortCombo", &dlg),
                                                   "Low - fastest, cheapest"));

        TEST_ASSERT(UnitTest::Gui::selectTab(find<QTabWidget>("qtllm_settingsTabs", &dlg), "Context"));
        auto* fullWindow = find<QCheckBox>("qtllm_fullWindowCheck", &dlg);
        auto* limit      = find<QSpinBox>("qtllm_contextLimitSpin", &dlg);
        TEST_ASSERT(fullWindow && fullWindow->isChecked());
        TEST_ASSERT(limit && !limit->isEnabled());           // nothing to set while "full window" is on

        TEST_ASSERT(UnitTest::Gui::setChecked(fullWindow, false));
        TEST_ASSERT(limit && limit->isEnabled());
        TEST_ASSERT(UnitTest::Gui::setSpinValue(limit, 300000));
        TEST_ASSERT(UnitTest::Gui::setChecked(find<QCheckBox>("qtllm_autoCompactCheck", &dlg), true));
        TEST_ASSERT(UnitTest::Gui::selectComboIndex(find<QComboBox>("qtllm_strategyCombo", &dlg), 1));

        TEST_ASSERT(UnitTest::Gui::click(find<QPushButton>("qtllm_applyButton", &dlg)));

        const QtLLM::ContextPolicy policy = client.contextPolicy();
        TEST_COMPARE(policy.limitTokens, 300000);
        TEST_ASSERT(policy.autoCompact);
        TEST_ASSERT(policy.strategy == QtLLM::ContextPolicy::Strategy::DropOldest);
        TEST_ASSERT(client.effort() == QtLLM::Effort::Low);
        // The usage bar now measures against the user's limit (or the
        // model's own window where that is smaller).
        TEST_ASSERT(client.contextBreakdown().contextWindowTokens <= 300000);

        // And the dialog shows a saved policy again.
        QtLLM::SettingsDialog again;
        again.setContextPolicy(policy);
        TEST_COMPARE(again.contextPolicy().limitTokens, 300000);
        TEST_ASSERT(again.contextPolicy().autoCompact);
    }

    // ------------------------------------------------------------ InterviewWidget

    TEST_FUNCTION(testInterviewCardAnswer)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        QtLLM::ChatDockWidget dock;
        QJsonObject result;
        QObject::connect(&dock, &QtLLM::ChatDockWidget::interviewFinished,
                         [&](const QJsonObject& r) { result = r; });
        TEST_ASSERT(UnitTest::Gui::showAndWait(&dock));

        const QJsonObject request{{"questions", QJsonArray{QJsonObject{
            {"question", "Which database?"},
            {"options", QJsonArray{QJsonObject{{"label", "Postgres"}},
                                   QJsonObject{{"label", "SQLite"}}}}}}}};
        QtLLM::InterviewWidget* card = dock.addInterviewWidget(request);
        TEST_ASSERT_NOT_NULL(card);
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return card->isVisible(); }));

        QWidget* submit = UnitTest::Gui::findWidgetByText("Submit", card);
        TEST_ASSERT_NOT_NULL(submit);
        TEST_ASSERT(submit && !submit->isEnabled());         // nothing chosen yet

        auto* option = qobject_cast<QAbstractButton*>(UnitTest::Gui::findWidgetByText("SQLite", card));
        TEST_ASSERT(UnitTest::Gui::setChecked(option, true));
        TEST_ASSERT(submit && submit->isEnabled());
        TEST_ASSERT(UnitTest::Gui::click(submit));

        TEST_ASSERT(card->isAnswered());
        TEST_COMPARE(result["status"].toString(), QString("answered"));
        TEST_COMPARE(result["answers"].toObject()["Which database?"].toString(), QString("SQLite"));
    }

    // ------------------------------------------------------------ built-in dialog tool, full stack
    //
    // model asks -> tool call -> modal QMessageBox -> the "user" clicks Yes ->
    // the answer goes back to the model -> final reply.
    TEST_FUNCTION(testMessageBoxToolAnsweredByUser)
    {
        TEST_START;
        if (!UnitTest::Gui::isAvailable()) { TEST_MESSAGE("Skipping: no GUI session"); return; }

        FakeApi api;
        // Deliberately mis-cased enum value: the handler must still see "question".
        api.replies.enqueue(FakeApi::toolUseReply("show_message",
            QJsonObject{{"type", "Question"}, {"message", "Delete the file?"}}));
        api.replies.enqueue(FakeApi::textReply("deleted"));

        QtLLM::Client client("key", api.url());
        QtLLM::BuiltinTools::registerTool(&client, QtLLM::BuiltinTool::MessageBox);

        QString response;
        QStringList errors;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { response = t; });
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { errors.append(m); });

        UnitTest::Gui::onNextMessageBox("Yes", 8000);   // registered before the blocking dialog
        client.sendPrompt("delete it");
        TEST_ASSERT(UnitTest::Gui::waitFor([&]() { return !response.isEmpty() || !errors.isEmpty(); }, 10000));

        TEST_ASSERT(errors.isEmpty());
        TEST_COMPARE(response, QString("deleted"));
        TEST_COMPARE(api.messageRequests, 2);
        // The second request carries the user's answer as the tool result.
        TEST_ASSERT(api.messageBodies.value(1).contains("\\\"answer\\\":\\\"yes\\\""));
    }
};

TEST_INSTANTIATE(TST_Gui);

#endif // UNITTEST_GUI_ENABLED
