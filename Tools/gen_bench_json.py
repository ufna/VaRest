#!/usr/bin/env python3
"""Deterministic benchmark fixtures shaped like a game backend's list endpoint.

    python3 Tools/gen_bench_json.py <out_dir>

Writes bench_1mb.json and bench_10mb.json (same seed every run, so numbers are comparable).
"""
import json
import random
import sys
from pathlib import Path

WORDS = ["sword", "shield", "potion", "Ключ", "épée", "盾", "arrow", "helm", "ring", "boots"]


def item(rng, i):
    return {
        "id": i,
        "uid": 9007199254740993 + i,          # beyond 2^53: must stay exact
        "name": f"{rng.choice(WORDS)} #{i}",
        "desc": "line one\nline \"two\"\t" + rng.choice(WORDS) * rng.randint(1, 4),
        "price": round(rng.uniform(0.01, 9999.99), 2),
        "weight": rng.random(),
        "active": rng.random() > 0.3,
        "owner": None if rng.random() > 0.8 else {"id": rng.randint(1, 10**6), "nick": rng.choice(WORDS)},
        "tags": rng.sample(WORDS, rng.randint(0, 4)),
        "stats": {k: rng.randint(-100, 100) for k in ("str", "dex", "int", "luck")},
    }


def build(target_bytes, seed):
    rng = random.Random(seed)
    items, size, i = [], 0, 0
    while size < target_bytes:
        it = item(rng, i)
        items.append(it)
        size += len(json.dumps(it, ensure_ascii=False).encode("utf-8")) + 1
        i += 1
    return {"ok": True, "data": {"items": items, "count": len(items)}}


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, size in (("bench_1mb.json", 1 << 20), ("bench_10mb.json", 10 << 20)):
        text = json.dumps(build(size, 20261019), ensure_ascii=False, separators=(",", ":"))
        (out / name).write_text(text, encoding="utf-8")
        print(f"{name}: {len(text.encode('utf-8'))} bytes")


if __name__ == "__main__":
    main()
