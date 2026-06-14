#include "agent/http.hpp"
#include "agent/http_detail.hpp"

#include "agent/strutil.hpp"  // hex_val

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>

#include <atomic>
#include <cstddef>

#include <curl/curl.h>

namespace moocode::http {

namespace {

// Cap connection establishment only (DNS + TCP + TLS). This is never "the model
// reasoning" — an unreachable host should fail fast — so it stays finite even
// when the overall transfer timeout is disabled.
constexpr long kConnectTimeoutSecs = 30;

// Apply the transfer timeout policy shared by every request. A non-positive
// `timeout_secs` means "no overall cap" so a long-reasoning model is never cut
// off mid-stream; only the connect phase remains bounded.
void apply_timeouts(CURL* c, long timeout_secs) {
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSecs);
    if (timeout_secs > 0) curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_secs);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);  // thread-safe timeouts
}

// Accumulate response body. libcurl-compatible write callback.
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Context for the streaming write callback: the user's on_chunk function and
// an optional cancel flag. The write callback returns 0 (aborting the transfer)
// when cancel is non-null and points to true.
struct StreamWriteCtx {
    const std::function<void(std::string_view)>* fn;
    const std::atomic<bool>* cancel;
};

// Forward each received range to the user's callback, checking the cancel flag
// first. Returning 0 tells libcurl to abort the transfer with CURLE_WRITE_ERROR.
size_t stream_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<const StreamWriteCtx*>(userdata);
    if (ctx->cancel && ctx->cancel->load(std::memory_order_acquire))
        return 0;  // abort — curl_easy_perform returns CURLE_WRITE_ERROR
    (*ctx->fn)(std::string_view(ptr, size * nmemb));
    return size * nmemb;
}

// RAII for a curl_slist header list.
struct SListGuard {
    curl_slist* list = nullptr;
    ~SListGuard() { curl_slist_free_all(list); }
    void append(const std::string& h) { list = curl_slist_append(list, h.c_str()); }
};

// Process-wide CURLSH share so every fresh easy handle reuses the expensive
// DNS cache, TLS-session cache, and connection pool across handles AND threads
// (the TUI worker + refresher threads, and sub-agent requests). Each call keeps
// its own freshly-init'd easy handle, so there is no option-bleed and the
// streaming path is untouched; only the slow connection state is shared. One
// mutex per lock-data class with a trivial lock/unlock avoids deadlock.
struct Share {
    CURLSH* sh = nullptr;
    std::mutex mtx[CURL_LOCK_DATA_LAST];
    static void lock(CURL*, curl_lock_data d, curl_lock_access, void* u) {
        static_cast<Share*>(u)->mtx[d].lock();
    }
    static void unlock(CURL*, curl_lock_data d, void* u) {
        static_cast<Share*>(u)->mtx[d].unlock();
    }
};

// Non-null only between global_init() and global_cleanup(). When null (e.g. a
// test that never calls global_init), every request behaves exactly as before.
Share* g_share = nullptr;

// Attach the shared handle to a fresh easy handle, when the share is active.
void apply_share(CURL* c) {
    if (g_share && g_share->sh) curl_easy_setopt(c, CURLOPT_SHARE, g_share->sh);
}

bool has_content_type(const std::vector<std::string>& headers) {
    return std::ranges::any_of(headers, [](const std::string& h) {
        return h.size() >= 13 &&
               std::equal(h.begin(), h.begin() + 13, "content-type:",
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) == b;
                          });
    });
}

}  // namespace

std::expected<Response, Error> post_json(std::string_view url,
                                         const std::vector<std::string>& headers,
                                         std::string_view body, long timeout_secs) {
    if (url.empty()) return std::unexpected(Error{.msg = "empty URL", .code = 0});

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) return std::unexpected(Error{.msg = "curl_easy_init failed", .code = 0});

    std::string url_s(url);
    std::string body_s(body);
    std::string response;

    SListGuard slist;
    if (!has_content_type(headers)) slist.append("Content-Type: application/json");
    for (const auto& h : headers) slist.append(h);

    CURL* c = curl.get();
    apply_share(c);
    curl_easy_setopt(c, CURLOPT_URL, url_s.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_s.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_s.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, slist.list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    apply_timeouts(c, timeout_secs);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip/deflate

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        return std::unexpected(Error{.msg = curl_easy_strerror(rc), .code = static_cast<int>(rc), .kind = ErrorKind::Transport});

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    return Response{.status = status, .body = std::move(response)};
}

std::expected<Response, Error> get(std::string_view url,
                                   const std::vector<std::string>& headers,
                                   long timeout_secs) {
    if (url.empty()) return std::unexpected(Error{.msg = "empty URL", .code = 0});

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) return std::unexpected(Error{.msg = "curl_easy_init failed", .code = 0});

    std::string url_s(url);
    std::string response;

    SListGuard slist;
    for (const auto& h : headers) slist.append(h);

    CURL* c = curl.get();
    apply_share(c);
    curl_easy_setopt(c, CURLOPT_URL, url_s.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    if (slist.list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, slist.list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
    apply_timeouts(c, timeout_secs);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");      // allow gzip/deflate
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);       // follow redirects

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK)
        return std::unexpected(Error{.msg = curl_easy_strerror(rc), .code = static_cast<int>(rc), .kind = ErrorKind::Transport});

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    return Response{.status = status, .body = std::move(response)};
}

std::string url_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(static_cast<unsigned char>(s[i + 1]));
            int lo = hex_val(static_cast<unsigned char>(s[i + 2]));
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::expected<long, Error> post_json_stream(
    std::string_view url, const std::vector<std::string>& headers,
    std::string_view body,
    const std::function<void(std::string_view)>& on_chunk,
    const std::atomic<bool>* cancel, long timeout_secs) {
    if (url.empty()) return std::unexpected(Error{.msg = "empty URL", .code = 0});

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) return std::unexpected(Error{.msg = "curl_easy_init failed", .code = 0});

    std::string url_s(url);
    std::string body_s(body);

    SListGuard slist;
    if (!has_content_type(headers)) slist.append("Content-Type: application/json");
    for (const auto& h : headers) slist.append(h);

    // Check the cancel flag before starting the transfer so a race (Escape
    // pressed between chunks) is caught even while no data is in flight.
    if (cancel && cancel->load(std::memory_order_acquire)) {
        return std::unexpected(Error{.msg = "cancelled", .code = CURLE_ABORTED_BY_CALLBACK, .kind = ErrorKind::Cancelled});
    }

    StreamWriteCtx wctx{&on_chunk, cancel};
    CURL* c = curl.get();
    apply_share(c);
    curl_easy_setopt(c, CURLOPT_URL, url_s.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_s.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_s.size()));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, slist.list);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &wctx);
    apply_timeouts(c, timeout_secs);
    // No CURLOPT_ACCEPT_ENCODING: SSE streams are plain text and must not be
    // buffered for decompression, so chunks reach on_chunk as they arrive.

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        // CURLE_WRITE_ERROR (stream_write_cb returned 0) most commonly means
        // the cancel flag was raised mid-transfer.  Translate the cryptic
        // "Failed writing received data to disk/application" into a clean
        // "cancelled" so the user knows what happened.
        if (cancel && cancel->load(std::memory_order_acquire))
            return std::unexpected(Error{.msg = "cancelled", .code = CURLE_ABORTED_BY_CALLBACK, .kind = ErrorKind::Cancelled});
        return std::unexpected(Error{.msg = curl_easy_strerror(rc), .code = static_cast<int>(rc), .kind = ErrorKind::Transport});
    }

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    return status;
}

void global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    // Build the process-wide share. On any failure, leave g_share null so the
    // code falls back to today's independent-handle behaviour.
    auto* s = new Share();
    s->sh = curl_share_init();
    if (!s->sh) {
        delete s;
        return;
    }
    curl_share_setopt(s->sh, CURLSHOPT_LOCKFUNC, &Share::lock);
    curl_share_setopt(s->sh, CURLSHOPT_UNLOCKFUNC, &Share::unlock);
    curl_share_setopt(s->sh, CURLSHOPT_USERDATA, s);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(s->sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    g_share = s;
}

void global_cleanup() {
    if (g_share) {
        if (g_share->sh) curl_share_cleanup(g_share->sh);
        delete g_share;
        g_share = nullptr;
    }
    curl_global_cleanup();
}

}  // namespace moocode::http
