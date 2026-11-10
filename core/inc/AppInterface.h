#pragma once
#include "QtLLM_base.h"
#include <QObject>
#include <QFlags>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPalette>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <functional>

class QWidget;
class QAction;
class QLayout;

namespace QtLLM
{

class Client;
class Tool;

// Lets the model inspect and operate the host application's user interface
// through "ui_*" tools, without the application writing a tool per control.
//
// Off by default: every capability registers its tools on the client only
// while it is enabled, so a disabled capability costs no tokens and cannot be
// called. Widgets are addressed by id strings (object-name paths, see
// idForWidget()). Excluded subtrees, the chat dock and widgets outside the
// registered roots are invisible to every tool; password fields are redacted.
//
// GUI thread only, like the tool handlers that call into it.
class QT_LLM_API AppInterface : public QObject
{
    Q_OBJECT
public:
    enum class Capability {
        ReadActions      = 1 << 0,   // ui_list_actions
        TriggerActions   = 1 << 1,   // ui_trigger_action
        ReadWidgets      = 1 << 2,   // ui_get_widget_tree, ui_find_widgets, ui_get_widget
        ReadProperties   = 1 << 3,   // ui_get_properties
        WriteProperties  = 1 << 4,   // ui_set_property
        ReadLayouts      = 1 << 5,   // ui_get_layout
        ModifyLayouts    = 1 << 6,   // ui_modify_layout, ui_undo_layout_change
        Screenshots      = 1 << 7,   // ui_screenshot
        Highlights       = 1 << 8,   // ui_highlight_widget
        Shapes           = 1 << 9,   // ui_draw_shape
        Animations       = 1 << 10,  // animation parameters accepted by the two above
        SpeechBubbles    = 1 << 11,  // ui_show_bubble
        Tours            = 1 << 12,  // ui_show_tour
        WidgetColors     = 1 << 13,  // ui_set_widget_colors, ui_restore_widget_colors
        Themes           = 1 << 14,  // ui_list_themes, ui_set_theme
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    AppInterface(Client* client, QWidget* rootWindow, QObject* parent = nullptr);
    ~AppInterface() override;

    void setCapabilityEnabled(Capability capability, bool enabled);
    bool isCapabilityEnabled(Capability capability) const;
    void setCapabilities(Capabilities capabilities);
    Capabilities capabilities() const { return m_capabilities; }

    // Further top-level windows (dialogs without a parent, tool windows).
    void addRoot(QWidget* window);
    // The widget and everything below it are hidden from every tool.
    void excludeWidget(QWidget* widget);
    // Property names ui_set_property may write. Default: text, value, checked,
    // currentIndex, currentText, enabled, visible, toolTip.
    void setWritableProperties(const QStringList& names);
    QStringList writableProperties() const { return m_writable; }
    // Reverts every layout modification made through ui_modify_layout, e.g.
    // when the conversation is cleared.
    void resetLayoutChanges();

    // Puts back the colours of every widget changed through
    // ui_set_widget_colors, e.g. when the conversation is cleared.
    void resetWidgetColors();

    // Themes (capability Themes). The model can only switch between what the
    // application registers here; it cannot define colours of its own.
    struct Theme
    {
        QString               name;          // what the model passes to ui_set_theme
        QString               description;   // one line, shown to the model
        std::function<void()> apply;
    };
    // `apply` does whatever switching to the theme means in this application.
    // Registering a name again replaces the theme.
    void registerTheme(const QString& name, const QString& description,
                       std::function<void()> apply);
    // The common case: an application-wide palette, optionally with a QStyle
    // (e.g. "Fusion") and an application style sheet.
    void registerTheme(const QString& name, const QString& description, const QPalette& palette,
                       const QString& styleName = QString(),
                       const QString& styleSheet = QString());
    // Registers "default" (the look the application has at this moment) and
    // "dark" (Fusion style with darkPalette()). Call it before changing the look.
    void registerStandardThemes();
    static QPalette darkPalette();
    QStringList themeNames() const;
    // Empty until a theme was applied through setTheme() / ui_set_theme.
    QString currentTheme() const { return m_currentTheme; }
    // False for a name that is not registered.
    bool setTheme(const QString& name);

    // Helpers, also used by the guidance tools.
    Client* client() const;
    QList<QWidget*> roots() const;
    // nullptr for unknown, stale, excluded or out-of-root ids.
    QWidget* widgetForId(const QString& id) const;
    // Empty for widgets outside the roots or excluded.
    QString idForWidget(QWidget* widget) const;
    bool isExcluded(QWidget* widget) const;

    // Tool logic. The registered handlers forward to these; each takes the
    // tool arguments and returns the tool result.
    QJsonObject listActions(const QJsonObject& args);
    QJsonObject triggerAction(const QJsonObject& args);
    QJsonObject getWidgetTree(const QJsonObject& args);
    QJsonObject findWidgets(const QJsonObject& args);
    QJsonObject getWidget(const QJsonObject& args);
    QJsonObject getProperties(const QJsonObject& args);
    // Not called setProperty: that would hide QObject::setProperty.
    QJsonObject setWidgetProperty(const QJsonObject& args);
    QJsonObject getLayout(const QJsonObject& args);
    QJsonObject modifyLayout(const QJsonObject& args);
    QJsonObject undoLayoutChange(const QJsonObject& args);
    QJsonObject screenshot(const QJsonObject& args);
    QJsonObject setWidgetColors(const QJsonObject& args);
    QJsonObject restoreWidgetColors(const QJsonObject& args);
    QJsonObject listThemes(const QJsonObject& args);
    QJsonObject applyTheme(const QJsonObject& args);

    // Guidance tools (capabilities Highlights, Shapes, Animations,
    // SpeechBubbles, Tours): the model draws on top of the application to show
    // the user where something is. Nothing in the application is changed.
    QJsonObject highlightWidget(const QJsonObject& args);
    QJsonObject drawShape(const QJsonObject& args);
    QJsonObject removeOverlay(const QJsonObject& args);
    QJsonObject clearOverlays(const QJsonObject& args);
    QJsonObject showBubble(const QJsonObject& args);
    QJsonObject showTour(const QJsonObject& args);
    QJsonObject closeBubble(const QJsonObject& args);
    QJsonObject clearBubbles(const QJsonObject& args);
    QJsonObject listGuidance(const QJsonObject& args);

    // Remove every highlight, shape, bubble and tour - e.g. when the
    // conversation they belonged to is cleared.
    void clearGuidance();
    // Off: every overlay is shown in its static end state (reduced motion).
    // Applies to all overlays of the application.
    void setAnimationsEnabled(bool enabled);

signals:
    void themeChanged(const QString& name);

private:
    struct ToolEntry;
    struct ActionEntry;

    QList<ToolEntry> toolsFor(Capability capability);
    void registerCapability(Capability capability);
    void unregisterCapability(Capability capability);
    // Registers/unregisters the tools of the guidance capabilities
    // (Highlights ... Tours); filled in by the visual guidance feature.
    void syncGuidanceTools();
    QJsonObject addOverlay(QWidget* window, QWidget* anchor, QJsonObject spec);

    QString segmentFor(QWidget* widget) const;
    bool excludedItself(QWidget* widget) const;
    // Empty object when found, otherwise the error result to return.
    QJsonObject resolve(const QString& id, QWidget*& widget) const;
    QList<ActionEntry> collectActions() const;
    QJsonObject treeNode(QWidget* widget, const QString& id, int depth,
                         bool visibleOnly, int& budget) const;
    QJsonObject layoutInfo(QLayout* layout, const QString& address) const;
    QLayout* resolveLayout(const QJsonObject& args, QJsonObject& error) const;

    QPointer<Client>               m_client;
    QList<QPointer<QWidget>>       m_roots;
    QList<QPointer<QWidget>>       m_excluded;
    QStringList                    m_writable;
    Capabilities                   m_capabilities;
    QList<std::function<void()>>   m_layoutUndo;
    QStringList                    m_guidanceTools;   // names registered by syncGuidanceTools()
    // Widgets recoloured by ui_set_widget_colors; the original style sheet and
    // the colours in effect are kept on the widget as dynamic properties.
    QList<QPointer<QWidget>>       m_colored;
    int                            m_colorSerial = 0;
    QList<Theme>                   m_themes;
    QString                        m_currentTheme;
    // Ids handed out, so a later call can tell "deleted" from "never existed"
    // and does not silently hit another widget that moved into the same slot.
    mutable QHash<QString, QPointer<QWidget>> m_ids;
};

} // namespace QtLLM

Q_DECLARE_OPERATORS_FOR_FLAGS(QtLLM::AppInterface::Capabilities)
