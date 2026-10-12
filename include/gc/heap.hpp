#pragma once

#include "bridge.hpp"
#include "root.hpp"
#include "utils.hpp"

#include <memory>
#include <functional>

namespace gc
{

class Heap
{
    using space_t = std::unique_ptr<uint8_t[]>;
public:
    explicit Heap(roots_t const& dynamicRoots, roots_t const& permanentRoots, StellaGcStats& stats)
        : m_dynamicRoots{dynamicRoots}
        , m_permanentRoots{permanentRoots}
        , m_stats{stats}
    {}

    void init(StellaGcConfig const& cfg)
    {
        if (m_fromSpace) { stella_abi_violation("re-initialization of heap requested"); }
        if (cfg.max_heap_bytes == 0) { stella_abi_violation("zero-length heap size requested"); }

        try
        {
            m_fromSpace = std::make_unique<uint8_t[]>(cfg.max_heap_bytes);
            m_toSpace = std::make_unique<uint8_t[]>(cfg.max_heap_bytes);
        }
        catch(std::bad_alloc const&)
        {
            stella_abi_violation("OOM during heap allocation");
        }
        
        m_limit = m_end = cfg.max_heap_bytes;
    }

    bool inFromSpace(void* object)
    {
        return object >= m_fromSpace.get() && object <= m_fromSpace.get() + m_end;
    }

    bool inToSpace(void* object)
    {
        return object >= m_toSpace.get() && object <= m_toSpace.get() + m_end;
    }

    bool owns(void* object)
    {
        return inToSpace(object) || inFromSpace(object);
    }

    StellaValue fromBegin() const noexcept { return reinterpret_cast<StellaValue>(m_fromSpace.get()); }
    StellaValue toBegin() const noexcept { return reinterpret_cast<StellaValue>(m_toSpace.get()); }
    size_t fromNext() const noexcept { return m_next; }
    size_t limit() const noexcept { return m_limit; }
    bool isCollecting() const noexcept { return m_isCollecting; }

    StellaValue alloc(StellaObjectDescriptor const* desc)
    {
        collectionStep();

        auto const requested = utils::object_size(desc);
        auto const toAlloc = utils::round_object_size(requested);
        
        m_stats.my_requested_bytes += requested;
        m_stats.my_rounded_bytes += toAlloc;

        StellaValue out{};
        if (m_isCollecting) { out = allocTo(toAlloc); }
        else { out = allocFrom(toAlloc); }

        out->descriptor = desc;
        out->fields = desc->slot_capacity == 0 ? nullptr : reinterpret_cast<StellaSlot*>(out + 1);
        out->gc_word = STELLA_GC_NULLPTR; // do not set forwarding reference since object allocated in new space
        for (size_t managedIdx = 0; managedIdx < desc->managed_count; ++managedIdx) { out->fields[managedIdx].managed = nullptr; }
        for (size_t primitiveIdx = 0; primitiveIdx < desc->primitive_count; ++primitiveIdx)
        {
            auto const real = desc->managed_count + primitiveIdx;
            out->fields[real].primitive = 0;
        }

        m_stats.my_occupied_bytes = m_next + (m_end - m_limit);
        m_stats.my_maximum_occupied_bytes = std::max(m_stats.my_maximum_occupied_bytes, m_stats.my_occupied_bytes);
        ++m_stats.allocated_objects;

        return out;
    }

    StellaValue forward(StellaValue obj)
    {
        if (!m_isCollecting) { return obj; }

        ++m_stats.read_barrier_activations;
        return forwardImpl(obj);
    }

private:
    bool enoughSpace(size_t sz) { return m_limit - sz >= m_next; }

    void indicateOom(size_t requested)
    {
        ++m_stats.failed_allocations;
        stella_gc_out_of_memory(requested);
    }

    StellaValue forwardImpl(StellaValue obj)
    {
        if (!obj || !inFromSpace(obj)) { return obj; }
        if (obj->gc_word != STELLA_GC_NULLPTR) { return reinterpret_cast<StellaValue>(obj->gc_word); }

        auto const objSize = utils::round_object_size(utils::object_size(obj->descriptor));
        if (utils::add_overflow(m_next, objSize)) { stella_abi_violation("heap position overflow during forward"); }
        if (m_next + objSize > m_limit) { indicateOom(objSize); }

        std::memcpy(m_toSpace.get() + m_next, obj, objSize);
        auto forwarded = reinterpret_cast<StellaValue>(m_toSpace.get() + m_next);
        forwarded->fields = reinterpret_cast<StellaSlot*>(forwarded + 1);

        m_next += objSize;
        obj->gc_word = reinterpret_cast<decltype(obj->gc_word)>(forwarded);
        return forwarded;
    }

    void scanOne()
    {
        auto obj = reinterpret_cast<StellaValue>(m_toSpace.get() + m_scan);
        for (size_t fieldIdx = 0; fieldIdx < obj->descriptor->managed_count; ++fieldIdx)
        {
            auto& field = obj->fields[fieldIdx].managed;
            field = forwardImpl(field);
        }

        m_scan += utils::round_object_size(utils::object_size(obj->descriptor));
        // TODO: inc metric
    }

    void finishWoBudget()
    {
        while (m_scan < m_next)
        {
            scanOne();
            ++m_stats.scan_out_of_budget;
        }

        m_isCollecting = false;
        std::swap(m_fromSpace, m_toSpace);
    }

    void collect()
    {
        ++m_stats.collections;

        m_isCollecting = true;
        m_scan = m_next = 0;
        m_limit = m_end;
        
        for (auto& root : m_permanentRoots) { *root.slot = forwardImpl(*root.slot); }
        for (auto& root : m_dynamicRoots) { *root.slot = forwardImpl(*root.slot); }
    }

    void collectionStep()
    {
        if (!m_isCollecting) { return; }

        while (m_scan < m_next && m_k --> 0) { scanOne(); }

        if (m_scan == m_next)
        {
            m_isCollecting = false;
            std::swap(m_fromSpace, m_toSpace);
        }
    }

    StellaValue allocFrom(size_t objSize)
    {
        if (enoughSpace(objSize))
        {
            auto allocated = reinterpret_cast<StellaValue>(m_fromSpace.get() + m_next);
            m_next += objSize;
            return allocated;
        }
        else
        {
            collect();
            return allocTo(objSize);
        }
    }

    StellaValue allocTo(size_t objSize)
    {
        if (enoughSpace(objSize))
        {
            m_limit -= objSize;
            return reinterpret_cast<StellaValue>(m_toSpace.get() + m_limit);
        }
        else
        {
            finishWoBudget();
            return allocFrom(objSize);
        }
    }

    static constexpr decltype(StellaObject::gc_word) STELLA_GC_NULLPTR = 0;

    roots_t const& m_dynamicRoots;
    roots_t const& m_permanentRoots;

    space_t m_fromSpace;
    space_t m_toSpace;

    size_t m_scan{};
    size_t m_next{};
    size_t m_limit{};
    size_t m_end{};

    size_t m_k{1};
    bool m_isCollecting{};

    StellaGcStats& m_stats;
};

} // namespace gc
