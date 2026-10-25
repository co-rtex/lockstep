#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace enginex::test {

// Unique temporary directory removed on destruction (isolates journal tests).
class TempDir {
public:
    TempDir() {
        std::string templ =
            (std::filesystem::temp_directory_path() / "enginex-test-XXXXXX").string();
        if (::mkdtemp(templ.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = templ;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_;
};

}  // namespace enginex::test
