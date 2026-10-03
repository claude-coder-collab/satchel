# satchel (Python)

cffi binding for the Satchel C API (`core/include/zp/zp.h`).

```python
import satchel

ctx = satchel.Context()
plan = ctx.plan(["/path/to/session"])
for c in plan.conflicts:
    print(c.kind, c.collision_key, c.entries)
plan.resolve([satchel.Resolution.rename(3, "take1 (2).flac")])
plan.build("session.zip", progress=lambda done, total: print(done, total))

archive = ctx.open("session.zip")
archive.extract("restored")          # PCM audio is restored from FLAC and verified
```

The shared library (`libsatchel.so` / `.dylib` / `satchel.dll`) is looked up in `SATCHEL_LIBRARY`,
next to the package, then in the repository's `build/*/core/{Release,Debug}` directories.

Tests: `pytest` (from this directory, after building the `zp_shared` target).
