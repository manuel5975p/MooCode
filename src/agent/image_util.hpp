#ifndef FLAGENT_IMAGE_UTIL_HPP
#define FLAGENT_IMAGE_UTIL_HPP

// Image-detection and base64-encoding helpers shared across the mentions,
// agent, and TUI layers. Header-only: depends only on STL + agent_types.

#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "agent/strutil.hpp"  // base64_encode
#include "agent/types.hpp"  // Error, ImageBlock

namespace flagent {

// Image file extensions (lowercase, with dot). Ordered by commonality.
inline constexpr std::string_view kImageExtensions[] = {
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg",
    ".ico", ".tiff", ".tif", ".heic", ".heif", ".avif",
};

// True when `path` ends with a known image extension (case-insensitive).
inline bool is_image_extension(std::string_view path) {
    // Find the last dot.
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    // Lowercase the extension for comparison.
    std::string ext;
    ext.reserve(path.size() - dot);
    for (std::size_t i = dot; i < path.size(); ++i)
        ext += static_cast<char>(
            std::tolower(static_cast<unsigned char>(path[i])));
    for (std::string_view known : kImageExtensions)
        if (ext == known) return true;
    return false;
}

// Map a path's extension to an IANA media type. pre: is_image_extension(path).
inline std::string image_media_type(std::string_view path) {
    auto dot = path.rfind('.');
    std::string ext;
    if (dot != std::string::npos) {
        for (std::size_t i = dot + 1; i < path.size(); ++i)
            ext += static_cast<char>(
                std::tolower(static_cast<unsigned char>(path[i])));
    }
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "tiff" || ext == "tif") return "image/tiff";
    if (ext == "heic") return "image/heic";
    if (ext == "heif") return "image/heif";
    if (ext == "avif") return "image/avif";
    return "image/" + ext;  // best-effort fallback
}

// True when a path or token looks like it refers to an image file. Used to
// detect image paths embedded in user prompts. Accepts tokens that end with
// a known image extension.
inline bool looks_like_image_path(std::string_view text) {
    return is_image_extension(text);
}

// Read `path` as binary, base64-encode, and return an ImageBlock with the
// correct media type. Returns Error on any I/O failure or when the image
// exceeds the size cap (0 = unlimited).
inline std::expected<ImageBlock, Error>
read_image(const std::filesystem::path& path, std::size_t max_bytes = 20 * 1024 * 1024) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::unexpected(
            Error{.msg = "not a regular file: " + path.string(), .code = 0});
    auto sz = std::filesystem::file_size(path, ec);
    if (ec)
        return std::unexpected(
            Error{.msg = "cannot stat image: " + path.string(), .code = 0});
    if (max_bytes > 0 && sz > max_bytes)
        return std::unexpected(
            Error{.msg = "image too large (" + std::to_string(sz) +
                      " bytes, max " + std::to_string(max_bytes) + ")",
                  .code = 0});
    // Slurp the whole file.
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::unexpected(
            Error{.msg = "cannot open image: " + path.string(), .code = 0});
    std::string raw(std::istreambuf_iterator<char>(f), {});
    if (f.bad())
        return std::unexpected(
            Error{.msg = "read error: " + path.string(), .code = 0});
    return ImageBlock{
        .base64_data = base64_encode(raw),
        .media_type = image_media_type(path.string()),
    };
}

// Scan `text` for word-bounded tokens that end with a known image extension.
// For each one, resolve the path under `root`, read the image, and add it to
// `images` and `errors`. The text is returned unchanged; image paths are left
// in place so the model can still see their filenames. Deduplicates by
// resolved path. pre: root exists.
struct ScanImageResult {
    std::vector<ImageBlock> images;
    std::vector<std::string> errors;  // one per failed image (non-fatal)
};

inline ScanImageResult scan_image_paths(std::string_view text,
                                         const std::filesystem::path& root,
                                         std::size_t max_bytes = 20 * 1024 * 1024) {
    ScanImageResult result;
    std::vector<std::string> seen;  // dedup by resolved path
    std::size_t pos = 0;
    while (pos < text.size()) {
        // Find the start of the next token (non-whitespace).
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos >= text.size()) break;
        std::size_t start = pos;
        // Find the end of the token (whitespace or end).
        while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
        std::string_view token = text.substr(start, pos - start);
        // Strip surrounding punctuation that might be part of the sentence
        // rather than the path. Accept a leading '~' or '.' (relative paths)
        // or '/' (absolute), and strip trailing ,.;:!?)]}"' etc.
        while (!token.empty()) {
            char last = token.back();
            if (last == ',' || last == '.' || last == ';' || last == ':' ||
                last == '!' || last == '?' || last == ')' || last == ']' ||
                last == '}' || last == '"' || last == '\'')
                token.remove_suffix(1);
            else
                break;
        }
        if (token.empty()) continue;
        if (!is_image_extension(token)) continue;
        // Resolve the path.
        std::error_code ec;
        auto resolved = std::filesystem::weakly_canonical(
            std::filesystem::absolute(root / token, ec), ec);
        if (ec) {
            result.errors.push_back("cannot resolve: " + std::string(token));
            continue;
        }
        std::string resolved_str = resolved.string();
        if (std::find(seen.begin(), seen.end(), resolved_str) != seen.end())
            continue;  // duplicate
        seen.push_back(resolved_str);
        auto img = read_image(resolved, max_bytes);
        if (!img) {
            result.errors.push_back(std::string(token) + ": " + img.error().msg);
            continue;
        }
        result.images.push_back(std::move(*img));
    }
    return result;
}

}  // namespace flagent

#endif  // FLAGENT_IMAGE_UTIL_HPP
