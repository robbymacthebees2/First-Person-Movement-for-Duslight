# First Person Controls

The right stick controls first-person look to just under 90 degrees vertically, avoiding the camera's straight-up/down
tracking singularity, and the left stick moves Link. Left-stick
lateral input makes Link sidestep while he continues facing the camera direction. Link uses his walk or run animation
according to the movement speed, and can attack with his sword without leaving first-person mode. First-person view is
retained during temporary action and movement processes, including jumping, and other mods can adjust movement speed
without this mod overwriting their changes. Movement remains functional at the pitch limits, and skid turns keep Link
facing the camera direction. During ladder and climb transitions, camera yaw follows Link's facing angle so first-person
movement remains aligned after dismounting. The HUD, including the minimap, remains visible in first-person mode, and
the right D-pad opens the map screen. First-person mode reduces Link's background wall-collision radius to help prevent
getting stuck in nearby walls or objects, including during ladder and climb transitions, with additional clearance
during ladder and roof top-outs. The normal collision size is used during jumps and falls so ground contact and landing
remain reliable, and outside first-person. Repeated blocked movement into a wall triggers a small outward recovery
nudge; repeated blocked actor contact also nudges Link away from that object's collision direction. First-person actor
collision displacement is strengthened, and Link's sword attack capsules are slightly wider to preserve close-range
enemy hits. The mod-menu panel also includes an optional toggle to restore first-person view after area transitions; it
is off by default.

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
