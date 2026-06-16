#include "agent/lsp_client.hpp"

#include "agent/fsutil.hpp"  // slurp
#include "agent/lsp_detail.hpp"  // Framed, try_parse_frame, frame
#include "agent/strutil.hpp"  // trim_sv, hex_val, hex_digit

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace moocode::lsp {

namespace {

// Length of the UTF-8 sequence introduced by lead byte `c` (1..4); 1 for any
// stray continuation/invalid byte so we always make forward progress.
int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// Decode the code point at `s[i]` (assumes a valid boundary); returns 0 on a
// malformed lead byte. Used only to count UTF-16 units, so exactness past the
// BMP boundary is all that matters.
std::uint32_t utf8_cp(std::string_view s, std::size_t i, int len) {
    auto b = [&](std::size_t k) -> std::uint32_t {
        return k < s.size() ? static_cast<unsigned char>(s[k]) : 0;
    };
    switch (len) {
        case 2: return ((b(i) & 0x1F) << 6) | (b(i + 1) & 0x3F);
        case 3: return ((b(i) & 0x0F) << 12) | ((b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F);
        case 4: return ((b(i) & 0x07) << 18) | ((b(i + 1) & 0x3F) << 12) |
                       ((b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
        default: return b(i);
    }
}

bool ci_starts_with(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

// --- pure helpers -----------------------------------------------------------

int byte_to_lsp_char(std::string_view line, std::size_t byte_off, PosEncoding enc) {
    if (byte_off > line.size()) byte_off = line.size();
    if (enc == PosEncoding::Utf8) return static_cast<int>(byte_off);
    int units = 0;
    for (std::size_t i = 0; i < byte_off;) {
        int len = utf8_len(static_cast<unsigned char>(line[i]));
        units += utf8_cp(line, i, len) > 0xFFFF ? 2 : 1;
        i += static_cast<std::size_t>(len);
    }
    return units;
}

std::size_t lsp_char_to_byte(std::string_view line, int character, PosEncoding enc) {
    if (character <= 0) return 0;
    if (enc == PosEncoding::Utf8) {
        auto c = static_cast<std::size_t>(character);
        return c < line.size() ? c : line.size();
    }
    int units = 0;
    std::size_t i = 0;
    while (i < line.size() && units < character) {
        int len = utf8_len(static_cast<unsigned char>(line[i]));
        units += utf8_cp(line, i, len) > 0xFFFF ? 2 : 1;
        i += static_cast<std::size_t>(len);
    }
    return i;
}

std::expected<Position, Error> position_of_symbol(std::string_view line, int line1,
                                                  std::string_view symbol,
                                                  int occurrence, PosEncoding enc) {
    if (symbol.empty())
        return std::unexpected(Error{.msg = "symbol must not be empty", .code = 0});
    if (occurrence < 1) occurrence = 1;
    std::size_t from = 0;
    std::size_t at = std::string_view::npos;
    for (int found = 0; found < occurrence;) {
        at = line.find(symbol, from);
        if (at == std::string_view::npos) break;
        if (++found == occurrence) break;
        from = at + 1;
    }
    if (at == std::string_view::npos)
        return std::unexpected(Error{
            .msg = "symbol '" + std::string(symbol) + "' not found on line " +
                   std::to_string(line1) +
                   " (check the path/line, or pass `occurrence`)",
            .code = 0});
    return Position{.line = line1 - 1, .character = byte_to_lsp_char(line, at, enc)};
}

std::string frame(const nlohmann::json& msg) {
    std::string body = msg.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::expected<std::optional<Framed>, Error> try_parse_frame(std::string_view buf) {
    const std::string_view sep = "\r\n\r\n";
    std::size_t hdr_end = buf.find(sep);
    if (hdr_end == std::string_view::npos) return std::optional<Framed>{};

    std::string_view headers = buf.substr(0, hdr_end);
    std::size_t content_length = std::string_view::npos;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t eol = headers.find("\r\n", pos);
        std::string_view line =
            headers.substr(pos, eol == std::string_view::npos ? headers.size() - pos : eol - pos);
        if (ci_starts_with(line, "content-length:")) {
            std::string_view v = trim_sv(line.substr(std::strlen("content-length:")));
            constexpr std::size_t kMaxFrameBody = 256u * 1024 * 1024;  // 256 MiB
            std::size_t n = 0;
            bool any = false;
            for (char c : v) {
                if (c < '0' || c > '9') break;
                std::size_t d = static_cast<std::size_t>(c - '0');
                if (n > (kMaxFrameBody - d) / 10)  // would exceed the cap
                    return std::unexpected(Error{.msg = "LSP Content-Length too large", .code = 0});
                n = n * 10 + d;
                any = true;
            }
            if (any) content_length = n;
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 2;
    }
    if (content_length == std::string_view::npos)
        return std::unexpected(Error{.msg = "LSP frame missing Content-Length header", .code = 0});

    std::size_t body_start = hdr_end + sep.size();
    if (buf.size() - body_start < content_length) return std::optional<Framed>{};

    std::string_view body = buf.substr(body_start, content_length);
    auto v = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (v.is_discarded())
        return std::unexpected(Error{.msg = "malformed JSON in LSP frame body", .code = 0});
    return Framed{.msg = std::move(v), .consumed = body_start + content_length};
}

std::expected<fs::path, Error> uri_to_path(std::string_view uri) {
    const std::string_view scheme = "file://";
    if (!uri.starts_with(scheme))
        return std::unexpected(Error{.msg = "not a file:// URI: " + std::string(uri), .code = 0});
    std::string_view rest = uri.substr(scheme.size());
    // Skip an (empty) authority: clangd emits file:///abs, so rest == "/abs".
    std::string decoded;
    decoded.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            int hi = hex_val(static_cast<unsigned char>(rest[i + 1]));
            int lo = hex_val(static_cast<unsigned char>(rest[i + 2]));
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        decoded.push_back(rest[i]);
    }
    return fs::path(decoded);
}

std::string path_to_uri(const fs::path& abs) {
    std::string s = abs.generic_string();
    std::string out = "file://";
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                          c == '_' || c == '~' || c == '/';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex_digit(c >> 4));
            out.push_back(hex_digit(c & 0xF));
        }
    }
    return out;
}

// --- ClangdSession ----------------------------------------------------------

namespace {
const char* language_id(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (e == ".c") return "c";
    if (e == ".m") return "objective-c";
    if (e == ".mm") return "objective-cpp";
    return "cpp";
}

#ifdef _WIN32
// UTF-8 -> UTF-16 for the wide Win32 APIs.
std::wstring win_widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n < 0 ? 0 : n), L'\0');
    if (n > 0)
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                              w.data(), n);
    return w;
}

// Join argv into a single CreateProcessW command line, quoting per the
// CommandLineToArgvW rules (escape embedded quotes and the backslashes before
// them). clangd's args are path-like, so quoting handles spaces in paths.
std::wstring win_command_line(const std::vector<std::string>& args) {
    std::wstring cl;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) cl += L' ';
        std::wstring a = win_widen(args[i]);
        if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            cl += a;
            continue;
        }
        cl += L'"';
        for (auto it = a.begin();; ++it) {
            std::size_t bs = 0;
            while (it != a.end() && *it == L'\\') { ++it; ++bs; }
            if (it == a.end()) { cl.append(bs * 2, L'\\'); break; }
            if (*it == L'"') { cl.append(bs * 2 + 1, L'\\'); cl += L'"'; }
            else { cl.append(bs, L'\\'); cl += *it; }
        }
        cl += L'"';
    }
    return cl;
}
#endif

}  // namespace

ClangdSession::~ClangdSession() {
    if (alive_ && initialized_) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        if (write_message({{"jsonrpc", "2.0"}, {"id", next_id_}, {"method", "shutdown"}, {"params", nullptr}}))
            (void)pump_until(next_id_++, deadline);
        (void)notify("exit", nlohmann::json::object());
    }
    kill_and_reap();
}

std::expected<void, Error> ClangdSession::spawn() {
    // The clangd command line is identical on both platforms.
    std::vector<std::string> args{cfg_.server_path, "--background-index",
                                  "--limit-references=2000", "--limit-results=200",
                                  "--pch-storage=memory"};
    if (cfg_.compile_commands_dir)
        args.push_back("--compile-commands-dir=" + cfg_.compile_commands_dir->string());
    std::string root_str = cfg_.root.string();

#ifdef _WIN32
    // Two pipes: parent->child stdin, child stdout->parent. The read end of the
    // stdin pipe and the write end of the stdout pipe are inheritable.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_rd = nullptr, in_wr = nullptr, out_rd = nullptr, out_wr = nullptr;
    if (!::CreatePipe(&in_rd, &in_wr, &sa, 0))
        return std::unexpected(Error{.msg = "CreatePipe failed", .code = 0});
    if (!::CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        ::CloseHandle(in_rd); ::CloseHandle(in_wr);
        return std::unexpected(Error{.msg = "CreatePipe failed", .code = 0});
    }
    ::SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);   // our write end private
    ::SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);  // our read end private

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli,
                                  sizeof(jeli));
    }

    std::wstring cmdline = win_command_line(args);
    std::wstring cwd_w = root_str.empty() ? std::wstring() : cfg_.root.wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);  // clangd diagnostics -> our stderr/NUL
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(
        nullptr, cmdline.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
        cwd_w.empty() ? nullptr : cwd_w.c_str(), &si, &pi);
    ::CloseHandle(in_rd);   // child owns these now
    ::CloseHandle(out_wr);
    if (!ok) {
        ::CloseHandle(in_wr); ::CloseHandle(out_rd);
        if (job) ::CloseHandle(job);
        return std::unexpected(Error{
            .msg = "failed to start clangd '" + cfg_.server_path +
                   "' (install clangd or set CLANGD_PATH)",
            .code = 0});
    }
    if (job) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    proc_ = pi.hProcess;
    job_ = job;
    to_child_w_ = in_wr;
    from_child_r_ = out_rd;
    reader_done_ = false;
    read_buf_.clear();
    // Reader thread: drain clangd's stdout into read_buf_ until the pipe breaks.
    HANDLE rd_handle = out_rd;
    reader_ = std::thread([this, rd_handle] {
        std::array<char, 8192> buf;
        DWORD n = 0;
        for (;;) {
            if (!::ReadFile(rd_handle, buf.data(), static_cast<DWORD>(buf.size()),
                            &n, nullptr) ||
                n == 0)
                break;
            std::lock_guard<std::mutex> lk(read_mtx_);
            read_buf_.append(buf.data(), static_cast<std::size_t>(n));
            read_cv_.notify_one();
        }
        std::lock_guard<std::mutex> lk(read_mtx_);
        reader_done_ = true;
        read_cv_.notify_one();
    });
#else
    // Ignore SIGPIPE once: writing to a clangd that has exited must not kill us.
    static std::once_flag sigpipe_once;
    std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });

    int in_pipe[2];   // parent -> child stdin
    int out_pipe[2];  // child stdout -> parent
    if (::pipe(in_pipe) != 0)
        return std::unexpected(Error{.msg = std::string("pipe: ") + std::strerror(errno), .code = 0});
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return std::unexpected(Error{.msg = std::string("pipe: ") + std::strerror(errno), .code = 0});
    }

    // Build argv in the parent so the child path is async-signal-safe (no STL).
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        return std::unexpected(Error{.msg = std::string("fork: ") + std::strerror(errno), .code = 0});
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        if (!root_str.empty() && ::chdir(root_str.c_str()) != 0) _exit(126);
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
        ::close(in_pipe[0]); ::close(in_pipe[1]);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::execvp(argv[0], argv.data());
        _exit(127);  // exec failed (binary not found)
    }

    ::setpgid(pid, pid);  // race-safe duplicate of the child's setpgid
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    to_child_ = in_pipe[1];
    from_child_ = out_pipe[0];
    if (int fl = ::fcntl(from_child_, F_GETFL, 0); fl >= 0)
        ::fcntl(from_child_, F_SETFL, fl | O_NONBLOCK);
    pid_ = pid;
#endif

    alive_ = true;
    initialized_ = false;
    index_done_ = false;
    next_id_ = 1;
    in_buf_.clear();
    index_tokens_.clear();
    open_docs_.clear();
    return {};
}

void ClangdSession::kill_and_reap() {
#ifdef _WIN32
    if (to_child_w_) { ::CloseHandle(to_child_w_); to_child_w_ = nullptr; }  // EOF -> clangd exits
    if (proc_) {
        // Give clangd a moment to exit on its closed stdin, then kill the tree.
        if (::WaitForSingleObject(proc_, 300) != WAIT_OBJECT_0) {
            if (job_) ::TerminateJobObject(job_, 1);
            else ::TerminateProcess(proc_, 1);
            ::WaitForSingleObject(proc_, INFINITE);
        }
        ::CloseHandle(proc_);
        proc_ = nullptr;
    }
    if (job_) { ::CloseHandle(job_); job_ = nullptr; }
    if (reader_.joinable()) reader_.join();  // pipe broken -> reader has returned
    if (from_child_r_) { ::CloseHandle(from_child_r_); from_child_r_ = nullptr; }
    {
        std::lock_guard<std::mutex> lk(read_mtx_);
        read_buf_.clear();
        reader_done_ = false;
    }
#else
    if (to_child_ >= 0) { ::close(to_child_); to_child_ = -1; }  // EOF -> clangd exits
    if (pid_ > 0) {
        ::kill(-pid_, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 30; ++i) {  // up to ~300ms for a graceful exit
            int st = 0;
            pid_t r = ::waitpid(pid_, &st, WNOHANG);
            if (r == pid_ || (r < 0 && errno == ECHILD)) { reaped = true; break; }
            ::poll(nullptr, 0, 10);
        }
        if (!reaped) {
            ::kill(-pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
    }
    if (from_child_ >= 0) { ::close(from_child_); from_child_ = -1; }
#endif
    alive_ = false;
    initialized_ = false;
}

std::expected<void, Error> ClangdSession::write_message(const nlohmann::json& msg) {
    std::string wire = frame(msg);
    return write_raw(wire.data(), wire.size());
}

std::expected<void, Error> ClangdSession::write_raw(const char* data, std::size_t len) {
#ifdef _WIN32
    if (!to_child_w_)
        return std::unexpected(Error{.msg = "clangd is not running", .code = 0});
    std::size_t off = 0;
    while (off < len) {
        DWORD wrote = 0;
        if (!::WriteFile(to_child_w_, data + off, static_cast<DWORD>(len - off),
                         &wrote, nullptr) ||
            wrote == 0) {
            alive_ = false;
            return std::unexpected(Error{.msg = "clangd write failed", .code = 0});
        }
        off += wrote;
    }
    return {};
#else
    if (to_child_ < 0)
        return std::unexpected(Error{.msg = "clangd is not running", .code = 0});
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(to_child_, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            alive_ = false;
            return std::unexpected(Error{.msg = std::string("clangd write failed: ") + std::strerror(errno), .code = 0});
        }
        off += static_cast<std::size_t>(n);
    }
    return {};
#endif
}

ClangdSession::ReadStep ClangdSession::read_step(int slice_ms, std::string& err) {
#ifdef _WIN32
    std::unique_lock<std::mutex> lk(read_mtx_);
    if (read_buf_.empty() && !reader_done_)
        read_cv_.wait_for(lk, std::chrono::milliseconds(slice_ms));
    if (!read_buf_.empty()) {
        in_buf_.append(read_buf_);
        read_buf_.clear();
        return ReadStep::Data;
    }
    return reader_done_ ? ReadStep::Eof : ReadStep::Timeout;
#else
    pollfd pfd{from_child_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, slice_ms);
    if (pr < 0) {
        if (errno == EINTR) return ReadStep::Timeout;
        err = std::string("clangd poll: ") + std::strerror(errno);
        return ReadStep::Error;
    }
    if (pr == 0) return ReadStep::Timeout;

    std::array<char, 8192> buf;
    ssize_t n = ::read(from_child_, buf.data(), buf.size());
    if (n == 0) return ReadStep::Eof;
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return ReadStep::Timeout;
        err = std::string("clangd read: ") + std::strerror(errno);
        return ReadStep::Error;
    }
    in_buf_.append(buf.data(), static_cast<std::size_t>(n));
    return ReadStep::Data;
#endif
}

void ClangdSession::dispatch(const nlohmann::json& msg) {
    if (!msg.contains("method") || !msg["method"].is_string()) return;  // stray response
    const std::string method = msg["method"].get<std::string>();

    if (msg.contains("id")) {  // server -> client request: must reply or clangd stalls
        nlohmann::json reply{{"jsonrpc", "2.0"}, {"id", msg["id"]}};
        if (method == "workspace/configuration") {
            nlohmann::json items = nlohmann::json::array();
            if (msg.contains("params") && msg["params"].contains("items") &&
                msg["params"]["items"].is_array())
                for (std::size_t i = 0; i < msg["params"]["items"].size(); ++i)
                    items.push_back(nlohmann::json::object());
            reply["result"] = items;
        } else if (method == "window/workDoneProgress/create" ||
                   method == "client/registerCapability" ||
                   method == "client/unregisterCapability") {
            reply["result"] = nullptr;
        } else {
            reply["error"] = {{"code", -32601}, {"message", "method not found"}};
        }
        (void)write_message(reply);
        return;
    }

    // Notification. Track background-index completion; ignore the rest. clangd's
    // progress token is not a stable string, so an indexing pass is identified by
    // the well-known token name OR a 'begin' title that mentions indexing, then
    // matched to its 'end' by token.
    if (method == "$/progress" && msg.contains("params")) {
        const auto& p = msg["params"];
        if (p.contains("token") && p.contains("value") && p["value"].contains("kind")) {
            std::string tok = p["token"].dump();  // normalize string|number token
            bool named_index = p["token"].is_string() &&
                               p["token"].get<std::string>() == "backgroundIndexProgress";
            const std::string kind = p["value"].value("kind", std::string());
            if (kind == "begin") {
                std::string title = p["value"].value("title", std::string());
                for (char& c : title) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                if (named_index || title.find("indexing") != std::string::npos)
                    index_tokens_.insert(tok);
            } else if (kind == "end" && (named_index || index_tokens_.count(tok))) {
                index_tokens_.erase(tok);
                index_done_ = true;  // an indexing pass completed
            }
        }
    }
}

std::expected<nlohmann::json, Error> ClangdSession::pump_until(
    std::optional<int> want_id, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        // 1. Consume everything already buffered before blocking on the pipe.
        for (;;) {
            auto framed = try_parse_frame(in_buf_);
            if (!framed) {  // malformed frame: the stream is unusable
                alive_ = false;
                return std::unexpected(framed.error());
            }
            if (!framed->has_value()) break;  // need more bytes
            nlohmann::json msg = std::move((*framed)->msg);
            in_buf_.erase(0, (*framed)->consumed);

            static const bool debug = std::getenv("MOOCODE_LSP_DEBUG") != nullptr;
            if (debug) {
                std::string tag = msg.value("method", std::string());
                if (tag.empty() && msg.contains("id")) tag = "response#" + msg["id"].dump();
                std::string extra;
                if (tag == "$/progress" && msg.contains("params")) {
                    const auto& p = msg["params"];
                    extra = " token=" + (p.contains("token") ? p["token"].dump() : "?") +
                            " kind=" + (p.contains("value") ? p["value"].value("kind", std::string("?")) : "?") +
                            " title=" + (p.contains("value") ? p["value"].value("title", std::string()) : "");
                }
                std::fprintf(stderr, "[lsp<-] %s%s\n", tag.c_str(), extra.c_str());
            }

            bool is_response = msg.contains("id") && !msg.contains("method");
            if (want_id && is_response && msg["id"].is_number_integer() &&
                msg["id"].get<int>() == *want_id) {
                if (msg.contains("error") && !msg["error"].is_null()) {
                    std::string m = msg["error"].value("message", std::string("clangd error"));
                    return std::unexpected(Error{.msg = "clangd: " + m, .code = 0});
                }
                return msg.contains("result") ? msg["result"] : nlohmann::json(nullptr);
            }
            dispatch(msg);
            if (!want_id && index_done_) return nlohmann::json(nullptr);  // index-wait done
        }

        if (cancel_ && cancel_->load())
            return std::unexpected(Error{.msg = "cancelled", .code = 0});
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (want_id)
                return std::unexpected(Error{.msg = "clangd request timed out", .code = 0});
            return nlohmann::json(nullptr);  // index wait expired (not an error)
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int slice = static_cast<int>(remaining < 200 ? remaining : 200);

        std::string err;
        switch (read_step(slice, err)) {
            case ReadStep::Data:
                break;  // new bytes in in_buf_: loop back to frame them
            case ReadStep::Timeout:
                continue;  // re-check deadline/cancel
            case ReadStep::Error:
                alive_ = false;
                return std::unexpected(Error{.msg = err, .code = 0});
            case ReadStep::Eof: {  // clangd closed stdout / exited
                alive_ = false;
#ifndef _WIN32
                int st = 0;
                bool exited = pid_ > 0 && ::waitpid(pid_, &st, WNOHANG) == pid_;
                if (exited && WIFEXITED(st) && WEXITSTATUS(st) == 127) {
                    pid_ = -1;
                    return std::unexpected(Error{
                        .msg = "clangd not found or failed to exec '" + cfg_.server_path +
                               "' (install clangd or set CLANGD_PATH)",
                        .code = 0});
                }
#endif
                return std::unexpected(Error{.msg = "clangd exited unexpectedly", .code = 0});
            }
        }
    }
}

std::expected<nlohmann::json, Error> ClangdSession::request_until(
    std::string_view method, const nlohmann::json& params,
    std::chrono::steady_clock::time_point deadline) {
    int id = next_id_++;
    nlohmann::json msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    if (auto w = write_message(msg); !w) return std::unexpected(w.error());
    return pump_until(id, deadline);
}

std::expected<nlohmann::json, Error> ClangdSession::request(std::string_view method,
                                                            const nlohmann::json& params) {
    return request_until(method, params, std::chrono::steady_clock::now() + cfg_.request_timeout);
}

std::expected<void, Error> ClangdSession::notify(std::string_view method,
                                                 const nlohmann::json& params) {
    return write_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

std::expected<void, Error> ClangdSession::handshake() {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::absolute(cfg_.root, ec), ec);
    if (ec) canon = cfg_.root;
    std::string root_uri = path_to_uri(canon);

    nlohmann::json caps{
        {"general", {{"positionEncodings", nlohmann::json::array({"utf-8", "utf-16"})}}},
        {"textDocument",
         {{"hover", {{"contentFormat", nlohmann::json::array({"markdown", "plaintext"})}}},
          {"definition", {{"linkSupport", true}}},
          {"implementation", {{"linkSupport", true}}},
          {"references", nlohmann::json::object()},
          {"documentSymbol", {{"hierarchicalDocumentSymbolSupport", true}}},
          {"callHierarchy", nlohmann::json::object()},
          {"rename", {{"prepareSupport", false}}}}},
        {"workspace",
         {{"symbol", nlohmann::json::object()}, {"configuration", true}, {"workspaceFolders", true}}},
        {"window", {{"workDoneProgress", true}}}};

    nlohmann::json params{
#ifdef _WIN32
        {"processId", static_cast<int>(::GetCurrentProcessId())},
#else
        {"processId", static_cast<int>(::getpid())},
#endif
        {"clientInfo", {{"name", "moocode"}}},
        {"rootUri", root_uri},
        {"rootPath", canon.string()},
        {"capabilities", caps},
        {"workspaceFolders", nlohmann::json::array({{{"uri", root_uri}, {"name", "root"}}})}};

    auto res = request("initialize", params);
    if (!res) return std::unexpected(res.error());

    encoding_ = PosEncoding::Utf16;
    if (res->contains("capabilities") && (*res)["capabilities"].contains("positionEncoding")) {
        const auto& enc = (*res)["capabilities"]["positionEncoding"];
        if (enc.is_string() && enc.get<std::string>() == "utf-8") encoding_ = PosEncoding::Utf8;
    }
    if (auto n = notify("initialized", nlohmann::json::object()); !n)
        return std::unexpected(n.error());
    initialized_ = true;
    if (cfg_.index_wait.count() > 0) wait_for_index(cfg_.index_wait);
    return {};
}

std::expected<void, Error> ClangdSession::ensure_started() {
    if (alive_ && initialized_) return {};
    if (alive_) kill_and_reap();  // a half-open session from a failed handshake
    if (auto s = spawn(); !s) return s;
    return handshake();
}

std::expected<void, Error> ClangdSession::sync_file(const fs::path& abs) {
    std::string content = slurp(abs);
    std::size_t h = std::hash<std::string>{}(content);
    auto it = open_docs_.find(abs);
    if (it == open_docs_.end()) {
        nlohmann::json params{{"textDocument",
                               {{"uri", path_to_uri(abs)},
                                {"languageId", language_id(abs)},
                                {"version", 1},
                                {"text", content}}}};
        if (auto n = notify("textDocument/didOpen", params); !n) return std::unexpected(n.error());
        open_docs_.emplace(abs, std::pair<int, std::size_t>{1, h});
        return {};
    }
    if (it->second.second != h) {
        int ver = ++it->second.first;
        it->second.second = h;
        nlohmann::json params{
            {"textDocument", {{"uri", path_to_uri(abs)}, {"version", ver}}},
            {"contentChanges", nlohmann::json::array({{{"text", content}}})}};
        if (auto n = notify("textDocument/didChange", params); !n) return std::unexpected(n.error());
    }
    return {};
}

std::expected<nlohmann::json, Error> ClangdSession::doc_pos_request(
    std::string_view method, const fs::path& abs, Position pos, const nlohmann::json& extra) {
    if (auto s = ensure_started(); !s) return std::unexpected(s.error());
    if (auto s = sync_file(abs); !s) return std::unexpected(s.error());
    nlohmann::json params{{"textDocument", {{"uri", path_to_uri(abs)}}},
                          {"position", {{"line", pos.line}, {"character", pos.character}}}};
    for (auto& [k, v] : extra.items()) params[k] = v;
    return request(method, params);
}

std::expected<nlohmann::json, Error> ClangdSession::definition(const fs::path& f, Position p) {
    return doc_pos_request("textDocument/definition", f, p);
}
std::expected<nlohmann::json, Error> ClangdSession::implementation(const fs::path& f, Position p) {
    return doc_pos_request("textDocument/implementation", f, p);
}
std::expected<nlohmann::json, Error> ClangdSession::references(const fs::path& f, Position p,
                                                               bool include_declaration) {
    return doc_pos_request("textDocument/references", f, p,
                           {{"context", {{"includeDeclaration", include_declaration}}}});
}
std::expected<nlohmann::json, Error> ClangdSession::hover(const fs::path& f, Position p) {
    return doc_pos_request("textDocument/hover", f, p);
}
std::expected<nlohmann::json, Error> ClangdSession::prepare_call_hierarchy(const fs::path& f,
                                                                           Position p) {
    return doc_pos_request("textDocument/prepareCallHierarchy", f, p);
}
std::expected<nlohmann::json, Error> ClangdSession::rename(const fs::path& f, Position p,
                                                           std::string_view new_name) {
    return doc_pos_request("textDocument/rename", f, p, {{"newName", std::string(new_name)}});
}

std::expected<nlohmann::json, Error> ClangdSession::document_symbols(const fs::path& abs) {
    if (auto s = ensure_started(); !s) return std::unexpected(s.error());
    if (auto s = sync_file(abs); !s) return std::unexpected(s.error());
    return request("textDocument/documentSymbol",
                   {{"textDocument", {{"uri", path_to_uri(abs)}}}});
}

std::expected<nlohmann::json, Error> ClangdSession::workspace_symbols(std::string_view query) {
    if (auto s = ensure_started(); !s) return std::unexpected(s.error());
    return request("workspace/symbol", {{"query", std::string(query)}});
}

std::expected<nlohmann::json, Error> ClangdSession::incoming_calls(const nlohmann::json& item) {
    return request("callHierarchy/incomingCalls", {{"item", item}});
}
std::expected<nlohmann::json, Error> ClangdSession::outgoing_calls(const nlohmann::json& item) {
    return request("callHierarchy/outgoingCalls", {{"item", item}});
}

bool ClangdSession::wait_for_index(std::optional<std::chrono::milliseconds> limit) {
    if (index_done_) return true;
    if (auto s = ensure_started(); !s) return index_done_;
    auto budget = limit.value_or(cfg_.index_wait);
    if (budget.count() <= 0) return index_done_;
    (void)pump_until(std::nullopt, std::chrono::steady_clock::now() + budget);
    return index_done_;
}

}  // namespace moocode::lsp
