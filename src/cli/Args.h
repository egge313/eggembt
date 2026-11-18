// A small command-line argument parser: `--name value`, `--name=value`, flags,
// repeated options, `--` to end the options, a few short aliases.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace cli {

struct ArgSpec {
    std::set<std::string> flags;   // options without a value
    std::set<std::string> valued;  // options with a value
};

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> values;
    std::set<std::string> flags;

    bool has(const std::string &flag) const { return flags.count(flag) > 0; }
    bool given(const std::string &name) const { return values.count(name) > 0; }
    std::string get(const std::string &name, const std::string &fallback = {}) const
    {
        const auto it = values.find(name);
        return it == values.end() ? fallback : it->second.back();
    }
    const std::vector<std::string> &all(const std::string &name) const
    {
        static const std::vector<std::string> kNone;
        const auto it = values.find(name);
        return it == values.end() ? kNone : it->second;
    }

    // Leaves `out` unchanged if the option is absent. False (with `error`) if it is not an integer in [min, max].
    bool getInt(const std::string &name, long long &out, long long min, long long max, std::string *error) const
    {
        const auto it = values.find(name);
        if (it == values.end())
            return true;
        const std::string &text = it->second.back();
        char *end = nullptr;
        errno = 0;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0' || errno != 0 || v < min || v > max) {
            if (error)
                *error = "--" + name + " needs a whole number from " + std::to_string(min) + " to " +
                         std::to_string(max) + ", not \"" + text + "\"";
            return false;
        }
        out = v;
        return true;
    }

    // GLM-5.3-Flash: like getInt for a real number, bounds inclusive.
    bool getDouble(const std::string &name, double &out, double min, double max, std::string *error) const
    {
        const auto it = values.find(name);
        if (it == values.end())
            return true;
        const std::string &text = it->second.back();
        char *end = nullptr;
        errno = 0;
        const double v = std::strtod(text.c_str(), &end);
        if (text.empty() || *end != '\0' || errno == ERANGE || !(v >= min && v <= max)) {
            if (error) {
                char lo[16], hi[16];
                std::snprintf(lo, sizeof lo, "%g", min);
                std::snprintf(hi, sizeof hi, "%g", max);
                *error = "--" + name + " needs a number from " + lo + " to " + hi + ", not \"" + text + "\"";
            }
            return false;
        }
        out = v;
        return true;
    }
};

inline bool parseArgs(const std::vector<std::string> &argv, const ArgSpec &spec, Args &out, std::string *error)
{
    const auto alias = [](const std::string &a) -> std::string {
        if (a == "-h")
            return "--help";
        if (a == "-q")
            return "--quiet";
        return a;
    };
    bool optionsEnded = false;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        std::string a = alias(argv[i]);
        if (optionsEnded || a.size() < 2 || a[0] != '-' || a == "-") {
            out.positional.push_back(a);
            continue;
        }
        if (a == "--") {
            optionsEnded = true;
            continue;
        }
        if (a.compare(0, 2, "--") != 0) {
            if (error)
                *error = "unknown option " + a;
            return false;
        }
        std::string name = a.substr(2);
        std::string value;
        bool hasValue = false;
        const std::size_t eq = name.find('=');
        if (eq != std::string::npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            hasValue = true;
        }
        if (spec.flags.count(name)) {
            if (hasValue) {
                if (error)
                    *error = "--" + name + " does not take a value";
                return false;
            }
            out.flags.insert(name);
        } else if (spec.valued.count(name)) {
            if (!hasValue) {
                if (i + 1 >= argv.size()) {
                    if (error)
                        *error = "--" + name + " needs a value";
                    return false;
                }
                value = argv[++i];
            }
            out.values[name].push_back(value);
        } else {
            if (error)
                *error = "unknown option --" + name;
            return false;
        }
    }
    return true;
}

}  // namespace cli
