# Follower Pokémon

OPTIONS → ENHANCEMENTS → PKMN FOLLOWER enables the first non-egg Pokémon with HP
in the party. The name is the same in every language. It is off by
default, stored as `follower=0|1` in `settings.txt`, and independent of VISIBLE
WILD. No changes to the save format or new graphical resources are required.

The follower uses the same 32×32 two-frame party icon as visible wild Pokémon.
These are bobbing icons, not directional walking sprites; shiny palettes and
interactions are outside this version's scope. A map-anchored sprite is drawn
by the existing 2D renderer or as a card by the voxel field-effects renderer.

## Behaviour

- Follows the walked path at a distance of 16 pixels, including corners,
  walking, running and fast-forward. Stopping keeps that distance.
- Bobs faster when moving faster. Faces right with a horizontal flip when
  moving right, left when moving left, and retains facing when moving
  vertically or standing still. Both renderers read the same OAM flip bit.
- Does not occupy an object-event slot, block the player, or trigger battles.
- Hides when reversing into the follower or an NPC occupies its tile.
- Appears immediately behind the player's facing direction on activation
  and after returning to the field. If that tile is blocked, tries the sides
  and front, excluding NPCs, water and incompatible elevations. If all are
  blocked, waits until a safe tile or walked trail becomes available.
- Retracts during scripts, locked field controls, jumping, bike riding, Surf
  and Dive, and returns immediately when ordinary field controls resume.
- Changes to the first eligible Pokémon when the party order, health or
  membership changes. Hides when none is eligible.
- If there is no free sprite or palette, skips spawning and retries; never
  substitutes another palette. Wild and follower icons share palette lifetime.
- In voxel, its visible feet meet the ground: the standing icon's transparent
  bottom rows are measured once outside rendering. Its map position is
  independent of the player sprite's size. Outdoors, it casts the same sun
  shadow as characters, using the same cropped picture, light and terrain.
  Indoors and with voxel lighting disabled, shadows follow the character rules.

## Implementation

`3ds_follower_trail.c` keeps a fixed 17-point pixel trail. Cardinal movement
adds each intermediate pixel, so speed changes do not change the distance.
Discontinuous coordinates reset the trail rather than drawing a line through
the map. `3ds_follower.c` translates the player sprite's interpolated position
to map coordinates and manages the follower lifecycle.

`3ds_field_icon.c` shares icon creation, streamed animation and palette release
with visible wild Pokémon. `0041-pokemon-follower.patch` adds one overworld
hook and routes wild icons through that helper.

## Validation

`make verify-follower` checks path corners, running, stopping, reversals,
warps, negative coordinates, camera scrolling, party changes, menu/script
states, and shared/exhausted palette and sprite slots. Geometry tests cover
cropped feet, raised terrain and cast shadows, with lighting on and off.

Hardware acceptance must cover both 2D and voxel: enter and exit a building,
cross connected maps, walk/run at 1× and 4×, jump a ledge, ride a bike, Surf,
open menus, change the lead, fight a battle, toggle the option, and play with
VISIBLE WILD off and on. Check sprite colours and depth ordering alongside
NPCs and multiple wild Pokémon. Hardware acceptance is pending.
