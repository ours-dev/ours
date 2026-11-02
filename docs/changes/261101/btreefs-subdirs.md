# btreefs-subdirs changes

## Title: directories under the root, paths that walk them (2026-11-01)

**Files**
- `system/service/btreefs/btreefs.cpp` — the path walk (`resolve`), the
  directory-relative Open (`open_below_dir`), the generalized child lookup
  and object creation (`find_child`, `create_object`), Unlink walking to the
  entry's parent (`unlink_at`) with the empty-directory rule, ReadDirents on
  any directory connection with the entries typed by their kind, and a
  connection target of Directory-or-File with the root as one directory
  among them.
- `system/service/init/test/routing_checks/` — check_disk makes a directory,
  makes a file in it, writes and reads it back, lists the directory (the
  file typed as a file), unlinks the file, then the directory, and checks
  the root's listing names neither.

**Symptom**
None — a planned step. The tree's key model (DIRENT items under an object,
the kind in the value) always allowed nesting; the component resolved only
one name under the root and refused every path with a '/' in it.

**Cause**
A path is the walk the VFS concept names: from a directory, each component
must name a directory until the last, which names whatever it is. The
component-side work is entirely in that walk -- the library needed nothing,
because a subdirectory is one more object with DIRENT items of its own, and
a DIRENT's value has carried the child's kind since the format was written.
Creation goes through Open, `ProtocolDirectory` saying which kind the name
should be; removal keeps the crash-safety order and adds the filesystem's
oldest rule, a directory leaves only when it is empty (NotEmpty).

**Change**
`resolve(directory, path)` splits the path, looks each component up by its
name hash, and answers the last one's object and kind -- NotDir for a
non-directory in the middle. Open resolves the parent components and treats
the last name as the entry: found (MustCreate refusing with AlreadyExists)
or created per the flags. Connections now hold Directory-or-File with an
object id, the volume's root being a directory whose object is 1, so Open,
ReadDirents, Clone and Unlink all work relative to whichever directory the
client holds. The root's listing types each entry by its DIRENT value's
kind.

**Reference**
The walk is the VFS lookup -- Linux's `link_path_walk()`, each component a
directory lookup that must name a directory until the last. The
`ProtocolDirectory` creation bit and the empty-directory refusal are
Fuchsia's `fuchsia.io` conventions pared to this protocol's flags and
statuses.

**Verification**
`boot_check.py`: dbg PASS (44), rel PASS (42, 6/6 required), host-tests PASS
157 -- both diskless, where /disk skips. With the GPT disk: `routing_checks:
passed` -- the directory made, the file made inside it, the write and the
read-back through the two-component path, the directory's listing naming the
file, the file unlinked, the directory unlinked, and the root's listing
naming neither; the host-side disk file shows the superblock's root and
height back to a single leaf and the freed block's bit clear. clang-tidy
quiet.

**Deliberately not done**
- No ".." beyond what the client's own path carries: the kernel-side walk
  keeps to the components it is given, the namespace's clean_path having
  resolved dot forms before they arrive.
- No rename or move between directories: a rename is a dirent pair and a
  walk, and nothing calls for it yet.
- memfs stays flat: it is the teaching server, and directories there would
  duplicate this walk with none of the disk.

**How it was found**
None.

**The user's prediction**
None. The user picked subdirectories from the three candidates.

**Three questions you should be able to answer**
- Why did the library need no change at all for subdirectories, and what in
  the on-disk format had carried the possibility since the first commit?
- Unlink of a directory checks emptiness by scanning the child's DIRENT
  range -- what stops that scan, and which item sorts before the DIRENTs?
- The Open path resolves the parent components and treats only the last
  name as creatable -- what would a mkdir -p want instead, and where would
  the loop go?
