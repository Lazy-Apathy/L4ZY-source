# Build fix for current MinGW-w64

Patch `00-build-fix.patch`, from the L4ZY 2026.10.16.1 source.

The 2020 source does not build with a current MinGW-w64 GCC. This patch makes it build, and changes nothing in the game:

- C++14 mode (`-std=gnu++14 -fpermissive`);
- the placement `new`/`delete` of `shared/tools.h` are only defined when the C++ library has not defined them;
- Windows 7 API level (`_WIN32_WINNT 0x0601`);
- the system libraries the other patches link with (Media Foundation, Direct3D 11, DXGI, shell, OLE).

Every other patch is made on top of this one.

## Applying

Apply it first, on the untouched Sauerbraten 2020 source tree (the `src/` and `data/` folders of the 2020 Edition):

```
git apply patches/00-build-fix.patch
```

- Checked: applies on the Sauerbraten 2020 source and compiles (MinGW-w64 GCC 16.2).


## Files

- `src/Makefile`
- `src/build-client.bat`
- `src/shared/cube.h`
- `src/shared/tools.h`
- `src/vcpp/sauerbraten.vcxproj`
