# Reverse-engineering notes

All addresses are RVAs into `EDF.dll` with TimeDateStamp `0x678CCB46`, SizeOfImage `0x22CE000`.
`src/plugin.cpp` checks the bytes it relies on (`CheckProfile`) and refuses to load otherwise.

## Vehicle603_Flak (Kepler / Bohr chassis)

| What | Where |
|---|---|
| vtable | `0x17DC620`; slot 55 = per-frame input `0x621460` (hooked) |
| vehicle matrix / position | `+0x60` (4x4, rows = right, up, forward, position) / `+0x90` |
| dead flag | `+0x2E8` |
| team | `+0x314` |
| seats | `+0x608` array, `+0x618` count, stride `0x340` |
| turret turn input written by the input slot | `+0x2AA0` (yaw, pitch) |
| seat: weapon holders / count | `+0xC8` / `+0xD8`; holder `+0x10` = weapon |
| seat: aim controller | `+0xE0`; axes at `+0x10`, stride `0x40`, `{min, max, angle, ...}` |
| seat: rider aim stick | `+0x2D0` (x, y) |

The input slot copies the stick into `+0x2AA0`; the plugin overwrites it after the stock code
runs. Turret rate is about 1.1–1.3 rad/s per unit input with the DLC turret parameters
`[65, 0.3, 0.3]`; the plugin learns the real rate per axis online. Pitch is negative-up
(-1.047 .. +0.087).

## Weapon (fields filled from the SGO at `0x68D4A0`)

| Field | Offset |
|---|---|
| LockonType / LockonTargetType | `0x6B0` / `0x6B4` |
| LockonRange | `0x6D0` |
| AmmoClass factory (looked up by FNV-1a name hash, `0x1195C50`) | `0x7F8` |
| AmmoSpeed (m/frame) / AmmoAlive (frames) / AmmoDamage | `0x894` / `0x898` / `0x89C` |
| AmmoExplosion | `0x8B0` |
| AmmoGravityFactor | `0x8E0` |

- Fire-start `0x690BB0` refuses a lock-on weapon with an empty lock list unless LockonType is 0 or
  5. No stock weapon uses 4, so the gate at `0x690C2E` (`cmp eax,5 / je`) is patched to
  `cmp eax,4 / jae`: type 4 fires with or without a lock. The mod's guns use type 4 with
  LockonRange 0, so they never lock and never show lock markers.
- The only stock reader of LockonTargetType (`0x696792`) maps it to a lock class (0 -> 3, 1 -> 2);
  with LockonRange 0 it has no effect, so the mod uses 1 to mark ground-attack guns.
- Weapon vtable slot 17 = "round spawned" `(weapon, bullet)`, called once per round by fire
  `0x696FD0`, which spawns rounds through the factory at `0x7F8` (`0x6970A5`).

## Lock-target registry (the enemy list)

`*(0x20B2AB0)` holds an MSVC `std::list` at `+8`; each node's `+0x10` is a lock point `T*`:
`+0` kind (0 = enemy kind), `+8` owning object, `+0x10` aim point (refreshed every frame from the
bone matrix by `0x6C7700`), `+0x29` valid, `+0x2A` lockable. The lock query `0x696710` walks the
same list on the game thread.

Team relations: `rows = *(*(0x20B2978) + 0x38)`, `relation = *(rows + team*0x38 + 0x18)`;
`relation[otherTeam] == 2` means enemy.

## Gravity and the ballistic solve

The game's vehicle aim (`0x622640`) takes the world gravity vector from
`*(*(0x20B2958) + 0x68) + 0x20` (virtual slot 0 returns a pointer to it, m/s^2), rotates it into
the vehicle frame and drops a round by `AmmoGravityFactor * gravity / 3600` metres per frame^2
(`0x622B65`), then solves the two launch angles at `0x50350`. Measured gravity: about 14.7 m/s^2.
The plugin does the same and takes the lower arc.

## GrenadeBullet01 (the flak round)

| What | Where |
|---|---|
| vtable | `0x17A17E0`; slot 1 deleting dtor `0x265B10`, slot 5 update `0x264AB0` (both hooked) |
| factory vtable | `0x17A1688` (`GrenadeBullet01_MapNoDamage`, the stock Bohr's round that spares buildings, is a separate class: `0x17A16E8`, factory `0x17A16C8`; the mod switches the Bohr to `GrenadeBullet01` and tells it apart from flak by LockonTargetType 1) |
| weak-this control block | `+0x30` |
| flight control block C | `+0x140` |
| C: flags / age / lifetime | `+0xAF4` / `+0xAF8` / `+0xA08`; expires when age >= lifetime (`0x236899`) |
| C: position / velocity (m/s) / stuck | `+0xB80` / `+0xB90` / `+0xC00` |
| C: blast radius / burst effect size | `+0x788` (damage sphere) / `+0xA20` (drawn at `/5`, `0x264B92`) |

`Ammo_CustomParameter[0] = 1` bursts on expiry (flag `0x20`); bounce 0 sticks the round to what it
hits. The fuses set age = lifetime before the stock update runs, so the round bursts that frame at
its (possibly moved) position.
