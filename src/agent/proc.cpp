#include "agent/proc.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <thread>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <string>

namespace fs = std::filesystem;

namespace moocode {

std::string select_lines(const std::string& text, std::int64_t offset,
                         std::int64_t limit) {
    std::size_t start = 0;
    for (std::int64_t line = 1; line < offset && start < text.size(); ++line) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            start = text.size();
            break;
        }
        start = nl + 1;
    }
    if (start >= text.size()) return std::string();
    if (limit < 0) return text.substr(start);

    std::size_t end = start;
    for (std::int64_t k = 0; k < limit && end < text.size(); ++k) {
        std::size_t nl = text.find('\n', end);
        if (nl == std::string::npos) {
            end = text.size();
            break;
        }
        end = nl + 1;
    }
    return text.substr(start, end - start);
}

namespace {

// Outcome of spawning argv and draining its merged stdout/stderr. The exit
// descriptor is platform-neutral so run_process can build its trailer once:
// POSIX maps WIFEXITED/WIFSIGNALED; Windows has only an exit code (a job-kill
// on timeout is reported via timed_out, not as a signal).
struct CaptureResult {
    std::string out;
    bool truncated = false;
    bool timed_out = false;
    bool exited = false;
    int exit_code = 0;
    bool signaled = false;  // POSIX only
    int term_sig = 0;
    std::optional<std::string> error;  // pipe/fork/spawn failure (no run at all)
};

#ifdef _WIN32

// UTF-8 -> UTF-16 for the Win32 wide APIs.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n < 0 ? 0 : n), L'\0');
    if (n > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              w.data(), n);
    return w;
}

// Quote one argument per the CommandLineToArgvW rules so CreateProcessW sees the
// argv we intended (escape embedded quotes and the backslashes preceding them).
void append_quoted(std::wstring& cl, const std::wstring& arg) {
    const bool needs = arg.empty() ||
                       arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs) { cl += arg; return; }
    cl += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cl.append(backslashes * 2, L'\\');  // escape all before closing quote
            break;
        }
        if (*it == L'"') {
            cl.append(backslashes * 2 + 1, L'\\');  // escape backslashes + the quote
            cl += L'"';
        } else {
            cl.append(backslashes, L'\\');
            cl += *it;
        }
    }
    cl += L'"';
}

std::wstring build_command_line(const std::vector<std::string>& argv) {
    std::wstring cl;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cl += L' ';
        append_quoted(cl, widen(argv[i]));
    }
    return cl;
}

CaptureResult spawn_capture(const std::vector<std::string>& argv,
                            const fs::path& cwd, int timeout_secs,
                            std::size_t max_bytes) {
    CaptureResult r;

    // One inheritable pipe: child writes both stdout and stderr, parent reads.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) {
        r.error = "CreatePipe failed";
        return r;
    }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // parent end stays private

    // A job object so the whole child tree dies on timeout (and on our exit).
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli,
                                  sizeof(jeli));
    }

    std::wstring cmdline = build_command_line(argv);
    std::wstring cwd_w = cwd.empty() ? std::wstring() : cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // CREATE_SUSPENDED so we can assign to the job before any child spawns;
    // CREATE_NO_WINDOW keeps console children from flashing a window.
    BOOL ok = ::CreateProcessW(
        nullptr, cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
        cwd_w.empty() ? nullptr : cwd_w.c_str(), &si, &pi);
    ::CloseHandle(wr);  // parent never writes the child's stdin-merge pipe
    if (!ok) {
        ::CloseHandle(rd);
        if (job) ::CloseHandle(job);
        r.error = "CreateProcess failed (command not found?)";
        return r;
    }
    if (job) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    // Drain on a reader thread so the wait below can enforce the deadline: a
    // blocking ReadFile cannot be interrupted by the timeout otherwise.
    std::string out;
    bool truncated = false;
    std::thread reader([&] {
        std::array<char, 4096> buf;
        DWORD n = 0;
        while (::ReadFile(rd, buf.data(), static_cast<DWORD>(buf.size()), &n,
                          nullptr) &&
               n > 0) {
            if (out.size() < max_bytes) {
                std::size_t room = max_bytes - out.size();
                out.append(buf.data(), std::min(room, static_cast<std::size_t>(n)));
                if (static_cast<std::size_t>(n) > room) truncated = true;
            } else {
                truncated = true;  // keep draining so the child can finish
            }
        }
    });

    DWORD wait = ::WaitForSingleObject(
        pi.hProcess, static_cast<DWORD>(timeout_secs) * 1000);
    if (wait == WAIT_TIMEOUT) {
        r.timed_out = true;
        if (job) ::TerminateJobObject(job, 1);
        else ::TerminateProcess(pi.hProcess, 1);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);  // ensure write end is closed
    reader.join();                                 // pipe now broken -> EOF

    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    if (job) ::CloseHandle(job);

    r.out = std::move(out);
    r.truncated = truncated;
    if (!r.timed_out) {
        r.exited = true;
        r.exit_code = static_cast<int>(code);
    }
    return r;
}

#else  // POSIX

CaptureResult spawn_capture(const std::vector<std::string>& argv,
                            const fs::path& cwd, int timeout_secs,
                            std::size_t max_bytes) {
    CaptureResult r;

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        r.error = std::string("pipe: ") + std::strerror(errno);
        return r;
    }

    // Build the exec argv in the parent: with tool calls now running on parallel
    // threads (see Agent::run), forks happen concurrently, and the post-fork
    // child must touch only async-signal-safe calls — no heap allocation. The
    // char* point into `argv`'s strings, valid in the forked child's copy.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        r.error = std::string("fork: ") + std::strerror(errno);
        return r;
    }

    if (pid == 0) {
        // Child: own process group so the whole tree is killable on timeout.
        ::setpgid(0, 0);
        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) _exit(126);
        }
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::execvp(cargv[0], cargv.data());
        _exit(127);  // exec failed
    }

    // Parent.
    (void)::setpgid(pid, pid);  // race-safe duplicate; EACCES/ESRCH expected
    ::close(pipefd[1]);

    std::string out;
    bool truncated = false;
    bool timed_out = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
            break;
        }
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now)
                                .count();
        pollfd pfd{pipefd[0], POLLIN, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(remaining_ms));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {  // poll timeout
            timed_out = true;
            break;
        }
        std::array<char, 4096> buf;
        ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;  // EOF: child closed its end
        if (out.size() < max_bytes) {
            std::size_t room = max_bytes - out.size();
            out.append(buf.data(), std::min(room, static_cast<std::size_t>(n)));
            if (static_cast<std::size_t>(n) > room) truncated = true;
        } else {
            truncated = true;  // keep draining so the child can finish
        }
    }
    ::close(pipefd[0]);

    if (timed_out) {
        ::kill(-pid, SIGKILL);
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        r.timed_out = true;
        return r;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

    r.out = std::move(out);
    r.truncated = truncated;
    if (WIFEXITED(status)) {
        r.exited = true;
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.term_sig = WTERMSIG(status);
    }
    return r;
}

#endif

}  // namespace

std::expected<std::string, Error> run_process(
    const std::vector<std::string>& argv, const fs::path& cwd, int timeout_secs,
    std::size_t max_bytes, std::int64_t max_lines) {
    if (argv.empty())
        return std::unexpected(Error{.msg = "run_process: empty argv", .code = 0});

    CaptureResult r = spawn_capture(argv, cwd, timeout_secs, max_bytes);
    if (r.error)
        return std::unexpected(Error{.msg = *r.error, .code = 0});
    if (r.timed_out)
        return std::unexpected(Error{
            .msg = "command timed out after " + std::to_string(timeout_secs) + "s",
            .code = 0});

    std::string out = std::move(r.out);
    bool truncated = r.truncated;

    bool line_truncated = false;
    if (max_lines > 0) {
        std::int64_t lc = 1;
        for (char c : out) if (c == '\n') ++lc;
        if (lc > max_lines) {
            out = select_lines(out, 1, max_lines);
            line_truncated = true;
        }
    }

    if (truncated || line_truncated) {
        out += "\n[output truncated";
        if (truncated) {
            if (max_bytes >= 1048576 && max_bytes % 1048576 == 0)
                out += " at " + std::to_string(max_bytes / 1048576) + " MiB";
            else if (max_bytes >= 1024 && max_bytes % 1024 == 0)
                out += " at " + std::to_string(max_bytes / 1024) + " KiB";
            else
                out += " at " + std::to_string(max_bytes) + " bytes";
        }
        if (line_truncated)
            out += " to " + std::to_string(max_lines) + " lines";
        out += "]";
    }
    if (r.exited) {
        out += "\n[exit code: " + std::to_string(r.exit_code) + "]";
    } else if (r.signaled) {
        out += "\n[killed by signal " + std::to_string(r.term_sig) + "]";
    }
    return out;
}

}  // namespace moocode
