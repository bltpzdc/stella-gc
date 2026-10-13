/* Заглушка сборщика мусора: полностью реализует интерфейс из stella/gc.h, проверяет аргументы,
 * считает статистику и соблюдает ограничение памяти, но освобождает память только при
 * завершении работы. Её можно взять за основу или заменить целиком.
 * Макросы GC_BEFORE_*, GC_WRITE_BARRIER и STELLA_GC_SUPPORT --- места расширения, которые
 * в самой заглушке ничего не делают. */


#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

#include <gc/context.hpp>

typedef struct Allocation Allocation;

struct Allocation {
  StellaValue object;
  size_t requested_bytes;
  size_t rounded_bytes;
  unsigned char *initialised;
#ifdef STELLA_GC_ALLOCATION_FIELDS
  STELLA_GC_ALLOCATION_FIELDS
#endif
  Allocation *next;
};

enum Lifecycle {
  LIFECYCLE_FRESH,
  LIFECYCLE_ACTIVE,
  LIFECYCLE_FINISHED
};

static enum Lifecycle lifecycle = LIFECYCLE_FRESH;
static size_t max_heap_bytes;
static Allocation *allocations;
static StellaGcStats counters;
static gc::Context g_gc_context{counters};

static void require_active(void) {
  if (lifecycle != LIFECYCLE_ACTIVE) {
    stella_abi_violation("collector operation outside its active lifetime");
  }
}

static bool add_overflows(size_t left, size_t right) {
  return left > SIZE_MAX - right;
}

static bool multiply_overflows(size_t left, size_t right) {
  return right != 0 && left > SIZE_MAX / right;
}

static size_t object_size(const StellaObjectDescriptor *descriptor) {
  if (multiply_overflows(descriptor->slot_capacity, sizeof(StellaSlot))) {
    stella_abi_violation("object slot capacity overflows size_t");
  }
  size_t slots = descriptor->slot_capacity * sizeof(StellaSlot);
  if (add_overflows(sizeof(StellaObject), slots)) {
    stella_abi_violation("object size overflows size_t");
  }
  return sizeof(StellaObject) + slots;
}

static size_t rounded_size(size_t size) {
  size_t alignment = alignof(max_align_t);
  size_t remainder = size % alignment;
  if (remainder == 0) {
    return size;
  }
  size_t padding = alignment - remainder;
  if (add_overflows(size, padding)) {
    stella_abi_violation("aligned object size overflows size_t");
  }
  return size + padding;
}

static void validate_descriptor(const StellaObjectDescriptor *descriptor) {
  if (descriptor == NULL) {
    stella_abi_violation("null object descriptor");
  }
  if (descriptor->abi_version != STELLA_C_GC_ABI_VERSION) {
    stella_abi_violation("incompatible descriptor ABI version");
  }
  if (descriptor->kind < STELLA_OBJECT_DATA ||
      descriptor->kind > STELLA_OBJECT_INDIRECTION) {
    stella_abi_violation("unknown object kind");
  }
  uint32_t known_flags = STELLA_DESCRIPTOR_STATIC |
                         STELLA_DESCRIPTOR_UPDATABLE;
  if ((descriptor->flags & ~known_flags) != 0) {
    stella_abi_violation("unknown descriptor flag");
  }
  if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0 &&
      (descriptor->flags & STELLA_DESCRIPTOR_UPDATABLE) != 0) {
    stella_abi_violation("static descriptor is marked updatable");
  }
  if (add_overflows(
          descriptor->managed_count,
          descriptor->primitive_count)) {
    stella_abi_violation("active field count overflows size_t");
  }
  if (descriptor->managed_count + descriptor->primitive_count >
      descriptor->slot_capacity) {
    stella_abi_violation("active fields exceed slot capacity");
  }
  if ((descriptor->kind == STELLA_OBJECT_CLOSURE ||
       descriptor->kind == STELLA_OBJECT_THUNK) &&
      descriptor->entry_code == NULL) {
    stella_abi_violation("executable descriptor has no entry code");
  }
  if ((descriptor->kind == STELLA_OBJECT_DATA ||
       descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
      descriptor->entry_code != NULL) {
    stella_abi_violation("non-executable descriptor has entry code");
  }
  if (descriptor->kind == STELLA_OBJECT_THUNK &&
      descriptor->entry_arity != 0) {
    stella_abi_violation("thunk descriptor has non-zero entry arity");
  }
  if ((descriptor->kind == STELLA_OBJECT_DATA ||
       descriptor->kind == STELLA_OBJECT_INDIRECTION) &&
      descriptor->entry_arity != 0) {
    stella_abi_violation("non-executable descriptor has non-zero entry arity");
  }
}

static Allocation *find_allocation(StellaValue object) {
  for (Allocation *allocation = allocations;
       allocation != NULL;
       allocation = allocation->next) {
    if (allocation->object == object) {
      return allocation;
    }
  }
  return NULL;
}

static bool heap_owns(StellaValue object) {
  return g_gc_context.heap().owns(object);
}

static Allocation *require_heap_object(StellaValue object) {
  if (object == NULL) {
    stella_abi_violation("null object");
  }
  validate_descriptor(object->descriptor);
  Allocation *allocation = find_allocation(object);
  if (allocation == NULL && !heap_owns(object)) {
    stella_abi_violation("object is not owned by the collector");
  }
  return allocation;
}

static void validate_object(StellaValue object) {
  if (object == NULL) {
    stella_abi_violation("null object");
  }
  validate_descriptor(object->descriptor);
  if ((object->descriptor->flags & STELLA_DESCRIPTOR_STATIC) == 0 &&
      find_allocation(object) == NULL && !heap_owns(object)) {
    stella_abi_violation("heap object is not owned by the collector");
  }
}

static void require_registered_root(StellaValue *root) {
  if (root == NULL || !g_gc_context.isRegisteredRoot(root)) {
    stella_abi_violation("field access uses an unregistered root");
  }
}

static void grow_roots(
    StellaValue ***roots,
    size_t *capacity,
    size_t required) {
  if (required <= *capacity) {
    return;
  }
  if (*capacity > SIZE_MAX / 2) {
    stella_abi_violation("root stack capacity overflows size_t");
  }
  size_t next = *capacity == 0 ? 8 : *capacity * 2;
  if (next < required || multiply_overflows(next, sizeof(**roots))) {
    stella_abi_violation("root stack capacity overflows size_t");
  }
  void *storage = realloc(*roots, next * sizeof(**roots));
  if (storage == NULL) {
    stella_abi_violation("root stack exhaustion");
  }
  *roots = (StellaValue **)storage;
  *capacity = next;
}

void stella_gc_init(const StellaGcConfig *config) {
  if (lifecycle != LIFECYCLE_FRESH) {
    stella_abi_violation("collector initialised more than once");
  }
  if (config == NULL) {
    stella_abi_violation("null collector configuration");
  }
  if (config->abi_version != STELLA_C_GC_ABI_VERSION) {
    stella_abi_violation("incompatible collector ABI version");
  }

  gc::config cfg{*config};
  cfg.readEnvs();
  if (cfg.max_heap_bytes == 0) {
    stella_abi_violation("zero maximum heap size");
  }
  g_gc_context.heap().init(cfg);
  max_heap_bytes = cfg.max_heap_bytes;
  lifecycle = LIFECYCLE_ACTIVE;
}

void stella_gc_shutdown(void) {
  require_active();

  while (allocations != NULL) {
    Allocation *next = allocations->next;
    free(allocations->initialised);
    free(allocations);
    allocations = next;
  }
  g_gc_context.cleanup();
  counters.occupied_bytes = 0;
  lifecycle = LIFECYCLE_FINISHED;
}

void stella_gc_push_root(StellaValue *root) {
  require_active();
  g_gc_context.pushDynamicRoot(root);
}

void stella_gc_pop_root(StellaValue *root) {
  require_active();
  g_gc_context.popDynamicRoot(root);
}

void stella_gc_register_permanent_root(StellaValue *root) {
  require_active();
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
#define GC_ALLOCATION_INIT(allocation) ((void)0)
#define GC_DESCRIPTION "malloc stub"
#endif

StellaValue stella_gc_alloc(const StellaObjectDescriptor *descriptor) {
  require_active();
  validate_descriptor(descriptor);
  if ((descriptor->flags & STELLA_DESCRIPTOR_STATIC) != 0) {
    stella_abi_violation("heap allocation requested with static descriptor");
  }
  size_t requested = object_size(descriptor);
  size_t rounded = rounded_size(requested);
  GC_BEFORE_ALLOC(requested);

  StellaValue object = g_gc_context.heap().alloc(descriptor);
  Allocation *allocation = (Allocation *)malloc(sizeof(*allocation));
  unsigned char *initialised = NULL;
  if (descriptor->slot_capacity != 0) {
    initialised = (unsigned char *)calloc(descriptor->slot_capacity, sizeof(*initialised));
  }
  if (allocation == NULL ||
      (descriptor->slot_capacity != 0 && initialised == NULL)) {
    free(allocation);
    free(initialised);
    ++counters.failed_allocations;
    stella_gc_out_of_memory(requested);
  }

  allocation->object = object;
  allocation->requested_bytes = requested;
  allocation->rounded_bytes = rounded;
  allocation->initialised = initialised;
  GC_ALLOCATION_INIT(allocation);
  allocation->next = allocations;
  allocations = allocation;

  return object;
}

void stella_object_init_managed(
    StellaValue object,
    size_t index,
    StellaValue value) {
  require_active();
  Allocation *allocation = require_heap_object(object);
  if (index >= object->descriptor->managed_count) {
    stella_abi_violation("managed initialisation index out of range");
  }
  if (allocation != NULL && allocation->initialised[index] != 0) {
    stella_abi_violation("managed field initialised more than once");
  }

  g_gc_context.initManaged(object, index, value);
}

void stella_object_init_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  require_active();
  Allocation *allocation = require_heap_object(object);
  if (index >= object->descriptor->primitive_count) {
    stella_abi_violation("primitive initialisation index out of range");
  }

  g_gc_context.initPrimitive(object, index, value);
}

StellaValue stella_gc_read_managed(
    StellaValue *object_root,
    size_t index) {
  require_active();
  require_registered_root(object_root);
  GC_BEFORE_READ(object_root);
  validate_object(*object_root);
  if (index >= (*object_root)->descriptor->managed_count) {
    stella_abi_violation("managed read index out of range");
  }
  require_active();
  return g_gc_context.readManaged(gc::root{object_root}, index);
}

void stella_gc_write_managed(
    StellaValue *object_root,
    size_t index,
    StellaValue *value_root) {
  require_active();
  g_gc_context.writeManaged(gc::root{object_root}, index, gc::root{value_root});
}

StellaPrimitive stella_object_read_primitive(
    StellaValue object,
    size_t index) {
  require_active();
  return g_gc_context.readPrimitive(object, index);
}

void stella_object_write_primitive(
    StellaValue object,
    size_t index,
    StellaPrimitive value) {
  require_active();
  g_gc_context.writePrimitive(object, index, value);
}

void stella_gc_update_object(
    StellaValue *object_root,
    const StellaObjectDescriptor *new_descriptor,
    StellaValue *const new_managed_roots[],
    const StellaPrimitive new_primitives[]) {
  require_active();
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
  require_active();
  if (output == NULL) {
    stella_abi_violation("null statistics output stream");
  }
  fprintf(output, "requested bytes: %zu\n", counters.requested_bytes);
  fprintf(output, "rounded bytes: %zu\n", counters.rounded_bytes);
  fprintf(output, "allocated objects: %zu\n", counters.allocated_objects);
  fprintf(output, "collections: %zu\n", counters.collections);
  fprintf(output, "occupied bytes: %zu\n", counters.occupied_bytes);
  fprintf(
      output,
      "maximum occupied bytes: %zu\n",
      counters.maximum_occupied_bytes);
  fprintf(output, "managed reads: %zu\n", counters.managed_reads);
  fprintf(output, "managed writes: %zu\n", counters.managed_writes);
  fprintf(output, "primitive reads: %zu\n", counters.primitive_reads);
  fprintf(output, "primitive writes: %zu\n", counters.primitive_writes);
  fprintf(
      output,
      "read barrier activations: %zu\n",
      counters.read_barrier_activations);
  fprintf(
      output,
      "write barrier activations: %zu\n",
      counters.write_barrier_activations);
  fprintf(output, "dynamic roots: %zu\n", counters.dynamic_root_depth);
  fprintf(
      output,
      "maximum dynamic roots: %zu\n",
      counters.maximum_dynamic_root_depth);
  fprintf(output, "permanent roots: %zu\n", counters.permanent_root_count);
  fprintf(output, "failed allocations: %zu\n", counters.failed_allocations);

  fprintf(output, "total scans: %zu\n", counters.total_scans);
  fprintf(output, "scans out of budget: %zu\n", counters.scan_out_of_budget);
}

void stella_gc_print_roots(FILE *output) {
  require_active();
  if (!output) { stella_abi_violation("null roots output stream"); }

  g_gc_context.printRoots(output);
}

void stella_gc_print_state(FILE *output) {
  require_active();
  if (output == NULL) {
    stella_abi_violation("null state output stream");
  }
  gc::Heap &heap = g_gc_context.heap();
  size_t const occupied = heap.fromNext();
  fprintf(
      output,
      "%s: from-space=[%p,%p) occupied=%zu limit=%zu to-space=%p\n",
      GC_DESCRIPTION,
      (void *)heap.fromBegin(),
      (void *)((unsigned char *)heap.fromBegin() + occupied),
      occupied,
      heap.limit(),
      (void *)heap.toBegin());
  if (heap.isCollecting()) {
    fprintf(output, "collection in progress\n");
  } else {
    unsigned char *pos = (unsigned char *)heap.fromBegin();
    unsigned char *const end = (unsigned char *)heap.fromBegin() + occupied;
    while (pos < end) {
      StellaValue object = (StellaValue)pos;
      const StellaObjectDescriptor *descriptor = object->descriptor;
      validate_descriptor(descriptor);
      size_t rounded = rounded_size(object_size(descriptor));
      fprintf(
          output,
          "allocation=[%p,%p) object=%p descriptor=%s kind=%" PRIu32
          " managed=%zu primitive=%zu capacity=%zu gc_word=%" PRIuPTR
          "\n",
          (void *)object,
          (void *)((unsigned char *)object + rounded),
          (void *)object,
          descriptor->debug_name == NULL ? "(unnamed)" : descriptor->debug_name,
          descriptor->kind,
          descriptor->managed_count,
          descriptor->primitive_count,
          descriptor->slot_capacity,
          object->gc_word);
      for (size_t i = 0; i < descriptor->managed_count; ++i) {
        fprintf(
            output,
            "  managed[%zu]=%p\n",
            i,
            (void *)object->fields[i].managed);
      }
      for (size_t i = 0; i < descriptor->primitive_count; ++i) {
        fprintf(
            output,
            "  primitive[%zu]=%" PRIuPTR "\n",
            i,
            object->fields[descriptor->managed_count + i].primitive);
      }
      pos += rounded;
    }
  }
  stella_gc_print_roots(output);
}
