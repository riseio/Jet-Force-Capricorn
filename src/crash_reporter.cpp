#include "crash_reporter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef JFC_VERSION
#define JFC_VERSION "development"
#endif
#ifndef JFC_MAIN_COMMIT
#define JFC_MAIN_COMMIT "unknown"
#endif
#ifndef JFC_SOURCE_FINGERPRINT
#define JFC_SOURCE_FINGERPRINT "unknown"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#else
#include <cerrno>
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace jfc::crash_reporter {
namespace {

std::atomic<uint64_t> current_vi{0};
std::atomic<int32_t> function_lookup_target{0};
std::atomic<const char*> function_lookup_caller{nullptr};

#ifdef _WIN32
std::wstring report_path;
std::wstring dump_path;

void write_windows_report(EXCEPTION_POINTERS* exception) noexcept {
    const HANDLE report = CreateFileW(
        report_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report != INVALID_HANDLE_VALUE) {
        const DWORD code = exception != nullptr && exception->ExceptionRecord != nullptr
            ? exception->ExceptionRecord->ExceptionCode
            : 0U;
        const void* address =
            exception != nullptr && exception->ExceptionRecord != nullptr
            ? exception->ExceptionRecord->ExceptionAddress
            : nullptr;
        const char* const lookup_caller = function_lookup_caller.load(
            std::memory_order_acquire);
        const uint32_t lookup_target = static_cast<uint32_t>(
            function_lookup_target.load(std::memory_order_relaxed));
        std::array<char, 1024> text{};
        const int length = std::snprintf(
            text.data(), text.size(),
            "JFC_CRASH version=%s\nmain_commit=%s\nsource_fingerprint=%s\n"
            "platform=windows\nvi=%llu\nthread=%lu\nexception=0x%08lX\n"
            "address=%p\nfunction_lookup_target=0x%08X\n"
            "function_lookup_caller=%s\ndump=crash-last.dmp\n",
            JFC_VERSION, JFC_MAIN_COMMIT, JFC_SOURCE_FINGERPRINT,
            static_cast<unsigned long long>(current_vi.load(
                std::memory_order_relaxed)),
            static_cast<unsigned long>(GetCurrentThreadId()),
            static_cast<unsigned long>(code), address, lookup_target,
            lookup_caller != nullptr ? lookup_caller : "none");
        if (length > 0) {
            DWORD written = 0;
            WriteFile(
                report, text.data(),
                static_cast<DWORD>(std::min<size_t>(
                    static_cast<size_t>(length), text.size() - 1U)),
                &written, nullptr);
            FlushFileBuffers(report);
        }
        CloseHandle(report);
    }

    const HANDLE dump = CreateFileW(
        dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information{};
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = exception;
        information.ClientPointers = FALSE;
        MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dump,
            static_cast<MINIDUMP_TYPE>(
                MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
            exception != nullptr ? &information : nullptr,
            nullptr, nullptr);
        FlushFileBuffers(dump);
        CloseHandle(dump);
    }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* exception) noexcept {
    write_windows_report(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
std::array<char, 4096> report_path{};
std::array<std::byte, 64U * 1024U> alternate_signal_stack{};

size_t append_text(char* output, size_t capacity, size_t cursor,
                   const char* text) noexcept {
    while (text != nullptr && *text != '\0' && cursor < capacity) {
        output[cursor++] = *text++;
    }
    return cursor;
}

size_t append_unsigned(char* output, size_t capacity, size_t cursor,
                       uint64_t value, unsigned base) noexcept {
    std::array<char, 32> reversed{};
    size_t count = 0;
    do {
        const unsigned digit = static_cast<unsigned>(value % base);
        reversed[count++] = static_cast<char>(
            digit < 10U ? '0' + digit : 'A' + digit - 10U);
        value /= base;
    } while (value != 0U && count < reversed.size());
    while (count > 0U && cursor < capacity) {
        output[cursor++] = reversed[--count];
    }
    return cursor;
}

bool write_all(int file, const char* data, size_t size) noexcept {
    while (size > 0U) {
        const ssize_t written = write(file, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void write_linux_report(int signal_number, siginfo_t* information) noexcept {
    const int report = open(
        report_path.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (report < 0) {
        return;
    }
    std::array<char, 2048> text{};
    size_t cursor = 0;
    cursor = append_text(text.data(), text.size(), cursor,
        "JFC_CRASH version=" JFC_VERSION "\nmain_commit=" JFC_MAIN_COMMIT
        "\nsource_fingerprint=" JFC_SOURCE_FINGERPRINT
        "\nplatform=linux\nvi=");
    cursor = append_unsigned(text.data(), text.size(), cursor,
        current_vi.load(std::memory_order_relaxed), 10U);
    cursor = append_text(text.data(), text.size(), cursor, "\nsignal=");
    cursor = append_unsigned(text.data(), text.size(), cursor,
        static_cast<unsigned>(signal_number), 10U);
    cursor = append_text(text.data(), text.size(), cursor, "\naddress=0x");
    cursor = append_unsigned(text.data(), text.size(), cursor,
        information != nullptr
            ? reinterpret_cast<uintptr_t>(information->si_addr)
            : 0U,
        16U);
    const char* const lookup_caller = function_lookup_caller.load(
        std::memory_order_acquire);
    cursor = append_text(
        text.data(), text.size(), cursor, "\nfunction_lookup_target=0x");
    cursor = append_unsigned(
        text.data(), text.size(), cursor,
        static_cast<uint32_t>(function_lookup_target.load(
            std::memory_order_relaxed)),
        16U);
    cursor = append_text(
        text.data(), text.size(), cursor, "\nfunction_lookup_caller=");
    cursor = append_text(
        text.data(), text.size(), cursor,
        lookup_caller != nullptr ? lookup_caller : "none");
    cursor = append_text(text.data(), text.size(), cursor, "\nbacktrace:\n");
    if (!write_all(report, text.data(), cursor)) {
        (void)close(report);
        return;
    }

    std::array<void*, 64> frames{};
    const int frame_count = backtrace(frames.data(), frames.size());
    if (frame_count > 0) {
        backtrace_symbols_fd(frames.data(), frame_count, report);
    }
    (void)fsync(report);
    (void)close(report);
}

void signal_handler(int signal_number, siginfo_t* information, void*) noexcept {
    write_linux_report(signal_number, information);
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(signal_number, &action, nullptr);
    (void)raise(signal_number);
    _exit(128 + signal_number);
}
#endif

}

static void report_terminating_exception() noexcept {
    try {
        if (auto error = std::current_exception()) std::rethrow_exception(error);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fatal unhandled exception: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "Fatal unhandled non-standard exception\n");
    }
    std::fflush(stderr);
}

#ifdef _WIN32
[[noreturn]] static void abort_handler(int) noexcept {

    static std::atomic_flag reporting = ATOMIC_FLAG_INIT;
    if (!reporting.test_and_set(std::memory_order_relaxed)) {
        report_terminating_exception();
        CONTEXT context{};
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = 0xE0004A46U;
        record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
        EXCEPTION_POINTERS exception{&record, &context};
        write_windows_report(&exception);
        TerminateProcess(GetCurrentProcess(), record.ExceptionCode);
        std::_Exit(EXIT_FAILURE);
    }

    for (;;) Sleep(INFINITE);
}
#endif

void install(const std::filesystem::path& app_folder) {
#ifdef _WIN32
    report_path = (app_folder / "crash-last.txt").wstring();
    dump_path = (app_folder / "crash-last.dmp").wstring();
    SetUnhandledExceptionFilter(&unhandled_exception_filter);
    if (std::signal(SIGABRT, &abort_handler) == SIG_ERR)
        throw std::runtime_error("Failed to install fatal worker exception handler");
#else
    const std::string path = (app_folder / "crash-last.txt").string();
    if (path.size() >= report_path.size()) {
        throw std::runtime_error("Crash report path exceeds platform limit");
    }
    std::memcpy(report_path.data(), path.c_str(), path.size() + 1U);

    stack_t stack{};
    stack.ss_sp = alternate_signal_stack.data();
    stack.ss_size = alternate_signal_stack.size();
    stack.ss_flags = 0;
    if (sigaltstack(&stack, nullptr) != 0) {
        throw std::runtime_error("Failed to install crash alternate stack");
    }

    std::array<void*, 1> frame{};
    (void)backtrace(frame.data(), frame.size());
    for (const int signal_number : {SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS}) {
        struct sigaction action{};
        action.sa_sigaction = &signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        if (sigaction(signal_number, &action, nullptr) != 0) {
            throw std::runtime_error("Failed to install crash signal handler");
        }
    }
    std::set_terminate([]() noexcept { report_terminating_exception(); std::abort(); });
#endif
}

void set_vi(uint64_t vi) noexcept {
    current_vi.store(vi, std::memory_order_relaxed);
}

void set_function_lookup_failure(int32_t target, const char* caller) noexcept {
    function_lookup_target.store(target, std::memory_order_relaxed);
    function_lookup_caller.store(
        caller != nullptr ? caller : "unknown", std::memory_order_release);
}

}
