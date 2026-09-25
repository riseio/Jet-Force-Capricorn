#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <compressapi.h>
#include <bcrypt.h>
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
DWORD run_game(const fs::path& runtime, const fs::path& data) {
    check(SetEnvironmentVariableW(L"APP_FOLDER_PATH", data.c_str()), "Cannot set portable data root");
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
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) { SetLastError(error); check(FALSE, "Cannot launch embedded game"); }
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job.get(), child.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
        DWORD original = GetLastError(); TerminateProcess(child.get(), 1); WaitForSingleObject(child.get(), INFINITE);
        SetLastError(original); check(FALSE, "Cannot start owned game process");
    }
    if (WaitForSingleObject(child.get(), INFINITE) != WAIT_OBJECT_0) check(FALSE, "Cannot wait for game exit");
    DWORD status = 1;
    check(GetExitCodeProcess(child.get(), &status), "Cannot read game exit status");
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
