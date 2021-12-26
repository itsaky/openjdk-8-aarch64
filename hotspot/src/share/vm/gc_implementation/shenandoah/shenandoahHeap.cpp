/*
 * Copyright (c) 2013, 2018, Red Hat, Inc. All rights reserved.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "memory/allocation.hpp"

#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shenandoah/shenandoahGCTraceTime.hpp"

#include "gc_implementation/shenandoah/shenandoahBarrierSet.hpp"
#include "gc_implementation/shenandoah/shenandoahClosures.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectorPolicy.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahControlThread.hpp"
#include "gc_implementation/shenandoah/shenandoahFreeSet.hpp"
#include "gc_implementation/shenandoah/shenandoahPhaseTimings.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkCompact.hpp"
#include "gc_implementation/shenandoah/shenandoahMarkingContext.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahMonitoringSupport.hpp"
#include "gc_implementation/shenandoah/shenandoahMetrics.hpp"
#include "gc_implementation/shenandoah/shenandoahOopClosures.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahPacer.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahPadding.hpp"
#include "gc_implementation/shenandoah/shenandoahParallelCleaning.hpp"
#include "gc_implementation/shenandoah/shenandoahRootProcessor.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.hpp"
#include "gc_implementation/shenandoah/shenandoahUtils.hpp"
#include "gc_implementation/shenandoah/shenandoahVerifier.hpp"
#include "gc_implementation/shenandoah/shenandoahCodeRoots.hpp"
#include "gc_implementation/shenandoah/shenandoahVMOperations.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkGroup.hpp"
#include "gc_implementation/shenandoah/shenandoahWorkerPolicy.hpp"
#include "gc_implementation/shenandoah/heuristics/shenandoahHeuristics.hpp"
#include "gc_implementation/shenandoah/mode/shenandoahIUMode.hpp"
#include "gc_implementation/shenandoah/mode/shenandoahPassiveMode.hpp"
#include "gc_implementation/shenandoah/mode/shenandoahSATBMode.hpp"
#if INCLUDE_JFR
#include "gc_implementation/shenandoah/shenandoahJfrSupport.hpp"
#endif

#include "memory/metaspace.hpp"
#include "runtime/vmThread.hpp"
#include "services/mallocTracker.hpp"

ShenandoahHeap* ShenandoahHeap::_heap = NULL;

class ShenandoahPretouchHeapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  const size_t _page_size;
public:
  ShenandoahPretouchHeapTask(size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Heap"),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions.next();
    while (r != NULL) {
      if (r->is_committed()) {
        os::pretouch_memory((char *) r->bottom(), (char *) r->end());
      }
      r = _regions.next();
    }
  }
};

class ShenandoahPretouchBitmapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;
  char* _bitmap_base;
  const size_t _bitmap_size;
  const size_t _page_size;
public:
  ShenandoahPretouchBitmapTask(char* bitmap_base, size_t bitmap_size, size_t page_size) :
    AbstractGangTask("Shenandoah Pretouch Bitmap"),
    _bitmap_base(bitmap_base),
    _bitmap_size(bitmap_size),
    _page_size(page_size) {}

  virtual void work(uint worker_id) {
    ShenandoahHeapRegion* r = _regions.next();
    while (r != NULL) {
      size_t start = r->index()       * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      size_t end   = (r->index() + 1) * ShenandoahHeapRegion::region_size_bytes() / MarkBitMap::heap_map_factor();
      assert (end <= _bitmap_size, err_msg("end is sane: " SIZE_FORMAT " < " SIZE_FORMAT, end, _bitmap_size));

      if (r->is_committed()) {
        os::pretouch_memory(_bitmap_base + start, _bitmap_base + end);
      }

      r = _regions.next();
    }
  }
};

jint ShenandoahHeap::initialize() {
  CollectedHeap::pre_initialize();

  //
  // Figure out heap sizing
  //

  size_t init_byte_size = collector_policy()->initial_heap_byte_size();
  size_t min_byte_size  = collector_policy()->min_heap_byte_size();
  size_t max_byte_size  = collector_policy()->max_heap_byte_size();
  size_t heap_alignment = collector_policy()->heap_alignment();

  size_t reg_size_bytes = ShenandoahHeapRegion::region_size_bytes();

  Universe::check_alignment(max_byte_size,  reg_size_bytes, "Shenandoah heap");
  Universe::check_alignment(init_byte_size, reg_size_bytes, "Shenandoah heap");

  _num_regions = ShenandoahHeapRegion::region_count();

  // Now we know the number of regions, initialize the heuristics.
  initialize_heuristics();

  size_t num_committed_regions = init_byte_size / reg_size_bytes;
  num_committed_regions = MIN2(num_committed_regions, _num_regions);
  assert(num_committed_regions <= _num_regions, "sanity");
  _initial_size = num_committed_regions * reg_size_bytes;

  size_t num_min_regions = min_byte_size / reg_size_bytes;
  num_min_regions = MIN2(num_min_regions, _num_regions);
  assert(num_min_regions <= _num_regions, "sanity");
  _minimum_size = num_min_regions * reg_size_bytes;

  // Default to max heap size.
  _soft_max_size = _num_regions * reg_size_bytes;

  _committed = _initial_size;

  size_t heap_page_size   = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  size_t bitmap_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();
  size_t region_page_size = UseLargePages ? (size_t)os::large_page_size() : (size_t)os::vm_page_size();

  //
  // Reserve and commit memory for heap
  //

  ReservedSpace heap_rs = Universe::reserve_heap(max_byte_size, heap_alignment);
  _reserved.set_word_size(0);
  _reserved.set_start((HeapWord*)heap_rs.base());
  _reserved.set_end((HeapWord*)(heap_rs.base() + heap_rs.size()));
  _heap_region = MemRegion((HeapWord*)heap_rs.base(), heap_rs.size() / HeapWordSize);
  _heap_region_special = heap_rs.special();

  assert((((size_t) base()) & ShenandoahHeapRegion::region_size_bytes_mask()) == 0,
         err_msg("Misaligned heap: " PTR_FORMAT, p2i(base())));

#if SHENANDOAH_OPTIMIZED_MARKTASK
  // The optimized ObjArrayChunkedTask takes some bits away from the full object bits.
  // Fail if we ever attempt to address more than we can.
  if ((uintptr_t)(heap_rs.base() + heap_rs.size()) >= ShenandoahMarkTask::max_addressable()) {
    FormatBuffer<512> buf("Shenandoah reserved [" PTR_FORMAT ", " PTR_FORMAT") for the heap, \n"
                          "but max object address is " PTR_FORMAT ". Try to reduce heap size, or try other \n"
                          "VM options that allocate heap at lower addresses (HeapBaseMinAddress, AllocateHeapAt, etc).",
                p2i(heap_rs.base()), p2i(heap_rs.base() + heap_rs.size()), ShenandoahMarkTask::max_addressable());
    vm_exit_during_initialization("Fatal Error", buf);
  }
#endif

  ReservedSpace sh_rs = heap_rs.first_part(max_byte_size);
  if (!_heap_region_special) {
    os::commit_memory_or_exit(sh_rs.base(), _initial_size, heap_alignment, false,
                              "Cannot commit heap memory");
  }

  //
  // Reserve and commit memory for bitmap(s)
  //

  _bitmap_size = MarkBitMap::compute_size(heap_rs.size());
  _bitmap_size = align_size_up(_bitmap_size, bitmap_page_size);

  size_t bitmap_bytes_per_region = reg_size_bytes / MarkBitMap::heap_map_factor();

  guarantee(bitmap_bytes_per_region != 0,
            err_msg("Bitmap bytes per region should not be zero"));
  guarantee(is_power_of_2(bitmap_bytes_per_region),
            err_msg("Bitmap bytes per region should be power of two: " SIZE_FORMAT, bitmap_bytes_per_region));

  if (bitmap_page_size > bitmap_bytes_per_region) {
    _bitmap_regions_per_slice = bitmap_page_size / bitmap_bytes_per_region;
    _bitmap_bytes_per_slice = bitmap_page_size;
  } else {
    _bitmap_regions_per_slice = 1;
    _bitmap_bytes_per_slice = bitmap_bytes_per_region;
  }

  guarantee(_bitmap_regions_per_slice >= 1,
            err_msg("Should have at least one region per slice: " SIZE_FORMAT,
                    _bitmap_regions_per_slice));

  guarantee(((_bitmap_bytes_per_slice) % bitmap_page_size) == 0,
            err_msg("Bitmap slices should be page-granular: bps = " SIZE_FORMAT ", page size = " SIZE_FORMAT,
                    _bitmap_bytes_per_slice, bitmap_page_size));

  ReservedSpace bitmap(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(bitmap.base(), mtGC);
  _bitmap_region = MemRegion((HeapWord*) bitmap.base(), bitmap.size() / HeapWordSize);
  _bitmap_region_special = bitmap.special();

  size_t bitmap_init_commit = _bitmap_bytes_per_slice *
                              align_size_up(num_committed_regions, _bitmap_regions_per_slice) / _bitmap_regions_per_slice;
  bitmap_init_commit = MIN2(_bitmap_size, bitmap_init_commit);
  if (!_bitmap_region_special) {
    os::commit_memory_or_exit((char *) _bitmap_region.start(), bitmap_init_commit, bitmap_page_size, false,
                              "Cannot commit bitmap memory");
  }

  _marking_context = new ShenandoahMarkingContext(_heap_region, _bitmap_region, _num_regions);

  if (ShenandoahVerify) {
    ReservedSpace verify_bitmap(_bitmap_size, bitmap_page_size);
    if (!verify_bitmap.special()) {
      os::commit_memory_or_exit(verify_bitmap.base(), verify_bitmap.size(), bitmap_page_size, false,
                                "Cannot commit verification bitmap memory");
    }
    MemTracker::record_virtual_memory_type(verify_bitmap.base(), mtGC);
    MemRegion verify_bitmap_region = MemRegion((HeapWord *) verify_bitmap.base(), verify_bitmap.size() / HeapWordSize);
    _verification_bit_map.initialize(_heap_region, verify_bitmap_region);
    _verifier = new ShenandoahVerifier(this, &_verification_bit_map);
  }

  // Reserve aux bitmap for use in object_iterate(). We don't commit it here.
  ReservedSpace aux_bitmap(_bitmap_size, bitmap_page_size);
  MemTracker::record_virtual_memory_type(aux_bitmap.base(), mtGC);
  _aux_bitmap_region = MemRegion((HeapWord*) aux_bitmap.base(), aux_bitmap.size() / HeapWordSize);
  _aux_bitmap_region_special = aux_bitmap.special();
  _aux_bit_map.initialize(_heap_region, _aux_bitmap_region);

  //
  // Create regions and region sets
  //
  size_t region_align = align_size_up(sizeof(ShenandoahHeapRegion), SHENANDOAH_CACHE_LINE_SIZE);
  size_t region_storage_size = align_size_up(region_align * _num_regions, region_page_size);
  region_storage_size = align_size_up(region_storage_size, os::vm_allocation_granularity());

  ReservedSpace region_storage(region_storage_size, region_page_size);
  MemTracker::record_virtual_memory_type(region_storage.base(), mtGC);
  if (!region_storage.special()) {
    os::commit_memory_or_exit(region_storage.base(), region_storage_size, region_page_size, false,
                              "Cannot commit region memory");
  }

  // Try to fit the collection set bitmap at lower addresses. This optimizes code generation for cset checks.
  // Go up until a sensible limit (subject to encoding constraints) and try to reserve the space there.
  // If not successful, bite a bullet and allocate at whatever address.
  {
    size_t cset_align = MAX2<size_t>(os::vm_page_size(), os::vm_allocation_granularity());
    size_t cset_size = align_size_up(((size_t) sh_rs.base() + sh_rs.size()) >> ShenandoahHeapRegion::region_size_bytes_shift(), cset_align);

    uintptr_t min = ShenandoahUtils::round_up_power_of_2(cset_align);
    uintptr_t max = (1u << 30u);

    for (uintptr_t addr = min; addr <= max; addr <<= 1u) {
      char* req_addr = (char*)addr;
      assert(is_ptr_aligned(req_addr, cset_align), "Should be aligned");
      ReservedSpace cset_rs(cset_size, cset_align, false, req_addr);
      if (cset_rs.is_reserved()) {
        assert(cset_rs.base() == req_addr, err_msg("Allocated where requested: " PTR_FORMAT ", " PTR_FORMAT, p2i(cset_rs.base()), addr));
        _collection_set = new ShenandoahCollectionSet(this, cset_rs, sh_rs.base());
        break;
      }
    }

    if (_collection_set == NULL) {
      ReservedSpace cset_rs(cset_size, cset_align, false);
      _collection_set = new ShenandoahCollectionSet(this, cset_rs, sh_rs.base());
    }
  }

  _regions = NEW_C_HEAP_ARRAY(ShenandoahHeapRegion*, _num_regions, mtGC);
  _free_set = new ShenandoahFreeSet(this, _num_regions);

  {
    ShenandoahHeapLocker locker(lock());

    for (size_t i = 0; i < _num_regions; i++) {
      HeapWord* start = (HeapWord*)sh_rs.base() + ShenandoahHeapRegion::region_size_words() * i;
      bool is_committed = i < num_committed_regions;
      void* loc = region_storage.base() + i * region_align;

      ShenandoahHeapRegion* r = new (loc) ShenandoahHeapRegion(start, i, is_committed);
      assert(is_ptr_aligned(r, SHENANDOAH_CACHE_LINE_SIZE), "Sanity");

      _marking_context->initialize_top_at_mark_start(r);
      _regions[i] = r;
      assert(!collection_set()->is_in(i), "New region should not be in collection set");
    }

    // Initialize to complete
    _marking_context->mark_complete();

    _free_set->rebuild();
  }

  if (AlwaysPreTouch) {
    // For NUMA, it is important to pre-touch the storage under bitmaps with worker threads,
    // before initialize() below zeroes it with initializing thread. For any given region,
    // we touch the region and the corresponding bitmaps from the same thread.
    ShenandoahPushWorkerScope scope(workers(), _max_workers, false);

    _pretouch_heap_page_size = heap_page_size;
    _pretouch_bitmap_page_size = bitmap_page_size;

#ifdef LINUX
    // UseTransparentHugePages would madvise that backing memory can be coalesced into huge
    // pages. But, the kernel needs to know that every small page is used, in order to coalesce
    // them into huge one. Therefore, we need to pretouch with smaller pages.
    if (UseTransparentHugePages) {
      _pretouch_heap_page_size = (size_t)os::vm_page_size();
      _pretouch_bitmap_page_size = (size_t)os::vm_page_size();
    }
#endif

    // OS memory managers may want to coalesce back-to-back pages. Make their jobs
    // simpler by pre-touching continuous spaces (heap and bitmap) separately.

    ShenandoahPretouchBitmapTask bcl(bitmap.base(), _bitmap_size, _pretouch_bitmap_page_size);
    _workers->run_task(&bcl);

    ShenandoahPretouchHeapTask hcl(_pretouch_heap_page_size);
    _workers->run_task(&hcl);
  }

  //
  // Initialize the rest of GC subsystems
  //

  set_barrier_set(new ShenandoahBarrierSet(this));

  _liveness_cache = NEW_C_HEAP_ARRAY(ShenandoahLiveData*, _max_workers, mtGC);
  for (uint worker = 0; worker < _max_workers; worker++) {
    _liveness_cache[worker] = NEW_C_HEAP_ARRAY(ShenandoahLiveData, _num_regions, mtGC);
    Copy::fill_to_bytes(_liveness_cache[worker], _num_regions * sizeof(ShenandoahLiveData));
  }

  // The call below uses stuff (the SATB* things) that are in G1, but probably
  // belong into a shared location.
  JavaThread::satb_mark_queue_set().initialize(SATB_Q_CBL_mon,
                                               SATB_Q_FL_lock,
                                               20 /*G1SATBProcessCompletedThreshold */,
                                               Shared_SATB_Q_lock);

  _monitoring_support = new ShenandoahMonitoringSupport(this);
  _phase_timings = new ShenandoahPhaseTimings(max_workers());
  ShenandoahStringDedup::initialize();
  ShenandoahCodeRoots::initialize();

  if (ShenandoahPacing) {
    _pacer = new ShenandoahPacer(this);
    _pacer->setup_for_idle();
  } else {
    _pacer = NULL;
  }

  _control_thread = new ShenandoahControlThread();

  log_info(gc, init)("Initialize Shenandoah heap: " SIZE_FORMAT "%s initial, " SIZE_FORMAT "%s min, " SIZE_FORMAT "%s max",
                     byte_size_in_proper_unit(_initial_size),  proper_unit_for_byte_size(_initial_size),
                     byte_size_in_proper_unit(_minimum_size),  proper_unit_for_byte_size(_minimum_size),
                     byte_size_in_proper_unit(max_capacity()), proper_unit_for_byte_size(max_capacity())
  );

  return JNI_OK;
}

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif

void ShenandoahHeap::initialize_heuristics() {
  if (ShenandoahGCMode != NULL) {
    if (strcmp(ShenandoahGCMode, "satb") == 0) {
      _gc_mode = new ShenandoahSATBMode();
    } else if (strcmp(ShenandoahGCMode, "iu") == 0) {
      _gc_mode = new ShenandoahIUMode();
    } else if (strcmp(ShenandoahGCMode, "passive") == 0) {
      _gc_mode = new ShenandoahPassiveMode();
    } else {
      vm_exit_during_initialization("Unknown -XX:ShenandoahGCMode option");
    }
  } else {
    ShouldNotReachHere();
  }
  _gc_mode->initialize_flags();
  if (_gc_mode->is_diagnostic() && !UnlockDiagnosticVMOptions) {
    vm_exit_during_initialization(
            err_msg("GC mode \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                    _gc_mode->name()));
  }
  if (_gc_mode->is_experimental() && !UnlockExperimentalVMOptions) {
    vm_exit_during_initialization(
            err_msg("GC mode \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                    _gc_mode->name()));
  }
  log_info(gc, init)("Shenandoah GC mode: %s",
                     _gc_mode->name());

  _heuristics = _gc_mode->initialize_heuristics();

  if (_heuristics->is_diagnostic() && !UnlockDiagnosticVMOptions) {
    vm_exit_during_initialization(
            err_msg("Heuristics \"%s\" is diagnostic, and must be enabled via -XX:+UnlockDiagnosticVMOptions.",
                    _heuristics->name()));
  }
  if (_heuristics->is_experimental() && !UnlockExperimentalVMOptions) {
    vm_exit_during_initialization(
            err_msg("Heuristics \"%s\" is experimental, and must be enabled via -XX:+UnlockExperimentalVMOptions.",
                    _heuristics->name()));
  }
  log_info(gc, init)("Shenandoah heuristics: %s",
                     _heuristics->name());
}

ShenandoahHeap::ShenandoahHeap(ShenandoahCollectorPolicy* policy) :
  SharedHeap(policy),
  _shenandoah_policy(policy),
  _heap_region_special(false),
  _regions(NULL),
  _free_set(NULL),
  _collection_set(NULL),
  _update_refs_iterator(this),
  _bytes_allocated_since_gc_start(0),
  _max_workers((uint)MAX2(ConcGCThreads, ParallelGCThreads)),
  _ref_processor(NULL),
  _marking_context(NULL),
  _bitmap_size(0),
  _bitmap_regions_per_slice(0),
  _bitmap_bytes_per_slice(0),
  _bitmap_region_special(false),
  _aux_bitmap_region_special(false),
  _liveness_cache(NULL),
  _aux_bit_map(),
  _verifier(NULL),
  _pacer(NULL),
  _gc_timer(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
  _phase_timings(NULL)
{
  _heap = this;

  log_info(gc, init)("GC threads: " UINTX_FORMAT " parallel, " UINTX_FORMAT " concurrent", ParallelGCThreads, ConcGCThreads);

  _scm = new ShenandoahConcurrentMark();

  _full_gc = new ShenandoahMarkCompact();
  _used = 0;

  _max_workers = MAX2(_max_workers, 1U);

  // SharedHeap did not initialize this for us, and we want our own workgang anyway.
  assert(SharedHeap::_workers == NULL && _workers == NULL, "Should not be initialized yet");
  _workers = new ShenandoahWorkGang("Shenandoah GC Threads", _max_workers,
                            /* are_GC_task_threads */true,
                            /* are_ConcurrentGC_threads */false);
  if (_workers == NULL) {
    vm_exit_during_initialization("Failed necessary allocation.");
  } else {
    _workers->initialize_workers();
  }
  assert(SharedHeap::_workers == _workers, "Sanity: initialized the correct field");
}

#ifdef _MSC_VER
#pragma warning( pop )
#endif

class ShenandoahResetBitmapTask : public AbstractGangTask {
private:
  ShenandoahRegionIterator _regions;

public:
  ShenandoahResetBitmapTask() :
    AbstractGangTask("Parallel Reset Bitmap Task") {}

  void work(uint worker_id) {
    ShenandoahHeapRegion* region = _regions.next();
    ShenandoahHeap* heap = ShenandoahHeap::heap();
    ShenandoahMarkingContext* const ctx = heap->marking_context();
    while (region != NULL) {
      if (heap->is_bitmap_slice_committed(region)) {
        ctx->clear_bitmap(region);
      }
      region = _regions.next();
    }
  }
};

void ShenandoahHeap::reset_mark_bitmap() {
  assert_gc_workers(_workers->active_workers());
  mark_incomplete_marking_context();

  ShenandoahResetBitmapTask task;
  _workers->run_task(&task);
}

void ShenandoahHeap::print_on(outputStream* st) const {
  st->print_cr("Shenandoah Heap");
  st->print_cr(" " SIZE_FORMAT "%s max, " SIZE_FORMAT "%s soft max, " SIZE_FORMAT "%s committed, " SIZE_FORMAT "%s used",
               byte_size_in_proper_unit(max_capacity()), proper_unit_for_byte_size(max_capacity()),
               byte_size_in_proper_unit(soft_max_capacity()), proper_unit_for_byte_size(soft_max_capacity()),
               byte_size_in_proper_unit(committed()),    proper_unit_for_byte_size(committed()),
               byte_size_in_proper_unit(used()),         proper_unit_for_byte_size(used()));
  st->print_cr(" " SIZE_FORMAT " x " SIZE_FORMAT"%s regions",
               num_regions(),
               byte_size_in_proper_unit(ShenandoahHeapRegion::region_size_bytes()),
               proper_unit_for_byte_size(ShenandoahHeapRegion::region_size_bytes()));

  st->print("Status: ");
  if (has_forwarded_objects())               st->print("has forwarded objects, ");
  if (is_concurrent_mark_in_progress())      st->print("marking, ");
  if (is_evacuation_in_progress())           st->print("evacuating, ");
  if (is_update_refs_in_progress())          st->print("updating refs, ");
  if (is_degenerated_gc_in_progress())       st->print("degenerated gc, ");
  if (is_full_gc_in_progress())              st->print("full gc, ");
  if (is_full_gc_move_in_progress())         st->print("full gc move, ");

  if (cancelled_gc()) {
    st->print("cancelled");
  } else {
    st->print("not cancelled");
  }
  st->cr();

  st->print_cr("Reserved region:");
  st->print_cr(" - [" PTR_FORMAT ", " PTR_FORMAT ") ",
               p2i(reserved_region().start()),
               p2i(reserved_region().end()));

  ShenandoahCollectionSet* cset = collection_set();
  st->print_cr("Collection set:");
  if (cset != NULL) {
    st->print_cr(" - map (vanilla): " PTR_FORMAT, p2i(cset->map_address()));
    st->print_cr(" - map (biased):  " PTR_FORMAT, p2i(cset->biased_map_address()));
  } else {
    st->print_cr(" (NULL)");
  }

  st->cr();
  MetaspaceAux::print_on(st);

  if (Verbose) {
    print_heap_regions_on(st);
  }
}

class ShenandoahInitGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread == NULL || !thread->is_Java_thread(), "Don't expect JavaThread this early");
    if (thread != NULL && thread->is_Worker_thread()) {
      thread->gclab().initialize(true);
    }
  }
};

void ShenandoahHeap::post_initialize() {
  if (UseTLAB) {
    MutexLocker ml(Threads_lock);

    ShenandoahInitGCLABClosure init_gclabs;
    Threads::threads_do(&init_gclabs);
  }

  _scm->initialize(_max_workers);
  _full_gc->initialize(_gc_timer);

  ref_processing_init();

  _heuristics->initialize();

  JFR_ONLY(ShenandoahJFRSupport::register_jfr_type_serializers());
}

size_t ShenandoahHeap::used() const {
  OrderAccess::acquire();
  return (size_t) _used;
}

size_t ShenandoahHeap::committed() const {
  OrderAccess::acquire();
  return _committed;
}

void ShenandoahHeap::increase_committed(size_t bytes) {
  shenandoah_assert_heaplocked_or_safepoint();
  _committed += bytes;
}

void ShenandoahHeap::decrease_committed(size_t bytes) {
  shenandoah_assert_heaplocked_or_safepoint();
  _committed -= bytes;
}

void ShenandoahHeap::increase_used(size_t bytes) {
  Atomic::add(bytes, &_used);
}

void ShenandoahHeap::set_used(size_t bytes) {
  OrderAccess::release_store_fence(&_used, bytes);
}

void ShenandoahHeap::decrease_used(size_t bytes) {
  assert(used() >= bytes, "never decrease heap size by more than we've left");
  Atomic::add(-(jlong)bytes, &_used);
}

void ShenandoahHeap::increase_allocated(size_t bytes) {
  Atomic::add(bytes, &_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::notify_mutator_alloc_words(size_t words, bool waste) {
  size_t bytes = words * HeapWordSize;
  if (!waste) {
    increase_used(bytes);
  }
  increase_allocated(bytes);
  if (ShenandoahPacing) {
    control_thread()->pacing_notify_alloc(words);
    if (waste) {
      pacer()->claim_for_alloc(words, true);
    }
  }
}

size_t ShenandoahHeap::capacity() const {
  return committed();
}

size_t ShenandoahHeap::max_capacity() const {
  return _num_regions * ShenandoahHeapRegion::region_size_bytes();
}

size_t ShenandoahHeap::soft_max_capacity() const {
  size_t v = OrderAccess::load_acquire((volatile size_t*)&_soft_max_size);
  assert(min_capacity() <= v && v <= max_capacity(),
         err_msg("Should be in bounds: " SIZE_FORMAT " <= " SIZE_FORMAT " <= " SIZE_FORMAT,
                 min_capacity(), v, max_capacity()));
  return v;
}

void ShenandoahHeap::set_soft_max_capacity(size_t v) {
  assert(min_capacity() <= v && v <= max_capacity(),
         err_msg("Should be in bounds: " SIZE_FORMAT " <= " SIZE_FORMAT " <= " SIZE_FORMAT,
                 min_capacity(), v, max_capacity()));
  OrderAccess::release_store_fence(&_soft_max_size, v);
}

size_t ShenandoahHeap::min_capacity() const {
  return _minimum_size;
}

size_t ShenandoahHeap::initial_capacity() const {
  return _initial_size;
}

bool ShenandoahHeap::is_in(const void* p) const {
  HeapWord* heap_base = (HeapWord*) base();
  HeapWord* last_region_end = heap_base + ShenandoahHeapRegion::region_size_words() * num_regions();
  return p >= heap_base && p < last_region_end;
}

void ShenandoahHeap::op_uncommit(double shrink_before, size_t shrink_until) {
  assert (ShenandoahUncommit, "should be enabled");

  // Application allocates from the beginning of the heap, and GC allocates at
  // the end of it. It is more efficient to uncommit from the end, so that applications
  // could enjoy the near committed regions. GC allocations are much less frequent,
  // and therefore can accept the committing costs.

  size_t count = 0;
  for (size_t i = num_regions(); i > 0; i--) { // care about size_t underflow
    ShenandoahHeapRegion* r = get_region(i - 1);
    if (r->is_empty_committed() && (r->empty_time() < shrink_before)) {
      ShenandoahHeapLocker locker(lock());
      if (r->is_empty_committed()) {
        if (committed() < shrink_until + ShenandoahHeapRegion::region_size_bytes()) {
          break;
        }

        r->make_uncommitted();
        count++;
      }
    }
    SpinPause(); // allow allocators to take the lock
  }

  if (count > 0) {
    _control_thread->notify_heap_changed();
  }
}

HeapWord* ShenandoahHeap::allocate_from_gclab_slow(Thread* thread, size_t size) {
  // Retain tlab and allocate object in shared space if
  // the amount free in the tlab is too large to discard.
  if (thread->gclab().free() > thread->gclab().refill_waste_limit()) {
    thread->gclab().record_slow_allocation(size);
    return NULL;
  }

  // Discard gclab and allocate a new one.
  // To minimize fragmentation, the last GCLAB may be smaller than the rest.
  size_t new_gclab_size = thread->gclab().compute_size(size);

  thread->gclab().clear_before_allocation();

  if (new_gclab_size == 0) {
    return NULL;
  }

  // Allocated object should fit in new GCLAB, and new_gclab_size should be larger than min
  size_t min_size = MAX2(size + ThreadLocalAllocBuffer::alignment_reserve(), ThreadLocalAllocBuffer::min_size());
  new_gclab_size = MAX2(new_gclab_size, min_size);

  // Allocate a new GCLAB...
  size_t actual_size = 0;
  HeapWord* obj = allocate_new_gclab(min_size, new_gclab_size, &actual_size);

  if (obj == NULL) {
    return NULL;
  }

  assert (size <= actual_size, "allocation should fit");

  if (ZeroTLAB) {
    // ..and clear it.
    Copy::zero_to_words(obj, actual_size);
  } else {
    // ...and zap just allocated object.
#ifdef ASSERT
    // Skip mangling the space corresponding to the object header to
    // ensure that the returned space is not considered parsable by
    // any concurrent GC thread.
    size_t hdr_size = oopDesc::header_size();
    Copy::fill_to_words(obj + hdr_size, actual_size - hdr_size, badHeapWordVal);
#endif // ASSERT
  }
  thread->gclab().fill(obj, obj + size, actual_size);
  return obj;
}

HeapWord* ShenandoahHeap::allocate_new_tlab(size_t word_size) {
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_tlab(word_size);
  return allocate_memory(req);
}

HeapWord* ShenandoahHeap::allocate_new_gclab(size_t min_size,
                                             size_t word_size,
                                             size_t* actual_size) {
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_gclab(min_size, word_size);
  HeapWord* res = allocate_memory(req);
  if (res != NULL) {
    *actual_size = req.actual_size();
  } else {
    *actual_size = 0;
  }
  return res;
}

HeapWord* ShenandoahHeap::allocate_memory(ShenandoahAllocRequest& req) {
  intptr_t pacer_epoch = 0;
  bool in_new_region = false;
  HeapWord* result = NULL;

  if (req.is_mutator_alloc()) {
    if (ShenandoahPacing) {
      pacer()->pace_for_alloc(req.size());
      pacer_epoch = pacer()->epoch();
    }

    if (!ShenandoahAllocFailureALot || !should_inject_alloc_failure()) {
      result = allocate_memory_under_lock(req, in_new_region);
    }

    // Allocation failed, block until control thread reacted, then retry allocation.
    //
    // It might happen that one of the threads requesting allocation would unblock
    // way later after GC happened, only to fail the second allocation, because
    // other threads have already depleted the free storage. In this case, a better
    // strategy is to try again, as long as GC makes progress.
    //
    // Then, we need to make sure the allocation was retried after at least one
    // Full GC, which means we want to try more than ShenandoahFullGCThreshold times.

    size_t tries = 0;

    while (result == NULL && _progress_last_gc.is_set()) {
      tries++;
      control_thread()->handle_alloc_failure(req);
      result = allocate_memory_under_lock(req, in_new_region);
    }

    while (result == NULL && tries <= ShenandoahFullGCThreshold) {
      tries++;
      control_thread()->handle_alloc_failure(req);
      result = allocate_memory_under_lock(req, in_new_region);
    }

  } else {
    assert(req.is_gc_alloc(), "Can only accept GC allocs here");
    result = allocate_memory_under_lock(req, in_new_region);
    // Do not call handle_alloc_failure() here, because we cannot block.
    // The allocation failure would be handled by the WB slowpath with handle_alloc_failure_evac().
  }

  if (in_new_region) {
    control_thread()->notify_heap_changed();
  }

  if (result != NULL) {
    size_t requested = req.size();
    size_t actual = req.actual_size();

    assert (req.is_lab_alloc() || (requested == actual),
            err_msg("Only LAB allocations are elastic: %s, requested = " SIZE_FORMAT ", actual = " SIZE_FORMAT,
                    ShenandoahAllocRequest::alloc_type_to_string(req.type()), requested, actual));

    if (req.is_mutator_alloc()) {
      notify_mutator_alloc_words(actual, false);

      // If we requested more than we were granted, give the rest back to pacer.
      // This only matters if we are in the same pacing epoch: do not try to unpace
      // over the budget for the other phase.
      if (ShenandoahPacing && (pacer_epoch > 0) && (requested > actual)) {
        pacer()->unpace_for_alloc(pacer_epoch, requested - actual);
      }
    } else {
      increase_used(actual*HeapWordSize);
    }
  }

  return result;
}

HeapWord* ShenandoahHeap::allocate_memory_under_lock(ShenandoahAllocRequest& req, bool& in_new_region) {
  ShenandoahHeapLocker locker(lock());
  return _free_set->allocate(req, in_new_region);
}

HeapWord*  ShenandoahHeap::mem_allocate(size_t size,
                                        bool*  gc_overhead_limit_was_exceeded) {
  ShenandoahAllocRequest req = ShenandoahAllocRequest::for_shared(size);
  return allocate_memory(req);
}

class ShenandoahConcurrentEvacuateRegionObjectClosure : public ObjectClosure {
private:
  ShenandoahHeap* const _heap;
  Thread* const _thread;
public:
  ShenandoahConcurrentEvacuateRegionObjectClosure(ShenandoahHeap* heap) :
    _heap(heap), _thread(Thread::current()) {}

  void do_object(oop p) {
    shenandoah_assert_marked(NULL, p);
    if (!p->is_forwarded()) {
      _heap->evacuate_object(p, _thread);
    }
  }
};

class ShenandoahEvacuationTask : public AbstractGangTask {
private:
  ShenandoahHeap* const _sh;
  ShenandoahCollectionSet* const _cs;
  bool _concurrent;
public:
  ShenandoahEvacuationTask(ShenandoahHeap* sh,
                           ShenandoahCollectionSet* cs,
                           bool concurrent) :
    AbstractGangTask("Parallel Evacuation Task"),
    _sh(sh),
    _cs(cs),
    _concurrent(concurrent)
  {}

  void work(uint worker_id) {
    ShenandoahEvacOOMScope oom_evac_scope;
    if (_concurrent) {
      ShenandoahConcurrentWorkerSession worker_session(worker_id);
      do_work();
    } else {
      ShenandoahParallelWorkerSession worker_session(worker_id);
      do_work();
    }
  }

private:
  void do_work() {
    ShenandoahConcurrentEvacuateRegionObjectClosure cl(_sh);
    ShenandoahHeapRegion* r;
    while ((r =_cs->claim_next()) != NULL) {
      assert(r->has_live(), err_msg("Region " SIZE_FORMAT " should have been reclaimed early", r->index()));
      _sh->marked_object_iterate(r, &cl);

      if (ShenandoahPacing) {
        _sh->pacer()->report_evac(r->used() >> LogHeapWordSize);
      }

      if (_sh->cancelled_gc()) {
        break;
      }
    }
  }
};

void ShenandoahHeap::trash_cset_regions() {
  ShenandoahHeapLocker locker(lock());

  ShenandoahCollectionSet* set = collection_set();
  ShenandoahHeapRegion* r;
  set->clear_current_index();
  while ((r = set->next()) != NULL) {
    r->make_trash();
  }
  collection_set()->clear();
}

void ShenandoahHeap::print_heap_regions_on(outputStream* st) const {
  st->print_cr("Heap Regions:");
  st->print_cr("EU=empty-uncommitted, EC=empty-committed, R=regular, H=humongous start, HC=humongous continuation, CS=collection set, T=trash, P=pinned");
  st->print_cr("BTE=bottom/top/end, U=used, T=TLAB allocs, G=GCLAB allocs, S=shared allocs, L=live data");
  st->print_cr("R=root, CP=critical pins, TAMS=top-at-mark-start, UWM=update watermark");
  st->print_cr("SN=alloc sequence number");

  for (size_t i = 0; i < num_regions(); i++) {
    get_region(i)->print_on(st);
  }
}

void ShenandoahHeap::trash_humongous_region_at(ShenandoahHeapRegion* start) {
  assert(start->is_humongous_start(), "reclaim regions starting with the first one");

  oop humongous_obj = oop(start->bottom());
  size_t size = humongous_obj->size();
  size_t required_regions = ShenandoahHeapRegion::required_regions(size * HeapWordSize);
  size_t index = start->index() + required_regions - 1;

  assert(!start->has_live(), "liveness must be zero");

  for(size_t i = 0; i < required_regions; i++) {
     // Reclaim from tail. Otherwise, assertion fails when printing region to trace log,
     // as it expects that every region belongs to a humongous region starting with a humongous start region.
     ShenandoahHeapRegion* region = get_region(index --);

    assert(region->is_humongous(), "expect correct humongous start or continuation");
    assert(!region->is_cset(), "Humongous region should not be in collection set");

    region->make_trash_immediate();
  }
}

class ShenandoahRetireGCLABClosure : public ThreadClosure {
private:
  bool _retire;
public:
  ShenandoahRetireGCLABClosure(bool retire) : _retire(retire) {};

  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().make_parsable(_retire);
  }
};

void ShenandoahHeap::make_parsable(bool retire_tlabs) {
  if (UseTLAB) {
    CollectedHeap::ensure_parsability(retire_tlabs);
    ShenandoahRetireGCLABClosure cl(retire_tlabs);
    Threads::java_threads_do(&cl);
    _workers->threads_do(&cl);
  }
}

class ShenandoahEvacuateUpdateRootsTask : public AbstractGangTask {
private:
  ShenandoahRootEvacuator* _rp;

public:
  ShenandoahEvacuateUpdateRootsTask(ShenandoahRootEvacuator* rp) :
    AbstractGangTask("Shenandoah evacuate and update roots"),
    _rp(rp) {}

  void work(uint worker_id) {
    ShenandoahParallelWorkerSession worker_session(worker_id);
    ShenandoahEvacOOMScope oom_evac_scope;
    ShenandoahEvacuateUpdateRootsClosure cl;

    MarkingCodeBlobClosure blobsCl(&cl, CodeBlobToOopClosure::FixRelocations);
    _rp->roots_do(worker_id, &cl);
  }
};

void ShenandoahHeap::evacuate_and_update_roots() {
  COMPILER2_PRESENT(DerivedPointerTable::clear());

  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only iterate roots while world is stopped");

  {
    ShenandoahRootEvacuator rp(ShenandoahPhaseTimings::init_evac);
    ShenandoahEvacuateUpdateRootsTask roots_task(&rp);
    workers()->run_task(&roots_task);
  }

  COMPILER2_PRESENT(DerivedPointerTable::update_pointers());
}

size_t  ShenandoahHeap::unsafe_max_tlab_alloc(Thread *thread) const {
  // Returns size in bytes
  return MIN2(_free_set->unsafe_peek_free(), ShenandoahHeapRegion::max_tlab_size_bytes());
}

size_t ShenandoahHeap::max_tlab_size() const {
  // Returns size in words
  return ShenandoahHeapRegion::max_tlab_size_words();
}

class ShenandoahResizeGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().resize();
  }
};

void ShenandoahHeap::resize_all_tlabs() {
  CollectedHeap::resize_all_tlabs();

  ShenandoahResizeGCLABClosure cl;
  Threads::java_threads_do(&cl);
  _workers->threads_do(&cl);
}

class ShenandoahAccumulateStatisticsGCLABClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {
    assert(thread->gclab().is_initialized(), err_msg("GCLAB should be initialized for %s", thread->name()));
    thread->gclab().accumulate_statistics();
    thread->gclab().initialize_statistics();
  }
};

void ShenandoahHeap::accumulate_statistics_all_gclabs() {
  ShenandoahAccumulateStatisticsGCLABClosure cl;
  Threads::java_threads_do(&cl);
  _workers->threads_do(&cl);
}

void ShenandoahHeap::collect(GCCause::Cause cause) {
  _control_thread->request_gc(cause);
}

void ShenandoahHeap::do_full_collection(bool clear_all_soft_refs) {
  //assert(false, "Shouldn't need to do full collections");
}

CollectorPolicy* ShenandoahHeap::collector_policy() const {
  return _shenandoah_policy;
}

void ShenandoahHeap::resize_tlabs() {
  CollectedHeap::resize_all_tlabs();
}

void ShenandoahHeap::accumulate_statistics_tlabs() {
  CollectedHeap::accumulate_statistics_all_tlabs();
}

HeapWord* ShenandoahHeap::block_start(const void* addr) const {
  ShenandoahHeapRegion* r = heap_region_containing(addr);
  if (r != NULL) {
    return r->block_start(addr);
  }
  return NULL;
}

size_t ShenandoahHeap::block_size(const HeapWord* addr) const {
  ShenandoahHeapRegion* r = heap_region_containing(addr);
  return r->block_size(addr);
}

bool ShenandoahHeap::block_is_obj(const HeapWord* addr) const {
  ShenandoahHeapRegion* r = heap_region_containing(addr);
  return r->block_is_obj(addr);
}

jlong ShenandoahHeap::millis_since_last_gc() {
  double v = heuristics()->time_since_last_gc() * 1000;
  assert(0 <= v && v <= max_jlong, err_msg("value should fit: %f", v));
  return (jlong)v;
}

void ShenandoahHeap::prepare_for_verify() {
  if (SafepointSynchronize::is_at_safepoint()) {
    make_parsable(false);
  }
}

void ShenandoahHeap::print_gc_threads_on(outputStream* st) const {
  workers()->print_worker_threads_on(st);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::print_worker_threads_on(st);
  }
}

void ShenandoahHeap::gc_threads_do(ThreadClosure* tcl) const {
  workers()->threads_do(tcl);
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::threads_do(tcl);
  }
}

void ShenandoahHeap::print_tracing_info() const {
  if (PrintGC || TraceGen0Time || TraceGen1Time) {
    ResourceMark rm;
    outputStream* out = gclog_or_tty;
    phase_timings()->print_global_on(out);

    out->cr();
    out->cr();

    shenandoah_policy()->print_gc_stats(out);

    out->cr();
    out->cr();
  }
}

void ShenandoahHeap::verify(bool silent, VerifyOption vo) {
  if (ShenandoahSafepoint::is_at_shenandoah_safepoint() || ! UseTLAB) {
    if (ShenandoahVerify) {
      verifier()->verify_generic(vo);
    } else {
      // TODO: Consider allocating verification bitmaps on demand,
      // and turn this on unconditionally.
    }
  }
}
size_t ShenandoahHeap::tlab_capacity(Thread *thr) const {
  return _free_set->capacity();
}

class ObjectIterateScanRootClosure : public ExtendedOopClosure {
private:
  MarkBitMap* _bitmap;
  Stack<oop,mtGC>* _oop_stack;

  template <class T>
  void do_oop_work(T* p) {
    T o = oopDesc::load_heap_oop(p);
    if (!oopDesc::is_null(o)) {
      oop obj = oopDesc::decode_heap_oop_not_null(o);
      obj = (oop) ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      assert(obj->is_oop(), "must be a valid oop");
      if (!_bitmap->isMarked((HeapWord*) obj)) {
        _bitmap->mark((HeapWord*) obj);
        _oop_stack->push(obj);
      }
    }
  }
public:
  ObjectIterateScanRootClosure(MarkBitMap* bitmap, Stack<oop,mtGC>* oop_stack) :
    _bitmap(bitmap), _oop_stack(oop_stack) {}
  void do_oop(oop* p)       { do_oop_work(p); }
  void do_oop(narrowOop* p) { do_oop_work(p); }
};

/*
 * This is public API, used in preparation of object_iterate().
 * Since we don't do linear scan of heap in object_iterate() (see comment below), we don't
 * need to make the heap parsable. For Shenandoah-internal linear heap scans that we can
 * control, we call SH::make_parsable().
 */
void ShenandoahHeap::ensure_parsability(bool retire_tlabs) {
  // No-op.
}

/*
 * Iterates objects in the heap. This is public API, used for, e.g., heap dumping.
 *
 * We cannot safely iterate objects by doing a linear scan at random points in time. Linear
 * scanning needs to deal with dead objects, which may have dead Klass* pointers (e.g.
 * calling oopDesc::size() would crash) or dangling reference fields (crashes) etc. Linear
 * scanning therefore depends on having a valid marking bitmap to support it. However, we only
 * have a valid marking bitmap after successful marking. In particular, we *don't* have a valid
 * marking bitmap during marking, after aborted marking or during/after cleanup (when we just
 * wiped the bitmap in preparation for next marking).
 *
 * For all those reasons, we implement object iteration as a single marking traversal, reporting
 * objects as we mark+traverse through the heap, starting from GC roots. JVMTI IterateThroughHeap
 * is allowed to report dead objects, but is not required to do so.
 */
void ShenandoahHeap::object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  if (!_aux_bitmap_region_special && !os::commit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size(), false)) {
    log_warning(gc)("Could not commit native memory for auxiliary marking bitmap for heap iteration");
    return;
  }

  // Reset bitmap
  _aux_bit_map.clear();

  Stack<oop,mtGC> oop_stack;
 
  ObjectIterateScanRootClosure oops(&_aux_bit_map, &oop_stack);
 
  {
    // First, we process GC roots according to current GC cycle.
    // This populates the work stack with initial objects.
    // It is important to relinquish the associated locks before diving
    // into heap dumper.
    ShenandoahHeapIterationRootScanner rp;
    rp.roots_do(&oops);
  }

  // Work through the oop stack to traverse heap.
  while (! oop_stack.is_empty()) {
    oop obj = oop_stack.pop();
    assert(obj->is_oop(), "must be a valid oop");
    cl->do_object(obj);
    obj->oop_iterate(&oops);
  }

  assert(oop_stack.is_empty(), "should be empty");

  if (!_aux_bitmap_region_special && !os::uncommit_memory((char*)_aux_bitmap_region.start(), _aux_bitmap_region.byte_size())) {
    log_warning(gc)("Could not uncommit native memory for auxiliary marking bitmap for heap iteration");
  }
}

void ShenandoahHeap::safe_object_iterate(ObjectClosure* cl) {
  assert(SafepointSynchronize::is_at_safepoint(), "safe iteration is only available during safepoints");
  object_iterate(cl);
}

void ShenandoahHeap::oop_iterate(ExtendedOopClosure* cl) {
  ObjectToOopClosure cl2(cl);
  object_iterate(&cl2);
}

void  ShenandoahHeap::gc_prologue(bool b) {
  Unimplemented();
}

void  ShenandoahHeap::gc_epilogue(bool b) {
  Unimplemented();
}

void ShenandoahHeap::heap_region_iterate(ShenandoahHeapRegionClosure* blk) const {
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* current = get_region(i);
    blk->heap_region_do(current);
  }
}

class ShenandoahParallelHeapRegionTask : public AbstractGangTask {
private:
  ShenandoahHeap* const _heap;
  ShenandoahHeapRegionClosure* const _blk;

  shenandoah_padding(0);
  volatile jint _index;
  shenandoah_padding(1);

public:
  ShenandoahParallelHeapRegionTask(ShenandoahHeapRegionClosure* blk) :
          AbstractGangTask("Parallel Region Task"),
          _heap(ShenandoahHeap::heap()), _blk(blk), _index(0) {}

  void work(uint worker_id) {
    jint stride = (jint)ShenandoahParallelRegionStride;

    jint max = (jint)_heap->num_regions();
    while (_index < max) {
      jint cur = Atomic::add(stride, &_index) - stride;
      jint start = cur;
      jint end = MIN2(cur + stride, max);
      if (start >= max) break;

      for (jint i = cur; i < end; i++) {
        ShenandoahHeapRegion* current = _heap->get_region((size_t)i);
        _blk->heap_region_do(current);
      }
    }
  }
};

void ShenandoahHeap::parallel_heap_region_iterate(ShenandoahHeapRegionClosure* blk) const {
  assert(blk->is_thread_safe(), "Only thread-safe closures here");
  if (num_regions() > ShenandoahParallelRegionStride) {
    ShenandoahParallelHeapRegionTask task(blk);
    workers()->run_task(&task);
  } else {
    heap_region_iterate(blk);
  }
}

class ShenandoahInitMarkUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahInitMarkUpdateRegionStateClosure() : _ctx(ShenandoahHeap::heap()->marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    assert(!r->has_live(),
           err_msg("Region " SIZE_FORMAT " should have no live data", r->index()));
    if (r->is_active()) {
      // Check if region needs updating its TAMS. We have updated it already during concurrent
      // reset, so it is very likely we don't need to do another write here.
      if (_ctx->top_at_mark_start(r) != r->top()) {
        _ctx->capture_top_at_mark_start(r);
      }
    } else {
      assert(_ctx->top_at_mark_start(r) == r->top(),
             err_msg("Region " SIZE_FORMAT " should already have correct TAMS", r->index()));
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahHeap::op_init_mark() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(Thread::current()->is_VM_thread(), "can only do this in VMThread");

  assert(marking_context()->is_bitmap_clear(), "need clear marking bitmap");
  assert(!marking_context()->is_complete(), "should not be complete");
  assert(!has_forwarded_objects(), "No forwarded objects on this path");

  if (ShenandoahVerify) {
    verifier()->verify_before_concmark();
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::accumulate_stats);
    accumulate_statistics_tlabs();
  }

  if (VerifyBeforeGC) {
    Universe::verify();
  }

  set_concurrent_mark_in_progress(true);
  // We need to reset all TLABs because we'd lose marks on all objects allocated in them.
  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::make_parsable);
    make_parsable(true);
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_region_states);
    ShenandoahInitMarkUpdateRegionStateClosure cl;
    parallel_heap_region_iterate(&cl);
  }

  // Make above changes visible to worker threads
  OrderAccess::fence();

  concurrent_mark()->mark_roots(ShenandoahPhaseTimings::scan_roots);

  if (UseTLAB) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::resize_tlabs);
    resize_tlabs();
  }

  if (ShenandoahPacing) {
    pacer()->setup_for_mark();
  }
}

void ShenandoahHeap::op_mark() {
  concurrent_mark()->mark_from_roots();
}

class ShenandoahFinalMarkUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
  ShenandoahHeapLock* const _lock;

public:
  ShenandoahFinalMarkUpdateRegionStateClosure() :
    _ctx(ShenandoahHeap::heap()->complete_marking_context()), _lock(ShenandoahHeap::heap()->lock()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    if (r->is_active()) {
      // All allocations past TAMS are implicitly live, adjust the region data.
      // Bitmaps/TAMS are swapped at this point, so we need to poll complete bitmap.
      HeapWord *tams = _ctx->top_at_mark_start(r);
      HeapWord *top = r->top();
      if (top > tams) {
        r->increase_live_data_alloc_words(pointer_delta(top, tams));
      }

      // We are about to select the collection set, make sure it knows about
      // current pinning status. Also, this allows trashing more regions that
      // now have their pinning status dropped.
      if (r->is_pinned()) {
        if (r->pin_count() == 0) {
          ShenandoahHeapLocker locker(_lock);
          r->make_unpinned();
        }
      } else {
        if (r->pin_count() > 0) {
          ShenandoahHeapLocker locker(_lock);
          r->make_pinned();
        }
      }

      // Remember limit for updating refs. It's guaranteed that we get no
      // from-space-refs written from here on.
      r->set_update_watermark_at_safepoint(r->top());
    } else {
      assert(!r->has_live(),
             err_msg("Region " SIZE_FORMAT " should have no live data", r->index()));
      assert(_ctx->top_at_mark_start(r) == r->top(),
             err_msg("Region " SIZE_FORMAT " should have correct TAMS", r->index()));
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahHeap::op_final_mark() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should be at safepoint");
  assert(!has_forwarded_objects(), "No forwarded objects on this path");

  // It is critical that we
  // evacuate roots right after finishing marking, so that we don't
  // get unmarked objects in the roots.

  if (!cancelled_gc()) {
    concurrent_mark()->finish_mark_from_roots(/* full_gc = */ false);

    TASKQUEUE_STATS_ONLY(concurrent_mark()->task_queues()->reset_taskqueue_stats());

    if (ShenandoahVerify) {
      verifier()->verify_roots_no_forwarded();
    }

    TASKQUEUE_STATS_ONLY(concurrent_mark()->task_queues()->print_taskqueue_stats());

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_region_states);
      ShenandoahFinalMarkUpdateRegionStateClosure cl;
      parallel_heap_region_iterate(&cl);

      assert_pinned_region_status();
    }

    // Force the threads to reacquire their TLABs outside the collection set.
    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::retire_tlabs);
      make_parsable(true);
    }

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::choose_cset);
      ShenandoahHeapLocker locker(lock());
      _collection_set->clear();
      heuristics()->choose_collection_set(_collection_set);
    }

    {
      ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_rebuild_freeset);
      ShenandoahHeapLocker locker(lock());
      _free_set->rebuild();
    }

    // If collection set has candidates, start evacuation.
    // Otherwise, bypass the rest of the cycle.
    if (!collection_set()->is_empty()) {
      ShenandoahGCPhase init_evac(ShenandoahPhaseTimings::init_evac);

      if (ShenandoahVerify) {
        verifier()->verify_before_evacuation();
      }

      set_evacuation_in_progress(true);
      // From here on, we need to update references.
      set_has_forwarded_objects(true);

      if (!is_degenerated_gc_in_progress()) {
        evacuate_and_update_roots();
      }
 
      if (ShenandoahPacing) {
        pacer()->setup_for_evac();
      }

      if (ShenandoahVerify) {
        verifier()->verify_roots_no_forwarded();
        verifier()->verify_during_evacuation();
      }
    } else {
      if (ShenandoahVerify) {
        verifier()->verify_after_concmark();
      }

      if (VerifyAfterGC) {
        Universe::verify();
      }
    }

  } else {
    concurrent_mark()->cancel();
    complete_marking();

    if (process_references()) {
      // Abandon reference processing right away: pre-cleaning must have failed.
      ReferenceProcessor *rp = ref_processor();
      rp->disable_discovery();
      rp->abandon_partial_discovery();
      rp->verify_no_references_recorded();
    }
  }
}

void ShenandoahHeap::op_conc_evac() {
  ShenandoahEvacuationTask task(this, _collection_set, true);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_stw_evac() {
  ShenandoahEvacuationTask task(this, _collection_set, false);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_updaterefs() {
  update_heap_references(true);
}

void ShenandoahHeap::op_cleanup_early() {
  free_set()->recycle_trash();
}

void ShenandoahHeap::op_cleanup_complete() {
  free_set()->recycle_trash();
}

class ShenandoahResetUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahMarkingContext* const _ctx;
public:
  ShenandoahResetUpdateRegionStateClosure() : _ctx(ShenandoahHeap::heap()->marking_context()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    if (r->is_active()) {
      // Reset live data and set TAMS optimistically. We would recheck these under the pause
      // anyway to capture any updates that happened since now.
      r->clear_live_data();
      _ctx->capture_top_at_mark_start(r);
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahHeap::op_reset() {
  if (ShenandoahPacing) {
    pacer()->setup_for_reset();
  }
  reset_mark_bitmap();

  ShenandoahResetUpdateRegionStateClosure cl;
  parallel_heap_region_iterate(&cl);
}

void ShenandoahHeap::op_preclean() {
  if (ShenandoahPacing) {
    pacer()->setup_for_preclean();
  }
  concurrent_mark()->preclean_weak_refs();
}

void ShenandoahHeap::op_full(GCCause::Cause cause) {
  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  full_gc()->do_it(cause);

  metrics.snap_after();

  if (metrics.is_good_progress()) {
    _progress_last_gc.set();
  } else {
    // Nothing to do. Tell the allocation path that we have failed to make
    // progress, and it can finally fail.
    _progress_last_gc.unset();
  }
}

void ShenandoahHeap::op_degenerated(ShenandoahDegenPoint point) {
  // Degenerated GC is STW, but it can also fail. Current mechanics communicates
  // GC failure via cancelled_concgc() flag. So, if we detect the failure after
  // some phase, we have to upgrade the Degenerate GC to Full GC.

  clear_cancelled_gc();

  ShenandoahMetricsSnapshot metrics;
  metrics.snap_before();

  switch (point) {
    // The cases below form the Duff's-like device: it describes the actual GC cycle,
    // but enters it at different points, depending on which concurrent phase had
    // degenerated.

    case _degenerated_outside_cycle:
      // We have degenerated from outside the cycle, which means something is bad with
      // the heap, most probably heavy humongous fragmentation, or we are very low on free
      // space. It makes little sense to wait for Full GC to reclaim as much as it can, when
      // we can do the most aggressive degen cycle, which includes processing references and
      // class unloading, unless those features are explicitly disabled.
      //
      // Note that we can only do this for "outside-cycle" degens, otherwise we would risk
      // changing the cycle parameters mid-cycle during concurrent -> degenerated handover.
      set_process_references(heuristics()->can_process_references());
      set_unload_classes(heuristics()->can_unload_classes());

      op_reset();

      op_init_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

    case _degenerated_mark:
      op_final_mark();
      if (cancelled_gc()) {
        op_degenerated_fail();
        return;
      }

      op_cleanup_early();

    case _degenerated_evac:
      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we can do evacuation. Otherwise, it would be the shortcut cycle.
      if (is_evacuation_in_progress()) {

        // Degeneration under oom-evac protocol might have left some objects in
        // collection set un-evacuated. Restart evacuation from the beginning to
        // capture all objects. For all the objects that are already evacuated,
        // it would be a simple check, which is supposed to be fast. This is also
        // safe to do even without degeneration, as CSet iterator is at beginning
        // in preparation for evacuation anyway.
        //
        // Before doing that, we need to make sure we never had any cset-pinned
        // regions. This may happen if allocation failure happened when evacuating
        // the about-to-be-pinned object, oom-evac protocol left the object in
        // the collection set, and then the pin reached the cset region. If we continue
        // the cycle here, we would trash the cset and alive objects in it. To avoid
        // it, we fail degeneration right away and slide into Full GC to recover.

        {
          sync_pinned_region_status();
          collection_set()->clear_current_index();

          ShenandoahHeapRegion* r;
          while ((r = collection_set()->next()) != NULL) {
            if (r->is_pinned()) {
              cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
              op_degenerated_fail();
              return;
            }
          }

          collection_set()->clear_current_index();
        }

        op_stw_evac();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      // If heuristics thinks we should do the cycle, this flag would be set,
      // and we need to do update-refs. Otherwise, it would be the shortcut cycle.
      if (has_forwarded_objects()) {
        op_init_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

    case _degenerated_updaterefs:
      if (has_forwarded_objects()) {
        op_final_updaterefs();
        if (cancelled_gc()) {
          op_degenerated_fail();
          return;
        }
      }

      op_cleanup_complete();
      break;

    default:
      ShouldNotReachHere();
  }

  if (ShenandoahVerify) {
    verifier()->verify_after_degenerated();
  }

  if (VerifyAfterGC) {
    Universe::verify();
  }

  metrics.snap_after();

  // Check for futility and fail. There is no reason to do several back-to-back Degenerated cycles,
  // because that probably means the heap is overloaded and/or fragmented.
  if (!metrics.is_good_progress()) {
    _progress_last_gc.unset();
    cancel_gc(GCCause::_shenandoah_upgrade_to_full_gc);
    op_degenerated_futile();
  } else {
    _progress_last_gc.set();
  }
}

void ShenandoahHeap::op_degenerated_fail() {
  log_info(gc)("Cannot finish degeneration, upgrading to Full GC");
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::op_degenerated_futile() {
  shenandoah_policy()->record_degenerated_upgrade_to_full();
  op_full(GCCause::_shenandoah_upgrade_to_full_gc);
}

void ShenandoahHeap::complete_marking() {
  if (is_concurrent_mark_in_progress()) {
    set_concurrent_mark_in_progress(false);
  }

  if (!cancelled_gc()) {
    // If we needed to update refs, and concurrent marking has been cancelled,
    // we need to finish updating references.
    set_has_forwarded_objects(false);
    mark_complete_marking_context();
  }
}

void ShenandoahHeap::force_satb_flush_all_threads() {
  if (!is_concurrent_mark_in_progress()) {
    // No need to flush SATBs
    return;
  }

  // Do not block if Threads lock is busy. This avoids the potential deadlock
  // when this code is called from the periodic task, and something else is
  // expecting the periodic task to complete without blocking. On the off-chance
  // Threads lock is busy momentarily, try to acquire several times.
  for (int t = 0; t < 10; t++) {
    if (Threads_lock->try_lock()) {
      JavaThread::set_force_satb_flush_all_threads(true);
      Threads_lock->unlock();

      // The threads are not "acquiring" their thread-local data, but it does not
      // hurt to "release" the updates here anyway.
      OrderAccess::fence();
      break;
    }
    os::naked_short_sleep(1);
  }
}

void ShenandoahHeap::set_gc_state_mask(uint mask, bool value) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Should really be Shenandoah safepoint");
  _gc_state.set_cond(mask, value);
  JavaThread::set_gc_state_all_threads(_gc_state.raw_value());
}

void ShenandoahHeap::set_concurrent_mark_in_progress(bool in_progress) {
  if (has_forwarded_objects()) {
    set_gc_state_mask(MARKING | UPDATEREFS, in_progress);
  } else {
    set_gc_state_mask(MARKING, in_progress);
  }
  JavaThread::satb_mark_queue_set().set_active_all_threads(in_progress, !in_progress);
}

void ShenandoahHeap::set_evacuation_in_progress(bool in_progress) {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "Only call this at safepoint");
  set_gc_state_mask(EVACUATION, in_progress);
}

void ShenandoahHeap::ref_processing_init() {
  MemRegion mr = reserved_region();

  assert(_max_workers > 0, "Sanity");

  bool mt_processing = ParallelRefProcEnabled && (ParallelGCThreads > 1);
  bool mt_discovery = _max_workers > 1;

  _ref_processor =
    new ReferenceProcessor(mr,    // span
                           mt_processing,           // MT processing
                           _max_workers,            // Degree of MT processing
                           mt_discovery,            // MT discovery
                           _max_workers,            // Degree of MT discovery
                           false,                   // Reference discovery is not atomic
                           NULL);                   // No closure, should be installed before use

  log_info(gc, init)("Reference processing: %s discovery, %s processing",
          mt_discovery ? "parallel" : "serial",
          mt_processing ? "parallel" : "serial");

  shenandoah_assert_rp_isalive_not_installed();
}

void ShenandoahHeap::acquire_pending_refs_lock() {
  _control_thread->slt()->manipulatePLL(SurrogateLockerThread::acquirePLL);
}

void ShenandoahHeap::release_pending_refs_lock() {
  _control_thread->slt()->manipulatePLL(SurrogateLockerThread::releaseAndNotifyPLL);
}

GCTracer* ShenandoahHeap::tracer() {
  return shenandoah_policy()->tracer();
}

size_t ShenandoahHeap::tlab_used(Thread* thread) const {
  return _free_set->used();
}

void ShenandoahHeap::cancel_gc(GCCause::Cause cause) {
  if (try_cancel_gc()) {
    FormatBuffer<> msg("Cancelling GC: %s", GCCause::to_string(cause));
    log_info(gc)("%s", msg.buffer());
    Events::log(Thread::current(), "%s", msg.buffer());
  }
}

uint ShenandoahHeap::max_workers() {
  return _max_workers;
}

void ShenandoahHeap::stop() {
  // The shutdown sequence should be able to terminate when GC is running.

  // Step 0. Notify policy to disable event recording.
  _shenandoah_policy->record_shutdown();

  // Step 1. Notify control thread that we are in shutdown.
  // Note that we cannot do that with stop(), because stop() is blocking and waits for the actual shutdown.
  // Doing stop() here would wait for the normal GC cycle to complete, never falling through to cancel below.
  _control_thread->prepare_for_graceful_shutdown();

  // Step 2. Notify GC workers that we are cancelling GC.
  cancel_gc(GCCause::_shenandoah_stop_vm);

  // Step 3. Wait until GC worker exits normally.
  _control_thread->stop();

  // Step 4. Stop String Dedup thread if it is active
  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahStringDedup::stop();
  }
}

void ShenandoahHeap::unload_classes_and_cleanup_tables(bool full_gc) {
  assert(heuristics()->can_unload_classes(), "Class unloading should be enabled");

  ShenandoahGCPhase root_phase(full_gc ?
                               ShenandoahPhaseTimings::full_gc_purge :
                               ShenandoahPhaseTimings::purge);

  ShenandoahIsAliveSelector alive;
  BoolObjectClosure* is_alive = alive.is_alive_closure();

  // Cleaning of klasses depends on correct information from MetadataMarkOnStack. The CodeCache::mark_on_stack
  // part is too slow to be done serially, so it is handled during the ShenandoahParallelCleaning phase.
  // Defer the cleaning until we have complete on_stack data.
  MetadataOnStackMark md_on_stack(false /* Don't visit the code cache at this point */);

  bool purged_class;

  // Unload classes and purge SystemDictionary.
  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_class_unload :
                            ShenandoahPhaseTimings::purge_class_unload);
    purged_class = SystemDictionary::do_unloading(is_alive,
                                                  false /* Defer klass cleaning */);
  }
  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_par :
                            ShenandoahPhaseTimings::purge_par);
    uint active = _workers->active_workers();
    ShenandoahParallelCleaningTask unlink_task(is_alive, true, true, active, purged_class);
    _workers->run_task(&unlink_task);
  }

  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_metadata :
                            ShenandoahPhaseTimings::purge_metadata);
    ClassLoaderDataGraph::free_deallocate_lists();
  }

  if (ShenandoahStringDedup::is_enabled()) {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_string_dedup :
                            ShenandoahPhaseTimings::purge_string_dedup);
    ShenandoahStringDedup::parallel_cleanup();
  }

  {
    ShenandoahGCPhase phase(full_gc ?
                            ShenandoahPhaseTimings::full_gc_purge_cldg :
                            ShenandoahPhaseTimings::purge_cldg);
    ClassLoaderDataGraph::purge();
  }
}

void ShenandoahHeap::set_has_forwarded_objects(bool cond) {
  set_gc_state_mask(HAS_FORWARDED, cond);
}

void ShenandoahHeap::set_process_references(bool pr) {
  _process_references.set_cond(pr);
}

void ShenandoahHeap::set_unload_classes(bool uc) {
  _unload_classes.set_cond(uc);
}

bool ShenandoahHeap::process_references() const {
  return _process_references.is_set();
}

bool ShenandoahHeap::unload_classes() const {
  return _unload_classes.is_set();
}

address ShenandoahHeap::in_cset_fast_test_addr() {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  assert(heap->collection_set() != NULL, "Sanity");
  return (address) heap->collection_set()->biased_map_address();
}

address ShenandoahHeap::cancelled_gc_addr() {
  return (address) ShenandoahHeap::heap()->_cancelled_gc.addr_of();
}

address ShenandoahHeap::gc_state_addr() {
  return (address) ShenandoahHeap::heap()->_gc_state.addr_of();
}

size_t ShenandoahHeap::conservative_max_heap_alignment() {
  size_t align = ShenandoahMaxRegionSize;
  if (UseLargePages) {
    align = MAX2(align, os::large_page_size());
  }
  return align;
}

size_t ShenandoahHeap::bytes_allocated_since_gc_start() {
  return OrderAccess::load_acquire(&_bytes_allocated_since_gc_start);
}

void ShenandoahHeap::reset_bytes_allocated_since_gc_start() {
  OrderAccess::release_store_fence(&_bytes_allocated_since_gc_start, (size_t)0);
}

void ShenandoahHeap::set_degenerated_gc_in_progress(bool in_progress) {
  _degenerated_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_in_progress(bool in_progress) {
  _full_gc_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_full_gc_move_in_progress(bool in_progress) {
  assert (is_full_gc_in_progress(), "should be");
  _full_gc_move_in_progress.set_cond(in_progress);
}

void ShenandoahHeap::set_update_refs_in_progress(bool in_progress) {
  set_gc_state_mask(UPDATEREFS, in_progress);
}

void ShenandoahHeap::register_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::add_nmethod(nm);
}

void ShenandoahHeap::unregister_nmethod(nmethod* nm) {
  ShenandoahCodeRoots::remove_nmethod(nm);
}

oop ShenandoahHeap::pin_object(JavaThread* thr, oop o) {
  heap_region_containing(o)->record_pin();
  return o;
}

void ShenandoahHeap::unpin_object(JavaThread* thr, oop o) {
  heap_region_containing(o)->record_unpin();
}

void ShenandoahHeap::sync_pinned_region_status() {
  ShenandoahHeapLocker locker(lock());

  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion *r = get_region(i);
    if (r->is_active()) {
      if (r->is_pinned()) {
        if (r->pin_count() == 0) {
          r->make_unpinned();
        }
      } else {
        if (r->pin_count() > 0) {
          r->make_pinned();
        }
      }
    }
  }

  assert_pinned_region_status();
}

#ifdef ASSERT
void ShenandoahHeap::assert_pinned_region_status() {
  for (size_t i = 0; i < num_regions(); i++) {
    ShenandoahHeapRegion* r = get_region(i);
    assert((r->is_pinned() && r->pin_count() > 0) || (!r->is_pinned() && r->pin_count() == 0),
           err_msg("Region " SIZE_FORMAT " pinning status is inconsistent", i));
  }
}
#endif

GCTimer* ShenandoahHeap::gc_timer() const {
  return _gc_timer;
}

#ifdef ASSERT
void ShenandoahHeap::assert_gc_workers(uint nworkers) {
  assert(nworkers > 0 && nworkers <= max_workers(), "Sanity");

  if (ShenandoahSafepoint::is_at_shenandoah_safepoint()) {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ParallelGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ParallelGCThreads, "Cannot use more than it has");
    } else {
      // Use ParallelGCThreads inside safepoints
      assert(nworkers == ParallelGCThreads, "Use ParalleGCThreads within safepoints");
    }
  } else {
    if (UseDynamicNumberOfGCThreads ||
        (FLAG_IS_DEFAULT(ConcGCThreads) && ForceDynamicNumberOfGCThreads)) {
      assert(nworkers <= ConcGCThreads, "Cannot use more than it has");
    } else {
      // Use ConcGCThreads outside safepoints
      assert(nworkers == ConcGCThreads, "Use ConcGCThreads outside safepoints");
    }
  }
}
#endif

ShenandoahVerifier* ShenandoahHeap::verifier() {
  guarantee(ShenandoahVerify, "Should be enabled");
  assert (_verifier != NULL, "sanity");
  return _verifier;
}

ShenandoahUpdateHeapRefsClosure::ShenandoahUpdateHeapRefsClosure() :
  _heap(ShenandoahHeap::heap()) {}

class ShenandoahUpdateHeapRefsTask : public AbstractGangTask {
private:
  ShenandoahHeap* _heap;
  ShenandoahRegionIterator* _regions;
  bool _concurrent;

public:
  ShenandoahUpdateHeapRefsTask(ShenandoahRegionIterator* regions, bool concurrent) :
    AbstractGangTask("Concurrent Update References Task"),
    _heap(ShenandoahHeap::heap()),
    _regions(regions),
    _concurrent(concurrent) {
  }

  void work(uint worker_id) {
    ShenandoahConcurrentWorkerSession worker_session(worker_id);
    ShenandoahUpdateHeapRefsClosure cl;
    ShenandoahHeapRegion* r = _regions->next();
    ShenandoahMarkingContext* const ctx = _heap->complete_marking_context();
    while (r != NULL) {
      HeapWord* update_watermark = r->get_update_watermark();
      assert (update_watermark >= r->bottom(), "sanity");
      if (r->is_active() && !r->is_cset()) {
        _heap->marked_object_oop_iterate(r, &cl, update_watermark);
      }
      if (ShenandoahPacing) {
        _heap->pacer()->report_updaterefs(pointer_delta(update_watermark, r->bottom()));
      }
      if (_heap->cancelled_gc()) {
        return;
      }
      r = _regions->next();
    }
  }
};

void ShenandoahHeap::update_heap_references(bool concurrent) {
  ShenandoahUpdateHeapRefsTask task(&_update_refs_iterator, concurrent);
  workers()->run_task(&task);
}

void ShenandoahHeap::op_init_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  set_evacuation_in_progress(false);

  if (ShenandoahVerify) {
    if (!is_degenerated_gc_in_progress()) {
      verifier()->verify_roots_no_forwarded_except(ShenandoahRootVerifier::ThreadRoots);
    }
    verifier()->verify_before_updaterefs();
  }

  set_update_refs_in_progress(true);

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs_prepare);

    make_parsable(true);

    // Reset iterator.
    _update_refs_iterator.reset();
  }

  if (ShenandoahPacing) {
    pacer()->setup_for_updaterefs();
  }
}

class ShenandoahFinalUpdateRefsUpdateRegionStateClosure : public ShenandoahHeapRegionClosure {
private:
  ShenandoahHeapLock* const _lock;

public:
  ShenandoahFinalUpdateRefsUpdateRegionStateClosure() : _lock(ShenandoahHeap::heap()->lock()) {}

  void heap_region_do(ShenandoahHeapRegion* r) {
    // Drop unnecessary "pinned" state from regions that does not have CP marks
    // anymore, as this would allow trashing them.

    if (r->is_active()) {
      if (r->is_pinned()) {
        if (r->pin_count() == 0) {
          ShenandoahHeapLocker locker(_lock);
          r->make_unpinned();
        }
      } else {
        if (r->pin_count() > 0) {
          ShenandoahHeapLocker locker(_lock);
          r->make_pinned();
        }
      }
    }
  }

  bool is_thread_safe() { return true; }
};

void ShenandoahHeap::op_final_updaterefs() {
  assert(ShenandoahSafepoint::is_at_shenandoah_safepoint(), "must be at safepoint");

  // Check if there is left-over work, and finish it
  if (_update_refs_iterator.has_next()) {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_finish_work);

    // Finish updating references where we left off.
    clear_cancelled_gc();
    update_heap_references(false);
  }

  // Clear cancelled GC, if set. On cancellation path, the block before would handle
  // everything. On degenerated paths, cancelled gc would not be set anyway.
  if (cancelled_gc()) {
    clear_cancelled_gc();
  }
  assert(!cancelled_gc(), "Should have been done right before");

  if (ShenandoahVerify && !is_degenerated_gc_in_progress()) {
    verifier()->verify_roots_no_forwarded_except(ShenandoahRootVerifier::ThreadRoots);
  }

  if (is_degenerated_gc_in_progress()) {
    concurrent_mark()->update_roots(ShenandoahPhaseTimings::degen_gc_update_roots);
  } else {
    concurrent_mark()->update_thread_roots(ShenandoahPhaseTimings::final_update_refs_roots);
  }

  // Has to be done before cset is clear
  if (ShenandoahVerify) {
    verifier()->verify_roots_in_to_space();
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_trash_cset);
    trash_cset_regions();
  }

  set_has_forwarded_objects(false);
  set_update_refs_in_progress(false);

  if (ShenandoahVerify) {
    verifier()->verify_after_updaterefs();
  }

  if (VerifyAfterGC) {
    Universe::verify();
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_update_region_states);
    ShenandoahFinalUpdateRefsUpdateRegionStateClosure cl;
    parallel_heap_region_iterate(&cl);

    assert_pinned_region_status();
  }

  {
    ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_rebuild_freeset);
    ShenandoahHeapLocker locker(lock());
    _free_set->rebuild();
  }
}

void ShenandoahHeap::print_extended_on(outputStream *st) const {
  print_on(st);
  print_heap_regions_on(st);
}

bool ShenandoahHeap::is_bitmap_slice_committed(ShenandoahHeapRegion* r, bool skip_self) {
  size_t slice = r->index() / _bitmap_regions_per_slice;

  size_t regions_from = _bitmap_regions_per_slice * slice;
  size_t regions_to   = MIN2(num_regions(), _bitmap_regions_per_slice * (slice + 1));
  for (size_t g = regions_from; g < regions_to; g++) {
    assert (g / _bitmap_regions_per_slice == slice, "same slice");
    if (skip_self && g == r->index()) continue;
    if (get_region(g)->is_committed()) {
      return true;
    }
  }
  return false;
}

bool ShenandoahHeap::commit_bitmap_slice(ShenandoahHeapRegion* r) {
  shenandoah_assert_heaplocked();

  // Bitmaps in special regions do not need commits
  if (_bitmap_region_special) {
    return true;
  }

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is already committed, meaning the bitmap
    // slice is already committed, we exit right away.
    return true;
  }

  // Commit the bitmap slice:
  size_t slice = r->index() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  char* start = (char*) _bitmap_region.start() + off;

  if (!os::commit_memory(start, len, false)) {
    return false;
  }

  if (AlwaysPreTouch) {
    os::pretouch_memory(start, start + len);
  }

  return true;
}

bool ShenandoahHeap::uncommit_bitmap_slice(ShenandoahHeapRegion *r) {
  shenandoah_assert_heaplocked();

  // Bitmaps in special regions do not need uncommits
  if (_bitmap_region_special) {
    return true;
  }

  if (is_bitmap_slice_committed(r, true)) {
    // Some other region from the group is still committed, meaning the bitmap
    // slice is should stay committed, exit right away.
    return true;
  }

  // Uncommit the bitmap slice:
  size_t slice = r->index() / _bitmap_regions_per_slice;
  size_t off = _bitmap_bytes_per_slice * slice;
  size_t len = _bitmap_bytes_per_slice;
  if (!os::uncommit_memory((char*)_bitmap_region.start() + off, len)) {
    return false;
  }
  return true;
}

void ShenandoahHeap::vmop_entry_init_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitMark op;
  VMThread::execute(&op); // jump to entry_init_mark() under safepoint
}

void ShenandoahHeap::vmop_entry_final_mark() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalMarkStartEvac op;
  VMThread::execute(&op); // jump to entry_final_mark under safepoint
}

void ShenandoahHeap::vmop_entry_init_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahInitUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_final_updaterefs() {
  TraceCollectorStats tcs(monitoring_support()->stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFinalUpdateRefs op;
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_entry_full(GCCause::Cause cause) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc_gross);

  try_inject_alloc_failure();
  VM_ShenandoahFullGC op(cause);
  VMThread::execute(&op);
}

void ShenandoahHeap::vmop_degenerated(ShenandoahDegenPoint point) {
  TraceCollectorStats tcs(monitoring_support()->full_stw_collection_counters());
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc_gross);

  VM_ShenandoahDegeneratedGC degenerated_gc((int)point);
  VMThread::execute(&degenerated_gc);
}

void ShenandoahHeap::entry_init_mark() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_mark);

  const char* msg = init_mark_event_message();
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_init_marking(),
                              "init marking");

  op_init_mark();
}

void ShenandoahHeap::entry_final_mark() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_mark);

  const char* msg = final_mark_event_message();
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_marking(),
                              "final marking");

  op_final_mark();
}

void ShenandoahHeap::entry_init_updaterefs() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::init_update_refs);

  static const char* msg = "Pause Init Update Refs";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  // No workers used in this phase, no setup required

  op_init_updaterefs();
}

void ShenandoahHeap::entry_final_updaterefs() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::final_update_refs);

  static const char* msg = "Pause Final Update Refs";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_final_update_ref(),
                              "final reference update");

  op_final_updaterefs();
}

void ShenandoahHeap::entry_full(GCCause::Cause cause) {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::full_gc);

  static const char* msg = "Pause Full";
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_fullgc(),
                              "full gc");

  op_full(cause);
}

void ShenandoahHeap::entry_degenerated(int point) {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::degen_gc);

  ShenandoahDegenPoint dpoint = (ShenandoahDegenPoint)point;
  const char* msg = degen_event_message(dpoint);
  GCTraceTime time(msg, PrintGC, _gc_timer, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_stw_degenerated(),
                              "stw degenerated gc");

  set_degenerated_gc_in_progress(true);
  op_degenerated(dpoint);
  set_degenerated_gc_in_progress(false);
}

void ShenandoahHeap::entry_mark() {
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  const char* msg = conc_mark_event_message();
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_marking(),
                              "concurrent marking");

  try_inject_alloc_failure();
  op_mark();
}

void ShenandoahHeap::entry_evac() {
  ShenandoahGCPhase conc_evac_phase(ShenandoahPhaseTimings::conc_evac);
  TraceCollectorStats tcs(monitoring_support()->concurrent_collection_counters());

  static const char *msg = "Concurrent evacuation";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_evac(),
                              "concurrent evacuation");

  try_inject_alloc_failure();
  op_conc_evac();
}

void ShenandoahHeap::entry_updaterefs() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_update_refs);

  static const char* msg = "Concurrent update references";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_update_ref(),
                              "concurrent reference update");

  try_inject_alloc_failure();
  op_updaterefs();
}

void ShenandoahHeap::entry_cleanup_early() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup_early);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  // This phase does not use workers, no need for setup

  try_inject_alloc_failure();
  op_cleanup_early();
}

void ShenandoahHeap::entry_cleanup_complete() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_cleanup_complete);

  static const char* msg = "Concurrent cleanup";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  // This phase does not use workers, no need for setup

  try_inject_alloc_failure();
  op_cleanup_complete();
}

void ShenandoahHeap::entry_reset() {
  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_reset);

  static const char* msg = "Concurrent reset";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id());
  EventMark em("%s", msg);

  ShenandoahWorkerScope scope(workers(),
                              ShenandoahWorkerPolicy::calc_workers_for_conc_reset(),
                              "concurrent reset");

  try_inject_alloc_failure();
  op_reset();
}

void ShenandoahHeap::entry_preclean() {
  if (ShenandoahPreclean && process_references()) {
    ShenandoahGCPhase conc_preclean(ShenandoahPhaseTimings::conc_preclean);

    static const char* msg = "Concurrent precleaning";
    GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id());
    EventMark em("%s", msg);

    ShenandoahWorkerScope scope(workers(),
                                ShenandoahWorkerPolicy::calc_workers_for_conc_preclean(),
                                "concurrent preclean",
                                /* check_workers = */ false);

    try_inject_alloc_failure();
    op_preclean();
  }
}

void ShenandoahHeap::entry_uncommit(double shrink_before, size_t shrink_until) {
  static const char *msg = "Concurrent uncommit";
  GCTraceTime time(msg, PrintGC, NULL, tracer()->gc_id(), true);
  EventMark em("%s", msg);

  ShenandoahGCPhase phase(ShenandoahPhaseTimings::conc_uncommit);

  op_uncommit(shrink_before, shrink_until);
}

void ShenandoahHeap::try_inject_alloc_failure() {
  if (ShenandoahAllocFailureALot && !cancelled_gc() && ((os::random() % 1000) > 950)) {
    _inject_alloc_failure.set();
    os::naked_short_sleep(1);
    if (cancelled_gc()) {
      log_info(gc)("Allocation failure was successfully injected");
    }
  }
}

bool ShenandoahHeap::should_inject_alloc_failure() {
  return _inject_alloc_failure.is_set() && _inject_alloc_failure.try_unset();
}

void ShenandoahHeap::enter_evacuation() {
  _oom_evac_handler.enter_evacuation();
}

void ShenandoahHeap::leave_evacuation() {
  _oom_evac_handler.leave_evacuation();
}

ShenandoahRegionIterator::ShenandoahRegionIterator() :
  _heap(ShenandoahHeap::heap()),
  _index(0) {}

ShenandoahRegionIterator::ShenandoahRegionIterator(ShenandoahHeap* heap) :
  _heap(heap),
  _index(0) {}

void ShenandoahRegionIterator::reset() {
  _index = 0;
}

bool ShenandoahRegionIterator::has_next() const {
  return _index < (jint)_heap->num_regions();
}

char ShenandoahHeap::gc_state() {
  return _gc_state.raw_value();
}

const char* ShenandoahHeap::init_mark_event_message() const {
  assert(!has_forwarded_objects(), "Should not have forwarded objects here");

  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (proc_refs && unload_cls) {
    return "Pause Init Mark (process weakrefs) (unload classes)";
  } else if (proc_refs) {
    return "Pause Init Mark (process weakrefs)";
  } else if (unload_cls) {
    return "Pause Init Mark (unload classes)";
  } else {
    return "Pause Init Mark";
  }
}

const char* ShenandoahHeap::final_mark_event_message() const {
  assert(!has_forwarded_objects(), "Should not have forwarded objects here");

  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (proc_refs && unload_cls) {
    return "Pause Final Mark (process weakrefs) (unload classes)";
  } else if (proc_refs) {
    return "Pause Final Mark (process weakrefs)";
  } else if (unload_cls) {
    return "Pause Final Mark (unload classes)";
  } else {
    return "Pause Final Mark";
  }
}

const char* ShenandoahHeap::conc_mark_event_message() const {
  assert(!has_forwarded_objects(), "Should not have forwarded objects here");

  bool proc_refs = process_references();
  bool unload_cls = unload_classes();

  if (proc_refs && unload_cls) {
    return "Concurrent marking (process weakrefs) (unload classes)";
  } else if (proc_refs) {
    return "Concurrent marking (process weakrefs)";
  } else if (unload_cls) {
    return "Concurrent marking (unload classes)";
  } else {
    return "Concurrent marking";
  }
}

const char* ShenandoahHeap::degen_event_message(ShenandoahDegenPoint point) const {
  switch (point) {
    case _degenerated_unset:
      return "Pause Degenerated GC (<UNSET>)";
    case _degenerated_outside_cycle:
      return "Pause Degenerated GC (Outside of Cycle)";
    case _degenerated_mark:
      return "Pause Degenerated GC (Mark)";
    case _degenerated_evac:
      return "Pause Degenerated GC (Evacuation)";
    case _degenerated_updaterefs:
      return "Pause Degenerated GC (Update Refs)";
    default:
      ShouldNotReachHere();
      return "ERROR";
  }
}

ShenandoahLiveData* ShenandoahHeap::get_liveness_cache(uint worker_id) {
#ifdef ASSERT
  assert(_liveness_cache != NULL, "sanity");
  assert(worker_id < _max_workers, "sanity");
  for (uint i = 0; i < num_regions(); i++) {
    assert(_liveness_cache[worker_id][i] == 0, "liveness cache should be empty");
  }
#endif
  return _liveness_cache[worker_id];
}

void ShenandoahHeap::flush_liveness_cache(uint worker_id) {
  assert(worker_id < _max_workers, "sanity");
  assert(_liveness_cache != NULL, "sanity");
  ShenandoahLiveData* ld = _liveness_cache[worker_id];
  for (uint i = 0; i < num_regions(); i++) {
    ShenandoahLiveData live = ld[i];
    if (live > 0) {
      ShenandoahHeapRegion* r = get_region(i);
      r->increase_live_data_gc_words(live);
      ld[i] = 0;
    }
  }
}