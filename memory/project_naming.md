---
name: project-naming
description: Project name is nyxdb (all lowercase), not NyxDB or nyxDB
metadata:
  type: feedback
---

The project name is **nyxdb** — all lowercase. Use it consistently in:
- CMake project name
- Binary names (`nyxdb`)
- C++ namespace (`nyx`)
- Docs, comments, file headers

**Why:** User explicitly corrected "nyxDB" → "nyxdb".
**How to apply:** Never capitalise the 'db' part. Namespace stays `nyx` (short form is fine).
