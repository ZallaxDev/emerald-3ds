# Trainer Gazette — data and text prototype

## Implemented scope

`LrGazetteQuery` projects the single caller-owned `LrWorldState.chronicle`.
Life events and Snap append to that same500-entry ring. Gazette does not own
a second history or cache of heard flags. Week4 factions/world features must
extend this owner rather than invent another event store.

Records use stable uint32 event IDs, absolute game-minute timestamps, full
uint16 trainer IDs, explicit region and place, text IDs, outcomes, and heard.
Place0 currently means unknown town, not the trainer's region as a town.
Overwrite discards the oldest physical record without recycling its ID.
Old heard-mark requests return false rather than marking its replacement.
ID exhaustion rejects further events atomically; simulation time still advances.

The query returns at most five unheard events, age at most7200 minutes,
excluding future timestamps. Filters support region, place and participant
trainer ID. Region/place255 and trainer NONE are wildcards. A query is read-only
and consumes no RNG. Explicit read acknowledgement calls `LrChronicleMarkHeard`.
Output arrays must hold min(maxCount,5) records and must not overlap world data.

## Selection and grouping

Score = kindWeight * (6 - floor(ageMinutes/1440)) + fameBonus.
Weights: death100, badge60, retirement50, clash30, life10. Fame bonus is the
largest known participant fameLevel/16 (0..15). Unknown people contribute0.
Ties use newest timestamp then lowest stable event ID. Select the top five
first, then group that selection by ascending place, preserving rank inside
each town. Thus grouping cannot promote an unimportant town's news over a
more important event elsewhere. Match Call can use the same participant filter.

## Text contract

`LrGazetteFormat` takes external title/trainer/place lookup callbacks and a
caller-owned buffer. Templates support only literal `{trainer}` (first person)
and `{place}`. Inserted names are not parsed recursively. Percent signs are
ordinary text, not printf directives. Unknown tokens remain literal.
Missing callbacks/text resolve to `Unknown trainer`, `Unknown place`, or
`An event occurred.`. Truncation returns false and terminates any nonempty
buffer; capacity0 writes nothing. No allocation, rendering, or platform string
encoding is implemented. Callbacks return valid NUL-terminated strings and
the destination must not overlap source templates/names.

## What the host demonstration proves

`test_gazette` simulates ten distinct profiles over30 daily advances, restores
the day15 snapshot, and reproduces the day30 state. It prints a bounded sample
using representative text and deliberately excludes permanent-retirement row12
for that demonstration, leaving the remaining eleven fixtures unchanged.
A separate two-profile six-month offline jump demonstrates bounded work and
repeatability. Printed captures/evolutions are pending intentions, not generated
or validated Pokémon. Full content, party generation and faction business are
Week4 work; qualitative long-term balance still needs larger simulations.

## Future integration boundaries

Core event kind numbers, uint32 event IDs, and uint16 people do NOT match the
existing port's GazetteEntry encoding. Phase3 needs an explicit adapter and
an output-buffer transport, not direct struct casts. Rendering belongs to
the later3DS UI. No specials stubs or extension-layer API changed here.

Real world accomplishments, real battle strength, and encounter accessibility
remain separate. Future story scenes acknowledge strength gaps and may continue
after a mandatory rival defeat. Gazette exposes real developments; it never
weakens a team or applies player-level rubber-banding. Pending team intentions
must become real parties in Week4 before battle integration can claim this.
