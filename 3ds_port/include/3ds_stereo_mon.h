#ifndef CTR_STEREO_MON_H
#define CTR_STEREO_MON_H

/*
 * The stereoscopic Pokémon planes: a battle's Pokémon and trainer pictures
 * as stacks of flat depth planes, drawn where their sprites stand with the
 * 3D slider's parallax.
 *
 * The planes come from the Sprite Stereo Lab (scripts/gen_stereo_mons.py)
 * as romfs:/stereo/mons.bin, a port asset: per Pokémon, for each of the two
 * frames of its animated front picture, one plane number per pixel. The
 * picture itself is the game's own, decoded at run time on the game side
 * (3ds_stereo_mon_game.c); the native side (3ds_stereo_mon.c) composes the
 * planes into one texture and draws them with Citro2D. The decoding of the
 * file and the composition are pure (3ds_stereo_mon_decode.c) and host-tested.
 *
 * SDK-free: game translation units include this header.
 */

#include <stdbool.h>
#include <stdint.h>

#define CTR_STEREO_MON_FILE "stereo/mons.bin"
/* The battle's other pictures (scripts/gen_stereo_backs.py): Pokémon backs
 * (0x8000 | national), trainers' fronts (0x4000 | trainer pic), the
 * players' backs (0xC000 | back pic), each entry with frames of its own. */
#define CTR_STEREO_BACKS_FILE "stereo/backs.bin"
#define CTR_STEREO_ID_BACK 0x8000
#define CTR_STEREO_ID_TRAINER 0x4000
#define CTR_STEREO_ID_PLAYER 0xC000
#define CTR_STEREO_MON_MAGIC "EM3DSTM1"
#define CTR_STEREO_MON_DIM 64
/* Frames the picture buffers hold (the players' backs have five). */
#define CTR_STEREO_MON_MAX_FRAMES 5
#define CTR_STEREO_MON_FILE_MAX_FRAMES 5
#define CTR_STEREO_MON_MAX_PLANES 15
/* The nibble of a transparent pixel. The released packs have none: every
 * pixel of a frame carries a plane (the opaque ones' filled outwards), and
 * where the picture is opaque comes from the game's own picture at run
 * time. The compositor still reads 15 as transparent in older files. */
#define CTR_STEREO_MON_NONE 15
/* Index entries a file may have, kept resident: the national dex, or the
 * backs file's Pokémon, trainers and players. */
#define CTR_STEREO_MON_MAX_COUNT 520
#define CTR_STEREO_MON_HEADER_BYTES 16
#define CTR_STEREO_MON_INDEX_BYTES 8

typedef struct
{
    uint16_t count;
    uint8_t planes, frames;
    uint16_t width, height;
    /* frames * width * height / 2: one entry's bytes. */
    uint32_t entryBytes;
} CtrStereoMonHeader;

/* ── Pure: the file and the composition (3ds_stereo_mon_decode.c) ─────── */

/* False for anything but a well-formed file of 64x64 planes. */
bool CtrStereoMon_ParseHeader(const uint8_t *file, uint32_t size, CtrStereoMonHeader *out);
/* Index entry `index`: its national dex number (or id) and where its planes
 * start. Only the header and the index need to be in `file` (its first
 * 16 + 8 * count bytes). */
bool CtrStereoMon_IndexEntry(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                             unsigned index, uint16_t *national, uint32_t *offset);
/* The entry's own number of frames (the index's reserved word, when set),
 * else the file's; and the bytes of planes that many frames take. */
unsigned CtrStereoMon_EntryFrames(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                                  unsigned index);
uint32_t CtrStereoMon_FramesBytes(const CtrStereoMonHeader *header, unsigned frames);
/* The same with the planes themselves, which lie entirely inside `file`. */
bool CtrStereoMon_Entry(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                        unsigned index, uint16_t *national, const uint8_t **planes);
/* The index of a national dex number's entry, or -1. */
int CtrStereoMon_Find(const uint8_t *file, uint32_t size, const CtrStereoMonHeader *header,
                      uint16_t national);
/* The plane of one pixel: 0 the farthest .. planes-1 the nearest, or
 * CTR_STEREO_MON_NONE. Frames the entry does not have repeat its last one. */
unsigned CtrStereoMon_Plane(const uint8_t *planes, const CtrStereoMonHeader *header,
                            unsigned frame, unsigned x, unsigned y);
/*
 * The file's planes merged into `groups` (2..planes): plane q belongs to
 * group q * groups / planes, so fewer groups mean fewer, thicker slices and
 * fewer tiles to keep.
 */
unsigned CtrStereoMon_Group(unsigned plane, unsigned planes, unsigned groups);
/*
 * One group of one frame as a 64x64 tile of a PICA texture `texWidth` texels
 * wide (Morton order, CtrVideo_Texel), at tile (tileX, tileY): the picture's
 * RGBA5551 pixels (rows top to bottom, 0 = transparent) where the group is,
 * transparent everywhere else - except a skirt of `skirt` pixels: pixels of
 * farther groups within that distance of the group's are on its tile too
 * (like the cardboard sides of the lab's planes), so that where the card
 * shifts away from the group behind it, the gap shows that group's own
 * colours rather than the scene through it. In a texture 64 texels wide a
 * tile is one contiguous run of 8 KiB.
 */
void CtrStereoMon_ComposeTile(const uint16_t *picture, const uint8_t *planes,
                              const CtrStereoMonHeader *header, unsigned frame, unsigned groups,
                              unsigned group, uint16_t *texture, unsigned texWidth,
                              unsigned tileX, unsigned tileY, unsigned skirt);

/* ── Native: the planes files (3ds_stereo_mon.c) ─────────────────────── */

/* Reads the planes file once. False when there is none: nothing to show. */
bool CtrStereoMon_Load(void);
unsigned CtrStereoMon_Count(void);
uint16_t CtrStereoMon_NationalAt(unsigned index);
bool CtrStereoMon_Has(uint16_t national);
/* The file's header once loaded (for CtrStereoMon_Plane and ComposeTile). */
const CtrStereoMonHeader *CtrStereoMon_Header(void);
/* One Pokémon's planes (header->entryBytes of them) into `out`, which holds
 * `size` bytes; false when the file has no such Pokémon or no room. */
bool CtrStereoMon_PlanesFor(uint16_t national, uint8_t *out, uint32_t size);
/* The same for the backs file (CTR_STEREO_BACKS_FILE), by id: its header,
 * whether it has the id, and the id's planes with their own number of
 * frames (`frames`, 1..CTR_STEREO_MON_FILE_MAX_FRAMES). */
const CtrStereoMonHeader *CtrStereoMon_BacksHeader(void);
bool CtrStereoMon_HasBack(uint16_t id);
bool CtrStereoMon_BackPlanesFor(uint16_t id, uint8_t *out, uint32_t size, unsigned *frames);
/* ── Native: the battle (3ds_stereo_battle.c) ─────────────────────────── */

/*
 * The battle's pictures drawn as stacks of planes instead of sprites: in
 * each of the four battler positions (B_POSITION_*: 0 and 2 the player's
 * side, 1 and 3 the opponent's) a Pokémon's front or back, a trainer's
 * front during the intro, or the player's back with its throw. The game
 * side names each position every frame of a battle (CtrStereoBattle_SetSlot):
 * the tile its sprite's picture is at in OBJ VRAM, the position's picture
 * buffer (the game's decoded front or back picture, or the trainer's front,
 * `bufferFrames` of 2048 bytes of 4bpp tiles), the ids of the planes the
 * buffer could be showing (the Pokémon's, the trainer's) with the first
 * frame of each one's own picture as the game's tables hold it (4bpp tiles,
 * 2048 bytes, or NULL: the planes files carry no outline, so these tell
 * which of the two the buffer holds), and on the player's side the raw sheet of the player's back (`backFrames` frames) with
 * its id; 0 for a slot with no sprite. The compositor finds the sprite in
 * OAM each frame, tells which picture and frame OBJ VRAM holds (so a
 * Substitute, a Transform or a ball's shrink fall back to the sprite),
 * composes the planes with the sprite's own palette as it stands in palette
 * RAM (its fades and tints come for free), leaves the sprite out of the
 * frame and draws the planes where it was.
 */
#define CTR_STEREO_SLOTS 4
#define CTR_STEREO_BATTLE_FRAME_BYTES 2048
#define CTR_STEREO_BUFFER_FRAMES 2
#define CTR_STEREO_BACK_FRAMES 5
void CtrStereoBattle_SetSlot(unsigned position, unsigned tileNum, const uint8_t *buffer, unsigned bufferFrames,
                             uint16_t monId, uint16_t trainerId, const uint8_t *monPicture,
                             const uint8_t *trainerPicture, uint16_t playerId, const uint8_t *backSheet,
                             unsigned backFrames);
void CtrStereoBattle_ClearBattlers(void);
/* At boot: the planes texture made while the linear heap still has a run
 * of 128 KiB for it, and the files' indexes read. */
void CtrStereoBattle_Reserve(void);
/* Whether the battle draws its Pokémon in 3D at all (OPTIONS' 3D BATTLE). */
bool CtrStereoBattle_Enabled(void);
/*
 * For the compositor (3ds_video.c), inside the Citro3D frame before anything
 * is drawn: finds this frame's Pokémon in OAM and composes their planes.
 * Sets the bit of each OAM entry drawn as planes in `skip` (128 bits, as
 * OAM is indexed); returns how many. `memory` is the CtrVideoMemory bound.
 */
unsigned CtrStereoBattle_Frame(const void *memory, bool battle, uint32_t skip[4]);
/* The sprite of OAM entry `oam` drawn as planes this frame: its GBA
 * position (the GBA's own wrapping) and its priority. */
bool CtrStereoBattle_Oam(unsigned oam, int *x, int *y, unsigned *priority);
/* Clears the bit of `oam` in what Frame found: drawn as a sprite after all. */
void CtrStereoBattle_Veto(unsigned oam, uint32_t skip[4]);
/*
 * Draws OAM entry `oam`'s planes with Citro2D into the current target and
 * view, the sprite's top-left corner at (x, y) in view units, `scale` view
 * units per sprite pixel. `eyeParallax` is the eye's slider (+ for the left
 * eye, - for the right, 0 flat) and `shiftScale` the view units per screen
 * pixel of displacement. `smooth` filters the planes bilinearly (for a draw
 * at a fractional scale; the texels are premultiplied, so the caller blends
 * ONE, ONE_MINUS_SRC_ALPHA), else nearest. The caller has set the blend
 * state it wants and forgets it afterwards.
 */
void CtrStereoBattle_Draw(unsigned oam, float x, float y, float scale, float eyeParallax, float shiftScale,
                          bool smooth);

/* ── Game side (3ds_stereo_mon_game.c) ────────────────────────────────── */

/* The wild Pokémon to meet instead of `species` when CHEATS' 3D WILD ONLY is
 * on: one that has planes (so the 3D battle can be tried anywhere); else
 * `species` itself. Called by CreateWildMon (patch 0057). */
uint16_t CtrStereoMonGame_WildSpecies(uint16_t species);

/* Once per frame, after input is scanned and before the game reads its
 * keys (next to CtrBottom_Frame): the battle's positions named. */
void CtrStereoMonGame_Frame(void);

#endif
