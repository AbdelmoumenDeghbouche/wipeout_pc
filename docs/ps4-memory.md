# PS4 and PS4 Pro memory compatibility

The runtime keeps a compatibility budget of 5056 MiB of direct memory for base PS4 and 5568 MiB when `GOW3_NEO=1`. `GOW3_DMEM_MB` can increase the selected budget, up to 16384 MiB; smaller or invalid values fall back to that model's budget. The pre-launch memory warning follows the same rule.

WipEout's Pro path reserves 512 MiB more for graphics. With the base budget, its later 200 MiB direct-memory allocation fails. A retry maps a smaller block and passes it to libc `free`, which rejects the address because it is outside the libc heap. The Pro budget lets the original allocation succeed without a manual override. Pro rendering still has a separate shader translation failure and is not yet playable.

These budgets are not raw console RAM or an exact model of the console's allocation. [Mark Cerny's explanation](https://www.gamedeveloper.com/business/inside-the-playstation-4-pro-with-mark-cerny) describes 512 MiB of additional game memory on Pro. The [shadPS4 memory configuration](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/memory.cpp) also accounts for the SDK, extended-memory flags and the requested flexible-memory reservation when deriving direct memory; this port retains its existing fixed split. The tested EU 01.07 executable uses SDK 4.5, declares 370 MiB of flexible memory and declares no extended-memory flags.
