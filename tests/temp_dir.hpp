#ifndef MOOCODE_TEMP_DIR_HPP
#define MOOCODE_TEMP_DIR_HPP

// RAII unique temporary directory for filesystem tests; removed on destruction.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace moocode::test {

class TempDir {
public:
    TempDir() {
        std::string tmpl =
            (std::filesystem::temp_directory_path() / "moocode_test_XXXXXX").string();
        std::string buf = tmpl;
        int fd = ::mkstemp(buf.data());
        if (fd != -1) {
            ::close(fd);
            std::filesystem::path p(buf);
            // mkstemp created a regular file; replace it with a directory.
            std::error_code ec;
            std::filesystem::remove(p, ec);
            std::filesystem::create_directory(p, ec);
            if (!ec) path_ = p;
        }
    }
    ~TempDir() {
        std::error_code ec;
        if (!path_.empty()) std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

    // Write a file (relative to the temp dir) with the given content.
    void write(const std::string& rel, const std::string& content) const {
        auto full = path_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream(full, std::ios::binary) << content;
    }

    std::string read(const std::string& rel) const {
        std::ifstream f(path_ / rel, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

private:
    std::filesystem::path path_;
};

}  // namespace moocode::test

#endif  // MOOCODE_TEMP_DIR_HPP
