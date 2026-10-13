#pragma once

#include "bridge.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <optional>

namespace gc
{

struct config
{
    uint32_t abi_version = STELLA_C_GC_ABI_VERSION;
    size_t max_heap_bytes = 0;
    size_t scansPerStep = 1;
    size_t collectionThreshhold = 100;

    constexpr config() = default;

    config(StellaGcConfig const& base) noexcept
        : abi_version{base.abi_version}
        , max_heap_bytes{base.max_heap_bytes}
    {}

    static config fromEnvironment()
    {
        config cfg;
        cfg.readEnvs();
        return cfg;
    }

    void readEnvs()
    {
        if (auto const value = readEnv("STELLA_GC_SCANS_PER_STEP")) { scansPerStep = *value; }
        if (auto const value = readEnv("STELLA_GC_COLLECTION_THRESHOLD")) { collectionThreshhold = *value; }
    }

private:
    static std::optional<size_t> readEnv(char const* name)
    {
        char const* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0') { return std::nullopt; }
        if (*raw == '-') { indicateBadValue(name); }

        errno = 0;
        char* end = nullptr;
        auto const value = std::strtoull(raw, &end, 10);
        if (errno == ERANGE || end == raw || *end != '\0' || value > SIZE_MAX) { indicateBadValue(name); }

        return static_cast<size_t>(value);
    }

    static void indicateBadValue(char const* name)
    {
        stella_abi_violation(fmt::format("invalid value of {} environment variable", name).c_str());
    }
};

} // namespace gc