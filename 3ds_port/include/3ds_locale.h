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
 * The port's own interface follows the game's language, or a fan translation
 * over the English game when built with PORT_LANG=pt_br (CTR_LANG_PT_BR) or
 * PORT_LANG=zh_cn (CTR_LANG_ZH_CN); the game's own texts stay those of its
 * ROM.
 *
 * CTR_TEXT takes seven strings or eight: the eighth, Simplified Chinese, is
 * optional, so a string translated later keeps the English one and the tree
 * builds while the work goes on. */
/* The trailing sentinel keeps __VA_ARGS__ non-empty for the 7-argument call,
 * which C99 does not allow to be empty. */
#define CTR_TEXT_PICK(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

#define CTR_TEXT8(english, spanish, french, italian, portuguese, german, japanese, chinese) \
    CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese)
#define CTR_TEXT7(english, spanish, french, italian, portuguese, german, japanese) \
    CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, english)

#define CTR_TEXT(...) CTR_TEXT_PICK(__VA_ARGS__, CTR_TEXT8, CTR_TEXT7, ~)(__VA_ARGS__)

#if CTR_LANG_ZH_CN
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (chinese)
#elif CTR_LANG_PT_BR
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (portuguese)
#elif GAME_LANGUAGE == LANGUAGE_SPANISH
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (spanish)
#elif GAME_LANGUAGE == LANGUAGE_FRENCH
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (french)
#elif GAME_LANGUAGE == LANGUAGE_ITALIAN
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (italian)
#elif GAME_LANGUAGE == LANGUAGE_GERMAN
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (german)
#elif GAME_LANGUAGE == LANGUAGE_JAPANESE
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (japanese)
#else
#define CTR_TEXT_SELECT(english, spanish, french, italian, portuguese, german, japanese, chinese) (english)
#endif

/* A text only the fan translations have: English, Portuguese, and Chinese
 * where it has been written (the third argument is optional, like the eighth
 * of CTR_TEXT). */
#define CTR_TEXT_PT_PICK(_1, _2, _3, NAME, ...) NAME
#define CTR_TEXT_PT3(english, portuguese, chinese) \
    CTR_TEXT(english, english, english, english, portuguese, english, english, chinese)
#define CTR_TEXT_PT2(english, portuguese) \
    CTR_TEXT(english, english, english, english, portuguese, english, english, english)
#define CTR_TEXT_PT(...) CTR_TEXT_PT_PICK(__VA_ARGS__, CTR_TEXT_PT3, CTR_TEXT_PT2, ~)(__VA_ARGS__)

/* The same with a German text. */
#define CTR_TEXT_PT_DE_PICK(_1, _2, _3, _4, NAME, ...) NAME
#define CTR_TEXT_PT_DE4(english, portuguese, german, chinese) \
    CTR_TEXT(english, english, english, english, portuguese, german, english, chinese)
#define CTR_TEXT_PT_DE3(english, portuguese, german) \
    CTR_TEXT(english, english, english, english, portuguese, german, english, english)
#define CTR_TEXT_PT_DE(...) \
    CTR_TEXT_PT_DE_PICK(__VA_ARGS__, CTR_TEXT_PT_DE4, CTR_TEXT_PT_DE3, ~)(__VA_ARGS__)

#endif
