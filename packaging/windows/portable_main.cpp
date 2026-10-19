#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <compressapi.h>
#include <bcrypt.h>
#include <dbghelp.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::span<const unsigned char>;
constexpr size_t max_payload = 512u * 1024u * 1024u;
constexpr uint32_t max_file = 128u * 1024u * 1024u;

[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
void check(BOOL success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) +
        " (Windows error " + std::to_string(GetLastError()) + ")");
}
class Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
    void reset() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
};

std::array<unsigned char, 32> digest(Bytes bytes) {
    std::array<unsigned char, 32> result{};
    if (bytes.size() > max_payload || BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
            nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
            result.data(), static_cast<ULONG>(result.size())) < 0)
        fail("Unable to verify portable payload SHA-256");
    return result;
}
Bytes resource(WORD id) {
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!info) fail("Portable EXE has no embedded runtime. Use the complete portable EXE.");
    DWORD size = SizeofResource(nullptr, info);
    if (size == 0 || size > max_payload) fail("Invalid portable resource size");
    auto data = static_cast<const unsigned char*>(LockResource(LoadResource(nullptr, info)));
    if (!data) fail("Cannot read portable resource");
    return {data, size};
}
class Reader {
    Bytes remaining_;
public:
    explicit Reader(Bytes bytes) : remaining_(bytes) {}
    Bytes take(size_t size) {
        if (size > remaining_.size()) fail("Truncated portable payload");
        auto result = remaining_.first(size); remaining_ = remaining_.subspan(size); return result;
    }
    uint32_t integer() {
        auto b = take(4);
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }
    bool empty() const { return remaining_.empty(); }
};
struct Entry { fs::path path; uint32_t size; Bytes hash; Bytes compressed; };
std::string safe_name(Bytes bytes) {
    if (bytes.empty() || bytes.size() > 200) fail("Invalid portable filename length");
    std::string name(bytes.begin(), bytes.end());
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
            fail("Unsafe portable filename");
    }
    size_t start = 0, depth = 0;
    do {
        size_t end = name.find('/', start);
        auto part = name.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || ++depth > 8)
            fail("Unsafe portable path component");
        std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return char(c >= 'a' && c <= 'z' ? c - 32 : c); });
        auto base = part.substr(0, part.find('.'));
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9'))
            fail("Reserved Windows device filename in portable payload");
        if (end == std::string::npos) break;
        start = end + 1;
    } while (true);
    return name;
}
std::vector<Entry> parse(Bytes payload) {
    Reader reader(payload);
    auto magic = reader.take(8);
    if (std::memcmp(magic.data(), "JFCPORT1", 8) != 0) fail("Unsupported portable payload format");
    uint32_t count = reader.integer();
    if (!count || count > 512) fail("Invalid portable file count");
    std::vector<Entry> entries;
    std::vector<std::string> names;
    size_t total = 0;
    bool executable = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length = reader.integer(), size = reader.integer(), packed = reader.integer();
        if (size > max_file || packed > max_payload || total > max_payload - size)
            fail("Portable expanded size limit exceeded");
        total += size;
        auto hash = reader.take(32);
        auto name = safe_name(reader.take(length));
        auto compressed = reader.take(packed);
        if ((size == 0) != (packed == 0)) fail("Invalid empty portable file");
        executable |= name == "JetForceCapricorn.exe";
        entries.push_back({fs::path(name), size, hash, compressed});
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
        if (name == "portable.txt" || name.starts_with("data/")) fail("Reserved portable state path in runtime payload");
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    for (size_t i = 1; i < names.size(); ++i) {
        if (names[i] == names[i - 1] || names[i].starts_with(names[i - 1] + '/'))
            fail("Conflicting portable payload paths");
    }
    if (!reader.empty() || !executable) fail("Incomplete or trailing portable payload data");
    return entries;
}

Handle directory(const fs::path& path, bool create) {
    if (create && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        check(FALSE, "Portable directory must be writable; cannot create directory");
    Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) check(FALSE, "Cannot open portable directory");
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(handle.get(), &info), "Cannot inspect portable directory");
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        fail("Portable data/runtime directories cannot be files, symlinks or junctions");
    return handle;
}
void remove_runtime(const fs::path& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) return;
        check(FALSE, "Cannot inspect old portable runtime");
    }
    auto pin = directory(path, false);
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((path / L"*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) check(FALSE, "Cannot enumerate portable runtime");
    try {
        do {
            if (std::wcscmp(item.cFileName, L".") == 0 || std::wcscmp(item.cFileName, L"..") == 0) continue;
            auto child = path / item.cFileName;
            if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_runtime(child);
            else if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                check(RemoveDirectoryW(child.c_str()), "Cannot remove runtime directory link");
            else check(DeleteFileW(child.c_str()), "Cannot remove portable runtime file");
        } while (FindNextFileW(search, &item));
        if (GetLastError() != ERROR_NO_MORE_FILES) check(FALSE, "Cannot finish portable runtime enumeration");
    } catch (...) { FindClose(search); throw; }
    FindClose(search);
    pin.reset();
    check(RemoveDirectoryW(path.c_str()), "Cannot remove portable runtime directory");
}
void extract(const std::vector<Entry>& entries, const fs::path& root) {
    std::vector<Handle> pins;
    pins.push_back(directory(root, true));
    pins.push_back(directory(root / L"temp", true));
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    check(CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor), "Cannot create Windows decompressor");
    try {
        for (const auto& entry : entries) {
            auto parent = root;
            for (const auto& component : entry.path.parent_path()) {
                parent /= component;
                pins.push_back(directory(parent, true));
            }
            std::vector<unsigned char> data(entry.size);
            if (entry.size) {
                SIZE_T actual = 0;
                check(Decompress(decompressor, const_cast<PUCHAR>(entry.compressed.data()), entry.compressed.size(),
                    data.data(), data.size(), &actual), "Portable runtime decompression failed");
                if (actual != data.size()) fail("Portable expanded file size mismatch");
            }
            auto hash = digest(data);
            if (!std::equal(hash.begin(), hash.end(), entry.hash.begin())) fail("Portable file SHA-256 mismatch");
            Handle file(CreateFileW((root / entry.path).c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE) check(FALSE, "Cannot create portable runtime file");
            DWORD written = 0;
            check(WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr), "Cannot write portable runtime file");
            if (written != data.size()) fail("Short portable runtime write");
        }
    } catch (...) { CloseDecompressor(decompressor); throw; }
    CloseDecompressor(decompressor);
}
fs::path executable_path() {
    std::wstring buffer(256, L'\0');
    for (;;) {
        DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) check(FALSE, "Cannot locate portable EXE");
        if (count < buffer.size()) { buffer.resize(count); return fs::path(buffer); }
        if (buffer.size() >= 32768) fail("Portable executable path is too long");
        buffer.resize(std::min<size_t>(32768, buffer.size() * 2));
    }
}
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}

void append_crash_log(HANDLE log, const char* text, size_t length) noexcept {
    LARGE_INTEGER end{};
    DWORD written = 0;
    if (!SetFilePointerEx(log, end, nullptr, FILE_END) ||
        !WriteFile(log, text, static_cast<DWORD>(length), &written, nullptr) || written != length ||
        !FlushFileBuffers(log)) {
        std::fprintf(stderr, "Portable crash log write failed (Windows error %lu)\n", GetLastError());
    }
}
Handle crash_output(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) check(FALSE, "Cannot open crash output");
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(file.get(), &info), "Cannot inspect crash output");
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1)
        fail("Crash output cannot be a link or directory");
    check(SetEndOfFile(file.get()), "Cannot reset crash output");
    return file;
}
void capture_crash(HANDLE process, HANDLE log, const fs::path& data, const DEBUG_EVENT& event) noexcept {
    std::array<char, 4096> text{};
    EXCEPTION_RECORD record = event.u.Exception.ExceptionRecord;

    record.ExceptionRecord = nullptr;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_ALL;
    Handle thread(OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId));
    const bool contextCaptured = thread.get() != nullptr && GetThreadContext(thread.get(), &context);
    const DWORD contextError = contextCaptured ? 0 : GetLastError();
    MEMORY_BASIC_INFORMATION memory{};
    VirtualQueryEx(process, record.ExceptionAddress, &memory, sizeof(memory));
    std::array<wchar_t, 32768> module{};
    if (memory.AllocationBase)
        K32GetModuleFileNameExW(process, static_cast<HMODULE>(memory.AllocationBase), module.data(), DWORD(module.size()));
    std::array<char, 2048> moduleUtf8{};
    if (!WideCharToMultiByte(CP_UTF8, 0, module.data(), -1, moduleUtf8.data(), int(moduleUtf8.size()), nullptr, nullptr))
        std::strcpy(moduleUtf8.data(), "unavailable");
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const int count = std::snprintf(text.data(), text.size(),
        "JFC_PORTABLE_CRASH version=%s main=%s source=%s\n"
        "utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ process_id=%lu thread_id=%lu first_chance=%lu\n"
        "exception_code=0x%08lX exception_flags=0x%08lX exception_address=%p parameters=%lu parameter0=0x%llX parameter1=0x%llX\n"
        "context_captured=%u context_error=%lu rip=0x%llX rsp=0x%llX\n"
        "fault_module=%s module_base=%p module_offset=0x%llX\n",
        JFC_VERSION, JFC_MAIN_COMMIT, JFC_SOURCE_FINGERPRINT,
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        event.dwProcessId, event.dwThreadId, event.u.Exception.dwFirstChance,
        record.ExceptionCode, record.ExceptionFlags, record.ExceptionAddress, record.NumberParameters,
        record.NumberParameters > 0 ? static_cast<unsigned long long>(record.ExceptionInformation[0]) : 0ULL,
        record.NumberParameters > 1 ? static_cast<unsigned long long>(record.ExceptionInformation[1]) : 0ULL,
        unsigned(contextCaptured), contextError, context.Rip, context.Rsp, moduleUtf8.data(), memory.AllocationBase,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record.ExceptionAddress) - reinterpret_cast<uintptr_t>(memory.AllocationBase)));
    const size_t length = count > 0 ? std::min(size_t(count), text.size() - 1) : 0;
    append_crash_log(log, text.data(), length);
    bool dumpWritten = false, reportWritten = false;
    DWORD dumpError = 0, reportError = 0;
    try {
        auto dump = crash_output(data / L"crash-last.dmp");
        EXCEPTION_POINTERS pointers{&record, &context};
        MINIDUMP_EXCEPTION_INFORMATION exception{event.dwThreadId, &pointers, FALSE};
        dumpWritten = MiniDumpWriteDump(process, event.dwProcessId, dump.get(),
            static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            contextCaptured ? &exception : nullptr, nullptr, nullptr) != FALSE;
        if (!dumpWritten) dumpError = GetLastError();
        if (!FlushFileBuffers(dump.get())) { dumpWritten = false; dumpError = GetLastError(); }
    } catch (const std::exception& error) {
        dumpError = GetLastError();
        append_crash_log(log, error.what(), std::strlen(error.what()));
    }
    try {
        auto report = crash_output(data / L"crash-last.txt");
        DWORD written = 0;
        reportWritten = WriteFile(report.get(), text.data(), DWORD(length), &written, nullptr) && written == length && FlushFileBuffers(report.get());
        if (!reportWritten) reportError = GetLastError();
    } catch (const std::exception& error) {
        reportError = GetLastError();
        append_crash_log(log, error.what(), std::strlen(error.what()));
    }
    const int resultLength = std::snprintf(text.data(), text.size(),
        "\nJFC_PORTABLE_CRASH_CAPTURE dump_written=%u dump_error=%lu report_written=%u report_error=%lu\n",
        unsigned(dumpWritten), dumpError, unsigned(reportWritten), reportError);
    append_crash_log(log, text.data(), size_t(resultLength));
}

struct MonitorStatistics {
    std::array<uint64_t, 10> counts{};
    uint64_t handlingTicks = 0, firstChance = 0, secondChance = 0;
    LARGE_INTEGER frequency{}, started{}, previous{};
    uint64_t previousCpu = 0;
    explicit MonitorStatistics() {
        check(QueryPerformanceFrequency(&frequency), "Cannot read diagnostic clock frequency");
        check(QueryPerformanceCounter(&started), "Cannot read diagnostic clock");
        previous = started;
    }
    void report(HANDLE process, HANDLE log, bool final) noexcept {
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) return;
        const double interval = double(now.QuadPart - previous.QuadPart) / frequency.QuadPart;
        if (!final && interval < 2.0) return;
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool cpuValid = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
        const uint64_t cpu = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
            ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        std::array<char, 768> line{};
        const int length = std::snprintf(line.data(), line.size(),
            "JFC_MONITOR elapsed_s=%.3f interval_s=%.3f final=%u child_cpu_valid=%u child_cpu_cores=%.3f event_counts=cumulative"
            " exception_events=%llu first_chance=%llu second_chance=%llu thread_creates=%llu"
            " dll_loads=%llu debug_strings=%llu handling_ms=%.3f\n",
            double(now.QuadPart - started.QuadPart) / frequency.QuadPart, interval, unsigned(final), unsigned(cpuValid),
            cpuValid && interval > 0 && cpu >= previousCpu ? double(cpu - previousCpu) / (10000000.0 * interval) : 0.0,
            static_cast<unsigned long long>(counts[EXCEPTION_DEBUG_EVENT]),
            static_cast<unsigned long long>(firstChance), static_cast<unsigned long long>(secondChance),
            static_cast<unsigned long long>(counts[CREATE_THREAD_DEBUG_EVENT]),
            static_cast<unsigned long long>(counts[LOAD_DLL_DEBUG_EVENT]),
            static_cast<unsigned long long>(counts[OUTPUT_DEBUG_STRING_EVENT]),
            1000.0 * double(handlingTicks) / frequency.QuadPart);
        previous = now;
        if (cpuValid) previousCpu = cpu;
        if (length <= 0 || size_t(length) >= line.size()) return;
        DWORD written = 0;
        if (!WriteFile(log, line.data(), DWORD(length), &written, nullptr) || written != DWORD(length))
            std::fprintf(stderr, "Portable monitor summary write failed (Windows error %lu)\n", GetLastError());
    }
};
DWORD wait_for_game(HANDLE process, HANDLE log, const fs::path& data, bool capture) {
    if (!capture) {
        check(WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0, "Cannot wait for game exit");
        DWORD status = 0;
        check(GetExitCodeProcess(process, &status), "Cannot read game exit status");
        return status;
    }
    constexpr char armed[] = "JFC_CRASH_MONITOR armed=1 owner=portable-parent mode=fatal-exception\n";
    append_crash_log(log, armed, sizeof(armed) - 1);
    bool initialBreakpoint = true, captured = false;
    MonitorStatistics statistics;
    for (;;) {
        DEBUG_EVENT event{};
        if (!WaitForDebugEventEx(&event, 2000)) {
            check(GetLastError() == ERROR_SEM_TIMEOUT, "Cannot wait for game diagnostic event");
            statistics.report(process, log, false);
            continue;
        }
        LARGE_INTEGER handlingStart{}, handlingEnd{};
        QueryPerformanceCounter(&handlingStart);
        if (event.dwDebugEventCode < statistics.counts.size()) ++statistics.counts[event.dwDebugEventCode];
        DWORD continuation = DBG_CONTINUE;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)
            CloseHandle(event.u.CreateProcessInfo.hFile);
        if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
            CloseHandle(event.u.LoadDll.hFile);

        if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const auto& exception = event.u.Exception;
            if (exception.dwFirstChance) ++statistics.firstChance;
            else ++statistics.secondChance;
            if (initialBreakpoint && exception.dwFirstChance && exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                initialBreakpoint = false;
            }
            else {
                continuation = DBG_EXCEPTION_NOT_HANDLED;
                if (!exception.dwFirstChance && !captured) {
                    captured = true;
                    capture_crash(process, log, data, event);
                }
            }
        }
        const bool exited = event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT;
        const DWORD status = exited ? event.u.ExitProcess.dwExitCode : 0;
        check(ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation), "Cannot continue game diagnostic event");
        QueryPerformanceCounter(&handlingEnd);
        if (handlingEnd.QuadPart >= handlingStart.QuadPart)
            statistics.handlingTicks += uint64_t(handlingEnd.QuadPart - handlingStart.QuadPart);
        statistics.report(process, log, exited);
        if (exited) {
            check(WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0, "Cannot finalize game exit");
            return status;
        }
    }
}
DWORD run_game(const fs::path& runtime, const fs::path& data) {
    wchar_t capturePolicy[2]{};
    const bool capture = GetEnvironmentVariableW(L"JFC_CRASH_CAPTURE", capturePolicy, 2) == 1 && capturePolicy[0] == L'1';

    SetLastError(ERROR_SUCCESS);
    const DWORD heapPolicyLength = GetEnvironmentVariableW(L"_NO_DEBUG_HEAP", nullptr, 0);
    const DWORD heapPolicyError = GetLastError();
    const bool defaultHeapPolicy = capture && heapPolicyLength == 0 && heapPolicyError == ERROR_ENVVAR_NOT_FOUND;
    if (defaultHeapPolicy) {
        check(SetEnvironmentVariableW(L"_NO_DEBUG_HEAP", L"1"), "Cannot preserve ordinary game heap policy");
    }
    else if (heapPolicyLength == 0 && heapPolicyError != ERROR_SUCCESS && heapPolicyError != ERROR_ENVVAR_NOT_FOUND) {
        SetLastError(heapPolicyError);
        check(FALSE, "Cannot inspect game heap policy");
    }
    check(SetEnvironmentVariableW(L"APP_FOLDER_PATH", data.c_str()), "Cannot set portable data root");
    check(SetEnvironmentVariableW(L"JFC_PORTABLE_ARTIFACT", executable_path().c_str()), "Cannot identify portable artifact");
    check(SetEnvironmentVariableW(L"TEMP", (runtime / L"temp").c_str()), "Cannot set portable temporary root");
    check(SetEnvironmentVariableW(L"TMP", (runtime / L"temp").c_str()), "Cannot set portable temporary root");
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) check(FALSE, "Cannot read portable command line");
    const auto game = runtime / L"JetForceCapricorn.exe";
    std::wstring command = quote(game.native());
    for (int i = 1; i < argc; ++i) command += L" " + quote(argv[i]);
    LocalFree(argv);
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle log(CreateFileW((data / L"game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (log.get() == INVALID_HANDLE_VALUE) check(FALSE, "Cannot create portable game log");
    BY_HANDLE_FILE_INFORMATION log_info{};
    check(GetFileInformationByHandle(log.get(), &log_info), "Cannot inspect portable game log");
    if ((log_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || log_info.nNumberOfLinks != 1)
        fail("Portable game log cannot be a link");
    check(SetEndOfFile(log.get()), "Cannot reset portable game log");
    if (capture) {
        const char* heapPolicyLog = defaultHeapPolicy ?
            "JFC_LAUNCH_HEAP policy=release_default fatal_capture=enabled\n" :
            "JFC_LAUNCH_HEAP policy=inherited fatal_capture=enabled\n";
        append_crash_log(log.get(), heapPolicyLog, std::strlen(heapPolicyLog));
    }
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) check(FALSE, "Cannot open game input");
    Handle job(CreateJobObjectW(nullptr, nullptr));
    check(job.get() != nullptr, "Cannot create portable game lifetime owner");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Cannot configure game lifetime");
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attributes(attribute_bytes);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes), "Cannot initialize game handle list");
    HANDLE inherited[] = {log.get(), input.get()};
    BOOL updated = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr);
    if (!updated) { DeleteProcThreadAttributeList(startup.lpAttributeList); check(FALSE, "Cannot restrict game handles"); }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = log.get(); startup.StartupInfo.hStdError = log.get(); startup.StartupInfo.hStdInput = input.get();
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | (capture ? DEBUG_ONLY_THIS_PROCESS : 0),
        nullptr, nullptr, &startup.StartupInfo, &process);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) { SetLastError(error); check(FALSE, "Cannot launch embedded game"); }
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.get(), child.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
        DWORD original = GetLastError();
        check(TerminateProcess(child.get(), 1), "Cannot stop game after failed start");

        wait_for_game(child.get(), log.get(), data, capture);
        SetLastError(original); check(FALSE, "Cannot start owned game process");
    }
    const DWORD status = wait_for_game(child.get(), log.get(), data, capture);
    char terminal[80];
    const int length = std::snprintf(terminal, sizeof(terminal), "\nJFC_PORTABLE_EXIT status=0x%08lX\n", status);
    LARGE_INTEGER end{};
    check(SetFilePointerEx(log.get(), end, nullptr, FILE_END), "Cannot seek game exit log");
    DWORD written = 0;
    check(WriteFile(log.get(), terminal, DWORD(length), &written, nullptr) && written == DWORD(length),
        "Cannot write game exit status");
    check(FlushFileBuffers(log.get()), "Cannot flush game exit status");
    return status;
}
void report(const char* error) {
    std::string message = std::string("Jet Force Capricorn portable: ") + error + "\n";
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    if (output && output != INVALID_HANDLE_VALUE && WriteFile(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) return;
    const int count = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), wide.data(), count);
    MessageBoxW(nullptr, wide.c_str(), L"Jet Force Capricorn", MB_OK | MB_ICONERROR);
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        auto payload = resource(101), expected = resource(102);
        auto hash = digest(payload);
        if (expected.size() != hash.size() || !std::equal(hash.begin(), hash.end(), expected.begin())) fail("Embedded runtime SHA-256 mismatch");
        auto entries = parse(payload);
        const auto data = executable_path().parent_path() / L"data";
        auto data_pin = directory(data, true);
        Handle lock(CreateFileW((data / L"portable.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (lock.get() == INVALID_HANDLE_VALUE) check(FALSE, "Portable game already running or data directory not writable");
        const auto runtime = data / L"portable-runtime";

        remove_runtime(runtime);
        DWORD status = 1;
        try { extract(entries, runtime); status = run_game(runtime, data); }
        catch (...) { try { remove_runtime(runtime); } catch (const std::exception& e) { report(e.what()); } throw; }
        remove_runtime(runtime);
        if (status != 0) {
            char code[32];
            std::snprintf(code, sizeof(code), "0x%08lX", status);
            const auto path = (data / L"game.log").u8string();
            const std::string message = "Game exited with status " + std::string(code) +
                ". Details are in:\n" + std::string(path.begin(), path.end());
            report(message.c_str());
        }
        return static_cast<int>(status);
    } catch (const std::exception& error) { report(error.what()); return 1; }
}
