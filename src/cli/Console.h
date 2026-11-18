// Terminal output helpers for the command-line tool: optional colour and plain
// text tables. Standard C++ only (colour is decided by the caller).
#pragma once

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

namespace cli {

struct Console {
    std::ostream &out;
    std::ostream &err;
    bool color = false;
    bool quiet = false;

    std::string paint(const char *code, const std::string &s) const
    {
        return color ? std::string("\x1b[") + code + "m" + s + "\x1b[0m" : s;
    }
    std::string bold(const std::string &s) const { return paint("1", s); }
    std::string dim(const std::string &s) const { return paint("2", s); }
    std::string red(const std::string &s) const { return paint("31", s); }
    std::string green(const std::string &s) const { return paint("32", s); }
    std::string yellow(const std::string &s) const { return paint("33", s); }
    std::string cyan(const std::string &s) const { return paint("36", s); }
};

// Number of characters (UTF-8 code points), which is what a terminal column shows for plain text.
inline std::size_t displayWidth(const std::string &s)
{
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

inline std::string padRight(const std::string &s, std::size_t width)
{
    const std::size_t w = displayWidth(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

inline std::string truncate(const std::string &s, std::size_t width)
{
    if (displayWidth(s) <= width)
        return s;
    std::string out;
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            if (n + 1 >= width)
                break;
            ++n;
        }
        out += static_cast<char>(c);
    }
    return out + "...";
}

// rows[0] is the header. Cells are plain text; the header is shown in bold.
inline void printTable(const Console &con, const std::vector<std::vector<std::string>> &rows,
                       const std::string &indent = "  ")
{
    if (rows.empty())
        return;
    std::vector<std::size_t> widths(rows[0].size(), 0);
    for (const auto &row : rows)
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], displayWidth(row[i]));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        std::string line = indent;
        for (std::size_t i = 0; i < rows[r].size() && i < widths.size(); ++i) {
            const bool last = i + 1 == rows[r].size();
            const std::string cell = last ? rows[r][i] : padRight(rows[r][i], widths[i] + 2);
            line += r == 0 ? con.bold(cell) : cell;
        }
        con.out << line << "\n";
    }
}

inline std::string percent(std::size_t part, std::size_t whole)
{
    if (whole == 0)
        return "n/a";
    return std::to_string(static_cast<int>(100.0 * static_cast<double>(part) / static_cast<double>(whole) + 0.5)) + "%";
}

}  // namespace cli
