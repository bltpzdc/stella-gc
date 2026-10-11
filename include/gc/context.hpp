#pragma once

#include "bridge.hpp"
#include "root.hpp"

#include <vector>

namespace gc
{

struct context
{
    explicit context(StellaGcStats& stats)
        : m_gcStats{stats}
    {}

    void cleanup() noexcept
    {
        if (m_dynamicRoots.size() != 0) { stella_abi_violation("runtime shutdown with dynamic roots registered"); }

        m_dynamicRoots.clear();
        m_permanentRoots.clear();
    }

    using roots_t = std::vector<root>;

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

private:
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

    StellaGcStats& m_gcStats;
};

};
