#include <QApplication>
#include <QMessageBox>
#include "QtLLM.h"
#include "ChatWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

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
    ChatWindow window(apiKey, baseUrl + "/v1/messages");
    window.show();

    return app.exec();
}
