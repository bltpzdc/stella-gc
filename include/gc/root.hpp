#pragma once

#include <stella/runtime.h>

#include <fmt/format.h>

#include <vector>

namespace gc
{

struct root
{
    explicit root(StellaValue* slot) noexcept
        : slot{slot}
    {}

    auto operator<=>(root const&) const noexcept = default;

    StellaValue* slot{};
};

using roots_t = std::vector<root>;

} // namespace gc

template <>
struct fmt::formatter<gc::root> : formatter<std::string_view>
{
    template <typename FORMAT_CONTEXT>
    auto format(gc::root root, FORMAT_CONTEXT& ctx) const -> decltype(ctx.out())
    {
        return root.slot 
            ? fmt::format_to(ctx.out(), "Root[slot={}, value={}]", static_cast<void const*>(root.slot), static_cast<void const*>(*root.slot))
            : fmt::format_to(ctx.out(), "Root[slot=nullptr, value=none]");
    }
};
