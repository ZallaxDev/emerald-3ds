"""The bottom screen's logic with its production functions, on the host.

The map extras (3ds_field_qol_extras.inc): the wild list's tables, timed
grass, methods, seen-only names, a method's pages of ENC_SHOWN rows; the
clock banner; their taps. The frame (3ds_bottom_shell.inc): the dock's
buttons laid out without overlap for eight and nine sections; the parts a
partial redraw is limited to (the clock, the Repel, a pressed button and the
title it names, the focus, the options' panel and tabs) and the changes that
must redraw everything; the copy round the game's window, which never
touches the picture the compositor draws there; the player's mark moving
alone. Functions are taken from the sources by name (voxel_runtime_test.function)
and built with mock primitives."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from voxel_runtime_test import function, ROOT


def between(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first) + len(end)] + '\n'


def statement(source, start):
    return between(source, start, ';')


def define(source, name):
    match = re.search(r'^#define ' + name + r'\b.*$', source, re.M)
    assert match, name
    return match.group(0) + '\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    bottom = (ROOT / 'src/3ds_bottom_ui.c').read_text(encoding='utf-8')
    shell = (ROOT / 'src/3ds_bottom_shell.inc').read_text(encoding='utf-8')
    plate = (ROOT / 'src/3ds_bottom_plate.inc').read_text(encoding='utf-8')
    options = (ROOT / 'src/3ds_bottom_options.inc').read_text(encoding='utf-8')
    clock = (ROOT / 'src/3ds_clock_store.c').read_text(encoding='utf-8')
    extras = (ROOT / 'src/3ds_extras.c').read_text(encoding='utf-8')
    table = (ROOT / 'src/3ds_field_qol_extras.inc').read_text(encoding='utf-8')
    # Entries after the sentinel corrupt count-based settings lookup.
    assert extras.index('#include "3ds_field_qol_extras.inc"') < extras.index('    {0},')
    hook_patch = ROOT.parent / 'patches/pokeemerald/0078-field-clock-notice.patch'
    if hook_patch.is_file():
        hook = hook_patch.read_text(encoding='utf-8')
        assert '+#include "3ds_clock_store.h"' in hook
        assert ' void NewGameInitData(void)\n {\n+#ifdef PLATFORM_3DS\n+    CtrClockStore_Load(NULL, 0);' in hook
    else:
        # Bootstrap overlays the port into upstream without the patch directory.
        # Verify the applied production hook there, rather than skipping it.
        new_game = (ROOT.parent / 'src/new_game.c').read_text(encoding='utf-8')
        assert '#include "3ds_clock_store.h"' in new_game
        init = function(new_game, 'void NewGameInitData(void)')
        reset = '#ifdef PLATFORM_3DS\n    CtrClockStore_Load(NULL, 0);\n#endif'
        assert reset in init
        assert init.index(reset) < init.index('gSaveFileStatus')
    for key in ('map_encounters', 'repel_steps', 'clock_notice'):
        assert f'"{key}", 2, 0, gCtrExtrasOffOn' in table
    # Every section's .inc is part of the build.
    for inc in ('plate', 'shell', 'field', 'options'):
        assert f'#include "3ds_bottom_{inc}.inc"' in bottom
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "3ds_clock_store.h"
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int8_t s8; typedef int16_t s16;
typedef bool bool8;
#define TRUE true
#define FALSE false
#define W 320
#define H 240
#define CTR_BOTTOM_WIN_X 40
#define CTR_BOTTOM_WIN_Y 40
#define PARTY_SIZE 6
#define POKEMON_NAME_LENGTH 10
#define PLAYER_NAME_LENGTH 7
#define MAX_MON_MOVES 4
#define ITEM_NONE 0
#define MAPSEC_NONE 213
#define CTR_TEXT(english, spanish, french, italian, german, portuguese) spanish
#define SPECIES_NONE 0
#define FLAG_GET_SEEN 0
#define VAR_REPEL_STEP_COUNT 0x4021
#define VAR_ALTERING_CAVE_WILD_SET 0x403E
#define MAP_GROUP(map) ((map) >> 8)
#define MAP_NUM(map) ((map) & 0xFF)
#define MAP_UNDEFINED (0xFF | (0xFF << 8))
#define MAP_ALTERING_CAVE ((24 << 8) | 5)
#define FOCUS_NONE 0xFF
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
struct ChooseMoveStruct { u8 data[64]; };
/* The game's screens' views (3ds_game_views.h): only compared here. */
struct CtrPartyView { u8 state, cursor, data[350]; u8 iconFrame[6]; s8 iconLift[6]; };
struct CtrSummaryView { u8 state, page, data[400]; const u8 *picTiles; u32 picSum; };
struct CtrBagView { u8 state, pocket, data[700]; };
struct CtrDexView { u8 state, national, data[300]; };
'''
    source += between(bottom, 'enum { SCR_MAP,', '};')
    source += between(bottom, 'enum { COL_Y = SCR_COUNT', '};')
    source += between(bottom, 'enum { OPT_TEXT_SPEED,', '};')
    source += define(bottom, 'HIT_OPTION_BACK')
    source += between(bottom, 'enum\n{\n    MODE_OFF,', '};')
    source += between(bottom, 'typedef struct\n{\n    u16 species;      /* SPECIES_NONE: empty slot */', '} MonView;')
    source += between(bottom, '#define ENC_ROWS', '} EncRow;')
    for name in ('ENC_SHOWN', 'OPT_ROWS_SHOWN', 'NUZ_TEXT'):
        source += define(bottom, name)
    source += between(bottom, '/* Everything a redraw reads.', '} ViewState;')
    source += between(bottom, 'enum { NATIVE_NONE,', '};')
    source += between(bottom, 'enum\n{\n    HIT_NONE = 0xFF,', '};')
    for name in ('PLATE_ML', 'PLATE_MT', 'PLATE_MR', 'PLATE_MB'):
        source += define(plate, name)
    source += function(plate, 'static void PlateBounds(')
    for name in ('HEAD_Y', 'HEAD_H', 'HEAD_Y_X', 'HEAD_Y_W', 'TITLE_X', 'TITLE_W', 'REPEL_X', 'REPEL_W', 'CLOCK_X',
                 'CLOCK_W', 'RUN_X', 'RUN_W', 'TITLE_WIDE_W', 'DOCK_Y', 'DOCK_TOP', 'DOCK_H', 'DOCK_GAP', 'AREA_X',
                 'AREA_Y', 'AREA_W', 'AREA_H', 'MAP_WIN_X', 'MAP_WIN_Y', 'MAP_WIN_W', 'MAP_WIN_H', 'MAP_ORIGIN_X',
                 'MAP_ORIGIN_Y', 'GAME_WIN_X', 'GAME_WIN_Y', 'GAME_WIN_W', 'GAME_WIN_H'):
        source += define(shell, name)
    for name in ('MAP_PIC_W', 'MAP_PIC_H'):
        source += define((ROOT / 'src/3ds_bottom_field.inc').read_text(encoding='utf-8'), name)
    for name in ('SUM_TAB_H', 'SUM_Y', 'SUM_LEFT_W', 'SUM_PIC_BOTTOM'):
        source += define((ROOT / 'src/3ds_bottom_game.inc').read_text(encoding='utf-8'), name)
    for name in ('OPT_PANEL_Y', 'OPT_PANEL_H'):
        source += define(options, name)
    source += r'''
typedef struct { s16 x, y, w, h; u8 id; } Hit;
static Hit sHits[64];
static u8 sHitCount;
static int sOX;
static ViewState sShown, sState;
static bool sInUse;
static bool CtrVideo_BottomInUse(void) {return sInUse;}
static bool CtrVideo_BottomWhole(void) {return false;}
static void CtrVideo_BottomStrips(const u16 *canvas) {(void)canvas;}
static bool8 PartyIconRect(const ViewState *s, int slot, int *x0, int *y0, int *x1, int *y1) {(void)s;(void)slot;(void)x0;(void)y0;(void)x1;(void)y1;return FALSE;}
/* The screen: which pixels each copy wrote. */
static u8 fbWrites[W*H];
static unsigned blits;
static void CtrBottom_BlitRect(const u16 *c,int x0,int y0,int x1,int y1) {
    (void)c;++blits;assert(x0>=0&&y0>=0&&x1<=W&&y1<=H&&x0<x1&&y0<y1);
    for(int x=x0;x<x1;++x)for(int y=y0;y<y1;++y)++fbWrites[x*H+y];
}
static void CtrBottom_Blit(const u16 *c,int x0,int x1) {CtrBottom_BlitRect(c,x0,0,x1,H);}
static u16 sCanvas[W*H];
/* The game's side of the map extras. */
static const u8 gSpeciesNames[16][11] = {"", "AAA", "BBB", "CCC", "DDD", "EEE", "FFF", "GGG", "HHH", "III"};
struct WildPokemon { u8 minLevel, maxLevel; u16 species; };
struct WildPokemonInfo { u8 encounterRate; const struct WildPokemon *wildPokemon; };
struct WildPokemonHeader { u8 mapGroup, mapNum; const struct WildPokemonInfo *landMonsInfo, *waterMonsInfo, *rockSmashMonsInfo, *fishingMonsInfo; };
static const struct WildPokemon routeLand[12] = {{2,3,1},{2,2,2},{3,4,1},{4,4,3},{2,2,2},{5,5,4},{3,3,1},{4,4,1},{6,6,5},{6,6,5},{7,7,6},{7,7,6}};
static const struct WildPokemon timedLand[12] = {{2,3,7},{2,2,2},{3,4,7},{4,4,3},{2,2,2},{5,5,4},{3,3,7},{4,4,7},{6,6,5},{6,6,5},{7,7,6},{7,7,6}};
static const struct WildPokemon wideLand[12] = {{2,2,1},{2,2,2},{2,2,3},{2,2,4},{2,2,5},{2,2,6},{2,2,7},{2,2,8},{2,2,9},{2,2,1},{2,2,2},{2,2,3}};
static const struct WildPokemon routeFish[10] = {{5,10,1},{5,10,2},{10,30,2},{10,30,3},{10,30,4},{20,40,5},{20,40,5},{20,40,6},{20,40,7},{25,45,7}};
static const struct WildPokemon seaWater[5] = {{20,30,3},{20,30,3},{20,30,4},{30,35,4},{30,35,5}};
static const struct WildPokemon caveA[12] = {{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1},{9,9,1}};
static const struct WildPokemon caveC[12] = {{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6},{8,8,6}};
static const struct WildPokemonInfo routeLandInfo={20,routeLand}, timedInfo={20,timedLand}, routeFishInfo={30,routeFish},
    seaWaterInfo={4,seaWater}, caveAInfo={7,caveA}, caveBInfo={7,timedLand}, caveCInfo={7,caveC}, wideInfo={20,wideLand};
static const struct WildPokemonHeader gWildMonHeaders[] = {
    {0,1,&routeLandInfo,NULL,NULL,&routeFishInfo}, {0,2,NULL,&seaWaterInfo,NULL,NULL},
    {24,5,&caveAInfo,NULL,NULL,NULL}, {24,5,&caveBInfo,NULL,NULL,NULL}, {24,5,&caveCInfo,NULL,NULL,NULL},
    {0,3,&wideInfo,&seaWaterInfo,NULL,NULL},
    {0xFF,0xFF,NULL,NULL,NULL,NULL}};
static struct { struct { int8_t mapGroup, mapNum; } location; } sSave1, *gSaveBlock1Ptr=&sSave1;
static u16 sVars[0x100];
static u16 VarGet(u16 id) {return sVars[id-0x4000];}
static bool sTimed; static unsigned landGroup=99, landNum=99;
static const struct WildPokemonInfo *CtrEncounters_Land(const struct WildPokemonInfo *o,unsigned g,unsigned n) {
    landGroup=g;landNum=n;return sTimed&&o==&routeLandInfo?&timedInfo:o;
}
static unsigned sMapped;
static u16 CtrEncounters_DisplaySpecies(u16 species) {return sMapped&&species==1?(sMapped==2?2:7):species;}
static int8_t sSeen[256];
static u16 SpeciesToNationalPokedexNum(u16 species) {return species+100;}
static int8_t GetSetPokedexFlag(u16 national,u8 caseId) {assert(caseId==FLAG_GET_SEEN);return sSeen[national];}
static int sExtra[3];
static int CtrExtras_Get(const char *key) {
    if(!strcmp(key,"map_encounters"))return sExtra[0];
    if(!strcmp(key,"repel_steps"))return sExtra[1];
    assert(!strcmp(key,"clock_notice"));return sExtra[2];
}
static CtrClockStoreStatus sStatus; static unsigned sSerial;
CtrClockStoreStatus CtrClockStore_Status(void) {return sStatus;}
unsigned CtrClockStore_LoadSerial(void) {return sSerial;}
static bool8 sEncView; static u8 sEncPage; static unsigned sClockNoticeSerial;
static u8 sPickMapsec;
#define SE_SELECT 1
static unsigned selectSounds;
static void PlaySE(int sound) {assert(sound==SE_SELECT);++selectSounds;}
/* The battle's parts are not under test here. */
static bool8 RectAddPlate(void *r,u8 id) {(void)r;(void)id;return FALSE;}
static u8 BattleCursorHit(const ViewState *s,u8 cursor) {(void)s;return cursor;}
'''
    source += function(clock, 'bool CtrClockStore_NeedsNotice(')
    source += between(bottom, 'enum { ENC_LAND', '};')
    for name in ('sEncLandRates', 'sEncWaterRates', 'sEncFishRates', 'sEncFirst', 'sEncSlots'):
        source += statement(bottom, 'static const u8 ' + name)
    for name in ('static void AddHit(', 'static u8 EncCollect(', 'static const struct WildPokemonHeader *EncHeader(',
                 'static void SnapshotEncounters(', 'static void SnapshotMapExtras(', 'static void MapExtrasTap(',
                 'static bool8 MapCursorOnlyMoved(', 'static void BlitAroundWindow('):
        source += function(bottom, name)
    for name in ('static int DockCount(', 'static void DockButton(', 'static int TitleWidth(',
                 'static void TitleRect(', 'static void RepelRect(', 'static void ClockRect('):
        source += function(shell, name)
    source += function(options, 'static void OptionsPanelRect(')
    source += between(bottom, 'typedef struct { int x0, y0, x1, y1; } Rect;', '} Rect;')
    for name in ('static void RectInit(', 'static void RectAdd(', 'static bool8 RectAddHitOf(',
                 'static bool8 RectAddHit(', 'static void AreaRect(', 'static bool8 RectAddFocus(',
                 'static bool8 DirtyRect('):
        source += function(bottom, name).replace('RectAddPlate(r,', 'RectAddPlate((void *)r,')
    source += r'''
static void Extras(ViewState *s) {
    u8 mapsec=s->mapsec,cx=s->cursorX,cy=s->cursorY,pick=s->pickMapsec;
    memset(s,0,sizeof(*s));s->mode=MODE_FIELD;s->mapsec=mapsec;s->cursorX=cx;s->cursorY=cy;s->pickMapsec=pick;
    SnapshotMapExtras(s);
}
static void TestTables(void) {
    EncRow rows[ENC_ROWS];
    ViewState s;
    int total=0;
    /* Duplicate slots are one row: chances added up, levels' range, slot order. */
    assert(EncCollect(rows,routeLand,0,12,sEncLandRates)==6);
    assert(rows[0].species==1&&rows[0].percent==40&&rows[0].minLevel==2&&rows[0].maxLevel==4);
    assert(rows[1].species==2&&rows[1].percent==30&&rows[1].minLevel==2&&rows[1].maxLevel==2);
    assert(rows[2].species==3&&rows[2].percent==10&&rows[4].species==5&&rows[4].percent==8);
    assert(rows[5].species==6&&rows[5].percent==2&&rows[5].minLevel==7);
    for(int i=0;i<6;++i)total+=rows[i].percent;
    assert(total==100);
    /* A route with grass and fishing: four pages, no surfing or Rock Smash invented. */
    sSave1.location.mapGroup=0;sSave1.location.mapNum=1;sExtra[0]=1;sEncView=1;sEncPage=0;sTimed=false;
    memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPages==4&&s.encPage==0&&s.encMethod==ENC_LAND&&s.encCount==6&&s.enc[0].species==1);
    assert(landGroup==0&&landNum==1);
    /* The timed tables: the grass page is what CtrEncounters_Land gives. */
    sTimed=true;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encMethod==ENC_LAND&&s.enc[0].species==7&&s.enc[0].percent==40);
    sEncPage=1;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encMethod==ENC_OLD_ROD&&s.encCount==2&&s.enc[0].species==1&&s.enc[0].percent==70&&s.enc[1].percent==30);
    sEncPage=6;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPage==2&&s.encMethod==ENC_GOOD_ROD&&s.encCount==3&&s.enc[0].species==2&&s.enc[0].percent==60);
    sEncPage=3;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encMethod==ENC_SUPER_ROD&&s.encCount==3&&s.enc[0].species==5&&s.enc[0].percent==80);
    assert(s.enc[2].species==7&&s.enc[2].percent==5&&s.enc[2].minLevel==20&&s.enc[2].maxLevel==45);
    /* More species than a page holds: the method's rows go on to its next page,
     * then the next method. Every row once, in slot order. */
    sTimed=false;sSave1.location.mapNum=3;sEncPage=0;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPages==3&&s.encMethod==ENC_LAND&&s.encCount==ENC_SHOWN&&s.enc[0].species==1&&s.enc[5].species==6);
    assert(s.enc[0].percent==24&&s.enc[1].percent==21&&s.enc[2].percent==11);
    sEncPage=1;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPage==1&&s.encMethod==ENC_LAND&&s.encCount==3&&s.enc[0].species==7&&s.enc[2].species==9);
    sEncPage=2;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encMethod==ENC_WATER&&s.encCount==3);
    sEncPage=3;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPage==0&&s.encMethod==ENC_LAND);
    /* Water only; no table at all. */
    sEncPage=0;sSave1.location.mapNum=2;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPages==1&&s.encMethod==ENC_WATER&&s.encCount==3&&s.enc[0].percent==90);
    sSave1.location.mapNum=9;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encPages==0&&s.encCount==0);
    /* Altering Cave: its set; one past the sets is the first. */
    sSave1.location.mapGroup=24;sSave1.location.mapNum=5;sVars[VAR_ALTERING_CAVE_WILD_SET-0x4000]=2;
    memset(&s,0,sizeof(s));SnapshotEncounters(&s);assert(s.encCount==1&&s.enc[0].species==6&&s.enc[0].percent==100);
    sVars[VAR_ALTERING_CAVE_WILD_SET-0x4000]=3;
    memset(&s,0,sizeof(s));SnapshotEncounters(&s);assert(s.encCount==1&&s.enc[0].species==1);
    /* Seen by national number. */
    sSave1.location.mapGroup=0;sSave1.location.mapNum=1;sSeen[102]=1;
    memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(!s.enc[0].seen&&s.enc[1].species==2&&s.enc[1].seen&&!s.enc[2].seen);
    /* Mapping precedes visibility: seeing the original does not reveal the new species. */
    sSeen[101]=1;sMapped=true;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.enc[0].species==7&&!s.enc[0].seen&&s.enc[0].percent==40);
    sSeen[107]=1;memset(&s,0,sizeof(s));SnapshotEncounters(&s);assert(s.enc[0].seen);
    sMapped=2;memset(&s,0,sizeof(s));SnapshotEncounters(&s);
    assert(s.encCount==5&&s.enc[0].species==2&&s.enc[0].percent==70);
    assert(s.enc[0].minLevel==2&&s.enc[0].maxLevel==4);
    sSeen[101]=sSeen[107]=0;sMapped=false;
}
static void TestExtras(void) {
    ViewState s={.mapsec=2,.cursorX=9,.cursorY=8,.pickMapsec=6};
    /* Off: nothing of them, the map as it was. */
    sExtra[0]=sExtra[1]=sExtra[2]=0;sEncView=1;sStatus=CTR_CLOCK_STORE_INVALID;sSerial=1;sVars[0x21]=40;
    Extras(&s);
    assert(!s.encOn&&!s.encView&&!s.encPages&&!s.clockNotice);
    assert(s.cursorX==9&&s.pickMapsec==6);
    /* The list: no marks, no picked cell, the area's tables. */
    sExtra[0]=1;sEncPage=0;sSave1.location.mapGroup=0;sSave1.location.mapNum=1;
    Extras(&s);assert(s.encOn&&s.encView&&s.encPages==4&&s.cursorX==0&&s.cursorY==0&&s.pickMapsec==MAPSEC_NONE);
    sEncView=0;s.cursorX=9;s.pickMapsec=6;Extras(&s);assert(s.encOn&&!s.encView&&!s.encPages&&s.cursorX==9&&s.pickMapsec==6);
    /* The banner: a saved clock lost, until dismissed for that load. */
    sExtra[2]=1;sClockNoticeSerial=0;sSerial=4;
    sStatus=CTR_CLOCK_STORE_INVALID;Extras(&s);assert(s.clockNotice);
    sStatus=CTR_CLOCK_STORE_MISMATCH;Extras(&s);assert(s.clockNotice);
    sClockNoticeSerial=4;Extras(&s);assert(!s.clockNotice);
    sSerial=5;Extras(&s);assert(s.clockNotice);
    sStatus=CTR_CLOCK_STORE_MISSING;Extras(&s);assert(s.clockNotice);
    sStatus=CTR_CLOCK_STORE_NONE;Extras(&s);assert(!s.clockNotice);
    sStatus=CTR_CLOCK_STORE_RECOVERED;Extras(&s);assert(!s.clockNotice);
    sStatus=CTR_CLOCK_STORE_RESTORED;Extras(&s);assert(!s.clockNotice);
    sExtra[2]=0;sStatus=CTR_CLOCK_STORE_INVALID;Extras(&s);assert(!s.clockNotice);
    sExtra[0]=sExtra[1]=0;sEncView=0;
}
static void TestInput(void) {
    memset(&sShown,0,sizeof(sShown));sEncView=0;sEncPage=5;sPickMapsec=8;
    MapExtrasTap(HIT_MAP_INFO);assert(!sEncView&&sEncPage==5&&sPickMapsec==8);
    /* The title toggles the list on the map alone. */
    sShown.encOn=1;sShown.screen=SCR_CARD;MapExtrasTap(HIT_MAP_INFO);assert(!sEncView);
    sShown.screen=SCR_MAP;MapExtrasTap(HIT_MAP_INFO);
    assert(sEncView&&sEncPage==0&&sPickMapsec==MAPSEC_NONE);
    sShown.encView=1;sShown.encPages=4;sShown.encPage=3;
    MapExtrasTap(HIT_MAP_PAGE);assert(sEncPage==0);
    sShown.encPage=1;MapExtrasTap(HIT_MAP_PAGE);assert(sEncPage==2);
    sShown.encPages=1;sEncPage=0;MapExtrasTap(HIT_MAP_PAGE);assert(sEncPage==0);
    sShown.encPages=0;MapExtrasTap(HIT_MAP_PAGE);assert(sEncPage==0);
    sShown.encPages=4;sShown.encView=0;MapExtrasTap(HIT_MAP_PAGE);assert(sEncPage==0);
    sShown.encView=1;MapExtrasTap(HIT_MAP_INFO);assert(!sEncView&&sEncPage==0);
    sShown.clockNotice=0;sSerial=20;sClockNoticeSerial=5;
    MapExtrasTap(HIT_CLOCK_NOTICE);assert(sClockNoticeSerial==5);
    sShown.clockNotice=1;MapExtrasTap(HIT_CLOCK_NOTICE);assert(sClockNoticeSerial==20);
    MapExtrasTap(HIT_MAP);assert(sClockNoticeSerial==20);
    assert(selectSounds==4);
}
/* The dock: inside the screen, a gap between buttons, the same width each. */
static void TestDock(void) {
    ViewState s={.mode=MODE_FIELD,.dock=1};
    for(int nuz=0;nuz<2;++nuz) {
        int prev=-1,width=-1;
        s.enabled=nuz?0x1FF:0xFF;
        assert(DockCount(&s)==(nuz?SCR_COUNT:SCR_NUZLOCKE));
        for(int i=0;i<DockCount(&s);++i) {
            int x,w;
            DockButton(&s,i,&x,&w);
            assert(x>=2&&x+w<=W-2&&w>=30);
            assert(width<0||w==width);width=w;
            assert(prev<0||x>=prev+DOCK_GAP);prev=x+w;
        }
    }
    /* The header's plates side by side, the title over the Repel's room without it. */
    assert(HEAD_Y_X+HEAD_Y_W<=TITLE_X&&TITLE_X+TITLE_W<=REPEL_X&&REPEL_X+REPEL_W<=CLOCK_X);
    assert(CLOCK_X+CLOCK_W<=RUN_X&&RUN_X+RUN_W<=W&&TITLE_X+TITLE_WIDE_W<CLOCK_X);
    s.repelShown=1;assert(TitleWidth(&s)==TITLE_W);s.repelShown=0;assert(TitleWidth(&s)==TITLE_WIDE_W);
    /* The content between the header and the dock; the windows in it. */
    assert(HEAD_Y+HEAD_H<=AREA_Y&&AREA_Y+AREA_H<=DOCK_Y&&AREA_X+AREA_W<=W);
    assert(MAP_ORIGIN_X+MAP_PIC_W+2==MAP_WIN_X+MAP_WIN_W&&MAP_ORIGIN_Y+MAP_PIC_H+2==MAP_WIN_Y+MAP_WIN_H);
    assert(GAME_WIN_X+2==CTR_BOTTOM_WIN_X&&GAME_WIN_Y+2==CTR_BOTTOM_WIN_Y&&GAME_WIN_W==244&&GAME_WIN_H==164);
    assert(GAME_WIN_Y>=AREA_Y&&GAME_WIN_Y+GAME_WIN_H<=AREA_Y+AREA_H);
}
/* Round the game's window, every pixel of a part once; inside it, none. */
static void TestBlit(void) {
    static const int parts[][4]={{0,0,W,H},{0,0,320,40},{30,30,60,220},{100,100,120,120},{270,190,300,210},{0,200,W,H},
                                 {39,39,281,201},{0,0,40,H}};
    for(int use=0;use<2;++use)
        for(unsigned k=0;k<sizeof(parts)/sizeof(parts[0]);++k) {
            const int *p=parts[k];
            sInUse=use;memset(fbWrites,0,sizeof(fbWrites));
            BlitAroundWindow(p[0],p[1],p[2],p[3]);
            for(int x=0;x<W;++x)for(int y=0;y<H;++y) {
                bool inPart=x>=p[0]&&x<p[2]&&y>=p[1]&&y<p[3];
                bool inWindow=x>=CTR_BOTTOM_WIN_X&&x<CTR_BOTTOM_WIN_X+240&&y>=CTR_BOTTOM_WIN_Y&&y<CTR_BOTTOM_WIN_Y+160;
                assert(fbWrites[x*H+y]==(inPart&&!(use&&inWindow)?1:0));
            }
        }
    /* A game's screen drawn natively: the window is the canvas's too. */
    sInUse=true;sState.native=NATIVE_PARTY;memset(fbWrites,0,sizeof(fbWrites));
    BlitAroundWindow(0,0,W,H);
    for(int i=0;i<W*H;++i)assert(fbWrites[i]==1);
    sInUse=false;sState.native=NATIVE_NONE;
}
static ViewState Field(u8 screen) {
    ViewState s;
    memset(&s,0,sizeof(s));
    s.mode=MODE_FIELD;s.screen=screen;s.dock=1;s.enabled=0xFF;s.focus=FOCUS_NONE;s.optFocus=FOCUS_NONE;
    s.pressed=HIT_NONE;s.mapsec=MAPSEC_NONE;s.pickMapsec=MAPSEC_NONE;s.repelShown=1;
    return s;
}
static bool Inside(Rect r,int x0,int y0,int x1,int y1) {return r.x0<=x0&&r.y0<=y0&&r.x1>=x1&&r.y1>=y1;}
static void TestDirty(void) {
    ViewState a=Field(SCR_MAP),b;
    Rect r;
    int x0,y0,x1,y1;
    sShown=a;sHitCount=0;
    for(int i=0;i<DockCount(&a);++i){int x,w;DockButton(&a,i,&x,&w);AddHit(x-1,DOCK_Y,w+2,H-DOCK_Y,HIT_COLUMN+i);}
    AddHit(TITLE_X,HEAD_Y-2,TITLE_W,HEAD_H+4,HIT_MAP_INFO);
    /* The clock's minute: its plate, nothing else. */
    b=a;b.clockMinute=601;assert(DirtyRect(&b,&a,&r));
    ClockRect(&x0,&y0,&x1,&y1);assert(r.x0==x0&&r.y0==y0&&r.x1==x1&&r.y1==y1);
    /* The Repel's steps: its plate; with the clock, both. */
    b=a;b.repelSteps=99;assert(DirtyRect(&b,&a,&r));
    RepelRect(&x0,&y0,&x1,&y1);assert(r.x0==x0&&r.y0==y0&&r.x1==x1&&r.y1==y1);
    b.clockRate=6;assert(DirtyRect(&b,&a,&r));ClockRect(&x0,&y0,&x1,&y1);assert(Inside(r,x0,y0,x1,y1));
    /* Its plate shown or hidden widens the title: everything. */
    b=a;b.repelShown=0;assert(!DirtyRect(&b,&a,&r));
    /* A dock button held: the button and the title that names its section. */
    b=a;b.pressed=HIT_COLUMN+SCR_BAG;assert(DirtyRect(&b,&a,&r));
    TitleRect(&a,&x0,&y0,&x1,&y1);assert(Inside(r,x0,y0,x1,y1));
    {int x,w;DockButton(&a,SCR_BAG,&x,&w);assert(Inside(r,x-1,DOCK_Y,x+w+1,H));}
    assert(DirtyRect(&a,&b,&r)&&Inside(r,x0,y0,x1,y1));
    /* The title pressed: its plate. */
    b=a;b.pressed=HIT_MAP_INFO;assert(DirtyRect(&b,&a,&r)&&Inside(r,TITLE_X,HEAD_Y,TITLE_X+TITLE_W,HEAD_Y+HEAD_H));
    assert(r.y1<=AREA_Y+PLATE_MB+4);
    /* The focus moved along the dock, or up to the title. */
    b=a;b.focus=SCR_MAP;assert(DirtyRect(&b,&a,&r));
    {int x,w;DockButton(&a,SCR_MAP,&x,&w);PlateBounds(x,DOCK_TOP,w,DOCK_H,&x0,&y0,&x1,&y1);
     assert(r.x0==x0&&r.y0==y0&&r.x1==x1&&r.y1==y1);}
    a.focus=SCR_MAP;b=a;b.focus=COL_TITLE;assert(DirtyRect(&b,&a,&r));
    TitleRect(&a,&x0,&y0,&x1,&y1);assert(Inside(r,x0,y0,x1,y1)&&r.y1>DOCK_TOP);
    a.focus=FOCUS_NONE;
    /* Anything else: everything. */
    b=a;b.mapsec=3;assert(!DirtyRect(&b,&a,&r));
    b=a;b.screen=SCR_CARD;assert(!DirtyRect(&b,&a,&r));
    b=a;b.enabled|=1<<SCR_NUZLOCKE;assert(!DirtyRect(&b,&a,&r));
    b=a;b.dock=0;assert(!DirtyRect(&b,&a,&r));
    b=a;b.encView=1;assert(!DirtyRect(&b,&a,&r));
    b=a;b.clockNotice=1;assert(!DirtyRect(&b,&a,&r));
    /* Without the dock (a game's screen with the whole screen): no parts. */
    a.dock=0;b=a;b.clockMinute=5;assert(!DirtyRect(&b,&a,&r));
    /* The options: a value or the focus is their panel; a tab, the area. */
    a=Field(SCR_OPTION);sShown=a;b=a;b.options[OPT_TEXT_SPEED]=2;assert(DirtyRect(&b,&a,&r));
    OptionsPanelRect(&x0,&y0,&x1,&y1);assert(r.x0==x0&&r.y0==y0&&r.x1==x1&&r.y1==y1);
    b=a;b.rows[3]=7;assert(DirtyRect(&b,&a,&r)&&r.y0==y0&&r.y1==y1);
    b=a;b.optFocus=2;assert(DirtyRect(&b,&a,&r)&&r.y0==y0);
    b=a;b.optPage=1;assert(DirtyRect(&b,&a,&r));
    assert(Inside(r,AREA_X,AREA_Y,AREA_X+AREA_W,AREA_Y+AREA_H)&&r.y0>=HEAD_Y&&r.y1<=DOCK_TOP+PLATE_MB);
    b=a;b.optSub=1;b.options[1]=1;assert(DirtyRect(&b,&a,&r)&&r.y0<OPT_PANEL_Y);
    b=a;b.saveStep=1;assert(!DirtyRect(&b,&a,&r));
    /* A native view of the game's screen: what it reads is the area; its
     * box held, that box. */
    a=Field(SCR_POKEMON);a.mode=MODE_PARTY_MENU;a.native=NATIVE_PARTY;sShown=a;
    b=a;b.pv.cursor=3;assert(DirtyRect(&b,&a,&r)&&Inside(r,AREA_X,AREA_Y,AREA_X+AREA_W,AREA_Y+AREA_H));
    b=a;b.party[2].hp=7;assert(DirtyRect(&b,&a,&r)&&Inside(r,AREA_X,AREA_Y,AREA_X+AREA_W,AREA_Y+AREA_H));
    sHitCount=0;AddHit(120,40,200,30,HIT_ROW+1);
    b=a;b.pressed=HIT_ROW+1;assert(DirtyRect(&b,&a,&r)&&Inside(r,120,40,320,70)&&r.y1<AREA_Y+AREA_H);
    b=a;b.native=NATIVE_NONE;assert(!DirtyRect(&b,&a,&r));
    /* Without a native view those fields are not a part. */
    a.native=NATIVE_NONE;b=a;b.pv.cursor=1;assert(!DirtyRect(&b,&a,&r));
    /* Elsewhere the options' fields are not a part. */
    a=Field(SCR_CARD);b=a;b.options[0]=1;assert(!DirtyRect(&b,&a,&r));
    b=a;b.cardBack=1;assert(!DirtyRect(&b,&a,&r));
}
static void TestMark(void) {
    ViewState a=Field(SCR_MAP),b;
    a.mapsec=2;a.cursorX=9;a.cursorY=8;
    b=a;b.cursorX++;assert(MapCursorOnlyMoved(&b,&a));
    b.repelSteps=40;assert(MapCursorOnlyMoved(&b,&a));
    b.mapsec=32;assert(MapCursorOnlyMoved(&b,&a));
    b=a;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.cursorX++;b.clockMinute++;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.cursorX++;b.pressed=1;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.cursorX++;b.dock=0;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.cursorX++;b.mode=MODE_POKEDEX;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.mapsec=MAPSEC_NONE;assert(!MapCursorOnlyMoved(&b,&a));
    /* No mark moving on the list or under the banner. */
    a.encView=1;b=a;b.mapsec=32;assert(!MapCursorOnlyMoved(&b,&a));
    a.encView=0;a.clockNotice=1;b=a;b.cursorX++;assert(!MapCursorOnlyMoved(&b,&a));
    a=Field(SCR_CARD);a.mapsec=2;b=a;b.cursorX=1;assert(!MapCursorOnlyMoved(&b,&a));
    /* Zoomed in the mark still moves alone; zooming or panning is all new,
     * and nothing walks on the Fly map. */
    a=Field(SCR_MAP);a.mapsec=2;a.cursorX=9;a.cursorY=8;a.mapZoom=1;a.mapViewX=40;
    b=a;b.cursorX++;assert(MapCursorOnlyMoved(&b,&a));
    b.mapZoom=0;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.cursorX++;b.mapViewX+=8;assert(!MapCursorOnlyMoved(&b,&a));
    a.mapZoom=0;a.mapFly=1;b=a;b.cursorX++;assert(!MapCursorOnlyMoved(&b,&a));
}
int main(void) {
    (void)sCanvas;(void)gSpeciesNames;(void)blits;
    TestTables();
    TestExtras();
    TestInput();
    TestDock();
    TestBlit();
    TestDirty();
    TestMark();
    puts("PASS bottom screen: wild list tables/timed grass/methods/pages of rows/Altering Cave, seen-only masking, "
         "taps and clock banner gating; dock and header layout; copies round the game's window; partial redraws "
         "(clock, Repel, held button and title, focus, options panel and tabs, native views) and whole-redraw fallbacks; the mark moving alone, zoomed too, never on the Fly map");
}
'''
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bottom-map-', dir=ROOT / 'build') as tmp:
        path = Path(tmp) / 'test.c'
        path.write_text(source, encoding='utf-8')
        exe = Path(tmp) / 'test.exe'
        subprocess.run([args.cc, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-iquote', str(ROOT / 'include'), str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
