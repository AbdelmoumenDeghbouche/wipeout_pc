// Asset-free regression using the production page state and readback queue, not test stubs.
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include "video_core/page_state.h"
#include "video_core/texture_cache/readback_queue.h"

using Core::MemoryPermission;
using VideoCore::PageState;

// Disable only these compile-time checks when running against the preserved pre-fix header:
// the same runtime checks must also reject its wrap at 256, not merely a changed type name.
#ifndef PAGE_STATE_PRIOR_WIDTH
using Count = PageState::WatcherCount;
static_assert(std::is_same_v<Count, u16>);
static_assert(sizeof(PageState) == 4);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<1, true>()), Count>);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<-1, true>()), Count>);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<0, true>()), Count>);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<1, false>()), Count>);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<-1, false>()), Count>);
static_assert(std::is_same_v<decltype(std::declval<PageState&>().AddDelta<0, false>()), Count>);
#endif

#define CHECK(condition)                                                                            \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                      \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

static void TestCounts() {
    PageState state{};
    CHECK(state.Perms() == MemoryPermission::ReadWrite);
    CHECK((state.AddDelta<1, false>() == 1));
    CHECK(state.Perms() == MemoryPermission::Read);
    // Includes the old wrap boundaries (256 and 512), plus independent overlapping readers.
    for (unsigned expected = 1; expected <= 1024; ++expected) {
        const auto actual = state.AddDelta<1, true>();
        if (actual != expected) {
            std::fprintf(stderr, "FAIL read count: expected %u, got %u\n", expected,
                         static_cast<unsigned>(actual));
            std::exit(1);
        }
        CHECK((state.AddDelta<0, true>() == expected));
        CHECK(state.ReadPerm() == MemoryPermission::None);
        CHECK(state.WritePerm() == MemoryPermission::None);
        CHECK(state.Perms() == MemoryPermission::None);
    }
    CHECK((state.AddDelta<-1, false>() == 0));
    CHECK(state.WritePerm() == MemoryPermission::Write);
    CHECK(state.Perms() == MemoryPermission::None); // Readers still block writes too.
    CHECK((state.AddDelta<1, false>() == 1));
    for (unsigned remaining = 1024; remaining != 0;) {
        CHECK((state.AddDelta<-1, true>() == --remaining));
        CHECK(state.Perms() == (remaining ? MemoryPermission::None : MemoryPermission::Read));
    }
    CHECK((state.AddDelta<-1, false>() == 0));
    CHECK(state.Perms() == MemoryPermission::ReadWrite);

    // Write watchers have the same width/return-value requirement, independently of readers.
    for (unsigned expected = 1; expected <= 1024; ++expected) {
        CHECK((state.AddDelta<1, false>() == expected));
        CHECK((state.AddDelta<0, false>() == expected));
        CHECK(state.Perms() == MemoryPermission::Read);
    }
    for (unsigned remaining = 1024; remaining != 0;) {
        CHECK((state.AddDelta<-1, false>() == --remaining));
        CHECK(state.Perms() == (remaining ? MemoryPermission::Read : MemoryPermission::ReadWrite));
    }
}

static void TestQueueSequencing() {
    VideoCore::ReadbackQueue<unsigned> queue;
    queue.SetMaxEntries(512);
    PageState state{};
    CHECK((state.AddDelta<1, false>() == 1)); // Independent resource write watcher.
    for (unsigned i = 1; i <= 17; ++i) CHECK((state.AddDelta<1, true>() == i));
    for (unsigned i = 0; i < 512; ++i) {
        // Different byte ranges, but all copies overlap the same protected 4 KiB page.
        queue.Push(0x10000 + i * 4, 4, i + 1, i);
        CHECK((state.AddDelta<1, true>() == i + 18));
        CHECK(state.Perms() == MemoryPermission::None);
    }
    CHECK(!queue.CanFit(1));
    CHECK(queue.RequiredTick(0x10fff, 1) == 512);
    unsigned published = 0;
    auto retire = [&](auto& entry) {
        if (entry.valid) {
            CHECK(entry.payload == published);
            ++published;
            if (entry.watched) state.AddDelta<-1, true>();
        }
    };
    queue.Retire([](auto tick) { return tick <= 256; }, retire);
    CHECK(published == 256);
    CHECK((state.AddDelta<0, true>() == 273));
    CHECK(state.Perms() == MemoryPermission::None);
    unsigned canceled = 0;
    auto cancel = [&](auto& entry) {
        if (entry.watched) state.AddDelta<-1, true>();
        ++canceled;
    };
    queue.Cancel(0x10fff, 1, cancel);
    queue.Cancel(0x10fff, 1, cancel); // Cannot release the same watch twice.
    CHECK(canceled == 256);
    CHECK((state.AddDelta<0, true>() == 17));
    CHECK(state.Perms() == MemoryPermission::None);
    queue.Retire([](auto) { return true; }, retire); // Invalid copies cannot release again.
    CHECK(queue.Empty() && queue.Bytes() == 0);
    CHECK(published == 256);
    CHECK((state.AddDelta<0, true>() == 17));
    for (unsigned remaining = 17; remaining != 0;) {
        CHECK((state.AddDelta<-1, true>() == --remaining));
        CHECK(state.Perms() == (remaining ? MemoryPermission::None : MemoryPermission::Read));
    }
    CHECK((state.AddDelta<-1, false>() == 0));
    CHECK(state.Perms() == MemoryPermission::ReadWrite);

    // Released copies stay valid for publication but must not drop their watchers twice.
    queue.Push(0x10000, 4, 513, 256);
    state.AddDelta<1, true>();
    CHECK(queue.ReleaseOutside(0x10fff, 1, 4096, [&](auto&) {
        state.AddDelta<-1, true>();
    }) == 1);
    CHECK(state.Perms() == MemoryPermission::ReadWrite);
    queue.Retire([](auto) { return true; }, retire);
    CHECK(published == 257 && queue.Empty());
    CHECK((state.AddDelta<0, true>() == 0));
}

#ifndef PAGE_STATE_PRIOR_WIDTH
template <bool is_read>
static void TestLimit() {
    PageState state{};
    for (unsigned expected = 1; expected <= std::numeric_limits<Count>::max(); ++expected) {
        CHECK((state.AddDelta<1, is_read>() == expected));
    }
    CHECK(state.Perms() == (is_read ? MemoryPermission::None : MemoryPermission::Read));
    for (unsigned remaining = std::numeric_limits<Count>::max(); remaining != 0;) {
        CHECK((state.AddDelta<-1, is_read>() == --remaining));
    }
    CHECK(state.Perms() == MemoryPermission::ReadWrite);
}

template <bool is_read>
static void TriggerOverflow() {
    PageState state{};
    for (unsigned i = 0; i <= std::numeric_limits<Count>::max(); ++i) {
        state.AddDelta<1, is_read>();
    }
    std::exit(99); // Real production assert support must terminate first (exit 23).
}
#endif

int main(int argc, char** argv) {
#ifndef PAGE_STATE_PRIOR_WIDTH
    if (argc == 2) {
        const std::string_view mode{argv[1]};
        if (mode == "overflow-read") TriggerOverflow<true>();
        if (mode == "overflow-write") TriggerOverflow<false>();
        PageState state{};
        if (mode == "underflow-read") state.AddDelta<-1, true>();
        if (mode == "underflow-write") state.AddDelta<-1, false>();
        return 99;
    }
    TestLimit<true>();
    TestLimit<false>();
#else
    (void)argc;
    (void)argv;
#endif
    TestCounts();
    TestQueueSequencing();
    std::puts("PASS PageState: 256/512/1024 counts, 512-copy queue + 17 readers + writer, "
              "retire/cancel/release permissions, and 65535 bounds");
}
