/*
 * Part of 3ds_video.c. Test builds only: BATTLE_TOGGLES, and the bits of
 * nocache.txt (CtrDumpOff) that the later units ask.
 */

#ifdef CTR_BATTLE_TOGGLES
/*
 * Console test builds (make BATTLE_TOGGLES=1): the compositor's nocache bits
 * turned off battle by battle, each battle's mask in the log and in perf.bin,
 * to compare one item on and off battle against battle on the console: a lost
 * refresh couples the frames after it, so frames cannot be compared one by one.
 * battletoggles.txt on the card lists the masks, one a line, taken in turn
 * from the first battle on, starting over at the end; a line frame=BITS has
 * those bits off on every other frame of every battle too, for what does not
 * change the picture and is measured in the queue. No file: every item of
 * the heavy battles' plan in turn, none off first.
 */
#define TOGGLES_MAX 16
static uint32_t sToggleList[TOGGLES_MAX] = {0u, 8388608u, 16777216u, 33554432u, 67108864u};
static unsigned sToggleCount = 5, sToggleBattles;
static uint32_t sToggleEveryOther, sToggleBattle, sToggleNow;

static void BattleTogglesRead(void)
{
    FILE *file = fopen("sdmc:/3ds/emerald3ds/battletoggles.txt", "r");
    char line[32];
    unsigned count = 0;

    if (file == NULL) return;
    while (fgets(line, sizeof(line), file))
    {
        unsigned long value;

        if (sscanf(line, "frame=%lu", &value) == 1) sToggleEveryOther = value;
        else if (sscanf(line, "%lu", &value) == 1 && count < TOGGLES_MAX) sToggleList[count++] = value;
    }
    fclose(file);
    if (count != 0) sToggleCount = count;
}

/* At the frame's start, the mask of its battle: one for the whole of it, its
 * transition too. Off a battle nothing is off. */
static void BattleTogglesFrame(void)
{
    static bool read, was;

    if (!read)
    {
        read = true;
        BattleTogglesRead();
    }
    if (sInBattle && !was)
    {
        sToggleBattle = sToggleList[sToggleBattles % sToggleCount];
        CtrLog_Write(CTR_LOG_VIDEO, "BATTLE toggles: battle %u at frame %lu, nocache bits off %lu, every other "
                     "frame %lu", sToggleBattles, (unsigned long)sStats.frames, (unsigned long)sToggleBattle,
                     (unsigned long)sToggleEveryOther);
        ++sToggleBattles;
    }
    was = sInBattle;
    sToggleNow = !sInBattle ? 0 : sToggleBattle | ((sStats.frames & 1) ? sToggleEveryOther : 0);
    sStats.toggles = sToggleNow;
}
#endif

#ifdef CTR_AUDIT_BITS
/* Audit builds: nocache.txt on the card turns the kept scene, its edges and
 * the whole frames composed at once off, for frames to compare against. */
static unsigned CtrDumpOff(void)
{
    static int read;
    static unsigned off;

    if (!read)
    {
        FILE *file = fopen("sdmc:/3ds/emerald3ds/nocache.txt", "r");

        read = 1;
        if (file)
        {
            if (fscanf(file, "%u", &off) != 1) off = 15;
            fclose(file);
        }
    }
#ifdef CTR_BATTLE_TOGGLES
    return off | sToggleNow;
#else
    return off;
#endif
}
#define CtrDumpNoCache() (CtrDumpOff() & 1)

/* For the voxel field's own (3ds_video.h): its bits. */
unsigned CtrVideo_DumpOff(void)
{
    return CtrDumpOff();
}
#endif
