#!/usr/bin/env python3
"""Reads what a measured run left on the SD card (phase F0 of the engine plan).

perf.bin is written by 3ds_prof.c: a 128-byte header naming the executable,
then one record per game frame (64 bytes in version 1, 96 in version 2, 112 in
versions 3 and 4, 128 in version 5, 216 in version 6).
Version 4 adds, of a picture, where the GPU was started early and the surfaces
it went without (engine reform, stage 1); version 5, of a voxel picture, the
CPU time of CtrVoxel_Draw and its passes and of the update's billboards
(stage 3, measure only); version 6, of a battle, the animation playing, a
double battle, why the scenery and the text were composed, the walks and the
sections frame by frame, and of every submission its queue's drains (heavy
battles, item M). port.log has the PERF, GPUMEM and SCENARIO lines. Commands:

  summary RUN.bin [--warmup N] [--measure M]   per mode: percentiles, lost refreshes, 3D
  compare A.bin B.bin [--warmup N]             the same, two runs side by side
  ab --a A1.bin A2.bin.. --b B1.bin B2.bin..   alternating repetitions: medians and spread
                                               (--settle 600 per run, --measure 6000 per mode)
  battle RUN.bin [--min N]                     version 6: by route, battle kind, animation and
                                               toggles (at least N frames): what each one cost
  csv RUN.bin OUT.csv                          every frame, for a spreadsheet
  identity RUN.bin --elf X.elf                 whether the run came from that executable
  sites port.log --elf X.elf                   GPUMEM sites named by function

The "ready" time is a picture's CPU work up to presenting it, without the wait
for the display; "gpuEnd" is when the GPU finished a submission, from the
moment its frame began, as the DROP line reckons it in every version: when it
set the GPU going plus its drawing time. That falls short when the GPU started
early, since citro3d times the drawing from FrameEnd; version 3's "gpuDone",
when the submission's queue last drained (citro3d swapping the top screen), is
reported apart. Version 2 keeps game frames (ticks), pictures and GPU
submissions apart: percentiles of ready are over the pictures, of gpuEnd over
the submissions whose drawing time was read, and a lost refresh is counted
only between two pictures with nothing meant to stop them in between (a held
top screen, a resume). Version 1 runs are read as before: every frame a
picture, which counted hidden frames' stale figures again.
--warmup drops the first N frames of every stretch of one screen in one mode.
"""
import argparse
import bisect
import os
import statistics
import struct
import subprocess
import sys

HEADER = struct.Struct("<4I40s12s4I8s24s12s")
RECORDS = {1: struct.Struct("<2I8B24H"), 2: struct.Struct("<4I8B28H8H"), 3: struct.Struct("<4I8B28H8HH6B8x"),
           4: struct.Struct("<4I8B28H8HH8B6x"), 5: struct.Struct("<4I8B28H8HH8B6x8H"),
           6: struct.Struct("<4I8B28H8HH8B6x8H" "IH4B2H2B3H2B12B13H12H2x")}
MAGIC = 0x46503345
TIMINGS = ["ready", "game", "bottom", "vblank", "present", "prep", "wait", "gpu", "gpuStart",
           "gap", "arrive", "palette", "layers", "draw", "frameEnd", "voxel", "voxelAfter",
           "stereoSample", "stereoEyes", "tasks", "sprites", "mix", "load", "profiler",
           "ioWait", "ioRead", "afterOwed", "spare2"]
COUNTS = ["draws", "targets", "copies", "syncReads", "chunksMissing", "modelsSkipped",
          "spritesLeftOut", "chunkBuilds"]
# Version 5: of a voxel picture (flag "voxel"), CtrVoxel_Draw's CPU time with
# both eyes summed and its passes (the chunks, the dynamic buffer's cards,
# the light in the air, the weather's particles), then the update's
# billboards and, of them, the slots gathered and the packing. Means over
# the voxel pictures; NaN for older runs.
VOXEL = ["vDraw", "vChunks", "vCards", "vAir", "vWeather", "vBillboards", "vGather", "vPack"]
# Version 6: of a picture, the layer walks and the sprites' (sBgTicks,
# sObjTicks), then the battle's sections, all inside draw: bw.* hold bs.* or
# bm.*, bs.whole holds bm.*. Means over the battle's pictures; NaN for older runs.
BATTLE = ["bgWalk", "objWalk", "bsPrep", "bsSlots", "bsText", "bsEyes", "bsWhole", "bwWorld", "bwPost", "bwScene",
          "bmScene", "bmPlace", "bmText"]
# Bits of why a battle's scenery and text were composed or kept (CTR_SCENE_WHY_*,
# CTR_TEXT_WHY_*), of its slots (3ds_video.c), and what a drain had queued (CTR_STAGE_*).
SCENE_WHY = ["reg", "scroll", "lines", "drawable", "version", "key", "palette", "stamp", "vram", "still", "path",
             "new", "slots", "kept", "composed", "layer"]
TEXT_WHY = ["reg", "view", "palette", "box", "pictures", "bgStamp", "bankStamp", "vram", "fade", "path", "kept"]
SLOTS_AGAIN = ["new", "pack", "moved", "palette", "blend", "window", "tiles"]
SLOTS_UNKEPT = ["scene", "bg", "objwin", "hud", "weather", "mosaic", "line", "size"]
STAGES = ["none", "layers", "world", "post", "scene", "logical", "end"]
STAGE_SWAP = 0x80
ANIM_KINDS = ["none", "move", "general", "special", "status"]
BATTLE_IN, BATTLE_DOUBLE = 1, 2
DRAIN_LIST = 12
SHOWN_TWICE = 254
MODES = ["other", "field", "voxel", "battle", "voxel-battle", "stage", "centred", "bottom"]
ROUTES = ["mono", "eyes", "planes", "battle-stereo", "voxel", "voxel-stereo", "battle-world",
          "battle-world-stereo", "transition", "blank", "bottom"]
FLATS = ["blend", "mosaic", "bg-front", "slots", "voxel-off", "no-surface", "no-planes", "other"]
# Version 2: what a game frame was (CTR_TICK_*), and the record's kind and flag bits.
KINDS = ["none", "shown", "paced", "fast", "held", "failed"]
PRESENTED = 1
KIND_BOUNDARY, KIND_LATE_CPU, KIND_LATE_GPU = 0x10, 0x20, 0x40
FLAGS = ["loading", "warmup", "resume", "pacing", "crossing", "voxel", "scenario", "fast"]
# A picture's own timings: averaged over the pictures, the rest over every frame.
PICTURE = {"present", "prep", "wait", "gap", "arrive", "palette", "layers", "draw", "frameEnd",
           "voxel", "voxelAfter", "afterOwed", "stereoSample", "stereoEyes"}
GPU = {"gpu", "gpuStart"}
PER_PICTURE_COUNTS = {"draws", "targets", "copies"}
# The components worth a column in the summary, in the order a frame spends them.
SHOWN = ["game", "bottom", "tasks", "sprites", "load", "ioWait", "ioRead", "vblank", "prep", "present",
         "palette", "layers", "draw", "frameEnd", "voxel", "voxelAfter", "afterOwed", "stereoSample", "stereoEyes",
         "gpu"]
# Version 3: a battle frame without the 3D slider, by what it did with the scenery.
SCENERY = ["none", "kept", "apart", "sprites alone", "whole"]
# Version 4: where the GPU was started early (CTR_EARLY_*), and the surfaces a
# picture went without (CTR_DENIED_* bits).
EARLY = ["frameEnd", "layers", "scene", "eye", "plane", "transition", "voxel", "battle"]
DENIED = ["layer", "plane", "bottom", "scene", "keeper", "blend", "slots", "voxel"]
# The plan's frame target (a target, not a promise): p99 of a picture's CPU work.
TARGET_P99_MS = 14.0


class Frame:
    __slots__ = ("tick", "scene", "image", "gpu_of", "kind", "mode", "route", "slider", "stereo", "flat",
                 "lost", "flags", "t", "n", "gpu_end", "picture",
                 "gpu_done", "shown", "scenery", "absent", "terrain", "asset_waits", "asset_fails",
                 "gpu_early", "denied", "v",
                 "twice", "toggles", "anim_id", "anim_kind", "battle", "sprites", "switches", "scene_why", "text_why",
                 "slots_again", "slots_unkept", "tiles", "vertices", "objects", "drains", "drains_lost", "drain_list",
                 "b")


def read_run(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size:
        sys.exit(f"{path}: too short for a perf.bin")
    fields = HEADER.unpack_from(data, 0)
    magic, version, size, timings, commit, diff, dirty, low, high, model, appver, linkid, reserved = fields
    record = RECORDS.get(version)
    counts = reserved[0] if version >= 2 else 0
    voxel = reserved[1] if version >= 5 else len(VOXEL)
    battle = reserved[2] if version >= 6 else len(BATTLE)
    drain_list = reserved[3] if version >= 6 else DRAIN_LIST
    if (magic != MAGIC or record is None or size != record.size
            or timings != (24 if version == 1 else len(TIMINGS)) or counts not in (0, len(COUNTS))
            or voxel != len(VOXEL) or battle != len(BATTLE) or drain_list != DRAIN_LIST):
        sys.exit(f"{path}: not a perf.bin this script reads "
                 f"(magic {magic:#x}, version {version}, record {size}, timings {timings})")
    header = {
        "format": version,
        "version": appver.rstrip(b"\0").decode(),
        "commit": commit.rstrip(b"\0").decode(),
        "diff": diff.rstrip(b"\0").decode(),
        "dirty": dirty,
        "code": (high << 32) | low,
        "model": model,
        "linkid": linkid.rstrip(b"\0").decode(errors="replace"),
    }
    frames = []
    for offset in range(HEADER.size, len(data) - record.size + 1, record.size):
        v = record.unpack_from(data, offset)
        fr = Frame()
        if version == 1:
            fr.tick, fr.scene, fr.mode, fr.route, fr.slider, fr.stereo, fr.flat, fr.lost, fr.flags = v[:9]
            fr.t = [x / 100.0 for x in v[10:]] + [0.0] * (len(TIMINGS) - 24)
            fr.n = [0] * len(COUNTS)
            fr.image = fr.gpu_of = 0
            fr.kind = PRESENTED
            fr.picture = True
            # v1 flags: voxel frame, scenario, sped up - v2's bits 5 to 7.
            fr.flags = (fr.flags & 7) << 5
        else:
            (fr.tick, fr.scene, fr.image, fr.gpu_of, fr.kind, fr.mode, fr.route, fr.slider,
             fr.stereo, fr.flat, fr.lost, fr.flags) = v[:12]
            fr.t = [x / 100.0 for x in v[12:12 + len(TIMINGS)]]
            fr.n = list(v[12 + len(TIMINGS):12 + len(TIMINGS) + len(COUNTS)])
            fr.picture = (fr.kind & 0x0F) == PRESENTED
        if version >= 3:
            (fr.gpu_done, fr.shown, fr.scenery, fr.absent, fr.terrain, fr.asset_waits,
             fr.asset_fails) = v[12 + len(TIMINGS) + len(COUNTS):12 + len(TIMINGS) + len(COUNTS) + 7]
            fr.gpu_done /= 100.0
        else:
            fr.gpu_done, fr.shown, fr.scenery, fr.absent, fr.terrain = 0.0, 255, 0, 0, 0
            fr.asset_waits = fr.asset_fails = 0
        base = 12 + len(TIMINGS) + len(COUNTS) + 7
        if version >= 4:
            fr.gpu_early, fr.denied = v[base:base + 2]
        else:
            fr.gpu_early, fr.denied = None, 0
        fr.v = [x / 100.0 for x in v[base + 2:base + 2 + len(VOXEL)]] if version >= 5 else None
        # Version 6's own code in shown: this picture and the one before took
        # effect at one VBlank, a 0 of its own time on screen and no unknown
        # (254 in v3 to v5 is a count, kept as it was).
        fr.twice = version >= 6 and fr.shown == SHOWN_TWICE
        if fr.twice:
            fr.shown = 0
        if version >= 6:
            i = base + 2 + len(VOXEL)
            (fr.toggles, fr.anim_id, fr.anim_kind, fr.battle, fr.sprites, fr.switches, fr.scene_why, fr.text_why,
             fr.slots_again, fr.slots_unkept, fr.tiles, fr.vertices, fr.objects, fr.drains,
             fr.drains_lost) = v[i:i + 15]
            i += 15
            stages = v[i:i + DRAIN_LIST]
            i += DRAIN_LIST
            fr.b = [x / 100.0 for x in v[i:i + len(BATTLE)]]
            i += len(BATTLE)
            fr.drain_list = [(t / 100.0, stage) for t, stage in zip(v[i:i + DRAIN_LIST], stages)][:fr.drains]
        else:
            fr.b = None
            fr.toggles = fr.anim_id = fr.anim_kind = fr.battle = fr.sprites = fr.switches = 0
            fr.scene_why = fr.text_why = fr.slots_again = fr.slots_unkept = 0
            fr.tiles = fr.vertices = fr.objects = fr.drains = fr.drains_lost = 0
            fr.drain_list = []
        fr.gpu_end = None
        frames.append(fr)
    if version == 1:
        # The GPU time read at a FrameBegin is the previous frame's.
        for prev, cur in zip(frames, frames[1:]):
            prev.gpu_end = prev.t[TIMINGS.index("gpuStart")] + cur.t[TIMINGS.index("gpu")]
    else:
        # Both figures of a record belong to submission gpu_of. Version 3's
        # gpu_done (when its queue last drained) stays a figure of its own,
        # so that gpuEnd means the same in a v2 run and a v3 one.
        for fr in frames:
            if fr.gpu_of:
                fr.gpu_end = fr.t[TIMINGS.index("gpuStart")] + fr.t[TIMINGS.index("gpu")]
    return header, frames


def stretches(frames):
    """Runs of frames on one screen in one mode."""
    run = []
    for fr in frames:
        if run and (fr.scene != run[-1].scene or fr.mode != run[-1].mode):
            yield run
            run = []
        run.append(fr)
    if run:
        yield run


def percentile(values, fraction):
    if not values:
        return float("nan")
    values = sorted(values)
    index = min(len(values) - 1, max(0, int(-(-len(values) * fraction // 1)) - 1))
    return values[index]


def groups(frames, warmup, measure=None, settle=0):
    """Frames by mode: the first `settle` frames of the run dropped (the
    save's load, the caches filling), then the first `warmup` of every
    stretch, and at most `measure` frames a mode."""
    by_mode = {}
    for run in stretches(frames[settle:]):
        kept = run[warmup:]
        have = by_mode.setdefault(run[0].mode, [])
        if measure is not None:
            kept = kept[:max(0, measure - len(have))]
        have.extend(kept)
    return by_mode


def mean(values):
    return sum(values) / len(values) if values else float("nan")


def stats(frames, v3=False):
    pictures = [f for f in frames if f.picture]
    ready = [f.t[0] for f in pictures]
    # ready without the voxel builds after FrameEnd, which fill the time left
    # to the VBlank on purpose: the part of a picture's work that is not elastic.
    # The slice owed a static model missing from the picture runs there too,
    # whatever the budget, and stays in (afterOwed: 0 before version 3).
    after, owed = TIMINGS.index("voxelAfter"), TIMINGS.index("afterOwed")
    fixed = [f.t[0] - (f.t[after] - f.t[owed]) for f in pictures]
    gpu = [f for f in frames if f.gpu_end is not None]
    gpu_end = [f.gpu_end for f in gpu]
    # Version 3: when the queue drained, over the submissions whose swap was seen.
    gpu_done = [f.gpu_done for f in gpu if f.gpu_done]
    nan = float("nan")
    out = {
        "frames": len(frames),
        "images": len(pictures),
        "ready50": percentile(ready, 0.5), "ready95": percentile(ready, 0.95),
        "ready99": percentile(ready, 0.99), "readyMax": max(ready) if ready else float("nan"),
        "fixed95": percentile(fixed, 0.95), "fixed99": percentile(fixed, 0.99),
        "gpuEnd50": percentile(gpu_end, 0.5), "gpuEnd95": percentile(gpu_end, 0.95),
        "gpuEnd99": percentile(gpu_end, 0.99), "gpuEndMax": max(gpu_end) if gpu_end else float("nan"),
        "gpuDone50": percentile(gpu_done, 0.5) if v3 else nan,
        "gpuDone95": percentile(gpu_done, 0.95) if v3 else nan,
        "gpuDone99": percentile(gpu_done, 0.99) if v3 else nan,
        "gpuDoneMax": max(gpu_done) if gpu_done else nan,
        "gpuDoneSeen": len(gpu_done), "gpuSubmissions": len(gpu),
        "missed": sum(f.lost for f in pictures),
        "missedFrames": sum(1 for f in pictures if f.lost),
        "missedCpu": sum(f.lost for f in pictures if f.kind & KIND_LATE_CPU),
        "missedGpu": sum(f.lost for f in pictures if f.kind & KIND_LATE_GPU),
        "boundaries": sum(1 for f in pictures if f.kind & KIND_BOUNDARY),
        # Version 3 (NaN for older runs): refreshes repeated by the swaps'
        # VBlanks, effects left out, game-thread asset waits. A picture's
        # shown is submission gpu_of's, so a boundary picture's counts: one
        # past a held top screen or a resume comes out not known (255).
        "repeated": sum(f.shown for f in pictures if f.shown != 255) if v3 else nan,
        "repeatedFrames": sum(1 for f in pictures if f.shown not in (0, 255)) if v3 else nan,
        "shownUnknown": sum(1 for f in pictures if f.shown == 255) if v3 else nan,
        "absent": sum(f.absent for f in frames) if v3 else float("nan"),
        "terrainSkipped": sum(1 for f in frames if f.terrain & 7) if v3 else float("nan"),
        "terrainResyncs": sum(1 for f in frames if f.terrain & 0x10) if v3 else float("nan"),
        "assetWaits": sum(f.asset_waits for f in frames) if v3 else float("nan"),
        "scenery": {name: sum(1 for f in pictures if f.scenery == i) for i, name in enumerate(SCENERY) if i},
        "sliderUp": sum(1 for f in pictures if f.slider > 2),
        "stereo": sum(1 for f in pictures if f.stereo),
        "flat": {name: sum(1 for f in pictures if f.flat >> i & 1) for i, name in enumerate(FLATS)},
        "routes": {},
        # Version 4: per route, the pictures the GPU started early on; by the
        # point it started at; pictures that went without each surface.
        "earlyRoutes": {},
        "earlyAt": {name: sum(1 for f in pictures if f.gpu_early == i) for i, name in enumerate(EARLY)},
        "denied": {name: sum(1 for f in pictures if f.denied >> i & 1) for i, name in enumerate(DENIED)},
        "v4": any(f.gpu_early is not None for f in frames),
        "kinds": {},
        "flags": {name: sum(1 for f in frames if f.flags >> i & 1) for i, name in enumerate(FLAGS)},
    }
    for f in pictures:
        name = ROUTES[f.route] if f.route < len(ROUTES) else str(f.route)
        out["routes"][name] = out["routes"].get(name, 0) + 1
        if f.gpu_early:
            out["earlyRoutes"][name] = out["earlyRoutes"].get(name, 0) + 1
    for f in frames:
        name = KINDS[f.kind & 0x0F] if (f.kind & 0x0F) < len(KINDS) else str(f.kind & 0x0F)
        out["kinds"][name] = out["kinds"].get(name, 0) + 1
    for name in TIMINGS:
        i = TIMINGS.index(name)
        over = gpu if name in GPU else pictures if name in PICTURE else frames
        out[name] = mean([f.t[i] for f in over])
    for i, name in enumerate(COUNTS):
        over = pictures if name in PER_PICTURE_COUNTS else frames
        out[name] = mean([f.n[i] for f in over]) if name in PER_PICTURE_COUNTS else sum(f.n[i] for f in over)
    # Version 5: over the pictures that updated the voxel world.
    voxel = [f for f in pictures if f.v is not None and f.flags >> FLAGS.index("voxel") & 1]
    out["v5"] = any(f.v is not None for f in frames)
    out["voxelPictures"] = len(voxel)
    for i, name in enumerate(VOXEL):
        out[name] = mean([f.v[i] for f in voxel]) if out["v5"] else nan
    # Version 6: over the battle's pictures; shown twice over all the pictures;
    # the drains over the submissions.
    v6 = any(f.b is not None for f in frames)
    battle = [f for f in pictures if f.battle & BATTLE_IN]
    out["v6"] = v6
    out["battleFrames"] = sum(1 for f in frames if f.battle & BATTLE_IN)
    out["doubleFrames"] = sum(1 for f in frames if f.battle & BATTLE_DOUBLE)
    out["animFrames"] = sum(1 for f in frames if f.anim_kind)
    out["battlePictures"] = len(battle)
    out["shownTwice"] = sum(1 for f in pictures if f.twice) if v6 else nan
    out["drains"] = mean([f.drains for f in gpu]) if v6 else nan
    out["drainsLost"] = sum(f.drains_lost for f in frames) if v6 else nan
    out["drainsPast"] = sum(max(0, f.drains - DRAIN_LIST) for f in frames) if v6 else nan
    for i, name in enumerate(BATTLE):
        out[name] = mean([f.b[i] for f in battle]) if v6 and battle else nan
    for name, field in (("tiles", "tiles"), ("vertices", "vertices"), ("objects", "objects"),
                        ("switches", "switches")):
        out[name] = mean([getattr(f, field) for f in battle]) if v6 and battle else nan
    out["sceneWhy"] = {n: sum(1 for f in battle if f.scene_why >> i & 1) for i, n in enumerate(SCENE_WHY)}
    out["textWhy"] = {n: sum(1 for f in battle if f.text_why >> i & 1) for i, n in enumerate(TEXT_WHY)}
    out["slotsAgain"] = {n: sum(1 for f in battle if f.slots_again >> i & 1) for i, n in enumerate(SLOTS_AGAIN)}
    out["slotsUnkept"] = {n: sum(1 for f in battle if f.slots_unkept >> i & 1) for i, n in enumerate(SLOTS_UNKEPT)}
    return out


def bits_text(counts):
    return ", ".join(f"{k} {v}" for k, v in counts.items() if v) or "none"


def describe(header):
    models = ["Old 3DS", "Old 3DS XL", "New 3DS", "Old 2DS", "New 3DS XL", "New 2DS XL"]
    model = models[header["model"]] if header["model"] < len(models) else f"model {header['model']}"
    dirty = f", {header['dirty']} files changed (diff {header['diff']})" if header["dirty"] else ""
    return (f"{header['version']} {header['commit'][:12]}{dirty}, code {header['code']:016x}, {model}, "
            f"perf.bin v{header['format']}")


def mode_name(mode):
    return MODES[mode] if mode < len(MODES) else str(mode)


def print_summary(path, warmup, measure):
    header, frames = read_run(path)
    print(f"{path}: {describe(header)}, {len(frames)} frames")
    for mode, fs in sorted(groups(frames, warmup, measure).items()):
        if not fs:
            continue
        s = stats(fs, header["format"] >= 3)
        mark = "" if s["ready99"] != s["ready99"] else (
            f"  [p99 {'<' if s['ready99'] < TARGET_P99_MS else '>='} {TARGET_P99_MS:.0f}]")
        print(f"\n[{mode_name(mode)}] {s['frames']} frames, {s['images']} pictures"
              f"  ready p50 {s['ready50']:.2f}  p95 {s['ready95']:.2f}  p99 {s['ready99']:.2f}"
              f"  max {s['readyMax']:.2f} ms{mark}"
              f"  (fixed, without the elastic voxel after-builds p95 {s['fixed95']:.2f}  p99 {s['fixed99']:.2f})"
              f"  | gpuEnd p95 {s['gpuEnd95']:.2f}  p99 {s['gpuEnd99']:.2f}  max {s['gpuEndMax']:.2f}"
              f"  | lost refreshes {s['missed']} ({s['missedFrames']} pictures; cpu {s['missedCpu']},"
              f" gpu {s['missedGpu']})")
        print("  frames: " + ", ".join(f"{k} {v}" for k, v in sorted(s["kinds"].items(), key=lambda kv: -kv[1]))
              + (f"; boundaries {s['boundaries']}" if s["boundaries"] else "")
              + "".join(f"; {k} {v}" for k, v in s["flags"].items() if v))
        print("  means: " + "  ".join(f"{n}={s[n]:.2f}" for n in SHOWN if s[n] >= 0.005)
              + f"  (profiler {s['profiler']:.3f})")
        if header["format"] >= 2:
            print(f"  per picture: draws {s['draws']:.1f}  targets {s['targets']:.2f}  copies {s['copies']:.2f}"
                  f";  in all: game-thread reads {s['syncReads']}, chunks missing {s['chunksMissing']}"
                  f" (picture-frames), models skipped {s['modelsSkipped']},"
                  f" sprites left out {s['spritesLeftOut']}, chunk builds {s['chunkBuilds']}")
        if header["format"] >= 3:
            print(f"  v3: gpuDone (queue drained) p50 {s['gpuDone50']:.2f}  p95 {s['gpuDone95']:.2f}"
                  f"  p99 {s['gpuDone99']:.2f}  max {s['gpuDoneMax']:.2f}"
                  f" ({s['gpuDoneSeen']} of {s['gpuSubmissions']} submissions);"
                  f" refreshes repeated (swaps' VBlanks) {s['repeated']} in {s['repeatedFrames']} pictures"
                  f" ({s['shownUnknown']} not known); effects left out {s['absent']};"
                  f" terrain left undrawn in {s['terrainSkipped']} frames, drawn again {s['terrainResyncs']};"
                  f" asset waits {s['assetWaits']}; battle without 3D: "
                  + ", ".join(f"{k} {v}" for k, v in s["scenery"].items() if v))
        print(f"  3D: slider up {s['sliderUp']}, shown {s['stereo']}; routes "
              + ", ".join(f"{k} {v}" for k, v in sorted(s["routes"].items(), key=lambda kv: -kv[1]))
              + "; " + ", ".join(f"{k} {v}" for k, v in s["flat"].items() if v))
        if s["v4"]:
            print("  v4: gpu early (early/pictures) "
                  + ", ".join(f"{k} {s['earlyRoutes'].get(k, 0)}/{v}"
                              for k, v in sorted(s["routes"].items(), key=lambda kv: -kv[1]))
                  + "; at " + ", ".join(f"{k} {v}" for k, v in s["earlyAt"].items() if v)
                  + "; surfaces denied " + (", ".join(f"{k} {v}" for k, v in s["denied"].items() if v) or "none"))
        if s["v5"] and s["voxelPictures"]:
            print(f"  v5: voxel cpu over {s['voxelPictures']} voxel pictures: draw {s['vDraw']:.2f}"
                  f" (chunks {s['vChunks']:.2f}  cards {s['vCards']:.2f}  air {s['vAir']:.2f}"
                  f"  weather {s['vWeather']:.2f});  billboards {s['vBillboards']:.2f}"
                  f" (gather {s['vGather']:.2f}  pack {s['vPack']:.2f})")
        if s["v6"]:
            print(f"  v6: pictures never shown (two swaps in one VBlank) {s['shownTwice']}; queue drains"
                  f" {s['drains']:.1f} per submission, past the list {s['drainsPast']}, lost {s['drainsLost']}")
        if s["v6"] and s["battlePictures"]:
            print(f"  v6 battle: {s['battleFrames']} frames, double {s['doubleFrames']}, animation {s['animFrames']};"
                  f" over {s['battlePictures']} pictures: tiles {s['tiles']:.0f}  vertices {s['vertices']:.0f}"
                  f"  objects {s['objects']:.1f}  switches {s['switches']:.1f}  "
                  + "  ".join(f"{n}={s[n]:.2f}" for n in BATTLE if s[n] >= 0.005))
            print(f"  v6 scenery: {bits_text(s['sceneWhy'])}; text: {bits_text(s['textWhy'])};"
                  f" slots again: {bits_text(s['slotsAgain'])}; not keepable: {bits_text(s['slotsUnkept'])}")


ROWS = ["frames", "images", "ready50", "ready95", "ready99", "readyMax", "fixed95", "fixed99", "gpuEnd95", "gpuEnd99",
        "gpuDone50", "gpuDone95", "gpuDone99", "missed", "missedCpu", "missedGpu", "repeated", "shownUnknown",
        "absent", "assetWaits", "terrainSkipped", "terrainResyncs", "draws", "targets", "copies", "syncReads",
        "chunksMissing", "modelsSkipped", "spritesLeftOut", "chunkBuilds"] + SHOWN + VOXEL + [
        "shownTwice", "drains", "drainsLost", "drainsPast", "tiles", "vertices", "objects", "switches"] + BATTLE


def print_compare(a, b, warmup):
    ha, fa = read_run(a)
    hb, fb = read_run(b)
    print(f"A {a}: {describe(ha)}\nB {b}: {describe(hb)}")
    ga, gb = groups(fa, warmup), groups(fb, warmup)
    for mode in sorted(set(ga) | set(gb)):
        label = mode_name(mode)
        if not ga.get(mode) or not gb.get(mode):
            print(f"\n[{label}] only in {'B' if gb.get(mode) else 'A'} "
                  f"({len(ga.get(mode) or gb.get(mode))} frames)")
            continue
        sa = stats(ga[mode], ha["format"] >= 3)
        sb = stats(gb[mode], hb["format"] >= 3)
        print(f"\n[{label}]{'':{max(0, 16 - len(label))}}{'A':>10}{'B':>10}{'B-A':>10}")
        for row in ROWS:
            va, vb = sa[row], sb[row]
            if va != va and vb != vb:
                continue  # (a v3 row, neither run has it)
            if va != va or vb != vb:
                # One side predates the row: shown, not compared.
                fa_ = f"{'-':>10}" if va != va else f"{va:10.2f}"
                fb_ = f"{'-':>10}" if vb != vb else f"{vb:10.2f}"
                print(f"  {row:16}{fa_}{fb_}")
                continue
            if abs(va) < 0.005 and abs(vb) < 0.005:
                continue
            print(f"  {row:16}{va:10.2f}{vb:10.2f}{vb - va:+10.2f}")


def print_ab(runs_a, runs_b, warmup, measure, settle):
    """Alternating repetitions of two builds: per mode, the median of each
    figure over the repetitions, its spread (min..max) and B - A of medians.
    A difference inside both spreads is not one."""
    sides = {}
    for side, runs in (("A", runs_a), ("B", runs_b)):
        per_mode = {}
        for path in runs:
            header, frames = read_run(path)
            print(f"{side} {path}: {describe(header)}")
            for mode, fs in groups(frames, warmup, measure, settle).items():
                if fs:
                    per_mode.setdefault(mode, []).append(stats(fs, header["format"] >= 3))
        sides[side] = per_mode
    for mode in sorted(set(sides["A"]) | set(sides["B"])):
        la, lb = sides["A"].get(mode, []), sides["B"].get(mode, [])
        label = mode_name(mode)
        if not la or not lb:
            print(f"\n[{label}] only in {'B' if lb else 'A'}")
            continue
        print(f"\n[{label}] repetitions A {len(la)}, B {len(lb)}")
        print(f"  {'':16}{'A median':>10}{'A range':>16}{'B median':>10}{'B range':>16}{'B-A':>9}")
        for row in ROWS:
            va = [s[row] for s in la if s[row] == s[row]]
            vb = [s[row] for s in lb if s[row] == s[row]]
            if not va or not vb:
                # A figure only one side's runs have (the other's predate it:
                # NaN there): shown, not compared.
                if (va or vb) and max(map(abs, va + vb)) >= 0.005:
                    print(f"  {row:16}" + "".join(
                        f"{statistics.median(v):10.2f}{min(v):8.2f}..{max(v):<6.2f}" if v else f"{'-':>10}{'':16}"
                        for v in (va, vb)))
                continue
            ma, mb = statistics.median(va), statistics.median(vb)
            if abs(ma) < 0.005 and abs(mb) < 0.005 and max(map(abs, va + vb)) < 0.005:
                continue
            overlap = min(va) <= max(vb) and min(vb) <= max(va)
            print(f"  {row:16}{ma:10.2f}{min(va):8.2f}..{max(va):<6.2f}{mb:10.2f}{min(vb):8.2f}..{max(vb):<6.2f}"
                  f"{mb - ma:+9.2f}{'' if overlap else '  *'}")
    print("\n* the two sides' ranges do not overlap")


def anim_name(fr):
    if not fr.anim_kind:
        return "-"
    kind = ANIM_KINDS[fr.anim_kind] if fr.anim_kind < len(ANIM_KINDS) else str(fr.anim_kind)
    return f"{kind} {fr.anim_id}"


def print_battle(path, least):
    """Version 6: a battle's pictures by route, single or double, animation
    and test build's toggles, with what each took and lost. rep is refreshes
    repeated, twice the pictures that never reached the screen, lost the
    refreshes lost; gpuDone p50/p95 over the submissions whose swap was seen;
    last the median time of the last drain at each stage, over its pictures."""
    header, frames = read_run(path)
    print(f"{path}: {describe(header)}")
    if header["format"] < 6:
        sys.exit("battle: a perf.bin v6 is needed")
    by = {}
    for fr in frames:
        if fr.battle & BATTLE_IN and fr.picture:
            by.setdefault((ROUTES[fr.route] if fr.route < len(ROUTES) else str(fr.route),
                           "double" if fr.battle & BATTLE_DOUBLE else "single", anim_name(fr), fr.toggles),
                          []).append(fr)
    draw = TIMINGS.index("draw")
    print(f"{'route':20}{'kind':8}{'animation':14}{'toggles':>10}{'pics':>7}{'rep':>5}{'twice':>6}{'lost':>5}"
          f"{'draw':>7}{'gpuDone':>14}{'bg':>6}{'tiles':>6}{'drains':>7}")
    for key, fs in sorted(by.items(), key=lambda kv: (kv[0][0], kv[0][1], -len(kv[1]))):
        if len(fs) < least:
            continue
        rep = sum(f.shown for f in fs if f.shown != 255)
        done = [f.gpu_done for f in fs if f.gpu_done]
        print(f"{key[0]:20}{key[1]:8}{key[2]:14}{key[3]:>10}{len(fs):>7}{rep:>5}{sum(1 for f in fs if f.twice):>6}"
              f"{sum(f.lost for f in fs):>5}{mean([f.t[draw] for f in fs]):>7.2f}"
              f"{percentile(done, 0.5):>7.2f}{percentile(done, 0.95):>7.2f}"
              f"{mean([f.b[0] for f in fs]):>6.2f}{mean([f.tiles for f in fs]):>6.0f}"
              f"{mean([f.drains for f in fs]):>7.1f}")
        stage = {}
        for f in fs:
            last = {}
            for at, tag in f.drain_list:
                last[tag & ~STAGE_SWAP] = at
            for tag, at in last.items():
                stage.setdefault(tag, []).append(at)
        print(f"{'':20}last drain: " + "  ".join(
            f"{STAGES[tag] if tag < len(STAGES) else tag} {percentile(ats, 0.5):.2f} ({len(ats)})"
            for tag, ats in sorted(stage.items()))
            + f"; scenery: {bits_text({n: sum(1 for f in fs if f.scene_why >> i & 1) for i, n in enumerate(SCENE_WHY)})}"
            + f"; text: {bits_text({n: sum(1 for f in fs if f.text_why >> i & 1) for i, n in enumerate(TEXT_WHY)})}")


def write_csv(path, out):
    _, frames = read_run(path)
    with open(out, "w", encoding="utf-8") as f:
        f.write("tick,scene,image,gpuOf,kind,mode,route,slider,stereo,flat,lost,flags,"
                + ",".join(TIMINGS) + "," + ",".join(COUNTS)
                + ",gpuEnd,gpuDone,shown,scenery,absent,terrain,assetWaits,assetFails,gpuEarly,denied,"
                + ",".join(VOXEL)
                + ",shownTwice,toggles,animKind,animId,battle,sprites,switches,sceneWhy,textWhy,slotsAgain,slotsUnkept,"
                + "tiles,vertices,objects,drains,drainsLost,drainList," + ",".join(BATTLE) + "\n")
        for fr in frames:
            kind = KINDS[fr.kind & 0x0F] if (fr.kind & 0x0F) < len(KINDS) else str(fr.kind & 0x0F)
            f.write(f"{fr.tick},{fr.scene:08x},{fr.image},{fr.gpu_of},{kind}{'+b' if fr.kind & KIND_BOUNDARY else ''},"
                    f"{mode_name(fr.mode)},"
                    f"{ROUTES[fr.route] if fr.route < len(ROUTES) else fr.route},{fr.slider},{fr.stereo},"
                    f"{fr.flat},{fr.lost},{fr.flags}," + ",".join(f"{t:.2f}" for t in fr.t) + ","
                    + ",".join(str(n) for n in fr.n)
                    + f",{'' if fr.gpu_end is None else f'{fr.gpu_end:.2f}'}"
                    + f",{f'{fr.gpu_done:.2f}' if fr.gpu_done else ''},{'' if fr.shown == 255 else fr.shown}"
                    + f",{fr.scenery},{fr.absent},{fr.terrain},{fr.asset_waits},{fr.asset_fails}"
                    + (f",{EARLY[fr.gpu_early] if fr.gpu_early < len(EARLY) else fr.gpu_early},{fr.denied}"
                       if fr.gpu_early is not None else ",,")
                    + "," + (",".join(f"{t:.2f}" for t in fr.v) if fr.v is not None else "," * (len(VOXEL) - 1))
                    + f",{int(fr.twice)},{fr.toggles},{ANIM_KINDS[fr.anim_kind] if fr.anim_kind < len(ANIM_KINDS) else fr.anim_kind}"
                    + f",{fr.anim_id},{fr.battle},{fr.sprites},{fr.switches},{fr.scene_why},{fr.text_why}"
                    + f",{fr.slots_again},{fr.slots_unkept},{fr.tiles},{fr.vertices},{fr.objects},{fr.drains}"
                    + f",{fr.drains_lost}," + " ".join(
                        f"{STAGES[s & 0x7F] if (s & 0x7F) < len(STAGES) else s & 0x7F}{'+swap' if s & STAGE_SWAP else ''}:{t:.2f}"
                        for t, s in fr.drain_list)
                    + "," + (",".join(f"{t:.2f}" for t in fr.b) if fr.b is not None else "," * (len(BATTLE) - 1))
                    + "\n")


def elf_text(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        sys.exit(f"{path}: not a 32-bit ELF")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<3H", data, 0x2E)
    sections = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    names = sections[shstrndx]
    for s in sections:
        name = data[names[4] + s[0]:data.index(b"\0", names[4] + s[0])].decode()
        if name == ".text":
            return data[s[4]:s[4] + s[5]]
    sys.exit(f"{path}: no .text")


def code_hash(text):
    h = 0xCBF29CE484222325
    for (word,) in struct.iter_unpack("<I", text[:len(text) // 4 * 4]):
        h = ((h ^ word) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def identity(path, elf):
    header, frames = read_run(path)
    print(f"{path}: {describe(header)}")
    # The link id, when the run has one: the code hash only matches the ELF
    # where the loader lays it out as linked (Azahar, not the console).
    if header["linkid"]:
        with open(elf, "rb") as f:
            same = header["linkid"].encode() in f.read()
        print(f"link id {header['linkid']}: {'same executable' if same else 'DIFFERENT executable'}")
        return 0 if same else 1
    h = code_hash(elf_text(elf))
    print(f"{elf}: code {h:016x}")
    print("same executable" if h == header["code"] else "DIFFERENT executable")
    return 0 if h == header["code"] else 1


def nm_symbols(elf):
    nm = os.environ.get("NM", "arm-none-eabi-nm")
    out = subprocess.run([nm, "-n", elf], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def sites(log, elf):
    addrs, names = nm_symbols(elf)

    def name(address):
        i = bisect.bisect_right(addrs, address) - 1
        return f"{names[i]}+{address - addrs[i]:#x}" if i >= 0 else "?"

    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "GPUMEM" not in line:
                continue
            line = line.rstrip("\n")
            if "GPUMEM sites" in line or "GPUMEM fail" in line:
                out = []
                for word in line.split():
                    if ":" in word and "=" in word and word[0] in "vl" and not word.startswith("live"):
                        pool, rest = word.split(":", 1)
                        address, value = rest.split("=", 1)
                        out.append(f"{'vram  ' if pool == 'v' else 'linear'} {value:>14}  {name(int(address, 16))}")
                    elif word.startswith("site="):
                        out.append(f"site {name(int(word[5:], 16))}")
                    elif out and word.startswith("x") and word[1:].isdigit() and "fail" not in line:
                        out[-1] = f"{out[-1]} ({word})"
                    else:
                        out.append(word)
                if "fail" in line:
                    print(" ".join(out))
                else:
                    print(out[0] + " " + out[1] + "\n    " + "\n    ".join(out[2:]))
            else:
                print(line)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command", required=True)
    s = sub.add_parser("summary"); s.add_argument("run"); s.add_argument("--warmup", type=int, default=60)
    s.add_argument("--measure", type=int, default=None)
    c = sub.add_parser("compare"); c.add_argument("a"); c.add_argument("b"); c.add_argument("--warmup", type=int, default=60)
    r = sub.add_parser("ab"); r.add_argument("--a", nargs="+", required=True); r.add_argument("--b", nargs="+", required=True)
    r.add_argument("--settle", type=int, default=600, help="frames dropped at the start of every run")
    r.add_argument("--warmup", type=int, default=60, help="frames dropped at the start of every stretch")
    r.add_argument("--measure", type=int, default=6000, help="frames measured per mode and run")
    b = sub.add_parser("battle"); b.add_argument("run"); b.add_argument("--min", type=int, default=8)
    v = sub.add_parser("csv"); v.add_argument("run"); v.add_argument("out")
    i = sub.add_parser("identity"); i.add_argument("run"); i.add_argument("--elf", required=True)
    g = sub.add_parser("sites"); g.add_argument("log"); g.add_argument("--elf", required=True)
    a = p.parse_args()
    if a.command == "summary":
        print_summary(a.run, a.warmup, a.measure)
    elif a.command == "compare":
        print_compare(a.a, a.b, a.warmup)
    elif a.command == "ab":
        print_ab(a.a, a.b, a.warmup, a.measure, a.settle)
    elif a.command == "battle":
        print_battle(a.run, a.min)
    elif a.command == "csv":
        write_csv(a.run, a.out)
    elif a.command == "identity":
        return identity(a.run, a.elf)
    elif a.command == "sites":
        sites(a.log, a.elf)
    return 0


if __name__ == "__main__":
    sys.exit(main())
