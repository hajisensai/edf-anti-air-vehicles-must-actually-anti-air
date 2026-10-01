"""EDF6 DSGO reader/writer with byte-exact round trip.

Layout (measured on Root.cpk WEAPON/OBJECT files):
  header  'DSGO', u32 node_table_off (16), u32 node_count, u32 node_size (16)
  nodes   16 bytes {u64 value, u32 type, u32 0}, numbered in depth-first pre-order
          type 0 double, 1 UTF-16 string at node + value, 2 and 4 blob at node + value,
          3 container block at node + value
  blocks  one per container or blob, in node order, each starting 8-aligned:
          container = {u32 name_off, u32 name_count, u32 idx_off (16), u32 count},
                      u32 child node index[count], {i32 str_off_from_entry, u32 member}[name_count]
                      (names sorted by UTF-16 code unit; name_off = 0 when unnamed, idx_off = 0 when empty)
          blob      = {u32 size, u32 data_off (smallest >= 8 making data 16-aligned)}, data;
                      blob blocks start 4-aligned, container blocks 8-aligned
  pool    deduplicated UTF-16 strings (names and values), zero-terminated, to EOF.
          The game's pool order is not derivable, so the parsed order is kept and
          new strings are appended.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Union


@dataclass
class Blob:
    data: bytes
    kind: int = 2  # node type 2 or 4; same block layout


@dataclass
class Node:
    """A list (names empty) or a dictionary (member index -> name)."""
    items: list['Value']
    names: dict[int, str] = field(default_factory=dict)

    def get(self, key: str) -> 'Value':
        for i, n in self.names.items():
            if n == key:
                return self.items[i]
        raise KeyError(key)

    def set(self, key: str, value: 'Value') -> None:
        for i, n in self.names.items():
            if n == key:
                self.items[i] = value
                return
        self.names[len(self.items)] = key
        self.items.append(value)


Value = Union[float, str, Blob, Node]


@dataclass
class Document:
    root: Node
    pool: list[str]


def _utf16(buf: bytes, off: int) -> str:
    end = off
    while buf[end:end + 2] != b'\0\0':
        end += 2
    return buf[off:end].decode('utf-16le')


def parse(buf: bytes) -> Document:
    if buf[:4] != b'DSGO':
        raise ValueError(f'not a DSGO file ({buf[:4]!r})')
    table, count, size = struct.unpack_from('<III', buf, 4)
    if (table, size) != (16, 16):
        raise ValueError(f'unexpected header {table} {size}')
    pool_at: dict[int, str] = {}

    def string(off: int) -> str:
        if off not in pool_at:
            pool_at[off] = _utf16(buf, off)
        return pool_at[off]

    def node(i: int) -> Value:
        pos = table + i * size
        raw, typ, pad = struct.unpack_from('<QII', buf, pos)
        if pad:
            raise ValueError(f'node {i}: nonzero pad')
        if typ == 0:
            return struct.unpack_from('<d', buf, pos)[0]
        if typ == 1:
            return string(pos + raw)
        if typ in (2, 4):
            n, data_off = struct.unpack_from('<II', buf, pos + raw)
            return Blob(buf[pos + raw + data_off:pos + raw + data_off + n], typ)
        if typ == 3:
            d = pos + raw
            name_off, name_count, idx_off, n = struct.unpack_from('<IIII', buf, d)
            kids = [node(j) for j in struct.unpack_from(f'<{n}I', buf, d + idx_off)]
            names = {}
            for k in range(name_count):
                e = d + name_off + k * 8
                so, member = struct.unpack_from('<iI', buf, e)
                names[member] = string(e + so)
            return Node(kids, names)
        raise ValueError(f'node {i}: unknown type {typ}')

    root = node(0)
    if not isinstance(root, Node):
        raise ValueError('root is not a container')
    return Document(root, [pool_at[o] for o in sorted(pool_at)])


def _key(s: str) -> list[int]:
    return list(s.encode('utf-16le')[i] | s.encode('utf-16le')[i + 1] << 8 for i in range(0, len(s) * 2, 2))


def write(doc: Document) -> bytes:
    order: list[Value] = []

    def number(v: Value) -> None:
        order.append(v)
        if isinstance(v, Node):
            for c in v.items:
                number(c)

    number(doc.root)
    kids: dict[int, list[int]] = {}

    def assign(v: Value, me: int) -> int:
        nxt = me + 1
        if isinstance(v, Node):
            kids[id(v)] = []
            for c in v.items:
                kids[id(v)].append(nxt)
                nxt = assign(c, nxt)
        return nxt

    assign(doc.root, 0)

    out = bytearray(16 + 16 * len(order))
    struct.pack_into('<4sIII', out, 0, b'DSGO', 16, len(order), 16)
    string_refs: list[tuple[int, str, str]] = []  # (field position, text, 'Q' node | 'i' name entry)

    for i, v in enumerate(order):
        pos = 16 + 16 * i
        if isinstance(v, float):
            struct.pack_into('<dII', out, pos, v, 0, 0)
        elif isinstance(v, str):
            struct.pack_into('<QII', out, pos, 0, 1, 0)
            string_refs.append((pos, v, 'Q'))
        elif isinstance(v, Blob):
            if len(out) % 4:
                out += bytes(4 - len(out) % 4)
            at = len(out)
            data_off = 8 + (-(at + 8)) % 16
            struct.pack_into('<QII', out, pos, at - pos, v.kind, 0)
            out += struct.pack('<II', len(v.data), data_off) + bytes(data_off - 8) + v.data
        elif isinstance(v, Node):
            if len(out) % 8:
                out += bytes(8 - len(out) % 8)
            at = len(out)
            struct.pack_into('<QII', out, pos, at - pos, 3, 0)
            n = len(v.items)
            names = sorted(v.names.items(), key=lambda kv: _key(kv[1]))
            name_off = 16 + 4 * n if names else 0
            out += struct.pack('<IIII', name_off, len(names), 16 if n else 0, n)
            out += struct.pack(f'<{n}I', *kids[id(v)])
            for member, name in names:
                string_refs.append((len(out), name, 'i'))
                out += struct.pack('<iI', 0, member)
        else:
            raise TypeError(type(v))

    pool = list(doc.pool)
    seen = set(pool)
    for _, text, _k in string_refs:
        if text not in seen:
            seen.add(text)
            pool.append(text)
    placed: dict[str, int] = {}
    for text in pool:
        placed[text] = len(out)
        out += text.encode('utf-16le') + b'\0\0'
    for pos, text, kind in string_refs:
        struct.pack_into('<' + kind, out, pos, placed[text] - pos)
    return bytes(out)


def to_py(v: Value):
    """Plain JSON-able view (dicts for named containers)."""
    if isinstance(v, Node):
        if v.names and len(v.names) == len(v.items):
            return {v.names[i]: to_py(c) for i, c in enumerate(v.items)}
        return [to_py(c) for c in v.items]
    if isinstance(v, Blob):
        return f'<blob {len(v.data)}>'
    return v
