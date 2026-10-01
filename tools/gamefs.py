"""Read-only access to files inside the game's Root.cpk."""
import os, sys
from functools import lru_cache

GAME = os.environ.get('EDF6_DIR', r'D:\steam\steamapps\common\EARTH DEFENSE FORCE 6')
sys.path.insert(0, os.environ.get('EDF6_PYTOOLS', r'D:\APP\edf-coop-stable-multislot\multislot\tools'))
import cpk, crilayla  # noqa: E402


@lru_cache(maxsize=None)
def _root() -> 'cpk.Cpk':
    return cpk.Cpk(os.path.join(GAME, 'Root.cpk'))


def _key(d: str, n: str) -> tuple[str, str]:
    for k in _root().index:
        if k[0].upper() == d.upper() and k[1].upper() == n.upper():
            return k
    raise KeyError(f'{d}/{n}')


def names(d: str) -> list[str]:
    return sorted(n for dd, n in _root().index if dd.upper() == d.upper())


def read(d: str, n: str) -> bytes:
    c = _root()
    e = c.index[_key(d, n)]
    with open(c.path, 'rb') as h:
        h.seek(c.base + int(e['FileOffset']))
        data = h.read(int(e['FileSize']))
    return crilayla.decompress(data) if int(e['ExtractSize']) != int(e['FileSize']) else data
