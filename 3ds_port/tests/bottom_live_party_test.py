#!/usr/bin/env python3
"""Compile the real live-party cursor and handoff functions against mock menus.

This test verifies that aborted PLAN_START requests never tap another party
selector, and that removed Pokémon cannot strand an ACTIONS selection.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
plans = (ROOT / "3ds_port/src/3ds_bottom_plans.inc").read_text(encoding="utf-8")
actions = (ROOT / "3ds_port/src/3ds_bottom_actions.inc").read_text(encoding="utf-8")
snapshot = (ROOT / "3ds_port/src/3ds_bottom_snapshot.inc").read_text(encoding="utf-8")


def extract(src: str, name: str, ret: str = "void") -> str:
    start = src.index(f"static {ret} {name}(")
    body = src.index("{", start)
    depth = 0
    for pos in range(body, len(src)):
        if src[pos] == "{":
            depth += 1
        elif src[pos] == "}":
            depth -= 1
            if depth == 0:
                return src[start:pos + 1]
    raise AssertionError(f"Unclosed C function: {name}")


production = "\n\n".join((
    extract(plans, "MakePlan", "Plan"),
    extract(plans, "StartPlan"),
    extract(plans, "CancelPlan"),
    extract(snapshot, "NormalizeLivePartyCursor"),
    extract(actions, "CompleteLivePartyHandoff"),
))

fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef u8 bool8;
#define TRUE 1
#define FALSE 0
#define PARTY_SIZE 6
#define MON_DATA_SPECIES 1
#define SPECIES_NONE 0
#define START_POKEMON 1
#define START_BAG 2
#define START_NONE 255
#define MODE_FIELD 1
#define MODE_PARTY_MENU 2
#define MODE_BAG_MENU 3
#define SCR_POKEMON 2
#define PARTY_MENU_TYPE_FIELD 0
#define PARTY_MENU_TYPE_SCRIPT 1
#define CTR_PARTY_VIEW_LIST 1

enum { PLAN_NONE, PLAN_PRESS, PLAN_START };
typedef struct {
    u8 kind, steps, wait, tries;
    bool8 release;
    s16 target;
    u16 keys;
} Plan;
static Plan sPlan;
static u8 sLivePartyCursor;
static u8 sLivePartyHandoff = PARTY_SIZE;
static u16 sLivePartyHandoffFrames;
static u8 sScreen = SCR_POKEMON;
static u8 gPlayerPartyCount;
static struct Pokemon { u16 species; } gPlayerParty[PARTY_SIZE];
static struct { bool8 active; } sSession;
static struct { bool8 inBattle; } gMain;
static struct { u8 menuType; } gPartyMenu;
struct CtrPartyView { u8 state; };
static int tapped = -1, tapCount, request = -1;

static u16 GetMonData(struct Pokemon *mon, int what)
{
    assert(what == MON_DATA_SPECIES);
    return mon->species;
}
static bool8 CtrParty_InWindow(void) { return TRUE; }
static bool8 CtrParty_GetView(struct CtrPartyView *view)
{
    view->state = CTR_PARTY_VIEW_LIST;
    return TRUE;
}
static void CtrParty_TapSlot(u8 slot) { tapped = slot; ++tapCount; }
static void CtrStartMenu_Request(u8 target) { request = target; }

/* PRODUCTION_CODE */

int main(void)
{
    for (int i = 0; i < PARTY_SIZE; ++i)
        gPlayerParty[i].species = i + 1;
    gPlayerPartyCount = 5;

    /* Depositing slot six must reset the stored cursor, not merely paint the
     * first entry as highlighted while ACTIONS remains invalid. */
    sLivePartyCursor = 5;
    NormalizeLivePartyCursor();
    assert(sLivePartyCursor == 0);
    sLivePartyCursor = 2;
    NormalizeLivePartyCursor();
    assert(sLivePartyCursor == 2);
    gPlayerParty[2].species = SPECIES_NONE;
    NormalizeLivePartyCursor();
    assert(sLivePartyCursor == 0);

    /* Cancel a START plan with a physical key: no delayed synthetic input. */
    sLivePartyHandoff = 3;
    sPlan = MakePlan(PLAN_START, START_POKEMON);
    CancelPlan();
    assert(sPlan.kind == PLAN_NONE && sLivePartyHandoff == PARTY_SIZE);
    assert(request == START_NONE);

    /* Any newly started action also revokes an earlier handoff. */
    sLivePartyHandoff = 2;
    StartPlan(PLAN_START, START_BAG);
    assert(sLivePartyHandoff == PARTY_SIZE && sPlan.target == START_BAG);

    /* A script's chooser may use the same native party adapter but must
     * never take the old field selection. */
    sSession.active = TRUE;
    sPlan = MakePlan(PLAN_NONE, 0);
    gPartyMenu.menuType = PARTY_MENU_TYPE_SCRIPT;
    sLivePartyHandoff = 3;
    CompleteLivePartyHandoff(MODE_PARTY_MENU);
    assert(sLivePartyHandoff == PARTY_SIZE && tapCount == 0);

    /* Even an ordinary field-party menu is not ours without its session. */
    sSession.active = FALSE;
    gPartyMenu.menuType = PARTY_MENU_TYPE_FIELD;
    sLivePartyHandoff = 3;
    CompleteLivePartyHandoff(MODE_PARTY_MENU);
    assert(sLivePartyHandoff == PARTY_SIZE && tapCount == 0);

    /* A correctly opened field party gets precisely one tap. */
    sSession.active = TRUE;
    sLivePartyHandoff = 3;
    CompleteLivePartyHandoff(MODE_PARTY_MENU);
    assert(tapped == 3 && tapCount == 1);
    assert(sLivePartyHandoff == PARTY_SIZE);
    CompleteLivePartyHandoff(MODE_PARTY_MENU);
    assert(tapCount == 1);

    /* Battle, alternate menu and timeout paths never leak the input. */
    sLivePartyHandoff = 2;
    CompleteLivePartyHandoff(MODE_BAG_MENU);
    assert(sLivePartyHandoff == PARTY_SIZE);
    sLivePartyHandoff = 2;
    gMain.inBattle = TRUE;
    CompleteLivePartyHandoff(MODE_PARTY_MENU);
    assert(sLivePartyHandoff == PARTY_SIZE && tapCount == 1);
    gMain.inBattle = FALSE;

    puts("live Party cursor and handoff fixture passed");
    return 0;
}
'''
source = fixture.replace("/* PRODUCTION_CODE */", production)
assert source != fixture
with tempfile.TemporaryDirectory() as tmp:
    inp = Path(tmp) / "live_party.c"
    out = Path(tmp) / "live_party"
    inp.write_text(source, encoding="utf-8")
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(inp), "-o", str(out)], check=True)
    subprocess.run([str(out)], check=True)
