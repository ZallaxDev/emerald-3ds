/* Host test for the script engine extension layer.
 *
 * Build: see `verify-script-extensions` in 3ds_port/Makefile.
 * The decomp headers are replaced by tests/script_extensions_host/ so the
 * registries can be exercised without the GBA toolchain. */

#include <assert.h>
#include <stdio.h>

#include "script_extensions.h"

/* The real build emits this from data/specials.inc through the script bundle
 * (see patches/script_extensions.patch). The test uses a value that cannot be
 * confused with the real 534, so a hardcoded base in the .c would fail here. */
#define TEST_SPECIAL_BASE 700
const u16 gUpstreamSpecialCount = TEST_SPECIAL_BASE;

/* The real build emits this from data/event_scripts.s (the gStdScripts table
 * size). Three upstream standard scripts in this test. */
const u16 gUpstreamStdScriptCount = 3;

static struct ScriptContext sCtx;

/* ---------------------------------------------------------------- commands */

static u16 sCmdContinueCalls;
static u16 sCmdYieldCalls;

static bool8 CmdContinue(struct ScriptContext *ctx)
{
    (void)ctx;
    sCmdContinueCalls++;
    return FALSE;
}

static bool8 CmdYield(struct ScriptContext *ctx)
{
    (void)ctx;
    sCmdYieldCalls++;
    return TRUE;
}

static void test_commands(void)
{
    ScriptExt_Reset();
    sCmdContinueCalls = sCmdYieldCalls = 0;

    assert(ScriptExt_RegisterCommand(CmdContinue, "lr_a") == 0xF0);
    assert(ScriptExt_RegisterCommand(CmdYield, "lr_b") == 0xF1);
    assert(ScriptExt_RegisterCommand(CmdContinue, "lr_c") == 0xF2);
    assert(ScriptExt_GetCommandOpcode("lr_a") == 0xF0);
    assert(ScriptExt_GetCommandOpcode("lr_b") == 0xF1);
    assert(ScriptExt_GetCommandOpcode("missing") == 0xFFFF);

    /* FALSE from a command means "keep executing", TRUE means "yield this
     * frame". The dispatcher must preserve that distinction. */
    assert(ScriptExt_DispatchCommand(&sCtx, 0xF0) == SCRIPT_EXT_CONTINUE);
    assert(sCmdContinueCalls == 1 && sCmdYieldCalls == 0);
    assert(ScriptExt_DispatchCommand(&sCtx, 0xF1) == SCRIPT_EXT_YIELD);
    assert(sCmdContinueCalls == 1 && sCmdYieldCalls == 1);
    assert(ScriptExt_DispatchCommand(&sCtx, 0xF2) == SCRIPT_EXT_CONTINUE);
    assert(sCmdContinueCalls == 2 && sCmdYieldCalls == 1);

    /* An unregistered hole in the single-byte range is not a stock command. */
    assert(ScriptExt_DispatchCommand(&sCtx, 0xF5) == SCRIPT_EXT_UNHANDLED);
    assert(ScriptExt_DispatchCommand(&sCtx, 0xEF) == SCRIPT_EXT_UNHANDLED);
    assert(ScriptExt_DispatchCommand(&sCtx, 0xFF) == SCRIPT_EXT_UNHANDLED);
    assert(ScriptExt_DispatchCommand(&sCtx, 0x1FF) == SCRIPT_EXT_UNHANDLED);
    assert(sCmdContinueCalls == 2 && sCmdYieldCalls == 1);

    for (u16 i = 3; i < SCRIPT_CMD_EXT_SINGLE_COUNT; i++)
        assert(ScriptExt_RegisterCommand(CmdContinue, "filler") == SCRIPT_CMD_EXT_SINGLE_BASE + i);
    assert(ScriptExt_RegisterCommand(CmdContinue, "overflow") == 0xFF);
    assert(ScriptExt_RegisterCommand(NULL, "null") == 0xFF);
    assert(ScriptExt_GetCommandOpcode("null") == 0xFFFF);
}

static void test_extended_commands(void)
{
    ScriptExt_Reset();
    sCmdContinueCalls = sCmdYieldCalls = 0;

    assert(ScriptExt_RegisterExtendedCommand(CmdContinue, "lr_x0") == 0x100);
    assert(ScriptExt_RegisterExtendedCommand(CmdYield, "lr_x1") == 0x101);
    assert(ScriptExt_GetCommandOpcode("lr_x0") == 0x100);
    assert(ScriptExt_GetCommandOpcode("lr_x1") == 0x101);

    assert(ScriptExt_DispatchCommand(&sCtx, 0x100) == SCRIPT_EXT_CONTINUE);
    assert(ScriptExt_DispatchCommand(&sCtx, 0x101) == SCRIPT_EXT_YIELD);
    assert(sCmdContinueCalls == 1 && sCmdYieldCalls == 1);
    assert(ScriptExt_DispatchCommand(&sCtx, 0x1FF) == SCRIPT_EXT_UNHANDLED);

    for (u16 i = 2; i < SCRIPT_CMD_EXT_EXTENDED_COUNT; i++)
        assert(ScriptExt_RegisterExtendedCommand(CmdContinue, "xfiller") == 0x100 + i);
    assert(ScriptExt_RegisterExtendedCommand(CmdContinue, "xoverflow") == 0xFFFF);

    assert(ScriptExt_DispatchCommand(&sCtx, 0x1FF) == SCRIPT_EXT_CONTINUE);
    assert(ScriptExt_DispatchCommand(&sCtx, 0x200) == SCRIPT_EXT_UNHANDLED);
}

/* ---------------------------------------------------------------- specials */

static u16 Spec0(void) { return 1000; }
static u16 Spec1(void) { return 1001; }
static u16 Spec2(void) { return 1002; }
static u16 Spec3(void) { return 1003; }
static u16 Spec4(void) { return 1004; }
static u16 Spec5(void) { return 1005; }
static u16 Spec6(void) { return 1006; }
static u16 Spec7(void) { return 1007; }
static u16 Spec8(void) { return 1008; }
static u16 Spec9(void) { return 1009; }

static void test_specials(void)
{
    static ScriptExtSpecialFunc const funcs[] = {
        Spec0, Spec1, Spec2, Spec3, Spec4, Spec5, Spec6, Spec7, Spec8, Spec9,
    };
    static const char *const names[] = {
        "LRS_A", "LRS_B", "LRS_C", "LRS_D", "LRS_E",
        "LRS_F", "LRS_G", "LRS_H", "LRS_I", "LRS_J",
    };

    ScriptExt_Reset();

    assert(ScriptExt_GetUpstreamSpecialCount() == TEST_SPECIAL_BASE);

    for (u16 i = 0; i < ARRAY_COUNT(funcs); i++)
    {
        assert(ScriptExt_RegisterSpecial(funcs[i], names[i]) == TEST_SPECIAL_BASE + i);
        assert(ScriptExt_GetSpecialIndex(names[i]) == TEST_SPECIAL_BASE + i);
        assert(ScriptExt_IsSpecialRegistered(i));
    }
    assert(!ScriptExt_IsSpecialRegistered(ARRAY_COUNT(funcs)));
    assert(ScriptExt_GetSpecialIndex("LRS_missing") == 0xFFFF);

    /* Scripts see the global index; the dispatcher takes the extension slot. */
    for (u16 i = 0; i < ARRAY_COUNT(funcs); i++)
        assert(ScriptExt_CallSpecial(i) == 1000 + i);
    assert(ScriptExt_CallSpecial(ARRAY_COUNT(funcs)) == 0);
    assert(ScriptExt_CallSpecial(0xFFFF) == 0);
}

/* -------------------------------------------------------------- std scripts */

static const u8 sStdScriptA[] = {0x01, 0x02};
static const u8 sStdScriptB[] = {0x03, 0x04};

static void test_std_scripts(void)
{
    ScriptExt_Reset();

    assert(ScriptExt_GetUpstreamStdScriptCount() == 3);
    assert(ScriptExt_RegisterStdScript(sStdScriptA, "lr_std_a") == 3);
    assert(ScriptExt_RegisterStdScript(sStdScriptB, "lr_std_b") == 4);
    assert(ScriptExt_GetStdScript(0) == NULL); /* upstream slots stay upstream */
    assert(ScriptExt_GetStdScript(2) == NULL);
    assert(ScriptExt_GetStdScript(3) == sStdScriptA);
    assert(ScriptExt_GetStdScript(4) == sStdScriptB);
    assert(ScriptExt_GetStdScript(5) == NULL);

    for (u16 i = 2; i < 32; i++)
        assert(ScriptExt_RegisterStdScript(sStdScriptA, "lr_std_filler") == 3 + i);
    assert(ScriptExt_RegisterStdScript(sStdScriptA, "lr_std_overflow") == 0xFF);
}

/* ---------------------------------------------------------- map script tags */

static u16 sMapRuns80;
static u16 sMapRuns81;

static void MapHandler80A(void) { sMapRuns80++; }
static void MapHandler80B(void) { sMapRuns80 += 10; }
static void MapHandler81(void) { sMapRuns81++; }

static void test_map_scripts(void)
{
    ScriptExt_Reset();
    sMapRuns80 = sMapRuns81 = 0;

    ScriptExt_RegisterMapScriptHandler(0x80, MapHandler80A);
    ScriptExt_RegisterMapScriptHandler(0x80, MapHandler80B);
    ScriptExt_RegisterMapScriptHandler(0x81, MapHandler81);

    ScriptExt_RunMapScript(0x80);
    assert(sMapRuns80 == 11);
    ScriptExt_RunMapScript(0x81);
    assert(sMapRuns81 == 1);
    ScriptExt_RunMapScript(0x82);
    assert(sMapRuns81 == 1);

    /* Tags below the reserved range are not extension tags. */
    ScriptExt_RegisterMapScriptHandler(0x01, MapHandler81);
    ScriptExt_RunMapScript(0x01);
    assert(sMapRuns81 == 1);
}

/* ------------------------------------------------------------- battle hooks */

static u16 sPreWild;
static u16 sPostWild;

static void HookPreWildA(void) { sPreWild++; }
static void HookPreWildB(void) { sPreWild += 10; }
static void HookPostWild(void) { sPostWild++; }

static void test_battle_hooks(void)
{
    ScriptExt_Reset();
    sPreWild = sPostWild = 0;

    BattleHook_Register(BATTLE_HOOK_PRE_WILD, HookPreWildA);
    BattleHook_Register(BATTLE_HOOK_PRE_WILD, HookPreWildB);
    BattleHook_Register(BATTLE_HOOK_PRE_WILD, HookPreWildA); /* duplicate: ignored */
    BattleHook_Register(BATTLE_HOOK_POST_WILD, HookPostWild);

    BattleHook_Call(BATTLE_HOOK_PRE_WILD);
    assert(sPreWild == 11);
    BattleHook_Call(BATTLE_HOOK_POST_WILD);
    assert(sPostWild == 1);
    BattleHook_Call(BATTLE_HOOK_PRE_TRAINER);
    BattleHook_Call(BATTLE_HOOK_MAX);
    assert(sPostWild == 1);
}

/* ---------------------------------------------------------- frame callbacks */

static u16 sFrameA;
static u16 sFrameB;
static u16 sFrameC;
static u16 sFrameOnce;

static void FrameA(void) { sFrameA++; }
static void FrameB(void) { sFrameB++; }
static void FrameC(void) { sFrameC++; }
static void FrameSelfRemoving(void)
{
    sFrameOnce++;
    FrameCallback_Unregister(FrameSelfRemoving);
}

static void test_frame_callbacks(void)
{
    ScriptExt_Reset();
    sFrameA = sFrameB = sFrameC = sFrameOnce = 0;

    FrameCallback_Register(FrameA);
    FrameCallback_Register(FrameB);
    FrameCallback_Register(FrameC);
    FrameCallback_Register(FrameB); /* duplicate: ignored */
    FrameCallback_RunAll();
    assert(sFrameA == 1 && sFrameB == 1 && sFrameC == 1);

    FrameCallback_Unregister(FrameB);
    FrameCallback_RunAll();
    assert(sFrameA == 2 && sFrameB == 1 && sFrameC == 2);

    /* A callback that removes itself must not make RunAll skip its neighbour. */
    FrameCallback_Unregister(FrameA);
    FrameCallback_Unregister(FrameC);
    FrameCallback_Register(FrameSelfRemoving);
    FrameCallback_Register(FrameC);
    FrameCallback_RunAll();
    assert(sFrameOnce == 1);
    assert(sFrameC == 3);
    FrameCallback_RunAll();
    assert(sFrameOnce == 1);
    assert(sFrameC == 4);
}

/* -------------------------------------------------------------- RAM scripts */

static void test_ram_scripts(void)
{
    static const u8 scriptA[] = {0x10, 0x11, 0x12};
    static const u8 scriptB[] = {0x20, 0x21};
    static u8 oversized[1025];

    ScriptExt_Reset();

    assert(ScriptExt_GetRamScript(0x4C52) == NULL);
    assert(ScriptExt_InitRamScript(0x4C52, scriptA, sizeof(scriptA)) == TRUE);
    assert(ScriptExt_InitRamScript(0x4242, scriptB, sizeof(scriptB)) == TRUE);

    const u8 *readA = ScriptExt_GetRamScript(0x4C52);
    const u8 *readB = ScriptExt_GetRamScript(0x4242);
    assert(readA != NULL && readA[0] == 0x10 && readA[2] == 0x12);
    assert(readB != NULL && readB[0] == 0x20 && readB[1] == 0x21);

    assert(ScriptExt_InitRamScript(0x4C52, scriptB, sizeof(scriptB)) == TRUE);
    readA = ScriptExt_GetRamScript(0x4C52);
    assert(readA[0] == 0x20 && readA[1] == 0x21);

    /* An oversize script must fail without consuming a slot. */
    assert(ScriptExt_InitRamScript(0x9999, oversized, sizeof(oversized)) == FALSE);
    assert(ScriptExt_GetRamScript(0x9999) == NULL);

    /* Two slots are in use; six more must still fit (8 total). */
    for (u16 id = 0x5000; id < 0x5006; id++)
        assert(ScriptExt_InitRamScript(id, scriptB, sizeof(scriptB)) == TRUE);
    assert(ScriptExt_InitRamScript(0x5006, scriptB, sizeof(scriptB)) == FALSE);

    ScriptExt_ClearRamScript(0x4C52);
    assert(ScriptExt_GetRamScript(0x4C52) == NULL);
    assert(ScriptExt_GetRamScript(0x4242) != NULL);
    assert(ScriptExt_InitRamScript(0x5006, scriptB, sizeof(scriptB)) == TRUE); /* freed slot */
    ScriptExt_ClearRamScript(0x9999); /* absent: no crash */
}

int main(void)
{
    ScriptExt_Init();

    test_commands();
    test_extended_commands();
    test_specials();
    test_std_scripts();
    test_map_scripts();
    test_battle_hooks();
    test_frame_callbacks();
    test_ram_scripts();

    puts("PASS script_extensions: command/special/std/map/battle/frame/RAM registries, "
         "overflow, out-of-range dispatch, FALSE-continue vs TRUE-yield");
    return 0;
}
