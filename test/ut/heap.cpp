#include <gc/context.hpp>

#include <iostream>

#include "test_util.hpp"

namespace
{

using test::alloc_simple;
using test::make_config;
using test::make_desc;
using test::rounded_size;

void init_allocates_both_spaces()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();

    CHECK(heap.fromBegin() == nullptr);
    CHECK(heap.toBegin() == nullptr);
    CHECK(heap.fromNext() == 0);
    CHECK(heap.limit() == 0);
    CHECK(!heap.isCollecting());

    heap.init(make_config(1000));

    CHECK(heap.fromBegin() != nullptr);
    CHECK(heap.toBegin() != nullptr);
    CHECK(heap.fromBegin() != heap.toBegin());
    CHECK(heap.fromNext() == 0);
    CHECK(heap.limit() == 1000);
    CHECK(!heap.isCollecting());
    CHECK(heap.inFromSpace(heap.fromBegin()));
    CHECK(heap.inToSpace(heap.toBegin()));
    CHECK(!heap.inFromSpace(heap.toBegin()));
    CHECK(!heap.inToSpace(heap.fromBegin()));
    CHECK(heap.owns(heap.fromBegin()));
    CHECK(heap.owns(heap.toBegin()));
    CHECK(!heap.owns(test::outside_address(heap, 1000)));
}

void init_boundary_is_inclusive()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(1000));

    auto const from_end = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(heap.fromBegin()) + 1000);
    auto const to_end = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(heap.toBegin()) + 1000);

    CHECK(heap.inFromSpace(from_end));
    CHECK(heap.inToSpace(to_end));

    auto const beyond_from = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(heap.fromBegin()) + 1001);
    CHECK(!heap.inFromSpace(beyond_from));
    CHECK(!heap.owns(beyond_from));
}

void init_rejects_zero_and_reinitialization()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();

    heap.init(make_config(512));
    CHECK_VIOLATION(heap.init(make_config(512)), "re-initialization of heap requested");

    StellaGcStats stats2 = test::make_stats();
    gc::Context ctx2{stats2};
    CHECK_VIOLATION(ctx2.heap().init(make_config(0)), "zero-length heap size requested");
}

void alloc_places_objects_consecutively()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);
    StellaValue first = heap.alloc(&desc);
    StellaValue second = heap.alloc(&desc);
    auto const second_addr = reinterpret_cast<StellaValue>(
        reinterpret_cast<uintptr_t>(heap.fromBegin()) + rounded_size(desc));

    CHECK(first == heap.fromBegin());
    CHECK(second == second_addr);
    CHECK(heap.fromNext() == 2 * rounded_size(desc));
    CHECK(heap.owns(first));
    CHECK(heap.owns(second));
    CHECK(heap.inFromSpace(first));
    CHECK(!heap.inToSpace(first));
}

void alloc_grows_accounting()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 2, 1, 4);
    heap.alloc(&desc);

    CHECK(stats.requested_bytes == test::requested_size(desc));
    CHECK(stats.rounded_bytes == rounded_size(desc));
    CHECK(stats.allocated_objects == 1);
    CHECK(stats.occupied_bytes == heap.fromNext());
    CHECK(stats.maximum_occupied_bytes == heap.fromNext());
    CHECK(stats.collections == 0);
}

void forward_is_identity_outside_collection()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(512));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);
    StellaValue obj = heap.alloc(&desc);

    CHECK(heap.forward(obj) == obj);
    CHECK(heap.forward(nullptr) == nullptr);
    CHECK(stats.read_barrier_activations == 0);
}

void collection_preserves_reachable_graph()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(1024));

    auto const x_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    auto const y_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);

    auto const old_from = heap.fromBegin();
    auto const old_to = heap.toBegin();

    StellaValue slot_x = nullptr;
    ctx.pushDynamicRoot(&slot_x);

    slot_x = heap.alloc(&x_desc);
    StellaValue x_old = slot_x;
    StellaValue y_old = heap.alloc(&y_desc);
    ctx.initManaged(x_old, 0, y_old);

    StellaValue garbage = nullptr;
    for (int i = 0; i < 30; ++i) { garbage = heap.alloc(&y_desc); }

    while (stats.collections == 0) { heap.alloc(&y_desc); }

    CHECK(stats.collections == 1);
    while (heap.isCollecting()) { heap.alloc(&y_desc); }

    CHECK(!heap.isCollecting());
    CHECK(stats.collections == 1);
    CHECK(heap.fromBegin() == old_to);
    CHECK(heap.toBegin() == old_from);

    StellaValue x_new = slot_x;
    CHECK(x_new != x_old);
    CHECK(heap.inFromSpace(x_new));
    CHECK(!heap.inFromSpace(x_old));

    StellaValue y_new = x_new->fields[0].managed;
    CHECK(y_new != y_old);
    CHECK(heap.inFromSpace(y_new));

    CHECK(x_old->gc_word == reinterpret_cast<uintptr_t>(x_new));
    CHECK(y_old->gc_word == reinterpret_cast<uintptr_t>(y_new));

    CHECK(garbage->gc_word == 0);
    CHECK(!heap.inFromSpace(garbage));
    CHECK(heap.owns(garbage));

    CHECK(stats.read_barrier_activations == 0);
}

void collection_is_incremental()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(256));

    auto const x_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    auto const y_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);

    auto const old_to = heap.toBegin();

    StellaValue slot_x = nullptr;
    ctx.pushDynamicRoot(&slot_x);

    slot_x = heap.alloc(&x_desc);
    StellaValue x_old = slot_x;
    StellaValue y_old = heap.alloc(&y_desc);
    ctx.initManaged(x_old, 0, y_old);

    for (int i = 0; i < 6; ++i) { heap.alloc(&y_desc); }
    CHECK(stats.collections == 0);

    StellaValue during = heap.alloc(&y_desc);
    CHECK(stats.collections == 1);
    CHECK(heap.isCollecting());
    CHECK(heap.fromNext() == rounded_size(x_desc));
    CHECK(heap.limit() == 256 - rounded_size(y_desc));
    CHECK(heap.inToSpace(during));
    CHECK(during->gc_word == 0);

    StellaValue y_new = heap.forward(y_old);
    CHECK(y_new != y_old);
    CHECK(heap.inToSpace(y_new));
    CHECK(y_old->gc_word == reinterpret_cast<uintptr_t>(y_new));
    CHECK(heap.forward(y_old) == y_new);
    CHECK(heap.forward(y_new) == y_new);

    StellaValue x_new = slot_x;
    CHECK(heap.forward(x_old) == x_new);
    CHECK(heap.forward(nullptr) == nullptr);

    auto const foreign = reinterpret_cast<StellaValue>(
        reinterpret_cast<uintptr_t>(heap.fromBegin()) + 4096);
    CHECK(heap.forward(foreign) == foreign);

    CHECK(stats.read_barrier_activations == 6);

    heap.alloc(&y_desc);
    CHECK(heap.isCollecting());
    CHECK(heap.fromNext() == rounded_size(x_desc) + rounded_size(y_desc));

    heap.alloc(&y_desc);
    CHECK(!heap.isCollecting());
    CHECK(heap.fromBegin() == old_to);
    CHECK(heap.fromNext() == rounded_size(x_desc) + rounded_size(y_desc) + rounded_size(y_desc));
    CHECK(heap.inFromSpace(x_new));
    CHECK(heap.inFromSpace(y_new));

    CHECK(during->descriptor == &y_desc);
}

void collection_keeps_objects_allocated_during_collection()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(256));

    auto const x_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);

    StellaValue slot_x = nullptr;
    ctx.pushDynamicRoot(&slot_x);
    slot_x = heap.alloc(&x_desc);

    for (int i = 0; i < 7; ++i) { heap.alloc(&x_desc); }
    CHECK(stats.collections == 0);

    StellaValue during = heap.alloc(&x_desc);
    CHECK(heap.isCollecting());
    CHECK(heap.limit() == 256 - rounded_size(x_desc));

    heap.alloc(&x_desc);
    CHECK(!heap.isCollecting());
    CHECK(heap.inFromSpace(during));
    CHECK(during->descriptor == &x_desc);
}

void oom_when_live_objects_do_not_fit_semispace()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    auto& heap = ctx.heap();
    heap.init(make_config(64));

    auto const x_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    auto const y_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);

    StellaValue slot_x = nullptr;
    ctx.pushDynamicRoot(&slot_x);

    slot_x = heap.alloc(&x_desc);
    StellaValue y_old = heap.alloc(&y_desc);
    ctx.initManaged(slot_x, 0, y_old);

    heap.alloc(&y_desc);
    CHECK(stats.collections == 1);
    CHECK(heap.isCollecting());

    bool caught = false;
    size_t requested = 0;
    try {
        heap.alloc(&y_desc);
    } catch (test::out_of_memory const& e) {
        caught = true;
        requested = e.requested();
    }

    CHECK(caught);
    CHECK(requested == rounded_size(y_desc));
    CHECK(stats.failed_allocations == 1);
}

void run_all()
{
    init_allocates_both_spaces();
    init_boundary_is_inclusive();
    init_rejects_zero_and_reinitialization();
    alloc_places_objects_consecutively();
    alloc_grows_accounting();
    forward_is_identity_outside_collection();
    collection_preserves_reachable_graph();
    collection_is_incremental();
    collection_keeps_objects_allocated_during_collection();
    oom_when_live_objects_do_not_fit_semispace();
}

} // namespace

int main()
{
    run_all();

    std::cout << test::checks() << " checks, " << test::failures() << " failures\n";
    return test::failures() == 0 ? 0 : 1;
}