#include <gc/context.hpp>

#include <cstring>
#include <iostream>

#include "test_util.hpp"

namespace
{

using test::alloc_simple;
using test::make_config;
using test::make_desc;
using test::rounded_size;

void alloc_initializes_object_and_stats()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 2, 1);
    StellaValue obj = ctx.alloc(&desc);

    CHECK(obj != nullptr);
    CHECK(obj->descriptor == &desc);
    CHECK(obj->gc_word == 0);
    CHECK(obj->fields != nullptr);
    CHECK(obj->fields[0].managed == nullptr);
    CHECK(obj->fields[1].managed == nullptr);
    CHECK(obj->fields[2].primitive == 0);
    CHECK(stats.allocated_objects == 1);
    CHECK(stats.requested_bytes == test::requested_size(desc));
    CHECK(stats.rounded_bytes == rounded_size(desc));
    CHECK(stats.occupied_bytes == rounded_size(desc));
    CHECK(stats.maximum_occupied_bytes == rounded_size(desc));
    CHECK(ctx.heap().fromNext() == rounded_size(desc));
}

void alloc_without_slots_has_null_fields()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);
    StellaValue obj = ctx.alloc(&desc);

    CHECK(obj == ctx.heap().fromBegin());
    CHECK(obj->fields == nullptr);
    CHECK(ctx.heap().fromNext() == rounded_size(desc));
}

void allocation_accounting_is_cumulative()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const small = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);
    auto const big = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0, 2);

    ctx.alloc(&small);
    CHECK(stats.allocated_objects == 1);
    CHECK(stats.requested_bytes == test::requested_size(small));
    CHECK(stats.rounded_bytes == rounded_size(small));

    StellaValue obj = ctx.alloc(&big);
    CHECK(stats.allocated_objects == 2);
    CHECK(stats.requested_bytes == test::requested_size(small) + test::requested_size(big));
    CHECK(stats.rounded_bytes == rounded_size(small) + rounded_size(big));
    CHECK(stats.occupied_bytes == ctx.heap().fromNext());
    CHECK(obj->fields == reinterpret_cast<StellaSlot*>(obj + 1));
}

void alloc_rejects_null_descriptor()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    CHECK_VIOLATION(ctx.alloc(nullptr), "null object descriptor");
}

void alloc_rejects_invalid_abi_version()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto desc = make_desc();
    desc.abi_version = 0;
    CHECK_VIOLATION(ctx.alloc(&desc), "invalid ABI version in object descriptor");
}

void alloc_rejects_invalid_kind()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto desc = make_desc();
    desc.kind = 0;
    CHECK_VIOLATION(ctx.alloc(&desc), "invalid kind in object descriptor");

    desc.kind = 5;
    CHECK_VIOLATION(ctx.alloc(&desc), "invalid kind in object descriptor");
}

void alloc_rejects_executable_without_entry_code()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto thunk = make_desc(STELLA_OBJECT_THUNK);
    CHECK_VIOLATION(ctx.alloc(&thunk), "no entry code in executable object");

    auto closure = make_desc(STELLA_OBJECT_CLOSURE);
    CHECK_VIOLATION(ctx.alloc(&closure), "no entry code in executable object");
}

void alloc_rejects_entry_code_in_non_executable()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const data = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0, 0, test::noop_entry);
    CHECK_VIOLATION(ctx.alloc(&data), "entry code in non-executabel object");

    auto const indirection = make_desc(STELLA_OBJECT_INDIRECTION, 0, 0, 0, 0, 0, test::noop_entry);
    CHECK_VIOLATION(ctx.alloc(&indirection), "entry code in non-executabel object");
}

void alloc_rejects_nonzero_arity_outside_closure()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const data = make_desc(STELLA_OBJECT_DATA, 0, 1);
    CHECK_VIOLATION(ctx.alloc(&data), "non-zero entry arity in non-closure object");

    auto const thunk = make_desc(STELLA_OBJECT_THUNK, 0, 1, 0, 0, 0, test::noop_entry);
    CHECK_VIOLATION(ctx.alloc(&thunk), "non-zero entry arity in non-closure object");

    auto const closure = make_desc(STELLA_OBJECT_CLOSURE, 0, 1, 0, 0, 0, test::noop_entry);
    CHECK(ctx.alloc(&closure) != nullptr);
}

void alloc_rejects_invalid_flags()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto desc = make_desc();
    desc.flags = STELLA_DESCRIPTOR_STATIC | STELLA_DESCRIPTOR_UPDATABLE | 4;
    CHECK_VIOLATION(ctx.alloc(&desc), "invalid flags in object descriptor");

    desc.flags = STELLA_DESCRIPTOR_STATIC | STELLA_DESCRIPTOR_UPDATABLE;
    CHECK_VIOLATION(ctx.alloc(&desc), "updatable static object");
}

void alloc_rejects_field_count_overflow()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto desc = make_desc();
    desc.managed_count = SIZE_MAX;
    desc.primitive_count = 1;
    CHECK_VIOLATION(ctx.alloc(&desc), "overflow fields count in object");
}

void alloc_rejects_fields_beyond_capacity()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 2, 2, 3);
    CHECK_VIOLATION(ctx.alloc(&desc),
                    "managed and primitive fields count bigger that slot capacity");
}

void alloc_rejects_static_descriptor()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_STATIC);
    CHECK_VIOLATION(ctx.alloc(&desc), "static object allocation requested");
}

void init_managed_and_primitive_fields()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 1);
    StellaValue obj = ctx.alloc(&desc);
    StellaValue value = alloc_simple(ctx);

    ctx.initManaged(obj, 0, value);
    ctx.initPrimitive(obj, 0, 42);

    CHECK(obj->fields[0].managed == value);
    CHECK(obj->fields[1].primitive == 42);
    CHECK(stats.managed_writes == 0);
    CHECK(stats.primitive_writes == 0);

    CHECK_VIOLATION(ctx.initManaged(obj, 1, value), "invalid index of initializating managed field");
    CHECK_VIOLATION(ctx.initPrimitive(obj, 1, 42), "invalid index of initializating primitive field");
}

void init_on_null_or_foreign_object_is_rejected()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 1);

    CHECK_VIOLATION(ctx.initManaged(nullptr, 0, nullptr), "nullable object");
    CHECK_VIOLATION(ctx.initPrimitive(nullptr, 0, 42), "nullable object");

    StellaObject foreign{};
    foreign.descriptor = &desc;
    CHECK_VIOLATION(ctx.initManaged(&foreign, 0, nullptr), "dynamic object not in heap");
    CHECK_VIOLATION(ctx.initPrimitive(&foreign, 0, 42), "dynamic object not in heap");
}

void read_write_managed_fields()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    StellaValue null_slot = nullptr;
    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;
    ctx.pushDynamicRoot(&slot_a);
    ctx.pushDynamicRoot(&slot_b);
    ctx.pushDynamicRoot(&null_slot);

    slot_a = ctx.alloc(&desc);
    slot_b = ctx.alloc(&desc);

    ctx.writeManaged(gc::root{&slot_a}, 0, gc::root{&slot_b});
    CHECK(slot_a->fields[0].managed == slot_b);
    CHECK(stats.managed_writes == 1);

    ctx.writeManaged(gc::root{&slot_a}, 0, gc::root{&null_slot});
    CHECK(slot_a->fields[0].managed == nullptr);
    CHECK(stats.managed_writes == 2);

    CHECK(ctx.readManaged(gc::root{&slot_a}, 0) == nullptr);
    CHECK(stats.managed_reads == 1);
    CHECK(stats.read_barrier_activations == 0);

    ctx.writeManaged(gc::root{&slot_a}, 0, gc::root{&slot_b});
    CHECK(ctx.readManaged(gc::root{&slot_a}, 0) == slot_b);
    CHECK(stats.managed_reads == 2);

    CHECK_VIOLATION(ctx.readManaged(gc::root{&slot_a}, 1), "invalid index of reading managed field");
    CHECK_VIOLATION(ctx.writeManaged(gc::root{&slot_a}, 1, gc::root{&slot_b}),
                    "invalid index of writing managed field");
}

void read_write_managed_require_registered_roots()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    StellaValue unregistered = ctx.alloc(&desc);
    StellaValue registered = nullptr;
    ctx.pushDynamicRoot(&registered);

    CHECK_VIOLATION(ctx.readManaged(gc::root{nullptr}, 0), "invalid or unregistered object root");
    CHECK_VIOLATION(ctx.readManaged(gc::root{&unregistered}, 0),
                    "invalid or unregistered object root");
    CHECK_VIOLATION(ctx.writeManaged(gc::root{&unregistered}, 0, gc::root{&registered}),
                    "invalid or unregistered object root");
    CHECK_VIOLATION(ctx.writeManaged(gc::root{&registered}, 0, gc::root{&unregistered}),
                    "invalid or unregistered object root");
}

void read_barrier_forwards_field_during_collection()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(256));

    auto const x_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0);
    auto const y_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 0);

    StellaValue slot_x = nullptr;
    ctx.pushDynamicRoot(&slot_x);

    slot_x = ctx.alloc(&x_desc);
    StellaValue x_old = slot_x;
    StellaValue y_old = ctx.alloc(&y_desc);
    ctx.initManaged(x_old, 0, y_old);

    for (int i = 0; i < 6; ++i) { ctx.alloc(&y_desc); }
    CHECK(stats.collections == 0);

    ctx.alloc(&y_desc);
    CHECK(stats.collections == 1);
    CHECK(ctx.heap().isCollecting());

    StellaValue x_new = slot_x;
    CHECK(x_new != x_old);
    CHECK(stats.read_barrier_activations == 0);
    CHECK(x_new->fields[0].managed == y_old);

    StellaValue y_new = ctx.readManaged(gc::root{&slot_x}, 0);

    CHECK(y_new != y_old);
    CHECK(y_new == x_new->fields[0].managed);
    CHECK(y_old->gc_word == reinterpret_cast<uintptr_t>(y_new));
    CHECK(stats.managed_reads == 1);
    CHECK(stats.read_barrier_activations == 1);
}

void read_primitive_and_write_primitive()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 1);
    StellaValue obj = ctx.alloc(&desc);

    CHECK(ctx.readPrimitive(obj, 0) == 0);
    CHECK(stats.primitive_reads == 1);

    ctx.writePrimitive(obj, 0, 777);
    CHECK(ctx.readPrimitive(obj, 0) == 777);
    CHECK(stats.primitive_reads == 2);
    CHECK(stats.primitive_writes == 1);

    CHECK_VIOLATION(ctx.readPrimitive(obj, 1), "invalid index of reading primitive field");
    CHECK_VIOLATION(ctx.writePrimitive(obj, 1, 777), "invalid index of writing primitive field");
    CHECK_VIOLATION(ctx.readPrimitive(nullptr, 0), "nullable object");
}

void static_objects_are_readonly()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const static_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_STATIC, 0, 1, 0);
    StellaObject static_object{};
    static_object.descriptor = &static_desc;
    static_object.fields = reinterpret_cast<StellaSlot*>(&static_object + 1);

    StellaValue slot_static = &static_object;
    StellaValue slot_value = nullptr;
    ctx.pushDynamicRoot(&slot_static);
    ctx.pushDynamicRoot(&slot_value);

    slot_value = alloc_simple(ctx);
    static_object.fields[0].managed = slot_value;

    CHECK(ctx.readManaged(gc::root{&slot_static}, 0) == slot_value);
    CHECK(stats.managed_reads == 1);

    CHECK_VIOLATION(ctx.writeManaged(gc::root{&slot_static}, 0, gc::root{&slot_value}),
                    "writing managed in static object");
    CHECK_VIOLATION(ctx.writePrimitive(&static_object, 0, 42),
                    "writing primitive in static object");
}

void primitives_reject_foreign_object()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 0, 1);
    StellaObject foreign{};
    foreign.descriptor = &desc;

    CHECK_VIOLATION(ctx.readPrimitive(&foreign, 0), "object from invalid space");
    CHECK_VIOLATION(ctx.writePrimitive(&foreign, 0, 42), "object from invalid space");
}

void update_object_replaces_layout()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const old_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 1, 1, 3);
    auto const new_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 2, 1, 3);

    StellaValue slot_obj = ctx.alloc(&old_desc);
    StellaValue slot_a = nullptr;
    StellaValue slot_b = nullptr;
    ctx.pushDynamicRoot(&slot_obj);
    ctx.pushDynamicRoot(&slot_a);
    ctx.pushDynamicRoot(&slot_b);
    slot_a = alloc_simple(ctx);
    slot_b = alloc_simple(ctx);

    std::vector<gc::root> managed_roots{gc::root{&slot_a}, gc::root{&slot_b}};
    std::vector<StellaPrimitive> primitives{42};

    ctx.updateObject(gc::root{&slot_obj}, managed_roots, primitives, &new_desc);

    CHECK(slot_obj->descriptor == &new_desc);
    CHECK(slot_obj->fields[0].managed == slot_a);
    CHECK(slot_obj->fields[1].managed == slot_b);
    CHECK(slot_obj->fields[2].primitive == 42);
    CHECK(stats.managed_writes == 2);
    CHECK(stats.primitive_writes == 1);
}

void update_object_with_spare_slots_clears_them()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const old_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 2, 0, 3);
    auto const new_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 1, 1, 3);

    StellaValue slot_obj = ctx.alloc(&old_desc);
    StellaValue slot_a = nullptr;
    ctx.pushDynamicRoot(&slot_obj);
    ctx.pushDynamicRoot(&slot_a);
    slot_a = alloc_simple(ctx);

    std::vector<gc::root> managed_roots{gc::root{&slot_a}};
    std::vector<StellaPrimitive> primitives{7};

    ctx.updateObject(gc::root{&slot_obj}, managed_roots, primitives, &new_desc);

    CHECK(slot_obj->descriptor == &new_desc);
    CHECK(slot_obj->fields[0].managed == slot_a);
    CHECK(slot_obj->fields[1].primitive == 7);
    CHECK(slot_obj->fields[2].primitive == 0);
}

void update_object_rejects_misuse()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    auto const updatable_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 1, 0, 2);
    auto const fixed_desc = make_desc(STELLA_OBJECT_DATA, 0, 0, 1, 0, 2);
    auto const static_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_STATIC, 0, 1, 0, 2);
    auto const resize_desc = make_desc(STELLA_OBJECT_DATA, STELLA_DESCRIPTOR_UPDATABLE, 0, 1, 0, 4);

    StellaValue slot_obj = ctx.alloc(&updatable_desc);
    StellaValue slot_fixed = ctx.alloc(&fixed_desc);
    StellaValue unregistered = nullptr;
    ctx.pushDynamicRoot(&slot_obj);
    ctx.pushDynamicRoot(&slot_fixed);

    CHECK_VIOLATION(ctx.updateObject(gc::root{&unregistered}, {}, {}, &updatable_desc),
                    "invalid or unregistered object root");
    CHECK_VIOLATION(ctx.updateObject(gc::root{&slot_fixed}, {}, {}, &updatable_desc),
                    "incompatible object-layout update flags");
    CHECK_VIOLATION(ctx.updateObject(gc::root{&slot_obj}, {}, {}, &static_desc),
                    "incompatible object-layout update flags");
    CHECK_VIOLATION(ctx.updateObject(gc::root{&slot_obj}, {}, {}, &resize_desc),
                    "object-layout update changes slot capacity");
    CHECK_VIOLATION(ctx.updateObject(gc::root{&slot_obj}, {}, {}, nullptr),
                    "null object descriptor");
    CHECK_VIOLATION(ctx.updateObject(gc::root{&slot_obj}, {gc::root{&unregistered}}, {}, &updatable_desc),
                    "invalid or unregistered object root");
}

void print_roots_writes_both_lists()
{
    StellaGcStats stats = test::make_stats();
    gc::Context ctx{stats};
    ctx.init(make_config(1024));

    FILE* output = std::tmpfile();
    CHECK(output != nullptr);

    ctx.printRoots(output);
    std::fflush(output);
    std::rewind(output);
    char buffer[256]{};
    size_t const n = std::fread(buffer, 1, sizeof(buffer) - 1, output);
    CHECK(n > 0);
    CHECK(std::strstr(buffer, "Dynamic roots:") != nullptr);
    CHECK(std::strstr(buffer, "Permanent roots:") != nullptr);

    StellaValue slot = nullptr;
    ctx.pushPermanentRoot(&slot);

    FILE* output_with_root = std::tmpfile();
    CHECK(output_with_root != nullptr);
    ctx.printRoots(output_with_root);
    std::fflush(output_with_root);
    std::rewind(output_with_root);
    char buffer_with_root[256]{};
    size_t const m = std::fread(buffer_with_root, 1, sizeof(buffer_with_root) - 1, output_with_root);
    CHECK(m > 0);
    CHECK(std::strstr(buffer_with_root, "Root[slot=") != nullptr);

    std::fclose(output);
    std::fclose(output_with_root);
}

void run_all()
{
    alloc_initializes_object_and_stats();
    alloc_without_slots_has_null_fields();
    allocation_accounting_is_cumulative();
    alloc_rejects_null_descriptor();
    alloc_rejects_invalid_abi_version();
    alloc_rejects_invalid_kind();
    alloc_rejects_executable_without_entry_code();
    alloc_rejects_entry_code_in_non_executable();
    alloc_rejects_nonzero_arity_outside_closure();
    alloc_rejects_invalid_flags();
    alloc_rejects_field_count_overflow();
    alloc_rejects_fields_beyond_capacity();
    alloc_rejects_static_descriptor();
    init_managed_and_primitive_fields();
    init_on_null_or_foreign_object_is_rejected();
    read_write_managed_fields();
    read_write_managed_require_registered_roots();
    read_barrier_forwards_field_during_collection();
    read_primitive_and_write_primitive();
    static_objects_are_readonly();
    primitives_reject_foreign_object();
    update_object_replaces_layout();
    update_object_with_spare_slots_clears_them();
    update_object_rejects_misuse();
    print_roots_writes_both_lists();
}

} // namespace

int main()
{
    run_all();

    std::cout << test::checks() << " checks, " << test::failures() << " failures\n";
    return test::failures() == 0 ? 0 : 1;
}