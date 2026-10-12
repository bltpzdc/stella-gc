#include <gc/context.hpp>

#include <iostream>

#include <fmt/format.h>

#include "test_util.hpp"

namespace
{

void add_dynamic_and_permanent_roots()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;
    StellaValue slot_c = nullptr;

    ctx.pushDynamicRoot(&slot_a);
    CHECK(stats.dynamic_root_depth == 1);
    CHECK(stats.maximum_dynamic_root_depth == 1);
    CHECK(ctx.isRegisteredRoot(&slot_a));
    CHECK(!ctx.isRegisteredRoot(&slot_b));

    ctx.pushDynamicRoot(&slot_b);
    CHECK(stats.dynamic_root_depth == 2);
    CHECK(stats.maximum_dynamic_root_depth == 2);
    CHECK(ctx.isRegisteredRoot(&slot_a));
    CHECK(ctx.isRegisteredRoot(&slot_b));

    ctx.pushPermanentRoot(&slot_c);
    CHECK(stats.permanent_root_count == 1);
    CHECK(ctx.isRegisteredRoot(&slot_c));
}

void duplicate_add_is_rejected()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;

    ctx.pushDynamicRoot(&slot_a);
    CHECK_VIOLATION(ctx.pushDynamicRoot(&slot_a), "duplicate root registration");
    CHECK(stats.dynamic_root_depth == 1);

    ctx.pushPermanentRoot(&slot_b);
    CHECK_VIOLATION(ctx.pushPermanentRoot(&slot_b), "duplicate root registration");
    CHECK_VIOLATION(ctx.pushDynamicRoot(&slot_b), "duplicate root registration");
    CHECK(stats.permanent_root_count == 1);
    CHECK(stats.dynamic_root_depth == 1);
}

void pop_clears_roots()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;

    ctx.pushDynamicRoot(&slot_a);
    ctx.pushDynamicRoot(&slot_b);

    ctx.popDynamicRoot(&slot_b);
    CHECK(stats.dynamic_root_depth == 1);
    CHECK(!ctx.isRegisteredRoot(&slot_b));
    CHECK(ctx.isRegisteredRoot(&slot_a));

    ctx.popDynamicRoot(&slot_a);
    CHECK(stats.dynamic_root_depth == 0);
    CHECK(!ctx.isRegisteredRoot(&slot_a));
    CHECK(!ctx.isRegisteredRoot(&slot_b));
}

void add_after_pop_is_allowed()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;

    ctx.pushDynamicRoot(&slot_a);
    ctx.popDynamicRoot(&slot_a);

    ctx.pushDynamicRoot(&slot_a);
    CHECK(stats.dynamic_root_depth == 1);
    CHECK(ctx.isRegisteredRoot(&slot_a));
}

void pop_misuse_is_rejected()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;

    CHECK_VIOLATION(ctx.popDynamicRoot(&slot_a), "dynamic root stack underflow");

    ctx.pushDynamicRoot(&slot_a);
    ctx.pushDynamicRoot(&slot_b);
    CHECK_VIOLATION(ctx.popDynamicRoot(&slot_a), "dynamic roots popped out of order");
    CHECK(stats.dynamic_root_depth == 2);

    ctx.popDynamicRoot(&slot_b);
    ctx.popDynamicRoot(&slot_a);
    CHECK(stats.dynamic_root_depth == 0);
}

void null_root_is_rejected()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    CHECK_VIOLATION(ctx.pushDynamicRoot(nullptr), "null root slot");
    CHECK_VIOLATION(ctx.pushPermanentRoot(nullptr), "null root slot");
}

void cleanup_clears_roots()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};

    StellaValue slot_a = nullptr;

    ctx.pushPermanentRoot(&slot_a);
    CHECK(ctx.isRegisteredRoot(&slot_a));

    ctx.cleanup();

    CHECK(!ctx.isRegisteredRoot(&slot_a));
}

void root_formats_for_debugging()
{
    CHECK(fmt::format("{}", gc::root{nullptr}) == "Root[slot=nullptr, value=none]");

    StellaValue value = nullptr;
    gc::root r{&value};
    CHECK(fmt::format("{}", r) ==
          fmt::format("Root[slot={}, value={}]",
                      static_cast<void const*>(&value),
                      static_cast<void const*>(value)));
}

void run_all()
{
    add_dynamic_and_permanent_roots();
    duplicate_add_is_rejected();
    pop_clears_roots();
    add_after_pop_is_allowed();
    pop_misuse_is_rejected();
    null_root_is_rejected();
    cleanup_clears_roots();
    root_formats_for_debugging();
}

} // namespace

int main()
{
    run_all();

    std::cout << test::checks() << " checks, " << test::failures() << " failures\n";
    return test::failures() == 0 ? 0 : 1;
}