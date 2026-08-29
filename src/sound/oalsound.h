#ifndef __OALSOUND_H__
#define __OALSOUND_H__

#ifdef OAL_LIFECYCLE_TEST
#include "oalsound_test_support.h"
#else
#include "i_sound.h"
#endif

#include <vector>

class OpenALSoundRenderer;

enum OpenALEndReason
{
	OALEND_None,
	OALEND_Natural,
	OALEND_ExplicitStop,
	OALEND_PoolEviction,
	OALEND_BackendError
};

enum OpenALFinalizeState
{
	OALFINAL_Active,
	OALFINAL_Pending,
	OALFINAL_Finalizing,
	OALFINAL_Finalized
};

class OpenALSound
{
public:
	OpenALSound ();

	unsigned int Buffer2D;
	unsigned int BufferMono;
	unsigned int SampleRate;
	unsigned int Frames;
	unsigned int Channels;
	bool HasLoop;
	unsigned int LoopStart;
	unsigned int LoopEnd;
	unsigned int References;
	bool DeferredDelete;
};

class OpenALChannel
{
public:
	OpenALChannel ();

	unsigned int Source;
	OpenALSound *Sound;
	FISoundChannel *Owner;
	float Gain;
	float RolloffGain;
	float EffectiveGain;
	float Distance;
	float DistanceScale;
	float Pitch;
	int Priority;
	unsigned int CachedPosition;
	unsigned long long LogicalStartFrame;
	unsigned long long AllocationSerial;
	bool Looping;
	bool NoPause;
	bool Is3D;
	bool IsArea;
	bool WasPlayingBeforePause;
	unsigned int PauseReasons;
	FRolloffInfo Rolloff;
	OpenALEndReason EndReason;
	OpenALFinalizeState FinalizeState;
};

class OpenALSoundRenderer : public SoundRenderer
{
public:
	OpenALSoundRenderer ();
	~OpenALSoundRenderer ();

	void SetSfxVolume (float volume);
	void SetMusicVolume (float volume);
	SoundHandle LoadSound (BYTE *sfxdata, int length);
	SoundHandle LoadSoundRaw (BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend = -1);
	void UnloadSound (SoundHandle sfx);
	unsigned int GetMSLength (SoundHandle sfx);
	unsigned int GetSampleLength (SoundHandle sfx);
	float GetOutputRate ();

	SoundStream *CreateStream (SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata);
	SoundStream *OpenStream (const char *filename, int flags, int offset, int length);

	FISoundChannel *StartSound (SoundHandle sfx, float vol, int pitch, int priority, int chanflags, FISoundChannel *reuse_chan);
	FISoundChannel *StartSound3D (SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan);
	void StopChannel (FISoundChannel *chan);
	void ChannelVolume (FISoundChannel *chan, float volume);
	void MarkStartTime (FISoundChannel *chan);
	unsigned int GetPosition (FISoundChannel *chan);
	bool ResolveEvictedPosition (FISoundChannel *chan, unsigned int *position);
	float GetAudibility (FISoundChannel *chan);
	void Sync (bool sync);
	void SetSfxPaused (bool paused, int slot);
	void SetInactive (EInactiveState inactive);
	void UpdateSoundParams3D (SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel);
	void UpdateListener (SoundListener *listener);
	void UpdateSounds ();

	bool IsValid ();
	void PrintStatus ();
	void PrintDriversList ();
	FString GatherStats ();

#ifdef OAL_LIFECYCLE_TEST
public:
#else
private:
#endif
	struct RestartState
	{
		unsigned int Position;
		unsigned long long ClockFrame;
	};

	struct LogicalPosition
	{
		FISoundChannel *Owner;
		OpenALSound *Sound;
		unsigned long long StartClock;
		unsigned long long StartPosition;
		unsigned long long OwnerToken;
		unsigned int SampleRate;
		unsigned int Frames;
		unsigned int LoopStart;
		unsigned int LoopEnd;
		float Pitch;
		bool Looping;
		bool NoPause;
	};

	bool Init ();
	void Shutdown ();
	const char *FindDeviceName (const char *name) const;
	SoundHandle CreateSound (const struct OALPCMData &data);
	FISoundChannel *Start2D (SoundHandle sfx, float volume, int pitch, int flags, int priority, FISoundChannel *reuseChan);
	unsigned int FindFreeSource () const;
	OpenALChannel *FindEvictionCandidate () const;
	bool IncomingWins (const OpenALChannel *candidate, int priority, float effectiveGain) const;
	bool FinalizePendingStopForReuse ();
	void RemoveActiveChannel (OpenALChannel *channel);
	void FinalizeChannel (OpenALChannel *channel, OpenALEndReason reason);
	unsigned int CachePosition (OpenALChannel *channel);
	void ApplyChannelGain (OpenALChannel *channel);
	float CalculateRolloffGain (FRolloffInfo &rolloff, float distanceScale, SoundListener *listener, const FVector3 &position, float *distance) const;
	void ApplySpatialState (OpenALChannel *channel, SoundListener *listener, const FVector3 &position, const FVector3 &velocity);
	void DestroySound (OpenALSound *sound);
	bool IsSourceReserved (unsigned int source) const;
	void AdvanceClocks ();
	bool PrepareRestart (OpenALSound *sound, float pitch, bool looping, bool noPause, FISoundChannel *reuseChan, int flags, RestartState *restart) const;
	OpenALChannel *CreateChannel (unsigned int source, OpenALSound *sound, float volume, float pitch, int priority, int flags);
	unsigned int AcquireSource (int priority, float effectiveGain);
	bool ApplyRestartPosition (OpenALChannel *channel, const RestartState &restart);
	FISoundChannel *PublishChannel (OpenALChannel *channel, FISoundChannel *reuseChan, const RestartState &restart);
	unsigned long long AllocateLogicalPositionToken ();
	const LogicalPosition *FindLogicalPosition (FISoundChannel *owner) const;
	void RememberLogicalPosition (FISoundChannel *owner, OpenALChannel *channel, const RestartState &restart);
	void ForgetLogicalPosition (FISoundChannel *owner);
	bool GetLogicalPosition (FISoundChannel *owner, unsigned int *position) const;
	void InitializePauseState (OpenALChannel *channel);
	void ApplyChannelPauseState (OpenALChannel *channel);
	unsigned long long GetChannelClock (bool noPause) const;

#ifdef OAL_LIFECYCLE_TEST
	void InjectStartFailureForTest ();
#endif

	void *Device;
	void *Context;
	unsigned int *Sources;
	int RequestedSources;
	int AllocatedSources;
	int OutputRate;
	bool InitSuccess;
	FString DeviceName;
	float SfxVolume;
	float MusicVolume;
	unsigned long long NextAllocationSerial;
	unsigned long long NextLogicalPositionToken;
	unsigned long long PausableOutputFrames;
	unsigned long long NonPausableOutputFrames;
	unsigned int PausableFrameRemainder;
	unsigned int NonPausableFrameRemainder;
	unsigned int LastClockMilliseconds;
	unsigned int SfxPaused;
	EInactiveState InactiveState;
	bool SyncPaused;
	bool PendingStartNoPause;
	std::vector<OpenALChannel *> ActiveChannels;
	std::vector<LogicalPosition> LogicalPositions;
	std::vector<unsigned int> RetiringSources;
#ifdef OAL_LIFECYCLE_TEST
	bool FailNextStart;
#endif
};

#endif