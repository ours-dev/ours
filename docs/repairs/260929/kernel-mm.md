# kernel-mm outstanding repairs

## 1. A mapping's properties are spelled in two mm enums and translated between them

**Severity**
Low, cleanup. Nothing is wrong today; the cost is friction and a place for the two spellings to drift apart.

**Symptom**
Adding one property, `Sealed`, took edits in four places: `ours::VmOptions` (the ABI), `mm::VmMapOption` (what a caller asks when creating a mapping), `mm::VmaFlags` (what the mapping stores), and the translation between the last two in `VmArea::create_mapping()`. `Pinned` is already spelled in both `VmMapOption` and `VmaFlags`. The permission bits are spelled three times (`VmaFlags::Read/Write/Exec`, `MmuFlags::Readable/Writable/Executable`, `VmOptions::Perm*`) with `extract_permissions()` and `permissions_of()` converting between the first two.

**Cause**
`VmMapOption` grew as "options for creating a mapping" and `VmaFlags` as "the state of an area or mapping". Some options are not one-off actions but become state (`Pinned`, `Sealed`), so each is declared twice and copied across at creation. Only `Commit` is truly a request: it means "populate now" and leaves nothing behind.

**Change**
Let creation take the state-carrying properties as `VmaFlags` and keep only what is a one-off action (`Commit`) as a separate request. Then `Pinned` and `Sealed` are declared once and `create_mapping()` has no option-to-flag translation for them. Keep `VmOptions` (the ABI, with Zircon's values, which must not leak mm types) and `MmuFlags` (hardware bits, which mean something else) separate: they are different layers, as Zircon's `ZX_VM_*` and `ARCH_MMU_FLAG_*` are. Ruled out: one enum for everything, which would put mm and hardware details into the ABI. Decide the shape with the user before building; the change touches every caller of `create_mapping()` and `map_with_vmo()`.

**Reference**
Zircon keeps one internal flags word that both creation and the stored object use (`VMAR_FLAG_*`), converted from `ZX_VM_*` in `VmAddressRegionDispatcher::Map()`.

**Acceptance**
No property is declared in both `VmMapOption` and `VmaFlags`; `VmArea::create_mapping()` does no translation for `Pinned` or `Sealed`; both images boot with `ours-verify` and `usercopy.test`, `pmnode.test` and `vmaseal.test` pass.
