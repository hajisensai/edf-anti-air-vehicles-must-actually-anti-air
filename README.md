# EDF6: Anti-Air Vehicles Must Actually Anti-Air

[简体中文](README.zh-CN.md)

The stock KG6 Kepler is EDF6's anti-air vehicle, yet it fires slow, non-exploding rounds that
mostly miss anything flying, with half the durability of a same-level tank. This mod makes it do
its job, and turns the DLC KG7 Bohr into a self-aiming ground-attack launcher.

It has two parts:

- **EDF6AutoTurret.dll**, an [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) plugin. The
  turret aims itself: it picks targets from the game's own enemy list, leads them, solves the
  round's ballistic arc and drives the turret with feed-forward so rounds stop trailing crossing
  targets. Flak rounds get a time fuse at the target's range, a proximity fuse and a contact fuse.
  Hold the aim stick to aim by hand; let go and the turret takes over again.
- **Weapon data** overriding the stock vehicles' own files. No weapon rows are added. The
  vehicles' descriptions in `WEAPONTEXT` are rewritten with the new numbers; only their own rows
  change, on top of whatever tables are already in `Mods`, so mods that edit the tables keep theirs.

## What changes

| Vehicle | Change |
|---|---|
| KG6 Kepler, E, F, YE, YF | Flak: exploding rounds (8 m blast), proximity / time / contact fuses, 480 m range. Half the fire rate at twice the damage per round (same damage per second on paper, half the bursts on screen). Durability x2. DLC YF-HV turret speed. Air targets first. |
| KG6 Kepler YF-HV (DLC) | Auto-aim only; keeps its high-velocity solid shot, durability and turret. |
| KG7 Bohr, Bohr B (DLC) | Auto-aim in ground mode: ground targets first, lobbed rounds aimed on their arc, stock impact fuse. Durability x2, blast 4 m -> 6 m. |

Why: the stock Keplers deal a third to a half of the damage per second of same-level tanks and
helicopters, with under half their durability, and the shortest range of any of them. The Bohr
already out-damages the same-level Barrias TZ4 but has well under half its durability.

## Install

Requires EDF6 (Steam) with [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) installed.

1. Copy `EDF6AutoTurret.dll` and `EDF6AutoTurret.ini` from a release (or a CI build artifact) into
   `<EDF6>\Mods\Plugins\`.
2. Build the weapon files from your own game data (they are derived from it, so they are not
   distributed) with Python 3.10+, straight into the game's `Mods` folder:

   ```
   set EDF6_DIR=C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6
   python tools\build.py --out "%EDF6_DIR%\Mods"
   ```

   It writes the vehicles' own call and gun files under `Mods\WEAPON\` and their eight rows of
   the `WEAPONTEXT.*.SGO` tables there; it reads the game's `Root.cpk` and never modifies it. Run it
   while the game is closed, and again after installing another mod that replaces `WEAPONTEXT`.
   `--no-text` leaves the text tables alone.

To uninstall, delete those files and the plugin (delete the `WEAPONTEXT` files only if no other
mod installed them; otherwise reinstall that mod's). Settings are in `EDF6AutoTurret.ini` and apply
while the game runs; `Debug=1` writes what the turret is doing to `EDF6AutoTurret.log`.

## Build the plugin

Visual Studio 2022 with the C++ x64 tools (CMake and Ninja come with it):

```
build.cmd
```

The DLL lands in `dist\Mods\Plugins\`. CI builds it on every push.

## Compatibility

Built against EDF.dll with TimeDateStamp `0x678CCB46`. The plugin checks the code it patches and
turns itself off if the game has changed.

Online play is untested. The mod adds no weapon rows, so players without it never meet a row they
do not have; but each machine simulates the vehicles from its own files, so in a mixed lobby the
Keplers will not behave the same for everyone. Have every player install it. Reverse-engineering notes: [docs/re-notes.md](docs/re-notes.md).

## License

MIT, see [LICENSE](LICENSE). Bundled: `third_party/EDFModLoader/PluginAPI.h` (MIT) and
`third_party/edf6-cpk` (CPK / CRILAYLA readers from EDF6MultiSlot by momotori01, public domain).
