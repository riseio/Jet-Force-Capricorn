#include "local_aot.hpp"
#include "local_aot_recipe.hpp"
#include "local_aot_process.hpp"
#include "local_aot_image.hpp"
#include "native_rom_data.hpp"
#include "scalar_shader.hpp"
#include "common/rt64_profiler_sha256.h"
#include "local_aot_metadata.hpp"
#include "librecomp/game.hpp"
#include "recompui/recompui.h"
#include <miniz/miniz.h>
#include <SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <set>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include "ultramodern/state.hpp"
#include "ultramodern/renderer_context.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
extern "C" const unsigned char _binary_bundle_zip_start[], _binary_bundle_zip_end[];
#endif

namespace jfc::local_aot {
namespace fs = std::filesystem;
using Sha256 = RT64::ProfilerDetail::Sha256;
void register_loaded_overlays();
std::string digest(std::span<const uint8_t> bytes) { Sha256 hash; hash.update(bytes.data(),bytes.size()); return hash.hex(); }
extern JfcAotModule loaded;
JfcAotHost host_api();
namespace {
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Native code setup cancelled") {}
};
struct Progress {
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> finished = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_uint64_t> prompt = std::make_shared<std::atomic_uint64_t>(0);
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now(), last{};
    std::string stage;
    uint32_t completed = 0, total = 0;
    Progress() {
        recompui::queue_ui_action([cancel = cancelled, done = finished, owned = prompt] {
            if (done->load()) return;
            recompui::open_info_prompt("Preparing native game code",
                "Preparing your validated ROM locally. This one-time step can take several minutes. No download is needed.",
                "Cancel", [cancel, done, owned] {
                    owned->store(0);
                    if (!done->load()) {
                        cancel->store(true);
                        recompui::open_notification("Cancelling native code setup",
                            "Stopping the compiler and removing temporary files. The launcher will return when cleanup finishes.");
                        owned->store(recompui::get_prompt_generation());
                    }
                }, recompui::ButtonStyle::Secondary);
            owned->store(recompui::get_prompt_generation());
        });
    }
    ~Progress() {
        finished->store(true);
        recompui::queue_ui_action([owned = prompt] {
            if (const auto generation = owned->exchange(0)) recompui::close_prompt(generation);
        });
    }
    void check() const {
        if (cancelled->load() || ultramodern::shutdown_token().stop_requested()) throw Cancelled{};
    }
    void pulse(bool force = false, bool cancellable = true) {
        if (cancellable) check();
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last < std::chrono::seconds(1)) return;
        last = now;
        auto text = stage;
        if (total) text += ": " + std::to_string(completed) + " / " + std::to_string(total);
        text += "\n" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now-started).count()) +
            " seconds elapsed. First setup runs offline; later launches reuse the native module.";
        recompui::queue_ui_action([text = std::move(text), cancel = cancelled, done = finished, owned = prompt] {
            if (!cancel->load() && !done->load()) recompui::update_prompt_text(text, owned->load());
        });
    }
    void report(std::string next, uint32_t count = 0, uint32_t size = 0, bool cancellable = true) {
        stage = std::move(next); completed = count; total = size; pulse(true, cancellable);

        std::fflush(stderr);
    }
};
#ifdef _WIN32
constexpr auto library_name = "game.dll";
constexpr auto tool_suffix = ".exe";
#else
constexpr auto library_name = "game.so";
constexpr auto tool_suffix = "";
#endif
void regular(const fs::path& path) {
    if (fs::is_symlink(fs::symlink_status(path))) throw std::runtime_error("Local compilation cache contains a symbolic link");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Local compilation cache contains a reparse point");
#endif
}
void directory(const fs::path& path) {
    regular(path);
    fs::create_directories(path);
}
void clear_tree(const fs::path& path, Progress& progress, bool cancellable = true) {
    regular(path);
    if (!fs::exists(path)) return;
    if (!fs::is_directory(path)) throw std::runtime_error("Invalid local compilation staging directory");
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(path), end; it != end; ++it) {
        const auto entry = it->path();
        bool link = fs::is_symlink(fs::symlink_status(entry));
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(entry.c_str());
        link |= attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#endif

        if (link) it.disable_recursion_pending();
        paths.push_back(entry);
        progress.pulse(false, cancellable);
    }

    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        fs::remove(*it);
        progress.pulse(false, cancellable);
    }
    fs::remove(path);
}
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    std::string result(reinterpret_cast<const char*>(value.data()), value.size());
#ifdef _WIN32

    if (result.starts_with("\\\\?\\UNC\\")) return "\\\\" + result.substr(8);
    if (result.starts_with("\\\\?\\")) return result.substr(4);
#endif
    return result;
}
fs::path cache_root(const fs::path& data_root) {
    auto base = fs::canonical(data_root).make_preferred();
#ifdef _WIN32

    auto native = base.native();
    if (!native.starts_with(L"\\\\?\\")) {
        native = native.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + native.substr(2) : L"\\\\?\\" + native;
    }
    base = fs::path(native);
#endif
    return base / "cache" / "local-aot" / kKitIdentity;
}
void publish(const fs::path& source, const fs::path& destination) {
    regular(source); regular(destination);
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish local compilation cache");
#else
    fs::rename(source, destination);
#endif
}
struct Lock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Lock(const fs::path& file) {
        regular(file);
        handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Another Capricorn instance is preparing this installation");
    }
    ~Lock() { CloseHandle(handle); }
#else
    int handle = -1;
    explicit Lock(const fs::path& file) {
        handle = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (handle < 0) throw std::runtime_error("Cannot open local compilation lock");
        if (flock(handle, LOCK_EX | LOCK_NB)) { close(handle); handle = -1;
            throw std::runtime_error("Another Capricorn instance is preparing this installation"); }
    }
    ~Lock() { close(handle); }
#endif
};

std::string checksum(const fs::path& file) {
    regular(file);
    if (!fs::is_regular_file(file) || fs::file_size(file) > 256ull * 1024 * 1024)
        throw std::runtime_error("Native module has an invalid file type or size");
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read locally compiled module");
    std::array<char, 65536> block;
    Sha256 hash;
    uint64_t size = 0;
    while (stream.read(block.data(), block.size()) || stream.gcount()) {
        hash.update(block.data(),size_t(stream.gcount()));
        size += stream.gcount();
        if (size > 256ull * 1024 * 1024) throw std::runtime_error("Native module exceeds the cache size limit");
    }
    if (!stream.eof()) throw std::runtime_error("Cannot verify locally compiled module");
    return std::string(kKitIdentity) + ":" + std::to_string(size) + ":" + hash.hex();
}
bool load(const fs::path& file, bool install = true) {
    regular(file);
#ifdef _WIN32
    HMODULE handle = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle) return false;
    auto initialize = reinterpret_cast<JfcAotInitialize>(GetProcAddress(handle, "jfc_local_module_init"));
#else
    void* handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) { std::fprintf(stderr, "Local module: %s\n", dlerror()); return false; }
    auto initialize = reinterpret_cast<JfcAotInitialize>(dlsym(handle, "jfc_local_module_init"));
#endif
    static const auto host = host_api();
    JfcAotModule candidate{};
    const bool valid = initialize && initialize(&host, &candidate) && candidate.functions &&
        candidate.function_count == kGameFunctionCount && candidate.sections && candidate.audio &&
        candidate.section_count == kSectionCount && candidate.total_sections == kTotalSections &&
        candidate.overlays && candidate.overlay_count == kOverlayCount &&
        candidate.effect_spirv && candidate.effect_spirv_size >= 20 && candidate.effect_spirv_size <= 1024*1024 &&
#ifdef _WIN32
        candidate.effect_dxil && candidate.effect_dxil_size >= 20 && candidate.effect_dxil_size <= 1024*1024;
#else
        !candidate.effect_dxil && candidate.effect_dxil_size == 0;
#endif
    if (valid && install) {

        loaded = candidate;
        return true;
    }
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return valid;
}
void extract(const fs::path& destination, Progress& progress) {
    const void* bytes;
    size_t size;
#ifdef _WIN32
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(104), MAKEINTRESOURCEW(10));
    const auto memory = resource ? LoadResource(nullptr, resource) : nullptr;
    bytes = memory ? LockResource(memory) : nullptr;
    size = resource ? SizeofResource(nullptr, resource) : 0;
#else
    bytes = _binary_bundle_zip_start;
    size = _binary_bundle_zip_end - _binary_bundle_zip_start;
#endif
    mz_zip_archive archive{};
    if (bytes && digest({static_cast<const uint8_t*>(bytes), size}) != kKitIdentity)
        throw std::runtime_error("The offline compilation kit checksum does not match this build");
    if (!bytes || !size || !mz_zip_reader_init_mem(&archive, bytes, size, 0))
        throw std::runtime_error("The offline compilation kit is missing or damaged");
    struct End { mz_zip_archive* archive; ~End() { mz_zip_reader_end(archive); } } end{&archive};
    const auto count = mz_zip_reader_get_num_files(&archive);
    if (count > 100000) throw std::runtime_error("Invalid compilation kit inventory");
    uint64_t total = 0;
    std::set<std::string> names;
    for (mz_uint i = 0; i < count; ++i) {
        progress.check();
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) throw std::runtime_error("Invalid compilation kit entry");
        fs::path relative = fs::u8path(stat.m_filename).make_preferred();
        if (relative.empty() || relative.is_absolute() || relative.has_root_path() ||
            relative.generic_string().find(':') != std::string::npos)
            throw std::runtime_error("Unsafe compilation kit path");
        auto key = relative.generic_string();
        for (auto& c : key) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
        if (!names.insert(key).second) throw std::runtime_error("Duplicate compilation kit path");
        for (const auto& part : relative) if (part == ".." || part == ".") throw std::runtime_error("Unsafe compilation kit path");
        if (((stat.m_external_attr >> 16) & 0170000) == 0120000)
            throw std::runtime_error("Compilation kit must not contain symbolic links");
        if (stat.m_uncomp_size > 2ull * 1024 * 1024 * 1024 - total)
            throw std::runtime_error("Compilation kit entry is too large");
        total += stat.m_uncomp_size;
        if (total > 2ull * 1024 * 1024 * 1024) throw std::runtime_error("Compilation kit is too large");
        const auto path = destination / relative;
        directory(path.parent_path());
        regular(path);
        if (mz_zip_reader_is_file_a_directory(&archive, i)) { directory(path); continue; }
        size_t length = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &length, 0);
        if (!data) throw std::runtime_error("Cannot unpack offline compilation kit");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(static_cast<const char*>(data), length);
        mz_free(data);
        if (!stream) throw std::runtime_error("Cannot write offline compilation kit: " + utf8(path));
#ifndef _WIN32
        if (relative == "compiler/zig" || relative == "shader/bin/dxc" || relative.begin()->string() == "generators")
            fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
#endif
        if (i % 100 == 0) progress.report("Preparing offline tools", i, count);
    }
}
#ifdef _WIN32
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}
#endif
bool tool_environment(std::string_view name) {
    for (const auto* key : {"PATH", "ZIG_GLOBAL_CACHE_DIR", "ZIG_LOCAL_CACHE_DIR", "ZIG_LIB_DIR",
            "TMP", "TEMP", "TMPDIR", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
            "LIBRARY_PATH", "COMPILER_PATH", "LD_PRELOAD", "LD_AUDIT", "LD_LIBRARY_PATH"})
        if (name == key) return true;
    return false;
}
void execute(std::vector<std::string> args, const fs::path& working, Progress& progress) {
    progress.check();
    directory(working / "temporary");
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quote(fs::u8path(arg).wstring()); }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = CreateFileW((working / "compiler.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (log == INVALID_HANDLE_VALUE || !job) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (job) CloseHandle(job);
        throw std::runtime_error("Cannot prepare compiler process");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job); CloseHandle(log); throw std::runtime_error("Cannot contain compiler process");
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};

    LPWCH inherited = GetEnvironmentStringsW();
    std::wstring environment;
    if (!inherited) { CloseHandle(job); CloseHandle(log); throw std::runtime_error("Cannot read compiler environment"); }
    for (const wchar_t* item = inherited; *item; item += wcslen(item) + 1) {
        std::string key;
        for (const wchar_t* letter = item; *letter && *letter != L'='; ++letter)
            key += *letter >= L'a' && *letter <= L'z' ? char(*letter - L'a' + L'A') : char(*letter);
        if (!tool_environment(key)) {
            environment.append(item); environment += L'\0';
        }
    }
    FreeEnvironmentStringsW(inherited);
    const auto compiler_cache_path = [](const fs::path& path) {
        auto value = path.wstring();

        if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
        if (value.starts_with(L"\\\\?\\")) return value.substr(4);
        return value;
    };
    environment += L"ZIG_GLOBAL_CACHE_DIR=" + compiler_cache_path(working / "zig-global"); environment += L'\0';
    environment += L"ZIG_LOCAL_CACHE_DIR=" + compiler_cache_path(working / "zig-local"); environment += L'\0';
    environment += L"PATH="; environment += L'\0';
    for (const auto* key : {L"TMP=", L"TEMP="}) {
        environment += key + compiler_cache_path(working / "temporary"); environment += L'\0';
    }
    environment += L'\0';
    const auto application = fs::u8path(args[0]).wstring();
    auto process_directory = working.wstring();
    if (process_directory.size() >= MAX_PATH - 2) {
        const auto required = GetShortPathNameW(working.c_str(), nullptr, 0);
        if (required) {
            std::wstring shortened(required, L'\0');
            const auto written = GetShortPathNameW(working.c_str(), shortened.data(), required);
            if (written && written < required) { shortened.resize(written); process_directory = std::move(shortened); }
        }
    }
    bool started = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), process_directory.c_str(), &startup, &process);
    const auto launch_error = started ? ERROR_SUCCESS : GetLastError();
    CloseHandle(log);
    if (!started) { CloseHandle(job); throw std::runtime_error("Cannot launch offline compiler (Windows error " + std::to_string(launch_error) + ")"); }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job);
        throw std::runtime_error("Cannot contain offline compiler");
    }
    ResumeThread(process.hThread); CloseHandle(process.hThread);
    try {
        for (;;) {
            const auto status = WaitForSingleObject(process.hProcess, 50);
            if (status == WAIT_OBJECT_0) break;
            if (status != WAIT_TIMEOUT) throw std::runtime_error("Cannot wait for offline compiler");
            progress.pulse();
        }
    } catch (...) {
        CloseHandle(job);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hProcess);
        throw;
    }
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(job);
#else

    if (args[0].find("/generators/") != std::string::npos || args[0].find("/shader/bin/") != std::string::npos) {
        const char* loader = std::getenv("JFC_AOT_LOADER");
        const char* libraries = std::getenv("JFC_AOT_LIBRARY_PATH");
        if (loader && libraries) args.insert(args.begin(), {loader, "--library-path", libraries});
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
        const std::string_view value(*entry);
        if (!tool_environment(value.substr(0, value.find('=')))) environment.emplace_back(*entry);
    }
    environment.push_back("ZIG_GLOBAL_CACHE_DIR=" + (working / "zig-global").string());
    environment.push_back("ZIG_LOCAL_CACHE_DIR=" + (working / "zig-local").string());
    environment.push_back("PATH=");
    environment.push_back("TMPDIR=" + (working / "temporary").string());
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    const int log = open((working / "compiler.log").c_str(), O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (log < 0) throw std::runtime_error("Cannot open offline compiler output");
    const pid_t child = spawn_owned_process(argv.data(), envp.data(), working.c_str(), log);
    const auto launch_error = errno;
    close(log);
    if (child < 0) throw std::runtime_error("Cannot contain offline compiler (Linux error " + std::to_string(launch_error) + ")");
    int status = 0;
    try {
        for (;;) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) break;
            if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot wait for offline compiler");
            progress.pulse();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } catch (...) {
        kill(-child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        throw;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    if (code) throw std::runtime_error("Offline tool " + fs::u8path(args[0]).filename().string() +
        " exited with status " + std::to_string(code));
}
void compile(const fs::path& tools, const fs::path& working, std::span<const uint8_t> rom, Progress& progress) {
    for (const auto* name : {"cpu.toml", "rsp.toml", "relocatable_sections.txt", "imports.c", "module.cpp"})
        fs::copy_file(tools / name, working / name, fs::copy_options::overwrite_existing);
    progress.report("Preparing local framebuffer shaders");
    const auto shader_template = tools / "shader" / "CpuFramebufferEffectsPS.hlsl";
    regular(shader_template);
    if (fs::file_size(shader_template) > 65536) throw std::runtime_error("Invalid shader template size");
    std::ifstream shader_input(shader_template, std::ios::binary);
    const std::string template_text{std::istreambuf_iterator<char>(shader_input), {}};
    const auto shader_source = jfc::shader::cpu_effect_source(rom, template_text);
    {
        std::ofstream output(working / "effects.hlsl", std::ios::binary);
        output.write(shader_source.data(), shader_source.size());
        output.close();
        if (!output) throw std::runtime_error("Cannot stage local framebuffer shader");
    }
    std::ofstream shader_header(working / "local_shader.h", std::ios::binary);
    for (const auto* format : {"spirv", "dxil"}) {
#ifndef _WIN32
        if (std::string_view(format) == "dxil") continue;
#endif
        const auto output_path = working / (std::string("effects.")+format);
        std::vector<std::string> command{utf8(tools / "shader" / "bin" / (std::string("dxc")+tool_suffix)),
            "-E", "PSMain", "-T", "ps_6_3", "-I", utf8(tools / "shader"),
            "-Fo", utf8(output_path), utf8(working / "effects.hlsl")};
        if (std::string_view(format) == "spirv")
            command.insert(command.end(), {"-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout"});
        execute(std::move(command), working, progress);
        const auto size = fs::file_size(output_path);
        if (size < 20 || size > 1024*1024) throw std::runtime_error("Invalid local shader output size");
        std::ifstream input(output_path, std::ios::binary);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("Cannot read local shader output");
        shader_header << "alignas(4) static const unsigned char effect_" << format << "[]={";
        for (auto value : bytes) shader_header << unsigned(value) << ',';
        shader_header << "};\n";
    }
    shader_header.close();
    if (!shader_header) throw std::runtime_error("Cannot stage local shader module data");
    {
        if (rom.empty()) throw std::runtime_error("Validated game image is unavailable");
        std::ofstream stream(working / "input.z64", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(rom.data()), rom.size());
        if (!stream) throw std::runtime_error("Cannot stage validated input for local compilation");
    }
    progress.report("Preparing local overlay metadata");
    const auto recipe_file=tools / "image.recipe";
    regular(recipe_file);
    const auto recipe_size=fs::file_size(recipe_file);
    if (recipe_size>20*1024*1024) throw std::runtime_error("Invalid local image recipe size");
    std::vector<uint8_t> image_recipe(static_cast<size_t>(recipe_size));
    std::ifstream recipe_input(recipe_file,std::ios::binary);
    if (!recipe_input.read(reinterpret_cast<char*>(image_recipe.data()),image_recipe.size()))
        throw std::runtime_error("Cannot read local overlay metadata");
    const auto analysis_image=reconstruct_image(image_recipe,rom);
    {
        std::ofstream output(working / "jfg.us.elf",std::ios::binary);
        output.write(reinterpret_cast<const char*>(analysis_image.data()),analysis_image.size());
        output.close();
        if (!output) throw std::runtime_error("Cannot stage local analysis image");
    }
    progress.report("Generating local game code");
    execute({utf8(tools / "generators" / (std::string("N64Recomp") + tool_suffix)), "./cpu.toml"}, working, progress);
    execute({utf8(tools / "generators" / (std::string("RSPRecomp") + tool_suffix)), "./rsp.toml"}, working, progress);
    fs::remove(working / "input.z64");
    fs::remove(working / "jfg.us.elf");
    std::vector<fs::path> sources;
    for (const auto& item : fs::directory_iterator(working / "cpu"))
        if (item.path().filename().string().starts_with("funcs_") && item.path().extension() == ".c") sources.push_back(item.path());
    std::sort(sources.begin(), sources.end());
    if (sources.empty()) throw std::runtime_error("Recompiler produced no local game code");
    sources.push_back(working / "imports.c");
    sources.push_back(working / "rsp.cpp");
    sources.push_back(working / "module.cpp");
    const auto compiler = utf8(tools / "compiler" / (std::string("zig") + tool_suffix));
    std::vector<std::string> objects;
    for (size_t index = 0; index < sources.size(); ++index) {
        progress.report("Compiling native game code", static_cast<uint32_t>(index), static_cast<uint32_t>(sources.size()));
        const auto object = utf8(working / ("part-" + std::to_string(index) + ".o"));
        std::vector<std::string> args{compiler, sources[index].extension() == ".c" ? "cc" : "c++",
            "-target", recipe::target, "-I", utf8(tools / "include"), "-I", utf8(working / "cpu"),
            sources[index].extension() == ".c" ? recipe::c_standard : recipe::cpp_standard,
            "-c", utf8(sources[index]), "-o", object};
        args.insert(args.end(), recipe::compile_flags.begin(), recipe::compile_flags.end());
        execute(std::move(args), working, progress);
        objects.push_back(object);
    }
    progress.report("Linking native game code");
    std::vector<std::string> link{compiler, "c++", "-target", recipe::target, "-o", utf8(working / library_name)};
    link.insert(link.end(), recipe::link_flags.begin(), recipe::link_flags.end());
#ifndef _WIN32
    link.push_back(recipe::no_undefined);
#endif
    link.insert(link.end(), objects.begin(), objects.end());
    execute(std::move(link), working, progress);
}
}
const JfcAotModule& module() {
    if (!loaded.functions) throw std::logic_error("Local game module has not been prepared");
    return loaded;
}
bool prepare() {
    if (!prepare(recomp::get_rom())) return false;
    try {
        ultramodern::state::VideoPause pause;
        ultramodern::state::run_renderer_operation(+[](ultramodern::renderer::RendererContext& renderer, void*) {
            renderer.install_cpu_framebuffer_shader(
                {loaded.effect_spirv, loaded.effect_spirv_size}, {loaded.effect_dxil, loaded.effect_dxil_size});
        }, nullptr);
        return true;
    } catch (const std::exception& error) {
        const std::string message = std::string("Cannot prepare native framebuffer rendering: ")+error.what();
        std::fprintf(stderr, "%s\n", message.c_str());
        recompui::queue_ui_action([message] {
            recompui::open_info_prompt("Native shader setup failed", message, "OK", [] {});
        });
        return false;
    }
}
bool prepare(std::span<const uint8_t> rom) {
    static std::mutex preparation_mutex;
    static bool prepared = false;
    std::lock_guard lock(preparation_mutex);
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    };
    const auto data_root = recomp::get_config_path();
    try {
        if (rom.size() != 33554432 || digest(rom) !=
                "159dde164c475976a3e527fbb20978431d4765f2c63019b3530c3aa8772595aa")
            throw std::runtime_error("The ROM does not match Jet Force Gemini North America revision 0");
        if (prepared) return true;
        const auto finish = [&] {
            register_loaded_overlays();
            prepared = true;
        };
        if (loaded.functions) { finish(); return true; }
        jfc::native_data::initialize(rom);
        const auto root = cache_root(data_root);

        fs::path cursor;
        for (const auto& part : root) { cursor /= part; regular(cursor); }
        directory(root);
        Lock lock(root / "install.lock");
        const auto binary = root / library_name;
        const auto receipt = root / "receipt.txt";
        const auto tools = root / "tools";
        const auto stage = root / "pending";

        if (fs::exists(stage) || fs::exists(tools)) {
            Progress cleanup;
            cleanup.report("Removing interrupted setup files");
            clear_tree(stage, cleanup);
            clear_tree(tools, cleanup);
        }
        regular(receipt);
        regular(binary);
        if (fs::exists(binary) && fs::exists(receipt)) {
            if (fs::is_regular_file(binary) && fs::file_size(binary) <= 256ull * 1024 * 1024 &&
                    fs::is_regular_file(receipt) && fs::file_size(receipt) <= 256) {
                std::ifstream stream(receipt);
                std::string expected; std::getline(stream, expected);
                if (stream.peek() == std::char_traits<char>::eof() && expected == checksum(binary) && load(binary)) {
                    finish();

                    std::fflush(stderr);
                    return true;
                }
            }
        }
        {
            Progress progress;
            for (const auto& path : {binary, receipt}) {
                regular(path);
                if (fs::exists(path) && !fs::is_regular_file(path))
                    throw std::runtime_error("The local module cache contains an unexpected file type");
            }
            regular(root / "compiler.log");
            try {
                directory(tools);
                directory(stage);
                progress.report("Preparing offline tools");
                extract(tools, progress);
                compile(tools, stage, rom, progress);
                progress.check();
                const auto identity = checksum(stage / library_name);
                if (!load(stage / library_name, false)) throw std::runtime_error("Locally compiled module failed compatibility validation");

                publish(stage / library_name, binary);
                std::ofstream output(stage / "receipt.txt", std::ios::trunc); output << identity << '\n'; output.close();
                if (!output) throw std::runtime_error("Cannot save local compilation receipt");
                publish(stage / "receipt.txt", receipt);
                progress.report("Removing temporary setup files",0,0,false);
                clear_tree(stage, progress,false);
                clear_tree(tools, progress,false);
                if (!load(binary)) throw std::runtime_error("Locally compiled module failed compatibility validation");

                std::fflush(stderr);
            } catch (...) {
                const auto failure=std::current_exception();
                std::string details;
                bool cancelled=false;
                try { std::rethrow_exception(failure); }
                catch(const Cancelled&) { cancelled=true; }
                catch(const std::exception& error) { details=error.what(); }
                catch(...) { details="Unknown native setup failure"; }
                if(!cancelled) {
                    std::ifstream log(stage / "compiler.log",std::ios::binary|std::ios::ate);
                    if(log) {
                        const auto size=log.tellg();
                        if(size>0) {
                            const auto length=std::min<std::streamoff>(size,4096);
                            log.seekg(size-length);
                            std::string tail(static_cast<size_t>(length),'\0');
                            if(log.read(tail.data(),length)) details+="\n\n"+tail;
                        }
                    }
                }
                try {
                    progress.report("Removing temporary setup files", 0, 0, false);
                    clear_tree(stage, progress, false);
                    clear_tree(tools, progress, false);
                } catch (const std::exception& cleanup_error) {
                    throw std::runtime_error(details+"\nTemporary setup cleanup failed: "+cleanup_error.what()+
                        "\nCheck free space and write access, then restart to retry cleanup.");
                }
                if(cancelled) std::rethrow_exception(failure);
                throw std::runtime_error(details);
            }
        }
        finish();
        return true;
    } catch (const Cancelled&) {

        std::fflush(stderr);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Local AOT: %s\n", error.what());
        std::fflush(stderr);
        const auto message = std::string("Native code setup did not finish. No existing game data was changed.\n\n") +
            error.what();
        recompui::queue_ui_action([message] {
            recompui::open_info_prompt("Native code setup failed", message, "OK", [] {});
        });
    }
    return false;
}
}
