#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace af::jni {

enum class HandleKind : std::uint8_t {
    Engine = 0,
    Renderer = 1,
};

/*
 * JNI-visible handles are positive opaque tokens, never native addresses. Each token carries a
 * type bit, a slot index, and a slot generation. Removing a handle invalidates its generation;
 * lookups return a shared lease, so removing a token cannot destroy an object under an active JNI
 * call. A slot is retired rather than allowing its generation to wrap and revive an old token.
 */
class HandleRegistry {
public:
    template <typename T>
    std::int64_t insert(HandleKind kind, const std::shared_ptr<T>& object) {
        if (!object) return 0;
        std::lock_guard<std::mutex> lock(mutex_);

        std::size_t index = 0;
        for (; index < slots_.size(); ++index) {
            if (!slots_[index].object && !slots_[index].retired) break;
        }
        if (index >= kSlotMask) return 0;
        if (index == slots_.size()) slots_.emplace_back();

        Slot& slot = slots_[index];
        slot.kind = kind;
        slot.object = object;
        return encode(kind, slot.generation, static_cast<std::uint32_t>(index));
    }

    template <typename T>
    std::shared_ptr<T> get(std::int64_t token, HandleKind expectedKind) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        HandleKind kind = HandleKind::Engine;
        if (!decode(token, &kind, &generation, &index) || kind != expectedKind ||
            index >= slots_.size()) {
            return {};
        }
        const Slot& slot = slots_[index];
        if (!slot.object || slot.kind != expectedKind || slot.generation != generation) return {};
        return std::static_pointer_cast<T>(slot.object);
    }

    template <typename T>
    std::shared_ptr<T> remove(std::int64_t token, HandleKind expectedKind) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
        HandleKind kind = HandleKind::Engine;
        if (!decode(token, &kind, &generation, &index) || kind != expectedKind ||
            index >= slots_.size()) {
            return {};
        }
        Slot& slot = slots_[index];
        if (!slot.object || slot.kind != expectedKind || slot.generation != generation) return {};

        std::shared_ptr<void> erased = std::move(slot.object);
        std::shared_ptr<T> object = std::static_pointer_cast<T>(erased);
        if (slot.generation == kGenerationMask) {
            slot.retired = true;
        } else {
            ++slot.generation;
        }
        return object;
    }

private:
    static constexpr std::uint64_t kSlotMask = 0x7fffffffULL;
    static constexpr std::uint32_t kGenerationMask = 0x7fffffffU;

    struct Slot {
        std::shared_ptr<void> object;
        std::uint32_t generation = 1;
        HandleKind kind = HandleKind::Engine;
        bool retired = false;
    };

    static std::int64_t encode(HandleKind kind, std::uint32_t generation, std::uint32_t index) {
        const std::uint64_t token = (std::uint64_t(kind) << 62) |
                                    (std::uint64_t(generation) << 31) |
                                    (std::uint64_t(index) + 1);
        return static_cast<std::int64_t>(token);
    }

    static bool decode(std::int64_t token, HandleKind* kind, std::uint32_t* generation,
                       std::uint32_t* index) {
        if (token <= 0 || !kind || !generation || !index) return false;
        const std::uint64_t bits = static_cast<std::uint64_t>(token);
        const std::uint64_t slotCode = bits & kSlotMask;
        const std::uint32_t tokenGeneration = static_cast<std::uint32_t>((bits >> 31) & kGenerationMask);
        if (slotCode == 0 || tokenGeneration == 0) return false;
        *kind = static_cast<HandleKind>((bits >> 62) & 1U);
        *generation = tokenGeneration;
        *index = static_cast<std::uint32_t>(slotCode - 1);
        return true;
    }

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
};

}  // namespace af::jni
