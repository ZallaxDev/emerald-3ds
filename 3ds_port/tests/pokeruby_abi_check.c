/* Compile-time contract for the experimental English Ruby/Sapphire modules.
 * Include the real game headers; no stand-in types or generated assets. */
#include "global.h"
#include "task.h"
#include "random.h"

#if !defined(__arm__) || !defined(__ARM_ARCH_6K__) || defined(__thumb__)
#error "The 3DS game code must use ARM state on ARMv6K"
#endif
#if !defined(__ARM_PCS_VFP)
#error "The 3DS port requires the hard-float calling convention"
#endif
#if !defined(PORTABLE) || !defined(PLATFORM_3DS) || !defined(__3DS__) || !defined(PORT_BRIDGE)
#error "The experimental check requires the 3DS bridge build flags"
#endif
#if !ENGLISH || REVISION != 0 || DEBUG != 0 || MODERN != 1
#error "This milestone checks the English revision-0 modern build only"
#endif
#if defined(RUBY) == defined(SAPPHIRE)
#error "Select exactly one game"
#endif
#ifdef __CHAR_UNSIGNED__
#error "Match the port's signed-char game build"
#endif

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "3DS byte order");
_Static_assert(sizeof(void *) == 4 && sizeof(TaskFunc) == 4, "32-bit pointers");
_Static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4, "game integer widths");
_Static_assert(sizeof(struct Task) == 0x28, "task size used by callback storage");
_Static_assert(offsetof(struct Task, data) == 8, "task data offset");
_Static_assert(sizeof(((struct Task *)0)->data) == 32, "task data slots");
_Static_assert(__builtin_types_compatible_p(__typeof__(&Random), u16 (*)(void)), "RNG return type");
_Static_assert(__builtin_types_compatible_p(__typeof__(&SeedRng), void (*)(u16)), "RNG seed type");
