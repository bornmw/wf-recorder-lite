#pragma once

// Minimal async child-process manager for the gtkmm port (replaces QProcess).
// Provides exactly the QProcess semantics the UI relied on:
//
//   onStarted              exec succeeded — the process is running
//                          (QProcess::started)
//   onFailStart            exec failed (QProcess::FailedToStart)
//   onStderr(chunk)        stderr data arrived (readyReadStandardError)
//   onFinished(code, ok)   the process exited; ok == false when it died by
//                          signal (QProcess::NormalExit vs. Crashed)
//
// stdout is redirected to /dev/null (nothing in this app reads it). stderr
// goes through a pipe to the GLib main loop. The child reports exec success
// by virtue of its CLOEXEC exec-report pipe closing (EOF == exec ok); an
// explicit '0' byte means exec failed.

#include <glib.h>
#include <glib-unix.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

class Child
{
public:
    ~Child();

    // fork + exec. Returns false if fork itself failed (out of memory);
    // exec failures are delivered asynchronously via onFailStart.
    bool start(const std::string &program,
               const std::vector<std::string> &args);

    // Remove the GLib fd sources without reaping. Used before a synchronous
    // teardown on the close path, where stopSync()/waitSync() do the reaping
    // and the async callbacks must not fire afterwards.
    void detach();

    // Signal the child (no-op once it has exited).
    void signal(int sig);

    // Reap the child, waiting up to `ms` milliseconds. Returns true if the
    // child has exited within the budget.
    bool waitSync(int ms);

    bool running() const
    {
        return m_pid > 0 && !m_exited;
    }
    pid_t pid() const
    {
        return m_pid;
    }
    // Valid only after the child has exited (async EOF path or waitSync).
    int exitCode() const
    {
        return m_exitCode;
    }
    bool normalExit() const
    {
        return m_normalExit;
    }

    std::function<void()> onStarted;          // exec succeeded (process running)
    std::function<void()> onFailStart;        // exec failed (FailedToStart)
    std::function<void(const std::string &)> onStderr; // stderr chunk arrived
    std::function<void(int code, bool normalExit)> onFinished; // process exited

private:
    void onChildExited();

    pid_t m_pid = -1;
    int m_execfd = -1; // child->parent: '0' byte if exec failed, EOF if ok
    int m_errfd = -1;  // child stderr -> parent
    guint m_execSrc = 0;
    guint m_errSrc = 0;
    bool m_exited = false;
    bool m_execFailed = false;
    int m_exitCode = -1;
    bool m_normalExit = false;

    static gboolean execReady(gint fd, GIOCondition cond, void *data);
    static gboolean errReady(gint fd, GIOCondition cond, void *data);
};

inline Child::~Child()
{
    detach();
    // Deliberately does NOT kill a running child: the owner (MainWindow)
    // manages teardown explicitly, mirroring the QProcess design where the
    // escalation timers and the close handler own the process lifecycle.
}

inline bool
Child::start(const std::string &program, const std::vector<std::string> &args)
{
    int execPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(execPipe) != 0 || pipe(errPipe) != 0)
    {
        if (execPipe[0] >= 0)
            close(execPipe[0]);
        if (execPipe[1] >= 0)
            close(execPipe[1]);
        if (errPipe[0] >= 0)
            close(errPipe[0]);
        if (errPipe[1] >= 0)
            close(errPipe[1]);
        return false;
    }
    // The exec-report child end is CLOEXEC: on exec success the kernel
    // closes it, so the parent sees EOF == "exec ok". On exec failure the
    // child writes a '0' byte first (below).
    fcntl(execPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid = fork();
    if (pid < 0)
    {
        close(execPipe[0]);
        close(execPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return false;
    }
    if (pid == 0)
    {
        // Child.
        dup2(errPipe[1], STDERR_FILENO);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        close(execPipe[0]);
        close(errPipe[0]);
        close(errPipe[1]);

        std::vector<char *> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(program.c_str(), argv.data());
        // exec failed: report it, then exit (the parent reaps us).
        const char fail = '0';
        const ssize_t wr = write(execPipe[1], &fail, 1);
        (void)wr;
        _exit(127);
    }

    // Parent.
    close(execPipe[1]);
    close(errPipe[1]);
    m_pid = pid;
    m_execfd = execPipe[0];
    m_errfd = errPipe[0];
    // Both must be NON-BLOCKING: the read()s happen inside the GLib main
    // loop. A blocking read on an idle pipe (child alive, nothing written)
    // stalls the whole loop — every timer and redraw freezes until the next
    // write. wf-recorder/ffmpeg emit stderr in periodic bursts, so this
    // mattered. The callbacks handle EAGAIN.
    fcntl(m_execfd, F_SETFL, O_NONBLOCK);
    fcntl(m_errfd, F_SETFL, O_NONBLOCK);
    m_execSrc = g_unix_fd_add(m_execfd, G_IO_IN, &Child::execReady, this);
    m_errSrc = g_unix_fd_add(m_errfd, G_IO_IN, &Child::errReady, this);
    return true;
}

inline void
Child::detach()
{
    if (m_execSrc)
    {
        g_source_remove(m_execSrc);
        m_execSrc = 0;
    }
    if (m_errSrc)
    {
        g_source_remove(m_errSrc);
        m_errSrc = 0;
    }
    if (m_execfd >= 0)
    {
        close(m_execfd);
        m_execfd = -1;
    }
    if (m_errfd >= 0)
    {
        close(m_errfd);
        m_errfd = -1;
    }
}

inline void
Child::signal(int sig)
{
    if (running())
        ::kill(m_pid, sig);
}

inline bool
Child::waitSync(int ms)
{
    if (m_exited)
        return true;
    int waited = 0;
    while (waited < ms)
    {
        int status = 0;
        const pid_t r = waitpid(m_pid, &status, WNOHANG);
        if (r == m_pid)
        {
            m_exited = true;
            m_normalExit = WIFEXITED(status);
            m_exitCode = m_normalExit ? WEXITSTATUS(status) : -WTERMSIG(status);
            return true;
        }
        if (r < 0)
            return true; // already reaped elsewhere (ECHILD)
        usleep(20 * 1000);
        waited += 20;
    }
    return false;
}

inline void
Child::onChildExited()
{
    if (m_exited)
        return;

    // Drain the exec-report pipe to the end first: the stderr EOF (which
    // triggers this) can race the exec-failure byte, and the outcome must
    // not depend on source ordering.
    if (m_execSrc)
    {
        char buf[16];
        ssize_t n;
        while ((n = read(m_execfd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; ++i)
                if (buf[i] == '0')
                    m_execFailed = true;
        g_source_remove(m_execSrc);
        m_execSrc = 0;
    }
    if (m_errSrc)
    {
        g_source_remove(m_errSrc);
        m_errSrc = 0;
    }
    if (m_execfd >= 0)
    {
        close(m_execfd);
        m_execfd = -1;
    }
    if (m_errfd >= 0)
    {
        close(m_errfd);
        m_errfd = -1;
    }

    int status = 0;
    while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR)
        continue;
    m_exited = true;
    m_normalExit = WIFEXITED(status);
    m_exitCode = m_normalExit ? WEXITSTATUS(status) : -WTERMSIG(status);

    if (m_execFailed)
    {
        // QProcess delivers exec failure as FailedToStart (errorOccurred),
        // never as finished — mirror that: only onFailStart fires.
        if (onFailStart)
            onFailStart();
    }
    else if (onFinished)
    {
        onFinished(m_exitCode, m_normalExit);
    }
}

inline gboolean
Child::execReady(gint fd, GIOCondition cond, void *data)
{
    Child *self = static_cast<Child *>(data);
    (void)cond;
    char buf[16];
    bool failed = false;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; ++i)
            if (buf[i] == '0')
                failed = true;
    if (self->m_execSrc)
    {
        g_source_remove(self->m_execSrc);
        self->m_execSrc = 0;
    }
    if (failed)
    {
        self->m_execFailed = true;
        // onFailStart fires from onChildExited() when the stderr EOF arrives
        // (immediately after — the child _exit()'s right after writing).
    }
    else if (self->onStarted)
    {
        self->onStarted();
    }
    return G_SOURCE_REMOVE;
}

inline gboolean
Child::errReady(gint fd, GIOCondition, void *data)
{
    Child *self = static_cast<Child *>(data);
    char buf[65536];
    for (;;)
    {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0)
        {
            if (self->onStderr)
                self->onStderr(std::string(buf, static_cast<size_t>(n)));
        }
        else if (n == 0)
        {
            // EOF: every writer of the pipe is gone, so the child has
            // exited. Reap it (this also handles the exec-failure path).
            self->onChildExited();
            return G_SOURCE_REMOVE;
        }
        else if (errno == EAGAIN)
        {
            break; // g_unix_fd_add only calls us when readable; be safe
        }
        else
        {
            self->onChildExited();
            return G_SOURCE_REMOVE;
        }
    }
    return G_SOURCE_CONTINUE;
}

// A GLib timer with whole-second granularity (QTimer replacement).
//
// NOTE: built on raw g_timeout_add_seconds() rather than the glibmm
// Glib::signal_timeout() wrapper. On this box (GLib 2.88 + glibmm 2.68) the
// wrapper's source is dispatched at most ~once per long interval while raw
// sources tick exactly — verified with side-by-side probes in the same
// process. The raw API is what the wrapper uses internally, so this loses
// nothing and is what the tests on this display rely on.
class Timer
{
public:
    Timer() = default;
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
    ~Timer()
    {
        stop();
    }

    // Schedule `handler` every `seconds`; the source is removed when the
    // handler returns false (single-shot = return false) or stop() is
    // called. Starting replaces any previous schedule.
    void start(int seconds, std::function<bool()> handler)
    {
        stop();
        m_handler = std::make_unique<std::function<bool()>>(std::move(handler));
        m_id = g_timeout_add_seconds(seconds, &Timer::onTimeout, this);
    }

    void stop()
    {
        if (m_id != 0)
        {
            g_source_remove(m_id);
            m_id = 0;
        }
        m_handler.reset();
    }

    bool active() const
    {
        return m_id != 0;
    }

private:
    static gboolean
    onTimeout(gpointer data)
    {
        auto *self = static_cast<Timer *>(data);
        const bool keep = self->m_handler ? (*self->m_handler)() : false;
        if (!keep)
            self->stop();
        return keep;
    }

    guint m_id = 0;
    std::unique_ptr<std::function<bool()>> m_handler;
};
