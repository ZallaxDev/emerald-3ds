#ifndef GUARD_PORT_LOG_H
#define GUARD_PORT_LOG_H

/*
 * Logging macro for game translation units: printf semantics, routed to the
 * 3DS logger (SD file + optional overlay).
 * Must not include any SDK header: it is reachable from game translation units.
 */

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char g_PortLogActive;

void Port_Log_Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define PORT_LOG(...) \
    do { if (g_PortLogActive) Port_Log_Printf(__VA_ARGS__); } while (0)

#ifdef __cplusplus
}
#endif

#endif
