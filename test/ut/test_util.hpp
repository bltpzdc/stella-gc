#pragma once

#include <gc/context.hpp>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace test
{

class abi_violation : public std::exception
{
public:
    explicit abi_violation(std::string message)
        : m_message{std::move(message)}
    {}

    char const* what() const noexcept override { return m_message.c_str(); }

private:
    std::string m_message;
};

class out_of_memory : public std::exception
{
public:
    explicit out_of_memory(size_t requested)
        : m_requested{requested}
    {}

    size_t requested() const noexcept { return m_requested; }

    char const* what() const noexcept override { return "collector out of memory"; }

private:
    size_t m_requested;
};

inline int& checks()
{
    static int value = 0;
    return value;
}

inline int& failures()
{
    static int value = 0;
    return value;
}

inline void report_failure(std::string const& message)
{
    ++failures();
    std::cerr << "FAIL " << message << '\n';
}

inline StellaGcStats make_stats()
{
    return StellaGcStats{};
}

inline StellaGcConfig make_config(size_t max_heap_bytes)
{
    StellaGcConfig config{};
    config.abi_version = STELLA_C_GC_ABI_VERSION;
    config.max_heap_bytes = max_heap_bytes;
    return config;
}

inline StellaObjectDescriptor make_desc(
    uint32_t kind = STELLA_OBJECT_DATA,
    uint32_t flags = 0,
    uint32_t entry_arity = 0,
    size_t managed = 0,
    size_t primitive = 0,
    size_t slot_capacity = 0,
    StellaEntryCode entry_code = nullptr,
    char const* debug_name = "test-object")
{
    StellaObjectDescriptor desc{};
    desc.abi_version = STELLA_C_GC_ABI_VERSION;
    desc.kind = kind;
    desc.flags = flags;
    desc.entry_arity = entry_arity;
    desc.managed_count = managed;
    desc.primitive_count = primitive;
    desc.slot_capacity = slot_capacity == 0 ? managed + primitive : slot_capacity;
    desc.entry_code = entry_code;
    desc.debug_name = debug_name;
    return desc;
}

inline size_t requested_size(StellaObjectDescriptor const& desc)
{
    return gc::utils::object_size(&desc);
}

inline size_t rounded_size(StellaObjectDescriptor const& desc)
{
    return gc::utils::round_object_size(gc::utils::object_size(&desc));
}

inline void noop_entry(StellaValue*, StellaValue*, size_t, StellaValue* const*)
{
}

inline StellaValue alloc_simple(
    gc::Context& ctx,
    uint32_t kind = STELLA_OBJECT_DATA,
    uint32_t flags = 0,
    uint32_t entry_arity = 0,
    size_t managed = 0,
    size_t primitive = 0,
    size_t slot_capacity = 0,
    StellaEntryCode entry_code = nullptr)
{
    auto const desc = make_desc(kind, flags, entry_arity, managed, primitive, slot_capacity, entry_code);
    return ctx.alloc(&desc);
}

inline void* outside_address(gc::Heap const& heap, size_t heap_bytes)
{
    auto const base = reinterpret_cast<uintptr_t>(heap.fromBegin());
    return reinterpret_cast<void*>(base + 2 * heap_bytes + 16);
}

} // namespace test

extern "C" [[noreturn]] void stella_abi_violation(const char* message)
{
    throw test::abi_violation{message};
}

extern "C" [[noreturn]] void stella_gc_out_of_memory(size_t requested_bytes)
{
    throw test::out_of_memory{requested_bytes};
}

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++test::checks();                                                  \
        if (!(cond)) {                                                     \
            test::report_failure(std::string{__FILE__} + ":" +             \
                std::to_string(__LINE__) + ": " + #cond);                  \
        }                                                                  \
    } while (0)

#define CHECK_VIOLATION(expr, expected)                                    \
    do {                                                                   \
        ++test::checks();                                                  \
        bool caught = false;                                               \
        try { expr; }                                                      \
        catch (test::abi_violation const& e) {                             \
            caught = true;                                                 \
            if (std::string{e.what()} != expected) {                       \
                test::report_failure(std::string{__FILE__} + ":" +         \
                    std::to_string(__LINE__) + ": " #expr " threw '" +     \
                    std::string{e.what()} + "'");                          \
            }                                                              \
        }                                                                  \
        if (!caught) {                                                     \
            test::report_failure(std::string{__FILE__} + ":" +             \
                std::to_string(__LINE__) + ": " #expr " did not violate"); \
        }                                                                  \
    } while (0)

#define CHECK_OOM(expr)                                                    \
    do {                                                                   \
        ++test::checks();                                                  \
        bool caught = false;                                               \
        try { expr; }                                                      \
        catch (test::out_of_memory const&) {                               \
            caught = true;                                                 \
        }                                                                  \
        if (!caught) {                                                     \
            test::report_failure(std::string{__FILE__} + ":" +             \
                std::to_string(__LINE__) + ": " #expr                      \
                " did not run out of memory");                             \
        }                                                                  \
    } while (0)
