"""Exercise production map dirty rectangles against a complete redraw (mock raster primitives)."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from voxel_runtime_test import function, ROOT

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    bottom = (ROOT / 'src/3ds_bottom_ui.c').read_text(encoding='utf-8')
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef int16_t s16; typedef bool bool8;
#define TRUE true
#define FALSE false
#define W 320
#define H 240
#define CW 240
#define CLOCK_X 5
#define CLOCK_Y 5
#define CLOCK_W 230
#define CLOCK_H 15
#define MAP_ORIGIN_X 8
#define MAP_ORIGIN_Y 28
#define MAP_WIN_X0 4
#define MAP_WIN_Y0 28
#define MAP_WIN_X1 236
#define MAP_WIN_Y1 196
#define NAME_X 5
#define NAME_Y 203
#define NAME_W 230
#define NAME_H 32
#define MAPSEC_NONE 255
#define SCR_MAP 0
#define SCR_OPTION 1
#define OPTION_ROWS 7
#define OPT_VOXEL 6
#define HIT_NONE 0
#define HIT_OPTION 20
#define HIT_COLUMN 40
#define COL_ITEMS 4
#define FOCUS_NONE 0
#define MODE_OFF 0
#define MODE_FIELD 1
#define MODE_BATTLE_INFO 10
#define MODE_BATTLE_ACTION 11
#define HIT_QUICK_BALL 60
#define ITEM_NONE 0
#define BAG_VIEW_WHOLE 2
#define CACHE_MAP 0
#define BOX_MENU 0
#define HIT_MAP 0
#define LABEL_FG(x) (x)
#define LABEL_SH(x) (x)
#define CTR_TEXT(english, spanish, french, italian) spanish
#define CTR_TIME_MORNING 0
#define CTR_TIME_NIGHT 2
#define TXT_WHITE 5
#define TXT_DARK 6
#define TXT_LIGHT 7
typedef struct { u8 screen,mode,inBattle,bagView,mapsec,cursorX,cursorY,gender,pickMapsec,pickX,pickY,pressed,enabled;
    u8 mapZoom,mapFly,pickType,pickPos; s16 mapViewX,mapViewY;
    u8 clockShown,clockValid,clockRate,clockPhase,blink,cursor,options[OPTION_ROWS]; u16 clockMinute;
    u8 optPage,optSub,optFocus,focus,extras[OPTION_ROWS],safari; u16 quickBall,quickBallCount; } ViewState;
typedef struct {int x0,y0,x1,y1;} Rect;
static bool RectAddPlate(Rect *rect,int hit) {(void)rect;(void)hit;return false;}
static bool RectAddHit(Rect *rect,int hit) {(void)rect;(void)hit;return false;}
static int BattleCursorHit(const ViewState *state,int cursor) {(void)state;return cursor;}
static u16 sCanvas[W*H], cache[W*H], fb[W*H], expected[W*H], *sCache[1]={cache}, *sDst;
static int sOX, sNormal, sSmall, blits;
static struct {int chosen,plate,chosenShadow;} sLook = {2,3,4};
static int clipX0=0,clipY0=0,clipX1=W,clipY1=H;
static void ClipToMapWindow(int saved[4]) {
    saved[0]=clipX0;saved[1]=clipY0;saved[2]=clipX1;saved[3]=clipY1;
    clipX0=MAP_WIN_X0;clipY0=MAP_WIN_Y0;clipX1=MAP_WIN_X1;clipY1=MAP_WIN_Y1;
}
static void RestoreClip(const int saved[4]) {clipX0=saved[0];clipY0=saved[1];clipX1=saved[2];clipY1=saved[3];}
static struct { const u8 *playerIcon[2], *cursorTiles; struct {u16 c[16];} playerIconPal[2],cursorPal; } sRes;
static bool CtrVideo_BottomInUse(void) {return false;}
static bool CtrVideo_BottomWhole(void) {return false;}
static void ResolveFonts(void) {}
static void Fill(int x,int y,int w,int h,u16 color) {
    for(int i=x;i<x+w;++i) for(int j=y;j<y+h;++j)
        if(i>=clipX0&&i<clipX1&&j>=clipY0&&j<clipY1) sDst[i*H+H-1-j]=color;
}
static void DrawSprite(const u8 *p,int w,int h,int x,int y,const u16 *c) {
    (void)c; Fill(x,y,w*8,h*8,*p);
}
static void GetMapName(u8 *name,u8 map,unsigned pad) {(void)pad;name[0]=map;name[1]=0;}
static void DrawPlate(int x,int y,int width,int height,const int*look,int unused,bool pressed) {
    (void)unused;(void)pressed;Fill(x,y,width,height,*look);
}
static const u8 *Ascii(const char *text) {return (const u8 *)text;}
static void DrawStrIn(const int*font,const u8*text,int x0,int x1,int y0,int y1,int fg,int shadow) {
    (void)font;(void)fg;(void)shadow;
    unsigned color=10;for(unsigned index=0;index<strlen((const char*)text);++index)color+=text[index];
    Fill(x0+2,y0+2,x1-x0-4,y1-y0-4,color);
}
static void AddHit(int a,int b,int c,int d,int e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
/* Zoom, the Fly map and the place panel are drawn only when on: not here. */
static void DrawZoomedMap(const ViewState *s) {(void)s;}
static void DrawFlyIcons(const ViewState *s) {(void)s;}
static void DrawMapPanel(const ViewState *s) {(void)s;}
static void DrawFlyBars(const ViewState *s) {(void)s;}
static void DrawZoomButton(const ViewState *s) {(void)s;}
static void CtrBottom_BlitRect(const u16 *p,int x0,int y0,int x1,int y1) {
    ++blits;
    for(int x=x0;x<x1;++x) for(int y=y0;y<y1;++y) fb[x*H+H-1-y]=p[x*H+H-1-y];
}
'''
    for name in ('static void RectInit(', 'static void RectAdd(', 'static bool8 OptionsAreaOnly(',
                 'static void DrawRegionName(', 'static void DrawMapClock(', 'static void MapToScreen(',
                 'static void DrawCellMark(', 'static void DrawMapMarks(', 'static void DrawRegionMap(',
                 'static bool8 MapCursorOnlyMoved(', 'static void MarkRect(',
                 'static bool8 MoveMapCursor(', 'static bool8 DirtyRect('):
        source += function(bottom, name)
    source += r'''
static void Check(ViewState from,ViewState to,int count) {
    sDst=sCanvas;memcpy(sCanvas,cache,sizeof(cache));DrawRegionMap(&from);memcpy(fb,sCanvas,sizeof(fb));
    sDst=expected;memcpy(expected,cache,sizeof(cache));DrawRegionMap(&to);
    assert(MapCursorOnlyMoved(&to,&from));blits=0;
    assert(MoveMapCursor(&from,&to));assert(blits==count);
    assert(!memcmp(expected,sCanvas,sizeof(expected)));assert(!memcmp(expected,fb,sizeof(expected)));
}
static void CheckFooter(ViewState from,ViewState to) {
    Rect rect;
    sDst=sCanvas;memcpy(sCanvas,cache,sizeof(cache));DrawRegionMap(&from);
    sDst=expected;memcpy(expected,cache,sizeof(cache));DrawRegionMap(&to);
    assert(DirtyRect(&to,&from,&rect));
    assert(rect.x0==CLOCK_X&&rect.y0==CLOCK_Y&&rect.x1==CLOCK_X+CLOCK_W&&rect.y1==CLOCK_Y+CLOCK_H);
    for(int column=rect.x0;column<rect.x1;++column)for(int row=rect.y0;row<rect.y1;++row)
        sCanvas[column*H+H-1-row]=cache[column*H+H-1-row];
    sDst=sCanvas;DrawMapClock(&to);
    assert(!memcmp(expected,sCanvas,sizeof(expected)));
    to.enabled++;assert(!DirtyRect(&to,&from,&rect));
}
int main(void) {
    static const u8 icon=90,cursor=91;
    sRes.playerIcon[0]=&icon;sRes.cursorTiles=&cursor;
    for(unsigned i=0;i<W*H;++i)cache[i]=(u16)(i*13);
    ViewState a={.mode=1,.mapsec=2,.cursorX=9,.cursorY=8,.pickMapsec=MAPSEC_NONE},b=a;
    b.cursorX++;Check(a,b,2);
    b.mapsec=32;Check(a,b,3);
    b.cursorX=a.cursorX;Check(a,b,3);
    a.pickMapsec=b.pickMapsec=6;a.pickX=b.pickX=9;a.pickY=b.pickY=8;Check(a,b,2);
    a.pickMapsec=MAPSEC_NONE;a.clockShown=1;a.clockValid=1;a.clockMinute=1439;a.clockRate=120;a.clockPhase=2;
    b=a;b.cursorX++;Check(a,b,2);b.mapsec=32;Check(a,b,3);
    a.clockValid=0;b=a;b.mapsec=32;Check(a,b,3);
    b=a;b.clockMinute++;assert(!MapCursorOnlyMoved(&b,&a));
    a.clockShown=0;b=a;b.clockShown=1;b.clockValid=1;b.clockRate=120;b.clockMinute=1439;b.clockPhase=2;CheckFooter(a,b);
    a=b;b.clockMinute=0;CheckFooter(a,b);a=b;b.clockRate=1;CheckFooter(a,b);
    a=b;b.clockValid=0;CheckFooter(a,b);a=b;b.clockShown=0;CheckFooter(a,b);
    a.clockShown=1;a.clockValid=1;a.clockPhase=1;a.clockMinute=959;
    b=a;b.clockMinute=960;CheckFooter(a,b);
    b.pressed=1;assert(!MapCursorOnlyMoved(&b,&a));b=a;b.enabled=1;assert(!MapCursorOnlyMoved(&b,&a));
    b=a;b.mode=2;assert(!MapCursorOnlyMoved(&b,&a));b=a;b.mapsec=MAPSEC_NONE;assert(!MapCursorOnlyMoved(&b,&a));
    ViewState optionsFrom={.mode=MODE_FIELD,.screen=SCR_OPTION},optionsTo=optionsFrom;
    Rect optionsRect;
    optionsTo.optPage=1;optionsTo.options[0]=1;
    assert(OptionsAreaOnly(&optionsTo,&optionsFrom));
    assert(DirtyRect(&optionsTo,&optionsFrom,&optionsRect));
    assert(optionsRect.x0==0&&optionsRect.y0==0&&optionsRect.x1==CW&&optionsRect.y1==H);
    optionsTo.pressed=HIT_COLUMN;
    assert(!OptionsAreaOnly(&optionsTo,&optionsFrom));
    puts("PASS bottom map: partial/full canvas and framebuffer equivalence, region changes with clock, invalid clock, picked marker, unrelated state fallback");
}
'''
    with tempfile.TemporaryDirectory(prefix='bottom-map-', dir=ROOT / 'build') as tmp:
        path=Path(tmp)/'test.c';path.write_text(source,encoding='utf-8');exe=Path(tmp)/'test.exe'
        subprocess.run([args.cc,'-std=c99','-O2','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
if __name__ == '__main__':
    main()
