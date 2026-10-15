"""Write a travel-frog save frame that the sim/firmware will actually accept.

frame_t layout as MSVC lays it out (tf_save_t is 8-byte aligned, so the body starts at 32):
  0  u32 magic 'TRFG'   4  u16 version   6  u16 body_len   8  u32 seq
  12 pad                16 i64 saved_at 24 u32 crc        28 pad   32 body[136]
"""
import struct, sys

BODY = {
    "state": 0, "clovers": 4, "items": 8, "trip_end_ms": 24, "angry_end_ms": 32,
    "trip_tier": 40, "cards": 44, "clover_ready_ms": 96,
    "last_reward_card": 128, "last_reward_clovers": 132,
}
FRESH = dict(state=0, clovers=5, items=[1, 0, 0, 0], trip_end_ms=0, angry_end_ms=0,
             trip_tier=0, cards=[0] * 12, clover_ready_ms=[0] * 4,
             last_reward_card=-1, last_reward_clovers=0)


def crc32(data):
    c = 0xFFFFFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 & (-(c & 1) & 0xFFFFFFFF))
    return c ^ 0xFFFFFFFF


def frame(seq, saved_at, body):
    b = bytearray(136)
    for k, off in BODY.items():
        v = body[k]
        if isinstance(v, list):
            fmt = "<" + ("i" * len(v) if k in ("items", "cards") else "q" * len(v))
            struct.pack_into(fmt, b, off, *v)
        else:
            struct.pack_into("<q" if k.endswith("_ms") else "<i", b, off, v)
    f = bytearray(168)
    struct.pack_into("<IHHI", f, 0, 0x47465254, 1, 136, seq)
    struct.pack_into("<q", f, 16, saved_at)
    f[28:32] = b"\0\0\0\0"
    f[32:168] = b
    struct.pack_into("<I", f, 24, crc32(bytes(f[:24]) + bytes(f[28:])))
    return bytes(f)


def main():
    path = sys.argv[1]
    kv = {}
    for a in sys.argv[2:]:
        k, v = a.split("=", 1)
        kv[k] = int(v)
    body = dict(FRESH, **kv)
    seq = kv.pop("_seq", 99)
    open(path, "wb").write(frame(seq, 60, body))
    print(f"{path}: seq={seq} " + " ".join(f"{k}={v}" for k, v in sorted(kv.items())))


if __name__ == "__main__":
    main()
