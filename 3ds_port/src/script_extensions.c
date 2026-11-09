#include "global.h"
#include "script_extensions.h"

#include <string.h>

// ============================================================
// 1. Command Registry
// ============================================================

static ScrCmdFunc sExtCmdTable[SCRIPT_CMD_EXT_SINGLE_COUNT] = {0};
static const char *sExtCmdNames[SCRIPT_CMD_EXT_SINGLE_COUNT] = {0};
static u8 sExtCmdCount = 0;

static ScrCmdFunc sExtCmdTableExtended[SCRIPT_CMD_EXT_EXTENDED_COUNT] = {0};
static const char *sExtCmdNamesExtended[SCRIPT_CMD_EXT_EXTENDED_COUNT] = {0};
static u16 sExtCmdCountExtended = 0;

u8 ScriptExt_RegisterCommand(ScrCmdFunc func, const char *name)
{
    if (func == NULL || sExtCmdCount >= SCRIPT_CMD_EXT_SINGLE_COUNT)
        return 0xFF;

    u8 opcode = SCRIPT_CMD_EXT_SINGLE_BASE + sExtCmdCount;
    sExtCmdTable[sExtCmdCount] = func;
    sExtCmdNames[sExtCmdCount] = name;
    sExtCmdCount++;
    return opcode;
}

u16 ScriptExt_RegisterExtendedCommand(ScrCmdFunc func, const char *name)
{
    if (func == NULL || sExtCmdCountExtended >= SCRIPT_CMD_EXT_EXTENDED_COUNT)
        return 0xFFFF;

    u16 opcode = SCRIPT_CMD_EXT_EXTENDED_BASE + sExtCmdCountExtended;
    sExtCmdTableExtended[sExtCmdCountExtended] = func;
    sExtCmdNamesExtended[sExtCmdCountExtended] = name;
    sExtCmdCountExtended++;
    return opcode;
}

u16 ScriptExt_GetCommandOpcode(const char *name)
{
    if (name == NULL)
        return 0xFFFF;

    for (u8 i = 0; i < sExtCmdCount; i++)
    {
        if (sExtCmdNames[i] != NULL && strcmp(sExtCmdNames[i], name) == 0)
            return SCRIPT_CMD_EXT_SINGLE_BASE + i;
    }
    for (u16 i = 0; i < sExtCmdCountExtended; i++)
    {
        if (sExtCmdNamesExtended[i] != NULL && strcmp(sExtCmdNamesExtended[i], name) == 0)
            return SCRIPT_CMD_EXT_EXTENDED_BASE + i;
    }
    return 0xFFFF;
}

u8 ScriptExt_DispatchCommand(struct ScriptContext *ctx, u16 opcode)
{
    ScrCmdFunc func = NULL;

    if (opcode >= SCRIPT_CMD_EXT_SINGLE_BASE
        && opcode < SCRIPT_CMD_EXT_SINGLE_BASE + SCRIPT_CMD_EXT_SINGLE_COUNT)
    {
        func = sExtCmdTable[opcode - SCRIPT_CMD_EXT_SINGLE_BASE];
    }
    else if (opcode >= SCRIPT_CMD_EXT_EXTENDED_BASE
        && opcode < SCRIPT_CMD_EXT_EXTENDED_BASE + SCRIPT_CMD_EXT_EXTENDED_COUNT)
    {
        func = sExtCmdTableExtended[opcode - SCRIPT_CMD_EXT_EXTENDED_BASE];
    }

    if (func == NULL)
        return SCRIPT_EXT_UNHANDLED;

    return func(ctx) == TRUE ? SCRIPT_EXT_YIELD : SCRIPT_EXT_CONTINUE;
}

// ============================================================
// 2. Specials Registry
// ============================================================

#define MAX_EXT_SPECIALS 256
static ScriptExtSpecialFunc sExtSpecials[MAX_EXT_SPECIALS] = {0};
static const char *sExtSpecialNames[MAX_EXT_SPECIALS] = {0};
static u16 sExtSpecialCount = 0;

// Emitted from data/specials.inc by the assembler at build time (see
// patches/script_extensions.patch); never a hardcoded C literal.
extern const u16 gUpstreamSpecialCount;

u16 ScriptExt_GetUpstreamSpecialCount(void)
{
    // 0 means the script bundle has not been loaded; treating that as 0xFFFF
    // keeps every stock special on the stock path instead of sending them all
    // through the extension dispatcher.
    return gUpstreamSpecialCount != 0 ? gUpstreamSpecialCount : 0xFFFF;
}

u16 ScriptExt_RegisterSpecial(ScriptExtSpecialFunc func, const char *name)
{
    u16 base = ScriptExt_GetUpstreamSpecialCount();

    if (func == NULL || base == 0xFFFF || sExtSpecialCount >= MAX_EXT_SPECIALS)
        return 0xFFFF;

    u16 index = base + sExtSpecialCount;
    sExtSpecials[sExtSpecialCount] = func;
    sExtSpecialNames[sExtSpecialCount] = name;
    sExtSpecialCount++;
    return index;
}

u16 ScriptExt_GetSpecialIndex(const char *name)
{
    u16 base = ScriptExt_GetUpstreamSpecialCount();

    if (name == NULL || base == 0xFFFF)
        return 0xFFFF;

    for (u16 i = 0; i < sExtSpecialCount; i++)
    {
        if (sExtSpecialNames[i] != NULL && strcmp(sExtSpecialNames[i], name) == 0)
            return base + i;
    }
    return 0xFFFF;
}

u16 ScriptExt_CallSpecial(u16 extIndex)
{
    if (extIndex < sExtSpecialCount && sExtSpecials[extIndex] != NULL)
        return sExtSpecials[extIndex]();

    return 0;
}

bool8 ScriptExt_IsSpecialRegistered(u16 extIndex)
{
    return extIndex < sExtSpecialCount && sExtSpecials[extIndex] != NULL;
}

// ============================================================
// 3. Standard Script Registry
// ============================================================

#define MAX_EXT_STD_SCRIPTS 32
static const u8 *sExtStdScripts[MAX_EXT_STD_SCRIPTS] = {0};
static const char *sExtStdScriptNames[MAX_EXT_STD_SCRIPTS] = {0};
static u8 sExtStdScriptCount = 0;

// The script bundle defines this from data/event_scripts.s.
extern const u16 gUpstreamStdScriptCount;

u16 ScriptExt_GetUpstreamStdScriptCount(void)
{
    // 0 means the script bundle has not been loaded yet; fail registration
    // rather than colliding with the stock indices.
    return gUpstreamStdScriptCount != 0 ? gUpstreamStdScriptCount : 0xFFFF;
}

u8 ScriptExt_RegisterStdScript(const u8 *scriptPtr, const char *name)
{
    u32 index;
    u16 base = ScriptExt_GetUpstreamStdScriptCount();

    if (scriptPtr == NULL || base == 0xFFFF || sExtStdScriptCount >= MAX_EXT_STD_SCRIPTS)
        return 0xFF;

    index = (u32)base + sExtStdScriptCount;
    if (index > 0xFE)
        return 0xFF;

    sExtStdScripts[sExtStdScriptCount] = scriptPtr;
    sExtStdScriptNames[sExtStdScriptCount] = name;
    sExtStdScriptCount++;
    return (u8)index;
}

const u8 *ScriptExt_GetStdScript(u8 index)
{
    u16 base = ScriptExt_GetUpstreamStdScriptCount();

    if (base == 0xFFFF || index < base)
        return NULL;

    index -= base;
    if (index < sExtStdScriptCount)
        return sExtStdScripts[index];

    return NULL;
}

// ============================================================
// 4. Map Script Tag Extensions
// ============================================================

#define MAX_MAP_SCRIPT_HANDLERS 32
static void (*sMapScriptHandlers[MAX_MAP_SCRIPT_HANDLERS])(void) = {0};
static u8 sMapScriptHandlerTags[MAX_MAP_SCRIPT_HANDLERS] = {0};
static u8 sMapScriptHandlerCount = 0;

void ScriptExt_RunMapScript(u8 tag)
{
    for (u8 i = 0; i < sMapScriptHandlerCount; i++)
    {
        if (sMapScriptHandlerTags[i] == tag && sMapScriptHandlers[i] != NULL)
            sMapScriptHandlers[i]();
    }
}

void ScriptExt_RegisterMapScriptHandler(u8 tag, void (*handler)(void))
{
    if (handler == NULL
        || tag < MAP_SCRIPT_EXT_TAG_BASE
        || sMapScriptHandlerCount >= MAX_MAP_SCRIPT_HANDLERS)
    {
        return;
    }

    sMapScriptHandlerTags[sMapScriptHandlerCount] = tag;
    sMapScriptHandlers[sMapScriptHandlerCount] = handler;
    sMapScriptHandlerCount++;
}

// ============================================================
// 5. Battle Callback Hooks
// ============================================================

#define MAX_BATTLE_HOOKS 8
static BattleHookFunc sBattleHooks[BATTLE_HOOK_MAX][MAX_BATTLE_HOOKS] = {{0}};
static u8 sBattleHookCounts[BATTLE_HOOK_MAX] = {0};

void BattleHook_Register(BattleHookType type, BattleHookFunc func)
{
    if (func == NULL || (int)type < 0 || type >= BATTLE_HOOK_MAX)
        return;

    u8 count = sBattleHookCounts[type];

    for (u8 i = 0; i < count; i++)
    {
        if (sBattleHooks[type][i] == func)
            return;
    }

    if (count >= MAX_BATTLE_HOOKS)
        return;

    sBattleHooks[type][count] = func;
    sBattleHookCounts[type] = count + 1;
}

void BattleHook_Call(BattleHookType type)
{
    if ((int)type < 0 || type >= BATTLE_HOOK_MAX)
        return;

    for (u8 i = 0; i < sBattleHookCounts[type]; i++)
    {
        if (sBattleHooks[type][i] != NULL)
            sBattleHooks[type][i]();
    }
}

// ============================================================
// 6. Global Frame Callbacks
// ============================================================

#define MAX_FRAME_CALLBACKS 16
static FrameCallbackFunc sFrameCallbacks[MAX_FRAME_CALLBACKS] = {0};
static u8 sFrameCallbackCount = 0;

void FrameCallback_Register(FrameCallbackFunc func)
{
    if (func == NULL || sFrameCallbackCount >= MAX_FRAME_CALLBACKS)
        return;

    // Avoid duplicates
    for (u8 i = 0; i < sFrameCallbackCount; i++)
    {
        if (sFrameCallbacks[i] == func)
            return;
    }

    sFrameCallbacks[sFrameCallbackCount] = func;
    sFrameCallbackCount++;
}

void FrameCallback_Unregister(FrameCallbackFunc func)
{
    for (u8 i = 0; i < sFrameCallbackCount; i++)
    {
        if (sFrameCallbacks[i] == func)
        {
            // Shift remaining down
            for (u8 j = i; j < sFrameCallbackCount - 1; j++)
                sFrameCallbacks[j] = sFrameCallbacks[j + 1];

            sFrameCallbackCount--;
            sFrameCallbacks[sFrameCallbackCount] = NULL;
            return;
        }
    }
}

void FrameCallback_RunAll(void)
{
    // A callback may unregister itself; it shifts the list down, so only
    // advance when the slot still holds the callback we just ran.
    for (u8 i = 0; i < sFrameCallbackCount; )
    {
        FrameCallbackFunc func = sFrameCallbacks[i];

        if (func == NULL)
        {
            i++;
            continue;
        }

        func();

        if (i < sFrameCallbackCount && sFrameCallbacks[i] == func)
            i++;
    }
}

// ============================================================
// 7. RAM Script Extensions (Namespaced)
// ============================================================

#define MAX_EXT_RAM_SCRIPTS 8
static ExtendedRamScript sExtRamScripts[MAX_EXT_RAM_SCRIPTS] = {0};
static u8 sExtRamScriptCount = 0;

bool8 ScriptExt_InitRamScript(u16 featureId, const u8 *script, u16 size)
{
    u8 slot = 0xFF;

    if (script == NULL || size > sizeof(sExtRamScripts[0].scriptData))
        return FALSE;

    for (u8 i = 0; i < sExtRamScriptCount; i++)
    {
        if (sExtRamScripts[i].featureId == featureId)
        {
            slot = i;
            break;
        }
    }

    if (slot == 0xFF)
    {
        if (sExtRamScriptCount >= MAX_EXT_RAM_SCRIPTS)
            return FALSE;

        slot = sExtRamScriptCount;
        sExtRamScriptCount++;
        sExtRamScripts[slot].featureId = featureId;
    }

    sExtRamScripts[slot].scriptSize = size;
    memcpy(sExtRamScripts[slot].scriptData, script, size);
    return TRUE;
}

const u8 *ScriptExt_GetRamScript(u16 featureId)
{
    for (u8 i = 0; i < sExtRamScriptCount; i++)
    {
        if (sExtRamScripts[i].featureId == featureId && sExtRamScripts[i].scriptSize != 0)
            return sExtRamScripts[i].scriptData;
    }

    return NULL;
}

void ScriptExt_ClearRamScript(u16 featureId)
{
    for (u8 i = 0; i < sExtRamScriptCount; i++)
    {
        if (sExtRamScripts[i].featureId == featureId)
        {
            for (u8 j = i; j < sExtRamScriptCount - 1; j++)
                sExtRamScripts[j] = sExtRamScripts[j + 1];

            sExtRamScriptCount--;
            memset(&sExtRamScripts[sExtRamScriptCount], 0, sizeof(sExtRamScripts[0]));
            return;
        }
    }
}

// ============================================================
// 8. Initialization
// ============================================================

void ScriptExt_Reset(void)
{
    memset(sExtCmdTable, 0, sizeof(sExtCmdTable));
    memset(sExtCmdNames, 0, sizeof(sExtCmdNames));
    sExtCmdCount = 0;

    memset(sExtCmdTableExtended, 0, sizeof(sExtCmdTableExtended));
    memset(sExtCmdNamesExtended, 0, sizeof(sExtCmdNamesExtended));
    sExtCmdCountExtended = 0;

    memset(sExtSpecials, 0, sizeof(sExtSpecials));
    memset(sExtSpecialNames, 0, sizeof(sExtSpecialNames));
    sExtSpecialCount = 0;

    memset(sExtStdScripts, 0, sizeof(sExtStdScripts));
    memset(sExtStdScriptNames, 0, sizeof(sExtStdScriptNames));
    sExtStdScriptCount = 0;

    memset(sMapScriptHandlers, 0, sizeof(sMapScriptHandlers));
    memset(sMapScriptHandlerTags, 0, sizeof(sMapScriptHandlerTags));
    sMapScriptHandlerCount = 0;

    memset(sBattleHooks, 0, sizeof(sBattleHooks));
    memset(sBattleHookCounts, 0, sizeof(sBattleHookCounts));

    memset(sFrameCallbacks, 0, sizeof(sFrameCallbacks));
    sFrameCallbackCount = 0;

    memset(sExtRamScripts, 0, sizeof(sExtRamScripts));
    sExtRamScriptCount = 0;
}

void ScriptExt_Init(void)
{
    // All registries live in BSS; feature init functions register afterwards
    // (e.g. LivingRival_RegisterSpecials(), CombatRework_RegisterCommands()).
    ScriptExt_Reset();
}
