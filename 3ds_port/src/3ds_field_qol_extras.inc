/*
 * Field quality-of-life extras for the bottom screen's map (3ds_bottom_ui.c,
 * "Map extras"). Lines of gCtrExtras: 3ds_extras.c includes this file inside
 * the table, on ENHANCEMENTS' second screen (beside the clock's lines).
 * All are OFF by default, and with them off the map looks and runs as before.
 *
 *   map_encounters  the name plate under the map toggles the window between
 *                   the map and the current area's wild Pokemon, one page a
 *                   method (grass, surfing, Rock Smash, each rod); a tap on
 *                   the list turns the page. Species not seen are "???".
 *   repel_steps     a Repel's remaining steps on the name plate, read from
 *                   VAR_REPEL_STEP_COUNT (never written).
 *   clock_notice    a banner over the map, dismissed with a tap, when an existing
 *                   save has no usable saved clock and the 3DS clock
 *                   is used instead (3ds_clock_store.h).
 */
{CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 1), CTR_TEXT("AREA WILD LIST", "LISTA SALVAJES", "LISTE SAUVAGES", "ELENCO SELV.", "LISTA SELVAGENS", "WILDE IM GEBIET", "やせいの リスト"), "map_encounters", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
{CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 1), CTR_TEXT("REPEL STEPS", "PASOS REPELENTE", "PAS REPOUSSE", "PASSI REPELL.", "PASSOS REPEL.", "SCHRITTE SCHUTZ", "むしよけの ほすう"), "repel_steps", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
{CTR_EXTRAS_SCREEN(CTR_EXTRAS_ENHANCEMENTS, 1), CTR_TEXT("CLOCK NOTICE", "AVISO RELOJ", "AVIS HORLOGE", "AVVISO OROL.", "AVISO RELÓGIO", "UHR-HINWEIS", "とけいの おしらせ"), "clock_notice", 2, 0, gCtrExtrasOffOn, NULL, NULL, NULL},
