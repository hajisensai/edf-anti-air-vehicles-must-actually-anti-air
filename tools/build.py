"""Turn the stock KG6 Kepler anti-air vehicles into flak vehicles and the DLC KG7 Bohr into a
self-aiming ground-attack launcher, into dist/Mods.

  python tools/build.py [--out dist/Mods]

Overrides only the Kepler / Bohr call SGOs and their gun pairs; no WEAPONTABLE /
WEAPONTEXT rows are added, so it coexists with mods that edit the tables. The guns get flak rounds
and the LockonType 4 marker the EDF6AutoTurret plugin aims; the calls get more durability and a
faster turret.
"""
from __future__ import annotations

import argparse
import copy
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dsgo  # noqa: E402
import gamefs  # noqa: E402
from dsgo import Node  # noqa: E402

# The five stock KG6 Kepler calls and the guns each one mounts (GUN02 / GUN03 are shared by two tiers).
CALLS = {
    'AWEAPON346.SGO': '01',   # KG6 Kepler
    'AWEAPON349.SGO': '02',   # KG6 Kepler E
    'AWEAPON352.SGO': '02',   # KG6 Kepler F
    'AWEAPON359.SGO': '03',   # KG6 Kepler YE
    'AWEAPON361.SGO': '03',   # KG6 Kepler YF
}
SIDES = ('L', 'R')
SOURCE_GUN = 'V_409HELI_GATLING01.SGO'   # EF31 Nereid auto-capture cannon
# Stock Kepler durability is under half a same-level tank's (YF 8750 vs Barrias TZ3 24000); the DLC
# Kepler YF-HV already runs 2.4x the YF's. Doubling keeps it the fragile one.
DURABILITY_SCALE = 2.0
TURRET = [65.0, 0.3, 0.3]  # gun-L turret params, the DLC Kepler YF-HV's: the stock 45-65/0.1 can't keep up
# Each gun fires every 6 frames instead of 3 at twice the damage per round: the same damage per
# second on paper, half the bursts on screen. The real gain is the blast and the proximity fuse.
FIRE_SLOWDOWN = 2.0

# The DLC Kepler YF-HV keeps its high-velocity solid shot, durability and fast turret (already the
# buffed Kepler); its guns only get the auto-aim marker.
HV_GUN = 'V603_FLAK_GUNH01_DLC_{side}.SGO'

# The KG7 Bohr (DLC 2) and Bohr B share one grenade-launcher pair. They keep their own rounds,
# damage and rate (already above the same-level Barrias TZ4); they get the auto-aim in ground mode
# (LockonTargetType 1: ground targets first, lobbed rounds, stock impact fuse), double durability
# (24500 / 29400 vs TZ4-R 60000) and a wider blast for crowds.
BOHR_CALLS = ('MPACK_B_WEAPON025.SGO', 'MPACK_B_WEAPON028.SGO')
BOHR_GUN = 'V603_FLAK_GLGUN01_DLC_{side}.SGO'
BOHR_EXPLOSION = 6.0   # stock 4
GROUND_TARGET_TYPE = 1.0

# Ballistics, fire rate, tracer colour, sound and muzzle flash come from the Nereid gun;
# model / bone / animation fields stay the Kepler gun's so the turret still works.
GUN_FIELDS = (
    'AmmoCount', 'FireInterval', 'FireAccuracy', 'FireRecoil', 'FireSe',
    'AmmoClass', 'AmmoSpeed', 'AmmoAlive', 'AmmoDamage', 'AmmoDamageReduce', 'AmmoExplosion',
    'AmmoIsPenetration', 'AmmoSize', 'AmmoHitSizeAdjust', 'AmmoHitImpulseAdjust', 'AmmoColor',
    'MuzzleFlash', 'MuzzleFlash_CustomParameter',
)
GUN_GRAVITY = 0.25  # Nereid uses 2.0 (it fires downward); anti-air wants a flat, fast arc
# Flak round: GrenadeBullet01 with custom type 1 bursts (blast damage + explosion effect) when its
# lifetime runs out; with bounce 0 it sticks to whatever it touches and bursts there at the same
# moment. AmmoAlive is the fuse: the EDF6AutoTurret plugin rewrites it each frame to the flight time
# to the tracked target, so rounds burst at the target's range; untracked rounds burst at max range.
GUN_AMMO = {
    'AmmoClass': 'GrenadeBullet01', 'AmmoModel': 'app:/WEAPON/bullet_grenade.rab',
    'AmmoSpeed': 8.0, 'AmmoAlive': 60.0, 'AmmoSize': 0.6, 'AmmoHitSizeAdjust': 1.0,
    'AmmoExplosion': 8.0, 'AmmoIsPenetration': 0.0,
    'AmmoColor': [3.0, 1.6, 0.6, 1.0],
    # [type 1 = burst on expiry, unused, unused, bounce 0 = stick, trail param, trail frames]
    'Ammo_CustomParameter': [1.0, -0.004, 1.0, 0.0, 0.05, 8.0],
    'AmmoHitSe': [0.0, 'common_damages_explode_S', 1.0, 1.0, 1.0, 200.0],
    'FireSe': [0.0, 'weapon_VHC_striker401_cannonTekkoRapid', 0.8, 1.0, 1.0, 40.0],
    'resource': ['app:/WEAPON/bullet_grenade.rab'],
}
# The game's own auto lock-on picks the target; the EDF6AutoTurret plugin slews the turret onto it.
# LockonType 4 auto-locks like 3 (bike missiles); the plugin patches the fire gate so type 4 also
# fires with no lock. DistributionType must stay 0: type 1 frees the list head on an empty-list shot.
# AutoTimeOut 1 keeps a lock across shots (0 consumes it on every round) until HoldTime runs out.
GUN_LOCKON = {
    'LockonType': 4.0, 'LockonTargetType': 0.0, 'Lockon_DistributionType': 0.0,
    'Lockon_FireEndToClear': 0.0, 'Lockon_AutoTimeOut': 1.0,
    'LockonAngle': [3.14, 1.57], 'LockonTime': 0.0, 'LockonFailedTime': 0.0, 'LockonHoldTime': 30.0,
}
# The guns lock everything within their full range (the plugin's proximity fuse needs to know where
# enemies are); the plugin auto-aims only within TrackRange (0.75 of the range) of EDF6AutoTurret.ini.
def py(v: object) -> object:
    if isinstance(v, (list, tuple)):
        return Node([py(x) for x in v])
    if isinstance(v, int) and not isinstance(v, bool):
        return float(v)
    return v


def load(d: str, n: str) -> dsgo.Document:
    return dsgo.parse(gamefs.read(d, n))


def build_gun(tier: str, side: str) -> bytes:
    doc = load('WEAPON', f'V603_FLAK_GUN{tier}_{side}.SGO')
    src = load('WEAPON', SOURCE_GUN).root
    r = doc.root
    damage, interval = r.get('AmmoDamage'), r.get('FireInterval')
    for k in GUN_FIELDS:
        r.set(k, copy.deepcopy(src.get(k)))
    r.set('AmmoGravityFactor', GUN_GRAVITY)
    for k, v in GUN_AMMO.items():
        if k != 'resource':
            r.set(k, py(v))
    r.set('AmmoDamage', damage * FIRE_SLOWDOWN)
    r.set('FireInterval', interval * FIRE_SLOWDOWN)
    res = r.get('resource')
    res.items += [x for x in GUN_AMMO['resource'] if x not in res.items]
    for k, v in GUN_LOCKON.items():
        r.set(k, py(v))
    # The plugin finds enemies itself (the game's lock-target registry); a zero lock range keeps the
    # guns from locking at all, so no lock markers flicker. The fire gate lets LockonType 4 fire unlocked.
    r.set('LockonRange', 0.0)
    return dsgo.write(doc)


def build_hv_gun(side: str) -> bytes:
    doc = load('WEAPON', HV_GUN.format(side=side))
    r = doc.root
    for k, v in GUN_LOCKON.items():
        r.set(k, py(v))
    r.set('LockonRange', 0.0)
    return dsgo.write(doc)


def build_bohr_gun(side: str) -> bytes:
    doc = load('WEAPON', BOHR_GUN.format(side=side))
    r = doc.root
    for k, v in GUN_LOCKON.items():
        r.set(k, py(v))
    r.set('LockonTargetType', GROUND_TARGET_TYPE)
    r.set('LockonRange', 0.0)
    r.set('AmmoExplosion', BOHR_EXPLOSION)
    return dsgo.write(doc)


def build_call(name: str, turret: list[float] | None, resources: list[str]) -> bytes:
    doc = load('WEAPON', name)
    r = doc.root
    setup = r.get('Ammo_CustomParameter').items[4].items[3]
    mul = setup.items[0]
    mul.items[0] = mul.items[0] * DURABILITY_SCALE
    if turret:
        setup.items[2].items[0].items[2] = py(turret)
    res = r.get('resource')
    res.items += [x for x in resources if x not in res.items]
    return dsgo.write(doc)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'dist', 'Mods'))
    out = os.path.abspath(ap.parse_args().out)
    files: dict[str, bytes] = {}
    for name in CALLS:
        files[f'WEAPON/{name}'] = build_call(name, TURRET, GUN_AMMO['resource'])
    for tier in sorted(set(CALLS.values())):
        for side in SIDES:
            files[f'WEAPON/V603_FLAK_GUN{tier}_{side}.SGO'] = build_gun(tier, side)
    for side in SIDES:
        files[f'WEAPON/{HV_GUN.format(side=side)}'] = build_hv_gun(side)
    for name in BOHR_CALLS:
        files[f'WEAPON/{name}'] = build_call(name, None, [])   # its turret is already the fast DLC one
    for side in SIDES:
        files[f'WEAPON/{BOHR_GUN.format(side=side)}'] = build_bohr_gun(side)
    for rel, data in files.items():
        dsgo.parse(data)
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        print(f'{rel:36s} {len(data):>10d}')


if __name__ == '__main__':
    main()
