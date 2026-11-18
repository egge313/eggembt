// A scratch directory that removes itself.
#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path()
                / ("mbt-test-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    std::string filePath(const std::string &name) const { return (path_ / name).string(); }

    // Appends raw bytes to a file (creating it if needed).
    void append(const std::string &name, const std::string &bytes) const
    {
        std::ofstream out(filePath(name), std::ios::binary | std::ios::app);
        out << bytes;
    }

private:
    std::filesystem::path path_;
};
