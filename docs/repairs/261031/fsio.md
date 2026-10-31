# fsio outstanding repairs

## 1. A connection opened through an entry whose provider then refuses the root's Open reads PeerClosed with no status

**Severity**
Low: a missing diagnostic. Nothing reads wrong data, and the status is recoverable by asking again, but a consumer that does not know to ask reports the wrong error.

**Symptom**
A consumer opens `/ghost`, an entry whose route ends at a provider that declares the directory and does not serve it. If the open is written before the provider has handled the root's forwarded Open, `fsio::open()` returns Ok, and the first call on the new connection answers `PeerClosed`, not `NotFound`. The second `fsio::open()` under the entry answers `NotFound`. On the target, with the `/ghost` case run first in `routing_checks`, the first answer was `PeerClosed` in 24 of 24 boots; run after the cases that read `files`, `NotFound` in 20 of 20. The same holds for any refusal the provider's server makes on the entry: `NotDir` for a joined path that ends at a file (`/files-file`).

**Cause**
The root forwards a directory use as one Open and does not wait (design invariant I1), so the provider's refusal is an epitaph written on the entry's channel after the consumer has the entry. The consumer's own Open is queued on the same channel, and it is destroyed with the channel when the provider closes it, so its new connection closes with nothing on it. `fsio` reads an entry's epitaph only when its own write on the entry answers `PeerClosed` (`fsio::open()`, `closed_status()`), which is the next open's, and the connection it already handed out has no way back to the entry.

**Change**
Not decided. The options: (a) leave it, as the model of a fire-and-forget Open, and have a consumer that must tell the cases apart open again, as `routing_checks` does (`refused_by_provider()`); the guide says so; (b) have `fsio` look at the entry when a call on a connection it opened answers `PeerClosed` with no epitaph, and answer the entry's status instead; that needs a way from a connection to its entry, and `fsio::open()` hands out a bare handle the caller owns, so it is a change to the library's interface; (c) a wrapper type for a connection that carries the entry. Recommended: (a) until a program wants the status on its first call.

**Reference**
None, this project's own design: D8 (a) and the directory-capability design's fifth row of "What the consumer sees".

**Acceptance**
Either `fsio::read()` on a connection opened through `/ghost` answers `NotFound` on the first call without a retry, or the guide's chapters on the namespace and on `routing_checks` say that a refusal by the provider can arrive on the second open, and `refused_by_provider()` cites that.
