// Loads a Petri net specification from JSON into pn::Net.
//
// The loader never stops at the first problem: it collects every error and
// warning with a JSON-path-like location ("$.arcs[3].to") so a model author
// can fix a file in one pass. A net is only produced if there are no errors.
#pragma once

#include "engine/Json.h"
#include "model/Net.h"

#include <optional>
#include <string>
#include <vector>

namespace pn {

struct LoadIssue {
    enum class Severity { Warning, Error };

    Severity severity;
    std::string path;     // e.g. "$.markings.error[0].tokens.fault"
    std::string message;
};

struct LoadResult {
    std::optional<Net> net;  // set only when there are no errors
    std::vector<LoadIssue> issues;

    bool ok() const { return net.has_value(); }
    bool hasErrors() const;
    bool hasWarnings() const;

    // One line per issue: "error $.arcs[2].to: unknown node \"x\""
    std::string report() const;
};

class JsonNetLoader {
public:
    static LoadResult loadFile(const std::string &fileName);
    static LoadResult loadJson(const std::string &json);
    static LoadResult loadValue(const JsonValue &root);
};

}  // namespace pn
