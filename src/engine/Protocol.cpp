#include "engine/Protocol.h"

#include "engine/Json.h"

namespace pn {

namespace {

const char *typeName(ProtocolMessage::Type t)
{
    switch (t) {
    case ProtocolMessage::Type::Hello:            return "hello";
    case ProtocolMessage::Type::RunStarted:       return "run_started";
    case ProtocolMessage::Type::Event:            return "event";
    case ProtocolMessage::Type::PostMortemResult: return "post_mortem_result";
    case ProtocolMessage::Type::Fault:            return "fault";
    case ProtocolMessage::Type::StartRun:         return "start_run";
    case ProtocolMessage::Type::Stimulate:        return "stimulate";
    case ProtocolMessage::Type::PostMortem:       return "post_mortem";
    case ProtocolMessage::Type::StopRun:          return "stop_run";
    case ProtocolMessage::Type::Shutdown:         return "shutdown";
    }
    return "shutdown";
}

bool parseType(const std::string &s, ProtocolMessage::Type &out)
{
    using T = ProtocolMessage::Type;
    for (T t : {T::Hello, T::RunStarted, T::Event, T::PostMortemResult, T::Fault, T::StartRun, T::Stimulate,
                T::PostMortem, T::StopRun, T::Shutdown}) {
        if (s == typeName(t)) {
            out = t;
            return true;
        }
    }
    return false;
}

bool setError(std::string *error, const std::string &text)
{
    if (error)
        *error = text;
    return false;
}

// Mandatory-field helpers: they report the field name on failure.
bool needString(const JsonValue &o, const char *key, std::string &out, std::string *error)
{
    const JsonValue *v = o.find(key);
    if (!v || !v->isString())
        return setError(error, std::string("missing string field \"") + key + "\"");
    out = v->asString();
    return true;
}

bool needBool(const JsonValue &o, const char *key, bool &out, std::string *error)
{
    const JsonValue *v = o.find(key);
    if (!v || !v->isBool())
        return setError(error, std::string("missing boolean field \"") + key + "\"");
    out = v->asBool();
    return true;
}

bool needInt(const JsonValue &o, const char *key, std::int64_t &out, std::string *error)
{
    const JsonValue *v = o.find(key);
    if (!v || !v->isNumber())
        return setError(error, std::string("missing number field \"") + key + "\"");
    out = static_cast<std::int64_t>(v->asNumber());
    return true;
}

std::int64_t optInt(const JsonValue &o, const char *key, std::int64_t fallback)
{
    const JsonValue *v = o.find(key);
    return (v && v->isNumber()) ? static_cast<std::int64_t>(v->asNumber()) : fallback;
}

std::string optString(const JsonValue &o, const char *key)
{
    const JsonValue *v = o.find(key);
    return (v && v->isString()) ? v->asString() : std::string();
}

}  // namespace

std::string encodeMessage(const ProtocolMessage &m)
{
    using T = ProtocolMessage::Type;
    JsonValue o = JsonValue::object();
    o.set("type", JsonValue::string(typeName(m.type)));

    switch (m.type) {
    case T::Hello:
        o.set("protocol", JsonValue::number(m.protocol));
        o.set("name", JsonValue::string(m.name));
        o.set("snapshot", JsonValue::boolean(m.snapshot));
        o.set("crash", JsonValue::boolean(m.crash));
        o.set("restart_ms", JsonValue::number(static_cast<double>(m.restartMs)));
        break;
    case T::RunStarted:
        o.set("ok", JsonValue::boolean(m.ok));
        if (!m.error.empty())
            o.set("error", JsonValue::string(m.error));
        break;
    case T::Event:
        if (m.run != 0)
            o.set("run", JsonValue::number(static_cast<double>(m.run)));
        o.set("kind", JsonValue::string(toString(m.kind)));
        if (!m.label.empty())
            o.set("label", JsonValue::string(m.label));
        o.set("t", JsonValue::number(static_cast<double>(m.timestampMs)));
        break;
    case T::PostMortemResult: {
        o.set("id", JsonValue::number(static_cast<double>(m.id)));
        o.set("ok", JsonValue::boolean(m.ok));
        if (!m.error.empty())
            o.set("error", JsonValue::string(m.error));
        JsonValue files = JsonValue::array();
        for (const std::string &f : m.files)
            files.push(JsonValue::string(f));
        o.set("files", files);
        if (!m.stack.empty())
            o.set("stack", JsonValue::string(m.stack));
        break;
    }
    case T::Fault:
        o.set("message", JsonValue::string(m.error));
        break;
    case T::StartRun:
        o.set("run", JsonValue::number(static_cast<double>(m.run)));
        o.set("initial", JsonValue::string(m.initial));
        o.set("quiescence_ms", JsonValue::number(static_cast<double>(m.quiescenceMs)));
        break;
    case T::Stimulate:
        o.set("label", JsonValue::string(m.label));
        break;
    case T::PostMortem:
        o.set("id", JsonValue::number(static_cast<double>(m.id)));
        o.set("mode", JsonValue::string(m.mode == DumpMode::Crash ? "crash" : "snapshot"));
        break;
    case T::StopRun:
        o.set("reason", JsonValue::string(m.reason.empty() ? "finished" : m.reason));
        break;
    case T::Shutdown:
        break;
    }
    return o.dump();
}

bool decodeMessage(const std::string &line, ProtocolMessage &out, std::string *error)
{
    using T = ProtocolMessage::Type;
    JsonValue o;
    std::string parseError;
    if (!JsonValue::parse(line, o, &parseError))
        return setError(error, "malformed JSON: " + parseError);
    if (!o.isObject())
        return setError(error, "a message must be a JSON object");

    ProtocolMessage m;
    std::string typeText;
    if (!needString(o, "type", typeText, error))
        return false;
    if (!parseType(typeText, m.type))
        return setError(error, "unknown message type \"" + typeText + "\"");

    switch (m.type) {
    case T::Hello: {
        std::int64_t version = 0;
        if (!needInt(o, "protocol", version, error))
            return false;
        if (version != kProtocolVersion)
            return setError(error, "unsupported protocol version " + std::to_string(version));
        m.protocol = static_cast<int>(version);
        m.name = optString(o, "name");
        if (const JsonValue *v = o.find("snapshot"))
            m.snapshot = v->asBool();
        if (const JsonValue *v = o.find("crash"))
            m.crash = v->asBool();
        m.restartMs = optInt(o, "restart_ms", 0);
        break;
    }
    case T::RunStarted:
        if (!needBool(o, "ok", m.ok, error))
            return false;
        m.error = optString(o, "error");
        break;
    case T::Event: {
        std::string kind;
        if (!needString(o, "kind", kind, error))
            return false;
        if (!parseEventKind(kind, m.kind))
            return setError(error, "unknown event kind \"" + kind + "\"");
        m.run = optInt(o, "run", 0);
        m.label = optString(o, "label");
        if (m.kind != EventKind::Quiescent && m.label.empty())
            return setError(error, "an observed or stimulated event needs a \"label\"");
        m.timestampMs = optInt(o, "t", 0);
        break;
    }
    case T::PostMortemResult:
        if (!needInt(o, "id", m.id, error) || !needBool(o, "ok", m.ok, error))
            return false;
        m.error = optString(o, "error");
        m.stack = optString(o, "stack");
        if (const JsonValue *files = o.find("files")) {
            for (const JsonValue &f : files->items())
                if (f.isString())
                    m.files.push_back(f.asString());
        }
        break;
    case T::Fault:
        if (!needString(o, "message", m.error, error))
            return false;
        break;
    case T::StartRun:
        if (!needString(o, "initial", m.initial, error))
            return false;
        m.run = optInt(o, "run", 0);
        m.quiescenceMs = optInt(o, "quiescence_ms", 0);
        break;
    case T::Stimulate:
        if (!needString(o, "label", m.label, error))
            return false;
        break;
    case T::PostMortem: {
        std::string mode;
        if (!needInt(o, "id", m.id, error) || !needString(o, "mode", mode, error))
            return false;
        if (mode == "snapshot")
            m.mode = DumpMode::Snapshot;
        else if (mode == "crash")
            m.mode = DumpMode::Crash;
        else
            return setError(error, "unknown post-mortem mode \"" + mode + "\"");
        break;
    }
    case T::StopRun:
        m.reason = optString(o, "reason");
        if (m.reason.empty())
            m.reason = "finished";
        break;
    case T::Shutdown:
        break;
    }
    out = std::move(m);
    return true;
}

}  // namespace pn
