// SPDX-License-Identifier: GPL-2.0-or-later
// wipeout: the register checksum (Liverpool::HashRegisterPacket) exists so a draw-preparation worker
// can prove its register file equals the GPU thread's at a draw. With no workers nobody reads it,
// and it was 3.4 % of the command thread at WipEout's pack start on the Steam Deck (2026-11-03).
#pragma once

#include "common/types.h"

namespace AmdGpu {

[[nodiscard]] constexpr bool HashRegisters(u32 prep_workers) noexcept {
    return prep_workers > 0;
}

} // namespace AmdGpu
