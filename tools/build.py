"""Build the anti-air autocannon vehicle (Kepler chassis + Nereid auto-capture cannon) into dist/Mods.

  python tools/build.py [--out dist/Mods] [--base-mods <game>/Mods]

WEAPONTABLE / WEAPONTEXT are rebuilt from the game's tables. With --base-mods, tables already in
that Mods folder (other mods that appended rows) are used as the base, and our row is replaced in
place if present, so several appending mods can coexist.
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

WEAPON_ID = 'aWeaponAAFlak01'
CALL_SGO = 'AWEAPONAAFLAK01.SGO'
VEHICLE_SGO = 'V603_FLAK_AA.SGO'
GUN_SGO = {'L': 'V603_FLAK_AAGUN01_L.SGO', 'R': 'V603_FLAK_AAGUN01_R.SGO'}
TEMPLATE_CALL = 'AWEAPON349.SGO'   # KG6 Kepler E (Ranger vehicle call)
TEMPLATE_ROW = 'aWeapon349'
SOURCE_GUN = 'V_409HELI_GATLING01.SGO'   # EF31 Nereid auto-capture cannon
LANGS = ('JA', 'EN', 'CN', 'KR', 'SC')
ACQUIRE = 0.0           # WEAPONTABLE column 5: 0 = ordinary weapon. non-zero appears to mark DLC content whose owned bit is dropped on reload (inferred, see re-notes); unlocking is a save write, not a table value
DURABILITY_MUL = 6.0    # Kepler F tier
DAMAGE_MUL = 6.0
TURRET = [65.0, 0.3, 0.3]  # gun-L turret params (DLC Kepler values: faster traverse)
BASE_DURABILITY = 350

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
# Damage x2 / fire interval x2 against the Nereid gun keeps the same damage per second.
GUN_AMMO = {
    'AmmoClass': 'GrenadeBullet01', 'AmmoModel': 'app:/WEAPON/bullet_grenade.rab',
    'AmmoSpeed': 8.0, 'AmmoAlive': 60.0, 'AmmoSize': 0.6, 'AmmoHitSizeAdjust': 1.0,
    'AmmoExplosion': 6.0, 'AmmoDamage': 15.0, 'AmmoIsPenetration': 0.0,
    'AmmoColor': [3.0, 1.6, 0.6, 1.0],
    # [type 1 = burst on expiry, unused, unused, bounce 0 = stick, trail param, trail frames]
    'Ammo_CustomParameter': [1.0, -0.004, 1.0, 0.0, 0.05, 8.0],
    'AmmoHitSe': [0.0, 'common_damages_explode_S', 1.0, 1.0, 1.0, 200.0],
    'FireInterval': 6.0,
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
CALL_READY_AT_START = True  # ReloadInit 1: callable right at mission start

NAMES = {
    'ja': 'ＫＧ６ケプラー・ネレイド',
    'en': 'KG6 Kepler Nereid',
    'cn': 'ＫＧ６克卜勒・海神',
    'kr': 'KG6 케플러 네레이드',
    'sc': 'ＫＧ６克卜勒・海神',
}
GUN_NAMES = {
    'L': {'ja': '自動捕捉機関砲（左）', 'en': 'Auto-Capture Cannon (Left)', 'cn': '自動捕捉加農砲（左）',
          'kr': '자동 포착 기관포(좌)', 'sc': '自动捕捉加农炮（左）'},
    'R': {'ja': '自動捕捉機関砲（右）', 'en': 'Auto-Capture Cannon (Right)', 'cn': '自動捕捉加農砲（右）',
          'kr': '자동 포착 기관포(우)', 'sc': '自动捕捉加农炮（右）'},
}


def describe(lang: str, dmg: float, rng: float) -> str:
    d, r = f'{dmg:.2f}'.rstrip('0').rstrip('.'), f'{rng:.1f}'
    return {
        'SC': f'操纵席：驾驶\n自动捕捉加农炮（左／右）（伤害：{d}  射程：{r}m）\n\n搭乘者１名。\n'
              '在克卜勒的车体上移植了海神直升机的自动捕捉加农炮。炮塔会自动对准射角内的敌人，'
              '优先锁定空中目标，驾驶员只需专心移动与扣下扳机。\n\n'
              '需要在战场上取得一定的功绩（击破多数敌人）才能请求投落。（伙伴的击破数也能列入功绩计算）',
        'CN': f'操縱席：駕駛\n自動捕捉加農砲（左／右）（傷害：{d}  射程：{r}m）\n\n搭乘者１名。\n'
              '在克卜勒的車體上移植了海神直升機的自動捕捉加農砲。砲塔會自動對準射角內的敵人，'
              '優先鎖定空中目標，駕駛員只需專心移動與扣下扳機。\n\n'
              '需要在戰場上取得一定的功績（擊破多數敵人）才能請求投落。（夥伴的擊破數也能列入功績計算）',
        'JA': f'操縦席：運転\n自動捕捉機関砲（左／右）（ダメージ：{d}  射程：{r}m）\n\n搭乗者１名。\n'
              'ケプラーの車体にネレイドの自動捕捉機関砲を移植した対空車両。砲塔は射角内の敵を自動で狙い、'
              '空中の敵を優先して捕捉する。\n\n'
              '戦場で一定の功績（多数の敵の撃破）をあげなければ投下要請できない。（仲間の撃破数も功績に数えられる）',
        'EN': f'Driver seat\nAuto-Capture Cannon (L/R) (Damage: {d}  Range: {r}m)\n\nSeats 1.\n'
              "A Kepler chassis fitted with the Nereid's auto-capture cannons. The turret aims itself at "
              'enemies in its firing arc, preferring airborne targets.\n\n'
              'Can only be requested after earning enough merit on the battlefield (defeating many enemies). '
              "(Teammates' kills also count.)",
        'KR': f'조종석: 운전\n자동 포착 기관포(좌/우) (대미지: {d}  사정거리: {r}m)\n\n탑승자 1명.\n'
              '케플러 차체에 네레이드의 자동 포착 기관포를 이식한 대공 차량. 포탑이 사각 안의 적을 자동으로 조준하며 '
              '공중의 적을 우선 포착한다.\n\n'
              '전장에서 일정한 공적(다수의 적 격파)을 올려야 투하를 요청할 수 있다. (동료의 격파 수도 공적에 포함된다)',
    }[lang]


def py(v: object) -> object:
    if isinstance(v, (list, tuple)):
        return Node([py(x) for x in v])
    if isinstance(v, int) and not isinstance(v, bool):
        return float(v)
    return v


def load(d: str, n: str) -> dsgo.Document:
    return dsgo.parse(gamefs.read(d, n))


def load_base(base_mods: str | None, d: str, n: str) -> dsgo.Document:
    if base_mods:
        p = os.path.join(base_mods, d, n)
        if os.path.exists(p):
            return dsgo.parse(open(p, 'rb').read())
    return load(d, n)


def set_names(root: Node, names: dict[str, str]) -> None:
    for lang, text in names.items():
        root.set(f'name.{lang}', text)


def build_gun(side: str) -> bytes:
    doc = load('WEAPON', f'V603_FLAK_GUN02_{side}.SGO')
    src = load('WEAPON', SOURCE_GUN).root
    r = doc.root
    for k in GUN_FIELDS:
        r.set(k, copy.deepcopy(src.get(k)))
    r.set('AmmoGravityFactor', GUN_GRAVITY)
    for k, v in GUN_AMMO.items():
        r.set(k, py(v))
    for k, v in GUN_LOCKON.items():
        r.set(k, py(v))
    r.set('LockonRange', gun_range())
    set_names(r, GUN_NAMES[side])
    return dsgo.write(doc)


def gun_range() -> float:
    return GUN_AMMO['AmmoSpeed'] * GUN_AMMO['AmmoAlive']


def build_call() -> bytes:
    doc = load('WEAPON', TEMPLATE_CALL)
    r = doc.root
    set_names(r, NAMES)
    if CALL_READY_AT_START:
        r.set('ReloadInit', 1.0)
    cp = r.get('Ammo_CustomParameter')
    spawn = cp.items[4]
    spawn.items[2] = f'app:/Object/{VEHICLE_SGO.lower()}'
    setup = spawn.items[3]
    setup.items[0] = py([DURABILITY_MUL, DAMAGE_MUL])
    guns = setup.items[2]
    guns.items[0].items[0] = f'app:/weapon/{GUN_SGO["L"].lower()}'
    guns.items[0].items[2] = py(TURRET)
    guns.items[1].items[0] = f'app:/weapon/{GUN_SGO["R"].lower()}'
    res = r.get('resource')
    res.items = [x for x in res.items if 'flak' not in str(x).lower()]
    res.items += [spawn.items[2], guns.items[0].items[0], guns.items[1].items[0], *GUN_AMMO['resource']]
    return dsgo.write(doc)


def build_vehicle() -> bytes:
    doc = load('OBJECT', 'V603_FLAK.SGO')
    setup = doc.root.get('vehicle_setup')
    guns = setup.items[2]
    guns.items[0].items[0] = f'app:/weapon/{GUN_SGO["L"].lower()}'
    guns.items[0].items[2] = py(TURRET)
    guns.items[1].items[0] = f'app:/weapon/{GUN_SGO["R"].lower()}'
    return dsgo.write(doc)


def build_table(base_mods: str | None) -> tuple[bytes, int, int]:
    """Returns (table bytes, template row index, our row index)."""
    doc = load_base(base_mods, 'WEAPON', 'WEAPONTABLE.SGO')
    rows = doc.root.get('table').items
    ids = [row.items[0] for row in rows]
    row = copy.deepcopy(rows[ids.index(TEMPLATE_ROW)])
    row.items[0] = WEAPON_ID
    row.items[1] = f'app:/weapon/{WEAPON_ID}.sgo'
    row.items[5] = ACQUIRE
    if WEAPON_ID in ids:
        at = ids.index(WEAPON_ID)
        rows[at] = row
    else:
        at = len(rows)
        rows.append(row)
    return dsgo.write(doc), ids.index(TEMPLATE_ROW), at


def build_text(base_mods: str | None, lang: str, template: int, at: int) -> bytes:
    doc = load_base(base_mods, 'WEAPON', f'WEAPONTEXT.{lang}.SGO')
    rows = doc.root.get('text_table').items
    row = copy.deepcopy(rows[template])
    row.items[0] = NAMES[lang.lower()]
    dmg = GUN_AMMO['AmmoDamage'] * DAMAGE_MUL
    row.items[1] = describe(lang, dmg, gun_range())
    for stat in row.items[2].items:
        if len(stat.items) == 2 and isinstance(stat.items[1], str) and stat.items[1].isdigit():
            stat.items[1] = str(int(BASE_DURABILITY * DURABILITY_MUL))
    if at < len(rows):
        rows[at] = row
    elif at == len(rows):
        rows.append(row)
    else:
        raise ValueError(f'WEAPONTEXT.{lang}: {len(rows)} rows, table row at {at}')
    return dsgo.write(doc)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'dist', 'Mods'))
    ap.add_argument('--base-mods', default=None)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    files: dict[str, bytes] = {}
    table, template, at = build_table(a.base_mods)
    files['WEAPON/WEAPONTABLE.SGO'] = table
    for lang in LANGS:
        files[f'WEAPON/WEAPONTEXT.{lang}.SGO'] = build_text(a.base_mods, lang, template, at)
    files[f'WEAPON/{CALL_SGO}'] = build_call()
    for side, name in GUN_SGO.items():
        files[f'WEAPON/{name}'] = build_gun(side)
    files[f'OBJECT/{VEHICLE_SGO}'] = build_vehicle()
    for rel, data in files.items():
        dsgo.parse(data)
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        print(f'{rel:36s} {len(data):>10d}')
    print(f'row {at} ({WEAPON_ID})')


if __name__ == '__main__':
    main()
