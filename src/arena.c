/* ----------------------------------------------------------------------------
Copyright (c) 2019, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
"Arenas" are fixed area's of OS memory from which we can allocate
large blocks (>= MI_ARENA_BLOCK_SIZE, 32MiB). 
In contrast to the rest of mimalloc, the arenas are shared between 
threads and need to be accessed using atomic operations.

Currently arenas are only used to for huge OS page (1GiB) reservations,
otherwise it delegates to direct allocation from the OS.
In the future, we can expose an API to manually add more kinds of arenas 
which is sometimes needed for embedded devices or shared memory for example.
(We can also employ this with WASI or `sbrk` systems to reserve large arenas
 on demand and be able to reuse them efficiently).

The arena allocation needs to be thread safe and we use an atomic
bitmap to allocate. The current implementation of the bitmap can
only do this within a field (`uintptr_t`) so we can allocate at most
blocks of 2GiB (64*32MiB) and no object can cross the boundary. This
can lead to fragmentation but fortunately most objects will be regions
of 256MiB in practice.
-----------------------------------------------------------------------------*/
#include "mimalloc.h"
#include "mimalloc-internal.h"
#include "mimalloc-atomic.h"

#include <string.h>  // memset

#include "bitmap.inc.c"  // atomic bitmap


// os.c
void* _mi_os_alloc_aligned(size_t size, size_t alignment, bool commit, bool* large, mi_os_tld_t* tld);
void  _mi_os_free(void* p, size_t size, mi_stats_t* stats);

void* _mi_os_alloc_huge_os_pages(size_t pages, int numa_node, mi_msecs_t max_secs, size_t* pages_reserved, size_t* psize);
void  _mi_os_free_huge_pages(void* p, size_t size, mi_stats_t* stats);

bool  _mi_os_commit(void* p, size_t size, bool* is_zero, mi_stats_t* stats); 


/* -----------------------------------------------------------
  Arena allocation
----------------------------------------------------------- */


// Block info: bit 0 contains the `in_use` bit, the upper bits the
// size in count of arena blocks.
typedef uintptr_t mi_block_info_t;
#define MI_SEGMENT_ALIGN      MI_SEGMENT_SIZE
#define MI_ARENA_BLOCK_SIZE   MI_SEGMENT_ALIGN         // 64MiB
#define MI_ARENA_MAX_OBJ_SIZE (MI_BITMAP_FIELD_BITS * MI_ARENA_BLOCK_SIZE)  // 4GiB
#define MI_ARENA_MIN_OBJ_SIZE (MI_ARENA_BLOCK_SIZE/2)  // 32MiB
#define MI_MAX_ARENAS         (64)                     // not more than 256 (since we use 8 bits in the memid)

// A memory arena descriptor
typedef struct mi_arena_s {
  uint8_t* start;                         // the start of the memory area
  size_t   block_count;                   // size of the area in arena blocks (of `MI_ARENA_BLOCK_SIZE`)
  size_t   field_count;                   // number of bitmap fields (where `field_count * MI_BITMAP_FIELD_BITS >= block_count`)
  int      numa_node;                     // associated NUMA node
  bool     is_zero_init;                  // is the arena zero initialized?
  bool     is_committed;                  // is the memory committed
  bool     is_large;                      // large OS page allocated
  volatile _Atomic(uintptr_t) search_idx; // optimization to start the search for free blocks
  mi_bitmap_field_t* blocks_dirty;        // are the blocks potentially non-zero?
  mi_bitmap_field_t* blocks_committed;    // if `!is_committed`, are the blocks committed?
  mi_bitmap_field_t  blocks_inuse[1];       // in-place bitmap of in-use blocks (of size `field_count`)
} mi_arena_t;


// The available arenas
static _Atomic(mi_arena_t*) mi_arenas[MI_MAX_ARENAS];
static _Atomic(uintptr_t)   mi_arena_count; // = 0


/* -----------------------------------------------------------
  Arena allocations get a memory id where the lower 8 bits are
  the arena index +1, and the upper bits the block index.
----------------------------------------------------------- */

// Use `0` as a special id for direct OS allocated memory.
#define MI_MEMID_OS   0

static size_t mi_memid_create(size_t arena_index, mi_bitmap_index_t bitmap_index) {
  mi_assert_internal(arena_index < 0xFE);
  mi_assert_internal(((bitmap_index << 8) >> 8) == bitmap_index); // no overflow?
  return ((bitmap_index << 8) | ((arena_index+1) & 0xFF));
}

static void mi_memid_indices(size_t memid, size_t* arena_index, mi_bitmap_index_t* bitmap_index) {
  mi_assert_internal(memid != MI_MEMID_OS);
  *arena_index = (memid & 0xFF) - 1;
  *bitmap_index = (memid >> 8);
}

static size_t mi_block_count_of_size(size_t size) {
  return _mi_divide_up(size, MI_ARENA_BLOCK_SIZE);
}

/* -----------------------------------------------------------
  Thread safe allocation in an arena
----------------------------------------------------------- */
static bool mi_arena_alloc(mi_arena_t* arena, size_t blocks, mi_bitmap_index_t* bitmap_idx) 
{
  const size_t fcount = arena->field_count;
  size_t idx = mi_atomic_read(&arena->search_idx);  // start from last search
  for (size_t visited = 0; visited < fcount; visited++, idx++) {
    if (idx >= fcount) idx = 0;  // wrap around
    if (mi_bitmap_try_find_claim_field(arena->blocks_inuse, idx, blocks, bitmap_idx)) {
      mi_atomic_write(&arena->search_idx, idx);  // start search from here next time
      return true;
    }
  }
  return false;
}


/* -----------------------------------------------------------
  Arena cache
----------------------------------------------------------- */
#define MI_CACHE_MAX (64)  // ~4GiB
#define MI_MAX_NUMA  (16)

#define MI_SLOT_IN_USE ((void*)1)

typedef struct mi_cache_slot_s {
  volatile _Atomic(void*) p;
  volatile size_t     memid;
  volatile mi_msecs_t expire;
  volatile bool       is_committed;
  volatile bool       is_large;
} mi_cache_slot_t;

static mi_cache_slot_t cache[MI_MAX_NUMA][MI_CACHE_MAX];

static void* mi_cache_pop(int numa_node, size_t size, size_t alignment, bool* commit, bool* large, bool* is_zero, size_t* memid, mi_os_tld_t* tld) {
  // only segment blocks
  if (size != MI_SEGMENT_SIZE || alignment > MI_SEGMENT_ALIGN) return NULL;

  // set numa range 
  int numa_min = numa_node;
  int numa_max = numa_min;
  if (numa_node < 0) {
    numa_min = 0;
    numa_max = _mi_os_numa_node_count() % MI_MAX_NUMA;
  }
  else {
    if (numa_node >= MI_MAX_NUMA) numa_node %= MI_MAX_NUMA;
    numa_min = numa_max = numa_node;
  }

  // find a free slot
  mi_cache_slot_t* slot;
  for (int n = numa_min; n <= numa_max; n++) {
    for (int i = 0; i < MI_CACHE_MAX; i++) {
      slot = &cache[n][i];
      void* p = mi_atomic_read_ptr_relaxed(&slot->p);
      if (p > MI_SLOT_IN_USE) { // not NULL or 1
        if (mi_atomic_cas_ptr_weak(&slot->p, MI_SLOT_IN_USE, p)) {
          // claimed
          if (!*large && slot->is_large) {
            // back out again
            mi_atomic_write_ptr(&slot->p, p); // make it available again
          }
          else {
            // keep it
            *memid = slot->memid;
            *large = slot->is_large;
            *is_zero = false;
            bool committed = slot->is_committed;
            mi_atomic_write_ptr(&slot->p, NULL); // set it free
            if (*commit && !committed) {
              bool commit_zero;
              _mi_os_commit(p, MI_SEGMENT_SIZE, &commit_zero, tld->stats);
            }            
            *commit = committed;
            return p;
          }
        }
      }
    }
  }
  return NULL;
}

static void mi_cache_purge(mi_os_tld_t* tld) {
  // TODO: for each numa node instead?
  // if (mi_option_get(mi_option_arena_reset_delay) == 0) return;

  mi_msecs_t now = _mi_clock_now();
  int numa_node = _mi_os_numa_node(NULL);
  if (numa_node > MI_MAX_NUMA) numa_node %= MI_MAX_NUMA;
  mi_cache_slot_t* slot;
  int purged = 0;
  for (int i = 0; i < MI_CACHE_MAX; i++) {
    slot = &cache[numa_node][i];
    void* p = mi_atomic_read_ptr_relaxed(&slot->p);
    if (p > MI_SLOT_IN_USE && !slot->is_committed && !slot->is_large) {
      mi_msecs_t expire = slot->expire;
      if (expire != 0 && now >= expire) {
        // expired, try to claim it
        if (mi_atomic_cas_ptr_weak(&slot->p, MI_SLOT_IN_USE, p)) {
          // claimed! test again
          if (!slot->is_committed && !slot->is_large && now >= slot->expire) {
            _mi_os_decommit(p, MI_SEGMENT_SIZE, tld->stats);
            slot->is_committed = false;
          }
          // and unclaim again
          mi_atomic_write_ptr(&slot->p, p);
          purged++;
          if (purged >= 4) break; // limit to at most 4 decommits per push
        }
      }
    }
  }
}


static bool mi_cache_push(void* start, size_t size, size_t memid, bool is_committed, bool is_large, mi_os_tld_t* tld) 
{
  mi_cache_purge(tld);
  
  // only for segment blocks
  if (size != MI_SEGMENT_SIZE || ((uintptr_t)start % MI_SEGMENT_ALIGN) != 0) return false;
  
  // try to add it to the cache
  int numa_node = _mi_os_numa_node(NULL);
  if (numa_node > MI_MAX_NUMA) numa_node %= MI_MAX_NUMA;
  mi_cache_slot_t* slot;
  for (int i = 0; i < MI_CACHE_MAX; i++) {
    slot = &cache[numa_node][i];
    void* p = mi_atomic_read_ptr_relaxed(&slot->p);
    if (p == NULL) { // free slot
      if (mi_atomic_cas_ptr_weak(&slot->p, MI_SLOT_IN_USE, NULL)) {
        // claimed!
        long delay = mi_option_get(mi_option_arena_reset_delay);
        if (delay == 0 && !is_large) {
          _mi_os_decommit(start, size, tld->stats);
          is_committed = false;
        }
        slot->expire = (is_committed ? 0 : _mi_clock_now() + delay);
        slot->is_committed = is_committed;
        slot->memid = memid;
        slot->is_large = is_large;
        mi_atomic_write_ptr(&slot->p, start); // and make it available;
        return true;
      }
    }
  }
  return false;
}


/* -----------------------------------------------------------
  Arena Allocation
----------------------------------------------------------- */

static void* mi_arena_alloc_from(mi_arena_t* arena, size_t arena_index, size_t needed_bcount, 
                                 bool* commit, bool* large, bool* is_zero, size_t* memid, mi_os_tld_t* tld) 
{
  mi_bitmap_index_t bitmap_index;
  if (!mi_arena_alloc(arena, needed_bcount, &bitmap_index)) return NULL;

  // claimed it! set the dirty bits (todo: no need for an atomic op here?)
  void* p  = arena->start + (mi_bitmap_index_bit(bitmap_index)*MI_ARENA_BLOCK_SIZE);
  *memid   = mi_memid_create(arena_index, bitmap_index);
  *is_zero = mi_bitmap_claim(arena->blocks_dirty, arena->field_count, needed_bcount, bitmap_index, NULL);
  *large   = arena->is_large;
  if (arena->is_committed) {
    // always committed
    *commit = true;
  }
  else if (commit) {
    // ensure commit now
    bool any_uncommitted;
    mi_bitmap_claim(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index, &any_uncommitted);
    if (any_uncommitted) {
      bool commit_zero;
      _mi_os_commit(p, needed_bcount * MI_ARENA_BLOCK_SIZE, &commit_zero, tld->stats);
      if (commit_zero) *is_zero = true;
    }
  }
  else {
    // no need to commit, but check if already fully committed
    *commit = mi_bitmap_is_claimed(arena->blocks_committed, arena->field_count, needed_bcount, bitmap_index);
  }
  return p;
}

void* _mi_arena_alloc_aligned(size_t size, size_t alignment, 
                              bool* commit, bool* large, bool* is_zero, 
                              size_t* memid, mi_os_tld_t* tld) 
{
  mi_assert_internal(commit != NULL && large != NULL && is_zero != NULL && memid != NULL && tld != NULL);
  mi_assert_internal(size > 0);
  *memid   = MI_MEMID_OS;
  *is_zero = false;
  bool default_large = false;
  if (large==NULL) large = &default_large;  // ensure `large != NULL`

  const int numa_node = _mi_os_numa_node(tld); // current numa node

  // try to allocate in an arena if the alignment is small enough
  // and the object is not too large or too small.
  if (alignment <= MI_SEGMENT_ALIGN && 
      size <= MI_ARENA_MAX_OBJ_SIZE && 
      size >= MI_ARENA_MIN_OBJ_SIZE)
  {
    const size_t bcount = mi_block_count_of_size(size);
    
    mi_assert_internal(size <= bcount*MI_ARENA_BLOCK_SIZE);
    // try numa affine allocation
    for (size_t i = 0; i < MI_MAX_ARENAS; i++) {
      mi_arena_t* arena = (mi_arena_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &mi_arenas[i]));
      if (arena==NULL) break; // end reached
      if ((arena->numa_node<0 || arena->numa_node==numa_node) && // numa local?
          (*large || !arena->is_large)) // large OS pages allowed, or arena is not large OS pages
      { 
        void* p = mi_arena_alloc_from(arena, i, bcount, commit, large, is_zero, memid, tld);
        mi_assert_internal((uintptr_t)p % alignment == 0);
        if (p != NULL) return p;
      }
    }
    // try from another numa node instead..
    for (size_t i = 0; i < MI_MAX_ARENAS; i++) {
      mi_arena_t* arena = (mi_arena_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &mi_arenas[i]));
      if (arena==NULL) break; // end reached
      if ((arena->numa_node>=0 && arena->numa_node!=numa_node) && // not numa local!
          (*large || !arena->is_large)) // large OS pages allowed, or arena is not large OS pages
      {
        void* p = mi_arena_alloc_from(arena, i, bcount, commit, large, is_zero, memid, tld);
        mi_assert_internal((uintptr_t)p % alignment == 0);
        if (p != NULL) return p;
      }
    }
  }

  // try to get from the cache 
  void* p = mi_cache_pop(numa_node, size, alignment, commit, large, is_zero, memid, tld);
  if (p != NULL) return p;


  // finally, fall back to the OS
  *is_zero = true;
  *memid   = MI_MEMID_OS;
  return _mi_os_alloc_aligned(size, alignment, *commit, large, tld);
}

void* _mi_arena_alloc(size_t size, bool* commit, bool* large, bool* is_zero, size_t* memid, mi_os_tld_t* tld) 
{
  return _mi_arena_alloc_aligned(size, MI_ARENA_BLOCK_SIZE, commit, large, is_zero, memid, tld);
}

/* -----------------------------------------------------------
  Arena free
----------------------------------------------------------- */

void _mi_arena_free(void* p, size_t size, size_t memid, bool is_committed, bool is_large, mi_os_tld_t* tld) {
  mi_assert_internal(size > 0 && tld->stats != NULL);
  if (p==NULL) return;
  if (size==0) return;

  if (memid == MI_MEMID_OS) {
    // was a direct OS allocation, pass through
    if (!mi_cache_push(p, size, memid, is_committed, is_large, tld)) {
      _mi_os_free(p, size, tld->stats);
    }
  }
  else {
    // allocated in an arena
    size_t arena_idx;
    size_t bitmap_idx;
    mi_memid_indices(memid, &arena_idx, &bitmap_idx);
    mi_assert_internal(arena_idx < MI_MAX_ARENAS);
    mi_arena_t* arena = (mi_arena_t*)mi_atomic_read_ptr_relaxed(mi_atomic_cast(void*, &mi_arenas[arena_idx]));
    mi_assert_internal(arena != NULL);
    if (arena == NULL) {
      _mi_fatal_error("trying to free from non-existent arena: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    mi_assert_internal(arena->field_count > mi_bitmap_index_field(bitmap_idx));
    if (arena->field_count <= mi_bitmap_index_field(bitmap_idx)) {
      _mi_fatal_error("trying to free from non-existent arena block: %p, size %zu, memid: 0x%zx\n", p, size, memid);
      return;
    }
    const size_t blocks = mi_block_count_of_size(size);
    bool ones = mi_bitmap_unclaim(arena->blocks_inuse, arena->field_count, blocks, bitmap_idx);
    if (!ones) {
      _mi_fatal_error("trying to free an already freed block: %p, size %zu\n", p, size);
      return;
    };
  }
}

/* -----------------------------------------------------------
  Add an arena.
----------------------------------------------------------- */

static bool mi_arena_add(mi_arena_t* arena) {
  mi_assert_internal(arena != NULL);
  mi_assert_internal((uintptr_t)arena->start % MI_SEGMENT_ALIGN == 0);
  mi_assert_internal(arena->block_count > 0);
  
  uintptr_t i = mi_atomic_addu(&mi_arena_count,1);
  if (i >= MI_MAX_ARENAS) {
    mi_atomic_subu(&mi_arena_count, 1);
    return false;
  }
  mi_atomic_write_ptr(mi_atomic_cast(void*,&mi_arenas[i]), arena);
  return true;
}


/* -----------------------------------------------------------
  Reserve a huge page arena.
----------------------------------------------------------- */
#include <errno.h> // ENOMEM

// reserve at a specific numa node
int mi_reserve_huge_os_pages_at(size_t pages, int numa_node, size_t timeout_msecs) mi_attr_noexcept {
  if (pages==0) return 0;
  if (numa_node < -1) numa_node = -1;
  if (numa_node >= 0) numa_node = numa_node % _mi_os_numa_node_count();
  size_t hsize = 0;
  size_t pages_reserved = 0;
  void* p = _mi_os_alloc_huge_os_pages(pages, numa_node, timeout_msecs, &pages_reserved, &hsize);
  if (p==NULL || pages_reserved==0) {
    _mi_warning_message("failed to reserve %zu gb huge pages\n", pages);
    return ENOMEM;
  }
  _mi_verbose_message("reserved %zu gb huge pages\n", pages_reserved);
  
  size_t bcount = mi_block_count_of_size(hsize);
  size_t fields = (bcount + MI_BITMAP_FIELD_BITS - 1) / MI_BITMAP_FIELD_BITS;
  size_t asize = sizeof(mi_arena_t) + (2*fields*sizeof(mi_bitmap_field_t));  
  mi_arena_t* arena = (mi_arena_t*)_mi_os_alloc(asize, &_mi_stats_main); // TODO: can we avoid allocating from the OS?
  if (arena == NULL) {
    _mi_os_free_huge_pages(p, hsize, &_mi_stats_main);
    return ENOMEM;
  }
  arena->block_count = bcount;
  arena->field_count = fields;
  arena->start = (uint8_t*)p;  
  arena->numa_node = numa_node; // TODO: or get the current numa node if -1? (now it allows anyone to allocate on -1)
  arena->is_large = true;
  arena->is_zero_init = true;
  arena->is_committed = true;
  arena->search_idx = 0;
  arena->blocks_dirty = &arena->blocks_inuse[bcount];
  arena->blocks_committed = NULL;
  // the bitmaps are already zero initialized due to os_alloc
  // just claim leftover blocks if needed
  size_t post = (fields * MI_BITMAP_FIELD_BITS) - bcount;
  if (post > 0) {
    // don't use leftover bits at the end
    mi_bitmap_index_t postidx = mi_bitmap_index_create(fields - 1, MI_BITMAP_FIELD_BITS - post);
    mi_bitmap_claim(arena->blocks_inuse, fields, post, postidx, NULL); 
  }
  
  mi_arena_add(arena);
  return 0;
}


// reserve huge pages evenly among the given number of numa nodes (or use the available ones as detected)
int mi_reserve_huge_os_pages_interleave(size_t pages, size_t numa_nodes, size_t timeout_msecs) mi_attr_noexcept {
  if (pages == 0) return 0;

  // pages per numa node
  size_t numa_count = (numa_nodes > 0 ? numa_nodes : _mi_os_numa_node_count());
  if (numa_count <= 0) numa_count = 1;
  const size_t pages_per = pages / numa_count;
  const size_t pages_mod = pages % numa_count;
  const size_t timeout_per = (timeout_msecs / numa_count) + 50;
  
  // reserve evenly among numa nodes
  for (size_t numa_node = 0; numa_node < numa_count && pages > 0; numa_node++) {
    size_t node_pages = pages_per;  // can be 0
    if (numa_node < pages_mod) node_pages++;
    int err = mi_reserve_huge_os_pages_at(node_pages, (int)numa_node, timeout_per);
    if (err) return err;
    if (pages < node_pages) {
      pages = 0;
    }
    else {
      pages -= node_pages;
    }
  }

  return 0;
}

int mi_reserve_huge_os_pages(size_t pages, double max_secs, size_t* pages_reserved) mi_attr_noexcept {
  UNUSED(max_secs);
  _mi_warning_message("mi_reserve_huge_os_pages is deprecated: use mi_reserve_huge_os_pages_interleave/at instead\n");
  if (pages_reserved != NULL) *pages_reserved = 0;
  int err = mi_reserve_huge_os_pages_interleave(pages, 0, (size_t)(max_secs * 1000.0));  
  if (err==0 && pages_reserved!=NULL) *pages_reserved = pages;
  return err;
}