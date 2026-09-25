#include "steam_deck_grips.hpp"

#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <string>

namespace {

constexpr uint16_t valve_vendor = 0x28DE;
constexpr uint16_t steam_deck_product = 0x1205;

int event_fd = -1;
std::array<int, jfc::deck_grips::raw_reader_capacity> raw_fds = [] {
    std::array<int, jfc::deck_grips::raw_reader_capacity> result{};
    result.fill(-1);
    return result;
}();
std::array<std::string, jfc::deck_grips::raw_reader_capacity> raw_paths{};
bool evdev_select_held = false;
bool evdev_l4_held = false;
bool evdev_l5_held = false, evdev_r5_held = false;
bool evdev_lower_available = false;
std::array<bool, jfc::deck_grips::raw_reader_capacity> raw_l4_held{};
std::array<bool, jfc::deck_grips::raw_reader_capacity> raw_r4_held{};
std::array<bool, jfc::deck_grips::raw_reader_capacity> raw_select_held{};
std::array<bool, jfc::deck_grips::raw_reader_capacity> raw_l5_held{};
std::array<bool, jfc::deck_grips::raw_reader_capacity> raw_r5_held{};
auto next_scan = std::chrono::steady_clock::time_point{};
bool unavailable_reported = false;
bool raw_reported = false;

void close_input(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool has_raw_reader() {
    for (int fd : raw_fds) {
        if (fd >= 0) {
            return true;
        }
    }
    return false;
}

void close_raw(size_t index) {
    close_input(raw_fds[index]);
    raw_paths[index].clear();
    raw_l4_held[index] = false;
    raw_r4_held[index] = false;
    raw_select_held[index] = false;
    raw_l5_held[index] = false;
    raw_r5_held[index] = false;
    if (!has_raw_reader()) {
        raw_reported = false;
    }
}

void open_deck() {
    const auto now = std::chrono::steady_clock::now();
    if ((event_fd >= 0 && evdev_lower_available) || has_raw_reader() || now < next_scan) {
        return;
    }
    next_scan = now + std::chrono::seconds(2);
    int last_error = 0;
    if (event_fd < 0) {
        constexpr size_t bits_per_word = sizeof(unsigned long) * 8;
        std::array<unsigned long,
            jfc::deck_grips::btn_gripr2 / bits_per_word + 1> key_bits{};
        for (int index = 0; index < 64; ++index) {
            const std::string path = "/dev/input/event" + std::to_string(index);
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (errno != ENOENT) {
                    last_error = errno;
                }
                continue;
            }
            input_id id{};
            key_bits.fill(0);
            const bool deck = ioctl(fd, EVIOCGID, &id) == 0 &&
                id.vendor == valve_vendor && id.product == steam_deck_product;
            const bool grips = ioctl(
                fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits.data()) >= 0 &&
                (key_bits[jfc::deck_grips::btn_gripl / bits_per_word] &
                    (1UL << (jfc::deck_grips::btn_gripl % bits_per_word))) != 0 &&
                (key_bits[jfc::deck_grips::btn_gripr / bits_per_word] &
                    (1UL << (jfc::deck_grips::btn_gripr % bits_per_word))) != 0;
            if (deck && grips) {
                event_fd = fd;
                evdev_lower_available =
                    (key_bits[jfc::deck_grips::btn_gripl2 / bits_per_word] &
                        (1UL << (jfc::deck_grips::btn_gripl2 % bits_per_word))) != 0 &&
                    (key_bits[jfc::deck_grips::btn_gripr2 / bits_per_word] &
                        (1UL << (jfc::deck_grips::btn_gripr2 % bits_per_word))) != 0;
                unavailable_reported = false;

                break;
            }
            close(fd);
        }
    }

    if (!evdev_lower_available && !has_raw_reader()) {
        size_t raw_count = 0;
        for (int index = 0; index < 32; ++index) {
            const std::string path = "/dev/hidraw" + std::to_string(index);
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (errno != ENOENT) {
                    last_error = errno;
                }
                continue;
            }
            struct hidraw_devinfo info{};
            if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
                    info.vendor == valve_vendor && info.product == steam_deck_product) {
                raw_fds[raw_count] = fd;
                raw_paths[raw_count] = path;
                ++raw_count;
                unavailable_reported = false;
                if (raw_count == raw_fds.size()) {
                    break;
                }
                continue;
            }
            close(fd);
        }
        if (raw_count != 0) {

        }
    }
    if (event_fd < 0 && !has_raw_reader() && !unavailable_reported) {
        std::fprintf(stderr, "JFC_CONTROLLER deck_grips=unavailable error=%d\n", last_error);
        unavailable_reported = true;
    }
}

}
#endif

jfc::deck_grips::Edges jfc::deck_grips::poll() {
#ifdef __linux__
    open_deck();
    Edges result{};
    if (event_fd >= 0) {
        input_event event{};
        for (;;) {
            const ssize_t size = read(event_fd, &event, sizeof(event));
            if (size == sizeof(event)) {
                if (event.type == EV_KEY && event.code == jfc::deck_grips::btn_gripl)
                    evdev_l4_held = event.value != 0;
                if (event.type == EV_KEY &&
                        event.code == jfc::deck_grips::btn_select) {
                    evdev_select_held = event.value != 0;
                }
                if (event.type == EV_KEY && event.code == jfc::deck_grips::btn_gripl2)
                    evdev_l5_held = event.value != 0;
                if (event.type == EV_KEY && event.code == jfc::deck_grips::btn_gripr2)
                    evdev_r5_held = event.value != 0;
                const Edges edges = parse_key_event(event.type, event.code, event.value);
                result.l4 = result.l4 || edges.l4;
                result.r4 = result.r4 || edges.r4;
                result.select = result.select || edges.select;
                result.l5 = result.l5 || edges.l5;
                result.r5 = result.r5 || edges.r5;
                continue;
            }
            if (size < 0 && errno == EINTR) continue;
            if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                std::fprintf(stderr, "JFC_CONTROLLER deck_grips=evdev_disconnected error=%d\n", errno);
                close_input(event_fd);
                evdev_select_held = false;
                evdev_l4_held = false;
                evdev_l5_held = evdev_r5_held = evdev_lower_available = false;
            }
            break;
        }
    }
    std::array<uint8_t, 64> report{};
    for (size_t index = 0; index < raw_fds.size(); ++index) {
        if (raw_fds[index] < 0) {
            continue;
        }
        for (;;) {
            const ssize_t size = read(
                raw_fds[index], report.data(), report.size());
            if (size > 0) {
                if (is_deck_report(report.data(), static_cast<size_t>(size)) &&
                        !raw_reported) {
                    raw_reported = true;

                }
                const Edges edges = parse_report(
                    report.data(), static_cast<size_t>(size),
                    raw_l4_held[index], raw_r4_held[index],
                    raw_select_held[index], raw_l5_held[index], raw_r5_held[index]);
                result.l4 = result.l4 || edges.l4;
                result.r4 = result.r4 || edges.r4;
                result.select = result.select || edges.select;
                result.l5 = result.l5 || edges.l5;
                result.r5 = result.r5 || edges.r5;
                continue;
            }
            if (size < 0 && errno == EINTR) continue;
            if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                std::fprintf(
                    stderr,
                    "JFC_CONTROLLER deck_grips=disconnected path=%s error=%d\n",
                    raw_paths[index].c_str(),
                    errno);
                close_raw(index);
            }
            break;
        }
    }
    result.select_held = evdev_select_held;
    result.upper_available = event_fd >= 0 || raw_reported;
    result.l4_held = evdev_l4_held;
    for (bool held : raw_l4_held) result.l4_held = result.l4_held || held;
    result.lower_available = (event_fd >= 0 && evdev_lower_available) || raw_reported;
    result.l5_held = evdev_l5_held;
    result.r5_held = evdev_r5_held;
    for (bool held : raw_select_held) {
        result.select_held = result.select_held || held;
    }
    for (bool held : raw_l5_held) result.l5_held = result.l5_held || held;
    for (bool held : raw_r5_held) result.r5_held = result.r5_held || held;
    return result;
#else
    return {};
#endif
}
