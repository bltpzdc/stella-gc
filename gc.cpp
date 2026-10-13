/* Заглушка сборщика мусора: полностью реализует интерфейс из stella/gc.h, проверяет аргументы,
 * считает статистику и соблюдает ограничение памяти, но освобождает память только при
 * завершении работы. Её можно взять за основу или заменить целиком.
 * Макросы GC_BEFORE_*, GC_WRITE_BARRIER и STELLA_GC_SUPPORT --- места расширения, которые
 * в самой заглушке ничего не делают. */


#include <cstdio>

#include <ranges>
#include <utility>
#include <vector>

#include <gc/context.hpp>

static StellaGcStats counters;
static gc::Context g_gc_context{counters};

void stella_gc_init(const StellaGcConfig *config) {
  if (config == NULL) {
    stella_abi_violation("null collector configuration");
  }
  if (config->abi_version != STELLA_C_GC_ABI_VERSION) {
    stella_abi_violation("incompatible collector ABI version");
  }

  gc::config cfg{*config};
  cfg.readEnvs();
  g_gc_context.init(cfg);
}

void stella_gc_shutdown(void) {
  g_gc_context.shutdown();
}

void stella_gc_push_root(StellaValue *root) {
  g_gc_context.pushDynamicRoot(root);
}

void stella_gc_pop_root(StellaValue *root) {
  g_gc_context.popDynamicRoot(root);
}

void stella_gc_register_permanent_root(StellaValue *root) {
  g_gc_context.pushPermanentRoot(root);
}

#ifdef STELLA_GC_SUPPORT
#include STELLA_GC_SUPPORT
#endif

#ifndef GC_BEFORE_ALLOC
#define GC_BEFORE_ALLOC(requested) ((void)0)
#define GC_BEFORE_READ(root) ((void)0)
#define GC_BEFORE_WRITE(root) ((void)0)
#define GC_BEFORE_UPDATE(root) ((void)0)
#define GC_WRITE_BARRIER(owner, value) ((void)0)
#define GC_DESCRIPTION "semispace baker collector"
#endif

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor) {
  return g_gc_context.alloc(descriptor);
}

void stella_object_init_managed(
    StellaValue object,
    size_t index,
    StellaValue value) {
  g_gc_context.initManaged(object, index, value);
}

void stella_object_init_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  g_gc_context.initPrimitive(object, index, value);
}

StellaValue stella_gc_read_managed(
    StellaValue *object_root,
    size_t index) {
  GC_BEFORE_READ(object_root);
  return g_gc_context.readManaged(gc::root{object_root}, index);
}

void stella_gc_write_managed(
    StellaValue *object_root,
    size_t index,
    StellaValue *value_root) {
  g_gc_context.writeManaged(gc::root{object_root}, index, gc::root{value_root});
}

StellaPrimitive stella_object_read_primitive(
    StellaValue object,
    size_t index) {
  return g_gc_context.readPrimitive(object, index);
}

void stella_object_write_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  g_gc_context.writePrimitive(object, index, value);
}

void stella_gc_update_object(
    StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]) {
  if (new_descriptor->managed_count != 0 && new_managed_roots == NULL) { stella_abi_violation("missing managed values for object-layout update"); }
  if (new_descriptor->primitive_count != 0 && new_primitives == NULL) { stella_abi_violation("missing primitive values for object-layout update"); }

  std::vector<gc::root> managedRoots;
  if (new_descriptor->managed_count != 0) {
    auto const managedRootsView = std::ranges::subrange{new_managed_roots, new_managed_roots + new_descriptor->managed_count}
        | std::views::transform([](StellaValue *slot) { return gc::root{slot}; });
    managedRoots = std::vector<gc::root>{managedRootsView.begin(), managedRootsView.end()};
  }

  std::vector<StellaPrimitive> primitives;
  if (new_descriptor->primitive_count != 0) {
    auto const primitivesView = std::ranges::subrange{new_primitives, new_primitives + new_descriptor->primitive_count}
        | std::views::transform([](StellaPrimitive value) { return value; });
    primitives = std::vector<StellaPrimitive>{primitivesView.begin(), primitivesView.end()};
  }

  g_gc_context.updateObject(gc::root{object_root}, std::move(managedRoots), std::move(primitives), new_descriptor);
}

void stella_gc_print_statistics(FILE *output) {
  g_gc_context.printStatistics(output);
}

void stella_gc_print_roots(FILE *output) {
  g_gc_context.printRoots(output);
}

void stella_gc_print_state(FILE *output) {
  g_gc_context.printState(output, GC_DESCRIPTION);
}
