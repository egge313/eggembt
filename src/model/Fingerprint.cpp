#include "model/Fingerprint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace pn {

namespace {

std::string join(const std::vector<std::string> &parts, char sep)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out += sep;
        out += parts[i];
    }
    return out;
}

}  // namespace

std::string graphFingerprint(const Net &net)
{
    std::vector<std::string> lines;

    for (const Place &p : net.places)
        lines.push_back("P " + p.id + " " + (p.bound ? std::to_string(*p.bound) : std::string("-")));

    const auto arcList = [&net](const char *tag, const std::vector<WeightedArc> &arcs) {
        std::vector<std::string> parts;
        for (const WeightedArc &a : arcs)
            parts.push_back(std::string(tag) + net.places[a.place].id + "*" + std::to_string(a.weight));
        std::sort(parts.begin(), parts.end());
        return join(parts, ',');
    };
    for (const Transition &t : net.transitions) {
        lines.push_back("T " + t.id + " " + arcList("i:", t.pre) + " " + arcList("o:", t.post) + " "
                        + arcList("h:", t.inhibit));
    }

    for (const NamedMarking &m : net.initialMarkings) {
        std::vector<std::string> parts;
        for (const TokenEntry &e : m.tokens)
            parts.push_back(net.places[e.place].id + "=" + std::to_string(e.tokens));
        std::sort(parts.begin(), parts.end());
        lines.push_back("M " + m.name + " " + join(parts, ','));
    }

    std::sort(lines.begin(), lines.end());

    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const std::string &line : lines) {
        for (unsigned char c : line) {
            h ^= c;
            h *= kPrime;
        }
        h ^= '\n';
        h *= kPrime;
    }

    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

}  // namespace pn
