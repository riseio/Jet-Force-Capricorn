#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio_output.hpp"
#include "audio_playback_clock.hpp"
#include "audio_state_stream.hpp"
#include "save_state_codec.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/state.hpp"
#include "common/rt64_performance_profiler.h"
#include "common/rt64_profiler_audio.h"
#include <atomic>
#include <cstdio>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <thread>

namespace {
SDL_AudioDeviceID audio_device = 0;
std::unique_ptr<jfc::audio::StateStream> audio_stream;
uint32_t audio_input_frequency = 32000, audio_output_frequency = 0;
uint32_t audio_callback_frames = 0;
bool audio_started = false;
std::atomic_bool audio_failed{false};
uint32_t audio_playback_pauses = 0;
jfc::audio::PlaybackClock playback_clock;
bool silent_playback_active = false;

uint64_t audio_profile_stream = 0, audio_profile_callback = 0;
std::mutex audio_mutex;
jfc::audio::OutputQueue audio_output;
std::atomic_bool state_audio_paused = false;
std::mutex audio_phase_mutex;
RT64::AudioProfile::Phase audio_activity = RT64::AudioProfile::Phase::Startup;
bool audio_profile_active = false, audio_transaction_paused = false;
std::mutex devices_mutex, device_wake_mutex, device_lifecycle_mutex;
std::condition_variable device_wake;
jfc::audio::OutputDevices devices;
std::atomic<uint64_t> devices_revision{1};
std::atomic_bool reopen_device{false}, test_pending{false};
std::atomic<int> test_volume{100};
std::jthread device_worker;
uint32_t test_frames = 0;

void publish_devices_locked() {
    devices.revision = devices_revision.fetch_add(1, std::memory_order_release) + 1;
}

int SDLCALL watch_audio(void*, SDL_Event* event) {
    if ((event->type == SDL_AUDIODEVICEADDED && !event->adevice.iscapture) ||
        (event->type == SDL_AUDIODEVICEREMOVED && !event->adevice.iscapture) ||
        event->type == SDL_APP_DIDENTERFOREGROUND) {
        reopen_device.store(true, std::memory_order_release);
        device_wake.notify_one();
    }
    return 1;
}

struct OpenOutput { SDL_AudioDeviceID device = 0; uint32_t frames = 0; };
OpenOutput open_output() {
    std::vector<std::string> names;
    for (int i = 0, count = SDL_GetNumAudioDevices(0); i < count; ++i)
        if (const char* name = SDL_GetAudioDeviceName(i, 0))
            if (std::find(names.begin(), names.end(), name) == names.end()) names.emplace_back(name);
    std::string selected;
    { std::lock_guard lock(devices_mutex); selected = devices.selected; }
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = 48000; desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 512;
    desired.callback = [](void*, Uint8* data, int bytes) {
        const bool profiling = RT64::PerformanceProfiler::enabled();
        const uint64_t callback = profiling ? ++audio_profile_callback : 0;
        const uint64_t missing_before = profiling ? audio_output.underruns() : 0;
        if (profiling) RT64::AudioProfile::callbackBegin(audio_profile_stream,
            callback, size_t(bytes) / (2 * sizeof(int16_t)), audio_output.size() / 2);
        auto pcm = std::span(reinterpret_cast<int16_t*>(data), size_t(bytes) / sizeof(int16_t));
        audio_output.consume(pcm);
        if (test_frames) {
            const int volume = test_volume.load(std::memory_order_relaxed);
            for (size_t i = 0; i + 1 < pcm.size() && test_frames; i += 2, --test_frames) {
                const auto elapsed = 28800 - test_frames;
                const double envelope = std::min({1.0, elapsed / 480.0, test_frames / 480.0});
                const int tone = int(std::sin(elapsed * (6.283185307179586 * 440.0 / 48000.0)) * volume * 40 * envelope);
                pcm[i] = int16_t(std::clamp(int(pcm[i]) + tone, -32768, 32767));
                pcm[i + 1] = int16_t(std::clamp(int(pcm[i + 1]) + tone, -32768, 32767));
            }
        }
        if (profiling) RT64::AudioProfile::callbackEnd(audio_profile_stream,
            callback, (audio_output.underruns() - missing_before) / 2);
    };
    const bool present = std::find(names.begin(), names.end(), selected) != names.end();
    auto opened = selected.empty() || present
        ? SDL_OpenAudioDevice(selected.empty() ? nullptr : selected.c_str(), 0, &desired, &obtained, 0) : 0;
    const bool fallback = !selected.empty() && !opened;
    std::string error;
    if (fallback) {
        error = present ? "Selected output unavailable: " + std::string(SDL_GetError()) : "The selected output is not connected.";
        opened = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    }
    if (!opened) {
        if (!error.empty()) error += " ";
        error += std::string("Audio unavailable: ") + SDL_GetError();
    }
    std::string active;
    if (opened) {
        active = selected;
        if (selected.empty() || fallback) {
            char* name = nullptr; SDL_AudioSpec spec{};
            active = SDL_GetDefaultAudioInfo(&name, &spec, 0) == 0 && name ? name : "System default";
            SDL_free(name);
        }
    }
    {
        std::lock_guard lock(devices_mutex);
        devices.names = std::move(names); devices.active = std::move(active);
        devices.error = std::move(error); devices.fallback = fallback;
        publish_devices_locked();
    }
    return {opened, obtained.samples};
}

void publish_audio_activity_locked() {
    if (audio_profile_active)
        RT64::AudioProfile::phase(audio_profile_stream,
            audio_activity == RT64::AudioProfile::Phase::Shutdown ? audio_activity :
            audio_transaction_paused ? RT64::AudioProfile::Phase::Paused : audio_activity);
}

void finish_audio_profile_locked() {
    std::lock_guard phase_lock(audio_phase_mutex);
    if (!audio_profile_active) return;
    audio_activity = RT64::AudioProfile::Phase::Shutdown;
    publish_audio_activity_locked();
    RT64::AudioProfile::stop(audio_profile_stream);
    audio_profile_active = false;
}

void start_audio_if_ready_locked() {
    if (audio_device && !audio_playback_pauses && !audio_started && audio_callback_frames &&
        audio_output.size() / 2 >= audio_callback_frames) {
        std::lock_guard phase_lock(audio_phase_mutex);
        if (audio_activity == RT64::AudioProfile::Phase::Shutdown) return;
        audio_started = true;
        SDL_PauseAudioDevice(audio_device, 0);
    }
}

void advance_silent_playback_locked(jfc::audio::PlaybackClock::Time now) {
    const uint64_t frames = playback_clock.advance(now, true);
    audio_output.discard(size_t(std::min<uint64_t>(frames, audio_output.size() / 2) * 2));
}

void pause_playback_locked() {
    if (!audio_stream)
        throw std::runtime_error("Audio output is unavailable for saving state");
    if (audio_playback_pauses == UINT32_MAX)
        throw std::overflow_error("Audio playback pause ownership overflow");
    if (audio_playback_pauses != 0) { ++audio_playback_pauses; return; }
    const auto now = jfc::audio::PlaybackClock::Clock::now();
    if (silent_playback_active) advance_silent_playback_locked(now);
    else playback_clock.advance(now, false);
    playback_clock.set_paused(now, true);
    ++audio_playback_pauses;
    if (audio_device) SDL_PauseAudioDevice(audio_device, 1);
    std::lock_guard phase_lock(audio_phase_mutex);
    audio_transaction_paused = true;
    publish_audio_activity_locked();
}

void resume_playback_locked() noexcept {
    if (audio_playback_pauses == 0) std::terminate();
    if (--audio_playback_pauses != 0) return;
    playback_clock.set_paused(jfc::audio::PlaybackClock::Clock::now(), false);
    std::lock_guard phase_lock(audio_phase_mutex);
    audio_transaction_paused = false;
    publish_audio_activity_locked();
    if (audio_device && audio_activity != RT64::AudioProfile::Phase::Shutdown &&
        (audio_started || (audio_callback_frames && audio_output.size() / 2 >= audio_callback_frames))) {
        audio_started = true;
        SDL_PauseAudioDevice(audio_device, 0);
    }
}
}

void set_audio_activity(ultramodern::AudioActivity activity) {
    using Phase = RT64::AudioProfile::Phase;
    Phase next;
    switch (activity) {
        case ultramodern::AudioActivity::Startup: next = Phase::Startup; break;
        case ultramodern::AudioActivity::Gameplay: next = Phase::Gameplay; break;
        case ultramodern::AudioActivity::Paused: next = Phase::Paused; break;
        case ultramodern::AudioActivity::Shutdown: next = Phase::Shutdown; break;
        case ultramodern::AudioActivity::Menu: next = Phase::Menu; break;
        case ultramodern::AudioActivity::Cutscene: next = Phase::Cutscene; break;
        case ultramodern::AudioActivity::Loading: next = Phase::Loading; break;
        default: throw std::runtime_error("Invalid native audio activity");
    }
    std::lock_guard phase_lock(audio_phase_mutex);
    if (audio_activity == Phase::Shutdown || audio_activity == next) return;
    audio_activity = next;
    publish_audio_activity_locked();
}

bool open_audio_locked(uint32_t frequency) {
    if (frequency == 0 ||
        frequency > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        SDL_SetError("Audio input frequency must be positive and fit a native integer");
        return false;
    }
    try {
        auto replacement = std::make_unique<jfc::audio::StateStream>(frequency, audio_output_frequency);
        if (audio_stream != nullptr) {
            jfc::audio::Samples tail;
            audio_stream->finish(tail);
            audio_output.append(tail);
            RT64::AudioProfile::produced(audio_profile_stream, 0, tail.size() / 2);
        }
        audio_stream.swap(replacement);
    } catch (const std::exception& error) {
        SDL_SetError("%s", error.what());
        audio_stream.reset();
    }
    if (audio_stream == nullptr) {
        if (audio_device) SDL_CloseAudioDevice(audio_device);
        finish_audio_profile_locked();
        audio_device = 0;
        audio_output_frequency = 0;
        audio_callback_frames = 0;
        audio_started = false;
        return false;
    }
    audio_input_frequency = frequency;
    start_audio_if_ready_locked();
    return true;
}

namespace {
void output_worker(std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    auto next_retry = Clock::now();
    while (!stop.stop_requested()) {
        {
            std::unique_lock wake(device_wake_mutex);
            device_wake.wait_for(wake, std::chrono::milliseconds(100), [&] {
                return stop.stop_requested() || reopen_device.load() || test_pending.load();
            });
        }
        if (stop.stop_requested()) break;
        bool retry = false, paused = false;
        SDL_AudioDeviceID retired = 0;
        {
            std::lock_guard lock(audio_mutex);
            const auto now = Clock::now();
            if (!audio_device && !audio_playback_pauses) {
                advance_silent_playback_locked(now);
            }
            else if (!audio_playback_pauses) playback_clock.advance(now, false);

            paused = audio_playback_pauses != 0;
            retry = !paused && (reopen_device.exchange(false) || (!audio_device && now >= next_retry) ||
                (audio_device && SDL_GetAudioDeviceStatus(audio_device) == SDL_AUDIO_STOPPED));
            if (retry) {
                retired = audio_device;
                if (retired) SDL_PauseAudioDevice(retired, 1);
                audio_device = 0; audio_started = false; test_frames = 0;
            }
        }
        if (paused) {
            std::unique_lock wake(device_wake_mutex);
            device_wake.wait_for(wake, std::chrono::milliseconds(20));
            continue;
        }
        if (retry) {

            if (retired) SDL_CloseAudioDevice(retired);
            auto opened = open_output();
            std::lock_guard lock(audio_mutex);
            const auto now = Clock::now();

            if (silent_playback_active) {
                if (!audio_playback_pauses) advance_silent_playback_locked(now);
            }
            else playback_clock.advance(now, false);
            audio_device = opened.device; audio_callback_frames = opened.frames;
            silent_playback_active = !audio_device;

            audio_started = false;
            start_audio_if_ready_locked();
            next_retry = now + std::chrono::seconds(2);
        }
        if (test_pending.exchange(false)) {
            std::lock_guard lock(audio_mutex);
            if (audio_playback_pauses) { test_pending = true; continue; }
            if (audio_device) {
                SDL_LockAudioDevice(audio_device); test_frames = 28800; SDL_UnlockAudioDevice(audio_device);
                audio_started = true; SDL_PauseAudioDevice(audio_device, 0);
            }
        }
    }
}
}

bool initialize_audio() {
    std::lock_guard lifecycle(device_lifecycle_mutex);
    if (device_worker.joinable()) return true;
    std::lock_guard lock(audio_mutex);
    audio_failed.store(false, std::memory_order_relaxed);
    audio_output_frequency = 48000;
    {
        std::lock_guard phase_lock(audio_phase_mutex);
        audio_activity = RT64::AudioProfile::Phase::Startup;
        RT64::AudioProfile::start(++audio_profile_stream, audio_output_frequency, 2);
        audio_profile_active = true;
    }
    if (!open_audio_locked(audio_input_frequency)) {
        audio_failed.store(true, std::memory_order_relaxed);
        finish_audio_profile_locked();
        return false;
    }
    auto opened = open_output();
    audio_device = opened.device; audio_callback_frames = opened.frames;
    silent_playback_active = !audio_device;
    const auto now = jfc::audio::PlaybackClock::Clock::now();
    playback_clock = jfc::audio::PlaybackClock(now);
    playback_clock.set_paused(now, audio_playback_pauses != 0);
    reopen_device = false; test_pending = false;
    start_audio_if_ready_locked();
    SDL_AddEventWatch(watch_audio, nullptr);
    try { device_worker = std::jthread(output_worker); }
    catch (...) {
        SDL_DelEventWatch(watch_audio, nullptr);
        if (audio_device) SDL_CloseAudioDevice(audio_device);
        audio_device = 0; audio_stream.reset(); finish_audio_profile_locked();
        throw;
    }
    return true;
}

void shutdown_audio() {
    std::lock_guard lifecycle(device_lifecycle_mutex);
    ::set_audio_activity(ultramodern::AudioActivity::Shutdown);
    SDL_DelEventWatch(watch_audio, nullptr);
    device_worker.request_stop(); device_wake.notify_one();
    if (device_worker.joinable()) device_worker.join();
    std::lock_guard lock(audio_mutex);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    audio_device = 0; audio_stream.reset(); finish_audio_profile_locked();
    audio_started = false; test_frames = 0;
    silent_playback_active = false;
    audio_output_frequency = audio_callback_frames = 0;
    audio_output.clear();
}

uint64_t jfc::audio::output_revision() { return devices_revision.load(std::memory_order_acquire); }
bool jfc::audio::output_failed() { return audio_failed.load(std::memory_order_relaxed); }
jfc::audio::OutputDevices jfc::audio::output_devices() {
    std::lock_guard lock(devices_mutex); return devices;
}
bool jfc::audio::select_output(const std::string& name) {
    if (name.size() > 1024 || name.find('\0') != std::string::npos) return false;
    {
        std::lock_guard lock(devices_mutex);
        if (devices.selected == name) return true;
        devices.selected = name; publish_devices_locked();
    }
    refresh_outputs(); return true;
}
void jfc::audio::refresh_outputs() { reopen_device = true; device_wake.notify_one(); }
void jfc::audio::test_output(double volume) {
    test_volume.store(int(std::clamp(std::isfinite(volume) ? volume : 0.0, 0.0, 100.0)));
    test_pending = true; device_wake.notify_one();
}

void queue_audio_samples(int16_t* samples, size_t sample_count, double main_volume) {
    if (sample_count == 0) return;
    if (!samples || (sample_count & 1) || sample_count > size_t(INT_MAX) / sizeof(int16_t))
        throw std::runtime_error("Invalid host stereo audio input extent");
    if (!std::isfinite(main_volume))
        throw std::runtime_error("Invalid host audio volume");
    std::lock_guard lock(audio_mutex);
    if (audio_stream == nullptr) {
        return;
    }
    if (RT64::PerformanceProfiler::enabled()) {

        RT64::PerformanceProfiler::recordCounter(
            "Audio.Native.BufferFrames", static_cast<double>(sample_count / 2),
            0, audio_input_frequency, audio_output_frequency);
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Host.QueuedFramesBeforeSubmit",
            audio_output.size() / 2);
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Host.UnderrunFrames", audio_output.underruns() / 2);
    }
    static std::vector<int16_t> input;
    static std::vector<int16_t> converted;
    input.resize(sample_count);
    const int volume = static_cast<int>(std::clamp(
        main_volume, 0.0, 100.0));
    for (size_t index = 0; index < sample_count; index += 2) {
        input[index] = static_cast<int16_t>(
            static_cast<int32_t>(samples[index + 1]) * volume / 200);
        input[index + 1] = static_cast<int16_t>(
            static_cast<int32_t>(samples[index]) * volume / 200);
    }
    try {
        audio_stream->put(input);
        audio_stream->take(converted);
        audio_output.append(converted);
        RT64::AudioProfile::produced(audio_profile_stream, sample_count / 2, converted.size() / 2);
        start_audio_if_ready_locked();
    } catch (const std::exception& error) {
        audio_failed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "Audio output failed: %s\n", error.what());
        ultramodern::quit();
    }
}

ultramodern::audio_queue_state_t audio_queue_state() {
    std::lock_guard lock(audio_mutex);
    if (audio_stream == nullptr || audio_output_frequency == 0) {
        return {};
    }
    const size_t output_frames =
        audio_output.size() / 2;
    const size_t input_frames = uint64_t(output_frames) * audio_input_frequency / audio_output_frequency;
    const size_t pending_frames = audio_stream ? audio_stream->pending_input_frames() : 0;
    if (RT64::PerformanceProfiler::enabled()) {
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Native.RemainingFrames", static_cast<double>(input_frames),
            0, audio_input_frequency, audio_output_frequency);
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Native.PendingInputFrames", static_cast<double>(pending_frames),
            0, audio_input_frequency, audio_output_frequency);
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Native.AuthoredRemainingFrames", static_cast<double>(input_frames + pending_frames),
            0, audio_input_frequency, audio_output_frequency);
    }
    return {input_frames, pending_frames};
}

void set_frequency(uint32_t frequency) {
    std::lock_guard lock(audio_mutex);
    if (frequency == audio_input_frequency) {
        return;
    }
    if (audio_stream == nullptr) {
        audio_input_frequency = frequency;
        return;
    }
    if (!open_audio_locked(frequency)) {
        audio_failed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "Failed to change audio frequency: %s\n", SDL_GetError());
        ultramodern::quit();
    }
}

namespace {
void write_samples(jfc::state::Writer& out, const jfc::audio::Samples& samples) {
    if ((samples.size() & 1) || samples.size() > jfc::audio::OutputQueue::max_samples)
        throw std::runtime_error("Invalid saved PCM extent");
    out.u32(uint32_t(samples.size()));
    for (size_t i = 0; i < samples.size(); i += 2)
        out.u32(uint32_t(uint16_t(samples[i])) | (uint32_t(uint16_t(samples[i + 1])) << 16));
}
jfc::audio::Samples read_samples(jfc::state::Reader& in, size_t maximum) {
    const auto count = in.u32();
    if ((count & 1) || count > maximum) throw std::runtime_error("Invalid saved PCM length");
    jfc::audio::Samples samples(count);
    for (size_t i = 0; i < count; i += 2) {
        const auto pair = in.u32();
        samples[i] = std::bit_cast<int16_t>(uint16_t(pair));
        samples[i + 1] = std::bit_cast<int16_t>(uint16_t(pair >> 16));
    }
    return samples;
}
}

jfc::state::AudioPlaybackPause::AudioPlaybackPause() {
    std::lock_guard lock(audio_mutex);
    pause_playback_locked();
}
jfc::state::AudioPlaybackPause::~AudioPlaybackPause() {
    std::lock_guard lock(audio_mutex);
    resume_playback_locked();
}

struct jfc::state::AudioPause::Impl {
    std::unique_lock<std::mutex> lock{audio_mutex, std::defer_lock};
    std::unique_ptr<jfc::audio::StateStream> prepared_stream;
    jfc::audio::Samples prepared_queue;
    size_t prepared_count = 0;
    std::array<uint8_t, 4096> prepared_dmem{};
    ultramodern::state::AudioState prepared_runtime;
    bool prepared = false, committed = false, acquired = false, playback_acquired = false;
    ~Impl() {
        if (acquired) {
            if (playback_acquired) resume_playback_locked();
            state_audio_paused.store(false, std::memory_order_release);
        }
    }
};
jfc::state::AudioPause::AudioPause() : impl(std::make_unique<Impl>()) {
    bool expected = false;
    if (!state_audio_paused.compare_exchange_strong(expected, true))
        throw std::runtime_error("Audio save-state transaction already active");
    impl->acquired = true;
    impl->lock.lock();
    pause_playback_locked();
    impl->playback_acquired = true;
}
jfc::state::AudioPause::~AudioPause() = default;
jfc::state::Bytes jfc::state::AudioPause::capture() const {
    const char* filter = SDL_GetHint(SDL_HINT_AUDIO_RESAMPLING_MODE);
    if (filter && *filter && std::strcmp(filter, "0") && std::strcmp(filter, "default"))
        throw std::runtime_error("Save states require SDL's default resampler; remove SDL_AUDIO_RESAMPLING_MODE override");
    Writer out; out.u32(1);
    const auto runtime = ultramodern::state::capture_audio();
    ultramodern::state::validate_audio(runtime);
    const auto& stream = audio_stream->captured();
    if (stream.input_rate != runtime.sample_rate || stream.output_rate != 48000)
        throw std::runtime_error("Audio owners disagree at save-state boundary");
    out.fields(runtime.sample_rate, runtime.feedback_offset, stream.input_rate, stream.output_rate);
    write_samples(out, stream.history); write_samples(out, stream.staging);
    write_samples(out, audio_output.capture());
    for (size_t i = 0; i < impl->prepared_dmem.size(); i += 4)
        out.u32(uint32_t(dmem[i]) | (uint32_t(dmem[i + 1]) << 8) |
            (uint32_t(dmem[i + 2]) << 16) | (uint32_t(dmem[i + 3]) << 24));
    return std::move(out.bytes);
}
void jfc::state::AudioPause::prepare(std::span<const uint8_t> saved) {
    Reader in(saved);
    if (in.u32() != 1) throw std::runtime_error("Unsupported audio save-state schema");
    ultramodern::state::AudioState runtime;
    jfc::audio::StreamState stream;
    in.fields(runtime.sample_rate, runtime.feedback_offset, stream.input_rate, stream.output_rate);
    ultramodern::state::validate_audio(runtime);
    jfc::audio::StateStream::validate_rates(stream.input_rate, stream.output_rate);
    if (stream.input_rate != runtime.sample_rate || stream.output_rate != audio_output_frequency)
        throw std::runtime_error("Incompatible save-state audio output format");

    stream.history = read_samples(in, 512 * 24 * 4);
    stream.staging = read_samples(in, 512 * 24 * 2);
    auto queue = read_samples(in, jfc::audio::OutputQueue::max_samples);
    std::array<uint8_t, 4096> rsp_memory;
    for (size_t i = 0; i < rsp_memory.size(); i += 4) {
        const auto value = in.u32();
        for (size_t n = 0; n < 4; ++n) rsp_memory[i + n] = uint8_t(value >> (8 * n));
    }
    in.end();
    auto prepared = std::make_unique<jfc::audio::StateStream>(stream);
    const auto queue_count = queue.size();
    queue.resize(std::max(size_t(48000 * 2 * 2), queue.size()));
    impl->prepared_stream = std::move(prepared);
    impl->prepared_queue = std::move(queue); impl->prepared_count = queue_count;
    impl->prepared_runtime = runtime; impl->prepared_dmem = rsp_memory;
    impl->prepared = true;
}
void jfc::state::AudioPause::commit() noexcept {
    if (!impl->prepared || impl->committed) std::terminate();
    audio_stream.swap(impl->prepared_stream);
    audio_input_frequency = impl->prepared_runtime.sample_rate;
    audio_output.commit(impl->prepared_queue, impl->prepared_count);
    playback_clock.reset_queue(jfc::audio::PlaybackClock::Clock::now());

    audio_started = impl->prepared_count / 2 >= audio_callback_frames;
    std::copy(impl->prepared_dmem.begin(), impl->prepared_dmem.end(), dmem);
    ultramodern::state::restore_audio(impl->prepared_runtime);
    impl->committed = true;
}
