#include "save_state_memory.hpp"
#include "save_state_codec.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace jfc::state {
namespace {

constexpr uint32_t fault_queue = 0x801024e0U;
constexpr size_t thread_count = 5;
constexpr std::array<int32_t, thread_count> ids{-4, 0, 1, 3, 5};
constexpr std::array<uint32_t, thread_count> entries{
    0x80001d7cU, 0x80066bd4U, 0x8003f558U, 0x80044b50U, 0x8004f978U};

size_t offset(uint32_t address, size_t size) {
    if (address < 0x80000000U || size > state_rdram_size ||
        uint64_t(address) + size > 0x80000000ULL + state_rdram_size) {
        throw std::runtime_error("Save-state guest address is outside RDRAM");
    }
    return address - 0x80000000U;
}

template<class T> T read_guest(std::span<const uint8_t> memory, uint32_t address) {
    T value;
    std::memcpy(&value, memory.data() + offset(address, sizeof(T)), sizeof(T));
    return value;
}

void validate_memory_shape(std::span<const uint8_t> memory,
    std::span<const ultramodern::state::ThreadBinding> bindings, const Roots& roots,
    bool persistent) {
    if (memory.size() != state_rdram_size || bindings.size() != thread_count) {
        throw std::runtime_error("Save state requires 8 MiB RAM and the five initialized JFG threads");
    }
    uint32_t seen = 0;
    uint32_t previous = 0;
    for (const auto& binding : bindings) {
        const auto role = std::find(ids.begin(), ids.end(), binding.id);
        if (role == ids.end()) throw std::runtime_error("Unknown guest thread in save state");
        const size_t index = size_t(role - ids.begin());
        const uint32_t address = uint32_t(binding.address);
        if ((seen & (1U << index)) || address <= previous ||
            uint32_t(binding.entrypoint) != entries[index] ||
            (binding.id != 5 && binding.argument != 0)) {
            throw std::runtime_error("Save-state thread identity or ordering is incompatible");
        }
        seen |= 1U << index;
        previous = address;
        if (binding.id == 5) {
            offset(uint32_t(binding.argument), 0x308);
            if (uint32_t(binding.argument) + 0xb0 != address ||
                uint32_t(roots[2].gpr[18]) != uint32_t(binding.argument)) {
                throw std::runtime_error("Save-state scheduler context is incompatible");
            }
        }
        const auto thread = read_guest<OSThread>(memory, address);
        if (thread.id != binding.id || thread.sp != binding.initial_sp ||
            (persistent ? thread.context != nullptr : thread.context != binding.context)) {
            throw std::runtime_error("Save-state guest/native thread binding mismatch");
        }

        if (thread.next != 0 && std::none_of(bindings.begin(), bindings.end(),
                [&](const auto& other) { return other.address == thread.next; })) {
            throw std::runtime_error("Invalid save-state thread link");
        }
        if (binding.id == 0) {

            const auto queue = read_guest<OSMesgQueue>(memory, fault_queue);
            if (uint32_t(thread.queue) != fault_queue || queue.blocked_on_recv != binding.address ||
                queue.validCount != 0 || queue.blocked_on_send != 0 || thread.next != 0 ||
                queue.msgCount <= 0 || queue.msgCount > 16) {
                throw std::runtime_error("Fault thread is not at its safe receive boundary");
            }
            offset(uint32_t(queue.msg), size_t(queue.msgCount) * sizeof(OSMesg));
        } else {
            if (thread.queue != 0) throw std::runtime_error("Save-state thread is still queued");
            if (binding.id != 1) {
                constexpr std::array<int32_t, 3> root_ids{3, -4, 5};
                const auto root = std::find(root_ids.begin(), root_ids.end(), binding.id);
                if (thread.state != OSThreadState::STOPPED || root == root_ids.end() ||
                    roots[size_t(root - root_ids.begin())].thread != address) {
                    throw std::runtime_error("Save-state execution owner is not parked");
                }
            }
        }
    }
    for (const auto& root : roots) validate_root(root, root);
}

void write_root(Writer& out, const RootState& root) {
    out.u32(root.thread);
    for (auto value : root.gpr) out.u64(value);
    for (auto value : root.fpr) out.u64(value);
    out.u64(root.hi); out.u64(root.lo);
    out.u32(root.status); out.u32(root.float_mode);
    for (auto value : root.locals) out.u64(value);
    out.u32(root.cop1);
}

RootState read_root(Reader& in) {
    RootState root;
    root.thread = in.u32();
    for (auto& value : root.gpr) value = in.u64();
    for (auto& value : root.fpr) value = in.u64();
    root.hi = in.u64(); root.lo = in.u64();
    root.status = in.u32(); root.float_mode = in.u32();
    for (auto& value : root.locals) value = in.u64();
    root.cop1 = in.u32();
    return root;
}
}

MemoryState capture_memory(std::span<const uint8_t> memory,
    std::span<const ultramodern::state::ThreadBinding> bindings, const Roots& roots) {
    validate_memory_shape(memory, bindings, roots, false);
    MemoryState captured{Bytes(memory.begin(), memory.end()), {}};
    Writer out;
    out.u32(1); out.u32(uint32_t(bindings.size()));
    for (const auto& binding : bindings) {
        out.u32(uint32_t(binding.address)); out.u32(uint32_t(binding.entrypoint));
        out.u32(uint32_t(binding.argument)); out.u32(uint32_t(binding.initial_sp));
        out.u32(uint32_t(binding.id));
        std::fill_n(captured.memory.data() + offset(uint32_t(binding.address), sizeof(OSThread)) +
            offsetof(OSThread, context), sizeof(UltraThreadContext*), uint8_t{0});
    }
    for (const auto& root : roots) write_root(out, root);
    captured.threads = std::move(out.bytes);
    return captured;
}

PreparedMemory prepare_memory(const MemoryState& saved, std::span<const uint8_t> current_memory,
    std::span<const ultramodern::state::ThreadBinding> current_bindings, const Roots& current_roots) {
    validate_memory_shape(current_memory, current_bindings, current_roots, false);
    Reader in(saved.threads);
    if (in.u32() != 1 || in.u32() != thread_count) {
        throw std::runtime_error("Incompatible save-state thread schema");
    }
    for (const auto& current : current_bindings) {
        const uint32_t address = in.u32(), entry = in.u32(), argument = in.u32();
        const uint32_t sp = in.u32(), id = in.u32();
        if (address != uint32_t(current.address) || entry != uint32_t(current.entrypoint) ||
            argument != uint32_t(current.argument) || sp != uint32_t(current.initial_sp) ||
            id != uint32_t(current.id)) {
            throw std::runtime_error("Save-state thread layout differs from this process");
        }
    }
    PreparedMemory prepared;
    for (size_t i = 0; i < prepared.roots.size(); ++i) {
        prepared.roots[i] = read_root(in);
        validate_root(prepared.roots[i], current_roots[i]);
    }
    in.end();
    validate_memory_shape(saved.memory, current_bindings, prepared.roots, true);
    prepared.memory = saved.memory;
    for (const auto& binding : current_bindings) {
        const size_t thread_offset = offset(uint32_t(binding.address), sizeof(OSThread));
        const auto old_thread = read_guest<OSThread>(saved.memory, uint32_t(binding.address));
        const auto current_thread = read_guest<OSThread>(current_memory, uint32_t(binding.address));
        if (old_thread.priority != current_thread.priority ||
            ((binding.id == 0 || binding.id == 1) && old_thread.state != current_thread.state)) {
            throw std::runtime_error("Save-state thread scheduling identity is incompatible");
        }
        std::memcpy(prepared.memory.data() + thread_offset + offsetof(OSThread, context),
            &binding.context, sizeof(binding.context));
    }
    return prepared;
}
}
