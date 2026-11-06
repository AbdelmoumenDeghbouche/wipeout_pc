/* SPDX-License-Identifier: GPL-2.0-or-later */
/* gow3: WipEout's GPU heap frees go to the texture cache (gow3gpu_guest_heap_freed), which drops
 * the images over each freed block; with GOW3_HEAP_TRACE=1, also who keeps the heap allocated.
 * The heap object behind the handle at
 * 0x2212548 (Ghidra) grew on every track load until an allocation failed (VR track loads: a
 * memcpy to address 0, device losses writing to 0, a garbled eye). Allocation goes through the
 * handle's vtable (0x19977f0, +0x10: handle, size, alignment), which is hooked; frees are called
 * directly in ~70 places, so the free itself (FUN_017f0f00: heap object, address) jumps here
 * first. Every live block of heap A is kept with the game code that asked for it; a thread
 * prints the callers with the most live memory, and how that changed, every 10 s. */
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

enum { GHIDRA_BASE = 0x1000000, VTABLE = 0x19977f0, HANDLE_A = 0x2212548, HEAP_A = 0x2212568,
       ALLOC_FN = 0x17f0af0, FREE_IMPL = 0x17f0f00, FREE_PROLOGUE = 12 };
enum { SLOTS = 1 << 17, CALLERS = 2048, SHOWN = 12 };
/* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12 */
static const unsigned char free_prologue[FREE_PROLOGUE] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                                           0x41, 0x56, 0x41, 0x55, 0x41, 0x54};

typedef ABI uint64_t (*AllocFn)(void *handle, uint64_t size, uint64_t align, uint64_t flags);
typedef ABI void (*FreeFn)(void *heap, uint64_t address);

static AllocFn original_alloc;
static FreeFn original_free;
static uint64_t image_base;
static void *heap_a;      /* the handle */
static void *heap_a_impl; /* the heap object behind it */

/* seq: the allocation's number (1, 2, ...), to tell what was placed after a hole was freed. */
typedef struct { uint64_t address, size, caller, align, seq; } Block; /* address 0: empty, ~0: removed */
static Block blocks[SLOTS];
static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t alloc_seq; /* under the lock */

/* gow3: freed holes. A 216 MiB eye target failed with 883 MiB free in 6217 gaps, the largest
 * 212 and 210.6 MiB: probably the last track's eye targets, freed at the load, with small
 * blocks then placed inside them. Every freed block of LARGE or more is remembered (under the
 * lock) until a large block is placed over it; a large allocation, and a failed one, say what
 * now sits inside each: how much, placed since by whom, and the longest free run left. */
enum { LARGE = 64 << 20, HOLES = 32, LARGE_REPORTS = 64, HOLE_CALLERS = 16 };
typedef struct { uint64_t address, size, caller, freed_at; } Hole; /* address 0: empty */
static Hole holes[HOLES];
static int next_hole;
static atomic_int large_reports;
static SRWLOCK report_lock = SRWLOCK_INIT; /* the report buffers; taken before the lock */

/* gow3: placement. The game's allocator (FUN_017f0af0) is best fit by size: every block goes
 * to the smallest free region it fits, wherever that is. The menu's render targets (682.7,
 * 800, 400 MiB... at 5020x5120) went back into the race's holes in another arrangement and a
 * 682.7 MiB one failed with 1548 MiB free. Only its alloc and free touch its trees (Ghidra:
 * init FUN_017f7430 once, teardown FUN_0164bfc0 at exit, getters read +0x10/+0x18/+0x24; the
 * free sites read +0 and +0x10 to pick the heap), so the hook places every block itself in
 * [base, base+size) and keeps +0x18 (bytes in use) and +0x24 (blocks) current; the game's
 * trees keep the one free region they were made with.
 * Render targets (FUN_017e8990 asks) go top down, the rest bottom up (address-ordered first
 * fit): each phase asks for the same targets in the same order, so they land where they were;
 * textures fill from the bottom and still use the menu's room in a race.
 * GOW3_HEAP_GAME_PLACEMENT=1 leaves placement to the game. */
enum { RT_FN_START = 0x17e8990, RT_FN_END = 0x17e9520, SPANS = 1 << 16 };
typedef struct { uint64_t start, end; } Span; /* free, by address, never adjacent */
static Span spans[SPANS];
static int span_count;
static int own_heap;     /* placement is the hook's */
static int own_ready;    /* spans hold the heap */

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) / a * a; }
static uint64_t align_down(uint64_t x, uint64_t a) { return x / a * a; }

/* Under the lock: give [at, +size) out of span i. 0 when the span table is full. */
static int take_span(int i, uint64_t at, uint64_t size) {
    const Span s = spans[i];
    const int left = at > s.start, right = at + size < s.end;
    if (left && right) {
        if (span_count == SPANS) return 0;
        memmove(&spans[i + 2], &spans[i + 1], (size_t)(span_count - i - 1) * sizeof(Span));
        spans[i].end = at;
        spans[i + 1] = (Span){at + size, s.end};
        ++span_count;
    } else if (left) {
        spans[i].end = at;
    } else if (right) {
        spans[i].start = at + size;
    } else {
        memmove(&spans[i], &spans[i + 1], (size_t)(span_count - i - 1) * sizeof(Span));
        --span_count;
    }
    return 1;
}

/* Under the lock: an address for size bytes, or 0. */
static uint64_t own_alloc(uint64_t size, uint64_t align, int top) {
    if (!size) return 0; /* the game's gives nothing for 0 bytes too */
    if (!align) align = 1;
    if (top) {
        for (int i = span_count - 1; i >= 0; --i) {
            if (spans[i].end - spans[i].start < size) continue;
            const uint64_t at = align_down(spans[i].end - size, align);
            if (at >= spans[i].start) return take_span(i, at, size) ? at : 0;
        }
    } else {
        for (int i = 0; i < span_count; ++i) {
            const uint64_t at = align_up(spans[i].start, align);
            if (at + size <= spans[i].end && at >= spans[i].start)
                return take_span(i, at, size) ? at : 0;
        }
    }
    return 0;
}

/* Under the lock: [address, +size) is free again, merged with its neighbours. */
static void own_free(uint64_t address, uint64_t size) {
    int lo = 0, hi = span_count; /* first span starting after address */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (spans[mid].start > address) hi = mid; else lo = mid + 1;
    }
    const int prev = lo - 1, next = lo;
    const uint64_t end = address + size;
    if ((prev >= 0 && spans[prev].end > address) || (next < span_count && spans[next].start < end)) {
        printf("Heap: freeing %#llx (%.1f MiB) overlaps free memory; ignored\n",
               (unsigned long long)address, (double)size / 1048576.0);
        return;
    }
    const int join_prev = prev >= 0 && spans[prev].end == address;
    const int join_next = next < span_count && spans[next].start == end;
    if (join_prev && join_next) {
        spans[prev].end = spans[next].end;
        memmove(&spans[next], &spans[next + 1], (size_t)(span_count - next - 1) * sizeof(Span));
        --span_count;
    } else if (join_prev) {
        spans[prev].end = end;
    } else if (join_next) {
        spans[next].start = address;
    } else if (span_count < SPANS) {
        memmove(&spans[next + 1], &spans[next], (size_t)(span_count - next) * sizeof(Span));
        spans[next] = (Span){address, end};
        ++span_count;
    } else {
        printf("Heap: free-region table full; %.1f MiB at %#llx is lost\n",
               (double)size / 1048576.0, (unsigned long long)address);
    }
}

/* Under the lock: the heap object's in-use bytes (+0x18) and block count (+0x24). */
static void own_counters(int64_t bytes, int32_t blocks) {
    unsigned char *h = heap_a_impl;
    uint64_t used; int32_t count;
    memcpy(&used, h + 0x18, 8); memcpy(&count, h + 0x24, 4);
    used += (uint64_t)bytes; count += blocks;
    memcpy(h + 0x18, &used, 8); memcpy(h + 0x24, &count, 4);
}

static int overlaps(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    return a < b + b_size && b < a + a_size;
}
/* Under the lock: forget the holes [address, +size) covers (a large block took their place). */
static void drop_holes(uint64_t address, uint64_t size) {
    for (int k = 0; k < HOLES; ++k)
        if (holes[k].address && overlaps(holes[k].address, holes[k].size, address, size))
            holes[k].address = 0;
}
static atomic_ullong failed_allocs;
/* Live bytes now, and the most there were (under the lock). */
static uint64_t live_bytes, peak_bytes, peak_reported;

static uint64_t slot_of(uint64_t address) {
    return (address >> 8) * 0x9E3779B97F4A7C15ull >> (64 - 17);
}

/* gow3: the lowest address and highest end the heap ever gave (under the lock). */
static uint64_t span_low = ~0ull, span_high;

/* gow3: after a failed allocation, where the free room is. A 440 MiB request failed
 * with 2.9 GB of 4992 MiB free, back to the menu: heavily cut up, or not all of it usable for
 * one block? From the live blocks the hook knows (their requested sizes). */
static Block sorted_blocks[SLOTS];
static int by_address(const void *a, const void *b) {
    const uint64_t x = ((const Block *)a)->address, y = ((const Block *)b)->address;
    return x < y ? -1 : x > y;
}
/* The live blocks by address into sorted_blocks, and the holes; under report_lock. */
static Hole hole_copy[HOLES];
static uint64_t seq_copy;
static size_t snapshot(void) {
    AcquireSRWLockShared(&lock);
    size_t n = 0;
    for (int i = 0; i < SLOTS; ++i) {
        if (blocks[i].address != 0 && blocks[i].address != ~0ull) sorted_blocks[n++] = blocks[i];
    }
    memcpy(hole_copy, holes, sizeof(holes));
    seq_copy = alloc_seq;
    ReleaseSRWLockShared(&lock);
    qsort(sorted_blocks, n, sizeof(Block), by_address);
    return n;
}

/* What sits in each remembered hole, the newest first; all of them, or (all == 0) only those
 * something smaller was placed in. Under report_lock, after snapshot(). */
static void report_holes(size_t n, int all) {
    int order[HOLES], count = 0;
    for (int k = 0; k < HOLES; ++k)
        if (hole_copy[k].address) order[count++] = k;
    for (int a = 1; a < count; ++a) /* newest first */
        for (int b = a; b > 0 && hole_copy[order[b]].freed_at > hole_copy[order[b - 1]].freed_at; --b) {
            const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    if (all) printf("Heap: %d freed holes of %d MiB or more remembered\n", count, LARGE >> 20);
    for (int o = 0; o < count; ++o) {
        const Hole *h = &hole_copy[order[o]];
        const uint64_t end = h->address + h->size;
        uint64_t inside = 0, blocks_in = 0, since = 0, run = 0, cursor = h->address;
        struct { uint64_t caller, bytes, count; } by[HOLE_CALLERS];
        int callers_in = 0;
        uint64_t other_bytes = 0, other_count = 0;
        for (size_t i = 0; i < n; ++i) {
            const Block *b = &sorted_blocks[i];
            if (b->address >= end) break;
            if (!overlaps(b->address, b->size, h->address, h->size)) continue;
            const uint64_t lo = b->address > h->address ? b->address : h->address;
            const uint64_t hi = b->address + b->size < end ? b->address + b->size : end;
            if (lo > cursor && lo - cursor > run) run = lo - cursor;
            if (hi > cursor) cursor = hi;
            inside += hi - lo;
            ++blocks_in;
            since += b->seq > h->freed_at;
            int c = 0;
            while (c < callers_in && by[c].caller != b->caller) ++c;
            if (c == callers_in && callers_in < HOLE_CALLERS) {
                by[callers_in].caller = b->caller;
                by[callers_in].bytes = by[callers_in].count = 0;
                ++callers_in;
            }
            if (c < callers_in) { by[c].bytes += hi - lo; ++by[c].count; }
            else { other_bytes += hi - lo; ++other_count; }
        }
        if (end > cursor && end - cursor > run) run = end - cursor;
        if (!all && !blocks_in) continue;
        printf("Heap:   hole %.1f MiB at %#llx (freed by %#llx, %llu allocations ago): %llu blocks, "
               "%.1f MiB in it (%llu placed since); longest free run %.1f MiB",
               (double)h->size / 1048576.0, (unsigned long long)h->address,
               (unsigned long long)(h->caller - image_base + GHIDRA_BASE),
               (unsigned long long)(seq_copy - h->freed_at), (unsigned long long)blocks_in,
               (double)inside / 1048576.0, (unsigned long long)since, (double)run / 1048576.0);
        for (int shown = 0; shown < 4 && shown < callers_in; ++shown) {
            int best = shown;
            for (int c = shown + 1; c < callers_in; ++c)
                if (by[c].bytes > by[best].bytes) best = c;
            if (best != shown) {
                const __typeof__(by[0]) t = by[best]; by[best] = by[shown]; by[shown] = t;
            }
            printf("%s %#llx %llu blocks %.2f MiB", shown ? "," : "; by",
                   (unsigned long long)(by[shown].caller - image_base + GHIDRA_BASE),
                   (unsigned long long)by[shown].count, (double)by[shown].bytes / 1048576.0);
        }
        for (int c = 4; c < callers_in; ++c) { other_bytes += by[c].bytes; other_count += by[c].count; }
        if (other_count)
            printf(", others %llu blocks %.2f MiB", (unsigned long long)other_count,
                   (double)other_bytes / 1048576.0);
        printf("\n");
    }
}

static void report_gaps(uint64_t wanted) {
    const size_t n = snapshot();
    AcquireSRWLockShared(&lock);
    const uint64_t low = span_low, high = span_high;
    ReleaseSRWLockShared(&lock);
    uint64_t top[5] = {0}, top_at[5] = {0}, free_total = 0, fitting = 0, gaps = 0;
    uint64_t cursor = low;
    for (size_t i = 0; i <= n; ++i) {
        const uint64_t start = i < n ? sorted_blocks[i].address : high;
        if (start > cursor) {
            const uint64_t gap = start - cursor;
            free_total += gap;
            ++gaps;
            fitting += gap >= wanted;
            for (int k = 0; k < 5; ++k) {
                if (gap > top[k]) {
                    for (int j = 4; j > k; --j) { top[j] = top[j - 1]; top_at[j] = top_at[j - 1]; }
                    top[k] = gap;
                    top_at[k] = cursor;
                    break;
                }
            }
        }
        if (i < n && sorted_blocks[i].address + sorted_blocks[i].size > cursor)
            cursor = sorted_blocks[i].address + sorted_blocks[i].size;
    }
    printf("Heap: %zu live blocks in %#llx-%#llx (%.1f MiB spanned); %.1f MiB free in %llu "
           "gaps, %llu of them %.1f MiB or more; largest:", n, (unsigned long long)low,
           (unsigned long long)high, (double)(high - low) / 1048576.0,
           (double)free_total / 1048576.0, (unsigned long long)gaps, (unsigned long long)fitting,
           (double)wanted / 1048576.0);
    for (int k = 0; k < 5 && top[k]; ++k)
        printf(" %.1f MiB at %#llx", (double)top[k] / 1048576.0, (unsigned long long)top_at[k]);
    printf("\n");
    uint64_t fields[8];
    memcpy(fields, heap_a_impl, sizeof(fields));
    printf("Heap: heap object:");
    for (int k = 0; k < 8; ++k) printf(" +%#x=%#llx", k * 8, (unsigned long long)fields[k]);
    printf("\n");
    report_holes(n, 1);
    fflush(stdout);
}

static void track(uint64_t address, uint64_t size, uint64_t caller, uint64_t align) {
    if (address < span_low) span_low = address;
    if (address + size > span_high) span_high = address + size;
    ++alloc_seq;
    for (uint64_t i = slot_of(address), n = 0; n < SLOTS; ++n, i = (i + 1) & (SLOTS - 1)) {
        if (blocks[i].address == 0 || blocks[i].address == ~0ull) {
            blocks[i] = (Block){address, size, caller, align, alloc_seq};
            live_bytes += size;
            if (live_bytes > peak_bytes) peak_bytes = live_bytes;
            break;
        }
    }
}


static ABI uint64_t traced_alloc(void *handle, uint64_t size, uint64_t align, uint64_t flags) {
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const uint64_t block_size = size & 0xffffffffu;
    if (handle != heap_a) return original_alloc(handle, size, align, flags);
    const uint64_t game_caller = caller - image_base + GHIDRA_BASE;
    const int top = game_caller >= RT_FN_START && game_caller < RT_FN_END;
    uint64_t address = 0;
    if (own_heap) {
        AcquireSRWLockExclusive(&lock);
        if (!own_ready) {
            uint64_t base = 0, total = 0;
            memcpy(&base, heap_a_impl, 8);
            memcpy(&total, (const unsigned char *)heap_a_impl + 0x10, 8);
            if (total) {
                spans[0] = (Span){base, base + total};
                span_count = 1;
                own_ready = 1;
                printf("Heap: the hook places heap A's blocks (%.1f MiB at %#llx): render targets "
                       "top down, the rest bottom up\n", (double)total / 1048576.0,
                       (unsigned long long)base);
                fflush(stdout);
            }
        }
        if (own_ready) {
            address = own_alloc(block_size, align & 0xffffffffu, top);
            if (address) own_counters((int64_t)block_size, 1);
        }
        ReleaseSRWLockExclusive(&lock);
        if (!own_ready) address = original_alloc(handle, size, align, flags);
    } else {
        address = original_alloc(handle, size, align, flags);
    }
    if (!address) {
        /* The game draws or copies into what it gets back: a null here is a GPU write to
         * address 0 (device lost) or a memcpy to it. Always said, and at once. */
        const unsigned long long n = atomic_fetch_add(&failed_allocs, 1) + 1;
        AcquireSRWLockShared(&lock);
        const uint64_t live = live_bytes;
        ReleaseSRWLockShared(&lock);
        if (n <= 20) {
            /* The heap object's own totals: +0x10 its size, +0x18 bytes in use. */
            uint64_t heap_total = 0, heap_used = 0;
            memcpy(&heap_total, (const unsigned char *)heap_a_impl + 0x10, 8);
            memcpy(&heap_used, (const unsigned char *)heap_a_impl + 0x18, 8);
            printf("Heap: the game's GPU heap could not give %.1f MiB (alignment %#llx) to "
                   "%#llx; heap %.1f of %.1f MiB in use (%.1f traced); %llu failed so far\n",
                   (double)size / 1048576.0, (unsigned long long)align,
                   (unsigned long long)(caller - image_base + GHIDRA_BASE),
                   (double)heap_used / 1048576.0, (double)heap_total / 1048576.0,
                   (double)live / 1048576.0, n);
            fflush(stdout);
            if (n <= 3) {
                AcquireSRWLockExclusive(&report_lock);
                report_gaps(block_size);
                ReleaseSRWLockExclusive(&report_lock);
            }
        }
        return address;
    }
    Hole reused = {0};
    AcquireSRWLockExclusive(&lock);
    if (block_size >= LARGE) {
        for (int k = 0; k < HOLES; ++k)
            if (holes[k].address && overlaps(holes[k].address, holes[k].size, address, block_size))
                reused = holes[k];
        drop_holes(address, block_size);
    }
    track(address, block_size, caller, align);
    ReleaseSRWLockExclusive(&lock);
    if (block_size >= LARGE && atomic_fetch_add(&large_reports, 1) < LARGE_REPORTS) {
        AcquireSRWLockExclusive(&report_lock);
        printf("Heap: %.1f MiB to %#llx at %#llx%s, ", (double)block_size / 1048576.0,
               (unsigned long long)game_caller, (unsigned long long)address,
               own_heap ? (top ? " (top down)" : " (bottom up)") : "");
        if (reused.address)
            printf("in the %.1f MiB hole at %#llx freed by %#llx\n", (double)reused.size / 1048576.0,
                   (unsigned long long)reused.address,
                   (unsigned long long)(reused.caller - image_base + GHIDRA_BASE));
        else
            printf("not in a freed hole\n");
        report_holes(snapshot(), 0);
        fflush(stdout);
        ReleaseSRWLockExclusive(&report_lock);
    }
    return address;
}

/* Removed slots end no lookup; once many build up (hours of track loads), the live blocks are
 * put back in place so a free does not walk the whole table. Under the lock. */
static uint64_t removed_slots;
static Block rehash_copy[SLOTS];
static void rehash(void) {
    memcpy(rehash_copy, blocks, sizeof(blocks));
    memset(blocks, 0, sizeof(blocks));
    for (int k = 0; k < SLOTS; ++k) {
        if (rehash_copy[k].address == 0 || rehash_copy[k].address == ~0ull) continue;
        for (uint64_t i = slot_of(rehash_copy[k].address), n = 0; n < SLOTS;
             ++n, i = (i + 1) & (SLOTS - 1)) {
            if (blocks[i].address == 0) {
                blocks[i] = rehash_copy[k];
                break;
            }
        }
    }
    removed_slots = 0;
}

static ABI void traced_free(void *heap, uint64_t address) {
    uint64_t freed = 0;
    int known = 0;
    if (heap == heap_a_impl && address) {
        AcquireSRWLockExclusive(&lock);
        for (uint64_t i = slot_of(address), n = 0; n < SLOTS; ++n, i = (i + 1) & (SLOTS - 1)) {
            if (blocks[i].address == 0) break;
            if (blocks[i].address == address) {
                const Block block = blocks[i];
                blocks[i].address = ~0ull;
                live_bytes -= block.size;
                freed = block.size;
                if (block.size >= LARGE) {
                    drop_holes(block.address, block.size);
                    holes[next_hole] = (Hole){block.address, block.size, block.caller, alloc_seq};
                    next_hole = (next_hole + 1) % HOLES;
                }
                if (++removed_slots > SLOTS / 4) rehash();
                known = 1;
                break;
            }
        }
        if (own_ready && known) {
            own_free(address, freed);
            own_counters(-(int64_t)freed, -1);
        }
        ReleaseSRWLockExclusive(&lock);
    }
    if (heap == heap_a_impl && own_ready) {
        /* The game's trees never held this block. */
        if (address && !known) {
            static atomic_int unknown;
            if (atomic_fetch_add(&unknown, 1) < 10)
                printf("Heap: free of %#llx, not a block of heap A; ignored\n",
                       (unsigned long long)address);
        }
    } else {
        original_free(heap, address);
    }
    /* The heap never unmaps, so without this the GPU's copies of every track's textures stayed
     * cached after the game let them go (1.3 -> 6.8 GB of images over a few VR races). */
    if (freed) gow3gpu_guest_heap_freed((uintptr_t)address, freed);
}

typedef struct { uint64_t caller, bytes, count, last_bytes; } Caller;
static Caller callers[CALLERS];
static Block copy[SLOTS];

static double mib(uint64_t bytes) { return (double)bytes / 1048576.0; }

static void *report_thread(void *unused) {
    (void)unused;
    for (;;) {
        Sleep(10000);
        AcquireSRWLockExclusive(&lock);
        memcpy(copy, blocks, sizeof(copy));
        const uint64_t peak = peak_bytes;
        peak_bytes = live_bytes; /* each report's peak is that of its own 10 s */
        ReleaseSRWLockExclusive(&lock);
        if (peak > peak_reported) peak_reported = peak;
        for (int c = 0; c < CALLERS; ++c) callers[c].bytes = callers[c].count = 0;
        uint64_t total = 0, live = 0;
        for (int i = 0; i < SLOTS; ++i) {
            if (copy[i].address == 0 || copy[i].address == ~0ull) continue;
            total += copy[i].size;
            ++live;
            const uint64_t key = copy[i].caller;
            for (uint64_t c = (key * 0x9E3779B97F4A7C15ull) >> (64 - 11), n = 0; n < CALLERS;
                 ++n, c = (c + 1) & (CALLERS - 1)) {
                if (callers[c].caller == key || callers[c].caller == 0) {
                    callers[c].caller = key;
                    callers[c].bytes += copy[i].size;
                    ++callers[c].count;
                    break;
                }
            }
        }
        printf("Heap trace: %" PRIu64 " blocks, %.1f MiB live in heap A; most in these 10 s "
               "%.1f MiB, most ever %.1f MiB; %llu failed allocations\n", live, mib(total),
               mib(peak), mib(peak_reported), (unsigned long long)atomic_load(&failed_allocs));
        int shown[SHOWN];
        for (int k = 0; k < SHOWN; ++k) {
            int best = -1;
            for (int c = 0; c < CALLERS; ++c) {
                int taken = 0;
                for (int j = 0; j < k; ++j) taken |= shown[j] == c;
                if (!taken && callers[c].bytes &&
                    (best < 0 || callers[c].bytes > callers[best].bytes))
                    best = c;
            }
            shown[k] = best;
            if (best < 0) break;
            const Caller *c = &callers[best];
            printf("  caller %#" PRIx64 ": %" PRIu64 " blocks, %.1f MiB (%+.1f)\n",
                   c->caller - image_base + GHIDRA_BASE, c->count, mib(c->bytes),
                   mib(c->bytes) - mib(c->last_bytes));
        }
        for (int c = 0; c < CALLERS; ++c) {
            if (callers[c].caller && callers[c].bytes >= callers[c].last_bytes + (8u << 20)) {
                printf("  grew: caller %#" PRIx64 " %+.1f MiB to %.1f MiB (%" PRIu64 " blocks)\n",
                       callers[c].caller - image_base + GHIDRA_BASE,
                       mib(callers[c].bytes) - mib(callers[c].last_bytes), mib(callers[c].bytes),
                       callers[c].count);
            }
            callers[c].last_bytes = callers[c].bytes;
        }
        fflush(stdout);
    }
    return NULL;
}

void runtime_heap_trace_install(unsigned char *image, uint64_t size) {
    image_base = (uint64_t)image;
    const uint64_t vtable = VTABLE - GHIDRA_BASE;
    if (vtable + 0x20 > size || FREE_IMPL - GHIDRA_BASE + FREE_PROLOGUE > size) return;
    uint64_t alloc_ptr;
    memcpy(&alloc_ptr, image + vtable + 0x10, 8);
    unsigned char *free_code = image + (FREE_IMPL - GHIDRA_BASE);
    if (alloc_ptr != image_base + ALLOC_FN - GHIDRA_BASE ||
        memcmp(free_code, free_prologue, FREE_PROLOGUE) != 0) {
        printf("Heap trace: the GPU heap's code is not as expected; not tracing\n");
        return;
    }
    original_alloc = (AllocFn)alloc_ptr;
    heap_a = image + (HANDLE_A - GHIDRA_BASE);
    heap_a_impl = image + (HEAP_A - GHIDRA_BASE);
    /* The free's first instructions, then a jump back past them. */
    unsigned char *trampoline =
        VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return;
    memcpy(trampoline, free_prologue, FREE_PROLOGUE);
    const uint64_t back = (uint64_t)(free_code + FREE_PROLOGUE);
    trampoline[12] = 0x48;
    trampoline[13] = 0xb8; /* mov rax, back */
    memcpy(trampoline + 14, &back, 8);
    trampoline[22] = 0xff;
    trampoline[23] = 0xe0; /* jmp rax */
    FlushInstructionCache(GetCurrentProcess(), trampoline, 24);
    original_free = (FreeFn)(void *)trampoline;
    const uint64_t hooked_alloc = (uint64_t)traced_alloc, hooked_free = (uint64_t)traced_free;
    memcpy(image + vtable + 0x10, &hooked_alloc, 8);
    unsigned char jump[FREE_PROLOGUE] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    memcpy(jump + 2, &hooked_free, 8); /* mov rax, traced_free; jmp rax */
    DWORD old;
    VirtualProtect(free_code, FREE_PROLOGUE, PAGE_EXECUTE_READWRITE, &old);
    memcpy(free_code, jump, FREE_PROLOGUE);
    VirtualProtect(free_code, FREE_PROLOGUE, old, &old);
    FlushInstructionCache(GetCurrentProcess(), free_code, FREE_PROLOGUE);
    const char *game_placement = getenv("GOW3_HEAP_GAME_PLACEMENT");
    own_heap = !(game_placement && *game_placement == '1');
    /* GOW3_HEAP_TRACE=1: every 10 s, the game code holding the most heap memory. */
    if (getenv("GOW3_HEAP_TRACE")) {
        pthread_t thread;
        pthread_create(&thread, NULL, report_thread, NULL);
        pthread_detach(thread);
        printf("Heap trace: allocations of the GPU heap are traced\n");
    }
}
