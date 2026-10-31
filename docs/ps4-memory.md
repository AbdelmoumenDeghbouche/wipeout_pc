# PS4 and PS4 Pro memory compatibility

The runtime keeps a compatibility budget of 5056 MiB of direct memory for base PS4 and 5568 MiB when `WIPEOUT_OC_NEO=1`. `WIPEOUT_OC_DMEM_MB` can increase the selected budget, up to 16384 MiB; smaller or invalid values fall back to that model's budget. The pre-launch memory warning follows the same rule.

WipEout's Pro path reserves 512 MiB more for graphics. In the earlier allocation investigation, the base budget made its later 200 MiB direct-memory allocation fail. A retry mapped a smaller block and passed it to libc `free`, which rejected the address because it was outside the libc heap. The Pro budget lets the original allocation succeed without a manual override. Pro mode remains experimental; see [video output and validation limits](ps4-pro-video.md).

In v0.2.49, the model-slot correction moves the game's 1396-entry model table to a linked
read-write reserve holding 4096 entries plus a zeroed sentinel. Separately, outside Pro mode,
the 4K, 3440x1440, 3840x1600, 5120x1440, 5120x2160 and 2880x1800 resolution patches give
the game's own video arena 1 GiB more. Neither mechanism increases physical VRAM or changes
the 5056/5568 MiB runtime budgets above. See [render resolutions](ultrawide.md).

These budgets are not raw console RAM or an exact model of the console's allocation. [Mark Cerny's explanation](https://www.gamedeveloper.com/business/inside-the-playstation-4-pro-with-mark-cerny) describes 512 MiB of additional game memory on Pro. The [shadPS4 memory configuration](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/memory.cpp) also accounts for the SDK, extended-memory flags and the requested flexible-memory reservation when deriving direct memory; this port retains its existing fixed split. The tested EU 01.07 executable uses SDK 4.5, declares 370 MiB of flexible memory and declares no extended-memory flags.
