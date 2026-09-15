# Equipment Color Changer source

This archive contains the C++ source corresponding to Equipment Color Runtime
0.2.2 for Skyrim SE/AE 1.6.1170.

## Build

Use Windows 10/11, Visual Studio 2022 Build Tools, xmake 3.1 or newer, and the
matching Skyrim Address Library development files. Open a Visual Studio Developer
PowerShell, change to this directory, and run:

    powershell -ExecutionPolicy Bypass -File .\build.ps1 -Test

The release DLL and PDB are written to
`build/windows/x64/releasedbg/`. The test targets exercise shader readback and
the D3D11 draw interception code.

The `lib/commonlibsse-ng` directory is the alandtse CommonLibSSE-NG `ng` source
used for this build. The `vendor` directories contain the MinHook source, the
SKSE Menu Framework API header, and the DXBC checksum source used by the plugin.
Their license texts are included beside the source.

The plugin targets Skyrim runtime 1.6.1170. It has no ESP or Papyrus scripts and
does not generate DDS textures.
