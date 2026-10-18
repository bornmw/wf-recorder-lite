#pragma once

#include <string>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>

#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>

// Pure path/format helpers, factored out of MainWindow so they are
// unit-testable (see tests/tst_pathtools.cpp).

// Home directory: $HOME, falling back to the passwd entry for the current
// user (mirrors the old QDir::homePath() behaviour).
inline std::string homePath()
{
    if (const char *h = std::getenv("HOME"))
        return h;
    if (const struct passwd *pw = getpwuid(geteuid()))
        if (pw->pw_dir)
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

// Non-throwing numeric parses for wlr-randr fields. std::stoi/stod raise
// std::invalid_argument/std::out_of_range on unexpected output, which would
// escape into the GUI thread; these return false and leave `out` untouched so
// callers keep their documented defaults instead of crashing.
inline bool parseInt(const std::string &s, int &out)
{
    try
    {
        size_t consumed = 0;
        out = std::stoi(s, &consumed);
        return consumed == s.size(); // reject trailing garbage
    }
    catch (const std::exception &)
    {
        return false;
    }
}

inline bool parseDouble(const std::string &s, double &out)
{
    try
    {
        size_t consumed = 0;
        out = std::stod(s, &consumed);
        return consumed == s.size();
    }
    catch (const std::exception &)
    {
        return false;
    }
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
                    int x2 = 0, y2 = 0;
                    if (parseInt(xs, x2) && parseInt(ys, y2))
                    {
                        x = x2;
                        y = y2;
                    }
                }
            }
        }
        else if (line.rfind("Scale:", 0) == 0)
        {
            const std::string rest = trimSpaces(line.substr(6));
            if (!rest.empty())
            {
                double sc = 0.0;
                if (parseDouble(rest, sc))
                    scale = sc;
            }
        }
    }
    return found;
}

// Run `wlr-randr` and return its stdout. Capped at ~2 s so a hung query can't
// freeze the UI (mirrors the old QProcess::waitForFinished(2000)). Returns
// false if the binary is missing/failed or the cap was hit.
inline bool runWlrRandr(std::string &out)
{
    out.clear();
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0)
        return false;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    const pid_t pid = fork();
    if (pid < 0)
    {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    if (pid == 0)
    {
        // Child: stdout -> pipe, everything else away.
        dup2(pipefd[1], STDOUT_FILENO);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                ::close(devnull);
        }
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // execvp: the bare name must be resolved through $PATH (execl would
        // look for ./wlr-randr relative to the app's cwd and fail).
        const char *argv[] = {"wlr-randr", (char *)nullptr};
        execvp("wlr-randr", const_cast<char *const *>(argv));
        _exit(127);
    }

    char buf[4096];
    int waited = 0;
    int status = 0;
    bool reaped = false;
    for (;;)
    {
        const ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n > 0)
        {
            out.append(buf, static_cast<size_t>(n));
        }
        else if (n == 0)
        {
            break;
        }
        else if (errno == EAGAIN || errno == EINTR)
        {
            if (waitpid(pid, &status, WNOHANG) == pid)
            {
                reaped = true;
                break; // child exited (EOF will follow)
            }
            if (waited >= 2000)
            {
                ::kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    continue;
                ::close(pipefd[0]);
                return false;
            }
            usleep(10 * 1000);
            waited += 10;
        }
        else
        {
            break;
        }
    }
    ::close(pipefd[0]);

    // Reap only if the EAGAIN branch didn't already: a second waitpid hits
    // ECHILD and the real exit status is lost — a non-zero exit would have
    // been misreported as success.
    if (!reaped)
    {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            continue;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The name of the first output listed (the first non-indented line's first
// token, e.g. "eDP-1"). Empty string if there is none.
inline std::string parseWlrRandrFirstOutput(const std::string &out)
{
    size_t start = 0;
    while (start < out.size())
    {
        const size_t nl = out.find('\n', start);
        const std::string raw =
            (nl == std::string::npos) ? out.substr(start)
                                      : out.substr(start, nl - start);
        start = (nl == std::string::npos) ? out.size() : nl + 1;
        if (raw.empty() || raw[0] == ' ')
            continue;
        return raw.substr(0, raw.find(' '));
    }
    return "";
}

// The PHYSICAL size of the current mode for `name` (a mode line shaped like
// "2880x1800 px, 120.000000 Hz (preferred, current)"). A line marked
// "current" wins; otherwise the first listed mode. Returns false if the
// output has no parseable mode line.
inline bool parseWlrRandrMode(const std::string &out, const std::string &name,
                              int &w, int &h)
{
    w = 0;
    h = 0;
    bool got = false;
    bool current = false;
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
            got = false;
            current = false;
            continue;
        }
        if (raw.empty() || cur != name)
            continue;

        const std::string line = trimSpaces(raw);
        // Mode lines: "2880x1800 px, 120.000000 Hz (preferred, current)"
        if (line.find(" px") == std::string::npos)
            continue;
        const size_t x = line.find('x');
        if (x == 0 || x == std::string::npos)
            continue;
        bool digits = true;
        for (size_t i = 0; i < x; ++i)
            digits = digits &&
                     std::isdigit(static_cast<unsigned char>(line[i]));
        if (!digits)
            continue;
        size_t end = line.find(' ', x + 1);
        if (end == std::string::npos)
            end = line.size();
        const std::string hs = line.substr(x + 1, end - x - 1);
        if (hs.empty())
            continue;
        for (const char c : hs)
            digits = digits && std::isdigit(static_cast<unsigned char>(c));
        if (!digits)
            continue;

        const bool isCurrent =
            (line.find("current") != std::string::npos);
        if (!got || (isCurrent && !current))
        {
            int w2 = 0, h2 = 0;
            if (parseInt(line.substr(0, x), w2) && parseInt(hs, h2))
            {
                w = w2;
                h = h2;
                got = true;
                current = isCurrent;
            }
        }
    }
    return got;
}
