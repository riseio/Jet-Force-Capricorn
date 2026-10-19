#include "metadata_writer.hpp"
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace jfc::modernization {
MetadataWriter::MetadataWriter(Commit commit, Report report)
    : commit(commit), report(report), worker([this] { run(); }) {}

MetadataWriter::~MetadataWriter() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    worker.join();
}

void MetadataWriter::submit(const std::filesystem::path& path, std::vector<uint8_t> bytes) {
    if (path.empty()) throw std::invalid_argument("QoL save path is empty");
    {
        std::lock_guard lock(mutex);
        if (failure) std::rethrow_exception(failure);
        if (stopping) throw std::logic_error("QoL save writer has stopped");

        pending = Snapshot{path, std::move(bytes)};
    }
    changed.notify_one();
}

void MetadataWriter::flush() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return !writing && !pending; });
    if (failure) std::rethrow_exception(failure);
}

void MetadataWriter::run() {
    std::unique_lock lock(mutex);
    for (;;) {
        changed.wait(lock, [&] { return stopping || pending.has_value(); });
        if (!pending) return;
        Snapshot snapshot = std::move(*pending);
        pending.reset();
        writing = true;
        lock.unlock();
        std::exception_ptr error;
        try {
            const auto result = commit(snapshot.path, snapshot.bytes);
            if (!result.durable) throw std::runtime_error(result.warning);
        }
        catch (...) { error = std::current_exception(); }

        lock.lock();
        writing = false;
        if (error) {
            failure = error;
            pending.reset();
        }
        changed.notify_all();
        if (error) {
            lock.unlock();
            try { std::rethrow_exception(error); }
            catch (const std::exception& e) {
                const auto path = snapshot.path.u8string();
                const std::string message = "QoL progress could not be saved to " +
                    std::string(reinterpret_cast<const char*>(path.data()), path.size()) + ": " + e.what();
                std::fprintf(stderr, "%s\n", message.c_str());
                try { if (report) report(message.c_str()); }
                catch (...) { std::fprintf(stderr, "QoL save error notification could not be queued\n"); }
            }
            catch (...) {
                std::fprintf(stderr, "QoL save failed with an unknown error\n");
                try { if (report) report("QoL progress could not be saved: unknown storage error."); }
                catch (...) { std::fprintf(stderr, "QoL save error notification could not be queued\n"); }
            }
            lock.lock();
        }
        if (stopping && !pending) return;
    }
}
}
