# CWR local development notes

## Windows build

- Use the `win-x64-clang-rwdi` preset for normal local development. It produces
  `dist/x64-win-rwdi/PoseidonGame.exe` (plus required DLLs).
- Build from a Visual Studio x64 developer environment. The checked-in preset
  uses `clang-cl`, the MSVC/Windows SDK headers and vcpkg dependencies.
- If `cmake`, `ninja`, or the compiler are not on `PATH`, this verified command
  builds the game without relying on shell setup:

  ```powershell
  cmd.exe /d /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "E:\projects\CWR\build\win-x64-clang-rwdi" --target PoseidonGame --parallel 8'
  ```

- First configure, or a deleted build directory, needs `VCPKG_ROOT` and may
  download manifest dependencies. When ccache is unavailable, disable the
  launchers during configure:

  ```powershell
  $env:VCPKG_ROOT = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg'
  cmake --preset win-x64-clang-rwdi -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
  ```

  The configured local toolchain also recognizes the portable LLVM install at
  `build/tools/LLVM`; an explicit `POSEIDON_LLVM_ROOT` environment variable is
  still supported when using another install.

## Game data and local runtime

- Game assets are intentionally outside source control. The local copied data
  root is `E:\projects\CWR\games-res` and is ignored by Git. Preserve its
  `BIN`, `DTA`, `AddOns`, `Worlds`, campaigns and mission directories.
- The matching Steam source data is the `Remastered` subdirectory, not the
  legacy parent directory:
  `C:\Program Files (x86)\Steam\steamapps\common\ARMA Cold War Assault\Remastered`.
- Do not overwrite the Steam executable. Run the build output against the
  local data root instead:

  ```powershell
  & 'E:\projects\CWR\dist\x64-win-rwdi\PoseidonGame.exe' --work-dir 'E:\projects\CWR\games-res' --window --no-splash
  ```

- The custom Radar HMMWV development mod is under
  `E:\projects\CWR\mods\@cwr_radar_hmmwv`. For a mod run, add:

  ```text
  --mods-dir "E:\projects\CWR\mods" --mod "@cwr_radar_hmmwv"
  ```

  Keep `OpenAL32.dll` beside `PoseidonGame.exe`; the `PoseidonGame` build
  target copies it to `dist` automatically.

## Fast checks

- Use `--check --strict` for initialization/configuration validation; it exits
  after startup and returns a non-zero status when initialization logs errors.
- The Radar HMMWV demo mission can be used for a smoke test:

  ```powershell
  & 'E:\projects\CWR\dist\x64-win-rwdi\PoseidonGame.exe' --work-dir 'E:\projects\CWR\games-res' --mods-dir 'E:\projects\CWR\mods' --mod '@cwr_radar_hmmwv' --test-mission 'E:\projects\CWR\mods\@cwr_radar_hmmwv\missions\CWRRadarHMMWVDemo.Eden' --window --no-splash --nosound
  ```

- `git -c safe.directory=E:/projects/CWR diff --check` is the final whitespace
  check. Do not reset or discard unrelated local edits: the worktree commonly
  contains active engine, mod and dashboard changes.

## LAN dashboard

- Development builds expose the local dashboard at
  `http://127.0.0.1:10001/` (the server binds `0.0.0.0:10001` for LAN access).
- Dashboard browser assets are maintained in `apps/cwr/Game/web/`. A
  `PoseidonGame` build copies them to `dist/x64-win-rwdi/web/`, and the server
  serves those external files with no-cache headers. For frontend-only edits,
  rebuild the target or copy the changed asset to that `dist/web` directory,
  then refresh the browser; a C++ recompile is not required after the asset is
  copied.

## Editing rules

- Use `apply_patch` for source edits. Prefer `rg` for repository searches.
- Never commit or modify copied game assets unless the task explicitly calls
  for a local runtime-data change. `games-res`, build outputs and crash dumps
  are ignored on purpose.
