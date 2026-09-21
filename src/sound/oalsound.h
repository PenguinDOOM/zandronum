#ifndef __OALSOUND_H__
#define __OALSOUND_H__

#ifdef OAL_LIFECYCLE_TEST
#include "oalsound_test_support.h"
#else
#include "i_sound.h"
#endif

#include <vector>

#ifdef OAL_LIFECYCLE_TEST
#define OAL_APIENTRY
typedef int OALsizei;
typedef unsigned int OALuint;
typedef int OALenum;
typedef int OALint;
typedef float OALfloat;
#else
#include <AL/al.h>
typedef ALsizei OALsizei;
typedef ALuint OALuint;
typedef ALenum OALenum;
typedef ALint OALint;
typedef ALfloat OALfloat;
#define OAL_APIENTRY AL_APIENTRY
#endif

#ifndef ALC_MAX_AUXILIARY_SENDS
#define ALC_MAX_AUXILIARY_SENDS 0x20003
#endif

typedef void (OAL_APIENTRY *OALGenEffects) (OALsizei, OALuint *);
typedef void (OAL_APIENTRY *OALDeleteEffects) (OALsizei, const OALuint *);
typedef void (OAL_APIENTRY *OALEffecti) (OALuint, OALenum, OALint);
typedef void (OAL_APIENTRY *OALEffectf) (OALuint, OALenum, OALfloat);
typedef void (OAL_APIENTRY *OALEffectfv) (OALuint, OALenum, const OALfloat *);
typedef void (OAL_APIENTRY *OALGenAuxiliaryEffectSlots) (OALsizei, OALuint *);
typedef void (OAL_APIENTRY *OALDeleteAuxiliaryEffectSlots) (OALsizei, const OALuint *);
typedef void (OAL_APIENTRY *OALAuxiliaryEffectSloti) (OALuint, OALenum, OALint);
typedef void (OAL_APIENTRY *OALAuxiliaryEffectSlotf) (OALuint, OALenum, OALfloat);
typedef void (OAL_APIENTRY *OALGenFilters) (OALsizei, OALuint *);
typedef void (OAL_APIENTRY *OALDeleteFilters) (OALsizei, const OALuint *);
typedef void (OAL_APIENTRY *OALFilteri) (OALuint, OALenum, OALint);
typedef void (OAL_APIENTRY *OALFilterf) (OALuint, OALenum, OALfloat);
typedef OALenum (OAL_APIENTRY *OALGetError) ();

struct OpenALReverbParameters
{
	float Gain;
	float GainHF;
	float GainLF;
	float DecayTime;
	float DecayHFRatio;
	float DecayLFRatio;
	float ReflectionsGain;
	float ReflectionsDelay;
	float ReflectionsPan[3];
	float LateReverbGain;
	float LateReverbDelay;
	float LateReverbPan[3];
	float EchoTime;
	float EchoDepth;
	float ModulationTime;
	float ModulationDepth;
	float AirAbsorptionGainHF;
	float HFReference;
	float LFReference;
	float RoomRolloffFactor;
	float Diffusion;
	float Density;
	bool DecayHFLimit;
	bool HasInvalidPan;
};

OpenALReverbParameters OALBuildReverbParameters (const REVERB_PROPERTIES &properties);

struct OpenALEFXFunctions
{
	OALGenEffects GenEffects;
	OALDeleteEffects DeleteEffects;
	OALEffecti Effecti;
	OALEffectf Effectf;
	OALEffectfv Effectfv;
	OALGenAuxiliaryEffectSlots GenAuxiliaryEffectSlots;
	OALDeleteAuxiliaryEffectSlots DeleteAuxiliaryEffectSlots;
	OALAuxiliaryEffectSloti AuxiliaryEffectSloti;
	OALAuxiliaryEffectSlotf AuxiliaryEffectSlotf;
	OALGenFilters GenFilters;
	OALDeleteFilters DeleteFilters;
	OALFilteri Filteri;
	OALFilterf Filterf;

	OpenALEFXFunctions ();
	bool IsCallable () const;
	bool IsFilterCallable () const;
};

struct OpenALCapabilities
{
	bool HRTFAdvertised;
	bool HRTFActiveKnown;
	bool HRTFActive;
	bool HRTFStatusKnown;
	int HRTFStatus;
	FString HRTFSpecifier;
	bool EFXAdvertised;
	OpenALEFXFunctions EFX;
	bool EFXCallable;
	bool EFXFilterCallable;
	bool EFXFilterUsable;
	bool EFXUsable;
	bool EFXApplied;
	int EFXSendCount;
	bool RadiusAdvertised;
	bool RadiusApplied;
	bool DopplerApplied;

	OpenALCapabilities ();
};

enum OpenALEFXReverbMode
{
	OALEFXREVERB_Dry,
	OALEFXREVERB_Standard,
	OALEFXREVERB_EAX
};

enum OpenALEFXFilterState
{
	OALEFXFILTER_Absent,
	OALEFXFILTER_Failed,
	OALEFXFILTER_Available
};

enum OpenALEFXFailure
{
	OALEFXFAIL_None,
	OALEFXFAIL_Unavailable,
	OALEFXFAIL_Properties,
	OALEFXFAIL_SlotAttach,
	OALEFXFAIL_WetSend,
	OALEFXFAIL_DrySend,
	OALEFXFAIL_AirAbsorption,
	OALEFXFAIL_DirectFilterAuto,
	OALEFXFAIL_SendGainAuto,
	OALEFXFAIL_SendGainHFAuto,
	OALEFXFAIL_DirectFilter,
	OALEFXFAIL_SourceRadius
};

struct OpenALEFXStatusSnapshot
{
	int SendCount;
	OpenALEFXReverbMode ReverbMode;
	OpenALEFXFilterState FilterState;
	bool Applied;
};

OpenALEFXStatusSnapshot OALGetEFXStatusSnapshot (const OpenALCapabilities &capabilities, bool usesEAX);

OpenALCapabilities OALBuildCapabilities (bool hrtfAdvertised, bool hrtfActiveKnown, bool hrtfActive,
	bool hrtfStatusKnown, int hrtfStatus,
	bool efxAdvertised, const OpenALEFXFunctions &efx, bool radiusAdvertised);
bool OALBuildHRTFContextAttributes (bool hrtfAdvertised, bool hrtfEnabled, int attributes[5]);

enum OpenALContextHRTFFailure
{
	OALHRTFCONTEXT_NoFailure,
	OALHRTFCONTEXT_ExtensionAbsent,
	OALHRTFCONTEXT_CreateFailed,
	OALHRTFCONTEXT_MakeCurrentFailed
};

#ifdef OAL_LIFECYCLE_TEST
struct OpenALEFXSourceAssignment
{
	unsigned int Source;
	int Slot;
	int Send;
	int Filter;
	int Error;
};

enum OpenALContextTestFailure
{
	OALCONTEXTTEST_NoFailure,
	OALCONTEXTTEST_FirstCreateFailure,
	OALCONTEXTTEST_FirstMakeCurrentFailure,
	OALCONTEXTTEST_SecondCreateFailure,
	OALCONTEXTTEST_SecondMakeCurrentFailure,
	OALCONTEXTTEST_BothMakeCurrentFailures,
	OALCONTEXTTEST_SecondOpenFailure
};

struct OpenALContextTestResult
{
	bool Success;
	bool AttributesApplied;
	int OpenCount;
	int CreateCount;
	int MakeCurrentCount;
	int DestroyCount;
	int CloseCount;
	bool FirstAttributesWereHRTF;
	bool FirstAttributesRequestedAuxiliarySend;
	bool SecondAttributesWereNull;
	bool HRTFAdvertised;
	bool HRTFExtensionQueriedOnDevice;
	OpenALContextHRTFFailure HRTFFailure;
	int HRTFSpecifierParameter;
};

OpenALContextTestResult OALTestRunContextInitialization (bool hrtfAdvertised, bool hrtfEnabled,
	OpenALContextTestFailure failure);
bool OALTestCopyHRTFSpecifier (const char *specifier, bool querySucceeded, OpenALCapabilities *capabilities);

struct OpenALHRTFQueryTestResult
{
	OpenALHRTFQueryTestResult ()
	: ActiveQueryCount (0),
	  StatusQueryCount (0)
	{
	}

	OpenALCapabilities Capabilities;
	int ActiveQueryCount;
	int StatusQueryCount;
};

OpenALHRTFQueryTestResult OALTestQueryHRTFCapabilities (bool hrtfAdvertised,
	bool activeQuerySucceeded, bool active, bool statusQuerySucceeded, int status);

struct OpenALEFXResourceTestResult
{
	OpenALEFXResourceTestResult () : Usable (false), UsesEAX (false), Effect (0), Slot (0), Filter (0) {}

	bool Usable;
	bool UsesEAX;
	OALuint Effect;
	OALuint Slot;
	OALuint Filter;
};

OpenALEFXResourceTestResult OALTestInitializeEFXResources (const OpenALEFXFunctions &functions,
	bool filterCallable, int sends, OALGetError getError);
void OALTestFinalizeEFXInitialization (const OpenALEFXFunctions &functions, OpenALEFXResourceTestResult *resources);
void OALTestReleaseEFXResources (const OpenALEFXFunctions &functions, OpenALEFXResourceTestResult *resources);
bool OALTestApplyEFXEnvironment (const OpenALEFXFunctions &functions, const OpenALEFXResourceTestResult &resources,
	const ReverbContainer *environment, OALGetError getError);
#endif

class OpenALSoundRenderer;
class OpenALStreamProducer;
struct ReverbContainer;

enum OpenALStreamState
{
	OALSTREAM_Stopped,
	OALSTREAM_Playing,
	OALSTREAM_Paused,
	OALSTREAM_Draining,
	OALSTREAM_Ended,
	OALSTREAM_Failed
};

#ifdef OAL_LIFECYCLE_TEST
enum OpenALStreamTestALOperation
{
	OALTESTAL_SourceQuery = 1,
	OALTESTAL_SourcePlay,
	OALTESTAL_SourcePause,
	OALTESTAL_BufferUpload,
	OALTESTAL_BufferQueue,
	OALTESTAL_BufferUnqueue,
	OALTESTAL_PositionQuery
};
#endif

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
	bool NoReverb;
	bool Is3D;
	bool IsArea;
	bool WasPlayingBeforePause;
	unsigned int PauseReasons;
	FRolloffInfo Rolloff;
	OpenALEndReason EndReason;
	OpenALFinalizeState FinalizeState;
};

class OpenALSoundStream : public SoundStream
{
public:
	OpenALSoundStream (OpenALSoundRenderer *owner, SoundStreamCallback callback, int bufferBytes, int flags, int sampleRate, void *userData);
	~OpenALSoundStream ();

	bool Play (bool looping, float volume);
	void Stop ();
	void SetVolume (float volume);
	bool SetPaused (bool paused);
	unsigned int GetPosition ();
	bool IsEnded ();
	bool SetPosition (unsigned int milliseconds);

#ifdef OAL_LIFECYCLE_TEST
	void SetProcessedFramesForTest (unsigned long long frames);
	void FailNextRewindForTest (bool terminal);
	void FailNextBufferUploadForTest ();
	void FailNextALOperationForTest (OpenALStreamTestALOperation operation);
	bool ProcessNextBufferForTest ();
	void AllowArbitrarySeekForTest ();
	OpenALStreamState GetStateForTest () const;
	unsigned int GetQueuedBufferCountForTest () const;
	unsigned long long GetBufferMediaStartForTest (unsigned int buffer) const;
	unsigned int GetBufferFramesForTest (unsigned int buffer) const;
	unsigned long long GetMediaFrameForTest () const;
#endif

	unsigned int Source;
	unsigned int Buffers[4];

private:
	friend class OpenALSoundRenderer;
	OpenALSoundStream (OpenALSoundRenderer *owner, OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate);
	OpenALSoundStream (const OpenALSoundStream &);
	OpenALSoundStream &operator= (const OpenALSoundStream &);

	bool QueueBuffer (unsigned int buffer);
	bool FindBufferIndex (unsigned int buffer, unsigned int *bufferIndex) const;
	bool ReadBufferFrames (unsigned int requestedFrames, unsigned int bytesPerFrame, unsigned long long *mediaStart, unsigned int *framesRead);
	bool SubmitBuffer (unsigned int buffer, unsigned int bufferIndex, unsigned long long mediaStart, unsigned int framesRead);
	bool RecycleProcessedBuffer ();
	bool ConvertBuffer (unsigned int frames);
	bool ClearQueuedBuffers (bool failOnError = true);
	bool RewindProducer (unsigned long long frame);
	bool ConfigureLoop ();
	unsigned long long NormalizeMediaFrame (unsigned long long frame) const;
	unsigned long long GetCurrentMediaFrame ();
	bool CheckALError (unsigned int operation);
#ifdef OAL_LIFECYCLE_TEST
	bool ConsumeTestALFailure (unsigned int operation);
#endif
	void SetFailed ();
	void InitializeBuffers (int bufferBytes, int flags);
	void ApplyGain ();
	void ApplyPauseState ();
	void UpdatePlaybackState ();
	void Update ();
	void SetInactive (bool paused);
	void ReleaseResources ();

	OpenALSoundRenderer *Owner;
	OpenALStreamProducer *Producer;
	std::vector<BYTE> InputBuffer;
	std::vector<short> PCM16Buffer;
	std::vector<BYTE> OutputBuffer;
	unsigned int BufferFrames[4];
	unsigned long long BufferMediaStart[4];
	std::vector<unsigned int> QueueOrder;
	unsigned int SampleRate;
	unsigned int StreamChannels;
	unsigned int InputBits;
	unsigned int OutputBits;
	unsigned int OutputFormat;
	unsigned long long MediaFrame;
	unsigned long long LoopStart;
	unsigned long long LoopEnd;
	float Volume;
	bool EndOfInput;
	bool Looping;
	bool HasLoopRange;
	bool UserPaused;
	bool InactivePaused;
	bool InputIsFloat;
	bool ResourcesReleased;
	OpenALStreamState State;
#ifdef OAL_LIFECYCLE_TEST
	bool TestFailNextRewind;
	bool TestRewindTerminal;
	bool TestFailNextBufferUpload;
	unsigned int TestFailNextALOperation;
#endif
};

class OpenALSoundRenderer : public SoundRenderer
{
public:
	friend class OpenALSoundStream;

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
	void MarkVirtualStart (FISoundChannel *chan, SoundHandle sound, int pitch, int flags);
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
	short *DecodeSample (int outlen, const void *coded, int sizebytes, ECodecType type);

#ifdef OAL_LIFECYCLE_TEST
	OpenALSoundStream *CreatePatternStreamForTest (unsigned int totalFrames, unsigned int loopStart, unsigned int loopEnd, int bufferBytes);
#endif

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
	bool ResolveChannelPosition (OpenALChannel *channel, unsigned int *position) const;
	unsigned int CachePosition (OpenALChannel *channel);
	void ApplyChannelGain (OpenALChannel *channel);
	float CalculateRolloffGain (FRolloffInfo &rolloff, float distanceScale, SoundListener *listener, const FVector3 &position, float *distance) const;
	void ApplySpatialState (OpenALChannel *channel, SoundListener *listener, const FVector3 &position, const FVector3 &velocity);
	void DestroySound (OpenALSound *sound);
	bool IsSourceReserved (unsigned int source) const;
	void AdvanceClocks ();
	bool PrepareRestart (OpenALSound *sound, float pitch, bool looping, bool noPause, FISoundChannel *reuseChan, int flags, RestartState *restart) const;
	float GetEffectivePitch (float basePitch, bool noPause) const;
	void RebaseLogicalPosition (LogicalPosition *logicalPosition, unsigned int position, float pitch);
	void UpdateActiveWaterChannels (bool pitchChanged, bool wasPitchActive);
	void UpdateVirtualWaterChannels (bool wasPitchActive);
	void UpdateWaterState (SoundListener *listener);
	bool ApplyWaterFilter (OpenALChannel *channel);
	void PrintWaterStatus () const;
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
	void ReleaseEFXResources ();
	bool ApplyEFXEnvironment (const ReverbContainer *environment);
	bool SetEFXSourceSend (unsigned int source, int slot, int send, int filter);
	bool EnsureEFXSourceDry (unsigned int source);
	bool ClearWaterFilter (OpenALChannel *channel);
	bool ResetEFXSourceProperty (unsigned int source, OALenum property, int value, bool floating, OpenALEFXFailure failure);
	bool ResetEFXSource (unsigned int source);
	bool ApplyChannelEFX (OpenALChannel *channel);
	bool ApplyEFXEnvironmentToChannels ();
	void DrainEFXEnvironmentFailure ();
	void FailEFXEnvironment ();
	void RetireEFXSource (OpenALChannel *channel);
	void RecordEFXFailure (OpenALEFXFailure failure);
	void UpdateEFXEnvironment (SoundListener *listener);
	unsigned long long GetChannelClock (bool noPause) const;
	void DestroyStream (OpenALSoundStream *stream);
	OpenALSoundStream *CreateStreamWithProducer (OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate);

#ifdef OAL_LIFECYCLE_TEST
	void InjectStartFailureForTest ();
	void InjectStartSetupFailureForTest ();
	void InjectPositionQueryFailureForTest ();
	void InjectEFXSourceFailureForTest (OpenALEFXFailure failure = OALEFXFAIL_WetSend, bool persistent = false, unsigned int source = 0);
	void ClearEFXSourceFailureForTest ();
	bool InjectEFXSourceFailure (unsigned int source, OpenALEFXFailure failure);
#endif

	void *Device;
	void *Context;
	OpenALCapabilities Capabilities;
	OALuint EFXEffect;
	OALuint EFXSlot;
	OALuint EFXFilter;
	FString OpenALVersion;
	unsigned int *Sources;
	int RequestedSources;
	int AllocatedSources;
	int OutputRate;
	bool InitSuccess;
	bool HRTFAttributesApplied;
	bool HRTFRequestedEnabled;
	bool EFXUsesEAX;
	bool InvalidReverbPanWarned;
	OpenALContextHRTFFailure HRTFFailure;
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
	bool WaterPitchActive;
	bool WaterFilterActive;
	float WaterFilterGainHF;
	bool EFXEnvironmentInitialized;
	bool EFXFailureDraining;
	const ReverbContainer *LastAttemptedEnvironment;
	const ReverbContainer *LastAppliedEnvironment;
	OpenALEFXFailure EFXFailure;
	std::vector<OpenALChannel *> ActiveChannels;
	std::vector<OpenALSoundStream *> ActiveStreams;
	std::vector<LogicalPosition> LogicalPositions;
	std::vector<unsigned int> RetiringSources;
#ifdef OAL_LIFECYCLE_TEST
	bool FailNextStart;
	bool FailNextStartSetup;
	bool FailNextPositionQuery;
	OpenALEFXFailure FailNextEFXSourceAssign;
	bool PersistentEFXSourceFailure;
	unsigned int FailEFXSourceAssignSource;
	unsigned int EFXSourceFailureCalls;
	bool EFXSourceFailureCallLimitExceeded;
	unsigned int LastEFXSource;
	int LastEFXSlot;
	int LastEFXSend;
	int LastEFXFilter;
	int LastEFXSourceError;
	int LastEFXSourceFailureError;
	unsigned int LastDirectFilterSource;
	int LastDirectFilter;
	int LastDirectFilterError;
	std::vector<OpenALEFXSourceAssignment> EFXSourceAssignments;
#endif
};

#ifdef OAL_LIFECYCLE_TEST
void OALTestFailNextEncodedSourceAssign ();
bool OALTestAcceptsEncodedInputSize (unsigned int bytes);
#endif

#endif