#include "ToolSettingsWidget.h"
#include "Client.h"
#include "Tool.h"

#include <QCheckBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <map>

namespace QtLLM
{

ToolSettingsWidget::ToolSettingsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_page = new QWidget(scroll);
    m_layout = new QVBoxLayout(m_page);
    m_layout->setSpacing(4);
    m_layout->addStretch();
    scroll->setWidget(m_page);
    outer->addWidget(scroll);
    refresh();
}

void ToolSettingsWidget::setClient(Client* client)
{
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    m_pending.clear();
    if (m_client)
        connect(m_client, &Client::toolsChanged, this, &ToolSettingsWidget::refresh);
    refresh();
}

Client* ToolSettingsWidget::client() const
{
    return m_client;
}

void ToolSettingsWidget::setLockedToolsVisible(bool visible)
{
    m_lockedVisible = visible;
    refresh();
}

void ToolSettingsWidget::setApplyImmediately(bool immediately)
{
    m_immediate = immediately;
    if (m_immediate)
        apply();
}

void ToolSettingsWidget::apply()
{
    if (!m_client)
        return;
    // Every change on the client asks for a refresh; the list is rebuilt once
    // at the end instead of under the check box that is being clicked.
    m_applying = true;
    const QMap<QString, bool> pending = m_pending;
    m_pending.clear();
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it)
        m_client->setToolEnabledByUser(it.key(), it.value());
    m_applying = false;
}

void ToolSettingsWidget::refresh()
{
    if (m_applying)
        return;

    // Delete synchronously: deleteLater() posted before a modal dialog's
    // exec() is not processed until the dialog closes, and the removed
    // widgets would stay visible as a pile at (0,0).
    m_checks.clear();
    while (m_layout->count() > 1) {
        QLayoutItem* item = m_layout->takeAt(0);
        if (item->widget()) {
            item->widget()->hide();
            delete item->widget();
        }
        delete item;
    }

    auto addLabel = [this](const QString& text, const QString& style) {
        auto* label = new QLabel(text, m_page);
        label->setWordWrap(true);
        label->setStyleSheet(style);
        m_layout->insertWidget(m_layout->count() - 1, label);
    };
    const QString hint = QStringLiteral("color: #888;");

    if (!m_client) {
        addLabel(tr("No client bound - call setClient() to list the registered tools here."), hint);
        return;
    }

    // Grouped by Tool::group(); tools without a group last.
    std::map<QString, QList<Tool>> groups;
    for (const Tool& tool : m_client->registeredTools()) {
        if (m_lockedVisible || m_client->isToolUserConfigurable(tool.name()))
            groups[tool.group()].append(tool);
    }
    if (groups.empty()) {
        addLabel(tr("No tools registered."), hint);
        return;
    }

    auto addGroup = [&](const QString& title, const QList<Tool>& tools) {
        addLabel(title, QStringLiteral("font-weight: bold; margin-top: 8px;"));
        for (const Tool& tool : tools) {
            const QString name = tool.name();
            const bool locked = !m_client->isToolUserConfigurable(name);
            QString text = QStringLiteral("%1  (%2)").arg(tool.title().isEmpty() ? name : tool.title(), name);
            if (locked)
                text += tr("  - set by the application");

            auto* check = new QCheckBox(text, m_page);
            check->setObjectName(QStringLiteral("qtllm_tool_") + name);
            check->setChecked(!locked && m_pending.contains(name) ? m_pending.value(name)
                                                                  : m_client->isToolEnabled(name));
            check->setEnabled(!locked);
            if (locked)
                check->setToolTip(tr("This tool is managed by the application and cannot be changed here."));
            connect(check, &QCheckBox::toggled, this, [this, name](bool enabled) {
                m_pending[name] = enabled;
                emit changed(name, enabled);
                if (m_immediate)
                    apply();
            });
            m_layout->insertWidget(m_layout->count() - 1, check);
            m_checks.insert(name, check);

            if (!tool.description().isEmpty())
                addLabel(tool.description(), QStringLiteral("color: #888; margin-left: 24px;"));
        }
    };

    for (const auto& entry : groups) {
        if (!entry.first.isEmpty())
            addGroup(entry.first, entry.second);
    }
    const auto ungrouped = groups.find(QString());
    if (ungrouped != groups.end())
        addGroup(tr("Other"), ungrouped->second);
}

} // namespace QtLLM
