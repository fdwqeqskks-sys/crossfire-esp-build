name: Build Windows x64 Placeholder Offset Mode

on:
  push:
    branches:
      - main
    paths:
      - crossfire_esp.cpp
      - .github/workflows/build.yml
  workflow_dispatch:

permissions:
  contents: read

jobs:
  build:
    name: Compile with MSVC
    runs-on: windows-latest

    steps:
      - name: Check out repository
        uses: actions/checkout@v4

      - name: Configure MSVC x64 environment
        uses: ilammy/msvc-dev-cmd@v1
        with:
          arch: x64

      - name: Compile hardcoded placeholder-offset build
        shell: cmd
        run: >-
          cl.exe /nologo /std:c++20 /EHsc /W4
          /DUNICODE /D_UNICODE
          /Fe:crossfire_esp.exe crossfire_esp.cpp
          /link User32.lib Gdi32.lib Advapi32.lib

      - name: Verify executable
        shell: powershell
        run: |
          if (-not (Test-Path -LiteralPath '.\crossfire_esp.exe' -PathType Leaf)) {
            throw 'crossfire_esp.exe was not generated.'
          }
          Get-Item -LiteralPath '.\crossfire_esp.exe' |
            Select-Object FullName, Length, LastWriteTime

      - name: Upload executable artifact
        uses: actions/upload-artifact@v4
        with:
          name: crossfire-esp-windows-x64
          path: crossfire_esp.exe
          if-no-files-found: error
          retention-days: 30
