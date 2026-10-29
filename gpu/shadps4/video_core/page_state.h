// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include "common/assert.h"
#include "common/types.h"
#include "core/address_space.h"

namespace VideoCore {

struct PageState {
    // A deep readback queue can put 512 copies on one page, in addition to other watchers.
    // Keep this compact: PageManager has one state per page of the 40-bit address space.
    using WatcherCount = u16;
    WatcherCount num_write_watchers;
    WatcherCount num_read_watchers;

    Core::MemoryPermission WritePerm() const noexcept {
        return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                       : Core::MemoryPermission::None;
    }

    Core::MemoryPermission ReadPerm() const noexcept {
        return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                      : Core::MemoryPermission::None;
    }

    Core::MemoryPermission Perms() const noexcept {
        // A pending readback must block writes too, until its GPU version reaches RAM.
        return num_read_watchers != 0 ? Core::MemoryPermission::None
                                     : ReadPerm() | WritePerm();
    }

    template <s32 delta, bool is_read>
    WatcherCount AddDelta() {
        static_assert(delta >= -1 && delta <= 1);
        auto& count = is_read ? num_read_watchers : num_write_watchers;
        if constexpr (delta == 1) {
            ASSERT_MSG(count < std::numeric_limits<WatcherCount>::max(), "Too many watchers");
            return ++count;
        } else if constexpr (delta == -1) {
            ASSERT_MSG(count > 0, "Not enough watchers");
            return --count;
        } else {
            return count;
        }
    }
};

} // namespace VideoCore
