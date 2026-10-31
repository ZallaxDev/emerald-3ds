/*
 * The Fly sky scene: between the climb into the cloud and the descent out of
 * it, the player on their Pokemon soars across the open sky above the deck,
 * with the wild flying Pokemon of the region passing by (3ds_video.c draws
 * it, voxel_fly.c times it). This is what the scene needs of the game:
 * supplied by 3ds_fly_sky.c (a game translation unit, PORT_BRIDGE) with
 * follower_mon.c (the overworld sprites) and field_effect.c (the Pokemon
 * doing the Fly). SDK-free.
 *
 * Every sprite is GBA 4bpp: square frames of `width` pixels, each frame
 * `frameBytes` (= width * width / 2) of 8x8 tiles in row-major order (tile
 * (tx, ty) at (ty * width / 8 + tx) * 32 bytes, two pixels a byte, the low
 * nibble first), colour 0 clear, with one 16-colour BGR555 palette. Frames
 * stay valid until CtrFlySky_Release.
 */
#ifndef CTR_FLY_SKY_H
#define CTR_FLY_SKY_H

#include <stdbool.h>
#include <stdint.h>

/* Frames of a Pokemon's overworld sprite (the follower file's order). */
enum
{
    CTR_SKY_FRAME_DOWN = 0,  /* two frames each: 0/1 down, 2/3 up, 4/5 left, 6/7 right */
    CTR_SKY_FRAME_UP = 2,
    CTR_SKY_FRAME_LEFT = 4,
    CTR_SKY_FRAME_RIGHT = 6,
    CTR_SKY_FRAMES = 8
};

typedef struct
{
    const uint8_t *tiles;    /* `frames` frames back to back */
    const uint16_t *palette; /* 16 BGR555 colours (the shiny one already chosen) */
    uint16_t width;          /* 32 or 64 */
    uint16_t frames;
    uint32_t frameBytes;
} CtrSkySprite;

/* The ENHANCEMENTS option ENHANCED FLYING (settings.txt key "fly_enhanced":
 * 0 off, 1 clouds, 2 full; default full). Cinematic: the camera's flight
 * through the cloud at all (off: the GBA's Fly as it was). Enabled: the sky
 * scene inside it too; false and the flight stays inside the cloud. */
bool CtrFlySky_Cinematic(void);
bool CtrFlySky_Enabled(void);


/* A Pokemon's overworld sprite from the follower data: false without one
 * (no data pack, or the species has none). Up to eight may be held at once. */
bool CtrFlySky_MonSprite(uint16_t species, uint32_t personality, bool shiny, CtrSkySprite *out);

/* The Pokemon doing the Fly (as field_effect.c chose it from the party),
 * and where the rider sits on it: `seatX/seatY` the rider's centre from the
 * sprite's centre, for its LEFT frames (negate seatX for RIGHT). False when
 * no Fly is under way or it has no sprite (the bird flies). */
bool CtrFlySky_FlyerSprite(CtrSkySprite *out, int8_t *seatX, int8_t *seatY);

/* The player seated on a Pokemon (the riding/surfing pose): frames 0 facing
 * left, 1 facing right, 32 pixels. */
bool CtrFlySky_RiderSprite(CtrSkySprite *out);

/* The flying species native to the flight: those of Flying type in the wild
 * encounter tables of the map flown from and the map flown to, unique, at
 * most `max`; when neither has any, a few of Hoenn's common birds. Returns
 * the count. */
unsigned CtrFlySky_Natives(uint16_t *species, unsigned max);

/* Reads the scene's files ahead of time (the Pokemon doing the Fly, the
 * natives, the legendaries that might pass) while the field move's banner
 * shows, so the scene's start does not wait on the SD card. */
void CtrFlySky_Prefetch(void);
/* Frees what the sprite calls loaded (after the scene). */
void CtrFlySky_Release(void);


/* The sky's weather: that of the map flown from (as it is now) and of the
 * map flown to (its header's), each as one of these; the scene crosses
 * from the one into the other halfway. */
enum
{
    CTR_SKY_CLEAR,   /* sunny, none, shade */
    CTR_SKY_CLOUDY,  /* sunny with clouds, horizontal fog, Route 123's cycle */
    CTR_SKY_RAIN,    /* rain, Route 119's cycle */
    CTR_SKY_STORM,   /* thunderstorm, downpour, the abnormal weather */
    CTR_SKY_SAND,    /* the desert's sandstorm */
    CTR_SKY_ASH,     /* Route 113's volcanic ash */
    CTR_SKY_DROUGHT, /* Groudon's harsh sunlight */
    CTR_SKY_WEATHERS
};
void CtrFlySky_Weather(uint8_t *from, uint8_t *to);

/* On landing: the map flown to gets its region's weather in the overworld
 * (the one the sky showed) when its own is plain sunny - Emerald's towns
 * only rain once a trigger tile is walked onto. Called as the Fly-in
 * begins (3ds_game_full.c). */
void CtrFlySky_Arrive(void);
/* Once a frame on the ground: some seconds after landing the region's
 * weather eases back to the map's own (unless the map was left). */
void CtrFlySky_ArriveTick(void);




#endif
