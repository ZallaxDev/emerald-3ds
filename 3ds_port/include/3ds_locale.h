#ifndef CTR_LOCALE_H
#define CTR_LOCALE_H

/* The platform sources (3ds_data.c) build without the game's include
 * directories: there, by path. */
#if defined(__has_include) && !__has_include("constants/global.h")
#include "../../include/constants/global.h"
#else
#include "constants/global.h"
#endif

/* Native UI literals are converted from UTF-8 into the game font encoding.
 * The port's own interface follows the game's language, or Brazilian
 * Portuguese over the English game when built with PORT_LANG=pt_br
 * (CTR_LANG_PT_BR); the game's own texts stay those of its ROM. */
#if CTR_LANG_PT_BR
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (portuguese)
#elif GAME_LANGUAGE == LANGUAGE_SPANISH
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (spanish)
#elif GAME_LANGUAGE == LANGUAGE_FRENCH
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (french)
#elif GAME_LANGUAGE == LANGUAGE_ITALIAN
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (italian)
#elif GAME_LANGUAGE == LANGUAGE_GERMAN
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (german)
#elif GAME_LANGUAGE == LANGUAGE_JAPANESE
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (japanese)
#else
#define CTR_TEXT(english, spanish, french, italian, portuguese, german, japanese) (english)
#endif

/* A text only English and Portuguese have. */
#define CTR_TEXT_PT(english, portuguese) CTR_TEXT(english, english, english, english, portuguese, english, english)
/* The same with a German text. */
#define CTR_TEXT_PT_DE(english, portuguese, german) CTR_TEXT(english, english, english, english, portuguese, german, english)

/* The same with French, Italian and German texts. */
#define CTR_TEXT_FR_IT_PT_DE(english, french, italian, portuguese, german) \
    CTR_TEXT(english, english, french, italian, portuguese, german, english)

#endif
