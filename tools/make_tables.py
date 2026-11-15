#!/usr/bin/env python3
"""Converts a folder of Serum-format wavetables (32-bit float mono WAV, 2048-sample frames, `clm ` chunk) into
VESSEL's table library:

    Wavetables/<Category>/<Name>.flac   16-bit mono FLAC, frames back to back (2048 samples each)
    Source/dsp/TableList.h              the table list, in menu order (the wt_table choice index is saved in
                                        sessions, so new tables must only ever be appended to the end)

Usage:  make_tables.py <source folder> <repo root>
        Each subfolder of the source is a category. Tables with more than 64 frames keep 64 evenly spaced ones
        (first and last included); the engine crossfades between neighbours. Tables shorter than 4 frames are
        padded with copies of their last frame (afconvert cannot encode 4096 samples or fewer). Each frame loses its DC, and the
        table as a whole is scaled to a 0.95 peak (frame-to-frame level changes are kept).
        Encoding uses macOS `afconvert`.
"""
import os, re, struct, subprocess, sys, tempfile
import numpy as np

FRAME = 2048
MAX_FRAMES = 64
MIN_STORED = 4   # frames written at least (the table list keeps the real count; the loader reads only those)
RENAME_CATEGORY = {"Synth One Shots": "One Shots"}


def read_wav(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    i, fmt, data = 12, None, None
    while i + 8 <= len(d):
        cid, n = d[i:i + 4], struct.unpack("<I", d[i + 4:i + 8])[0]
        body = d[i + 8:i + 8 + n]
        if cid == b"fmt ": fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data": data = body
        i += 8 + n + (n & 1)
    tag, ch, _, _, _, bits = fmt
    if tag == 3 and bits == 32: x = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 16: x = np.frombuffer(data, dtype="<i2") / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
    else:
        raise ValueError("unsupported WAV format %s/%s" % (tag, bits))
    if ch > 1: x = x.reshape(-1, ch).mean(axis=1)
    return x


def table_name(file_stem):
    s = re.sub(r"^ESW\s+", "", file_stem).strip()
    s = s.split(" - ", 1)[1] if " - " in s else re.sub(r"^\w+\s+", "", s)   # "Analog - 303 Saw" / "Digital Bell 03"
    return re.sub(r"\s+", " ", s).strip()


def natural(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", s)]


def main():
    src, repo = sys.argv[1], sys.argv[2]
    out_dir = os.path.join(repo, "Wavetables")
    header = os.path.join(repo, "Source", "dsp", "TableList.h")
    entries = []
    for cat_dir in sorted(os.listdir(src)):
        full = os.path.join(src, cat_dir)
        if not os.path.isdir(full): continue
        cat = RENAME_CATEGORY.get(cat_dir, cat_dir)
        seen = set()
        files = [f for f in os.listdir(full) if f.lower().endswith(".wav") and not f.startswith("._")]
        for fn in sorted(files, key=lambda f: natural(table_name(os.path.splitext(f)[0]))):
            name = table_name(os.path.splitext(fn)[0])
            base, k = name, 2
            while name.lower() in seen: name = "%s %d" % (base, k); k += 1
            seen.add(name.lower())
            entries.append((cat, name, os.path.join(full, fn)))

    tmp = tempfile.mkdtemp()
    rows, total = [], 0
    for cat, name, path in entries:
        x = read_wav(path)
        n = max(1, len(x) // FRAME)
        frames = x[: n * FRAME].reshape(n, FRAME) if len(x) >= FRAME else np.resize(x, (1, FRAME))
        if n > MAX_FRAMES:
            frames = frames[np.round(np.linspace(0, n - 1, MAX_FRAMES)).astype(int)]
        frames = frames - frames.mean(axis=1, keepdims=True)
        peak = np.abs(frames).max()
        if peak < 1e-6:
            print("skip (silent):", cat, name); continue
        frames *= 0.95 / peak
        stored = frames
        if len(stored) < MIN_STORED:   # afconvert writes an empty FLAC for <= 4096 samples: pad with the last frame
            stored = np.concatenate([stored, np.repeat(stored[-1:], MIN_STORED - len(stored), axis=0)])
        pcm = np.clip(np.round(stored.reshape(-1) * 32767.0), -32767, 32767).astype("<i2")

        safe = re.sub(r"[^A-Za-z0-9 ._()&+-]", "", name).strip()
        rel = "%s/%s.flac" % (cat, safe)
        dst = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        wav = os.path.join(tmp, "t.wav")
        with open(wav, "wb") as f:
            data = pcm.tobytes()
            f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 48000, 96000, 2, 16)
                    + b"data" + struct.pack("<I", len(data)) + data)
        subprocess.run(["afconvert", "-f", "flac", "-d", "flac", wav, dst], check=True)
        if os.path.getsize(dst) < 1024:
            raise RuntimeError("afconvert produced an empty FLAC for " + rel)
        rows.append((cat, name, rel, len(frames)))
        total += os.path.getsize(dst)

    def q(s): return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    lines = ["#pragma once",
             "",
             "// Generated by tools/make_tables.py from the Echo Sound Works Core Tables (CC0 / public domain).",
             "// Menu order is saved in sessions (the wt_table choice index): new tables are only ever appended.",
             "",
             "namespace vessel::dsp",
             "{",
             "struct TableInfo { const char* category; const char* name; const char* file; int frames; };",
             "",
             "inline constexpr TableInfo kTables[] =",
             "{"]
    for cat, name, rel, nf in rows:
        lines.append("    { %s, %s, %s, %d }," % (q(cat), q(name), q(rel), nf))
    lines += ["};", "", "inline constexpr int kNumTables = (int) (sizeof (kTables) / sizeof (kTables[0]));",
              "} // namespace vessel::dsp", ""]
    open(header, "w").write("\n".join(lines))
    print("%d tables, %d frames, %.1f MB of FLAC -> %s" % (len(rows), sum(r[3] for r in rows), total / 1e6, out_dir))


if __name__ == "__main__":
    main()
