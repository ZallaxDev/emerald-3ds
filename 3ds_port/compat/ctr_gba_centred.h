/*
 * Force-included into the translation units whose screens are shown centred
 * as a GBA screen (full.mk, CTR_GBA_CENTRED_SRCS): the fly map, the Town Map,
 * the title menu with the professor's speech, the naming screen, the clock,
 * the choice of starter from the professor's bag, the communication error
 * screen, the Berry Blender, the wireless minigames and contests;
 * and into pokenav.c, pokemon_storage_system.c (the PC's boxes),
 * pokemon_summary_screen.c, item_menu.c (the bag), pokedex.c, trade.c and
 * trainer_card.c (the field's card, with the classic menus),
 * whose screens are shown on the bottom screen instead.
 *
 * Unlike a stage (ctr_gba_stage.h) nothing is invented around the picture:
 * it sits 1:1 in the middle of the top screen, the layers that wrap on the
 * GBA (the region map's affine sea) carry on into the margins as they would,
 * and a screen whose background is a plain field or a repeating pattern has
 * it carried on to the edges (3ds_video.c, sCentredFills). The units build
 * with the GBA geometry (CTR_GBA_STAGE) and their VBlank callbacks go through
 * the bridge, one setter per screen, so the compositor knows which of them is
 * up. main.h declares the renamed function, so the prototype still matches.
 */
#ifndef CTR_GBA_CENTRED_H
#define CTR_GBA_CENTRED_H

#if defined(CTR_CENTRED_MAIN_MENU)
#define SetVBlankCallback CtrCentredMainMenu_SetVBlankCallback
#elif defined(CTR_CENTRED_NAMING)
#define SetVBlankCallback CtrCentredNaming_SetVBlankCallback
#elif defined(CTR_CENTRED_CLOCK)
#define SetVBlankCallback CtrCentredClock_SetVBlankCallback
#elif defined(CTR_CENTRED_STARTER)
#define SetVBlankCallback CtrCentredStarter_SetVBlankCallback
#elif defined(CTR_CENTRED_POKENAV)
#define SetVBlankCallback CtrCentredPokenav_SetVBlankCallback
#elif defined(CTR_CENTRED_STORAGE)
#define SetVBlankCallback CtrCentredStorage_SetVBlankCallback
#elif defined(CTR_CENTRED_SUMMARY)
#define SetVBlankCallback CtrCentredSummary_SetVBlankCallback
#elif defined(CTR_CENTRED_BAG)
#define SetVBlankCallback CtrCentredBag_SetVBlankCallback
#elif defined(CTR_CENTRED_POKEDEX)
#define SetVBlankCallback CtrCentredPokedex_SetVBlankCallback
#elif defined(CTR_CENTRED_PARTY)
#define SetVBlankCallback CtrCentredParty_SetVBlankCallback
#elif defined(CTR_CENTRED_SHOP)
#define SetVBlankCallback CtrCentredShop_SetVBlankCallback
#elif defined(CTR_CENTRED_TRADE)
#define SetVBlankCallback CtrCentredTrade_SetVBlankCallback
#elif defined(CTR_CENTRED_BLENDER)
#define SetVBlankCallback CtrCentredBlender_SetVBlankCallback
#elif defined(CTR_CENTRED_CARD)
#define SetVBlankCallback CtrCentredCard_SetVBlankCallback
#else
#define SetVBlankCallback CtrCentred_SetVBlankCallback
#endif

#endif
