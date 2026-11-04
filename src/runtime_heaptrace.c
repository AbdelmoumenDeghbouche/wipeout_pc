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

typedef struct { uint64_t address, size, caller, align; } Block; /* address 0: empty, ~0: removed */
static Block blocks[SLOTS];
static SRWLOCK lock = SRWLOCK_INIT;
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
static void report_gaps(uint64_t wanted) {
    AcquireSRWLockShared(&lock);
    size_t n = 0;
    for (int i = 0; i < SLOTS; ++i) {
        if (blocks[i].address != 0 && blocks[i].address != ~0ull) sorted_blocks[n++] = blocks[i];
    }
    const uint64_t low = span_low, high = span_high;
    ReleaseSRWLockShared(&lock);
    qsort(sorted_blocks, n, sizeof(Block), by_address);
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
    fflush(stdout);
}

static void track(uint64_t address, uint64_t size, uint64_t caller, uint64_t align) {
    if (address < span_low) span_low = address;
    if (address + size > span_high) span_high = address + size;
    for (uint64_t i = slot_of(address), n = 0; n < SLOTS; ++n, i = (i + 1) & (SLOTS - 1)) {
        if (blocks[i].address == 0 || blocks[i].address == ~0ull) {
            blocks[i] = (Block){address, size, caller, align};
            live_bytes += size;
            if (live_bytes > peak_bytes) peak_bytes = live_bytes;
            break;
        }
    }
}


static ABI uint64_t traced_alloc(void *handle, uint64_t size, uint64_t align, uint64_t flags) {
    const uint64_t caller = (uint64_t)__builtin_return_address(0);
    const uint64_t block_size = size & 0xffffffffu;
    const uint64_t address = original_alloc(handle, size, align, flags);
    if (handle != heap_a) return address;
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
            if (n <= 3) report_gaps(block_size);
        }
        return address;
    }
    AcquireSRWLockExclusive(&lock);
    track(address, block_size, caller, align);
    ReleaseSRWLockExclusive(&lock);
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
    if (heap == heap_a_impl && address) {
        AcquireSRWLockExclusive(&lock);
        for (uint64_t i = slot_of(address), n = 0; n < SLOTS; ++n, i = (i + 1) & (SLOTS - 1)) {
            if (blocks[i].address == 0) break;
            if (blocks[i].address == address) {
                const Block block = blocks[i];
                blocks[i].address = ~0ull;
                live_bytes -= block.size;
                freed = block.size;
                if (++removed_slots > SLOTS / 4) rehash();
                break;
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    original_free(heap, address);
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
    /* GOW3_HEAP_TRACE=1: every 10 s, the game code holding the most heap memory. */
    if (getenv("GOW3_HEAP_TRACE")) {
        pthread_t thread;
        pthread_create(&thread, NULL, report_thread, NULL);
        pthread_detach(thread);
        printf("Heap trace: allocations of the GPU heap are traced\n");
    }
}
