---
description: Build WinUAE using MSBuild
---

To build the WinUAE project:

1. Locate MSBuild.exe (usually in `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`)
// turbo
2. Run the build command:
```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "od-win32\winuae_msvc15\winuae_msvc.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal
```
