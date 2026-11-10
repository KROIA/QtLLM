#pragma once
#include "QtLLM_base.h"
#include <QMap>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QVBoxLayout;

namespace QtLLM
{

class Client;

// The tools of a client, grouped, with a check box each: where the user of an
// application decides which tools the assistant may use. The Settings dialog's
// "Tools" tab is one of these; an application with a settings window of its
// own embeds another.
//
// What the user can do here is limited by the application: a tool it locked
// with Client::setToolUserConfigurable(name, false) (or a whole group with
// setToolGroupUserConfigurable) is shown with its state but cannot be
// switched - or is left out with setLockedToolsVisible(false).
class QT_LLM_API ToolSettingsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ToolSettingsWidget(QWidget* parent = nullptr);

    void setClient(Client* client);
    Client* client() const;

    // Locked tools: listed greyed out (default) or not at all.
    void setLockedToolsVisible(bool visible);
    bool lockedToolsVisible() const { return m_lockedVisible; }

    // true: a click changes the client at once. false (default): the choices
    // are written by apply() - for a dialog with OK / Apply / Cancel.
    void setApplyImmediately(bool immediately);
    bool applyImmediately() const { return m_immediate; }

    // Writes the user's choices to the client (Client::setToolEnabledByUser).
    void apply();
    // Rebuilds the list from the client; choices not yet applied are kept.
    // Called automatically when the client's tools change.
    void refresh();

signals:
    // The user switched a tool (not yet applied unless applyImmediately()).
    void changed(const QString& toolName, bool enabled);

private:
    QPointer<Client>          m_client;
    QWidget*                  m_page = nullptr;
    QVBoxLayout*              m_layout = nullptr;
    QMap<QString, QCheckBox*> m_checks;     // tool name -> check box
    QMap<QString, bool>       m_pending;    // switched by the user, not applied yet
    bool                      m_lockedVisible = true;
    bool                      m_immediate = false;
    bool                      m_applying = false;
};

} // namespace QtLLM
