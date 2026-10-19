#pragma once

#include "atomic_file.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace jfc::modernization {

class MetadataWriter {
public:
    using Commit = files::WriteResult (*)(const std::filesystem::path&, std::span<const uint8_t>);
    using Report = void (*)(const char*);
    explicit MetadataWriter(Commit commit = files::write_atomic, Report report = nullptr);
    ~MetadataWriter();
    MetadataWriter(const MetadataWriter&) = delete;
    MetadataWriter& operator=(const MetadataWriter&) = delete;

    void submit(const std::filesystem::path&, std::vector<uint8_t>);

    void flush();

private:
    struct Snapshot { std::filesystem::path path; std::vector<uint8_t> bytes; };
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Snapshot> pending;
    std::exception_ptr failure;
    bool writing = false;
    bool stopping = false;
    Commit commit;
    Report report;
    std::thread worker;
    void run();
};
}
