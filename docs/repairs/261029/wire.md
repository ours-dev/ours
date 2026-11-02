# wire outstanding repairs

## 1. The first design of the framing says an unknown or malformed message gets an epitaph, and the code and the later design say it gets none

**Severity**
Low: the code is consistent with itself and with `docs/design/261004/component-framework.md`, and nothing misbehaves. A reader who follows the comment in `wire.hpp` to its source is told the opposite of what every server does.

**Symptom**
`system/lib/wire/include/wire/wire.hpp:19-20` names `docs/design/260924/userspace-filesystem.md` §2.1 as the source of the framing. Section 2.5 of that document (lines 163-164) says a malformed message is answered with an epitaph `InvArgs` and an unknown ordinal with an epitaph `NotSupported`, and line 858 repeats it ("先发 epitaph（InvArgs 或 NotSupported）再关"). Every server in the tree does the opposite: it closes the connection and writes nothing (`examples/echo/echo_server.cpp:172-191`, `system/lib/ioservice/include/ioservice/server.hpp:83-90`, `system/lib/ioservice/src/server.cpp:562-565`), and `docs/design/261004/component-framework.md:323` records the rule ("close the connection without an epitaph"). The 260924 document carries "Superseded" banners on other sections (lines 336 and 374) but none on 2.5 or on the table at line 858.

**Cause**
The rule changed on 2026-10-04 with the component-framework design, which edited the sections it replaced and left section 2.5 as written. The header comment still points at the document as a whole.

**Change**
Add a one-line banner under the 2.5 heading and beside the table at line 858, in English, saying that the rule is superseded by `docs/design/261004/component-framework.md` ("The protocol subset": a malformed message or an unknown ordinal closes the connection without an epitaph), as the document already does for its other superseded sections. Ruled out: rewriting the Chinese text, which the repository's rule leaves as it is.

**Reference**
None, this project's own design.

**Acceptance**
`grep -n "epitaph" docs/design/260924/userspace-filesystem.md` shows a banner at section 2.5 and at the table near line 858 pointing at the 261004 design.
