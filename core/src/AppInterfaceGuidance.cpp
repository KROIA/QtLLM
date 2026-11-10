// Guidance tools of AppInterface: the model points at things for the user -
// highlights, shapes, speech bubbles, guided tours. The drawing itself lives in
// GuideOverlay and SpeechBubble; this file only turns tool calls into calls on
// those classes, behind the capability switches.
#include "AppInterface.h"
#include "Client.h"
#include "GuideOverlay.h"
#include "SpeechBubble.h"
#include "Tool.h"
#include "ToolResult.h"

#include <QJsonArray>

namespace QtLLM
{

namespace
{

const char* const kGroup = "App UI guidance";

QJsonObject pointSchema(const char* description)
{
    return QJsonObject{
        {"type", "object"},
        {"description", description},
        {"properties", QJsonObject{{"x", QJsonObject{{"type", "number"}}},
                                   {"y", QJsonObject{{"type", "number"}}}}},
        {"required", QJsonArray{"x", "y"}}};
}

QJsonObject animationSchema()
{
    return QJsonObject{
        {"type", "object"},
        {"description",
         "Optional animation. blink: switches on and off; fade: fades in (fadeOut also fades out "
         "at the end); pulse: the outline breathes; colorLoop: cycles through 'colors'; "
         "marchingAnts: a moving dashed outline; drawOn: a line or arrow grows from start to end. "
         "Use sparingly - one animated item draws the eye, several are noise."},
        {"properties", QJsonObject{
            {"type", QJsonObject{{"type", "string"},
                                 {"enum", QJsonArray{"blink", "fade", "pulse", "colorLoop",
                                                     "marchingAnts", "drawOn"}}}},
            {"periodMs", QJsonObject{{"type", "integer"},
                                     {"description", "Length of one cycle in milliseconds (default 1000)."}}},
            {"count", QJsonObject{{"type", "integer"},
                                  {"description", "Number of cycles; 0 (default) repeats until removed."}}},
            {"colors", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}},
                                   {"description", "Colours for colorLoop, e.g. [\"#ff8800\", \"#0088ff\"]."}}},
            {"fadeOut", QJsonObject{{"type", "boolean"}}}}},
        {"required", QJsonArray{"type"}}};
}

SpeechBubble::Placement placementFromString(const QString& name)
{
    if (name == QLatin1String("right")) return SpeechBubble::Placement::Right;
    if (name == QLatin1String("below")) return SpeechBubble::Placement::Below;
    if (name == QLatin1String("left"))  return SpeechBubble::Placement::Left;
    if (name == QLatin1String("above")) return SpeechBubble::Placement::Above;
    return SpeechBubble::Placement::Auto;
}

} // namespace

void AppInterface::syncGuidanceTools()
{
    if (!m_client) {
        m_guidanceTools.clear();
        return;
    }

    // The tool set depends on several switches at once (Animations changes the
    // schema of two tools), so it is rebuilt as a whole.
    for (const QString& name : m_guidanceTools)
        m_client->unregisterTool(name);
    m_guidanceTools.clear();

    const bool highlights = isCapabilityEnabled(Capability::Highlights);
    const bool shapes     = isCapabilityEnabled(Capability::Shapes);
    const bool animations = isCapabilityEnabled(Capability::Animations);
    const bool bubbles    = isCapabilityEnabled(Capability::SpeechBubbles);
    const bool tours      = isCapabilityEnabled(Capability::Tours);

    auto add = [this](Tool tool, ToolHandler handler) {
        // These tools only draw on top of the application; they change
        // nothing in it, so a consent dialog need not ask about them.
        tool.setGroup(QString::fromLatin1(kGroup)).setReadOnly(true);
        m_client->registerTool(tool, std::move(handler));
        m_guidanceTools.append(tool.name());
    };

    if (highlights) {
        Tool tool;
        tool.setName("ui_highlight_widget")
            .setTitle("Highlight widget")
            .setDescription(
                "Draw an outline around a widget to show the user where something is. Use it "
                "whenever you refer to a control the user has to find or click. The outline "
                "follows the widget and disappears with it. Returns the overlay id; remove it "
                "with ui_remove_overlay once it has served its purpose, or give durationMs.")
            .addParameter("id", "string", "Widget id from ui_get_widget_tree or ui_find_widgets.", true)
            .addParameter("color", "string", "Outline colour, e.g. \"#ff8800\" (default orange).", false)
            .addParameter("durationMs", "integer",
                          "Remove the highlight after this many milliseconds; 0 (default) keeps it "
                          "until removed. At most 300000.", false);
        if (animations)
            tool.addRawParameter("animation", animationSchema(), false);
        add(tool, [this](const QJsonObject& args) { return highlightWidget(args); });
    }

    if (shapes) {
        Tool tool;
        tool.setName("ui_draw_shape")
            .setTitle("Draw shape")
            .setDescription(
                "Draw a shape on top of a window: a frame around an area, a line or arrow from "
                "one place to another, a circle, or a spotlight that dims everything except the "
                "target. Give 'target' to attach the shape to a widget (it then follows the "
                "widget), or 'rect' / 'from' + 'to' in window coordinates (see the geometry "
                "values of ui_get_widget_tree). An arrow with 'from' and 'target' points at the "
                "widget. Returns the overlay id.")
            .addEnumParameter("shape", {"rect", "roundedRect", "ellipse", "line", "arrow", "spotlight"},
                              "What to draw.", true)
            .addParameter("target", "string", "Widget id to attach the shape to.", false)
            .addRawParameter("rect", QJsonObject{
                {"type", "object"},
                {"description", "Area in window coordinates, when there is no target."},
                {"properties", QJsonObject{{"x", QJsonObject{{"type", "number"}}},
                                           {"y", QJsonObject{{"type", "number"}}},
                                           {"width", QJsonObject{{"type", "number"}}},
                                           {"height", QJsonObject{{"type", "number"}}}}},
                {"required", QJsonArray{"x", "y", "width", "height"}}}, false)
            .addRawParameter("from", pointSchema("Start of a line or arrow, window coordinates."), false)
            .addRawParameter("to", pointSchema("End of a line or arrow, window coordinates."), false)
            .addParameter("color", "string", "Stroke colour, e.g. \"#ff8800\".", false)
            .addParameter("fill", "string", "Fill colour; use #aarrggbb for a translucent fill.", false)
            .addParameter("strokeWidth", "number", "Line width in pixels (default 2).", false)
            .addParameter("durationMs", "integer",
                          "Remove the shape after this many milliseconds; 0 (default) keeps it.", false);
        if (animations)
            tool.addRawParameter("animation", animationSchema(), false);
        add(tool, [this](const QJsonObject& args) { return drawShape(args); });
    }

    if (highlights || shapes) {
        Tool removeTool;
        removeTool.setName("ui_remove_overlay")
            .setTitle("Remove overlay")
            .setDescription("Remove one highlight or shape by the id returned when it was drawn.")
            .addParameter("id", "string", "Overlay id.", true);
        add(removeTool, [this](const QJsonObject& args) { return removeOverlay(args); });

        Tool clearTool;
        clearTool.setName("ui_clear_overlays")
            .setTitle("Clear overlays")
            .setDescription("Remove every highlight and shape. Do this when the explanation they "
                            "belonged to is finished.");
        add(clearTool, [this](const QJsonObject& args) { return clearOverlays(args); });
    }

    if (bubbles) {
        Tool tool;
        tool.setName("ui_show_bubble")
            .setTitle("Show speech bubble")
            .setDescription(
                "Show a speech bubble that points at a widget (or at a point in the window) and "
                "explains something about it, right where it is. Keep the text short - one or two "
                "sentences. The user can move the bubble and close it with its x button. Returns "
                "the bubble id. For several steps in a row use ui_show_tour instead.")
            .addParameter("text", "string", "The explanation shown in the bubble.", true)
            .addParameter("target", "string", "Widget id the bubble points at.", false)
            .addRawParameter("point", pointSchema("Where to point when there is no target widget, "
                                                  "in window coordinates."), false)
            .addParameter("title", "string", "Optional short heading.", false)
            .addParameter("number", "integer", "Optional step number shown as a badge.", false)
            .addEnumParameter("placement", {"auto", "right", "below", "left", "above"},
                              "Side of the target the bubble sits on (default auto: wherever there "
                              "is room).", false);
        add(tool, [this](const QJsonObject& args) { return showBubble(args); });
    }

    if (tours) {
        Tool tool;
        tool.setName("ui_show_tour")
            .setTitle("Show guided tour")
            .setDescription(
                "Explain a procedure of several steps with numbered speech bubbles, each pointing "
                "at the widget its step concerns. mode 'all_at_once' shows every bubble at the "
                "same time (good for up to about four steps); 'one_at_a_time' shows the current "
                "step only, highlights its widget and lets the user walk through with Back / Next. "
                "All targets must be in the same window. Returns the tour id.")
            .addArrayParameter("steps", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"target", QJsonObject{{"type", "string"}, {"description", "Widget id of this step."}}},
                    {"text", QJsonObject{{"type", "string"}, {"description", "What to do or notice here."}}},
                    {"title", QJsonObject{{"type", "string"}}}}},
                {"required", QJsonArray{"target", "text"}}},
                "The steps, in order.", true)
            .addEnumParameter("mode", {"all_at_once", "one_at_a_time"},
                              "Default: all_at_once.", false);
        add(tool, [this](const QJsonObject& args) { return showTour(args); });
    }

    if (bubbles || tours) {
        Tool closeTool;
        closeTool.setName("ui_close_bubble")
            .setTitle("Close bubble or tour")
            .setDescription("Close one speech bubble or one tour by its id.")
            .addParameter("id", "string", "Bubble id or tour id.", true);
        add(closeTool, [this](const QJsonObject& args) { return closeBubble(args); });

        Tool clearTool;
        clearTool.setName("ui_clear_bubbles")
            .setTitle("Clear bubbles")
            .setDescription("Close every speech bubble and tour.");
        add(clearTool, [this](const QJsonObject& args) { return clearBubbles(args); });
    }

    if (highlights || shapes || bubbles || tours) {
        Tool tool;
        tool.setName("ui_list_guidance")
            .setTitle("List guidance")
            .setDescription(
                "List the highlights, shapes, bubbles and tours currently shown, and which "
                "bubbles the user has closed. Check before adding more, so the window does not "
                "fill up with leftovers.");
        add(tool, [this](const QJsonObject& args) { return listGuidance(args); });
    }
}

// ---------------------------------------------------------------- overlays

QJsonObject AppInterface::addOverlay(QWidget* window, QWidget* anchor, QJsonObject spec)
{
    // Without the Animations switch an animation is not offered in the schema;
    // one sent anyway is dropped rather than refused.
    if (!isCapabilityEnabled(Capability::Animations))
        spec.remove("animation");

    OverlayItem item;
    item.anchor = anchor;
    QString error;
    if (!GuideOverlay::overlayItemFromJson(spec, &item, &error))
        return toolError(error);

    const QString id = GuideOverlay::forWindow(window)->addItem(item);
    if (id.isEmpty())
        return toolError(QStringLiteral(
            "the overlay was not added: it needs a target widget or valid coordinates, and a "
            "window holds at most %1 overlays (free some with ui_clear_overlays)")
            .arg(int(GuideOverlay::kMaxItems)));
    return toolOk({{"overlay", id}});
}

QJsonObject AppInterface::highlightWidget(const QJsonObject& args)
{
    QWidget* widget = nullptr;
    const QJsonObject error = resolve(args.value("id").toString(), widget);
    if (!error.isEmpty())
        return error;

    QJsonObject spec = args;
    spec.remove("id");                      // the widget id, not an overlay id
    spec["shape"] = QStringLiteral("roundedRect");
    return addOverlay(widget->window(), widget, spec);
}

QJsonObject AppInterface::drawShape(const QJsonObject& args)
{
    QWidget* anchor = nullptr;
    if (args.contains("target")) {
        const QJsonObject error = resolve(args.value("target").toString(), anchor);
        if (!error.isEmpty())
            return error;
    }
    QWidget* window = anchor ? anchor->window() : roots().value(0);
    if (!window)
        return toolError(QStringLiteral("no window to draw on"));

    QJsonObject spec = args;
    spec.remove("target");
    spec.remove("id");
    return addOverlay(window, anchor, spec);
}

QJsonObject AppInterface::removeOverlay(const QJsonObject& args)
{
    const QString id = args.value("id").toString();
    for (QWidget* root : roots()) {
        if (GuideOverlay::forWindow(root)->removeItem(id))
            return toolOk();
    }
    return toolError(QStringLiteral("no overlay with id '%1' (it may have expired or its widget "
                                    "may be gone)").arg(id));
}

QJsonObject AppInterface::clearOverlays(const QJsonObject&)
{
    int removed = 0;
    for (QWidget* root : roots()) {
        GuideOverlay* overlay = GuideOverlay::forWindow(root);
        removed += overlay->itemIds().size();
        overlay->clear();
    }
    return toolOk({{"removed", removed}});
}

// ---------------------------------------------------------------- bubbles

QJsonObject AppInterface::showBubble(const QJsonObject& args)
{
    const QString text = args.value("text").toString();
    if (text.trimmed().isEmpty())
        return toolError(QStringLiteral("missing required parameter: text"));
    const QString title = args.value("title").toString();
    const int number = args.value("number").toInt(0);
    const SpeechBubble::Placement placement = placementFromString(args.value("placement").toString());

    QString id;
    if (args.contains("target")) {
        QWidget* target = nullptr;
        const QJsonObject error = resolve(args.value("target").toString(), target);
        if (!error.isEmpty())
            return error;
        id = BubbleManager::forWindow(target)->showBubble(target, text, title, number, placement);
    } else if (args.value("point").isObject()) {
        QWidget* window = roots().value(0);
        if (!window)
            return toolError(QStringLiteral("no window to show the bubble in"));
        const QJsonObject point = args.value("point").toObject();
        id = BubbleManager::forWindow(window)->showBubble(
            QPoint(point.value("x").toInt(), point.value("y").toInt()), text, title, number, placement);
    } else {
        return toolError(QStringLiteral("give either 'target' (a widget id) or 'point'"));
    }

    if (id.isEmpty())
        return toolError(QStringLiteral("the bubble could not be shown"));
    return toolOk({{"bubble", id}});
}

QJsonObject AppInterface::showTour(const QJsonObject& args)
{
    const QJsonArray stepsJson = args.value("steps").toArray();
    if (stepsJson.isEmpty())
        return toolError(QStringLiteral("missing required parameter: steps (at least one)"));

    QList<TourStep> steps;
    QWidget* window = nullptr;
    for (int i = 0; i < stepsJson.size(); ++i) {
        const QJsonObject stepJson = stepsJson[i].toObject();
        QWidget* target = nullptr;
        QJsonObject error = resolve(stepJson.value("target").toString(), target);
        if (!error.isEmpty()) {
            error["step"] = i + 1;
            return error;
        }
        if (!window)
            window = target->window();
        else if (target->window() != window)
            return toolError(QStringLiteral("step %1 is in a different window than step 1; a tour "
                                            "stays in one window").arg(i + 1));
        TourStep step;
        step.target = target;
        step.text   = stepJson.value("text").toString();
        step.title  = stepJson.value("title").toString();
        steps.append(step);
    }

    const BubbleManager::TourMode mode =
        args.value("mode").toString() == QLatin1String("one_at_a_time")
            ? BubbleManager::TourMode::OneAtATime
            : BubbleManager::TourMode::AllAtOnce;
    const QString id = BubbleManager::forWindow(window)->showTour(steps, mode);
    if (id.isEmpty())
        return toolError(QStringLiteral("the tour could not be shown"));
    return toolOk({{"tour", id}, {"steps", steps.size()}});
}

QJsonObject AppInterface::closeBubble(const QJsonObject& args)
{
    const QString id = args.value("id").toString();
    for (QWidget* root : roots()) {
        BubbleManager* manager = BubbleManager::forWindow(root);
        if (manager->closeTour(id) || manager->closeBubble(id))
            return toolOk();
    }
    return toolError(QStringLiteral("no bubble or tour with id '%1' (the user may have closed it)").arg(id));
}

QJsonObject AppInterface::clearBubbles(const QJsonObject&)
{
    for (QWidget* root : roots())
        BubbleManager::forWindow(root)->clear();
    return toolOk();
}

QJsonObject AppInterface::listGuidance(const QJsonObject&)
{
    QJsonArray overlays;
    QJsonArray bubbles;
    for (QWidget* root : roots()) {
        for (const QString& id : GuideOverlay::forWindow(root)->itemIds())
            overlays.append(id);
        for (const QJsonValue& entry : BubbleManager::forWindow(root)->describe())
            bubbles.append(entry);
    }
    return toolOk({{"overlays", overlays}, {"bubbles", bubbles}});
}

void AppInterface::clearGuidance()
{
    for (QWidget* root : roots()) {
        GuideOverlay::forWindow(root)->clear();
        BubbleManager::forWindow(root)->clear();
    }
}

void AppInterface::setAnimationsEnabled(bool enabled)
{
    GuideOverlay::setAnimationsEnabled(enabled);
}

} // namespace QtLLM
