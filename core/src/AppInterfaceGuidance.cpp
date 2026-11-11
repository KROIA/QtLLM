// Guidance tools of AppInterface: the model explains things in place - speech
// bubbles and guided tours. The drawing itself lives in SpeechBubble; this file
// only turns tool calls into calls on it, behind the capability switches.
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

    // Bubbles and tours share tools, so the set is rebuilt as a whole.
    for (const QString& name : m_guidanceTools)
        m_client->unregisterTool(name);
    m_guidanceTools.clear();

    const bool bubbles = isCapabilityEnabled(Capability::SpeechBubbles);
    const bool tours   = isCapabilityEnabled(Capability::Tours);

    auto add = [this](Tool tool, ToolHandler handler) {
        // These tools only draw on top of the application; they change
        // nothing in it, so a consent dialog need not ask about them.
        tool.setGroup(QString::fromLatin1(kGroup)).setReadOnly(true);
        m_client->registerTool(tool, std::move(handler));
        m_guidanceTools.append(tool.name());
    };

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

        Tool listTool;
        listTool.setName("ui_list_guidance")
            .setTitle("List guidance")
            .setDescription(
                "List the bubbles and tours currently shown, and which bubbles the user has "
                "closed. Check before adding more, so the window does not fill up with leftovers.");
        add(listTool, [this](const QJsonObject& args) { return listGuidance(args); });
    }
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
    QJsonArray bubbles;
    for (QWidget* root : roots()) {
        for (const QJsonValue& entry : BubbleManager::forWindow(root)->describe())
            bubbles.append(entry);
    }
    return toolOk({{"bubbles", bubbles}});
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
