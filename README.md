# First Person Controls

Use the right stick to look around and the left stick to move Link while in first-person mode.

Built for [Dusklight](https://github.com/TwilitRealm/dusklight) using its
[modding SDK](https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md).

## Build

Use the Dusklight template's standard CMake build:

```sh
cmake -B build
cmake --build build
```

The resulting `build/mods/first_person_controls.dusk` is for the local platform. The included GitHub Actions workflow
builds the supported platform bundles and combines them into a distributable `.dusk`.

## Install

Copy the bundle into the game's mods folder:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

During development, click **Reload** in the in-game mod manager after replacing the bundle.
