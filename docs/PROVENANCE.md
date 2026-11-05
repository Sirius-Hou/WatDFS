# Source provenance

This repository preserves the author's completed CS 454/654 distributed filesystem project in a standalone presentation layout.

| Files | Source |
| --- | --- |
| Root client/server, headers, and reader/writer lock | Author-confirmed final `p2_submission.zip` |
| `baseline/` client/server and headers | Author-confirmed `p1_final_submission` directory |
| `lib/librpc.a`, `lib/libwatdfsmain.a` | Course-provided archives from `provided-code`; identical archives were also present in the working P2 directory |
| Makefiles | Original versions, adjusted for shared library paths and explicit thread linkage |
| README, documentation, and Python validation tools | Added for repository presentation and reproducible testing |

The C++ source and headers are preserved byte-for-byte from the selected versions. In particular, the submitted `rw_lock.cpp` is retained together with its matching server; it is not replaced by the different starter lock or the working-directory variant.

The course supplied the RPC transport, client FUSE entry point, API headers, logging support, and starter code. The client/server implementations extend that framework. The submitted reader/writer implementation differs from the provided starter version, while retaining its interface.

Original attribution comments are retained. No MIT or other blanket license is added, and inclusion here does not grant additional rights to course-provided material.

Assignment PDFs, submission ZIPs, temporary review files, alternate P2 copies, build products, and unrelated work are not included. The assignment's submission rules are historical context, not instructions for this repository's users.
