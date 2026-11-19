#include "io/RunLogIO.h"

#include "engine/Json.h"

#include <filesystem>
#include <sstream>
#include <system_error>

namespace pn {

namespace {

constexpr int kFormatVersion = 1;

std::string headerLine(const std::string &fingerprint)
{
    JsonValue o = JsonValue::object();
    o.set("type", JsonValue::string("header"));
    o.set("format", JsonValue::string("eggembt-runlog"));
    o.set("version", JsonValue::number(kFormatVersion));
    o.set("graph", JsonValue::string(fingerprint));
    return o.dump() + "\n";
}

std::string runLine(const PersistedRun &r)
{
    JsonValue o = JsonValue::object();
    o.set("type", JsonValue::string("run"));
    o.set("initial", JsonValue::string(r.initialMarking));

    JsonValue steps = JsonValue::array();
    for (const std::string &t : r.transitions)
        steps.push(JsonValue::string(t));
    o.set("steps", steps);

    if (!r.events.empty()) {
        JsonValue events = JsonValue::array();
        for (const SutEvent &e : r.events) {
            JsonValue eo = JsonValue::object();
            eo.set("kind", JsonValue::string(toString(e.kind)));
            if (!e.label.empty())
                eo.set("label", JsonValue::string(e.label));
            if (e.timestampMs != 0)
                eo.set("t", JsonValue::number(static_cast<double>(e.timestampMs)));
            events.push(eo);
        }
        o.set("events", events);
    }

    o.set("outcome", JsonValue::string(toString(r.outcome)));
    if (!r.detail.empty())
        o.set("detail", JsonValue::string(r.detail));
    if (!r.note.empty())
        o.set("note", JsonValue::string(r.note));
    // GLM-5.3-Flash: which crash entry the run's spontaneous death was
    // attributed to (the rebuild cannot re-derive it from the recording).
    if (!r.crashAttribution.empty())
        o.set("crash_attribution", JsonValue::string(r.crashAttribution));
    if (!r.crashAttempts.empty()) {
        JsonValue attempts = JsonValue::array();
        for (const std::string &k : r.crashAttempts)
            attempts.push(JsonValue::string(k));
        o.set("crash_attempts", attempts);
    }

    // GLM-5.3-Flash: the command hooks this run executed (they travel with
    // the run so the report shows the evidence of every data check after a
    // restore; the commands are NOT re-executed on restore/replay).
    if (!r.hooks.empty()) {
        JsonValue hooks = JsonValue::array();
        for (const HookRecord &h : r.hooks) {
            JsonValue hobj = JsonValue::object();
            hobj.set("transition", JsonValue::string(h.transition));
            hobj.set("label", JsonValue::string(h.label));
            hobj.set("async", JsonValue::boolean(h.async));
            hobj.set("status", JsonValue::string(h.status));
            if (h.exit != kHookNoExit)
                hobj.set("exit", JsonValue::number(h.exit));
            hobj.set("output", JsonValue::string(h.output));
            hooks.push(hobj);
        }
        o.set("hooks", hooks);
    }

    if (!r.dumps.empty()) {
        JsonValue dumps = JsonValue::array();
        for (const DumpRecord &d : r.dumps) {
            JsonValue dobj = JsonValue::object();
            dobj.set("step", JsonValue::number(d.stepIndex));
            dobj.set("marking", JsonValue::string(d.marking));
            dobj.set("mode", JsonValue::string(toString(d.mode)));
            JsonValue files = JsonValue::array();
            for (const std::string &f : d.files)
                files.push(JsonValue::string(f));
            dobj.set("files", files);
            dobj.set("stack", JsonValue::string(d.stackText));
            dumps.push(dobj);
        }
        o.set("dumps", dumps);
    }
    return o.dump() + "\n";
}

std::string stringOf(const JsonValue &o, const char *key)
{
    const JsonValue *v = o.find(key);
    return v ? v->asString() : std::string();
}

bool runFromJson(const JsonValue &o, PersistedRun &r, std::string *err)
{
    const JsonValue *initial = o.find("initial");
    if (!initial || !initial->isString()) {
        *err = "missing string \"initial\"";
        return false;
    }
    r.initialMarking = initial->asString();

    const JsonValue *steps = o.find("steps");
    if (!steps || !steps->isArray()) {
        *err = "missing array \"steps\"";
        return false;
    }
    for (const JsonValue &v : steps->items()) {
        if (!v.isString()) {
            *err = "\"steps\" must contain only strings";
            return false;
        }
        r.transitions.push_back(v.asString());
    }

    if (const JsonValue *events = o.find("events")) {
        for (const JsonValue &ev : events->items()) {
            SutEvent e;
            if (!parseEventKind(stringOf(ev, "kind"), e.kind)) {
                *err = "unknown event \"kind\"";
                return false;
            }
            e.label = stringOf(ev, "label");
            if (const JsonValue *t = ev.find("t"))
                e.timestampMs = static_cast<std::int64_t>(t->asNumber());
            r.events.push_back(std::move(e));
        }
    }

    if (!parseOutcome(stringOf(o, "outcome"), r.outcome)) {
        *err = "missing or unknown \"outcome\"";
        return false;
    }
    r.detail = stringOf(o, "detail");
    r.note = stringOf(o, "note");
    r.crashAttribution = stringOf(o, "crash_attribution");  // GLM-5.3-Flash
    if (const JsonValue *attempts = o.find("crash_attempts"))
        for (const JsonValue &k : attempts->items())
            r.crashAttempts.push_back(k.asString());

    // GLM-5.3-Flash: the command hooks this run executed (adopted into the
    // rebuilt run records; never re-executed).
    if (const JsonValue *hooks = o.find("hooks")) {
        for (const JsonValue &hv : hooks->items()) {
            HookRecord rec;
            rec.transition = stringOf(hv, "transition");
            rec.label = stringOf(hv, "label");
            rec.async = hv.find("async") ? hv.find("async")->asBool() : false;
            rec.status = stringOf(hv, "status");
            if (const JsonValue *ex = hv.find("exit"))
                rec.exit = static_cast<int>(ex->asNumber());
            rec.output = stringOf(hv, "output");
            r.hooks.push_back(std::move(rec));
        }
    }

    if (const JsonValue *dumps = o.find("dumps")) {
        for (const JsonValue &dv : dumps->items()) {
            DumpRecord rec;
            if (const JsonValue *step = dv.find("step"))
                rec.stepIndex = static_cast<std::uint32_t>(step->asNumber());
            rec.marking = stringOf(dv, "marking");
            if (!parseDumpMode(stringOf(dv, "mode"), rec.mode)) {
                *err = "unknown dump \"mode\"";
                return false;
            }
            if (const JsonValue *files = dv.find("files"))
                for (const JsonValue &f : files->items())
                    rec.files.push_back(f.asString());
            rec.stackText = stringOf(dv, "stack");
            r.dumps.push_back(std::move(rec));
        }
    }
    return true;
}

// Parses the header line; returns the fingerprint in `graph`.
bool parseHeader(const std::string &line, std::string &graph, std::string *err)
{
    JsonValue o;
    if (!JsonValue::parse(line, o) || !o.isObject()) {
        *err = "first line is not a valid header";
        return false;
    }
    // DeepSeek V4.1 Flash: logs written before the rename say "mbt-runlog"; they
    // are still readable, new logs use the new tag.
    const std::string format = stringOf(o, "format");
    if (stringOf(o, "type") != "header" || (format != "eggembt-runlog" && format != "mbt-runlog")) {
        *err = "not an eggembt run log";
        return false;
    }
    const JsonValue *version = o.find("version");
    if (!version || static_cast<int>(version->asNumber()) != kFormatVersion) {
        *err = "unsupported run log version";
        return false;
    }
    graph = stringOf(o, "graph");
    return true;
}

bool isBlank(const std::string &s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool readWhole(const std::string &path, std::string &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

}  // namespace

RunLogReadResult readRunLog(const std::string &path)
{
    RunLogReadResult result;
    std::string data;
    if (!readWhole(path, data)) {
        result.error = "cannot open " + path;
        return result;
    }

    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (start <= data.size()) {
            const std::size_t nl = data.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(data.substr(start));
                break;
            }
            lines.push_back(data.substr(start, nl - start));
            start = nl + 1;
        }
    }

    // Index of the last non-empty line, the only one allowed to be truncated.
    int last = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        if (!isBlank(lines[static_cast<std::size_t>(i)]))
            last = i;
    if (last < 0) {
        result.error = path + " is empty";
        return result;
    }

    bool haveHeader = false;
    for (int i = 0; i <= last; ++i) {
        const std::string &line = lines[static_cast<std::size_t>(i)];
        if (isBlank(line))
            continue;
        const std::string lineNo = std::to_string(i + 1);

        if (!haveHeader) {
            std::string err;
            if (!parseHeader(line, result.contents.fingerprint, &err)) {
                result.error = "line " + lineNo + ": " + err;
                return result;
            }
            haveHeader = true;
            continue;
        }

        JsonValue o;
        if (!JsonValue::parse(line, o) || !o.isObject()) {
            if (i == last) {
                result.contents.warnings.push_back("line " + lineNo + ": ignored truncated last line");
                break;
            }
            result.error = "line " + lineNo + ": invalid JSON";
            return result;
        }
        const std::string type = stringOf(o, "type");
        if (type != "run") {
            result.contents.warnings.push_back("line " + lineNo + ": ignored unknown record type \"" + type + "\"");
            continue;
        }
        PersistedRun run;
        std::string err;
        if (!runFromJson(o, run, &err)) {
            result.error = "line " + lineNo + ": " + err;
            return result;
        }
        result.contents.runs.push_back(std::move(run));
    }
    result.ok = true;
    return result;
}

bool RunLogWriter::open(const std::string &path, const std::string &fingerprint, std::string *error)
{
    close();
    std::error_code ec;
    const bool existing = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0;
    bool needsNewline = false;

    if (existing) {
        std::string all;
        if (!readWhole(path, all)) {
            if (error)
                *error = "cannot read " + path;
            return false;
        }
        const std::size_t firstNl = all.find('\n');
        std::string graph;
        std::string err;
        if (!parseHeader(all.substr(0, firstNl), graph, &err)) {
            if (error)
                *error = path + ": " + err;
            return false;
        }
        if (graph != fingerprint) {
            if (error)
                *error = path + " belongs to a different model; refusing to append";
            return false;
        }
        // An interrupted append leaves a partial last line without a trailing
        // newline. That run never finished, so drop the fragment instead of
        // gluing the next run onto it.
        if (all.back() != '\n') {
            const std::size_t pos = all.rfind('\n');
            if (pos != std::string::npos) {
                std::filesystem::resize_file(path, pos + 1, ec);
                if (ec) {
                    if (error)
                        *error = "cannot repair " + path + ": " + ec.message();
                    return false;
                }
            } else {
                needsNewline = true;  // only a header without its newline
            }
        }
    }

    file_.open(path, std::ios::binary | std::ios::app);
    if (!file_) {
        if (error)
            *error = "cannot open " + path + " for writing";
        return false;
    }
    if (!existing)
        file_ << headerLine(fingerprint);
    else if (needsNewline)
        file_ << '\n';
    file_.flush();
    if (!file_) {
        if (error)
            *error = "cannot write " + path;
        file_.close();
        return false;
    }
    return true;
}

bool RunLogWriter::append(const PersistedRun &run, std::string *error)
{
    if (!file_.is_open()) {
        if (error)
            *error = "run log is not open";
        return false;
    }
    file_ << runLine(run);
    file_.flush();
    if (!file_) {
        if (error)
            *error = "cannot write run log";
        return false;
    }
    return true;
}

void RunLogWriter::close()
{
    if (file_.is_open())
        file_.close();
    file_.clear();
}

}  // namespace pn
