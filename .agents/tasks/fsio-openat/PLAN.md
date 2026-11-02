# fsio-openat: opens relative to a directory handle, not only the namespace

**Date**: 2026-11-01
**Status**: Proposed

## Request
Programs sometimes open or create by a bare name relative to a directory they
hold, not by a namespace-absolute path. Today `fsio::open` requires a leading
'/' (`Namespace::resolve` refuses anything else), while every server already
accepts relative opens on a directory connection. Expose the client half.

## Decisions
| Decision | Options | Recommendation | User's prediction | Decided |
|---|---|---|---|---|
| Name | `open_at` vs overloading `open` | overload | -- | overload on the first parameter, 2026-11-01 |
| ".." in the path | refuse / pass to the server | refuse (BadPath) | -- | refuse, 2026-11-01 |
| Process cwd | now / later | later, after this lands | -- | later, 2026-11-01 |

## Steps
1. `fsio::open(Uandle dir, std::string_view path, Flags)` -- the Open message
   fsio::open sends after the namespace hop, sent to |dir| instead, the path
   never namespace-resolved. Leading '/' or a ".." past cleaning: BadPath.
   The send half is shared with the existing open (one helper, two fronts).
2. routing_checks: hold the /disk root's directory handle, open_at a check
   file relative to it, write, read back, unlink.

## Files
- `system/lib/fsio/` -- the overload; the send half factored out of open.
- `system/service/init/test/routing_checks/routing_checks.cpp` -- the
  open_at check.

## Test plan
Host: path cleaning's refusals via the existing path tests' shape. Boot:
`boot_check.py` both images diskless green; a disked boot whose check_disk
exercises the open_at path end to end.
