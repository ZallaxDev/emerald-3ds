#ifndef GUARD_SCRIPT_H
#define GUARD_SCRIPT_H

/* Host-test stand-in for the decomp's include/script.h. The extension layer
 * only needs the command signature and an opaque-enough context. */

#include "global.h"

struct ScriptContext;

typedef bool8 (*ScrCmdFunc)(struct ScriptContext *);

struct ScriptContext
{
    u8 mode;
    const u8 *scriptPtr;
    ScrCmdFunc *cmdTable;
    ScrCmdFunc *cmdTableEnd;
};

#endif /* GUARD_SCRIPT_H */
