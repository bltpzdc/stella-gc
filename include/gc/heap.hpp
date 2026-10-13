#pragma once

#include "bridge.hpp"
#include "config.hpp"
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

    void init(gc::config const& cfg)
    {
        if (m_fromSpace) { stella_abi_violation("re-initialization of heap requested"); }
        if (cfg.max_heap_bytes == 0) { stella_abi_violation("zero-length heap size requested"); }
        if (cfg.scansPerStep == 0) { stella_abi_violation("zero scans per allocation step"); }
        if (cfg.collectionThreshhold == 0 || cfg.collectionThreshhold > 100) { stella_abi_violation("collection threshold out of range"); }

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
        m_scansPerStep = cfg.scansPerStep;
        m_collectionThreshhold = cfg.collectionThreshhold;
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
        
        m_stats.requested_bytes += requested;
        m_stats.rounded_bytes += toAlloc;

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

        m_stats.occupied_bytes = m_next + (m_end - m_limit);
        m_stats.maximum_occupied_bytes = std::max(m_stats.maximum_occupied_bytes, m_stats.occupied_bytes);
        ++m_stats.allocated_objects;

        return out;
    }

    // this forward is used only for managed reads in Baker's algorithm
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

    // forwards object from FROM_SPACE to TO_SPACE
    // if object is static or null does nothing
    // if object was already forwarded returns the existing forwarding reference from meta gc_word field
    // if object is forwarded in 1st time, returns new address in TO_SPACE and sets a forwarding reference 
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

    // scans objects in Cheney's queue; invariant "scan < next" should be checked by caller
    void scanOne()
    {
        auto obj = reinterpret_cast<StellaValue>(m_toSpace.get() + m_scan);
        for (size_t fieldIdx = 0; fieldIdx < obj->descriptor->managed_count; ++fieldIdx)
        {
            auto& field = obj->fields[fieldIdx].managed;
            field = forwardImpl(field);
        }

        m_scan += utils::round_object_size(utils::object_size(obj->descriptor));
        ++m_stats.total_scans;
    }

    // synchronious collection finish in case of OOM for a new allocation
    void finishWoBudget()
    {
        while (m_scan < m_next)
        {
            ++m_stats.scan_out_of_budget;
            scanOne();
        }

        m_isCollecting = false;
        std::swap(m_fromSpace, m_toSpace);
    }

    // only starts incremental collection; roots forwarding
    void collect()
    {
        ++m_stats.collections;

        m_isCollecting = true;
        m_scan = m_next = 0;
        m_limit = m_end;
        
        for (auto& root : m_permanentRoots) { *root.slot = forwardImpl(*root.slot); }
        for (auto& root : m_dynamicRoots) { *root.slot = forwardImpl(*root.slot); }
    }

    // executes before every new allocation as a part of incremental collection
    void collectionStep()
    {
        if (!m_isCollecting) { return; }

        auto scansLeft = m_scansPerStep;
        while (m_scan < m_next && scansLeft --> 0) { scanOne(); }

        if (m_scan == m_next)
        {
            m_isCollecting = false;
            std::swap(m_fromSpace, m_toSpace);
        }
    }

    bool thresholdReached() const noexcept
    {
        // during a collection old objects are copied to the beginning (m_next)
        // while new allocations consume the end (m_end - m_limit), so the
        // occupied part of from-space is their sum
        auto const occupied = m_next + (m_end - m_limit);
        return occupied * 100 >= m_end * m_collectionThreshhold;
    }

    StellaValue allocFrom(size_t objSize)
    {
        if (enoughSpace(objSize) && !thresholdReached())
        {
            auto allocated = reinterpret_cast<StellaValue>(m_fromSpace.get() + m_next);
            m_next += objSize;
            return allocated;
        }

        collect();
        return allocTo(objSize);
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

    size_t m_scansPerStep{1};
    size_t m_collectionThreshhold{100};
    bool m_isCollecting{};

    StellaGcStats& m_stats;
};

} // namespace gc
