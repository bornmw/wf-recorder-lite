#pragma once

#include <string>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <pwd.h>
#include <unistd.h>

// Pure path/format helpers, factored out of MainWindow so they are
// unit-testable (see tests/tst_pathtools.cpp).

// Home directory: $HOME, falling back to the passwd entry for the current
// user (mirrors the old QDir::homePath() behaviour).
inline std::string homePath()
{
    if (const char *h = std::getenv("HOME"))
        return h;
    if (const struct passwd *pw = getpwuid(geteuid()))
        if (pw && pw->pw_dir)
            return pw->pw_dir;
    return "/";
}

// Drop a known container extension (case-insensitive) so callers can force
// the one matching the selected format.
inline std::string stripKnownExt(const std::string &p)
{
    static const char *known[] = {".mp4", ".gif", ".mkv", ".webm", ".mov"};
    for (const char *ext : known)
    {
        const size_t n = std::strlen(ext);
        if (p.size() < n)
            continue;
        bool match = true;
        for (size_t i = 0; i < n; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(p[p.size() - n + i])) !=
                std::tolower(static_cast<unsigned char>(ext[i])))
            {
                match = false;
                break;
            }
        }
        if (match)
            return p.substr(0, p.size() - n);
    }
    return p;
}

// Field text -> canonical path: empty falls back to "recording", a bare
// file name is placed in the home directory.
inline std::string normalizeRecordPath(const std::string &fieldText)
{
    std::string p = fieldText;
    // trim
    const auto b = p.find_first_not_of(" \t");
    if (b == std::string::npos)
        p.clear();
    else
    {
        const auto e = p.find_last_not_of(" \t");
        p = p.substr(b, e - b + 1);
    }
    if (p.empty())
        p = "recording";
    if (p.find('/') == std::string::npos)
        p = homePath() + "/" + p;
    return p;
}

// Human-readable byte size: "500 B" / "2 KB" / "45.2 MB".
inline std::string formatSize(long long bytes)
{
    char buf[32];
    if (bytes >= 1024LL * 1024LL)
    {
        std::snprintf(buf, sizeof buf, "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    }
    if (bytes >= 1024)
    {
        // round to nearest KB, as before
        const long long kb = (bytes + 512) / 1024;
        std::snprintf(buf, sizeof buf, "%lld KB", kb);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%lld B", bytes);
    return buf;
}

// Trim leading/trailing spaces.
inline std::string trimSpaces(const std::string &s)
{
    const auto b = s.find_first_not_of(' ');
    if (b == std::string::npos)
        return "";
    const auto e = s.find_last_not_of(' ');
    return s.substr(b, e - b + 1);
}

// Parse wlr-randr output for one output's PHYSICAL position and scale.
// Returns true if the output is listed; fields that are missing keep the
// defaults (x=0, y=0, scale=1.0) set on entry.
inline bool parseWlrRandr(const std::string &out, const std::string &name,
                          int &x, int &y, double &scale)
{
    x = 0;
    y = 0;
    scale = 1.0;
    bool found = false;
    std::string cur;
    size_t start = 0;
    while (start <= out.size())
    {
        const size_t nl = out.find('\n', start);
        const std::string raw =
            (nl == std::string::npos) ? out.substr(start)
                                      : out.substr(start, nl - start);
        start = (nl == std::string::npos) ? out.size() + 1 : nl + 1;

        if (!raw.empty() && raw[0] != ' ')
        {
            cur = raw.substr(0, raw.find(' '));
            continue;
        }
        if (raw.empty() || cur != name)
            continue;

        found = true; // output is listed; missing fields keep the defaults
        const std::string line = trimSpaces(raw);
        if (line.rfind("Position:", 0) == 0)
        {
            const std::string rest = trimSpaces(line.substr(9));
            const size_t c = rest.find(',');
            if (c != std::string::npos)
            {
                const std::string xs = trimSpaces(rest.substr(0, c));
                const std::string ys = trimSpaces(rest.substr(c + 1));
                if (!xs.empty() && !ys.empty())
                {
                    x = std::stoi(xs);
                    y = std::stoi(ys);
                }
            }
        }
        else if (line.rfind("Scale:", 0) == 0)
        {
            const std::string rest = trimSpaces(line.substr(6));
            if (!rest.empty())
                scale = std::stod(rest);
        }
    }
    return found;
}
