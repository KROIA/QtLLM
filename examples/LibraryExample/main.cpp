#include <QApplication>
#include <QMessageBox>
#include "QtLLM.h"
#include "ChatWindow.h"

#define DEMO_ANTHROPIC_FOUNDRY 1
#define DEMO_OLLAMA 2

#define USED_DEMO DEMO_ANTHROPIC_FOUNDRY

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
	QtLLM::Profiler::start();
    Log::UI::createConsoleView(Log::UI::ConsoleViewType::nativeConsoleView);

#if USED_DEMO == DEMO_ANTHROPIC_FOUNDRY
    QString apiKey  = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_API_KEY"));
    QString baseUrl = QString::fromUtf8(qgetenv("ANTHROPIC_FOUNDRY_BASE_URL"));

    if (apiKey.isEmpty() || baseUrl.isEmpty()) {
        QMessageBox::critical(nullptr,
            "Missing Configuration",
            "Please set the following environment variables before launching:\n\n"
            "  ANTHROPIC_FOUNDRY_API_KEY\n"
            "  ANTHROPIC_FOUNDRY_BASE_URL");
        return 1;
    }

    // ANTHROPIC_FOUNDRY_BASE_URL is a base URL — append the messages endpoint path
   // ChatWindow window(apiKey, baseUrl + "/v1/messages", "claude-haiku-4-5");
    ChatWindow window(apiKey, baseUrl + "/v1/messages", "claude-sonnet-4-6");
#elif USED_DEMO == DEMO_OLLAMA
    ChatWindow window(QtLLM::Provider::Ollama, "", "http://localhost:11434/api/chat", "gpt-oss:20b");
#endif
    window.show();

    int ret = app.exec();
    QtLLM::Profiler::stop("LibraryExample.prof");
    return ret;
}
