/*
 * Sprites' identities (include/3ds_sprite_ids.h). sprite.c numbers every
 * sprite CreateSpriteAt makes (gCtrSpriteId); what it cannot see is a sprite
 * made by copying another into a free slot, as battle animations clone the
 * battlers (gSprites[i] = gSprites[j]). Once a frame, a slot that has come into
 * use still carrying the number it had before is such a copy, and is given a
 * number of its own here.
 *
 * Not seen either way: a slot freed and filled by a copy within the same
 * frame. CTR_SPRITE_ID_AUDIT checks, every frame, that a sprite keeping its
 * number keeps its template too, and logs the slots where it does not.
 */
#include "global.h"
#include "sprite.h"
#include "3ds_platform.h"
#include "3ds_sprite_ids.h"

extern u32 gCtrSpriteId[MAX_SPRITES + 1];
extern u32 gCtrSpriteNextId;

static u32 sSeenId[MAX_SPRITES];
static bool8 sSeenInUse[MAX_SPRITES];
static u32 sMade, sCopies;
#ifdef CTR_SPRITE_ID_AUDIT
static const struct SpriteTemplate *sSeenTemplate[MAX_SPRITES];
static u32 sAnomalies;
#endif

void CtrSprites_Track(void)
{
    for (unsigned i = 0; i < MAX_SPRITES; ++i)
    {
        bool8 inUse = gSprites[i].inUse;

        if (inUse && !sSeenInUse[i] && gCtrSpriteId[i] == sSeenId[i])
        {
            gCtrSpriteId[i] = ++gCtrSpriteNextId;
            ++sCopies;
        }
        else if (inUse && gCtrSpriteId[i] != sSeenId[i])
            ++sMade;
#ifdef CTR_SPRITE_ID_AUDIT
        if (inUse && sSeenInUse[i] && gCtrSpriteId[i] == sSeenId[i] && gSprites[i].template != sSeenTemplate[i])
        {
            if (sAnomalies++ < 40)
                CtrLog_Write(CTR_LOG_GAME, "CONTRACT sprite %u kept id %lu with another template", i,
                             (unsigned long)gCtrSpriteId[i]);
        }
        sSeenTemplate[i] = gSprites[i].template;
#endif
        sSeenId[i] = gCtrSpriteId[i];
        sSeenInUse[i] = inUse;
    }
}

void CtrSprites_Report(void)
{
#ifdef CTR_SPRITE_ID_AUDIT
    CtrLog_Write(CTR_LOG_GAME, "SPRITES identities: %lu made, %lu copies numbered here, %lu kept with another template",
                 (unsigned long)sMade, (unsigned long)sCopies, (unsigned long)sAnomalies);
#else
    CtrLog_Write(CTR_LOG_GAME, "SPRITES identities: %lu made, %lu copies numbered here",
                 (unsigned long)sMade, (unsigned long)sCopies);
#endif
}
