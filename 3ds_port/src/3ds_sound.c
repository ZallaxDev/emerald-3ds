/*
 * The game's sound engine (m4a) on a worker core (CtrAudio_StartWorker).
 *
 * The VBlank handler calls m4aSoundVSync, which hands the last mixed frame to
 * the DSP, then m4aSoundMain, which runs the sequencer and mixes the next
 * frame. Both are routed here by the linker (--wrap, full.mk): with a worker
 * they become one hand-over and the game thread goes on with its next frame
 * while the worker mixes.
 *
 * On the GBA the engine runs inside an interrupt and the game's calls into it
 * are never interrupted by the game: m4a guards its players with a flag that
 * makes the interrupted side skip, not wait. On another core that flag would
 * drop a song start or a cry whenever the two met, so every entry point the
 * game uses holds the worker's lock instead, and is wrapped here too.
 */
#include "global.h"
#include "gba/m4a_internal.h"
#include "port_prof.h"
#include "3ds_audio.h"
#include "3ds_platform.h"

static bool sThreaded, sTried;
/* Runs of the sequencer, counted as each ends (under the sound lock) and read
 * without it (IsPokemonCryPlaying): a run counted is a run whose state the
 * reader sees. */
static u32 sEngineRuns;

void __real_m4aSoundVSync(void);
void __real_m4aSoundMain(void);

static void EngineMain(void)
{
    __real_m4aSoundMain();
    __atomic_add_fetch(&sEngineRuns, 1, __ATOMIC_RELEASE);
}

static void MixFrame(void)
{
    __real_m4aSoundVSync();
    EngineMain();
}

/* A fast-forwarded frame that is not shown leaves the engine alone: one tick
 * per frame on the display keeps music and effects at their speed. */
void __wrap_m4aSoundVSync(void)
{
    if (!sThreaded && CtrPlatform_SoundTick())
        __real_m4aSoundVSync();
}

void __wrap_m4aSoundMain(void)
{
    if (!CtrPlatform_SoundTick())
        return;
    PORT_PROF_BEGIN(mix);
    if (!sTried)
    {
        sTried = true;
        sThreaded = CtrAudio_StartWorker(MixFrame);
    }
    if (sThreaded && !CtrAudio_Kick())
    {
        sThreaded = false;
        __real_m4aSoundVSync();
    }
    if (!sThreaded)
        EngineMain();
    PORT_PROF_END(mix, PORT_PROF_MIX);
}


#define LOCKED_VOID(name, params, args)            \
    void __real_##name params;                     \
    void __wrap_##name params                      \
    {                                              \
        CtrAudio_LockSound();                      \
        __real_##name args;                        \
        CtrAudio_UnlockSound();                    \
    }

LOCKED_VOID(m4aSoundInit, (void), ())
LOCKED_VOID(m4aSoundMode, (u32 mode), (mode))
LOCKED_VOID(m4aSoundVSyncOn, (void), ())
LOCKED_VOID(m4aSoundVSyncOff, (void), ())
LOCKED_VOID(m4aSongNumStart, (u16 n), (n))
LOCKED_VOID(m4aSongNumStartOrChange, (u16 n), (n))
LOCKED_VOID(m4aSongNumStartOrContinue, (u16 n), (n))
LOCKED_VOID(m4aSongNumStop, (u16 n), (n))
LOCKED_VOID(m4aSongNumContinue, (u16 n), (n))
LOCKED_VOID(m4aMPlayAllStop, (void), ())
LOCKED_VOID(m4aMPlayAllContinue, (void), ())
LOCKED_VOID(m4aMPlayStop, (struct MusicPlayerInfo *info), (info))
LOCKED_VOID(m4aMPlayContinue, (struct MusicPlayerInfo *info), (info))
LOCKED_VOID(m4aMPlayFadeOut, (struct MusicPlayerInfo *info, u16 speed), (info, speed))
LOCKED_VOID(m4aMPlayFadeOutTemporarily, (struct MusicPlayerInfo *info, u16 speed), (info, speed))
LOCKED_VOID(m4aMPlayFadeIn, (struct MusicPlayerInfo *info, u16 speed), (info, speed))
LOCKED_VOID(m4aMPlayImmInit, (struct MusicPlayerInfo *info), (info))
LOCKED_VOID(m4aMPlayTempoControl, (struct MusicPlayerInfo *info, u16 tempo), (info, tempo))
LOCKED_VOID(m4aMPlayVolumeControl, (struct MusicPlayerInfo *info, u16 tracks, u16 volume), (info, tracks, volume))
LOCKED_VOID(m4aMPlayPitchControl, (struct MusicPlayerInfo *info, u16 tracks, s16 pitch), (info, tracks, pitch))
LOCKED_VOID(m4aMPlayPanpotControl, (struct MusicPlayerInfo *info, u16 tracks, s8 pan), (info, tracks, pan))
LOCKED_VOID(m4aMPlayModDepthSet, (struct MusicPlayerInfo *info, u16 tracks, u8 depth), (info, tracks, depth))
LOCKED_VOID(m4aMPlayLFOSpeedSet, (struct MusicPlayerInfo *info, u16 tracks, u8 speed), (info, tracks, speed))
LOCKED_VOID(MPlayStart, (struct MusicPlayerInfo *info, struct SongHeader *header), (info, header))
LOCKED_VOID(MPlayContinue, (struct MusicPlayerInfo *info), (info))
LOCKED_VOID(MPlayFadeOut, (struct MusicPlayerInfo *info, u16 speed), (info, speed))
LOCKED_VOID(SetPokemonCryVolume, (u8 value), (value))
LOCKED_VOID(SetPokemonCryPanpot, (s8 value), (value))
LOCKED_VOID(SetPokemonCryPitch, (s16 value), (value))
LOCKED_VOID(SetPokemonCryLength, (u16 value), (value))
LOCKED_VOID(SetPokemonCryRelease, (u8 value), (value))
LOCKED_VOID(SetPokemonCryProgress, (u32 value), (value))
LOCKED_VOID(SetPokemonCryChorus, (s8 value), (value))
LOCKED_VOID(SetPokemonCryStereo, (u32 value), (value))
LOCKED_VOID(SetPokemonCryPriority, (u8 value), (value))
/* main.c's, called from sound.c: it zeroes the cry songs a playing cry's
 * sequencer reads on the worker. */
LOCKED_VOID(ClearPokemonCrySongs, (void), ())
/*
 * A cry is playing once the sequencer has run its note and given it a
 * channel. On the GBA the VBlank after PlayCry has done that before the game
 * asks; here the next frame may come first: a fast-forwarded frame that is not
 * shown leaves the engine alone, and the worker may not have started its
 * frame yet. The game, finding no cry, cleared its song
 * (IsCryPlayingOrClearCrySongs) before it ever sounded: a mon sent out of its
 * ball was silent at x2 and above. Until the sequencer has run, a cry just
 * started counts as playing.
 */
static bool8 sCryPending[MAX_POKEMON_CRIES];
static u32 sCryStartRun[MAX_POKEMON_CRIES];

static int CryIndex(const struct MusicPlayerInfo *info)
{
    int i = info - gPokemonCryMusicPlayers;

    return i >= 0 && i < MAX_POKEMON_CRIES ? i : -1;
}

struct MusicPlayerInfo *__real_SetPokemonCryTone(struct ToneData *tone);
struct MusicPlayerInfo *__wrap_SetPokemonCryTone(struct ToneData *tone)
{
    struct MusicPlayerInfo *info;
    int i;

    CtrAudio_LockSound();
    info = __real_SetPokemonCryTone(tone);
    i = CryIndex(info);
    if (i >= 0)
    {
        sCryPending[i] = TRUE;
        sCryStartRun[i] = sEngineRuns;
    }
    CtrAudio_UnlockSound();
    return info;
}

bool32 __real_IsPokemonCryPlaying(struct MusicPlayerInfo *info);

/* m4a's IsPokemonCryPlaying with each pointer read once: the worker may
 * change them meanwhile, and a second read of track->chan could be NULL. */
static bool32 CryPlayingUnlocked(const struct MusicPlayerInfo *info)
{
    struct MusicPlayerTrack *track = info->tracks;
    struct SoundChannel *chan = __atomic_load_n(&track->chan, __ATOMIC_RELAXED);

    return chan != NULL && __atomic_load_n(&chan->track, __ATOMIC_RELAXED) == track;
}

/*
 * Polled every frame while a cry plays (the BGM ducking task, IsCryPlaying),
 * so first without the sound lock, which the worker holds for its whole mix
 * (1.6-2.6 ms, peaks of 4 ms on an Old 3DS): "playing" is taken as it is
 * read. A read in the middle of a run sees its state before or after it, and
 * "playing" only from a cry that was playing before; "finished", which the
 * game acts on (it clears the cry songs), is read again under the lock, as
 * before. sCryPending and sCryStartRun belong to the game's thread
 * (SetPokemonCryTone and this).
 */
bool32 __wrap_IsPokemonCryPlaying(struct MusicPlayerInfo *info)
{
    bool32 playing;
    int i = CryIndex(info);

    if (i >= 0 && sCryPending[i] && sCryStartRun[i] != __atomic_load_n(&sEngineRuns, __ATOMIC_ACQUIRE))
        sCryPending[i] = FALSE;
    if ((i >= 0 && sCryPending[i]) || CryPlayingUnlocked(info))
        return TRUE;
    CtrAudio_LockSound();
    playing = __real_IsPokemonCryPlaying(info);
    CtrAudio_UnlockSound();
    return playing;
}

/*
 * The music a little quieter while the bottom screen has the player's
 * attention. A track's volume (volX, 64 full) is lowered only from full, and
 * raised back only from the ducked level: the game's own changes (the music
 * lowered under a cry and raised after it) are left as they are, and a song
 * started meanwhile, at full, is lowered in turn.
 */
#define DUCKED_VOLX 32   /* half */
extern struct MusicPlayerInfo gMPlayInfo_BGM;

void CtrAudio_DuckMusic(bool duck)
{
    static bool sDucked;
    struct MusicPlayerTrack *track;
    u8 from = duck ? 64 : DUCKED_VOLX, to = duck ? DUCKED_VOLX : 64;

    if (!duck && !sDucked)
        return;
    sDucked = duck;
    CtrAudio_LockSound();
    if (gMPlayInfo_BGM.ident == ID_NUMBER)
    {
        track = gMPlayInfo_BGM.tracks;
        for (int i = 0; i < gMPlayInfo_BGM.trackCount; ++i, ++track)
            if ((track->flags & MPT_FLG_EXIST) && track->volX == from)
            {
                track->volX = to;
                track->flags |= MPT_FLG_VOLCHG;
            }
    }
    CtrAudio_UnlockSound();
}
