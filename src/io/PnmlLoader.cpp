// GLM-5.3-Flash: PNML import, doku section 12 item 6. See PnmlLoader.h for
// the conventions. The XML reader below is deliberately small: well-formed
// PNML in, issues out; no entity catalogs, no DTD validation, no schemas.

#include "io/PnmlLoader.h"

#include "model/NetBuilder.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pn {

namespace {

// ---------------------------------------------------------------------------
// A tiny XML tree
// ---------------------------------------------------------------------------

struct XmlAttr {
    std::string name;    // namespace prefixes stripped
    std::string value;   // entities decoded
};

struct XmlNode {
    std::string name;    // namespace prefixes stripped
    std::vector<XmlAttr> attrs;
    std::string text;    // concatenated character data (entities decoded)
    std::vector<XmlNode> children;

    const std::string *attr(const std::string &name) const
    {
        for (const XmlAttr &a : attrs)
            if (a.name == name)
                return &a.value;
        return nullptr;
    }

    const XmlNode *child(const std::string &name) const
    {
        for (const XmlNode &c : children)
            if (c.name == name)
                return &c;
        return nullptr;
    }
};

// Strips a namespace prefix ("p1:place" -> "place").
std::string localName(const std::string &raw)
{
    const std::size_t colon = raw.rfind(':');
    return colon == std::string::npos ? raw : raw.substr(colon + 1);
}

// Decodes the five predefined entities and numeric character references. A
// stray ampersand or an unknown entity stays as literal text.
std::string decodeEntities(const std::string &raw)
{
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '&') {
            out += raw[i++];
            continue;
        }
        const std::size_t semi = raw.find(';', i);
        if (semi != std::string::npos && semi <= i + 10) {
            const std::string ent = raw.substr(i + 1, semi - i - 1);
            if (ent == "lt") { out += '<'; i = semi + 1; continue; }
            if (ent == "gt") { out += '>'; i = semi + 1; continue; }
            if (ent == "amp") { out += '&'; i = semi + 1; continue; }
            if (ent == "quot") { out += '"'; i = semi + 1; continue; }
            if (ent == "apos") { out += '\''; i = semi + 1; continue; }
            if (!ent.empty() && ent[0] == '#') {
                long code = 0;
                bool ok = true;
                if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X'))
                    code = std::strtol(ent.c_str() + 2, nullptr, 16);
                else
                    code = std::strtol(ent.c_str() + 1, nullptr, 10);
                if (code <= 0 || code > 0x10FFFF)
                    ok = false;
                if (ok) {
                    // UTF-8 encode the code point (ASCII comes out unchanged).
                    const unsigned long c = static_cast<unsigned long>(code);
                    if (c < 0x80)
                        out += static_cast<char>(c);
                    else if (c < 0x800) {
                        out += static_cast<char>(0xC0 | (c >> 6));
                        out += static_cast<char>(0x80 | (c & 0x3F));
                    } else if (c < 0x10000) {
                        out += static_cast<char>(0xE0 | (c >> 12));
                        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (c & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (c >> 18));
                        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (c & 0x3F));
                    }
                    i = semi + 1;
                    continue;
                }
            }
        }
        out += raw[i++];  // a stray '&' or an unsupported entity: keep the text
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(const std::string &text) : s_(text) {}

    bool parse(XmlNode &root, std::string &error)
    {
        skipPrologue();
        if (!parseNode(root, error))
            return false;
        skipSpace();
        if (pos_ < s_.size()) {
            error = "unexpected content after the root element at byte " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    void skipSpace()
    {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
            ++pos_;
    }

    // <?xml ...?>, <!-- --> and <!DOCTYPE ... [ ... ]> before the root.
    void skipPrologue()
    {
        for (;;) {
            skipSpace();
            if (s_.compare(pos_, 2, "<?") == 0) {
                const std::size_t end = s_.find("?>", pos_);
                if (end == std::string::npos) {
                    pos_ = s_.size();
                    return;
                }
                pos_ = end + 2;
            } else if (s_.compare(pos_, 4, "<!--") == 0) {
                const std::size_t end = s_.find("-->", pos_);
                if (end == std::string::npos) {
                    pos_ = s_.size();
                    return;
                }
                pos_ = end + 3;
            } else if (s_.compare(pos_, 2, "<!") == 0) {
                // DOCTYPE and friends: skip everything up to the '>' that
                // balances the markup, bracketed internal subset included.
                // (A quoted '<' or '>' inside the DOCTYPE would confuse this;
                // PNML files do not carry any.)
                long depth = 0;
                while (pos_ < s_.size()) {
                    const char c = s_[pos_++];
                    if (c == '<') ++depth;
                    else if (c == '>' && --depth == 0)
                        break;
                }
            } else {
                return;
            }
        }
    }

    bool parseNode(XmlNode &out, std::string &error)
    {
        if (pos_ >= s_.size() || s_[pos_] != '<') {
            error = "expected an element at byte " + std::to_string(pos_);
            return false;
        }
        ++pos_;
        std::size_t i = pos_;
        while (i < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i]))
               && s_[i] != '>' && s_[i] != '/' && s_[i] != '<')
            ++i;
        if (i >= s_.size()) {
            error = "unterminated start tag";
            return false;
        }
        out.name = localName(s_.substr(pos_, i - pos_));
        if (out.name.empty()) {
            error = "empty element name at byte " + std::to_string(pos_);
            return false;
        }
        pos_ = i;

        // Attributes, up to '>' (or the self-closing '/>').
        for (;;) {
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == '/') {
                if (pos_ + 1 >= s_.size() || s_[pos_ + 1] != '>') {
                    error = "malformed self-closing tag at byte " + std::to_string(pos_);
                    return false;
                }
                pos_ += 2;
                return true;
            }
            if (pos_ < s_.size() && s_[pos_] == '>') {
                ++pos_;
                break;
            }
            std::size_t j = pos_;
            while (j < s_.size() && !std::isspace(static_cast<unsigned char>(s_[j]))
                   && s_[j] != '=' && s_[j] != '>' && s_[j] != '/')
                ++j;
            if (j == pos_ || j >= s_.size()) {
                error = "malformed attribute in <" + out.name + "> at byte " + std::to_string(pos_);
                return false;
            }
            XmlAttr a;
            a.name = localName(s_.substr(pos_, j - pos_));
            pos_ = j;
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '=') {
                error = "attribute \"" + a.name + "\" of <" + out.name + "> has no value";
                return false;
            }
            ++pos_;
            skipSpace();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) {
                error = "attribute \"" + a.name + "\" of <" + out.name + "> is not quoted";
                return false;
            }
            const char quote = s_[pos_++];
            const std::size_t end = s_.find(quote, pos_);
            if (end == std::string::npos) {
                error = "unterminated value of attribute \"" + a.name + "\"";
                return false;
            }
            a.value = decodeEntities(s_.substr(pos_, end - pos_));
            pos_ = end + 1;
            out.attrs.push_back(std::move(a));
        }

        // Content, until the matching end tag.
        for (;;) {
            const std::size_t lt = s_.find('<', pos_);
            if (lt == std::string::npos) {
                error = "missing end tag for <" + out.name + ">";
                return false;
            }
            out.text += decodeEntities(s_.substr(pos_, lt - pos_));
            pos_ = lt;
            if (s_.compare(pos_, 9, "<![CDATA[") == 0) {
                const std::size_t end = s_.find("]]>", pos_);
                if (end == std::string::npos) {
                    error = "unterminated CDATA section";
                    return false;
                }
                out.text += s_.substr(pos_ + 9, end - (pos_ + 9));
                pos_ = end + 3;
                continue;
            }
            if (s_.compare(pos_, 4, "<!--") == 0) {
                const std::size_t end = s_.find("-->", pos_);
                if (end == std::string::npos) {
                    error = "unterminated comment";
                    return false;
                }
                pos_ = end + 3;
                continue;
            }
            if (s_.compare(pos_, 2, "<?") == 0) {
                const std::size_t end = s_.find("?>", pos_);
                if (end == std::string::npos) {
                    error = "unterminated processing instruction";
                    return false;
                }
                pos_ = end + 2;
                continue;
            }
            if (s_.compare(pos_, 2, "</") == 0) {
                const std::size_t end = s_.find('>', pos_);
                if (end == std::string::npos) {
                    error = "unterminated end tag";
                    return false;
                }
                const std::string closing = localName(s_.substr(pos_ + 2, end - (pos_ + 2)));
                if (closing != out.name) {
                    error = "end tag </" + closing + "> does not close <" + out.name + ">";
                    return false;
                }
                pos_ = end + 1;
                return true;
            }
            XmlNode kid;
            if (!parseNode(kid, error))
                return false;
            out.children.push_back(std::move(kid));
        }
    }

    const std::string &s_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// The loader proper
// ---------------------------------------------------------------------------

struct Diag {
    std::vector<LoadIssue> issues;
    void error(const std::string &path, const std::string &message)
    {
        issues.push_back({LoadIssue::Severity::Error, path, message});
    }
    void warning(const std::string &path, const std::string &message)
    {
        issues.push_back({LoadIssue::Severity::Warning, path, message});
    }
};

// The trimmed text of a <name><text>...</text></name> (or a direct text node).
std::string nameText(const XmlNode &el)
{
    const XmlNode *name = el.child("name");
    if (!name)
        return {};
    std::string text = name->text;
    if (const XmlNode *t = name->child("text"))
        text = t->text;
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Collects places, transitions and arcs from the <net> element, whatever they
// are nested in (<page> or deeper).
void collect(const XmlNode &el, std::vector<const XmlNode *> &places,
             std::vector<const XmlNode *> &transitions, std::vector<const XmlNode *> &arcs)
{
    for (const XmlNode &c : el.children) {
        if (c.name == "place") places.push_back(&c);
        else if (c.name == "transition") transitions.push_back(&c);
        else if (c.name == "arc") arcs.push_back(&c);
        else collect(c, places, transitions, arcs);
    }
}

// A non-negative 32-bit integer after dropping whitespace.
bool parseCount(const std::string &raw, std::uint32_t &out)
{
    std::string text;
    for (const char c : raw)
        if (!std::isspace(static_cast<unsigned char>(c)))
            text += c;
    if (text.empty())
        return false;
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(text, &used);
    } catch (const std::exception &) {
        return false;
    }
    if (used != text.size() || value > 0xFFFFFFFFul)
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

std::string baseName(const std::string &path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    return name;
}

// DeepSeek V4.1 Flash: the helper that picked one <toolspecific> is gone -- every
// reader loops over el.children itself, the kind/label and milestone tags
// compose in any order, and each accepts both the "eggembt" tool attribute and
// the legacy "mbt" one.

LoadResult importNet(const std::string &xml, const std::string &sourceName)
{
    LoadResult result;
    Diag d;

    XmlNode root;
    std::string xmlError;
    if (!XmlParser(xml).parse(root, xmlError)) {
        d.error("$", xmlError);
        result.issues = std::move(d.issues);
        return result;
    }

    // The first <net> element, wherever it sits.
    const XmlNode *netEl = nullptr;
    std::vector<const XmlNode *> stack{&root};
    while (!stack.empty() && !netEl) {
        const XmlNode *cur = stack.back();
        stack.pop_back();
        for (const XmlNode &c : cur->children) {
            if (c.name == "net") {
                netEl = &c;
                break;
            }
            stack.push_back(&c);
        }
    }
    if (!netEl) {
        d.error("$", "no <net> element: this is not a PNML file");
        result.issues = std::move(d.issues);
        return result;
    }

    std::string netName = nameText(*netEl);
    if (netName.empty())
        netName = baseName(sourceName.empty() ? std::string("model") : sourceName);

    NetBuilder builder(netName);

    std::vector<const XmlNode *> places, transitions, arcs;
    collect(*netEl, places, transitions, arcs);

    // --- places ---------------------------------------------------------------
    // GLM-5.3-Flash: initial tokens per place, so per-place initial markings
    // can be unioned into ONE initial marking of the model at the end.
    std::unordered_map<std::string, std::uint32_t> initialTokens;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const std::string p = "place " + std::to_string(i);
        const XmlNode &el = *places[i];
        const std::string *id = el.attr("id");
        if (!id || id->empty()) {
            d.error(p, "a place needs an \"id\" attribute");
            continue;
        }
        if (builder.net().placeIndex.count(*id) || builder.net().transitionIndex.count(*id)) {
            d.error(p, "duplicate id \"" + *id + "\" (ids are shared by places and transitions)");
            continue;
        }
        builder.place(*id);

        // Initial marking: the standard <initialMarking><text>N</text></initialMarking>,
        // or the shorthand attribute form some tools emit.
        std::uint32_t tokens = 0;
        if (const XmlNode *im = el.child("initialMarking")) {
            std::string raw = im->text;
            if (const XmlNode *t = im->child("text"))
                raw = t->text;
            if (!parseCount(raw, tokens))
                d.error(p + " initialMarking", "must be a token count (a non-negative integer)");
        } else if (const std::string *shorthand = el.attr("initialMarking")) {
            if (!parseCount(*shorthand, tokens))
                d.error(p + " initialMarking", "must be a token count (a non-negative integer)");
        }
        if (tokens > 0)
            initialTokens.emplace(*id, tokens);

        // GLM-5.3-Flash: no bounds and no target/error markings come out of
        // PNML. Whatever a place's toolspecific asks for is ignored, loudly.
        if (el.child("toolspecific"))
            d.warning(p, "has a toolspecific element: it is ignored (bounds, target and error markings "
                         "are not imported from PNML)");
        if (el.child("capacity"))
            d.warning(p + " capacity", "place bounds are not imported from PNML");
    }

    // --- transitions ------------------------------------------------------------
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const std::string p = "transition " + std::to_string(i);
        const XmlNode &el = *transitions[i];
        const std::string *id = el.attr("id");
        if (!id || id->empty()) {
            d.error(p, "a transition needs an \"id\" attribute");
            continue;
        }
        if (builder.net().placeIndex.count(*id) || builder.net().transitionIndex.count(*id)) {
            d.error(p, "duplicate id \"" + *id + "\" (ids are shared by places and transitions)");
            continue;
        }

        const XmlNode *specific = nullptr;
        // DeepSeek V4.1 Flash: the kind/label toolspecific is the first eggembt one that
        // is not the milestone tag; the milestone tag is read separately
        // below, so the two compose in any order.
        for (const XmlNode &c : el.children) {
            if (c.name != "toolspecific")
                continue;
            const std::string *tool = c.attr("tool");
            if (tool && *tool != "eggembt" && *tool != "mbt")  // DeepSeek V4.1 Flash: legacy tag
                continue;
            const std::string *k = c.attr("kind");
            if (k && *k == "milestone")
                continue;
            if (!specific)
                specific = &c;
        }
        std::string kindText, labelText;
        if (specific) {
            if (const std::string *k = specific->attr("kind"))
                kindText = *k;
            if (const std::string *l = specific->attr("label"))
                labelText = *l;
        }

        // GLM-5.3-Flash: the kind. An explicit toolspecific kind wins; without
        // one, the id's first byte decides as a fallback ('?' controllable,
        // '!' observable, otherwise internal). An internal transition without
        // an explicit label stays SILENT: its id is never used as a label, so
        // a named internal step does not become a traced one by accident.
        TransitionKind kind = TransitionKind::Internal;
        bool haveKind = false;
        if (!kindText.empty()) {
            if (kindText == "controllable") { kind = TransitionKind::Controllable; haveKind = true; }
            else if (kindText == "observable") { kind = TransitionKind::Observable; haveKind = true; }
            else if (kindText == "internal") { kind = TransitionKind::Internal; haveKind = true; }
            else
                d.error(p + " toolspecific", "kind must be \"controllable\", \"observable\" or \"internal\"");
        }
        std::string label = labelText;
        if (!haveKind && labelText.empty() && !id->empty() && ((*id)[0] == '?' || (*id)[0] == '!')) {
            // The prefix fallback; the prefix is stripped from the label.
            if ((*id)[0] == '?')
                kind = TransitionKind::Controllable;
            else
                kind = TransitionKind::Observable;
            label = id->substr(1);
        }
        if ((kind == TransitionKind::Controllable || kind == TransitionKind::Observable) && label.empty()) {
            // A controllable or observable step needs a label to map to the
            // SUT: fall back to the id (stripped of a prefix).
            label = (!id->empty() && ((*id)[0] == '?' || (*id)[0] == '!')) ? id->substr(1) : *id;
        }
        const TransitionIndex t = builder.transition(*id, kind, label);

        // GLM-5.3-Flash: the milestone tag, as its own toolspecific so it
        // composes with the kind one (the kind tag decides the visibility).
        for (const XmlNode &c : el.children) {
            if (c.name != "toolspecific")
                continue;
            const std::string *tool = c.attr("tool");
            if (tool && *tool != "eggembt" && *tool != "mbt")  // DeepSeek V4.1 Flash: legacy tag
                continue;
            const std::string *k = c.attr("kind");
            if (!k || *k != "milestone")
                continue;
            std::uint32_t rank = 1;
            if (const std::string *r = c.attr("rank")) {
                if (!parseCount(*r, rank) || rank < 1) {
                    d.error(p + " toolspecific", "rank must be an integer >= 1");
                    rank = 1;
                }
            }
            builder.milestone(t, rank);
            if (kind == TransitionKind::Internal && label.empty())
                d.error(p + " toolspecific",
                        "a milestone transition must be visible to the tester: give the kind toolspecific "
                        "a \"kind\" of controllable or observable, or an explicit \"label\"");
        }
    }

    // --- arcs -------------------------------------------------------------------
    for (std::size_t i = 0; i < arcs.size(); ++i) {
        const std::string p = "arc " + std::to_string(i);
        const XmlNode &el = *arcs[i];
        const std::string *source = el.attr("source");
        const std::string *target = el.attr("target");
        if (!source || source->empty() || !target || target->empty()) {
            d.error(p, "an arc needs \"source\" and \"target\" attributes");
            continue;
        }
        const auto fromPlace = builder.net().findPlace(*source);
        const auto fromTransition = builder.net().findTransition(*source);
        const auto toPlace = builder.net().findPlace(*target);
        const auto toTransition = builder.net().findTransition(*target);
        if ((!fromPlace && !fromTransition) || (!toPlace && !toTransition)) {
            const std::string &missing = (fromPlace || fromTransition) ? *target : *source;
            d.error(p, "unknown arc endpoint \"" + missing + "\"");
            continue;
        }
        if ((fromPlace && toPlace) || (fromTransition && toTransition)) {
            d.error(p, "an arc must connect a place and a transition");
            continue;
        }

        // Weight: the attribute shorthand or <inscription><text>N</text></inscription>.
        std::uint32_t weight = 1;
        if (const XmlNode *ins = el.child("inscription")) {
            std::string raw = ins->text;
            if (const XmlNode *t = ins->child("text"))
                raw = t->text;
            if (!parseCount(raw, weight) || weight < 1) {
                d.error(p + " inscription", "must be an arc weight (an integer >= 1)");
                weight = 1;
            }
        } else if (const std::string *w = el.attr("weight")) {
            if (!parseCount(*w, weight) || weight < 1) {
                d.error(p, "the arc weight must be an integer >= 1");
                weight = 1;
            }
        }

        // GLM-5.3-Flash: inhibitor arcs, the standard <type value="inhibitor">
        // child or the shorthand type attribute.
        std::string typeText;
        if (const XmlNode *type = el.child("type")) {
            if (const std::string *v = type->attr("value"))
                typeText = *v;
            else
                typeText = type->text;
        } else if (const std::string *t = el.attr("type")) {
            typeText = *t;
        }
        bool inhibitor = false;
        if (!typeText.empty() && typeText != "normal") {
            if (typeText == "inhibitor") {
                inhibitor = true;
            } else {
                d.error(p, "unknown arc type \"" + typeText + "\" (normal or inhibitor)");
                continue;
            }
        }

        if (fromPlace && toTransition) {
            if (inhibitor)
                builder.inhibitor(*fromPlace, *toTransition, weight);
            else
                builder.input(*fromPlace, *toTransition, weight);
        } else if (inhibitor) {
            d.error(p, "an inhibitor arc always points from a place to a transition");
        } else {
            builder.output(*fromTransition, *toPlace, weight);
        }
    }

    // --- initial markings: the union of every place's tokens ---------------------
    SparseMarking initial;
    const Net &built = builder.net();
    for (std::size_t i = 0; i < built.places.size(); ++i) {
        const auto it = initialTokens.find(built.places[i].id);
        if (it != initialTokens.end())
            initial.push_back({static_cast<PlaceIndex>(i), it->second});
    }
    if (initial.empty()) {
        d.error("$", "no place carries tokens: the net has no initial marking");
        result.issues = std::move(d.issues);
        return result;
    }
    // GLM-5.3-Flash: like the JSON loader, a net is only produced when there
    // are no errors.
    for (const LoadIssue &i : d.issues)
        if (i.severity == LoadIssue::Severity::Error) {
            result.issues = std::move(d.issues);
            return result;
        }
    std::sort(initial.begin(), initial.end(),
              [](const TokenEntry &a, const TokenEntry &b) { return a.place < b.place; });

    Net net = builder.build();
    NamedMarking m;
    m.name = netName.empty() ? "initial" : netName;  // one marking, named after the net
    m.match = MatchMode::Exact;
    m.tokens = std::move(initial);
    net.initialMarkings.push_back(std::move(m));
    result.net = std::move(net);
    result.issues = std::move(d.issues);
    return result;
}

}  // namespace

LoadResult PnmlLoader::loadFile(const std::string &fileName)
{
    std::ifstream in(fileName, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.issues.push_back({LoadIssue::Severity::Error, fileName, "cannot open " + fileName});
        return r;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return importNet(buffer.str(), fileName);
}

LoadResult PnmlLoader::loadText(const std::string &xml, const std::string &sourceName)
{
    return importNet(xml, sourceName);
}

}  // namespace pn
