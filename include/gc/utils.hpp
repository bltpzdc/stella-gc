#pragma once

#include <cstdint>

#include "bridge.hpp"

namespace gc::utils
{

inline bool add_overflow(size_t l, size_t r)
{
    return l > SIZE_MAX - r;
}

inline bool mul_overflow(size_t l, size_t r)
{
    return r != 0 && l > SIZE_MAX / r;
}

inline size_t object_size(StellaObjectDescriptor const* desc)
{
    if (mul_overflow(desc->slot_capacity, sizeof(StellaSlot))) { stella_abi_violation("slot length overflow"); }
    auto const slotsSize = desc->slot_capacity * sizeof(StellaSlot);
    if (add_overflow(slotsSize, sizeof(StellaObject))) { stella_abi_violation("object total size overflow"); }

    return slotsSize + sizeof(StellaObject);
}

inline size_t round_object_size(size_t objectSize)
{
    auto const alignment = alignof(max_align_t);
    auto const overBorder = objectSize % alignment;
    if (!overBorder) { return objectSize; }

    auto const padding = alignment - overBorder;
    if (add_overflow(objectSize, padding)) { stella_abi_violation("object total round size overflow"); }
    return objectSize + padding;
}

} // namespace gc::utils
