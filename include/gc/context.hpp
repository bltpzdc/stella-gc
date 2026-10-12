#pragma once

#include "bridge.hpp"
#include "heap.hpp"
#include "root.hpp"

#include <vector>

namespace gc
{

class Context
{
public:
    explicit Context(StellaGcStats& stats)
        : m_heap{m_dynamicRoots, m_permanentRoots, stats}
        , m_gcStats{stats}
    {}

    void cleanup() noexcept
    {
        if (m_dynamicRoots.size() != 0) { stella_abi_violation("runtime shutdown with dynamic roots registered"); }

        m_dynamicRoots.clear();
        m_permanentRoots.clear();
    }

    bool isRegisteredRoot(StellaValue* root) const
    {
        return std::ranges::contains(m_dynamicRoots, gc::root{root})
            || std::ranges::contains(m_permanentRoots, gc::root{root});
    }

    void pushDynamicRoot(StellaValue* root)
    {
        pushRootImpl(m_dynamicRoots, root, [this](auto& stats) {
            stats.dynamic_root_depth = m_dynamicRoots.size();
            stats.maximum_dynamic_root_depth = std::max(stats.maximum_dynamic_root_depth, stats.dynamic_root_depth); }
        );
    }

    void popDynamicRoot(StellaValue* root)
    {
        if (m_dynamicRoots.size() == 0) { stella_abi_violation("dynamic root stack underflow"); }
        if (m_dynamicRoots.back() != gc::root{root}) { stella_abi_violation("dynamic roots popped out of order"); }
        
        m_dynamicRoots.pop_back();
        m_gcStats.dynamic_root_depth = m_dynamicRoots.size();
    }

    void pushPermanentRoot(StellaValue* root)
    {
        pushRootImpl(m_permanentRoots, root, [this](auto& stats) { stats.permanent_root_count = m_permanentRoots.size(); });
    }

    void printRoots(FILE* output) const
    {
        fprintf(output, "Dynamic roots:\n%s\n", fmt::format("{}", fmt::join(m_dynamicRoots, "\n")).c_str());
        fprintf(output, "Permanent roots:\n%s\n", fmt::format("{}", fmt::join(m_permanentRoots, "\n")).c_str());
    }

    StellaValue alloc(StellaObjectDescriptor const* desc)
    {
        validateDesc(desc);
        if (desc->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC) { stella_abi_violation("static object allocation requested"); }

        return m_heap.alloc(desc);
    }

    void initManaged(StellaValue obj, size_t idx, StellaValue value)
    {
        validateDynamicObject(obj);
        if (idx >= obj->descriptor->managed_count) { stella_abi_violation("invalid index of initializating managed field"); }

        obj->fields[idx].managed = value;
    }

    void initPrimitive(StellaValue obj, size_t idx, StellaPrimitive value)
    {
        validateDynamicObject(obj);
        if (idx >= obj->descriptor->primitive_count) { stella_abi_violation("invalid index of initializating primitive field"); }

        auto const real = obj->descriptor->managed_count + idx;
        obj->fields[real].primitive = value;
    }

    StellaValue readManaged(root root, size_t idx)
    {
        ++m_gcStats.managed_reads;

        validateRoot(root.slot);
        auto obj = *root.slot;
        validateAnyObject(obj);
        if (idx >= obj->descriptor->managed_count) { stella_abi_violation("invalid index of reading managed field"); }
        auto& field = obj->fields[idx].managed;
        field = m_heap.forward(field); // managed read barrier

        return field;
    }

    void writeManaged(root target, size_t idx, root value)
    {
        ++m_gcStats.managed_writes;

        validateRoot(target.slot);
        validateRoot(value.slot);
        auto& obj = *target.slot;
        validateAnyObject(obj);

        if (obj->descriptor->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC) { stella_abi_violation("writing managed in static object"); }
        if (idx >= obj->descriptor->managed_count) { stella_abi_violation("invalid index of writing managed field"); }

        obj->fields[idx].managed = *value.slot; // no write managed barriers in Baker's algorithm
    }

    StellaPrimitive readPrimitive(StellaValue obj, size_t idx)
    {
        ++m_gcStats.primitive_reads;

        validateAnyObject(obj);
        if (idx >= obj->descriptor->primitive_count) { stella_abi_violation("invalid index of reading primitive field"); }

        auto const real = obj->descriptor->managed_count + idx;
        return obj->fields[real].primitive;
    }

    void writePrimitive(StellaValue target, size_t idx, StellaPrimitive value)
    {
        ++m_gcStats.primitive_writes;

        validateAnyObject(target);
        if (target->descriptor->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC) { stella_abi_violation("writing primitive in static object"); }
        if (idx >= target->descriptor->primitive_count) { stella_abi_violation("invalid index of writing primitive field"); }

        auto const real = target->descriptor->managed_count + idx;
        target->fields[real].primitive = value;
    }

    void updateObject(root objectRoot, std::vector<root> managedRoots, std::vector<StellaPrimitive> const& newPrimitives, StellaObjectDescriptor const* newDescriptor)
    {
        validateRoot(objectRoot.slot);
        auto& obj = *objectRoot.slot;
        validateDynamicObject(obj);
        validateDesc(newDescriptor);
        auto const& oldDescriptor = obj->descriptor;
        if ((oldDescriptor->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_UPDATABLE) == 0
            || (newDescriptor->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC) != 0) { stella_abi_violation("incompatible object-layout update flags"); }
        if (oldDescriptor->slot_capacity != newDescriptor->slot_capacity) { stella_abi_violation("object-layout update changes slot capacity"); }
        for (auto const& managedRoot : managedRoots) { validateRoot(managedRoot.slot); }

        // no write barriers since Baker's algorithm doesn't have them
        for (size_t i = 0; i < newDescriptor->managed_count; ++i) {
            obj->fields[i].managed = *managedRoots[i].slot;
            ++m_gcStats.managed_writes;
        }
        for (size_t i = 0; i < newDescriptor->primitive_count; ++i) {
            auto const real = newDescriptor->managed_count + i;
            obj->fields[real].primitive = newPrimitives[i];
            ++m_gcStats.primitive_writes;
        }
        for (size_t i = newDescriptor->managed_count + newDescriptor->primitive_count; i < newDescriptor->slot_capacity; ++i) {
            obj->fields[i].primitive = 0;
        }
        obj->descriptor = newDescriptor;
    }

    Heap& heap() noexcept { return m_heap; }
    Heap const& heap() const noexcept { return m_heap; }

private:
    void validateRoot(StellaValue* root)
    {
        if (!root || !isRegisteredRoot(root)) { stella_abi_violation("invalid or unregistered object root"); }
    }

    void validateDesc(StellaObjectDescriptor const* desc)
    {
        if (!desc) { stella_abi_violation("null object descriptor"); }
        if (desc->abi_version != STELLA_C_GC_ABI_VERSION) { stella_abi_violation("invalid ABI version in object descriptor"); }

        if (auto const kind = desc->kind; kind < StellaObjectKind::STELLA_OBJECT_DATA || kind > StellaObjectKind::STELLA_OBJECT_INDIRECTION) { stella_abi_violation("invalid kind in object descriptor"); }
        auto const isExecutable = desc->kind == StellaObjectKind::STELLA_OBJECT_CLOSURE || desc->kind == StellaObjectKind::STELLA_OBJECT_THUNK;
        if (isExecutable && !desc->entry_code) { stella_abi_violation("no entry code in executable object"); }
        if (!isExecutable && desc->entry_code) { stella_abi_violation("entry code in non-executabel object"); }
        if (desc->kind != StellaObjectKind::STELLA_OBJECT_CLOSURE && desc->entry_arity != 0) { stella_abi_violation("non-zero entry arity in non-closure object"); }

        if (auto const flags = desc->flags; flags > (StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC | StellaDescriptorFlags::STELLA_DESCRIPTOR_UPDATABLE)) { stella_abi_violation("invalid flags in object descriptor"); }
        auto const isStatic = desc->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC;
        auto const isUpdatable = desc->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_UPDATABLE;
        if (isStatic && isUpdatable) { stella_abi_violation("updatable static object"); }

        auto const m = desc->managed_count;
        auto const p = desc->primitive_count;
        auto const c = desc->slot_capacity;
        if (utils::add_overflow(m, p)) { stella_abi_violation("overflow fields count in object"); }
        if (m + p > c) { stella_abi_violation("managed and primitive fields count bigger that slot capacity"); }
    }

    void validateObjectCommon(StellaValue object)
    {
        if (!object) { stella_abi_violation("nullable object"); }
        validateDesc(object->descriptor);
    }

    void validateDynamicObject(StellaValue object)
    {
        validateObjectCommon(object);
        if (!m_heap.owns(object)) { stella_abi_violation("dynamic object not in heap"); }
    }

    void validateAnyObject(StellaValue object)
    {
        validateObjectCommon(object);
        auto const isStatic = object->descriptor->flags & StellaDescriptorFlags::STELLA_DESCRIPTOR_STATIC;
        if (!isStatic && !m_heap.owns(object)) { stella_abi_violation("object from invalid space"); }
    }

    void pushRootImpl(roots_t& roots, StellaValue* root, auto incMetric)
    {
        if (!root) { stella_abi_violation("null root slot"); }
        if (isRegisteredRoot(root)) { stella_abi_violation("duplicate root registration"); }

        try { roots.emplace_back(root); }
        catch (std::bad_alloc const&) { stella_abi_violation("root stack exhaustion"); }
        catch (std::length_error const&) { stella_abi_violation("dynamic roots capacity overflows size_t"); }

        incMetric(m_gcStats);
    }

    roots_t m_dynamicRoots;
    roots_t m_permanentRoots;

    Heap m_heap;

    StellaGcStats& m_gcStats;
};

} // namespace gc
