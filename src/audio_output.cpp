#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio_output.hpp"
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

namespace {
SDL_AudioDeviceID audio_device = 0;
std::unique_ptr<jfc::audio::StateStream> audio_stream;
uint32_t audio_input_frequency = 32000, audio_output_frequency = 0;
uint32_t audio_callback_frames = 0;
bool audio_started = false;

uint64_t audio_profile_stream = 0, audio_profile_callback = 0;
std::mutex audio_mutex;
jfc::audio::OutputQueue audio_output;
std::atomic_bool state_audio_paused = false;

void start_audio_if_ready_locked() {
    if (audio_device && !audio_started && audio_callback_frames &&
        audio_output.size() / 2 >= audio_callback_frames) {
        audio_started = true;
        RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Gameplay);
        SDL_PauseAudioDevice(audio_device, 0);
    }
}
}

bool open_audio_locked(uint32_t frequency) {
    if (frequency == 0 ||
        frequency > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (audio_device == 0) {
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = [](void*, Uint8* data, int bytes) {
            const bool profiling = RT64::PerformanceProfiler::enabled();
            const uint64_t callback = ++audio_profile_callback;
            const uint64_t missing_before = audio_output.underruns();
            if (profiling) RT64::AudioProfile::callbackBegin(audio_profile_stream,
                callback, size_t(bytes) / (2 * sizeof(int16_t)), audio_output.size() / 2);
            audio_output.consume(std::span(reinterpret_cast<int16_t*>(data), size_t(bytes) / sizeof(int16_t)));
            if (profiling) RT64::AudioProfile::callbackEnd(audio_profile_stream,
                callback, (audio_output.underruns() - missing_before) / 2);
        };
        SDL_AudioSpec obtained{};
        audio_device = SDL_OpenAudioDevice(
            nullptr, 0, &desired, &obtained, 0);
        if (audio_device == 0) {
            return false;
        }
        audio_output_frequency = static_cast<uint32_t>(obtained.freq);
        audio_callback_frames = obtained.samples;
        audio_started = false;
        RT64::AudioProfile::start(++audio_profile_stream, audio_output_frequency, 2);
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
        SDL_CloseAudioDevice(audio_device);
        RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Shutdown);
        RT64::AudioProfile::stop(audio_profile_stream);
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

bool initialize_audio() {
    std::lock_guard lock(audio_mutex);
    return open_audio_locked(audio_input_frequency);
}

void shutdown_audio() {
    std::lock_guard lock(audio_mutex);
    if (audio_stream != nullptr) {
        audio_stream.reset();
    }
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Shutdown);
        RT64::AudioProfile::stop(audio_profile_stream);
        audio_device = 0;
    }
    audio_started = false;
    audio_output_frequency = audio_callback_frames = 0;
    audio_output.clear();
}

void queue_audio_samples(int16_t* samples, size_t sample_count, double main_volume) {
    if (samples == nullptr || sample_count < 2) {
        return;
    }
    std::lock_guard lock(audio_mutex);
    if (audio_device == 0 || audio_stream == nullptr) {
        return;
    }
    sample_count &= ~size_t{1};
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
        std::fprintf(stderr, "Audio output failed: %s\n", error.what());
        ultramodern::quit();
    }
}

size_t frames_remaining() {
    std::lock_guard lock(audio_mutex);
    if (audio_device == 0 || audio_output_frequency == 0) {
        return 0;
    }
    const size_t output_frames =
        audio_output.size() / 2;
    const size_t input_frames = output_frames * audio_input_frequency / audio_output_frequency;
    if (RT64::PerformanceProfiler::enabled()) {
        RT64::PerformanceProfiler::recordCounter(
            "Audio.Native.RemainingFrames", static_cast<double>(input_frames),
            0, audio_input_frequency, audio_output_frequency);
    }
    return input_frames;
}

void set_frequency(uint32_t frequency) {
    std::lock_guard lock(audio_mutex);
    if (frequency == audio_input_frequency) {
        return;
    }
    if (audio_device == 0) {
        audio_input_frequency = frequency;
        return;
    }
    if (!open_audio_locked(frequency)) {
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

struct jfc::state::AudioPause::Impl {
    std::unique_lock<std::mutex> lock{audio_mutex, std::defer_lock};
    std::unique_ptr<jfc::audio::StateStream> prepared_stream;
    jfc::audio::Samples prepared_queue;
    size_t prepared_count = 0;
    std::array<uint8_t, 4096> prepared_dmem{};
    ultramodern::state::AudioState prepared_runtime;
    bool prepared = false, committed = false, acquired = false;
    ~Impl() {
        if (acquired) {
            if (audio_device) {
                if (audio_started) {
                    RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Gameplay);
                    SDL_PauseAudioDevice(audio_device, 0);
                } else {
                    RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Startup);
                    start_audio_if_ready_locked();
                }
            }
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
    if (!audio_device || !audio_stream) throw std::runtime_error("Audio output is unavailable for saving state");
    SDL_PauseAudioDevice(audio_device, 1);
    RT64::AudioProfile::phase(audio_profile_stream, RT64::AudioProfile::Phase::Paused);
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

    audio_started = impl->prepared_count / 2 >= audio_callback_frames;
    std::copy(impl->prepared_dmem.begin(), impl->prepared_dmem.end(), dmem);
    ultramodern::state::restore_audio(impl->prepared_runtime);
    impl->committed = true;
}
