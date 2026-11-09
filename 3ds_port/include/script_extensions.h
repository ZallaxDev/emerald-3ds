#ifndef GUARD_SCRIPT_EXTENSIONS_H
#define GUARD_SCRIPT_EXTENSIONS_H

// global.h must come first: script.h uses u8/u16/u32/bool8 without defining
// them, and upstream includes script.h only after global.h.
#include "global.h"
#include "script.h"

// ============================================================
// 1. Command Registry — 2-byte prefix opcode scheme
// ============================================================

// Stock commands: 0x00–0xEF (upstream max 0xE5)
// Extension single-byte: 0xF0–0xFE (15 slots)
// Extension prefix: 0xFF + extended byte 0x00–0xFF (256 slots)
// Total extension capacity: 271 opcodes
#define SCRIPT_CMD_EXT_SINGLE_BASE 0xF0
#define SCRIPT_CMD_EXT_SINGLE_COUNT 15
#define SCRIPT_CMD_EXT_PREFIX 0xFF
#define SCRIPT_CMD_EXT_EXTENDED_BASE 0x100  // logical: 0xFF 0x00 = 0x100
#define SCRIPT_CMD_EXT_EXTENDED_COUNT 256

// ScriptExt_DispatchCommand results. A command returns TRUE to yield for this
// frame and FALSE to keep executing; the dispatcher preserves that distinction
// instead of collapsing it into a single bool.
#define SCRIPT_EXT_UNHANDLED 0 // no handler: the caller must stop the script
#define SCRIPT_EXT_CONTINUE 1  // handled; execute the next command
#define SCRIPT_EXT_YIELD 2     // handled; return to the caller for this frame

// Register a custom single-byte command (0xF0–0xFE). Returns opcode or 0xFF.
u8 ScriptExt_RegisterCommand(ScrCmdFunc func, const char *name);

// Register an extended command (0x100–0x1FF). Returns logical opcode or 0xFFFF.
u16 ScriptExt_RegisterExtendedCommand(ScrCmdFunc func, const char *name);

// Lookup opcode by name (for script compiler integration)
u16 ScriptExt_GetCommandOpcode(const char *name);

// Called by script.c:RunScriptCommand after the stock table lookup fails.
// `opcode` is either 0xF0–0xFE or 0x100–0x1FF (the 0xFF prefix is already
// consumed by the caller).
u8 ScriptExt_DispatchCommand(struct ScriptContext *ctx, u16 opcode);

// ============================================================
// 2. Specials Registry — Runtime base (survives upstream additions)
// ============================================================

// Special function signature (matches stock: u16 (*)(void))
typedef u16 (*ScriptExtSpecialFunc)(void);

// Upstream special count, emitted from data/specials.inc at build time.
// Returns 0xFFFF if the script bundle has not been loaded yet, so no stock
// special is ever mistaken for an extension.
u16 ScriptExt_GetUpstreamSpecialCount(void);

// Register a special. Returns assigned global index or 0xFFFF on failure.
u16 ScriptExt_RegisterSpecial(ScriptExtSpecialFunc func, const char *name);

// Lookup by name
u16 ScriptExt_GetSpecialIndex(const char *name);

// Is this extension slot registered?
bool8 ScriptExt_IsSpecialRegistered(u16 extIndex);

// Called by scrcmd.c:ScrCmd_special/specialvar for extended dispatch.
// `extIndex` is the extension slot (global index - upstream count).
// `special` ignores the return value (the special sets gSpecialVar_Result,
// like stock specials); `specialvar` assigns it to the target var.
u16 ScriptExt_CallSpecial(u16 extIndex);

// ============================================================
// 3. Standard Script Registry
// ============================================================

// Register a standard script (bytecode pointer). Returns the global index
// (upstream gStdScripts count + extension slot) or 0xFF.
u8 ScriptExt_RegisterStdScript(const u8 *scriptPtr, const char *name);

// Number of scripts upstream gStdScripts already holds.
u16 ScriptExt_GetUpstreamStdScriptCount(void);

// Fetch an extension script by its global index.
const u8 *ScriptExt_GetStdScript(u8 index);

// ============================================================
// 4. Map Script Tag Extensions
// ============================================================

// Reserve tag range 0x80–0xFF for extensions (128 tags)
#define MAP_SCRIPT_EXT_TAG_BASE 0x80

// Custom map script runner — called from MapHeaderRunScriptType.
// Lifecycle callbacks live at MAP_SCRIPT_EXT_TAG_BASE + the stock tag
// (e.g. 0x81 = ON_TRANSITION); semantic tags (territory, milestone, ...)
// use the rest of the range and are triggered by feature code.
// All handlers registered for `tag` run, in registration order.
void ScriptExt_RunMapScript(u8 tag);

// Register a handler for an extended map script tag (0x80–0xFF).
void ScriptExt_RegisterMapScriptHandler(u8 tag, void (*handler)(void));

// ============================================================
// 5. Battle Callback Hooks
// ============================================================

typedef enum {
    BATTLE_HOOK_PRE_WILD,
    BATTLE_HOOK_POST_WILD,
    BATTLE_HOOK_PRE_TRAINER,
    BATTLE_HOOK_POST_TRAINER,
    BATTLE_HOOK_GIVE_STARTER,
    BATTLE_HOOK_MAX
} BattleHookType;

typedef void (*BattleHookFunc)(void);

void BattleHook_Register(BattleHookType type, BattleHookFunc func);
void BattleHook_Call(BattleHookType type);

// ============================================================
// 6. Global Frame Callbacks
// ============================================================

typedef void (*FrameCallbackFunc)(void);

void FrameCallback_Register(FrameCallbackFunc func);
void FrameCallback_Unregister(FrameCallbackFunc func);

// Called from field_control_avatar.c after TryRunOnFrameMapScript()
void FrameCallback_RunAll(void);

// ============================================================
// 7. RAM Script Extensions (Namespaced)
// ============================================================

// Extended RAM script with namespace (feature ID) to avoid collisions
typedef struct {
    u16 featureId;      // e.g., 0x4C52 = "LR" for Living Rivals
    u16 scriptSize;
    u8  scriptData[1024];
} ExtendedRamScript;

bool8 ScriptExt_InitRamScript(u16 featureId, const u8 *script, u16 size);
const u8 *ScriptExt_GetRamScript(u16 featureId);
void ScriptExt_ClearRamScript(u16 featureId);

// ============================================================
// 8. Initialization
// ============================================================

// Clears every registry. For host tests and a fresh game boot.
void ScriptExt_Reset(void);

// Call once at game startup (from CtrGame_Init or AgbMain).
void ScriptExt_Init(void);

#endif // GUARD_SCRIPT_EXTENSIONS_H
