"""Order-preserving depth clustering with indivisible anatomical parts.

No colour segmentation or semantic guesses: masks must be supplied by an author.
Optional collapse_group on parts explicitly joins pieces of the same surface.
"""
from collections import Counter
import math
import statistics


def cluster_depths(items, limit):
    """Globally optimal contiguous weighted least-squares partition (small input).

    items: (id, normalized depth, positive weight). Equal depths never split.
    Returns id -> plane, and cluster centres. Keeps existing depth order.
    """
    if not 1 <= limit <= 15:
        raise ValueError('invalid group limit')
    by_depth = {}
    for ident, depth, weight in items:
        if not math.isfinite(depth) or not 0 <= depth <= 1 or weight <= 0:
            raise ValueError('invalid depth or weight')
        by_depth.setdefault(depth, []).append((ident, weight))
    depths = sorted(by_depth)
    if not depths:
        return {}, []
    n, k = len(depths), min(limit, len(depths))
    weights = [sum(w for _, w in by_depth[d]) for d in depths]
    costs, centres = {}, {}
    for a in range(n):
        w = s = ss = 0.0
        for b in range(a, n):
            w += weights[b]
            s += weights[b] * depths[b]
            ss += weights[b] * depths[b] ** 2
            costs[a, b+1] = max(0.0, ss - s*s/w)
            centres[a, b+1] = s/w
    dp = {(0, 0): (0.0, [])}
    for g in range(1, k+1):
        for b in range(g, n+1):
            candidates = [(dp[g-1, a][0] + costs[a, b], dp[g-1, a][1] + [(a, b)])
                          for a in range(g-1, b) if (g-1, a) in dp]
            dp[g, b] = min(candidates, key=lambda c: c[0])
    mapping, result_centres = {}, []
    for plane, (a, b) in enumerate(dp[k, n][1]):
        result_centres.append(centres[a, b])
        for d in depths[a:b]:
            for ident, _ in by_depth[d]:
                mapping[ident] = plane
    return mapping, result_centres


def unpack(data):
    return [p for b in data for p in (b & 15, b >> 4)]


def pack(pixels):
    return bytes(pixels[i] | (pixels[i+1] << 4) for i in range(0, len(pixels), 2))


def clean_fragments(pixels, max_area=8):
    """Merge small 8-connected depth islands into touching, larger surfaces.

    Only unannotated drafts use this. A merge needs a shared pixel edge,
    a region larger than max_area, and at most one depth-group step. This
    preserves detached details and avoids assigning near tips to far planes.
    Decisions use the original regions, not earlier changes in traversal order.
    """
    if len(pixels) % 4096:
        raise ValueError('cleanup requires whole 64x64 frames')
    result = list(pixels)
    changes = []
    for frame in range(len(pixels)//4096):
        values = pixels[frame*4096:(frame+1)*4096]
        labels = [-1]*4096
        regions = []
        for start, depth in enumerate(values):
            if depth == 15 or labels[start] != -1:
                continue
            ident = len(regions)
            labels[start] = ident
            region, pending = [], [start]
            while pending:
                i = pending.pop()
                region.append(i)
                x, y = i % 64, i // 64
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x+dx, y+dy
                        if not 0 <= xx < 64 or not 0 <= yy < 64:
                            continue
                        j = yy*64+xx
                        if labels[j] == -1 and values[j] == depth:
                            labels[j] = ident
                            pending.append(j)
            regions.append(region)
        for region in regions:
            if len(region) > max_area:
                continue
            depth = values[region[0]]
            neighbours = Counter()
            for i in region:
                x, y = i % 64, i // 64
                for xx, yy in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                    if not 0 <= xx < 64 or not 0 <= yy < 64:
                        continue
                    j = yy*64+xx
                    label = labels[j]
                    if label >= 0 and len(regions[label]) > max_area and abs(values[j]-depth) == 1:
                        neighbours[label] += 1
            if not neighbours:
                continue
            into = min(neighbours, key=lambda label: (-neighbours[label], -len(regions[label]), label))
            target = values[regions[into][0]]
            for i in region:
                result[frame*4096+i] = target
            changes.append({'frame': frame, 'from_group': depth, 'to_group': target,
                            'pixels': [[i%64,i//64] for i in sorted(region)]})
    return result, changes


def smart_pixels(data, source, target, anatomy=None, cleanup=False):
    if not 2 <= target <= source <= 15 or len(data) % 2048:
        raise ValueError('invalid plane counts or frame size')
    pixels = unpack(data)
    if any(p != 15 and p >= source for p in pixels):
        raise ValueError('invalid source plane')
    counts = Counter(p for p in pixels if p != 15)
    report = {'source_levels': len(counts), 'semantic_cleanup': anatomy is not None}
    if anatomy is None:
        # A fallback only: clustering cannot discover anatomy or repair noisy masks.
        mapping, centres = cluster_depths([(p, p/(source-1), w) for p, w in counts.items()], target)
        out = [15 if p == 15 else mapping[p] for p in pixels]
        report.update(method='depth-only-needs-part-review', source_plane_to_group=mapping)
        if cleanup:
            out, changes = clean_fragments(out)
            report.update(method='automatic-spatial-draft', fragment_merges=changes,
                          cleaned_pixels=sum(len(change['pixels']) for change in changes),
                          review_required=True)
    else:
        if anatomy.get('version') != 1 or anatomy.get('size') != [64, 64]:
            raise ValueError('expected version-1 64x64 anatomy')
        parts = anatomy.get('parts', [])
        by_id = {}
        for part in parts:
            pid = part.get('id')
            if type(pid) is not int or pid <= 0 or pid in by_id or not part.get('name'):
                raise ValueError('invalid anatomical part')
            by_id[pid] = part
        frames = anatomy.get('frames', [])
        if len(frames)*4096 != len(pixels):
            raise ValueError('anatomy frame count differs from pack')
        labels = [None]*len(pixels)
        part_pixels = {pid: [] for pid in by_id}
        for fi, frame in enumerate(frames):
            rows = frame.get('rows', [])
            if len(rows) != 64:
                raise ValueError('expected 64 anatomy rows')
            for y, runs in enumerate(rows):
                for run in runs:
                    if len(run) != 3 or any(type(x) is not int for x in run):
                        raise ValueError('invalid anatomy run')
                    a, b, pid = run
                    if not 0 <= a < b <= 64 or pid not in by_id:
                        raise ValueError('invalid anatomy run bounds or part')
                    for x in range(a, b):
                        i = fi*4096 + y*64 + x
                        if labels[i] is not None or pixels[i] == 15:
                            raise ValueError('anatomy overlaps or covers transparency')
                        labels[i] = pid
                        part_pixels[pid].append(pixels[i])
        if any(p != 15 and label is None for p, label in zip(pixels, labels)):
            raise ValueError('anatomy does not cover every opaque pixel')
        depths, surfaces = {}, {}
        for pid, part in by_id.items():
            if not part_pixels[pid]:
                raise ValueError('unused anatomical part')
            depth = part.get('depth', statistics.median(part_pixels[pid])/(source-1))
            if type(depth) not in (int, float) or not math.isfinite(depth) or not 0 <= depth <= 1:
                raise ValueError('invalid anatomical depth')
            depths[pid] = depth
            group = part.get('collapse_group')
            if group is not None and (not isinstance(group, str) or not group.strip()):
                raise ValueError('collapse_group must be a nonempty surface name')
            key = ('surface', group) if group is not None else ('part', pid)
            surfaces.setdefault(key, []).append(pid)
        # One vote per anatomical part: a tiny claw is not drowned out by the torso.
        items = [(key, statistics.mean(depths[p] for p in members), len(members))
                 for key, members in surfaces.items()]
        mapping, centres = cluster_depths(items, target)
        part_groups = {p: mapping[key] for key, members in surfaces.items() for p in members}
        out = [15 if pid is None else part_groups[pid] for pid in labels]
        report.update(method='anatomical-parts', parts=[
            {'id': pid, 'name': part['name'], 'depth': depths[pid], 'group': part_groups[pid],
             'source_levels': len(set(part_pixels[pid]))}
            for pid, part in by_id.items()])
        # Explicit surface grouping can override old ordering. Make that reviewable.
        report['ordering_overrides'] = [
            [a, b] for a in by_id for b in by_id
            if depths[a] < depths[b] and part_groups[a] > part_groups[b]]
    report['groups_used'] = len(centres)
    report['cluster_centres'] = centres
    # Fill the available range when fewer than target groups suffice; put a single
    # coplanar surface near the existing 0.62 pivot instead of behind the screen.
    levels = ([int(0.62*(target-1)+0.5)] if len(centres) == 1 else
              [int(p*(target-1)/(len(centres)-1)+0.5) for p in range(len(centres))])
    report['output_levels'] = levels
    if anatomy is not None:
        for part in report['parts']:
            part['plane'] = levels[part['group']]
    output = [15 if p == 15 else levels[p] for p in out]
    report['occupied_output_levels'] = sorted(set(output) - {15})
    return pack(output), report
