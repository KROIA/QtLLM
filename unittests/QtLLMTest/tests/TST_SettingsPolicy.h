#pragma once

// What an application lets its users change: which providers are offered,
// whether an endpoint is fixed, which models may be used, and single fields
// of the settings dialog.

#include "UnitTest.h"
#include "QtLLM.h"
#include "FakeApi.h"
#include "FakeOpenAi.h"
#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <functional>

class TST_SettingsPolicy : public UnitTest::Test
{
    TEST_CLASS(TST_SettingsPolicy)
public:
    TST_SettingsPolicy()
        : Test("TST_SettingsPolicy")
    {
        ADD_TEST(TST_SettingsPolicy::testModelPatterns);
        ADD_TEST(TST_SettingsPolicy::testClientRefusesModelsThatAreNotAllowed);
        ADD_TEST(TST_SettingsPolicy::testClientListsAndPicksOnlyAllowedModels);
        ADD_TEST(TST_SettingsPolicy::testOfferedProviders);
        ADD_TEST(TST_SettingsPolicy::testFixedEndpoint);
        ADD_TEST(TST_SettingsPolicy::testModelWhitelistInDialog);
        ADD_TEST(TST_SettingsPolicy::testBlockedModelIsNotApplied);
        ADD_TEST(TST_SettingsPolicy::testFixedModel);
        ADD_TEST(TST_SettingsPolicy::testApplyHandsTheRulesToTheClient);
        ADD_TEST(TST_SettingsPolicy::testLockedAndHiddenFields);
    }

private:
    using Dialog = QtLLM::SettingsDialog;
    using P = QtLLM::SettingsDialog::Provider;

    static bool spin(const std::function<bool()>& condition, int timeoutMs = 5000)
    {
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            if (condition())
                loop.quit();
        });
        poll.start(10);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        if (!condition())
            loop.exec();
        return condition();
    }

    template <typename T>
    static T* child(Dialog& dialog, const char* name) { return dialog.findChild<T*>(name); }

    static QStringList items(QComboBox* combo)
    {
        QStringList texts;
        for (int i = 0; i < combo->count(); ++i)
            texts.append(combo->itemText(i));
        return texts;
    }

    // ------------------------------------------------------------ client

    TEST_FUNCTION(testModelPatterns)
    {
        TEST_START;

        using C = QtLLM::Client;
        TEST_ASSERT(C::modelMatches("claude-sonnet-5-5", {"claude-sonnet-*"}));
        TEST_ASSERT(C::modelMatches("Claude-Sonnet-5-5", {"claude-sonnet-*"}));        // case does not matter
        TEST_ASSERT(!C::modelMatches("claude-opus-5-5", {"claude-sonnet-*", "claude-haiku-*"}));
        TEST_ASSERT(C::modelMatches("llama3.2:latest", {"llama3.2:latest"}));
        TEST_ASSERT(!C::modelMatches("llama3x2:latest", {"llama3.2:latest"}));         // a dot is a dot
        TEST_ASSERT(C::modelMatches("gpt-oss:120b", {"*:120b"}));
        TEST_ASSERT(C::modelMatches("hf.co/someone/model:q4", {"hf.co/*"}));           // * crosses a slash
        TEST_ASSERT(C::modelMatches("model-a", {"model-?"}));
        TEST_ASSERT(!C::modelMatches("model-ab", {"model-?"}));
        TEST_ASSERT(!C::modelMatches("anything", {}));
    }

    TEST_FUNCTION(testClientRefusesModelsThatAreNotAllowed)
    {
        TEST_START;

        FakeApi api;
        api.replies.enqueue(FakeApi::textReply("fine"));

        QtLLM::Client client("key", api.url());
        client.setModel("claude-haiku-5-5");
        client.setModelFilter({"claude-sonnet-*", "claude-haiku-*"}, {"*-preview"});
        TEST_ASSERT(client.isModelAllowed("claude-sonnet-5-5"));
        TEST_ASSERT(!client.isModelAllowed("claude-opus-5-5"));       // not on the whitelist
        TEST_ASSERT(!client.isModelAllowed("claude-sonnet-6-preview")); // blacklist wins

        // A model that is not allowed is not taken over - from code, a saved
        // setting or a dialog alike.
        client.setModel("claude-opus-5-5");
        TEST_COMPARE(client.model(), QString("claude-haiku-5-5"));
        client.setModel("claude-sonnet-5-5");
        TEST_COMPARE(client.model(), QString("claude-sonnet-5-5"));

        // The rules change while a model that is now forbidden is in use:
        // nothing is sent with it.
        client.setModelFilter({"claude-haiku-*"});
        QStringList errors;
        QObject::connect(&client, &QtLLM::Client::errorOccurred, [&](const QString& m) { errors.append(m); });
        client.sendPrompt("hello");
        TEST_COMPARE(errors.size(), 1);
        TEST_ASSERT(errors.value(0).contains("claude-sonnet-5-5"));
        TEST_COMPARE(api.messageRequests, 0);
        TEST_ASSERT(client.conversationHistory().isEmpty());

        client.setModel("claude-haiku-5-5");
        QString answer;
        QObject::connect(&client, &QtLLM::Client::responseReady, [&](const QString& t) { answer = t; });
        client.sendPrompt("hello");
        TEST_ASSERT(spin([&]() { return !answer.isEmpty(); }));
        TEST_COMPARE(api.messageRequests, 1);
    }

    // The provider's list reaches the application filtered, and the automatic
    // choice of a model stays inside it. (The fake server offers m1 and m2.)
    TEST_FUNCTION(testClientListsAndPicksOnlyAllowedModels)
    {
        TEST_START;

        FakeOpenAi server;
        QtLLM::Client client(QtLLM::Provider::OpenAI, server.baseUrl());
        client.setModelFilter({}, {"m1"});
        QStringList listed;
        bool gotList = false;
        QObject::connect(&client, &QtLLM::Client::modelsAvailable, [&](const QStringList& models) {
            listed = models;
            gotList = true;
        });
        client.fetchAvailableModels();
        TEST_ASSERT(spin([&]() { return gotList && client.model() == "m2"; }));
        TEST_COMPARE(listed, QStringList{"m2"});
        TEST_COMPARE(client.model(), QString("m2"));
    }

    // ------------------------------------------------------------ dialog

    TEST_FUNCTION(testOfferedProviders)
    {
        TEST_START;

        Dialog dialog;
        auto* combo = child<QComboBox>(dialog, "qtllm_providerCombo");
        TEST_COMPARE(combo->count(), 5);

        Dialog::ProviderPolicy off;
        off.offered = false;
        dialog.setProviderPolicy(P::Claude, off);
        dialog.setProviderPolicy(P::OpenAICompatible, off);
        TEST_COMPARE(combo->count(), 3);
        TEST_ASSERT(combo->findData(int(P::Claude)) < 0);
        // The one that was selected is gone: the selection moved on.
        TEST_ASSERT(dialog.provider() == P::Ollama);
        TEST_ASSERT(combo->isEnabled());

        // Exactly one: nothing to choose, the box is locked.
        dialog.setAvailableProviders({P::Foundry});
        TEST_COMPARE(combo->count(), 1);
        TEST_ASSERT(dialog.provider() == P::Foundry);
        TEST_ASSERT(!combo->isEnabled());
        TEST_ASSERT(!dialog.providerPolicy(P::Ollama).offered);

        // setProvider() cannot select what is not offered.
        dialog.setProvider(P::Ollama);
        TEST_ASSERT(dialog.provider() == P::Foundry);
    }

    TEST_FUNCTION(testFixedEndpoint)
    {
        TEST_START;

        Dialog dialog;
        dialog.setFoundryEndpointUrl("https://corp.example/anthropic");
        dialog.setFoundryApiKey("app-secret");
        Dialog::ProviderPolicy fixed;
        fixed.endpointEditable = false;
        dialog.setProviderPolicy(P::Foundry, fixed);

        auto* url = child<QLineEdit>(dialog, "qtllm_foundryUrlEdit");
        auto* key = child<QLineEdit>(dialog, "qtllm_foundryKeyEdit");
        dialog.setProvider(P::Foundry);
        TEST_ASSERT(!url->isHidden());
        TEST_ASSERT(!url->isEnabled());          // shown, not changeable
        TEST_ASSERT(key->isHidden());            // the application's key is not shown
        // The values are still the application's and still get applied.
        TEST_ASSERT(dialog.foundryEndpointUrl().startsWith("https://corp.example/anthropic"));
        TEST_COMPARE(dialog.foundryApiKey(), QString("app-secret"));
        // Set QTLLM_TEST_GRABS to a folder to get a picture of what was just checked.
        if (qEnvironmentVariableIsSet("QTLLM_TEST_GRABS")) {
            fixed.allowedModels = QStringList{"claude-sonnet-5-5", "claude-haiku-5-5"};
            dialog.setProviderPolicy(P::Foundry, fixed);
            dialog.setFieldEditable(Dialog::Field::SystemPrompt, false);
            dialog.setSystemPrompt("You are the assistant of this application.");
            dialog.resize(560, 520);
            dialog.grab().save(qEnvironmentVariable("QTLLM_TEST_GRABS") + "/settings_policy.png");
            fixed.allowedModels.clear();
            dialog.setProviderPolicy(P::Foundry, fixed);
            dialog.setFieldEditable(Dialog::Field::SystemPrompt, true);
        }

        // Another provider is the user's own business.
        dialog.setProvider(P::Claude);
        TEST_ASSERT(child<QLineEdit>(dialog, "qtllm_endpointUrlEdit")->isEnabled());
        TEST_ASSERT(!child<QLineEdit>(dialog, "qtllm_apiKeyEdit")->isHidden());
    }

    TEST_FUNCTION(testModelWhitelistInDialog)
    {
        TEST_START;

        Dialog dialog;
        auto* combo = child<QComboBox>(dialog, "qtllm_modelCombo");
        TEST_ASSERT(combo->isEditable());

        Dialog::ProviderPolicy policy;
        policy.allowedModels = QStringList{"claude-sonnet-*", "claude-haiku-5-5"};
        policy.blockedModels = QStringList{"*-preview"};
        dialog.setProviderPolicy(P::Claude, policy);
        dialog.setProvider(P::Claude);

        // With a whitelist the user picks; a name without wildcard is there
        // before the provider has listed anything.
        TEST_ASSERT(!combo->isEditable());
        TEST_ASSERT(items(combo).contains("claude-haiku-5-5"));
        TEST_ASSERT(dialog.isModelAllowed(P::Claude, dialog.model()));

        // What the provider lists is filtered.
        dialog.setAvailableModels({"claude-opus-5-5", "claude-sonnet-5-5", "claude-sonnet-6-preview",
                                   "claude-haiku-5-5"});
        QStringList offered = items(combo);
        offered.sort();
        TEST_COMPARE(offered, QStringList({"claude-haiku-5-5", "claude-sonnet-5-5"}));

        // A saved model that is no longer allowed is not put back.
        dialog.setModel("claude-sonnet-5-5");
        dialog.setModel("claude-opus-5-5");
        TEST_COMPARE(dialog.model(), QString("claude-sonnet-5-5"));

        // Other providers are not affected.
        dialog.setProvider(P::Ollama);
        TEST_ASSERT(dialog.isModelAllowed(P::Ollama, "claude-opus-5-5"));
    }

    // Blacklist only: the user may type - and Apply refuses a blocked name.
    TEST_FUNCTION(testBlockedModelIsNotApplied)
    {
        TEST_START;

        Dialog dialog;
        Dialog::ProviderPolicy policy;
        policy.blockedModels = QStringList{"*:120b"};
        dialog.setProviderPolicy(P::Ollama, policy);
        dialog.setProvider(P::Ollama);
        auto* combo = child<QComboBox>(dialog, "qtllm_modelCombo");
        auto* hint  = child<QLabel>(dialog, "qtllm_modelHint");
        TEST_ASSERT(combo->isEditable());

        int applied = 0;
        QObject::connect(&dialog, &Dialog::settingsApplied, [&]() { ++applied; });

        combo->setEditText("gpt-oss:120b");
        child<QPushButton>(dialog, "qtllm_applyButton")->click();
        TEST_COMPARE(applied, 0);
        TEST_ASSERT(!hint->isHidden());
        TEST_ASSERT(hint->text().contains("gpt-oss:120b"));

        combo->setEditText("llama3.2:latest");
        child<QPushButton>(dialog, "qtllm_applyButton")->click();
        TEST_COMPARE(applied, 1);
        TEST_ASSERT(hint->isHidden());
    }

    TEST_FUNCTION(testFixedModel)
    {
        TEST_START;

        Dialog dialog;
        dialog.setProvider(P::Foundry);
        dialog.setModel("claude-sonnet-5-5");
        Dialog::ProviderPolicy policy;
        policy.modelEditable = false;
        dialog.setProviderPolicy(P::Foundry, policy);

        auto* combo = child<QComboBox>(dialog, "qtllm_modelCombo");
        TEST_ASSERT(!combo->isEnabled());
        TEST_ASSERT(!child<QPushButton>(dialog, "qtllm_detectModelsButton")->isEnabled());
        // The provider's list does not move it either.
        dialog.setAvailableModels({"claude-haiku-5-5"});
        TEST_COMPARE(dialog.model(), QString("claude-sonnet-5-5"));
    }

    TEST_FUNCTION(testApplyHandsTheRulesToTheClient)
    {
        TEST_START;

        QtLLM::Client client(QtLLM::Provider::Ollama, "http://127.0.0.1:9");
        Dialog dialog;
        Dialog::ProviderPolicy policy;
        policy.blockedModels = QStringList{"*:120b"};
        dialog.setProviderPolicy(P::Ollama, policy);
        dialog.setProvider(P::Ollama);
        dialog.setModel("llama3.2:latest");
        dialog.applyTo(&client);

        TEST_COMPARE(client.model(), QString("llama3.2:latest"));
        TEST_ASSERT(!client.isModelAllowed("gpt-oss:120b"));
        client.setModel("gpt-oss:120b");                     // e.g. from the application's own model box
        TEST_COMPARE(client.model(), QString("llama3.2:latest"));
    }

    TEST_FUNCTION(testLockedAndHiddenFields)
    {
        TEST_START;

        Dialog dialog;
        using F = Dialog::Field;
        auto* prompt = child<QTextEdit>(dialog, "qtllm_systemPromptEdit");
        auto* effort = child<QComboBox>(dialog, "qtllm_effortCombo");
        auto* font   = child<QSpinBox>(dialog, "qtllm_fontSizeSpin");
        auto* debug  = child<QCheckBox>(dialog, "qtllm_showToolCallsCheck");
        auto* combo  = child<QComboBox>(dialog, "qtllm_providerCombo");

        dialog.setSystemPrompt("You are the assistant of this application.");
        dialog.setFieldEditable(F::SystemPrompt, false);
        dialog.setFieldEditable(F::Effort, false);
        dialog.setFieldEditable(F::Provider, false);
        dialog.setFieldVisible(F::ShowToolCalls, false);
        dialog.setFieldVisible(F::FontSize, false);

        TEST_ASSERT(prompt->isReadOnly());           // readable, not changeable
        TEST_ASSERT(!prompt->isHidden());
        TEST_ASSERT(!effort->isEnabled());
        TEST_ASSERT(!combo->isEnabled());
        TEST_ASSERT(debug->isHidden());
        TEST_ASSERT(font->isHidden());

        // Still so after the provider changed from code.
        dialog.setProvider(P::Ollama);
        TEST_ASSERT(dialog.provider() == P::Ollama);
        TEST_ASSERT(!combo->isEnabled());
        TEST_ASSERT(debug->isHidden());

        dialog.setFieldEditable(F::SystemPrompt, true);
        dialog.setFieldVisible(F::FontSize, true);
        TEST_ASSERT(!prompt->isReadOnly());
        TEST_ASSERT(!font->isHidden());
    }
};

TEST_INSTANTIATE(TST_SettingsPolicy);
