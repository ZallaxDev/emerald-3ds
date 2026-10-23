#ifndef CTR_EXTRAS_H
#define CTR_EXTRAS_H

/*
 * Optional gameplay settings, shown on pages of their own in the bottom
 * screen's OPTIONS (ENHANCEMENTS, CHEATS) next to the game's and the port's
 * options. Each one is a line of the table in src/3ds_extras.c; its value is
 * kept in settings.txt under its key (CtrSettings_GetInt). A page with no
 * line has no tab, and OPTIONS looks as before.
 *
 * SDK-free: game translation units include it to read a setting.
 */
#include <stdbool.h>
#include <stdint.h>

enum
{
    CTR_EXTRAS_OPTIONS,        /* the page of the game's and the port's options */
    CTR_EXTRAS_ENHANCEMENTS,
    CTR_EXTRAS_CHEATS,
    CTR_EXTRAS_PAGES
};

/* A tab can hold more than one screen of cells (twelve each): the tab reads
 * "CHEATS 1/2" and a tap on it shows the next. An extra names its screen;
 * the first is the tab itself. */
#define CTR_EXTRAS_SCREEN(page, screen) ((page) | ((screen) << 4))
#define CTR_EXTRAS_TAB(page) ((page) & 15)

typedef struct
{
    uint8_t page;                  /* CTR_EXTRAS_ENHANCEMENTS or CTR_EXTRAS_CHEATS, or a later
                                    * screen of one (CTR_EXTRAS_SCREEN) */
    const char *name;              /* ASCII, as the cell shows it */
    const char *key;               /* settings.txt key */
    uint8_t count;                 /* values 0..count-1; 0: an action, run by act */
    uint8_t fallback;              /* the value when settings.txt has none */
    const char *const *values;     /* ASCII text of each value; for an action, one text */
    void (*act)(void);             /* an action (it plays its own sound), or after the value changed; may be NULL */
    /* Optional, for a value that is not one of `values` (an item...): steps
     * it instead of CtrExtras_Step, and gives its text in the game's own
     * characters (the cell arrows show when step is set). */
    void (*step)(int direction);
    const uint8_t *(*text)(void);
} CtrExtra;

extern const CtrExtra gCtrExtras[];
extern const char *const gCtrExtrasOffOn[2];   /* "OFF", "ON" */
extern const unsigned gCtrExtraCount;

int CtrExtras_Value(const CtrExtra *extra);
/* For the game code: the value of the extra with this key (0 when no build
 * line has it). */
int CtrExtras_Get(const char *key);
void CtrExtras_Step(const CtrExtra *extra, int direction);
/* An extra that acts on the voxel overworld alone. */
bool CtrExtras_NeedsVoxel(const CtrExtra *extra);
/* For an action: shows that screen of a tab (3ds_bottom_ui.c). */
void CtrExtras_ShowScreen(unsigned page, unsigned screen);

#endif // CTR_EXTRAS_H
