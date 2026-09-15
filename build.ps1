param([switch]$Test)
$ErrorActionPreference = 'Stop'
$env:XMAKE_GLOBALDIR = 'E:/MO2/dev/toolchain/xmake-global'
$erXmake = 'E:/MO2/dev/toolchain/xmake-3.1.0/xmake/xmake.exe'
$erDevCmd = 'E:/MO2/dev/toolchain/VSBuildTools2022/Common7/Tools/VsDevCmd.bat'
Push-Location $PSScriptRoot
try {
    & $env:ComSpec /d /s /c "`"`"$erDevCmd`" -no_logo -arch=x64 -host_arch=x64 && `"$erXmake`" build EquipmentColorRuntime`""
    if ($LASTEXITCODE -ne 0) { throw 'Plugin build failed' }
    if ($Test) {
        & $env:ComSpec /d /s /c "`"`"$erDevCmd`" -no_logo -arch=x64 -host_arch=x64 && `"$erXmake`" build RuntimeShaderTests`""
        if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }
        & './build/windows/x64/releasedbg/RuntimeShaderTests.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
        & $env:ComSpec /d /s /c "`"`"$erDevCmd`" -no_logo -arch=x64 -host_arch=x64 && `"$erXmake`" build RuntimeHookTests`""
        if ($LASTEXITCODE -ne 0) { throw 'Hook test build failed' }
        & './build/windows/x64/releasedbg/RuntimeHookTests.exe'
        if ($LASTEXITCODE -ne 0) { throw 'Hook tests failed' }
    }
} finally { Pop-Location }
