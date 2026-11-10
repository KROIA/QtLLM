// Input tools of AppInterface: the model clicks, types and presses keys like a
// user (capability SimulateInput) and waits for the UI to react (WaitForUi).
// Events go through QApplication::sendEvent, so pressed/released/clicked,
// textEdited and everything connected to them fire as for real input.
#include "AppInterface.h"
#include "Client.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextEdit>
#include <QTimer>
#include <QVariant>

// Exported by QtGui in Qt 5.15 and Qt 6 with this signature; QtTest declares
// and uses it the same way. Offers a key press to the application's shortcut
// map (QShortcut, QAction shortcuts); true when a shortcut consumed it.
QT_BEGIN_NAMESPACE
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k,
                                               Qt::KeyboardModifiers mods, const QString& text,
                                               bool autorep, ushort count);
QT_END_NAMESPACE

namespace QtLLM
{

namespace
{

const char* const kGroup = "App UI";

const char* const kLookAgain =
    "Delivered right after this call returns, so a dialog it opens does not block you. "
    "Look again (or use ui_wait_for) to see the effect.";

const QStringList& waitConditions()
{
    static const QStringList conditions{
        QStringLiteral("visible"), QStringLiteral("hidden"), QStringLiteral("enabled"),
        QStringLiteral("disabled"), QStringLiteral("exists"), QStringLiteral("gone"),
        QStringLiteral("property_equals"), QStringLiteral("window_opened"),
        QStringLiteral("window_closed")};
    return conditions;
}

void sendKeyEvent(QWidget* receiver, int key, Qt::KeyboardModifiers modifiers, const QString& text)
{
    QPointer<QWidget> guard(receiver);
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(receiver, &press);
    if (!guard)
        return;
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(guard.data(), &release);
}

// The text a key produces when typed, as the platform would report it.
QString textForKey(int key, Qt::KeyboardModifiers modifiers)
{
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return QString();
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     return QStringLiteral("\r");
    case Qt::Key_Escape:    return QString(QChar(0x1b));
    case Qt::Key_Tab:       return QStringLiteral("\t");
    case Qt::Key_Backspace: return QString(QChar(0x08));
    default:                break;
    }
    if (key < 0x20 || key > 0x7e)
        return QString();
    // ponytail: Shift only changes the case of letters; Shift+1 gives "1", not "!".
    const QChar character(key);
    return QString(modifiers.testFlag(Qt::ShiftModifier) ? character : character.toLower());
}

// Like a real key press: shortcuts first, then the widget (and its parents,
// QApplication propagates an ignored key event).
void deliverKey(QWidget* receiver, int key, Qt::KeyboardModifiers modifiers)
{
    const QString text = textForKey(key, modifiers);
    if (qt_sendShortcutOverrideEvent(receiver, 0, key, modifiers, text, false, 1))
        return;
    sendKeyEvent(receiver, key, modifiers, text);
}

void typeInto(QWidget* editor, const QString& text)
{
    QPointer<QWidget> guard(editor);
    for (int i = 0; i < text.size() && guard; ++i) {
        QString piece(text[i]);
        if (text[i].isHighSurrogate() && i + 1 < text.size())
            piece += text[++i];
        const ushort code = piece[0].unicode();
        if (code == '\n')
            sendKeyEvent(editor, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        else if (code == '\t')
            sendKeyEvent(editor, Qt::Key_Tab, Qt::NoModifier, piece);
        else if (code >= 0x20)
            sendKeyEvent(editor, code < 0x80 ? QChar(code).toUpper().unicode() : int(Qt::Key_unknown),
                         Qt::NoModifier, piece);
    }
}

void sendClick(QWidget* widget, const QPoint& pos, Qt::MouseButton button, bool doubleClick)
{
    // The event goes where a real click would land: the deepest child under
    // the point (a view's viewport, a spin box's line edit).
    QWidget* child = widget->childAt(pos);
    QPointer<QWidget> receiver(child ? child : widget);
    const QPoint local = child ? child->mapFrom(widget, pos) : pos;
    auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        if (!receiver)
            return;
        QMouseEvent event(type, local, receiver->mapTo(receiver->window(), local),
                          receiver->mapToGlobal(local), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(receiver.data(), &event);
    };
    send(QEvent::MouseButtonPress, button);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    if (doubleClick) {
        send(QEvent::MouseButtonDblClick, button);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
    }
}

// Empty when a user could operate the widget, otherwise the error to return.
QJsonObject checkReachable(QWidget* widget, const QString& id)
{
    if (!widget->isVisible())
        return toolError(QStringLiteral("widget '%1' is not visible").arg(id));
    if (!widget->isEnabled())
        return toolError(QStringLiteral("widget '%1' is disabled").arg(id));
    // ponytail: a window-modal dialog is treated like an application-modal one.
    QWidget* modal = QApplication::activeModalWidget();
    if (!modal)
        return QJsonObject();
    for (QWidget* window = widget->window(); window; window = window->parentWidget()) {
        if (window == modal)
            return QJsonObject();
    }
    return toolError(QStringLiteral("the modal window '%1' blocks the rest of the application; "
                                    "answer or close it first (see ui_list_windows)")
                         .arg(modal->windowTitle()));
}

bool sameValue(const QJsonValue& current, const QJsonValue& expected)
{
    return current == expected
        || (!current.isUndefined() && !current.isNull()
            && current.toVariant().toString() == expected.toVariant().toString());
}

} // namespace

void AppInterface::syncInputTools()
{
    if (!m_client) {
        m_inputTools.clear();
        return;
    }
    for (const QString& name : m_inputTools)
        m_client->unregisterTool(name);
    m_inputTools.clear();

    const QString idHelp = QStringLiteral(
        "Take ids from ui_get_widget_tree or ui_find_widgets.");

    if (isCapabilityEnabled(Capability::SimulateInput)) {
        auto add = [this](Tool& tool, ToolHandler handler) {
            tool.setGroup(QString::fromLatin1(kGroup)).setReadOnly(false);
            m_client->registerTool(tool, std::move(handler));
            m_inputTools.append(tool.name());
        };

        Tool clickTool;
        clickTool.setName("ui_click")
            .setTitle("Click")
            .setStatusText("Clicking…")
            .setDescription(
                "Click a widget with the mouse, exactly as the user would: buttons, check boxes, "
                "radio buttons, tabs, combo boxes, list entries. Everything connected to the "
                "click runs. Hidden or disabled widgets, and widgets behind a modal dialog, are "
                "refused. For a menu command prefer ui_trigger_action; to set a value directly "
                "ui_set_property. Returns at once: " + QString::fromLatin1(kLookAgain) + " " + idHelp)
            .addParameter("id", "string", "Widget id.", true)
            .addEnumParameter("button", {"left", "right", "middle"}, "Mouse button (default left).")
            .addParameter("double", "boolean", "Double-click (default false).")
            .addParameter("x", "integer", "Horizontal position inside the widget in pixels "
                                          "(default: the centre).")
            .addParameter("y", "integer", "Vertical position inside the widget in pixels "
                                          "(default: the centre).");
        add(clickTool, [this](const QJsonObject& args) { return click(args); });

        Tool typeTool;
        typeTool.setName("ui_type_text")
            .setTitle("Type text")
            .setStatusText("Typing…")
            .setDescription(
                "Type text into a text field, text editor, spin box or editable combo box, key by "
                "key as the user would, so the application reacts as to real typing. By default "
                "the current content is replaced; replace: false inserts at the cursor. A newline "
                "presses Enter. Read-only, disabled and hidden widgets are refused. Returns the "
                "widget's text afterwards (password fields: [redacted]). " + idHelp)
            .addParameter("id", "string", "Widget id.", true)
            .addParameter("text", "string", "Text to type. Empty with replace: true clears the field.",
                          true)
            .addParameter("replace", "boolean", "Replace the current content (default true).")
            .addParameter("submit", "boolean",
                          "Press Enter afterwards, e.g. to run a search or accept a dialog "
                          "(default false). " + QString::fromLatin1(kLookAgain));
        add(typeTool, [this](const QJsonObject& args) { return typeText(args); });

        Tool keyTool;
        keyTool.setName("ui_press_key")
            .setTitle("Press key")
            .setStatusText("Pressing a key…")
            .setDescription(
                "Press one key or key combination, e.g. Enter, Escape, Tab, Down, F5, Ctrl+S, "
                "Ctrl+Shift+Z. Goes to the widget given by id, else to the focused widget of the "
                "active application window. Application shortcuts (menu shortcuts, QShortcut) fire "
                "as for a real key press while the application window is active; when the user "
                "is working in another program they may not, so for a menu command prefer "
                "ui_trigger_action. Escape usually cancels a dialog, Enter accepts it. " +
                QString::fromLatin1(kLookAgain))
            .addParameter("key", "string", "The key in Qt's notation: 'Enter', 'Escape', "
                                           "'Ctrl+S', 'Down', 'F5'.", true)
            .addParameter("id", "string", "Widget id. Omit for the focused widget.");
        add(keyTool, [this](const QJsonObject& args) { return pressKey(args); });
    }

    if (isCapabilityEnabled(Capability::WaitForUi)) {
        Tool wait;
        wait.setName("ui_wait_for")
            .setTitle("Wait for the UI")
            .setStatusText("Waiting for the application…")
            .setDescription(
                "Wait until something in the application happened, e.g. after a click: a widget "
                "became visible, hidden, enabled or disabled, appeared (exists) or disappeared "
                "(gone), a property has a value (property_equals, e.g. a label's text or a "
                "progress bar's value), a new window opened (window_opened, no id needed; returns "
                "its id and title) or a window closed (window_closed). Checks every 100 ms. "
                "Returns waited_ms when met; on timeout an error with the current state, so you "
                "can decide what to do. Cheaper than reading the UI again and again.")
            .addEnumParameter("condition", waitConditions(), "What to wait for.", true)
            .addParameter("id", "string", "Widget or window id; needed for all but window_opened.")
            .addParameter("property", "string", "Property name for property_equals.")
            .addRawParameter("value", QJsonObject{{"description",
                                                   "Expected value for property_equals."}})
            .addParameter("timeoutSeconds", "number", "How long to wait (default 10, at most 120).");
        wait.setGroup(QString::fromLatin1(kGroup)).setReadOnly(true).setTimeoutMs(130000);
        m_client->registerAsyncTool(wait, [this](const QJsonObject& input, ToolCompletion done) {
            waitFor(input, [done](const QJsonObject& result) { done.finish(result); },
                    [done]() { return done.isFinished(); });
        });
        m_inputTools.append(wait.name());
    }
}

QJsonObject AppInterface::click(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!widget)
        return error;
    const QJsonObject unreachable = checkReachable(widget, id);
    if (!unreachable.isEmpty())
        return unreachable;

    const QString buttonName = args.value("button").toString(QStringLiteral("left"));
    Qt::MouseButton button = Qt::LeftButton;
    if (buttonName == QLatin1String("right"))
        button = Qt::RightButton;
    else if (buttonName == QLatin1String("middle"))
        button = Qt::MiddleButton;
    else if (buttonName != QLatin1String("left"))
        return toolError(QStringLiteral("invalid 'button'"),
                         {{"allowed_values", QJsonArray{"left", "right", "middle"}}});

    QPoint pos = widget->rect().center();
    // A check box or radio button stretched by its layout reacts only on its
    // box and label; the middle of the widget can be empty space. Aim at the box.
    const bool isCheck = qobject_cast<QCheckBox*>(widget) != nullptr;
    if (isCheck || qobject_cast<QRadioButton*>(widget)) {
        QStyleOptionButton option;
        option.initFrom(widget);
        pos = widget->style()->subElementRect(isCheck ? QStyle::SE_CheckBoxIndicator
                                                      : QStyle::SE_RadioButtonIndicator,
                                              &option, widget).center();
    }
    if (args.contains("x"))
        pos.setX(args.value("x").toInt());
    if (args.contains("y"))
        pos.setY(args.value("y").toInt());
    if (!widget->rect().contains(pos))
        return toolError(QStringLiteral("(%1, %2) lies outside the widget, which is %3 x %4 pixels")
                             .arg(pos.x()).arg(pos.y()).arg(widget->width()).arg(widget->height()));

    const bool doubleClick = args.value("double").toBool(false);
    QTimer::singleShot(0, widget, [widget, pos, button, doubleClick]() {
        sendClick(widget, pos, button, doubleClick);
    });
    return toolOk({{"clicked", id}, {"note", QString::fromLatin1(kLookAgain)}});
}

QJsonObject AppInterface::typeText(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!widget)
        return error;
    const QJsonObject unreachable = checkReachable(widget, id);
    if (!unreachable.isEmpty())
        return unreachable;

    QWidget* editor = widget;
    bool readOnly = false;
    std::function<void()> selectAll;
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        readOnly = edit->isReadOnly();
        selectAll = [edit]() { edit->selectAll(); };
    } else if (auto* textEdit = qobject_cast<QTextEdit*>(widget)) {
        readOnly = textEdit->isReadOnly();
        selectAll = [textEdit]() { textEdit->selectAll(); };
    } else if (auto* plain = qobject_cast<QPlainTextEdit*>(widget)) {
        readOnly = plain->isReadOnly();
        selectAll = [plain]() { plain->selectAll(); };
    } else if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget)) {
        readOnly = spin->isReadOnly();
        selectAll = [spin]() { spin->selectAll(); };
    } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        if (!combo->isEditable() || !combo->lineEdit())
            return toolError(QStringLiteral("combo box '%1' is not editable; pick an entry with "
                                            "ui_click or ui_set_property (currentText)").arg(id));
        QLineEdit* line = combo->lineEdit();
        editor = line;
        readOnly = line->isReadOnly();
        selectAll = [line]() { line->selectAll(); };
    } else {
        return toolError(QStringLiteral("widget '%1' (%2) does not take text")
                             .arg(id, QString::fromLatin1(widget->metaObject()->className())));
    }
    if (readOnly)
        return toolError(QStringLiteral("widget '%1' is read-only").arg(id));

    QPointer<QWidget> guard(editor);
    if (args.value("replace").toBool(true)) {
        selectAll();
        // Delete instead of setText(): it fires the same signals as the user's key.
        sendKeyEvent(editor, Qt::Key_Delete, Qt::NoModifier, QString());
    }
    if (guard)
        typeInto(editor, args.value("text").toString());
    if (!guard)
        return toolError(QStringLiteral("widget '%1' was deleted while typing").arg(id));

    const bool submit = args.value("submit").toBool(false);
    if (submit) {
        QTimer::singleShot(0, editor, [editor]() {
            deliverKey(editor, Qt::Key_Return, Qt::NoModifier);
        });
    }
    // getWidget() reads the text the way every other tool does, password redaction included.
    QJsonObject result{{"id", id}, {"text", getWidget({{"id", id}}).value("text").toString()}};
    if (submit)
        result["submitted"] = QString::fromLatin1(kLookAgain);
    return toolOk(result);
}

QJsonObject AppInterface::pressKey(const QJsonObject& args)
{
    const QString keyName = args.value("key").toString();
    const QKeySequence sequence = QKeySequence::fromString(keyName, QKeySequence::PortableText);
    int combined = 0;
    if (sequence.count() == 1) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        combined = sequence[0].toCombined();
#else
        combined = sequence[0];
#endif
    }
    const int key = combined & ~int(Qt::KeyboardModifierMask);
    const Qt::KeyboardModifiers modifiers(combined & int(Qt::KeyboardModifierMask));
    if (key == 0 || key == Qt::Key_unknown)
        return toolError(QStringLiteral("'%1' is not a single key; give one key with optional "
                                        "modifiers, e.g. 'Enter', 'Escape', 'Ctrl+S', 'Down'")
                             .arg(keyName));

    QWidget* target = nullptr;
    QString id = args.value("id").toString();
    if (!id.isEmpty()) {
        const QJsonObject error = resolve(id, target);
        if (!target)
            return error;
    } else {
        QWidget* window = QApplication::activeWindow();
        if (!window || idForWidget(window).isEmpty()) {
            window = nullptr;
            for (QWidget* root : roots()) {
                if (root->isVisible() && !isExcluded(root)) {
                    window = root;
                    break;
                }
            }
        }
        if (!window)
            return toolError(QStringLiteral("no visible application window to send the key to"));
        target = window->focusWidget();
        // Keys meant for the application must not land in the chat.
        if (!target || isExcluded(target))
            target = window;
        id = idForWidget(target);
    }
    const QJsonObject unreachable = checkReachable(target, id);
    if (!unreachable.isEmpty())
        return unreachable;

    QTimer::singleShot(0, target, [target, key, modifiers]() {
        deliverKey(target, key, modifiers);
    });
    return toolOk({{"key", sequence.toString(QKeySequence::PortableText)},
                   {"target", id},
                   {"note", QString::fromLatin1(kLookAgain)}});
}

void AppInterface::waitFor(const QJsonObject& args, std::function<void(const QJsonObject&)> finish,
                           std::function<bool()> cancelled)
{
    const QString condition = args.value("condition").toString();
    if (!waitConditions().contains(condition)) {
        finish(toolError(QStringLiteral("unknown condition '%1'").arg(condition),
                         {{"allowed_values", QJsonArray::fromStringList(waitConditions())}}));
        return;
    }
    const QString id = args.value("id").toString();
    const bool windowOpened = condition == QLatin1String("window_opened");
    if (id.isEmpty() && !windowOpened) {
        finish(toolError(QStringLiteral("'%1' needs 'id'").arg(condition)));
        return;
    }
    const QString propertyName = args.value("property").toString();
    const QJsonValue expected = args.value("value");
    const bool propertyEquals = condition == QLatin1String("property_equals");
    if (propertyEquals && (propertyName.isEmpty() || expected.isUndefined())) {
        finish(toolError(QStringLiteral("property_equals needs 'property' and 'value'")));
        return;
    }
    const int timeoutMs = qBound(100, int(args.value("timeoutSeconds").toDouble(10) * 1000), 120000);

    QList<QPointer<QWidget>> before;
    for (QWidget* root : roots()) {
        if (root->isVisible())
            before.append(root);
    }

    // The current state; `met` tells whether the condition holds.
    auto check = [this, condition, id, propertyName, expected, propertyEquals, before](bool& met) {
        if (condition == QLatin1String("window_opened")) {
            QJsonArray open;
            for (QWidget* root : roots()) {
                if (!root->isVisible() || isExcluded(root))
                    continue;
                if (!before.contains(QPointer<QWidget>(root))) {
                    met = true;
                    const QString windowId = segmentFor(root);
                    m_ids.insert(windowId, root);
                    return QJsonObject{{"window", windowId}, {"title", root->windowTitle()}};
                }
                open.append(segmentFor(root));
            }
            met = false;
            return QJsonObject{{"open_windows", open}};
        }
        QWidget* widget = widgetForId(id);
        const bool exists = widget != nullptr;
        const bool visible = exists && widget->isVisible();
        const bool enabled = exists && widget->isEnabled();
        QJsonObject state{{"exists", exists}};
        if (exists) {
            state["visible"] = visible;
            state["enabled"] = enabled;
        }
        if (propertyEquals) {
            const QJsonValue current = exists
                ? getProperties({{"id", id}, {"names", QJsonArray{propertyName}}})
                      .value("properties").toObject().value(propertyName)
                : QJsonValue();
            if (exists)
                state["value"] = current;
            met = exists && sameValue(current, expected);
        } else if (condition == QLatin1String("visible")) {
            met = visible;
        } else if (condition == QLatin1String("hidden") || condition == QLatin1String("window_closed")) {
            met = !visible;
        } else if (condition == QLatin1String("enabled")) {
            met = enabled;
        } else if (condition == QLatin1String("disabled")) {
            met = exists && !enabled;
        } else if (condition == QLatin1String("exists")) {
            met = exists;
        } else {
            met = !exists;
        }
        return state;
    };

    if (propertyEquals) {
        QWidget* widget = widgetForId(id);
        if (widget && widget->metaObject()->indexOfProperty(propertyName.toLatin1().constData()) < 0) {
            finish(toolError(QStringLiteral("%1 has no property '%2'")
                                 .arg(QString::fromLatin1(widget->metaObject()->className()),
                                      propertyName)));
            return;
        }
    }

    QElapsedTimer clock;
    clock.start();
    // True when finished (met, timed out or cancelled).
    auto poll = [check, finish, cancelled, clock, timeoutMs, condition]() {
        if (cancelled && cancelled())
            return true;
        bool met = false;
        QJsonObject state = check(met);
        const qint64 waited = clock.elapsed();
        if (met) {
            state["waited_ms"] = waited;
            finish(toolOk(state));
            return true;
        }
        if (waited < timeoutMs)
            return false;
        finish(toolError(QStringLiteral("'%1' was not met within %2 s")
                             .arg(condition).arg(timeoutMs / 1000.0),
                         {{"waited_ms", waited}, {"state", state}}));
        return true;
    };
    if (poll())
        return;
    // Parented to this object: the polling stops when the AppInterface goes.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, timer, [timer, poll]() {
        if (poll()) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start(100);
}

} // namespace QtLLM
