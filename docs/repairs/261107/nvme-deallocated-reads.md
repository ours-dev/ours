# nvme-deallocated-reads outstanding repairs

## 1. The nvme driver reads a fresh image's deallocated blocks as BadState

**Severity**
High.

**Symptom**

Booting the dev image with a fresh `make_gpt_disk` NVMe disk and the
volume-manager resident (the LVM round's scan reads LBA 0-3 of every
routed device), the nvme driver answers commands with BadState and says
on the console: `nvme: the device failed a command, DW3 0x80170000` /
`0x80160000`. routing_checks' Nvme read/write checks fail
deterministically. The same disk and the same boot on baseline passes:
the delta is the concurrent reader.

**Cause**

DW3's status field decodes to SCT 4 (media and data integrity), SC 0x0B
(de-allocated or unwritten logical block): the device reports the LBAs
the scan reads as deallocated. Two mistakes meet. The driver treats any
nonzero completion status as `BadState` (`is_command_ok`), where a
deallocated read is the device's answer to the question, not a failure
-- the NVMe default for a deallocated read is zeros, and the driver
should hand the zeros back (or issue the DSM deallocate on format).
And whatever makes QEMU's controller report these LBAs deallocated under
a snapshot overlay after the maker wrote them is unverified: the driver
never read an image it had not written through first, so the path had
never run.

**Change**

None yet. The shape it should take: `Queues::wait_for` returns the
completion's status field, and `Device::read_blocks` treats SCT 4 /
SC 0x0B as a zero fill, the way Linux's `nvme_rw_checks()` treats
`NVME_SC_DNR`-less media statuses. Whether the scan's reads trigger the
device's deallocation report at all (a stale overlay page?) is part of
the same look.

**Reference**

Linux `nvme_error_status()` and `nvme_should_fail()`; the block layer's
treatment of a deallocated read as zeros (`blkdev_issue_flush`-adjacent,
`NVME_SC_DEALLOC`).

**Acceptance**

The dev image boots with a fresh `make_gpt_disk` NVMe disk and the
volume-manager resident; routing_checks' Nvme read and write checks
pass; a read of a deallocated LBA answers zeros through
ours.block.Block.

**Found how**

The volume round's NVME_DISK regression boot: baseline control passed
with the same image, the branch failed deterministically (twice), the
DW3 words decoded from the console's hex.
