> **Status: implemented in `13e7601`.** Kept as a record of the requirements;
> this is not open work. See `documentation/ToolUse.md` for the shipped API.

# Feature Request — QtLLM: generische Tool-Infrastruktur

**An:** QtLLM-Repo (`Projects\QtLLM`, `core/`)
**Von:** SwOptionEditor (AP6424) — erster Konsument der Tool-API
**Datum:** 2026-08-10
**Ziel:** Mechanismen, die der SwOptionEditor heute rund um `QtLLM::Client::registerTool` selbst gebaut hat, in die Bibliothek heben, damit sie nicht in jeder App neu entstehen.
**Grundregel:** ALLES additiv. Bestehende `registerTool`-Aufrufe und Handler dürfen nicht brechen. Neue Verhaltensänderungen sind default AUS.

---

## Kontext — was die Bibliothek heute hat

- `QtLLM::Tool` (`core/inc/Tool.h`): `setName`, `setDescription`, `addParameter(name, type, description, required)`, `toApiObject()` (Claude, `input_schema`, Tool.cpp:43-72), `toOpenAiApiObject()` (Ollama/OpenAI, Tool.cpp:74-105).
- `QtLLM::Client` (`core/inc/Client.h`): `registerTool(Tool, ToolHandler)` (:49), `registerTool(name, description, rawSchema, ToolHandler)` (:52-55), `unregisterTool(name)` (:57), `setSystemPrompt` (:46), Signale `toolInvoked`/`toolCompleted` (:85-99), intern `QMap<QString, RegisteredTool> m_tools` (:118) + `syncToolsToProtocol()` (Client.cpp:147-159).
- `using ToolHandler = std::function<QJsonObject(const QJsonObject&)>` (`core/inc/ProtocolBase.h:15`).
- Dispatch in `ClaudeProtocol.cpp:355-396`; die Konvention `result["status"] == "error"` → `tool_result.is_error` existiert dort bereits (:388-389).

Nicht vorhanden: Enum im Schema, Parametervalidierung, Enable/Disable ohne Deregistrierung, Tool-Metadaten, Consent-Hook, Call-Cap, Exception-Schutz. Grep über `core/`, `documentation/`, `README.md` nach `TODO|FIXME|catalog|permission|consent|toolGroup`: kein Treffer (einziger TODO ist `UsageHistory.h:48`, unrelated).

---

## FR-1 — Enum-Parameter im `Tool`-Builder

**Problem:** `Tool::addParameter` erzeugt pro Property nur `type` + `description` (Tool.cpp:52-60, :86-94). Kein `enum`. Jedes Tool mit fester Wertemenge (im SwOptionEditor z.B. Objekttyp-Auswahl) muss deshalb den Raw-Schema-Overload `registerTool(name, description, QJsonObject, handler)` nehmen und das komplette JSON-Schema von Hand bauen. Die App führt dafür ein eigenes Feld `ToolDef::customSchema` mit einem separaten Registrierungszweig.

**Vorschlag:**

```cpp
// Tool.h
Tool& addParameter(const QString& name,
                   const QString& type,
                   const QString& description,
                   bool required = false,
                   const QStringList& enumValues = {});
```

`enumValues` nicht leer → `"enum": [...]` in beide Schema-Ausgaben (`toApiObject`, `toOpenAiApiObject`).

**Optional, falls Default-Argument-Kette unschön wird:** zusätzliche Überladung `Tool& addEnumParameter(const QString& name, const QStringList& allowed, const QString& description, bool required = false)` (Typ implizit `"string"`).

**Akzeptanz:** Ein Tool mit Enum-Parameter lässt sich ohne Raw-Schema registrieren; erzeugtes Schema enthält `enum` für Claude und für OpenAI/Ollama.

---

## FR-2 — Parametervalidierung im `Tool`

**Problem:** Die App validiert Tool-Argumente selbst (`LLMManager::checkParams`, LLMManager.cpp:519-568, ~50 Zeilen): Pflichtfelder, Typprüfung (string/integer/number/boolean/array/object), Enum-Prüfung case-insensitiv, und ein Fehlerobjekt mit `expected`-Map bzw. `allowed_values`. Das ist reine Schema-Arbeit — das Schema kennt die Bibliothek, nicht die App. Jede weitere App würde das identisch nachbauen.

**Vorschlag:**

```cpp
// Tool.h — leeres Objekt = gültig, sonst fertiges Fehlerobjekt
QJsonObject validate(const QJsonObject& args) const;

// Client.h — default false (kein Verhaltens-Change für bestehende Nutzer)
void setValidateToolInput(bool enabled);
bool validateToolInput() const;
```

Ist `setValidateToolInput(true)` gesetzt, prüft der Client vor dem Handler-Aufruf und liefert bei Verstoß direkt `{status:"error", message, expected|allowed_values}` als `tool_result` zurück, ohne den Handler zu rufen.

Fehlerformat als Referenz (aus der App, gerne übernehmen):

```json
{"status": "error", "message": "missing required parameter: id",
 "expected": {"id": "integer", "fields": "object"}}
{"status": "error", "message": "invalid value for 'type'",
 "allowed_values": ["SwOption", "Property", "AssignmentTable"]}
```

Enum-Vergleich sollte case-insensitiv sein — Modelle liefern Werte oft in abweichender Schreibweise.

**Akzeptanz:** Bei aktivierter Validierung sieht der Handler nur schema-konforme Argumente; bei deaktivierter Validierung ist das Verhalten bit-identisch zu heute.

---

## FR-3 — Exception-Schutz um den Handler-Aufruf

**Problem/Bug:** `ClaudeProtocol.cpp:378` ruft den Handler ohne `try`/`catch`. Eine Exception im Handler fliegt durch die Netzwerk-Callback-Kette. Die App fängt das heute selbst in einem Wrapper ab (`makeWrappedHandler`, LLMManager.cpp:1035-1050). Das ist Robustheit der Bibliothek, keine App-Aufgabe.

**Vorschlag:** Handler-Aufruf in allen Protokoll-Implementierungen (`ClaudeProtocol`, `OllamaProtocol`) in `try { ... } catch (const std::exception& e) { ... } catch (...) { ... }` kapseln, Ergebnis `{"status":"error","message":"Internal error: <what>"}`. Damit greift automatisch die vorhandene `is_error`-Konvention (ClaudeProtocol.cpp:388-389).

**Akzeptanz:** Ein Handler, der wirft, führt zu einem `is_error`-Toolergebnis; die Konversation läuft weiter, der Prozess stürzt nicht ab.

---

## FR-4 — Tool-Call-Cap pro Turn

**Problem:** Ohne Obergrenze kann ein Modell in einer Endlosschleife Tools aufrufen. Die App zählt selbst (`kMaxToolCallsPerTurn = 40`, LLMManager.h:213, Zähler in `makeWrappedHandler` LLMManager.cpp:1007, Reset in `onUserMessage`). `ClaudeProtocol.cpp:376` zählt Tool-Aufrufe bereits intern.

**Vorschlag:**

```cpp
// Client.h — 0 = unbegrenzt (Default, heutiges Verhalten)
void setMaxToolCallsPerTurn(int maxCalls);
int  maxToolCallsPerTurn() const;
```

Bei Überschreitung: kein Handler-Aufruf mehr, `tool_result` mit `{status:"error", message:"tool call limit reached"}`, zusätzlich ein Signal (z.B. `toolCallLimitReached(int limit)`), damit die App es dem Nutzer anzeigen kann. Reset bei jedem neuen User-Turn.

**Akzeptanz:** Mit Cap 3 werden im selben Turn maximal 3 Handler ausgeführt; ohne Cap ändert sich nichts.

---

## FR-5 — Tools aktivieren/deaktivieren ohne Deregistrierung

**Problem (wichtigster Punkt).** Der SwOptionEditor blendet Tools situativ aus: Mutations-Tools nur im Bearbeiten-Modus, und ab dem laufenden Ausbau zusätzlich pro Tool per Benutzereinstellung (rund 78 Tools, einzeln an-/abschaltbar, plus temporäre Freigaben „nur diese Sitzung"). Weil die Bibliothek nur `registerTool`/`unregisterTool` kennt, entsteht ein register/unregister-Ping-Pong samt eigener Buchführung (`m_registeredTools`, `applyToolAvailability()`, LLMManager.cpp:1080-1100) — inklusive der Notwendigkeit, `Tool`-Definition und Handler dauerhaft in der App vorzuhalten, nur um sie später erneut registrieren zu können.

**Vorschlag:**

```cpp
// Client.h
void setToolEnabled(const QString& toolName, bool enabled);
bool isToolEnabled(const QString& toolName) const;
QStringList toolNames() const;              // alle registrierten
QStringList enabledToolNames() const;       // die aktuell an das Modell gemeldeten
```

Deaktivierte Tools bleiben in `m_tools` (Client.h:118) samt Handler, werden aber in `syncToolsToProtocol()` (Client.cpp:147-159) beim Aufbau der Schema-Liste übersprungen.

**Kritisch:** `setToolEnabled` muss zwingend `syncToolsToProtocol()` auslösen. Andernfalls entsteht eine stille Inkonsistenz zwischen gemeldeter Schema-Liste und Handler-Map, und der Nutzer landet in `ClaudeProtocol.cpp:364-373` („No handler registered for tool: …").

**Zweitrangig, aber nützlich:** Ein deaktiviertes Tool, das trotzdem aufgerufen wird (Modell nutzt eine gecachte Toolliste), sollte `{status:"error", message:"tool is disabled"}` liefern statt „unknown tool" — die Fehlermeldung ist für das Modell handlungsleitend.

**Akzeptanz:** `setToolEnabled(name, false)` entfernt das Tool aus dem nächsten Request-Schema, ohne dass Definition oder Handler verloren gehen; `true` bringt es unverändert zurück.

---

## FR-6 — Tool-Metadaten für Bedien-Oberflächen

**Problem:** Für ein Einstellungsfenster „welche Tools darf der Assistent nutzen" braucht man mehr als `name` + `description` (die `description` ist Modell-Text, meist englisch und technisch). Der SwOptionEditor pflegt dafür einen separaten Katalog mit Anzeigetitel, Gruppe, Kurzbeschreibung, „Was es tut / Was es braucht / Was es liefert" und Statustext. Titel/Gruppe/Statustext sind generische UI-Bedürfnisse; die Fließtexte sind es weniger.

**Vorschlag (bewusst schlank):**

```cpp
// Tool.h — reine Trägerfelder, von der Bibliothek nie an das Modell geschickt
Tool& setTitle(const QString&);        // Anzeigename für UI
Tool& setGroup(const QString&);        // Gruppierung für UI
Tool& setStatusText(const QString&);   // "Suche Objekte…" während der Ausführung
QString title() const; QString group() const; QString statusText() const;

// Client.h
QList<Tool> registeredTools() const;   // Basis für ein generisches Tool-Settings-Widget
```

**Bonus (FR-6b):** Wenn `statusText` gesetzt ist, kann der Client ihn bei `toolInvoked` selbst im `ChatDockWidget` anzeigen — das Widget gehört ohnehin der Bibliothek. Heute macht das jede App von Hand.

**Grenze:** Bitte keine Pflicht-Kopplung. Der App-seitige Katalog ist absichtlich frei von QtLLM-Abhängigkeiten (Kommentar `LLMToolCatalog.h:9-10`), damit ein Einstellungsdialog die Toolliste auch dann aufbauen kann, wenn gar kein LLM-Stack existiert. Die Metadaten-Felder sollen optional bleiben, nicht zur einzigen Quelle werden. Ein mitgeliefertes generisches Settings-Widget wäre willkommen, aber nur als Angebot.

---

## FR-7 — Consent-Hook vor der Tool-Ausführung

**Problem:** Für sicherheitsrelevante Tools (löschen, exportieren) will die App den Nutzer fragen, bevor der Handler läuft — Muster „accept/decline" wie in Claude Code. Heute muss jede App das komplett selbst um den Handler herumbauen.

**Vorschlag:**

```cpp
// Client.h — Default: kein Handler gesetzt = alles erlaubt (heutiges Verhalten)
using ToolConsentHandler = std::function<bool(const QString& toolName, const QJsonObject& input)>;
void setToolConsentHandler(ToolConsentHandler handler);
```

Wird vor dem Tool-Handler aufgerufen. Rückgabe `false` → Handler wird nicht ausgeführt, Ergebnis `{status:"error", message:"user declined"}`. Aufruf synchron im GUI-Thread (wie Tool-Handler auch, siehe `documentation/ToolUse.md:74`), damit ein modaler Dialog möglich ist.

**Akzeptanz:** Ohne gesetzten Consent-Handler ist das Verhalten unverändert; mit Handler und `false` läuft der Tool-Handler nachweislich nicht.

---

## FR-8 — Dokumentation & Helfer für die Ergebniskonvention

**Problem:** Die Konvention `{"status":"error", ...}` → `tool_result.is_error` ist bereits implementiert (ClaudeProtocol.cpp:388-389), steht aber in keiner Doku. Jede App erfindet ihr Ergebnisformat neu.

**Vorschlag:**

```cpp
// z.B. ToolResult.h
namespace QtLLM {
    QJsonObject toolError(const QString& message, const QJsonObject& extra = {});
    QJsonObject toolOk(const QJsonObject& payload = {});
}
```

Plus ein Abschnitt in `documentation/ToolUse.md`: Ergebniskonvention, Fehlerformat, Threading-Zusage (Handler synchron im Event-Loop-Thread), sowie ein Hinweis zum Prompt-Cache (siehe unten).

---

## Querschnittliche Hinweise

**Prompt-Cache.** `ClaudeProtocol::buildRequestBody()` (ClaudeProtocol.cpp:142-163) legt `tools` und `system` in den gecachten Prefix (`cache_control: ephemeral`). Jede Änderung an der Toolliste — heute durch register/unregister, künftig durch `setToolEnabled` — invalidiert diesen Prefix. Das ist kein neues Problem, sollte aber in `ToolUse.md` stehen, damit Apps Umschaltvorgänge nicht unnötig oft auslösen.

**`setSystemPrompt` mitten im Gespräch.** `Client.cpp:95` setzt nur Feld + Protokoll, ohne History-Reset; wirkt ab dem nächsten Request. Das ist genau das gewünschte Verhalten für dynamische Prompt-Abschnitte (die App baut daraus eine Sektion „aktuell deaktivierte Tools"). Bitte in `ToolUse.md` als Zusage festhalten, damit sich Apps darauf verlassen können.

**Zeitpunkt.** Der SwOptionEditor befindet sich gerade mitten in einem Umbau von `LLMManager` (Per-Tool-Gating, Tool-Katalog). Die App-seitige Migration auf die neue Bibliotheks-API erfolgt erst nach Abschluss dieses Umbaus. Die Bibliotheksarbeit kann unabhängig davon starten — sie ist rein additiv.

---

## Priorisierung aus Sicht des Konsumenten

| Prio | FR | Begründung |
|---|---|---|
| 1 | FR-5 Enable/Disable | Entfernt die aufwendigste Eigenbau-Mechanik; ohne sie bleibt das register/unregister-Ping-Pong in jeder App |
| 2 | FR-3 Exception-Schutz | Robustheitslücke in der Bibliothek, unabhängig vom Rest |
| 3 | FR-1 Enum + FR-2 Validierung | Zusammen ersetzen sie `customSchema` und `checkParams` (~50 Zeilen) vollständig |
| 4 | FR-7 Consent-Hook | Freigabe-Ablauf ist ein wiederkehrendes Muster |
| 5 | FR-4 Call-Cap | Kleine Ergänzung, Zähler existiert bereits |
| 6 | FR-6 Metadaten, FR-8 Doku/Helfer | Komfort, kein Blocker |

## Nicht-Ziele

Domänenlogik bleibt in der App: die konkreten Tool-Handler, App-Typen als Parameter (z.B. ID-Parsing in Domänentypen), die Kopplung von Tool-Verfügbarkeit an einen App-Zustand (z.B. Bearbeiten-Modus), lokalisierte Katalogtexte, Persistenz der Benutzereinstellungen, Inhalt des System-Prompts.

## Erwartete Wirkung beim Konsumenten

Rund 120 bis 150 Zeilen entfallen im SwOptionEditor: `checkParams()` (LLMManager.cpp:519-568), der Enum-/Raw-Schema-Zweig in `registerOneTool()` (:1058-1078), Validierung, Cap und try/catch aus `makeWrappedHandler()` (:997-1056), sowie die Registrierungs-Buchführung in `applyToolAvailability()` (:1080-1100).
