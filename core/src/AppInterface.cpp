#include "AppInterface.h"
#include "ChatDockWidget.h"
#include "Client.h"
#include "GuideOverlay.h"
#include "SpeechBubble.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaEnum>
#include <QMetaProperty>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRect>
#include <QSet>
#include <QSpacerItem>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>

namespace QtLLM
{

struct AppInterface::ToolEntry
{
    Tool        tool;
    ToolHandler handler;
};

struct AppInterface::ActionEntry
{
    QString           id;
    QString           menu;
    QPointer<QAction> action;
};

namespace
{

const int kMaxTreeNodes   = 300;
const int kMaxListItems   = 200;
const int kMaxFindResults = 50;
const int kMaxMarkers     = 150;
const int kTreeTextChars  = 80;
const int kMaxActionLog   = 500;
// Dynamic property holding the id segment of a window found by window
// discovery, so the id stays the same while the window lives.
const char* const kWindowIdProperty = "qtllm_windowId";
const int kDetailTextChars = 2000;

QString redacted() { return QStringLiteral("[redacted]"); }

bool isPassword(const QWidget* widget)
{
    const auto* edit = qobject_cast<const QLineEdit*>(widget);
    return edit && edit->echoMode() != QLineEdit::Normal;
}

// Properties of a QLineEdit that would reveal what was typed into it.
bool isSecretProperty(const QString& name)
{
    return name == QLatin1String("text") || name == QLatin1String("displayText")
        || name == QLatin1String("selectedText");
}

QString elide(const QString& text, int maxChars)
{
    return text.size() > maxChars ? text.left(maxChars - 1) + QChar(0x2026) : text;
}

QString stripMnemonic(QString text)
{
    text.replace(QLatin1String("&&"), QString(QChar(1)));
    text.remove(QLatin1Char('&'));
    text.replace(QChar(1), QLatin1Char('&'));
    return text;
}

// What the user reads on the widget; password fields are already redacted.
QString widgetText(QWidget* widget)
{
    if (isPassword(widget))
        return redacted();
    if (auto* button = qobject_cast<QAbstractButton*>(widget))
        return stripMnemonic(button->text());
    if (auto* label = qobject_cast<QLabel*>(widget))
        return label->text();
    if (auto* edit = qobject_cast<QLineEdit*>(widget))
        return edit->text();
    if (auto* combo = qobject_cast<QComboBox*>(widget))
        return combo->currentText();
    if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget))
        return spin->text();
    if (auto* group = qobject_cast<QGroupBox*>(widget))
        return group->title();
    if (auto* textEdit = qobject_cast<QTextEdit*>(widget))
        return textEdit->toPlainText();
    if (auto* plain = qobject_cast<QPlainTextEdit*>(widget))
        return plain->toPlainText();
    if (auto* menu = qobject_cast<QMenu*>(widget))
        return stripMnemonic(menu->title());
    if (widget->isWindow() || qobject_cast<QDockWidget*>(widget))
        return widget->windowTitle();
    return QString();
}

// Widgets a user clicks or types into; their children (a combo's line edit, a
// view's scroll bars) are part of them and get no marker of their own.
bool isInteractive(QWidget* widget)
{
    return qobject_cast<QAbstractButton*>(widget) || qobject_cast<QLineEdit*>(widget)
        || qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
        || qobject_cast<QAbstractSlider*>(widget) || qobject_cast<QTabBar*>(widget)
        || qobject_cast<QAbstractItemView*>(widget) || qobject_cast<QTextEdit*>(widget)
        || qobject_cast<QPlainTextEdit*>(widget);
}

QList<QWidget*> childWidgets(QWidget* parent)
{
    QList<QWidget*> result;
    for (QObject* child : parent->children()) {
        if (child->isWidgetType())
            result.append(static_cast<QWidget*>(child));
    }
    return result;
}

QString cleanName(const QWidget* widget)
{
    QString name = widget->objectName();
    return name.replace(QLatin1Char('/'), QLatin1Char('_'));
}

// One id step: the object name (with #n only when siblings share it), or
// ClassName#n counting the unnamed siblings of the same class.
QString segmentFrom(QWidget* widget, const QList<QWidget*>& siblings)
{
    const QString name = cleanName(widget);
    int index = 0;
    int count = 0;
    for (QWidget* sibling : siblings) {
        const bool same = name.isEmpty()
            ? cleanName(sibling).isEmpty() && sibling->metaObject() == widget->metaObject()
            : cleanName(sibling) == name;
        if (!same)
            continue;
        if (sibling == widget)
            index = count;
        ++count;
    }
    if (!name.isEmpty())
        return count > 1 ? name + QLatin1Char('#') + QString::number(index) : name;
    return QString::fromLatin1(widget->metaObject()->className())
        + QLatin1Char('#') + QString::number(index);
}

QJsonArray rectArray(const QRect& rect)
{
    return QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()};
}

QJsonValue propertyToJson(const QMetaProperty& property, const QVariant& value)
{
    if (!value.isValid())
        return QJsonValue();
    if (property.isEnumType()) {
        const QMetaEnum metaEnum = property.enumerator();
        const int raw = value.toInt();
        const QByteArray keys = metaEnum.isFlag() ? metaEnum.valueToKeys(raw)
                                                  : QByteArray(metaEnum.valueToKey(raw));
        return keys.isEmpty() ? QJsonValue(raw) : QJsonValue(QString::fromLatin1(keys));
    }
    switch (value.userType()) {
    case QMetaType::QRect:   return rectArray(value.toRect());
    case QMetaType::QRectF:  return rectArray(value.toRectF().toRect());
    case QMetaType::QSize:   return QJsonArray{value.toSize().width(), value.toSize().height()};
    case QMetaType::QSizeF:  return QJsonArray{value.toSizeF().width(), value.toSizeF().height()};
    case QMetaType::QPoint:  return QJsonArray{value.toPoint().x(), value.toPoint().y()};
    case QMetaType::QPointF: return QJsonArray{value.toPointF().x(), value.toPointF().y()};
    case QMetaType::QColor:  return value.value<QColor>().name(QColor::HexArgb);
    case QMetaType::QFont:   return value.value<QFont>().toString();
    case QMetaType::QKeySequence:
        return value.value<QKeySequence>().toString(QKeySequence::PortableText);
    default:
        break;
    }
    const QJsonValue json = QJsonValue::fromVariant(value);
    if (!json.isNull() || value.isNull())
        return json;
    if (value.canConvert<QString>())
        return value.toString();
    return QJsonValue(QJsonValue::Undefined);
}

bool convertVariant(QVariant& value, int type)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.convert(QMetaType(type));
#else
    return value.convert(type);
#endif
}

const QList<QPair<QString, Qt::Alignment>>& alignmentNames()
{
    static const QList<QPair<QString, Qt::Alignment>> names{
        {QStringLiteral("left"),    Qt::AlignLeft},
        {QStringLiteral("right"),   Qt::AlignRight},
        {QStringLiteral("hcenter"), Qt::AlignHCenter},
        {QStringLiteral("justify"), Qt::AlignJustify},
        {QStringLiteral("top"),     Qt::AlignTop},
        {QStringLiteral("bottom"),  Qt::AlignBottom},
        {QStringLiteral("vcenter"), Qt::AlignVCenter},
    };
    return names;
}

QString alignmentToString(Qt::Alignment alignment)
{
    QStringList parts;
    for (const auto& entry : alignmentNames()) {
        if (alignment & entry.second)
            parts.append(entry.first);
    }
    return parts.join(QLatin1Char('|'));
}

bool alignmentFromString(const QString& text, Qt::Alignment& alignment)
{
    alignment = Qt::Alignment();
    for (const QString& part : text.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        const QString key = part.trimmed().toLower();
        if (key == QLatin1String("none"))
            continue;
        if (key == QLatin1String("center")) {
            alignment |= Qt::AlignCenter;
            continue;
        }
        bool found = false;
        for (const auto& entry : alignmentNames()) {
            if (entry.first == key) {
                alignment |= entry.second;
                found = true;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

void moveBoxItem(QBoxLayout* box, int from, int to)
{
    const int stretch = box->stretch(from);
    QLayoutItem* item = box->takeAt(from);
    // takeAt() detaches a nested layout from its parent; insertLayout() adopts it again.
    if (QLayout* nested = item->layout())
        box->insertLayout(to, nested, stretch);
    else
        box->insertItem(to, item);
    box->setStretch(to, stretch);
}

const char* const kIdHelp =
    "Widgets are addressed by id: a '/'-separated path starting at the window, made of "
    "object names where the application set them and ClassName#n (the n-th unnamed child "
    "of that class, counting from 0) otherwise, e.g. 'mainWindow/central/saveButton' or "
    "'mainWindow/QWidget#0/QPushButton#2'. Take ids from ui_get_widget_tree or "
    "ui_find_widgets instead of guessing; they stay valid while the window's widgets do "
    "not change.";

} // namespace

namespace
{

const int kLastCapabilityBit = 19;   // Capability::WaitForUi

using Capability = AppInterface::Capability;

bool isInputCapability(Capability capability)
{
    return capability == Capability::SimulateInput || capability == Capability::WaitForUi;
}

bool isItemCapability(Capability capability)
{
    return capability == Capability::ReadItems || capability == Capability::SelectItems
        || capability == Capability::EditItems;
}

// Highlights ... Tours share tools, so they are registered as a set
// (syncGuidanceTools) instead of one capability at a time.
bool isGuidanceCapability(AppInterface::Capability capability)
{
    return static_cast<int>(capability) >= static_cast<int>(AppInterface::Capability::Highlights)
        && static_cast<int>(capability) <= static_cast<int>(AppInterface::Capability::Tours);
}

} // namespace

// ---------------------------------------------------------------- setup

AppInterface::AppInterface(Client* client, QWidget* rootWindow, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_writable{QStringLiteral("text"), QStringLiteral("value"), QStringLiteral("checked"),
                 QStringLiteral("currentIndex"), QStringLiteral("currentText"),
                 QStringLiteral("enabled"), QStringLiteral("visible"), QStringLiteral("toolTip")}
{
    addRoot(rootWindow);
    if (client) {
        connect(client, &Client::toolInvoked, this, &AppInterface::onToolInvoked);
        connect(client, &Client::toolCompleted, this, &AppInterface::onToolCompleted);
        // A cancelled turn never completes its calls; nothing is pending after a turn.
        connect(client, &Client::requestFinished, this, [this]() { m_pendingActions.clear(); });
    }
}

AppInterface::~AppInterface()
{
    if (!m_client)
        return;
    const Capabilities enabled = m_capabilities;
    m_capabilities = Capabilities();
    for (int bit = 0; bit <= kLastCapabilityBit; ++bit) {
        const Capability capability = static_cast<Capability>(1 << bit);
        if (enabled.testFlag(capability) && !isGuidanceCapability(capability))
            unregisterCapability(capability);
    }
    syncGuidanceTools();
    syncInputTools();
    syncItemTools();
}

void AppInterface::setCapabilityEnabled(Capability capability, bool enabled)
{
    if (m_capabilities.testFlag(capability) == enabled)
        return;
    m_capabilities.setFlag(capability, enabled);
    if (isGuidanceCapability(capability))
        syncGuidanceTools();
    else if (isInputCapability(capability))
        syncInputTools();
    else if (isItemCapability(capability))
        syncItemTools();
    else if (enabled)
        registerCapability(capability);
    else
        unregisterCapability(capability);
}

bool AppInterface::isCapabilityEnabled(Capability capability) const
{
    return m_capabilities.testFlag(capability);
}

void AppInterface::setCapabilities(Capabilities capabilities)
{
    for (int bit = 0; bit <= kLastCapabilityBit; ++bit) {
        const Capability capability = static_cast<Capability>(1 << bit);
        setCapabilityEnabled(capability, capabilities.testFlag(capability));
    }
}

void AppInterface::addRoot(QWidget* window)
{
    if (window && !m_roots.contains(QPointer<QWidget>(window)))
        m_roots.append(window);
}

void AppInterface::excludeWidget(QWidget* widget)
{
    if (widget && !m_excluded.contains(QPointer<QWidget>(widget)))
        m_excluded.append(widget);
}

void AppInterface::setWindowDiscoveryEnabled(bool enabled)
{
    m_windowDiscovery = enabled;
}

void AppInterface::setWritableProperties(const QStringList& names)
{
    m_writable = names;
    // The description lists the allowed names, so it has to be refreshed.
    if (isCapabilityEnabled(Capability::WriteProperties))
        registerCapability(Capability::WriteProperties);
}

void AppInterface::resetLayoutChanges()
{
    while (!m_layoutUndo.isEmpty())
        m_layoutUndo.takeLast()();
}

void AppInterface::resetPropertyChanges()
{
    while (!m_propertyUndo.isEmpty())
        m_propertyUndo.takeLast()();
}

void AppInterface::clearActionLog()
{
    m_actionLog.clear();
}

void AppInterface::onToolInvoked(const QString& name, const QJsonObject& input)
{
    if (!m_client || !name.startsWith(QLatin1String("ui_")))
        return;
    for (const Tool& tool : m_client->registeredTools()) {
        if (tool.name() == name) {
            if (tool.isReadOnly())
                return;
            // The log is shown to people: what went into a password field
            // does not belong in it.
            QJsonObject logged = input;
            if (isPassword(widgetForId(input.value("id").toString()))) {
                for (const char* key : {"text", "value"}) {
                    if (logged.contains(QLatin1String(key)))
                        logged[QLatin1String(key)] = redacted();
                }
            }
            m_pendingActions.append(qMakePair(name, logged));
            return;
        }
    }
}

void AppInterface::onToolCompleted(const QString& name, const QJsonObject& result)
{
    for (int i = 0; i < m_pendingActions.size(); ++i) {
        if (m_pendingActions[i].first != name)
            continue;
        ActionRecord record;
        record.time = QDateTime::currentDateTime();
        record.tool = name;
        record.input = m_pendingActions.takeAt(i).second;
        record.ok = result.value("status").toString() != QLatin1String("error");
        record.message = result.value("message").toString();
        m_actionLog.append(record);
        if (m_actionLog.size() > kMaxActionLog)
            m_actionLog.removeFirst();
        emit actionPerformed(record);
        return;
    }
}

Client* AppInterface::client() const
{
    return m_client;
}

QList<QWidget*> AppInterface::roots() const
{
    QList<QWidget*> result;
    for (const QPointer<QWidget>& root : m_roots) {
        if (root && !result.contains(root.data()))
            result.append(root.data());
    }
    if (m_windowDiscovery) {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            if (isDiscoverableWindow(window) && !result.contains(window))
                result.append(window);
        }
    }
    return result;
}

bool AppInterface::isDiscoverableWindow(QWidget* widget) const
{
    if (!m_windowDiscovery || !widget->isWindow() || !widget->isVisible())
        return false;
    switch (widget->windowType()) {
    case Qt::Popup:
    case Qt::ToolTip:
    case Qt::SplashScreen:
        return false;
    default:
        break;
    }
    if (qobject_cast<GuideOverlay*>(widget) || qobject_cast<SpeechBubble*>(widget))
        return false;
    return !isExcluded(widget);
}

// ---------------------------------------------------------------- tool registration

QList<AppInterface::ToolEntry> AppInterface::toolsFor(Capability capability)
{
    const QString idHelp = QString::fromLatin1(kIdHelp);
    const QString group = QStringLiteral("App UI");
    QList<ToolEntry> entries;
    auto add = [&](Tool& tool, bool readOnly, ToolHandler handler) {
        tool.setGroup(group).setReadOnly(readOnly);
        entries.append({tool, std::move(handler)});
    };

    switch (capability) {
    case Capability::ReadActions: {
        Tool tool;
        tool.setName("ui_list_actions")
            .setTitle("List menu actions")
            .setStatusText("Reading the application's menus…")
            .setDescription(
                "List the commands of the host application's menus and toolbars. Each entry: "
                "id (pass it to ui_trigger_action), text, menu path, shortcut; 'enabled': false "
                "when it cannot be used right now; 'checked' only for toggle actions. Use this "
                "first when the user asks you to do something the application has a command for.")
            .addParameter("filter", "string",
                          "Case-insensitive text matched against id, text and menu path. "
                          "Omit to list everything.");
        add(tool, true, [this](const QJsonObject& args) { return listActions(args); });
        break;
    }
    case Capability::TriggerActions: {
        Tool tool;
        tool.setName("ui_trigger_action")
            .setTitle("Run menu action")
            .setStatusText("Running a menu command…")
            .setDescription(
                "Run a menu or toolbar command, exactly as if the user clicked it. Disabled "
                "actions are refused. The command runs right after this call returns, so a "
                "dialog it opens does not block you; inspect the UI afterwards to see the effect.")
            .addParameter("id", "string", "Action id from ui_list_actions.", true);
        add(tool, false, [this](const QJsonObject& args) { return triggerAction(args); });
        break;
    }
    case Capability::ReadWidgets: {
        Tool tree;
        tree.setName("ui_get_widget_tree")
            .setTitle("Read widget tree")
            .setStatusText("Reading the window layout…")
            .setDescription(
                "Return the widget hierarchy of the application windows as a compact tree: id, "
                "class, visible text, geometry [x,y,w,h] relative to the parent; 'enabled': false "
                "for disabled widgets. A node with 'more': n has n further children; call again "
                "with that node as 'root' to see them. " + idHelp)
            .addParameter("root", "string",
                          "Id of the widget to start at. Omit for all application windows.")
            .addParameter("depth", "integer",
                          "Levels below the root to include (default 2, max 10).")
            .addParameter("visibleOnly", "boolean",
                          "Skip hidden widgets (default true). Widgets on inactive tabs are hidden.");
        add(tree, true, [this](const QJsonObject& args) { return getWidgetTree(args); });

        Tool find;
        find.setName("ui_find_widgets")
            .setTitle("Find widgets")
            .setStatusText("Searching the user interface…")
            .setDescription(
                "Find widgets anywhere in the application windows and return their ids, class and "
                "text; hidden ones are marked 'visible': false. Give at least one criterion; all "
                "given criteria must match (case-insensitive). Faster than walking the tree when "
                "you know a label or name. " + idHelp)
            .addParameter("text", "string", "Part of the visible text (button label, field content, title).")
            .addParameter("class", "string",
                          "Qt class name or part of it, e.g. 'QPushButton', 'Button', 'QLineEdit'.")
            .addParameter("name", "string", "Part of the widget's object name.");
        add(find, true, [this](const QJsonObject& args) { return findWidgets(args); });

        Tool details;
        details.setName("ui_get_widget")
            .setTitle("Read widget")
            .setStatusText("Reading a widget…")
            .setDescription(
                "Return the details of one widget, depending on its type: full text, value and "
                "range, checked state, combo box items, tabs, tool tip, global screen geometry "
                "[x,y,w,h]. Password fields report their text as [redacted]. " + idHelp)
            .addParameter("id", "string", "Widget id.", true);
        add(details, true, [this](const QJsonObject& args) { return getWidget(args); });

        Tool windows;
        windows.setName("ui_list_windows")
            .setTitle("List windows")
            .setStatusText("Listing the application windows…")
            .setDescription(
                "List the application windows you can see: id (the first step of every widget id "
                "inside it), title, class, modal, active; 'visible': false for a hidden one. Call "
                "it after an action that may have opened a dialog. A modal window blocks all other "
                "windows until it is closed: deal with it (answer, cancel) before you go on in "
                "another window.");
        add(windows, true, [this](const QJsonObject& args) { return listWindows(args); });
        break;
    }
    case Capability::ReadProperties: {
        Tool tool;
        tool.setName("ui_get_properties")
            .setTitle("Read widget properties")
            .setStatusText("Reading widget properties…")
            .setDescription(
                "Read Qt properties (QMetaProperty) of a widget. Without 'names' you get the "
                "properties of the widget's own class plus enabled, visible and toolTip; empty "
                "values are left out. Name any QWidget property (geometry, styleSheet, font, ...) "
                "to read it. Unknown names are listed under 'unknown'. " + idHelp)
            .addParameter("id", "string", "Widget id.", true)
            .addArrayParameter("names", "string", "Property names to read.");
        add(tool, true, [this](const QJsonObject& args) { return getProperties(args); });
        break;
    }
    case Capability::WriteProperties: {
        Tool tool;
        tool.setName("ui_set_property")
            .setTitle("Change widget property")
            .setStatusText("Changing a widget…")
            .setDescription(
                "Set a Qt property of a widget, e.g. a field's text, a spin box value, a check "
                "box state or a combo box's currentIndex/currentText. Allowed property names: "
                + m_writable.join(QStringLiteral(", ")) + ". The value is converted to the "
                "property's type; enum properties take the key name. Disabled widgets refuse "
                "everything but enabled and visible. Returns the value after the write. " + idHelp)
            .addParameter("id", "string", "Widget id.", true)
            .addParameter("name", "string", "Property name.", true)
            .addRawParameter("value",
                             QJsonObject{{"description",
                                          "New value: string, number or boolean as the property needs."}},
                             true);
        add(tool, false, [this](const QJsonObject& args) { return setWidgetProperty(args); });

        Tool undo;
        undo.setName("ui_undo_property_change")
            .setTitle("Undo property change")
            .setStatusText("Undoing a widget change…")
            .setDescription("Revert the most recent ui_set_property change, putting the old value "
                            "back. Call it repeatedly to go further back. Returns how many earlier "
                            "changes remain undoable.");
        add(undo, false, [this](const QJsonObject& args) { return undoPropertyChange(args); });
        break;
    }
    case Capability::ReadLayouts: {
        Tool tool;
        tool.setName("ui_get_layout")
            .setTitle("Read layout")
            .setStatusText("Reading a layout…")
            .setDescription(
                "Describe the layout of a container widget: type, spacing, margins [l,t,r,b] and "
                "its items in order with their index, widget id, stretch, grid position or form "
                "row, alignment. Nested layouts appear inline with a 'sublayout' address to pass "
                "to ui_modify_layout. Use the id of the widget that owns the layout (the parent "
                "of the widgets you want to arrange). " + idHelp)
            .addParameter("id", "string", "Id of the widget that owns the layout.", true)
            .addParameter("sublayout", "string",
                          "Address of a nested layout as shown by an earlier call, e.g. '2' or '2/0'.");
        add(tool, true, [this](const QJsonObject& args) { return getLayout(args); });
        break;
    }
    case Capability::ModifyLayouts: {
        Tool modify;
        modify.setName("ui_modify_layout")
            .setTitle("Change layout")
            .setStatusText("Rearranging the window…")
            .setDescription(
                "Change a layout as read with ui_get_layout. Operations: set_spacing (value), "
                "set_margins (margins [l,t,r,b] or value for all four), set_stretch (item and "
                "value for box layouts; row or column and value for grid layouts), move_item "
                "(item to position 'to'; box layouts only), set_alignment (item, alignment like "
                "'left|top', 'center' or 'none'), set_visible (item, visible). Every change is "
                "recorded; ui_undo_layout_change reverts the last one.")
            .addParameter("id", "string", "Id of the widget that owns the layout.", true)
            .addParameter("sublayout", "string", "Address of a nested layout, e.g. '2/0'.")
            .addEnumParameter("operation",
                              {"set_spacing", "set_margins", "set_stretch", "move_item",
                               "set_alignment", "set_visible"},
                              "What to change.", true)
            .addParameter("item", "integer", "Item index from ui_get_layout.")
            .addParameter("value", "integer", "Spacing, margin or stretch in pixels/units.")
            .addArrayParameter("margins", "integer", "Margins [left, top, right, bottom].")
            .addParameter("to", "integer", "Target index for move_item.")
            .addParameter("row", "integer", "Grid row for set_stretch.")
            .addParameter("column", "integer", "Grid column for set_stretch.")
            .addParameter("alignment", "string",
                          "'|'-separated: left, right, hcenter, justify, top, bottom, vcenter, "
                          "center, none.")
            .addParameter("visible", "boolean", "For set_visible.");
        add(modify, false, [this](const QJsonObject& args) { return modifyLayout(args); });

        Tool undo;
        undo.setName("ui_undo_layout_change")
            .setTitle("Undo layout change")
            .setStatusText("Undoing a layout change…")
            .setDescription("Revert the most recent ui_modify_layout change. Returns how many "
                            "earlier changes remain undoable.");
        add(undo, false, [this](const QJsonObject& args) { return undoLayoutChange(args); });
        break;
    }
    case Capability::Screenshots: {
        Tool tool;
        tool.setName("ui_screenshot")
            .setTitle("Screenshot")
            .setStatusText("Taking a screenshot…")
            .setDescription(
                "Return a picture of an application window or a single widget. Prefer a widget "
                "over the whole window and a small maxEdge: images are expensive and stay in the "
                "conversation. With annotate: true every visible control carries a numbered red "
                "badge at its top-left corner and 'markers' maps each number to the widget id, so "
                "refer to what you see by number rather than by coordinates. Password fields are "
                "blacked out. " + idHelp)
            .addParameter("target", "string", "Widget id to capture. Omit for the main window.")
            .addParameter("maxEdge", "integer",
                          "Longest image edge in pixels after scaling (default 1568).")
            .addParameter("annotate", "boolean", "Draw numbered markers on the controls.");
        add(tool, true, [this](const QJsonObject& args) { return screenshot(args); });
        break;
    }
    case Capability::WidgetColors: {
        Tool set;
        set.setName("ui_set_widget_colors")
            .setTitle("Colour widget")
            .setStatusText("Changing widget colours…")
            .setDescription(
                "Change the colours of one widget: background, text and/or border. Only the "
                "colours given are changed; the widget keeps them until they are restored with "
                "ui_restore_widget_colors, so restore them when they have served their purpose "
                "or when the user asks. Colours are CSS names (\"gold\", \"tomato\") or hex "
                "values (\"#ffcc00\"). The change applies to this widget, not to the widgets "
                "inside it. A natively drawn control (button, group box) loses its native frame "
                "once it is coloured. Keep text readable: with a new background, set a text "
                "colour that contrasts with it. To draw attention without changing the application, "
                "prefer the highlight tools if they are available. " + idHelp)
            .addParameter("id", "string", "Widget id.", true)
            .addParameter("background", "string", "Background colour.")
            .addParameter("text", "string", "Text colour.")
            .addParameter("border", "string", "Border colour (drawn as a 2 px solid line).");
        add(set, false, [this](const QJsonObject& args) { return setWidgetColors(args); });

        Tool restore;
        restore.setName("ui_restore_widget_colors")
            .setTitle("Restore widget colours")
            .setStatusText("Restoring widget colours…")
            .setDescription("Give a widget changed with ui_set_widget_colors its original "
                            "colours back. Without an id, every changed widget is restored.")
            .addParameter("id", "string", "Widget id. Omit to restore all.");
        add(restore, false, [this](const QJsonObject& args) { return restoreWidgetColors(args); });
        break;
    }
    case Capability::Themes: {
        const QStringList names = themeNames();
        Tool list;
        list.setName("ui_list_themes")
            .setTitle("List themes")
            .setStatusText("Reading the themes…")
            .setDescription("List the colour themes this application offers, with a short "
                            "description each, and which one is active.");
        add(list, true, [this](const QJsonObject& args) { return listThemes(args); });

        Tool set;
        set.setName("ui_set_theme")
            .setTitle("Switch theme")
            .setStatusText("Switching the theme…")
            .setDescription("Switch the whole application to one of its predefined colour "
                            "themes. Only the listed themes exist; a theme cannot be created "
                            "or modified.");
        if (names.isEmpty())
            set.addParameter("theme", "string", "Theme name from ui_list_themes.", true);
        else
            set.addEnumParameter("theme", names, "Theme name.", true);
        add(set, false, [this](const QJsonObject& args) { return applyTheme(args); });
        break;
    }
    default:
        break;
    }
    return entries;
}

void AppInterface::registerCapability(Capability capability)
{
    if (!m_client)
        return;
    for (const ToolEntry& entry : toolsFor(capability))
        m_client->registerTool(entry.tool, entry.handler);
}

void AppInterface::unregisterCapability(Capability capability)
{
    if (!m_client)
        return;
    for (const ToolEntry& entry : toolsFor(capability))
        m_client->unregisterTool(entry.tool.name());
}

// ---------------------------------------------------------------- ids

bool AppInterface::excludedItself(QWidget* widget) const
{
    return m_excluded.contains(QPointer<QWidget>(widget))
        || qobject_cast<ChatDockWidget*>(widget) != nullptr;
}

bool AppInterface::isExcluded(QWidget* widget) const
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (excludedItself(current))
            return true;
    }
    return false;
}

QString AppInterface::segmentFor(QWidget* widget) const
{
    QList<QWidget*> explicitRoots;
    for (const QPointer<QWidget>& root : m_roots) {
        if (root && !explicitRoots.contains(root.data()))
            explicitRoots.append(root.data());
    }
    if (explicitRoots.contains(widget))
        return segmentFrom(widget, explicitRoots);
    const QVariant stored = widget->property(kWindowIdProperty);
    if (stored.isValid())
        return stored.toString();

    // A discovered window keeps its id while it lives, so an unnamed one gets
    // a serial (@n) instead of a position that shifts when another window closes.
    static int serial = 0;
    QString segment = cleanName(widget);
    bool taken = segment.isEmpty();
    for (QWidget* other : roots()) {
        if (taken)
            break;
        if (other == widget)
            continue;
        const QString otherSegment = explicitRoots.contains(other)
            ? segmentFrom(other, explicitRoots) : other->property(kWindowIdProperty).toString();
        taken = otherSegment == segment;
    }
    if (taken) {
        const QString base = segment.isEmpty()
            ? QString::fromLatin1(widget->metaObject()->className()) : segment;
        segment = base + QLatin1Char('@') + QString::number(++serial);
    }
    widget->setProperty(kWindowIdProperty, segment);
    return segment;
}

QString AppInterface::idForWidget(QWidget* widget) const
{
    if (!widget || isExcluded(widget))
        return QString();
    const QList<QWidget*> rootList = roots();
    QStringList steps;
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (rootList.contains(current)) {
            steps.prepend(segmentFor(current));
            const QString id = steps.join(QLatin1Char('/'));
            m_ids.insert(id, widget);
            return id;
        }
        if (!current->parentWidget())
            break;
        steps.prepend(segmentFrom(current, childWidgets(current->parentWidget())));
    }
    return QString();
}

QWidget* AppInterface::widgetForId(const QString& id) const
{
    if (id.isEmpty())
        return nullptr;

    const QStringList steps = id.split(QLatin1Char('/'));
    const QList<QWidget*> rootList = roots();
    QWidget* found = nullptr;
    for (QWidget* root : rootList) {
        if (segmentFor(root) == steps.first()) {
            found = root;
            break;
        }
    }
    for (int i = 1; found && i < steps.size(); ++i) {
        const QList<QWidget*> children = childWidgets(found);
        found = nullptr;
        for (QWidget* child : children) {
            if (segmentFrom(child, children) == steps[i]) {
                found = child;
                break;
            }
        }
    }
    if (found && isExcluded(found))
        found = nullptr;

    const auto known = m_ids.constFind(id);
    if (known != m_ids.constEnd()) {
        // The widget this id was given out for moved: the id now names a
        // different widget, which must not be operated by accident.
        if (known.value())
            return found == known.value() ? found : nullptr;
        // Deleted. A path of object names only still means the same thing if
        // the widget was recreated (a dialog opened again); a #n step does not.
        if (id.contains(QLatin1Char('#')))
            return nullptr;
    }
    if (found)
        m_ids.insert(id, found);
    return found;
}

QJsonObject AppInterface::resolve(const QString& id, QWidget*& widget) const
{
    widget = widgetForId(id);
    if (widget)
        return QJsonObject();
    if (id.isEmpty())
        return toolError(QStringLiteral("missing widget id"));
    if (m_ids.contains(id))
        return toolError(QStringLiteral("widget '%1' no longer exists (it was closed, deleted or "
                                        "moved); get current ids with ui_get_widget_tree or "
                                        "ui_find_widgets").arg(id));
    return toolError(QStringLiteral("no widget with id '%1'; get ids with ui_get_widget_tree or "
                                    "ui_find_widgets").arg(id));
}

// ---------------------------------------------------------------- actions

QList<AppInterface::ActionEntry> AppInterface::collectActions() const
{
    QList<ActionEntry> entries;
    QSet<QAction*> seen;
    QHash<QString, int> idCount;

    std::function<void(const QList<QAction*>&, const QString&)> walk =
        [&](const QList<QAction*>& actions, const QString& path) {
        for (QAction* action : actions) {
            if (action->isSeparator() || !action->isVisible() || seen.contains(action))
                continue;
            seen.insert(action);
            const QString text = stripMnemonic(action->text());
            const QString itemPath = path.isEmpty() ? text : path + QLatin1Char('/') + text;
            if (QMenu* menu = action->menu()) {
                walk(menu->actions(), itemPath);
                continue;
            }
            QString id = action->objectName().isEmpty() ? itemPath : action->objectName();
            const int duplicate = idCount[id]++;
            if (duplicate > 0)
                id += QLatin1Char('#') + QString::number(duplicate);
            entries.append({id, path, action});
        }
    };

    for (QWidget* root : roots()) {
        if (isExcluded(root))
            continue;
        // Menu bars first, so an action that is also on a toolbar keeps its menu path.
        for (QMenuBar* bar : root->findChildren<QMenuBar*>()) {
            if (!isExcluded(bar))
                walk(bar->actions(), QString());
        }
        for (QToolBar* bar : root->findChildren<QToolBar*>()) {
            if (isExcluded(bar))
                continue;
            // "Main toolbar", not "Main": the model otherwise tells the user
            // to look in a menu of that name.
            QString title = bar->windowTitle();
            if (!title.contains(QLatin1String("toolbar"), Qt::CaseInsensitive))
                title = (title + QLatin1String(" toolbar")).trimmed();
            walk(bar->actions(), title);
        }
    }
    return entries;
}

QJsonObject AppInterface::listActions(const QJsonObject& args)
{
    const QString filter = args.value("filter").toString();
    QJsonArray list;
    bool truncated = false;
    for (const ActionEntry& entry : collectActions()) {
        const QString text = stripMnemonic(entry.action->text());
        if (!filter.isEmpty() && !entry.id.contains(filter, Qt::CaseInsensitive)
            && !text.contains(filter, Qt::CaseInsensitive)
            && !entry.menu.contains(filter, Qt::CaseInsensitive))
            continue;
        if (list.size() >= kMaxListItems) {
            truncated = true;
            break;
        }
        QJsonObject item{{"id", entry.id}, {"text", text}};
        if (!entry.menu.isEmpty())
            item["menu"] = entry.menu;
        const QString shortcut = entry.action->shortcut().toString(QKeySequence::PortableText);
        if (!shortcut.isEmpty())
            item["shortcut"] = shortcut;
        if (!entry.action->isEnabled())
            item["enabled"] = false;
        if (entry.action->isCheckable())
            item["checked"] = entry.action->isChecked();
        list.append(item);
    }
    QJsonObject result{{"actions", list}};
    if (truncated)
        result["truncated"] = true;
    return toolOk(result);
}

QJsonObject AppInterface::triggerAction(const QJsonObject& args)
{
    const QString id = args.value("id").toString();
    for (const ActionEntry& entry : collectActions()) {
        if (entry.id != id)
            continue;
        if (!entry.action->isEnabled())
            return toolError(QStringLiteral("action '%1' is disabled and cannot be triggered now")
                                 .arg(id));
        // Queued: an action that opens a modal dialog would otherwise hold
        // this tool call until the user closes it.
        QTimer::singleShot(0, entry.action.data(), &QAction::trigger);
        return toolOk({{"triggered", id}});
    }
    return toolError(QStringLiteral("no action with id '%1'; get ids with ui_list_actions").arg(id));
}

// ---------------------------------------------------------------- widgets

QJsonObject AppInterface::treeNode(QWidget* widget, const QString& id, int depth,
                                   bool visibleOnly, int& budget) const
{
    --budget;
    m_ids.insert(id, widget);
    QJsonObject node{{"id", id}, {"class", QString::fromLatin1(widget->metaObject()->className())}};
    const QString text = elide(widgetText(widget).simplified(), kTreeTextChars);
    if (!text.isEmpty())
        node["text"] = text;
    if (!widget->isEnabled())
        node["enabled"] = false;
    if (!widget->isVisible())
        node["visible"] = false;
    node["geometry"] = rectArray(widget->geometry());

    const QList<QWidget*> all = childWidgets(widget);
    QList<QWidget*> shown;
    for (QWidget* child : all) {
        // A discovered window is a root of its own, not a child of its parent's tree.
        if (!excludedItself(child) && (!visibleOnly || child->isVisible())
            && !isDiscoverableWindow(child))
            shown.append(child);
    }
    if (shown.isEmpty())
        return node;
    if (depth <= 0) {
        node["more"] = shown.size();
        return node;
    }
    QJsonArray children;
    for (int i = 0; i < shown.size(); ++i) {
        if (budget <= 0) {
            node["more"] = shown.size() - i;
            break;
        }
        const QString childId = id + QLatin1Char('/') + segmentFrom(shown[i], all);
        children.append(treeNode(shown[i], childId, depth - 1, visibleOnly, budget));
    }
    node["children"] = children;
    return node;
}

QJsonObject AppInterface::getWidgetTree(const QJsonObject& args)
{
    const int depth = qBound(0, args.value("depth").toInt(2), 10);
    const bool visibleOnly = args.value("visibleOnly").toBool(true);
    int budget = kMaxTreeNodes;
    QJsonArray tree;

    if (args.contains("root")) {
        QWidget* root = nullptr;
        const QString rootId = args.value("root").toString();
        const QJsonObject error = resolve(rootId, root);
        if (!root)
            return error;
        tree.append(treeNode(root, rootId, depth, visibleOnly, budget));
    } else {
        const QList<QWidget*> rootList = roots();
        for (QWidget* root : rootList) {
            if (isExcluded(root) || (visibleOnly && !root->isVisible()) || budget <= 0)
                continue;
            tree.append(treeNode(root, segmentFor(root), depth, visibleOnly, budget));
        }
    }
    QJsonObject result{{"tree", tree}};
    if (budget <= 0)
        result["truncated"] = true;
    return toolOk(result);
}

QJsonObject AppInterface::findWidgets(const QJsonObject& args)
{
    const QString text = args.value("text").toString();
    const QString className = args.value("class").toString();
    const QString name = args.value("name").toString();
    if (text.isEmpty() && className.isEmpty() && name.isEmpty())
        return toolError(QStringLiteral("give at least one of 'text', 'class', 'name'"));
    const QByteArray classLatin1 = className.toLatin1();

    QJsonArray matches;
    bool truncated = false;
    std::function<void(QWidget*, const QString&)> walk = [&](QWidget* widget, const QString& id) {
        if (truncated)
            return;
        const QString widgetLabel = widgetText(widget);
        const QString widgetClass = QString::fromLatin1(widget->metaObject()->className());
        const bool match =
            (text.isEmpty() || widgetLabel.contains(text, Qt::CaseInsensitive))
            && (className.isEmpty() || widget->inherits(classLatin1.constData())
                || widgetClass.contains(className, Qt::CaseInsensitive))
            && (name.isEmpty() || widget->objectName().contains(name, Qt::CaseInsensitive));
        if (match) {
            if (matches.size() >= kMaxFindResults) {
                truncated = true;
                return;
            }
            m_ids.insert(id, widget);
            QJsonObject item{{"id", id}, {"class", widgetClass}};
            const QString shortText = elide(widgetLabel.simplified(), kTreeTextChars);
            if (!shortText.isEmpty())
                item["text"] = shortText;
            if (!widget->isEnabled())
                item["enabled"] = false;
            if (!widget->isVisible())
                item["visible"] = false;
            matches.append(item);
        }
        const QList<QWidget*> children = childWidgets(widget);
        for (QWidget* child : children) {
            if (!excludedItself(child) && !isDiscoverableWindow(child))
                walk(child, id + QLatin1Char('/') + segmentFrom(child, children));
        }
    };
    for (QWidget* root : roots()) {
        if (!isExcluded(root))
            walk(root, segmentFor(root));
    }

    QJsonObject result{{"widgets", matches}};
    if (truncated)
        result["truncated"] = true;
    return toolOk(result);
}

QJsonObject AppInterface::listWindows(const QJsonObject&)
{
    QJsonArray list;
    for (QWidget* root : roots()) {
        if (isExcluded(root))
            continue;
        const QString id = segmentFor(root);
        m_ids.insert(id, root);
        QJsonObject item{{"id", id},
                         {"title", root->windowTitle()},
                         {"class", QString::fromLatin1(root->metaObject()->className())},
                         {"modal", root->isModal()},
                         {"active", root->isActiveWindow()}};
        if (!root->isVisible())
            item["visible"] = false;
        list.append(item);
    }
    return toolOk({{"windows", list}});
}

QJsonObject AppInterface::getWidget(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!widget)
        return error;

    QJsonObject info{{"id", id}, {"class", QString::fromLatin1(widget->metaObject()->className())}};
    const QString text = widgetText(widget);
    if (!text.isEmpty()) {
        info["text"] = elide(text, kDetailTextChars);
        if (text.size() > kDetailTextChars)
            info["textLength"] = text.size();
    }
    if (isPassword(widget))
        info["password"] = true;
    if (!widget->isEnabled())
        info["enabled"] = false;
    if (!widget->isVisible())
        info["visible"] = false;
    if (!widget->toolTip().isEmpty())
        info["toolTip"] = widget->toolTip();
    info["globalGeometry"] = rectArray(QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size()));

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        if (button->isCheckable())
            info["checked"] = button->isChecked();
    } else if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        if (!edit->placeholderText().isEmpty())
            info["placeholder"] = edit->placeholderText();
        if (edit->isReadOnly())
            info["readOnly"] = true;
    } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        QJsonArray items;
        for (int i = 0; i < combo->count() && i < kMaxListItems; ++i)
            items.append(combo->itemText(i));
        info["items"] = items;
        if (combo->count() > kMaxListItems)
            info["itemCount"] = combo->count();
        info["currentIndex"] = combo->currentIndex();
        if (combo->isEditable())
            info["editable"] = true;
    } else if (auto* spin = qobject_cast<QSpinBox*>(widget)) {
        info["value"] = spin->value();
        info["range"] = QJsonArray{spin->minimum(), spin->maximum()};
    } else if (auto* doubleSpin = qobject_cast<QDoubleSpinBox*>(widget)) {
        info["value"] = doubleSpin->value();
        info["range"] = QJsonArray{doubleSpin->minimum(), doubleSpin->maximum()};
    } else if (auto* slider = qobject_cast<QAbstractSlider*>(widget)) {
        info["value"] = slider->value();
        info["range"] = QJsonArray{slider->minimum(), slider->maximum()};
    } else if (auto* progress = qobject_cast<QProgressBar*>(widget)) {
        info["value"] = progress->value();
        info["range"] = QJsonArray{progress->minimum(), progress->maximum()};
    } else if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
        QJsonArray tabList;
        for (int i = 0; i < tabs->count() && i < kMaxListItems; ++i)
            tabList.append(stripMnemonic(tabs->tabText(i)));
        info["tabs"] = tabList;
        info["currentIndex"] = tabs->currentIndex();
    } else if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
        if (view->model()) {
            info["rows"] = view->model()->rowCount();
            info["columns"] = view->model()->columnCount();
        }
    } else if (auto* textEdit = qobject_cast<QTextEdit*>(widget)) {
        if (textEdit->isReadOnly())
            info["readOnly"] = true;
    } else if (auto* plain = qobject_cast<QPlainTextEdit*>(widget)) {
        if (plain->isReadOnly())
            info["readOnly"] = true;
    }
    return toolOk(info);
}

// ---------------------------------------------------------------- properties

QJsonObject AppInterface::getProperties(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!widget)
        return error;

    const QMetaObject* meta = widget->metaObject();
    QStringList names;
    for (const QJsonValue& value : args.value("names").toArray())
        names.append(value.toString());
    const bool explicitNames = !names.isEmpty();
    if (!explicitNames) {
        names = QStringList{QStringLiteral("enabled"), QStringLiteral("visible"),
                            QStringLiteral("toolTip")};
        for (int i = QWidget::staticMetaObject.propertyCount(); i < meta->propertyCount(); ++i)
            names.append(QString::fromLatin1(meta->property(i).name()));
    }

    QJsonObject values;
    QJsonArray unknown;
    const bool password = isPassword(widget);
    for (const QString& name : names) {
        const int index = meta->indexOfProperty(name.toLatin1().constData());
        if (index < 0) {
            unknown.append(name);
            continue;
        }
        const QMetaProperty property = meta->property(index);
        if (!property.isReadable())
            continue;
        QJsonValue value = password && isSecretProperty(name)
            ? QJsonValue(redacted())
            : propertyToJson(property, property.read(widget));
        if (!explicitNames && (value.isUndefined() || value.isNull()
                               || (value.isString() && value.toString().isEmpty())
                               || (value.isArray() && value.toArray().isEmpty())))
            continue;
        if (value.isUndefined())
            value = QStringLiteral("<%1>").arg(QString::fromLatin1(property.typeName()));
        values[name] = value;
    }
    QJsonObject result{{"id", id}, {"properties", values}};
    if (!unknown.isEmpty())
        result["unknown"] = unknown;
    return toolOk(result);
}

QJsonObject AppInterface::setWidgetProperty(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!widget)
        return error;

    const QString name = args.value("name").toString();
    if (!m_writable.contains(name))
        return toolError(QStringLiteral("property '%1' may not be changed").arg(name),
                         {{"writable", QJsonArray::fromStringList(m_writable)}});
    const QMetaObject* meta = widget->metaObject();
    const int index = meta->indexOfProperty(name.toLatin1().constData());
    if (index < 0)
        return toolError(QStringLiteral("%1 has no property '%2'")
                             .arg(QString::fromLatin1(meta->className()), name));
    const QMetaProperty property = meta->property(index);
    if (!property.isWritable())
        return toolError(QStringLiteral("property '%1' is read-only").arg(name));
    // A user could not change a disabled control either.
    if (!widget->isEnabled() && name != QLatin1String("enabled") && name != QLatin1String("visible"))
        return toolError(QStringLiteral("widget '%1' is disabled").arg(id));
    const QJsonValue json = args.value("value");
    if (json.isUndefined())
        return toolError(QStringLiteral("missing 'value'"));

    QVariant value;
    if (property.isEnumType()) {
        const QMetaEnum metaEnum = property.enumerator();
        if (json.isString()) {
            bool ok = false;
            const QByteArray key = json.toString().toLatin1();
            const int raw = metaEnum.isFlag() ? metaEnum.keysToValue(key.constData(), &ok)
                                              : metaEnum.keyToValue(key.constData(), &ok);
            if (!ok) {
                QJsonArray keys;
                for (int i = 0; i < metaEnum.keyCount(); ++i)
                    keys.append(QString::fromLatin1(metaEnum.key(i)));
                return toolError(QStringLiteral("invalid value for '%1'").arg(name),
                                 {{"allowed_values", keys}});
            }
            value = raw;
        } else {
            value = json.toInt();
        }
    } else {
        value = json.toVariant();
        if (!convertVariant(value, property.userType()))
            return toolError(QStringLiteral("cannot convert the value to %1")
                                 .arg(QString::fromLatin1(property.typeName())));
    }
    const QVariant old = property.read(widget);
    if (!property.write(widget, value))
        return toolError(QStringLiteral("the widget rejected the value for '%1'").arg(name));
    QPointer<QWidget> guard(widget);
    const QByteArray propertyName = name.toLatin1();
    m_propertyUndo.append([guard, propertyName, old]() {
        if (!guard)
            return false;
        guard->setProperty(propertyName.constData(), old);
        return true;
    });

    const QJsonValue now = isPassword(widget) && isSecretProperty(name)
        ? QJsonValue(redacted())
        : propertyToJson(property, property.read(widget));
    return toolOk({{"id", id}, {"name", name}, {"value", now}});
}

QJsonObject AppInterface::undoPropertyChange(const QJsonObject&)
{
    if (m_propertyUndo.isEmpty())
        return toolError(QStringLiteral("there is no property change to undo"));
    if (!m_propertyUndo.takeLast()())
        return toolError(QStringLiteral("the widget of the last property change no longer exists; "
                                        "nothing was reverted"),
                         {{"undoSteps", m_propertyUndo.size()}});
    return toolOk({{"undoSteps", m_propertyUndo.size()}});
}

// ---------------------------------------------------------------- layouts

QLayout* AppInterface::resolveLayout(const QJsonObject& args, QJsonObject& error) const
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    error = resolve(id, widget);
    if (!widget)
        return nullptr;
    QLayout* layout = widget->layout();
    if (!layout) {
        QWidget* parent = widget->parentWidget();
        const QString parentId = parent && parent->layout() ? idForWidget(parent) : QString();
        error = parentId.isEmpty()
            ? toolError(QStringLiteral("widget '%1' has no layout").arg(id))
            : toolError(QStringLiteral("widget '%1' has no layout; it is placed in the layout of '%2'")
                            .arg(id, parentId));
        return nullptr;
    }
    const QString address = args.value("sublayout").toString();
    for (const QString& step : address.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int index = step.toInt(&ok);
        QLayoutItem* item = ok ? layout->itemAt(index) : nullptr;
        if (!item || !item->layout()) {
            error = toolError(QStringLiteral("no nested layout at '%1'").arg(address));
            return nullptr;
        }
        layout = item->layout();
    }
    return layout;
}

QJsonObject AppInterface::layoutInfo(QLayout* layout, const QString& address) const
{
    QJsonObject info{{"type", QString::fromLatin1(layout->metaObject()->className())}};
    if (!address.isEmpty())
        info["sublayout"] = address;

    auto* box = qobject_cast<QBoxLayout*>(layout);
    auto* grid = qobject_cast<QGridLayout*>(layout);
    auto* form = qobject_cast<QFormLayout*>(layout);
    if (box) {
        const bool horizontal = box->direction() == QBoxLayout::LeftToRight
                             || box->direction() == QBoxLayout::RightToLeft;
        info["direction"] = horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical");
    }
    if (grid) {
        info["spacing"] = QJsonArray{grid->horizontalSpacing(), grid->verticalSpacing()};
        QJsonObject rowStretch;
        for (int r = 0; r < grid->rowCount(); ++r) {
            if (grid->rowStretch(r) != 0)
                rowStretch[QString::number(r)] = grid->rowStretch(r);
        }
        QJsonObject columnStretch;
        for (int c = 0; c < grid->columnCount(); ++c) {
            if (grid->columnStretch(c) != 0)
                columnStretch[QString::number(c)] = grid->columnStretch(c);
        }
        if (!rowStretch.isEmpty())
            info["rowStretch"] = rowStretch;
        if (!columnStretch.isEmpty())
            info["columnStretch"] = columnStretch;
    } else if (form) {
        info["spacing"] = QJsonArray{form->horizontalSpacing(), form->verticalSpacing()};
    } else {
        info["spacing"] = layout->spacing();
    }
    const QMargins margins = layout->contentsMargins();
    info["margins"] = QJsonArray{margins.left(), margins.top(), margins.right(), margins.bottom()};

    QJsonArray items;
    for (int i = 0; i < layout->count() && i < kMaxListItems; ++i) {
        QLayoutItem* item = layout->itemAt(i);
        QJsonObject entry{{"index", i}};
        if (QWidget* widget = item->widget()) {
            const QString widgetId = idForWidget(widget);
            if (widgetId.isEmpty())
                continue;
            entry["widget"] = widgetId;
            if (widget->isHidden())
                entry["hidden"] = true;
        } else if (QLayout* nested = item->layout()) {
            entry["layout"] = layoutInfo(nested, address.isEmpty()
                                                     ? QString::number(i)
                                                     : address + QLatin1Char('/') + QString::number(i));
        } else if (QSpacerItem* spacer = item->spacerItem()) {
            entry["spacer"] = QJsonArray{spacer->sizeHint().width(), spacer->sizeHint().height()};
        }
        if (box && box->stretch(i) != 0)
            entry["stretch"] = box->stretch(i);
        if (grid) {
            int row = 0, column = 0, rowSpan = 1, columnSpan = 1;
            grid->getItemPosition(i, &row, &column, &rowSpan, &columnSpan);
            entry["row"] = row;
            entry["column"] = column;
            if (rowSpan != 1)
                entry["rowSpan"] = rowSpan;
            if (columnSpan != 1)
                entry["columnSpan"] = columnSpan;
        }
        if (form) {
            int row = 0;
            QFormLayout::ItemRole role = QFormLayout::FieldRole;
            form->getItemPosition(i, &row, &role);
            entry["row"] = row;
            entry["role"] = role == QFormLayout::LabelRole   ? QStringLiteral("label")
                          : role == QFormLayout::FieldRole   ? QStringLiteral("field")
                                                             : QStringLiteral("spanning");
        }
        if (item->alignment() != Qt::Alignment())
            entry["alignment"] = alignmentToString(item->alignment());
        items.append(entry);
    }
    info["items"] = items;
    if (layout->count() > kMaxListItems)
        info["truncated"] = true;
    return info;
}

QJsonObject AppInterface::getLayout(const QJsonObject& args)
{
    QJsonObject error;
    QLayout* layout = resolveLayout(args, error);
    if (!layout)
        return error;
    return toolOk({{"id", args.value("id").toString()},
                   {"layout", layoutInfo(layout, args.value("sublayout").toString())}});
}

QJsonObject AppInterface::modifyLayout(const QJsonObject& args)
{
    QJsonObject error;
    QLayout* layout = resolveLayout(args, error);
    if (!layout)
        return error;

    const QString operation = args.value("operation").toString();
    QPointer<QLayout> guard(layout);
    auto* box = qobject_cast<QBoxLayout*>(layout);
    auto* grid = qobject_cast<QGridLayout*>(layout);
    auto* form = qobject_cast<QFormLayout*>(layout);
    const int item = args.value("item").toInt(-1);
    const bool hasItem = args.contains("item") && item >= 0 && item < layout->count();
    const QString itemError = QStringLiteral("'%1' needs 'item', an index between 0 and %2")
                                  .arg(operation).arg(layout->count() - 1);
    std::function<void()> undo;

    if (operation == QLatin1String("set_spacing")) {
        if (!args.contains("value"))
            return toolError(QStringLiteral("set_spacing needs 'value'"));
        const int value = args.value("value").toInt();
        if (grid) {
            const int h = grid->horizontalSpacing();
            const int v = grid->verticalSpacing();
            grid->setSpacing(value);
            undo = [guard, h, v]() {
                if (auto* g = qobject_cast<QGridLayout*>(guard.data())) {
                    g->setHorizontalSpacing(h);
                    g->setVerticalSpacing(v);
                }
            };
        } else if (form) {
            const int h = form->horizontalSpacing();
            const int v = form->verticalSpacing();
            form->setSpacing(value);
            undo = [guard, h, v]() {
                if (auto* f = qobject_cast<QFormLayout*>(guard.data())) {
                    f->setHorizontalSpacing(h);
                    f->setVerticalSpacing(v);
                }
            };
        } else {
            const int old = layout->spacing();
            layout->setSpacing(value);
            undo = [guard, old]() {
                if (guard)
                    guard->setSpacing(old);
            };
        }
    } else if (operation == QLatin1String("set_margins")) {
        QMargins margins;
        const QJsonArray list = args.value("margins").toArray();
        if (list.size() == 4) {
            margins = QMargins(list[0].toInt(), list[1].toInt(), list[2].toInt(), list[3].toInt());
        } else if (args.contains("value")) {
            const int value = args.value("value").toInt();
            margins = QMargins(value, value, value, value);
        } else {
            return toolError(QStringLiteral("set_margins needs 'margins' [l,t,r,b] or 'value'"));
        }
        const QMargins old = layout->contentsMargins();
        layout->setContentsMargins(margins);
        undo = [guard, old]() {
            if (guard)
                guard->setContentsMargins(old);
        };
    } else if (operation == QLatin1String("set_stretch")) {
        if (!args.contains("value"))
            return toolError(QStringLiteral("set_stretch needs 'value'"));
        const int value = args.value("value").toInt();
        if (box) {
            if (!hasItem)
                return toolError(itemError);
            const int old = box->stretch(item);
            box->setStretch(item, value);
            undo = [guard, item, old]() {
                auto* b = qobject_cast<QBoxLayout*>(guard.data());
                if (b && item < b->count())
                    b->setStretch(item, old);
            };
        } else if (grid && args.contains("row")) {
            const int row = args.value("row").toInt();
            const int old = grid->rowStretch(row);
            grid->setRowStretch(row, value);
            undo = [guard, row, old]() {
                if (auto* g = qobject_cast<QGridLayout*>(guard.data()))
                    g->setRowStretch(row, old);
            };
        } else if (grid && args.contains("column")) {
            const int column = args.value("column").toInt();
            const int old = grid->columnStretch(column);
            grid->setColumnStretch(column, value);
            undo = [guard, column, old]() {
                if (auto* g = qobject_cast<QGridLayout*>(guard.data()))
                    g->setColumnStretch(column, old);
            };
        } else {
            return toolError(grid ? QStringLiteral("set_stretch on a grid needs 'row' or 'column'")
                                  : QStringLiteral("set_stretch works on box and grid layouts only"));
        }
    } else if (operation == QLatin1String("move_item")) {
        if (!box)
            return toolError(QStringLiteral("move_item works on box layouts only"));
        if (!hasItem)
            return toolError(itemError);
        const int to = args.value("to").toInt(-1);
        if (!args.contains("to") || to < 0 || to >= box->count())
            return toolError(QStringLiteral("move_item needs 'to', an index between 0 and %1")
                                 .arg(box->count() - 1));
        if (to == item)
            return toolOk({{"operation", operation}, {"changed", false}});
        moveBoxItem(box, item, to);
        undo = [guard, item, to]() {
            auto* b = qobject_cast<QBoxLayout*>(guard.data());
            if (b && to < b->count() && item < b->count())
                moveBoxItem(b, to, item);
        };
    } else if (operation == QLatin1String("set_alignment")) {
        if (!hasItem)
            return toolError(itemError);
        Qt::Alignment alignment;
        if (!alignmentFromString(args.value("alignment").toString(), alignment))
            return toolError(QStringLiteral("invalid 'alignment'"),
                             {{"allowed_values", QJsonArray{"left", "right", "hcenter", "justify",
                                                            "top", "bottom", "vcenter", "center",
                                                            "none"}}});
        QLayoutItem* layoutItem = layout->itemAt(item);
        const Qt::Alignment old = layoutItem->alignment();
        layoutItem->setAlignment(alignment);
        layout->invalidate();
        undo = [guard, item, old]() {
            if (guard && item < guard->count()) {
                guard->itemAt(item)->setAlignment(old);
                guard->invalidate();
            }
        };
    } else if (operation == QLatin1String("set_visible")) {
        if (!hasItem || !layout->itemAt(item)->widget())
            return toolError(QStringLiteral("set_visible needs 'item', the index of a widget item"));
        QWidget* widget = layout->itemAt(item)->widget();
        if (isExcluded(widget))
            return toolError(itemError);
        if (!args.contains("visible"))
            return toolError(QStringLiteral("set_visible needs 'visible'"));
        const bool wasHidden = widget->isHidden();
        widget->setVisible(args.value("visible").toBool());
        QPointer<QWidget> widgetGuard(widget);
        undo = [widgetGuard, wasHidden]() {
            if (widgetGuard)
                widgetGuard->setHidden(wasHidden);
        };
    } else {
        return toolError(QStringLiteral("unknown operation '%1'").arg(operation),
                         {{"allowed_values", QJsonArray{"set_spacing", "set_margins", "set_stretch",
                                                        "move_item", "set_alignment", "set_visible"}}});
    }

    m_layoutUndo.append(undo);
    return toolOk({{"operation", operation}, {"undoSteps", m_layoutUndo.size()}});
}

QJsonObject AppInterface::undoLayoutChange(const QJsonObject&)
{
    if (m_layoutUndo.isEmpty())
        return toolError(QStringLiteral("there is no layout change to undo"));
    m_layoutUndo.takeLast()();
    return toolOk({{"undoSteps", m_layoutUndo.size()}});
}

// ---------------------------------------------------------------- screenshot

QJsonObject AppInterface::screenshot(const QJsonObject& args)
{
    QWidget* target = nullptr;
    QString targetId = args.value("target").toString();
    if (!targetId.isEmpty()) {
        const QJsonObject error = resolve(targetId, target);
        if (!target)
            return error;
    } else {
        for (QWidget* root : roots()) {
            if (root->isVisible() && !isExcluded(root)) {
                target = root;
                break;
            }
        }
        if (!target)
            return toolError(QStringLiteral("no visible application window to capture"));
        targetId = idForWidget(target);
    }
    if (!target->isVisible())
        return toolError(QStringLiteral("widget '%1' is not visible").arg(targetId));

    const QPixmap pixmap = target->grab();
    QImage image = pixmap.toImage();
    // Keeps painting in widget coordinates on high-DPI screens.
    image.setDevicePixelRatio(pixmap.devicePixelRatio());
    QPainter painter(&image);
    if (isPassword(target))
        painter.fillRect(target->rect(), Qt::black);

    const bool annotate = args.value("annotate").toBool(false);
    QList<QWidget*> marked;
    bool truncated = false;
    std::function<void(QWidget*, bool)> walk = [&](QWidget* widget, bool insideControl) {
        for (QWidget* child : childWidgets(widget)) {
            // Child windows are not part of the grab.
            if (!child->isVisible() || child->isWindow())
                continue;
            const QRect rect(child->mapTo(target, QPoint(0, 0)), child->size());
            if (excludedItself(child)) {
                painter.fillRect(rect, QColor(128, 128, 128));
                continue;
            }
            if (isPassword(child))
                painter.fillRect(rect, Qt::black);
            const bool control = isInteractive(child);
            if (annotate && control && !insideControl && !child->visibleRegion().isEmpty()) {
                if (marked.size() < kMaxMarkers)
                    marked.append(child);
                else
                    truncated = true;
            }
            walk(child, insideControl || control);
        }
    };
    walk(target, false);

    QJsonObject markers;
    if (annotate) {
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        for (int i = 0; i < marked.size(); ++i) {
            const QString label = QString::number(i + 1);
            const QPoint topLeft = marked[i]->mapTo(target, QPoint(0, 0));
            const QRect badge(topLeft, QSize(qMax(14, metrics.horizontalAdvance(label) + 6), 14));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(220, 0, 0));
            painter.drawRoundedRect(badge, 3, 3);
            painter.setPen(Qt::white);
            painter.drawText(badge, Qt::AlignCenter, label);
            markers[label] = idForWidget(marked[i]);
        }
    }
    painter.end();

    QJsonObject payload{{"target", targetId},
                        {"size", QJsonArray{target->width(), target->height()}}};
    if (annotate)
        payload["markers"] = markers;
    if (truncated)
        payload["truncated"] = true;
    const int maxEdge = args.value("maxEdge").toInt(0);
    return toolImage(image, payload, maxEdge > 0 ? maxEdge : 1568);
}

} // namespace QtLLM
