#ifndef GUARD_PORT_PROF_H
#define GUARD_PORT_PROF_H

/*
 * Frame profiler: where a frame's time goes, by section. Each section adds
 * the ticks between its begin and end to this frame's total; sections nest,
 * so the figures are inclusive (the I/O inside a decompression counts in
 * both). The platform reports the frame and clears it at present.
 * Reachable from game translation units: no SDK header here.
 */

#include <stdint.h>

enum
{
    PORT_PROF_TASKS,      /* RunTasks */
    PORT_PROF_SPRITES,    /* AnimateSprites + BuildOamBuffer */
    PORT_PROF_FADE,       /* UpdatePaletteFade */
    PORT_PROF_VBLANK,     /* the game's VBlank handler */
    PORT_PROF_DMA3,       /* ProcessDma3Requests */
    PORT_PROF_MIX,        /* m4aSoundMain */
    PORT_PROF_IO,         /* asset payloads read from storage */
    PORT_PROF_LZ,         /* LZ77 / RL decompression */
    PORT_PROF_COPY,       /* CpuSet, CpuFastSet, DMA */
    PORT_PROF_LINES,      /* scanline registers played through */
    PORT_PROF_PALETTE,    /* compositor: palette and fade detection */
    PORT_PROF_LAYERS,     /* compositor: layer textures */
    PORT_PROF_DRAW,       /* compositor: geometry for the frame */
    PORT_PROF_FRAMEEND,   /* C3D_FrameEnd */
    /* Inside draw, a battle over the voxel world (RenderBattleWorld): */
    PORT_PROF_BW_WORLD,   /* the world's geometry (CtrVoxel_Draw) */
    PORT_PROF_BW_POST,    /* the world to the screen: blur, glow, shadows, curtain */
    PORT_PROF_BW_SCENE,   /* the battle's own picture: scene and layers over it */
    /* Inside draw, a battle with 3D (RenderBattleStereo, 2D or voxel): */
    PORT_PROF_BS_PREP,    /* scenery, sprite groups and slots planned */
    PORT_PROF_BS_SLOTS,   /* each group composed into its slot */
    PORT_PROF_BS_TEXT,    /* the text box composed once */
    PORT_PROF_BS_EYES,    /* both eyes put together */
    PORT_PROF_BS_WHOLE,   /* both eyes composed whole (groups not apart) */
    /* The game thread waiting on the asset table: for its lock, or for a
     * payload another thread is reading (3ds_assets.c GetPayload). */
    PORT_PROF_IO_WAIT,
    /* Inside draw, a battle's picture composed flat (RenderBattleScene and what
     * follows it); inside bw.scene over the world, inside bs.whole per eye: */
    PORT_PROF_BM_SCENE,   /* the scenery and the sprites composed, or kept */
    PORT_PROF_BM_PLACE,   /* the scene on the logical surface, and its edges */
    PORT_PROF_BM_TEXT,    /* the text box composed */
    PORT_PROF_COUNT
};

/* The table a battle animation was launched from (Port_ProfBattle). */
enum
{
    PORT_ANIM_NONE, PORT_ANIM_MOVE, PORT_ANIM_GENERAL, PORT_ANIM_SPECIAL, PORT_ANIM_STATUS,
};
/* What the game is in, for the frame's record. */
#define PORT_BATTLE_IN 1u       /* gMain.inBattle */
#define PORT_BATTLE_DOUBLE 2u   /* BATTLE_TYPE_DOUBLE */

/* Whether anything is measured: there is a log to report to. */
unsigned Port_ProfOn(void);
uint32_t Port_ProfTick(void);
void Port_ProfAdd(unsigned section, uint32_t start);
/* The screen the frame belongs to, named in the log by its callback. */
void Port_ProfScene(const void *callback2);
/* The frame being made, as the game has it: the animation playing (its table
 * PORT_ANIM_*, 0 for none, and its index there), PORT_BATTLE_* and the
 * sprites in use. Read by the record, never by anything that draws. */
void Port_ProfBattle(unsigned animKind, unsigned animId, unsigned battle, unsigned sprites);

#define PORT_PROF_BEGIN(name) uint32_t portProf_##name = Port_ProfTick()
#define PORT_PROF_END(name, section) Port_ProfAdd((section), portProf_##name)

#endif
