# Optional Nuzlocke tracker

Open OPTIONS → ENHANCEMENTS, select NUZLOCKE on the second screen, then set
TRACKER to ON. It defaults to OFF (`nuzlocke=0` in `settings.txt`). This is a
manual notebook: it does not enforce catches, deaths, releases or level caps,
and never changes Pokémon or the game's save format.

## Recording a run

- **AREA** is the current map's region-map section. Maps sharing that section
  share one encounter entry; it is not a separate entry for each floor or room.
  Area and party actions require the overworld.
- **ENCOUNTER** shows that area's recorded status. Choose UNUSED, CAUGHT or
  MISSED with SET TO, then tap RECORD to apply it. UNUSED clears the entry.
  SET TO is a separate choice, not an automatic record of a catch.
- **PARTY POKEMON** selects a current party member. Tap FALLEN MARK twice on
  the same Pokémon to mark it fallen, or twice again to clear its mark.
  Manual marking also permits a healthy Pokémon; eggs cannot be marked.
- **MARK FAINTED** requires two taps and marks current non-egg party members
  with zero HP that have not already been marked. It does not inspect the PC
  or automatically mark Pokémon after a battle.
- Fallen marks use personality and original-trainer ID, so changing party order
  or depositing or evolving a Pokémon does not lose its mark. Shedinja has a
  separate identity flag because the game copies Ninjask's personality and
  trainer ID into it. Marking or clearing one sibling does not affect the other.
  There are at most 128 marks.
  A full list rejects new marks; a bulk mark can add only as many as fit.
  To clear a stored mark, select that Pokémon in the party.
- **NEXT LEVEL CAP** is a reference based on the number of badges: gym caps
  are 15, 19, 24, 29, 31, 33, 42 and 46; with eight badges it shows the
  Champion's level 58. It imposes no level restriction.
- **CAUGHT/MISS/FALLEN** shows the three totals in that order.

Sidecars now use format version 2. Version 1 records still load and upgrade on
next successful save, preserving encounter records and fallen marks on the
original Pokémon's lineage. A legacy mark cannot tell Ninjask from its copied
Shedinja: it remains on Nincada/Ninjask, and players who intended Shedinja should
clear or reapply that mark manually. Older builds cannot read version 2 records;
keep a backup before reverting builds.

OFF retains recorded progress and the current choice/party selection, but
blocks encounter and fallen edits. Most live tracker values show `--`; SET TO,
PARTY POKEMON and SAVE LINK remain visible. You can still change the choice
or selected party slot. Turning OFF does not erase records or stop an owned
record from following later successful saves.

## Saving and recovery

Edits remain in memory until a successful in-game save. SAVE LINK shows
SAVE TO KEEP while edits are pending, THIS SAVE after the record is written,
and WRITE FAILED if the auxiliary write fails. Retry an in-game save while
that session is still running; restarting discards unsaved tracker edits.

The tracker writes `SD:/3ds/emerald3ds/emerald3ds.nuzlocke`, separately from the
`.sav`, through a `.nuzlocke.tmp` file. On filesystems that cannot rename over
an existing file, it keeps the previous record as `.nuzlocke.bak` during
replacement. Keep the `.sav` and available tracker files together when backing
up or restoring a run.

Each record has its own integrity checks and must match the CRC and size of
the loaded game's two save-slot regions. Loading checks the main record,
then `.tmp`, then `.bak`, and applies the first valid match. A backup is not
guaranteed to match a later save. No match starts an empty tracker: SAVE LINK
shows OTHER SAVE for a valid record belonging to another save image, DAMAGED
for invalid records without a valid mismatched record, or NEW when no record
is found. None of these cases modifies the game save.

An empty tracker loaded without a match leaves an unrelated or damaged record
alone on later saves. Recording an entry lets this session replace it on the
next successful save. A new game deliberately starts an empty tracker and
replaces the old run's record on its first successful save, including when
TRACKER is OFF. Once a record belongs to this session, every successful save
updates its binding, even if all entries are cleared or TRACKER is OFF.

If writing the tracker fails, the game save has already succeeded and remains
usable. The previous tracker record may no longer match it, so recovery of
that progress after restarting is not guaranteed. Scenario playback does not
write the player's tracker files.
