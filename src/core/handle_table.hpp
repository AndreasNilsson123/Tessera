#pragma once

#include <tessera/tessera.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace tessera {

/*
 * Thread-safe table mapping opaque handles to shared objects.
 *
 * Handle layout (see ADR 0003):  [63..56] type tag | [55..32] generation (24 bit, never 0) | [31..0] index
 *
 *  - 0 is never a valid handle (generation and tag are non-zero).
 *  - A destroyed slot's generation is bumped, so a stale handle fails instead of aliasing a new object.
 *  - The type tag keeps handles of different object kinds from being confused with one another.
 *  - get() returns a shared_ptr: an object being used by one thread stays alive if another thread
 *    destroys its handle concurrently. Objects are released outside the table lock, because their
 *    destructors may call into native driver code.
 *  - closeAndDrain() makes later insert() calls fail; open() re-enables the table (init/terminate).
 */
template <class T> class HandleTable {
  public:
    HandleTable(uint8_t tag, uint32_t maxEntries) : tag_(tag), max_(maxEntries) {}

    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    void open() {
        std::lock_guard<std::mutex> lock(m_);
        open_ = true;
    }

    bool isOpen() const {
        std::lock_guard<std::mutex> lock(m_);
        return open_;
    }

    /* Returns TESSERA_NULL_HANDLE if the table is closed or full. May throw std::bad_alloc. */
    TesseraHandle insert(std::shared_ptr<T> object) {
        std::lock_guard<std::mutex> lock(m_);
        if (!open_)
            return TESSERA_NULL_HANDLE;
        uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (slots_.size() >= max_)
                return TESSERA_NULL_HANDLE;
            slots_.emplace_back();
            index = static_cast<uint32_t>(slots_.size() - 1);
        }
        slots_[index].object = std::move(object);
        ++live_;
        return make(index, slots_[index].generation);
    }

    /* Null if the handle is zero, malformed, of another type, stale, or destroyed. */
    std::shared_ptr<T> get(TesseraHandle handle) const {
        std::lock_guard<std::mutex> lock(m_);
        const Slot* slot = lookup(handle);
        return slot ? slot->object : nullptr;
    }

    /* Invalidates the handle and returns the object (null if the handle was not valid). The caller
     * should let the returned pointer die outside any lock of its own. */
    std::shared_ptr<T> remove(TesseraHandle handle) {
        std::shared_ptr<T> removed;
        {
            std::lock_guard<std::mutex> lock(m_);
            Slot* slot = lookup(handle);
            if (!slot)
                return nullptr;
            removed = std::move(slot->object);
            slot->object.reset();
            retire(*slot, static_cast<uint32_t>(slot - slots_.data()));
        }
        return removed;
    }

    /* Closes the table and invalidates every handle. Returned objects are released by the caller. */
    std::vector<std::shared_ptr<T>> closeAndDrain() {
        std::vector<std::shared_ptr<T>> drained;
        std::lock_guard<std::mutex> lock(m_);
        open_ = false;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].object) {
                drained.push_back(std::move(slots_[i].object));
                slots_[i].object.reset();
                retire(slots_[i], static_cast<uint32_t>(i));
            }
        }
        return drained;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_);
        return live_;
    }

  private:
    struct Slot {
        std::shared_ptr<T> object;
        uint32_t generation = 1; // 1..kGenMask
    };
    static constexpr uint32_t kGenMask = 0x00FFFFFFu;

    TesseraHandle make(uint32_t index, uint32_t generation) const {
        return (static_cast<uint64_t>(tag_) << 56) | (static_cast<uint64_t>(generation) << 32) | index;
    }

    // Caller holds m_.
    Slot* lookup(TesseraHandle handle) {
        if (static_cast<uint8_t>(handle >> 56) != tag_)
            return nullptr;
        const uint32_t generation = static_cast<uint32_t>(handle >> 32) & kGenMask;
        const uint32_t index = static_cast<uint32_t>(handle);
        if (index >= slots_.size())
            return nullptr;
        Slot& slot = slots_[index];
        if (!slot.object || slot.generation != generation)
            return nullptr;
        return &slot;
    }
    const Slot* lookup(TesseraHandle handle) const {
        return const_cast<HandleTable*>(this)->lookup(handle);
    }

    // Caller holds m_. Bumps the generation (wrapping past 0) and recycles the index.
    void retire(Slot& slot, uint32_t index) {
        slot.generation = (slot.generation + 1u) & kGenMask;
        if (slot.generation == 0)
            slot.generation = 1;
        free_.push_back(index);
        --live_;
    }

    const uint8_t tag_;
    const uint32_t max_;
    mutable std::mutex m_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::size_t live_ = 0;
    bool open_ = false;
};

} // namespace tessera
