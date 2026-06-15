#include "agent/proc.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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

std::expected<std::string, Error> run_process(
    const std::vector<std::string>& argv, const fs::path& cwd, int timeout_secs,
    std::size_t max_bytes, std::int64_t max_lines) {
    if (argv.empty())
        return std::unexpected(Error{.msg = "run_process: empty argv", .code = 0});

    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return std::unexpected(Error{.msg = std::string("pipe: ") + std::strerror(errno), .code = 0});

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
        return std::unexpected(Error{.msg = std::string("fork: ") + std::strerror(errno), .code = 0});
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
        return std::unexpected(Error{.msg = "command timed out after " + std::to_string(timeout_secs) + "s",
            .code = 0});
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}

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
    if (WIFEXITED(status)) {
        out += "\n[exit code: " + std::to_string(WEXITSTATUS(status)) + "]";
    } else if (WIFSIGNALED(status)) {
        out += "\n[killed by signal " + std::to_string(WTERMSIG(status)) + "]";
    }
    return out;
}

}  // namespace moocode
