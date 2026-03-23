# Vendored Third-Party Libraries

This project now keeps each vendored library in its own root directory under `third_party/`:

```text
third_party/
  nlohmann_json/
    include/
    share/
  fmt/
    include/
    share/
    lib/
    bin/
    debug/
  spdlog/
    include/
    share/
    lib/
    bin/
    debug/
```

`CMakeLists.txt` loads each package from its own root, so do not flatten all libraries into a
single shared `include/` or `share/` directory.

Example sync commands from a local `vcpkg` installation:

```powershell
Move-Item .\third_party\include\nlohmann .\third_party\nlohmann_json\include\
Move-Item .\third_party\share\nlohmann_json .\third_party\nlohmann_json\share\

Copy-Item D:\vcpkg\installed\x64-windows\include\fmt .\third_party\fmt\include\ -Recurse -Force
Copy-Item D:\vcpkg\installed\x64-windows\share\fmt .\third_party\fmt\share\ -Recurse -Force
Copy-Item D:\vcpkg\installed\x64-windows\lib\fmt.lib .\third_party\fmt\lib\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\bin\fmt.dll .\third_party\fmt\bin\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\debug\lib\fmtd.lib .\third_party\fmt\debug\lib\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\debug\bin\fmtd.dll .\third_party\fmt\debug\bin\ -Force

Copy-Item D:\vcpkg\installed\x64-windows\include\spdlog .\third_party\spdlog\include\ -Recurse -Force
Copy-Item D:\vcpkg\installed\x64-windows\share\spdlog .\third_party\spdlog\share\ -Recurse -Force
Copy-Item D:\vcpkg\installed\x64-windows\lib\spdlog.lib .\third_party\spdlog\lib\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\bin\spdlog.dll .\third_party\spdlog\bin\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\debug\lib\spdlogd.lib .\third_party\spdlog\debug\lib\ -Force
Copy-Item D:\vcpkg\installed\x64-windows\debug\bin\spdlogd.dll .\third_party\spdlog\debug\bin\ -Force
```

`nlohmann_json` is still header-only. `fmt` and `spdlog` are vendored with both import libraries
and runtime DLLs so the executable can run directly from the build output directory.
