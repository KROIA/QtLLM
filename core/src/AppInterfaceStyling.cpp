// Styling tools of AppInterface: recolouring single widgets (with restore) and
// switching between the themes the application registered.
#include "AppInterface.h"
#include "ToolResult.h"

#include <QApplication>
#include <QColor>
#include <QJsonArray>
#include <QStyle>
#include <QVariant>
#include <QWidget>

namespace QtLLM
{

namespace
{

// Dynamic properties on a recoloured widget.
const char* const kColorId   = "qtllmColorId";         // unique; what the style rule selects on
const char* const kOrigStyle = "qtllmOriginalStyle";   // style sheet before the first change
const char* const kColors    = "qtllmColors";          // QVariantMap role -> colour in effect

const char* const kRoles[] = {"background", "text", "border"};

QString cssColor(const QColor& color)
{
    return color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
}

void restoreWidget(QWidget* widget)
{
    widget->setStyleSheet(widget->property(kOrigStyle).toString());
    widget->setProperty(kColorId, QVariant());
    widget->setProperty(kOrigStyle, QVariant());
    widget->setProperty(kColors, QVariant());
}

} // namespace

// ---------------------------------------------------------------- widget colours

QJsonObject AppInterface::setWidgetColors(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QString id = args.value("id").toString();
    const QJsonObject error = resolve(id, widget);
    if (!error.isEmpty())
        return error;

    // Colours go through QColor, so nothing but a colour can reach the style sheet.
    QVariantMap colors = widget->property(kColors).toMap();
    bool any = false;
    for (const char* role : kRoles) {
        const QString given = args.value(QLatin1String(role)).toString().trimmed();
        if (given.isEmpty())
            continue;
        const QColor color(given);
        if (!color.isValid())
            return toolError(QStringLiteral("'%1' is not a colour; use a CSS colour name or a hex "
                                            "value such as #ffcc00").arg(given));
        colors[QLatin1String(role)] = cssColor(color);
        any = true;
    }
    if (!any)
        return toolError(QStringLiteral("give at least one of background, text, border"));

    if (!widget->property(kColorId).isValid()) {
        widget->setProperty(kOrigStyle, widget->styleSheet());
        widget->setProperty(kColorId, QString::number(++m_colorSerial));
        m_colored.append(widget);
    }
    widget->setProperty(kColors, colors);

    // A style sheet without selector also applies to every child; the rule
    // below selects this one widget through its dynamic property.
    QString sheet = widget->property(kOrigStyle).toString();
    if (!sheet.trimmed().isEmpty() && !sheet.contains(QLatin1Char('{')))
        sheet = QStringLiteral("* { %1 }").arg(sheet);   // bare declarations cannot be mixed with rules
    QString rule;
    if (colors.contains("background"))
        rule += QStringLiteral("background-color: %1; ").arg(colors["background"].toString());
    if (colors.contains("text"))
        rule += QStringLiteral("color: %1; ").arg(colors["text"].toString());
    if (colors.contains("border"))
        rule += QStringLiteral("border: 2px solid %1; ").arg(colors["border"].toString());
    sheet += QStringLiteral("\n*[%1=\"%2\"] { %3}")
                 .arg(QLatin1String(kColorId), widget->property(kColorId).toString(), rule);
    widget->setStyleSheet(sheet);

    return toolOk({{"id", id}, {"colors", QJsonObject::fromVariantMap(colors)},
                   {"note", "restore with ui_restore_widget_colors"}});
}

QJsonObject AppInterface::restoreWidgetColors(const QJsonObject& args)
{
    const QString id = args.value("id").toString();
    if (id.isEmpty()) {
        int count = 0;
        for (const QPointer<QWidget>& widget : m_colored)
            count += widget ? 1 : 0;
        resetWidgetColors();
        return toolOk({{"restored", count}});
    }

    QWidget* widget = nullptr;
    const QJsonObject error = resolve(id, widget);
    if (!error.isEmpty())
        return error;
    if (!widget->property(kColorId).isValid())
        return toolOk({{"restored", 0}, {"note", "this widget's colours were not changed"}});
    restoreWidget(widget);
    m_colored.removeAll(QPointer<QWidget>(widget));
    return toolOk({{"restored", 1}});
}

void AppInterface::resetWidgetColors()
{
    for (const QPointer<QWidget>& widget : m_colored) {
        if (widget)
            restoreWidget(widget);
    }
    m_colored.clear();
}

// ---------------------------------------------------------------- themes

void AppInterface::registerTheme(const QString& name, const QString& description,
                                 std::function<void()> apply)
{
    if (name.isEmpty() || !apply)
        return;
    const Theme theme{name, description, std::move(apply)};
    bool replaced = false;
    for (Theme& existing : m_themes) {
        if (existing.name == name) {
            existing = theme;
            replaced = true;
        }
    }
    if (!replaced)
        m_themes.append(theme);
    // ui_set_theme lists the names, so it has to be refreshed.
    if (isCapabilityEnabled(Capability::Themes))
        registerCapability(Capability::Themes);
}

void AppInterface::registerTheme(const QString& name, const QString& description,
                                 const QPalette& palette, const QString& styleName,
                                 const QString& styleSheet)
{
    registerTheme(name, description, [palette, styleName, styleSheet]() {
        QApplication::setPalette(palette);
        if (!styleName.isEmpty())
            QApplication::setStyle(styleName);
        qApp->setStyleSheet(styleSheet);
        // Qt does not hand a new application palette on to the widgets below
        // a widget with a style sheet (a recoloured one, for instance); they
        // would keep the text colours of the previous theme.
        for (QWidget* widget : QApplication::allWidgets()) {
            if (!widget->styleSheet().isEmpty())
                widget->setStyleSheet(widget->styleSheet());
        }
    });
}

void AppInterface::registerStandardThemes()
{
    registerTheme(QStringLiteral("default"), QStringLiteral("The application's standard look."),
                  QApplication::palette(), QApplication::style()->objectName(),
                  qApp->styleSheet());
    registerTheme(QStringLiteral("dark"), QStringLiteral("Dark background with light text."),
                  darkPalette(), QStringLiteral("Fusion"));
    if (m_currentTheme.isEmpty())
        m_currentTheme = QStringLiteral("default");
}

QPalette AppInterface::darkPalette()
{
    const QColor window(45, 45, 48), base(30, 30, 30), text(220, 220, 220), disabled(128, 128, 128);
    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(60, 60, 64));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(86, 156, 214));
    palette.setColor(QPalette::Highlight, QColor(38, 110, 180));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, disabled);
    return palette;
}

QStringList AppInterface::themeNames() const
{
    QStringList names;
    for (const Theme& theme : m_themes)
        names.append(theme.name);
    return names;
}

bool AppInterface::setTheme(const QString& name)
{
    for (const Theme& theme : m_themes) {
        if (theme.name.compare(name, Qt::CaseInsensitive) != 0)
            continue;
        const std::function<void()> apply = theme.apply;   // apply may register themes
        const QString canonical = theme.name;
        apply();
        m_currentTheme = canonical;
        emit themeChanged(canonical);
        return true;
    }
    return false;
}

QJsonObject AppInterface::listThemes(const QJsonObject&)
{
    QJsonArray list;
    for (const Theme& theme : m_themes)
        list.append(QJsonObject{{"name", theme.name}, {"description", theme.description}});
    return toolOk({{"themes", list}, {"current", m_currentTheme}});
}

QJsonObject AppInterface::applyTheme(const QJsonObject& args)
{
    const QString name = args.value("theme").toString();
    if (!setTheme(name))
        return toolError(QStringLiteral("no theme '%1'; available: %2")
                             .arg(name, themeNames().join(QStringLiteral(", "))));
    return toolOk({{"theme", m_currentTheme}});
}

} // namespace QtLLM
