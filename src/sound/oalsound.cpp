#include "oalsound.h"

#include "oaldata.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <limits>
#include <math.h>

#ifdef OAL_LIFECYCLE_TEST
#include "oalsound_test_support.h"
#else
#include "c_cvars.h"
#include "c_console.h"
#include "i_system.h"
#include "s_sound.h"
#include "v_text.h"
#endif

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif

#ifndef AL_LOOP_POINTS_SOFT
#define AL_LOOP_POINTS_SOFT 0x2015
#endif

EXTERN_CVAR (Int, snd_channels)
EXTERN_CVAR (String, snd_openal_device)
EXTERN_CVAR (Float, snd_sfxvolume)
EXTERN_CVAR (Bool, snd_pitched)

enum
{
	OALPAUSE_Gameplay = 1,
	OALPAUSE_Inactive = 2,
	OALPAUSE_Sync = 4
};

static unsigned long long SaturatingAdd (unsigned long long left, unsigned long long right)
{
	return std::numeric_limits<unsigned long long>::max () - left < right ?
		std::numeric_limits<unsigned long long>::max () : left + right;
}

static bool ConvertOutputFramesToSampleFrames (unsigned long long outputFrames, unsigned int sampleRate, float pitch, int outputRate, unsigned long long *sampleFrames)
{
	long double converted;
	if (sampleRate == 0 || outputRate <= 0 || pitch != pitch || pitch <= 0.f || pitch > std::numeric_limits<float>::max ())
	{
		return false;
	}
	converted = ((long double)outputFrames * sampleRate * pitch) / outputRate;
	if (converted != converted || converted >= (long double)std::numeric_limits<unsigned long long>::max ())
	{
		*sampleFrames = std::numeric_limits<unsigned long long>::max ();
	}
	else
	{
		*sampleFrames = (unsigned long long)converted;
	}
	return true;
}

static bool CalculateRestartPosition (unsigned long long startPosition, unsigned long long elapsedOutputFrames, unsigned int sampleRate, float pitch, int outputRate, bool looping, unsigned int frames, unsigned int loopStart, unsigned int loopEnd, unsigned int *position)
{
	unsigned long long elapsedSampleFrames;
	unsigned long long sampleFrame;
	if (!ConvertOutputFramesToSampleFrames (elapsedOutputFrames, sampleRate, pitch, outputRate, &elapsedSampleFrames))
	{
		return false;
	}
	sampleFrame = SaturatingAdd (startPosition, elapsedSampleFrames);
	if (!looping)
	{
		if (sampleFrame >= frames)
		{
			return false;
		}
		*position = (unsigned int)sampleFrame;
		return true;
	}
	if (loopEnd <= loopStart)
	{
		return false;
	}
	*position = sampleFrame < loopStart ? (unsigned int)sampleFrame :
		loopStart + (unsigned int)((sampleFrame - loopStart) % (loopEnd - loopStart));
	return true;
}

static unsigned int GetHostMilliseconds ()
{
#ifdef OAL_LIFECYCLE_TEST
	return OALTestMilliseconds ();
#else
	return I_MSTime ();
#endif
}

static FVector3 ToOpenALCoordinates (const FVector3 &vector)
{
	FVector3 converted;
	converted.X = vector.X;
	converted.Y = vector.Y;
	converted.Z = -vector.Z;
	return converted;
}

OpenALSound::OpenALSound ()
	: Buffer2D (0), BufferMono (0), SampleRate (0), Frames (0), Channels (0),
	  HasLoop (false), LoopStart (0), LoopEnd (0), References (0), DeferredDelete (false)
{
}

OpenALChannel::OpenALChannel ()
	: Source (0), Sound (NULL), Owner (NULL), Gain (0.f), Pitch (1.f), Priority (0),
	  RolloffGain (1.f), EffectiveGain (0.f), Distance (0.f), DistanceScale (1.f),
	  CachedPosition (0), LogicalStartFrame (0), AllocationSerial (0), Looping (false), NoPause (false), Is3D (false), IsArea (false),
	  WasPlayingBeforePause (false), PauseReasons (0), Rolloff (), EndReason (OALEND_None),
	  FinalizeState (OALFINAL_Active)
{
}

OpenALSoundRenderer::OpenALSoundRenderer ()
	: Device (NULL), Context (NULL), Sources (NULL), RequestedSources (0),
	  AllocatedSources (0), OutputRate (0), InitSuccess (false), SfxVolume (1.f),
		MusicVolume (1.f), NextAllocationSerial (0), NextLogicalPositionToken (~0ull), PausableOutputFrames (0),
	  NonPausableOutputFrames (0), PausableFrameRemainder (0), NonPausableFrameRemainder (0),
	  LastClockMilliseconds (0), SfxPaused (0), InactiveState (INACTIVE_Active),
	  SyncPaused (false), PendingStartNoPause (false)
#ifdef OAL_LIFECYCLE_TEST
	  , FailNextStart (false)
#endif
{
	InitSuccess = Init ();
	LastClockMilliseconds = GetHostMilliseconds ();
}

OpenALSoundRenderer::~OpenALSoundRenderer ()
{
	Shutdown ();
}

bool OpenALSoundRenderer::Init ()
{
	const char *requestedDevice = *snd_openal_device;
	const char *deviceName = NULL;
	ALCdevice *device;
	ALCcontext *context;

	if (stricmp (requestedDevice, "default") != 0)
	{
		deviceName = FindDeviceName (requestedDevice);
		if (deviceName == NULL)
		{
			Printf (TEXTCOLOR_RED "OpenAL device '%s' was not found. Falling back to FMOD.\n", requestedDevice);
			return false;
		}
	}

	device = alcOpenDevice (deviceName);
	if (device == NULL)
	{
		Printf (TEXTCOLOR_RED "OpenAL could not open device '%s'. Falling back to FMOD.\n", requestedDevice);
		return false;
	}
	Device = device;
	DeviceName = alcGetString (device, ALC_DEVICE_SPECIFIER);

	context = alcCreateContext (device, NULL);
	if (context == NULL || !alcMakeContextCurrent (context))
	{
		Printf (TEXTCOLOR_RED "OpenAL could not create a current context. Falling back to FMOD.\n");
		if (context != NULL)
		{
			alcDestroyContext (context);
		}
		alcCloseDevice (device);
		Device = NULL;
		return false;
	}
	Context = context;

	if (!alIsExtensionPresent ("AL_SOFT_loop_points"))
	{
		Printf (TEXTCOLOR_RED "OpenAL device '%s' lacks AL_SOFT_loop_points. Falling back to FMOD.\n", DeviceName.GetChars());
		return false;
	}

	alDistanceModel (AL_NONE);
	alDopplerFactor (0.f);
	if (alGetError () != AL_NO_ERROR)
	{
		Printf (TEXTCOLOR_RED "OpenAL could not configure distance attenuation and Doppler. Falling back to FMOD.\n");
		return false;
	}

	RequestedSources = snd_channels > 0 ? snd_channels : 1;
	Sources = new unsigned int[RequestedSources];
	for (int i = 0; i < RequestedSources; ++i)
	{
		ALuint source;
		alGetError ();
		alGenSources (1, &source);
		if (alGetError () != AL_NO_ERROR)
		{
			break;
		}
		Sources[AllocatedSources++] = source;
	}

	if (AllocatedSources == 0)
	{
		Printf (TEXTCOLOR_RED "OpenAL could not allocate an SFX source. Falling back to FMOD.\n");
		return false;
	}

	alcGetIntegerv (device, ALC_FREQUENCY, 1, &OutputRate);
	if (alcGetError (device) != ALC_NO_ERROR)
	{
		OutputRate = 0;
	}
	return true;
}

void OpenALSoundRenderer::Shutdown ()
{
	ALCcontext *context = (ALCcontext *)Context;
	ALCdevice *device = (ALCdevice *)Device;

	if (context != NULL)
	{
		alcMakeContextCurrent (context);
		while (!ActiveChannels.empty ())
		{
			FinalizeChannel (ActiveChannels[0], OALEND_BackendError);
		}
		if (Sources != NULL && AllocatedSources > 0)
		{
			alDeleteSources (AllocatedSources, (ALuint *)Sources);
		}
	}
	delete[] Sources;
	Sources = NULL;
	AllocatedSources = 0;
	RequestedSources = 0;

	if (context != NULL)
	{
		alcMakeContextCurrent (NULL);
		alcDestroyContext (context);
	}
	Context = NULL;
	if (device != NULL)
	{
		alcCloseDevice (device);
	}
	Device = NULL;
	InitSuccess = false;
}

const char *OpenALSoundRenderer::FindDeviceName (const char *name) const
{
	const ALCchar *devices;
	ALCenum specifier;

	if (alcIsExtensionPresent (NULL, "ALC_ENUMERATE_ALL_EXT"))
	{
		specifier = ALC_ALL_DEVICES_SPECIFIER;
	}
	else if (alcIsExtensionPresent (NULL, "ALC_ENUMERATION_EXT"))
	{
		specifier = ALC_DEVICE_SPECIFIER;
	}
	else
	{
		Printf (TEXTCOLOR_RED "OpenAL cannot enumerate named devices on this system.\n");
		return NULL;
	}

	devices = alcGetString (NULL, specifier);
	for (const ALCchar *device = devices; device != NULL && *device != '\0'; device += strlen (device) + 1)
	{
		if (strcmp (device, name) == 0)
		{
			return device;
		}
	}
	return NULL;
}

void OpenALSoundRenderer::SetSfxVolume (float volume)
{
	SfxVolume = volume;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		ApplyChannelGain (ActiveChannels[index]);
	}
}

void OpenALSoundRenderer::SetMusicVolume (float volume)
{
	MusicVolume = volume;
}

SoundHandle OpenALSoundRenderer::CreateSound (const OALPCMData &data)
{
	SoundHandle handle = { NULL };
	OpenALSound *sound;
	ALenum format;
	ALint loopPoints[2];

	if (data.SampleRate > (unsigned int)std::numeric_limits<ALsizei>::max () ||
		data.Samples.size () > (size_t)std::numeric_limits<ALsizei>::max () / sizeof (short))
	{
		return handle;
	}
	sound = new OpenALSound;
	sound->SampleRate = data.SampleRate;
	sound->Frames = data.Frames;
	sound->Channels = data.Channels;
	sound->HasLoop = data.HasLoop;
	sound->LoopStart = data.LoopStart;
	sound->LoopEnd = data.LoopEnd;
	format = data.Channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

	alGetError ();
	alGenBuffers (1, (ALuint *)&sound->Buffer2D);
	alBufferData (sound->Buffer2D, format, &data.Samples[0], (ALsizei)(data.Samples.size () * sizeof (short)), (ALsizei)data.SampleRate);
	if (data.HasLoop)
	{
		loopPoints[0] = (ALint)data.LoopStart;
		loopPoints[1] = (ALint)data.LoopEnd;
		alBufferiv (sound->Buffer2D, AL_LOOP_POINTS_SOFT, loopPoints);
	}
	if (alGetError () != AL_NO_ERROR)
	{
		DestroySound (sound);
		return handle;
	}
	if (data.Channels == 2)
	{
		OALPCMResult mono = OALDownmixToMono (data);
		if (!mono.IsValid ())
		{
			DestroySound (sound);
			return handle;
		}
		alGenBuffers (1, (ALuint *)&sound->BufferMono);
		alBufferData (sound->BufferMono, AL_FORMAT_MONO16, &mono.Data.Samples[0], (ALsizei)(mono.Data.Samples.size () * sizeof (short)), (ALsizei)mono.Data.SampleRate);
		if (data.HasLoop)
		{
			alBufferiv (sound->BufferMono, AL_LOOP_POINTS_SOFT, loopPoints);
		}
		if (alGetError () != AL_NO_ERROR)
		{
			DestroySound (sound);
			return handle;
		}
	}
	handle.data = sound;
	return handle;
}

SoundHandle OpenALSoundRenderer::LoadSound (BYTE *sfxdata, int length)
{
	SoundHandle handle = { NULL };
	if (length <= 0)
	{
		return handle;
	}
	OALPCMResult result = OALParseWavePCM (sfxdata, (size_t)length);
	if (!result.IsValid ())
	{
		DPrintf ("OpenAL could not load WAVE sample: error %d\n", result.Error);
		return handle;
	}
	return CreateSound (result.Data);
}

SoundHandle OpenALSoundRenderer::LoadSoundRaw (BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	SoundHandle handle = { NULL };
	if (length <= 0)
	{
		return handle;
	}
	OALPCMResult result = OALConvertRawPCM (sfxdata, (size_t)length, (unsigned int)frequency, (unsigned int)channels, bits, loopstart, loopend);
	if (!result.IsValid ())
	{
		DPrintf ("OpenAL could not load raw sample: error %d\n", result.Error);
		return handle;
	}
	return CreateSound (result.Data);
}

void OpenALSoundRenderer::DestroySound (OpenALSound *sound)
{
	if (sound == NULL)
	{
		return;
	}
	if (sound->BufferMono != 0 && sound->BufferMono != sound->Buffer2D)
	{
		alDeleteBuffers (1, (ALuint *)&sound->BufferMono);
	}
	if (sound->Buffer2D != 0)
	{
		alDeleteBuffers (1, (ALuint *)&sound->Buffer2D);
	}
	delete sound;
}

void OpenALSoundRenderer::UnloadSound (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	if (sound == NULL)
	{
		return;
	}
	sound->DeferredDelete = true;
	if (sound->References == 0)
	{
		DestroySound (sound);
	}
}

unsigned int OpenALSoundRenderer::GetMSLength (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	if (sound == NULL || sound->SampleRate == 0)
	{
		return 0;
	}
	return (unsigned int)(((unsigned long long)sound->Frames * 1000) / sound->SampleRate);
}

unsigned int OpenALSoundRenderer::GetSampleLength (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	return sound == NULL ? 0 : sound->Frames;
}

float OpenALSoundRenderer::GetOutputRate ()
{
	return (float)OutputRate;
}

SoundStream *OpenALSoundRenderer::CreateStream (SoundStreamCallback, int, int, int, void *)
{
	return NULL;
}

SoundStream *OpenALSoundRenderer::OpenStream (const char *, int, int, int)
{
	return NULL;
}

bool OpenALSoundRenderer::IsSourceReserved (unsigned int source) const
{
	return std::find (RetiringSources.begin (), RetiringSources.end (), source) != RetiringSources.end ();
}

void OpenALSoundRenderer::AdvanceClocks ()
{
	unsigned int now = GetHostMilliseconds ();
	unsigned int elapsed = now - LastClockMilliseconds;
	LastClockMilliseconds = now;
	if (elapsed == 0 || OutputRate <= 0)
	{
		return;
	}
	if (SfxPaused == 0 && InactiveState != INACTIVE_Complete && !SyncPaused)
	{
		unsigned long long frames = (unsigned long long)elapsed * OutputRate + PausableFrameRemainder;
		PausableFrameRemainder = (unsigned int)(frames % 1000);
		frames /= 1000;
		PausableOutputFrames = std::numeric_limits<unsigned long long>::max () - PausableOutputFrames < frames ?
			std::numeric_limits<unsigned long long>::max () : PausableOutputFrames + frames;
	}
	if (InactiveState != INACTIVE_Complete && !SyncPaused)
	{
		unsigned long long frames = (unsigned long long)elapsed * OutputRate + NonPausableFrameRemainder;
		NonPausableFrameRemainder = (unsigned int)(frames % 1000);
		frames /= 1000;
		NonPausableOutputFrames = std::numeric_limits<unsigned long long>::max () - NonPausableOutputFrames < frames ?
			std::numeric_limits<unsigned long long>::max () : NonPausableOutputFrames + frames;
	}
}

void OpenALSoundRenderer::InitializePauseState (OpenALChannel *channel)
{
	channel->PauseReasons = (!channel->NoPause && SfxPaused != 0 ? OALPAUSE_Gameplay : 0) |
		(InactiveState == INACTIVE_Complete ? OALPAUSE_Inactive : 0) | (SyncPaused ? OALPAUSE_Sync : 0);
}

void OpenALSoundRenderer::ApplyChannelPauseState (OpenALChannel *channel)
{
	ALint state = AL_STOPPED;
	if (channel == NULL || channel->FinalizeState != OALFINAL_Active)
	{
		return;
	}
	if (channel->PauseReasons != 0)
	{
		alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
		if (state == AL_PLAYING)
		{
			channel->WasPlayingBeforePause = true;
			alSourcePause (channel->Source);
		}
	}
	else if (channel->WasPlayingBeforePause)
	{
		channel->WasPlayingBeforePause = false;
		alSourcePlay (channel->Source);
	}
}

unsigned long long OpenALSoundRenderer::GetChannelClock (bool noPause) const
{
	return noPause ? NonPausableOutputFrames : PausableOutputFrames;
}

bool OpenALSoundRenderer::PrepareRestart (OpenALSound *sound, float pitch, bool looping, bool noPause, FISoundChannel *reuseChan, int flags, RestartState *restart) const
{
	unsigned long long selectedClock = GetChannelClock (noPause);
	unsigned long long elapsedOutputFrames;
	unsigned long long savedPosition;
	const LogicalPosition *logicalPosition;
	unsigned int loopStart = sound->HasLoop ? sound->LoopStart : 0;
	unsigned int loopEnd = sound->HasLoop ? sound->LoopEnd : sound->Frames;
	restart->Position = 0;
	restart->ClockFrame = selectedClock;
	if (reuseChan == NULL)
	{
		return true;
	}
	if (OutputRate <= 0 || sound->SampleRate == 0 || pitch <= 0.f)
	{
		return false;
	}
	if (flags & SNDF_ABSTIME)
	{
		savedPosition = reuseChan->StartTime.AsOne;
		if (savedPosition >= sound->Frames)
		{
			return false;
		}
		restart->Position = (unsigned int)savedPosition;
		return true;
	}
	logicalPosition = FindLogicalPosition (reuseChan);
	if (logicalPosition != NULL && logicalPosition->OwnerToken != 0 && logicalPosition->Sound == sound &&
		reuseChan->StartTime.AsOne == logicalPosition->OwnerToken)
	{
		elapsedOutputFrames = selectedClock >= logicalPosition->StartClock ? selectedClock - logicalPosition->StartClock : 0;
		return CalculateRestartPosition (logicalPosition->StartPosition, elapsedOutputFrames, logicalPosition->SampleRate,
			logicalPosition->Pitch, OutputRate, logicalPosition->Looping, logicalPosition->Frames,
			logicalPosition->LoopStart, logicalPosition->LoopEnd, &restart->Position);
	}
	elapsedOutputFrames = selectedClock >= reuseChan->StartTime.AsOne ? selectedClock - reuseChan->StartTime.AsOne : 0;
	return CalculateRestartPosition (0, elapsedOutputFrames, sound->SampleRate, pitch, OutputRate, looping,
		sound->Frames, loopStart, loopEnd, &restart->Position);
}

OpenALChannel *OpenALSoundRenderer::CreateChannel (unsigned int source, OpenALSound *sound, float volume, float pitch, int priority, int flags)
{
	OpenALChannel *channel = new OpenALChannel;
	channel->Source = source;
	channel->Sound = sound;
	channel->Gain = volume;
	channel->Pitch = pitch;
	channel->Priority = priority;
	channel->Looping = (flags & SNDF_LOOP) != 0;
	channel->NoPause = (flags & SNDF_NOPAUSE) != 0;
	channel->AllocationSerial = ++NextAllocationSerial;
	InitializePauseState (channel);
	return channel;
}

unsigned int OpenALSoundRenderer::AcquireSource (int priority, float effectiveGain)
{
	unsigned int source = FindFreeSource ();
	OpenALChannel *candidate;
	if (source == 0 && FinalizePendingStopForReuse ())
	{
		source = FindFreeSource ();
	}
	if (source != 0)
	{
		return source;
	}
	candidate = FindEvictionCandidate ();
	if (!IncomingWins (candidate, priority, effectiveGain))
	{
		return 0;
	}
	FinalizeChannel (candidate, OALEND_PoolEviction);
	return FindFreeSource ();
}

unsigned long long OpenALSoundRenderer::AllocateLogicalPositionToken ()
{
	if (NextLogicalPositionToken <= 0xffffffffull)
	{
		NextLogicalPositionToken = ~0ull;
	}
	return NextLogicalPositionToken--;
}

bool OpenALSoundRenderer::ApplyRestartPosition (OpenALChannel *channel, const RestartState &restart)
{
	alSourcei (channel->Source, AL_SAMPLE_OFFSET, (ALint)restart.Position);
	if (alGetError () != AL_NO_ERROR)
	{
		return false;
	}
	channel->CachedPosition = restart.Position;
	channel->LogicalStartFrame = restart.ClockFrame;
	return true;
}

const OpenALSoundRenderer::LogicalPosition *OpenALSoundRenderer::FindLogicalPosition (FISoundChannel *owner) const
{
	for (size_t index = 0; index < LogicalPositions.size (); ++index)
	{
		if (LogicalPositions[index].Owner == owner)
		{
			return &LogicalPositions[index];
		}
	}
	return NULL;
}

void OpenALSoundRenderer::RememberLogicalPosition (FISoundChannel *owner, OpenALChannel *channel, const RestartState &restart)
{
	LogicalPosition *logicalPosition = const_cast<LogicalPosition *> (FindLogicalPosition (owner));
	if (logicalPosition == NULL)
	{
		LogicalPositions.push_back (LogicalPosition ());
		logicalPosition = &LogicalPositions.back ();
	}
	logicalPosition->Owner = owner;
	logicalPosition->Sound = channel->Sound;
	logicalPosition->StartClock = restart.ClockFrame;
	logicalPosition->StartPosition = restart.Position;
	logicalPosition->OwnerToken = 0;
	logicalPosition->SampleRate = channel->Sound->SampleRate;
	logicalPosition->Frames = channel->Sound->Frames;
	logicalPosition->LoopStart = channel->Sound->HasLoop ? channel->Sound->LoopStart : 0;
	logicalPosition->LoopEnd = channel->Sound->HasLoop ? channel->Sound->LoopEnd : channel->Sound->Frames;
	logicalPosition->Pitch = channel->Pitch;
	logicalPosition->Looping = channel->Looping;
	logicalPosition->NoPause = channel->NoPause;
}

void OpenALSoundRenderer::ForgetLogicalPosition (FISoundChannel *owner)
{
	for (size_t index = 0; index < LogicalPositions.size (); ++index)
	{
		if (LogicalPositions[index].Owner == owner)
		{
			LogicalPositions.erase (LogicalPositions.begin () + index);
			return;
		}
	}
}

bool OpenALSoundRenderer::GetLogicalPosition (FISoundChannel *owner, unsigned int *position) const
{
	const LogicalPosition *logicalPosition = FindLogicalPosition (owner);
	unsigned long long clock;
	if (logicalPosition == NULL || logicalPosition->OwnerToken == 0 || owner == NULL ||
		owner->StartTime.AsOne != logicalPosition->OwnerToken)
	{
		return false;
	}
	clock = GetChannelClock (logicalPosition->NoPause);
	return CalculateRestartPosition (logicalPosition->StartPosition,
		clock >= logicalPosition->StartClock ? clock - logicalPosition->StartClock : 0,
		logicalPosition->SampleRate, logicalPosition->Pitch, OutputRate, logicalPosition->Looping,
		logicalPosition->Frames, logicalPosition->LoopStart, logicalPosition->LoopEnd, position);
}

FISoundChannel *OpenALSoundRenderer::PublishChannel (OpenALChannel *channel, FISoundChannel *reuseChan, const RestartState &restart)
{
	FISoundChannel *owner;
	if (!ApplyRestartPosition (channel, restart))
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	alSourcePlay (channel->Source);
#ifdef OAL_LIFECYCLE_TEST
	if (FailNextStart)
	{
		FailNextStart = false;
		alSourcei (channel->Source, 0, 0);
	}
#endif
	if (alGetError () != AL_NO_ERROR)
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	owner = reuseChan != NULL ? reuseChan : S_GetChannel (channel);
	if (owner == NULL)
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	channel->Owner = owner;
	owner->SysChannel = channel;
	owner->StartTime.AsOne = restart.ClockFrame;
	RememberLogicalPosition (owner, channel, restart);
	PendingStartNoPause = false;
	++channel->Sound->References;
	ActiveChannels.push_back (channel);
	ApplyChannelPauseState (channel);
	return owner;
}

#ifdef OAL_LIFECYCLE_TEST
void OpenALSoundRenderer::InjectStartFailureForTest ()
{
	FailNextStart = true;
}
#endif

unsigned int OpenALSoundRenderer::FindFreeSource () const
{
	for (int index = 0; index < AllocatedSources; ++index)
	{
		bool inUse = IsSourceReserved (Sources[index]);
		for (size_t channel = 0; !inUse && channel < ActiveChannels.size (); ++channel)
		{
			inUse = ActiveChannels[channel]->Source == Sources[index];
		}
		if (!inUse)
		{
			return Sources[index];
		}
	}
	return 0;
}

OpenALChannel *OpenALSoundRenderer::FindEvictionCandidate () const
{
	OpenALChannel *candidate = NULL;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (channel->FinalizeState != OALFINAL_Active)
		{
			continue;
		}
		if (candidate == NULL || channel->Priority < candidate->Priority ||
			(channel->Priority == candidate->Priority && (channel->EffectiveGain < candidate->EffectiveGain ||
			(channel->EffectiveGain == candidate->EffectiveGain && channel->AllocationSerial < candidate->AllocationSerial))))
		{
			candidate = channel;
		}
	}
	return candidate;
}

bool OpenALSoundRenderer::FinalizePendingStopForReuse ()
{
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (channel->FinalizeState == OALFINAL_Pending)
		{
			FinalizeChannel (channel, channel->EndReason);
			return true;
		}
	}
	return false;
}

bool OpenALSoundRenderer::IncomingWins (const OpenALChannel *candidate, int priority, float effectiveGain) const
{
	return candidate != NULL && (priority > candidate->Priority ||
		(priority == candidate->Priority && effectiveGain > candidate->EffectiveGain));
}

void OpenALSoundRenderer::RemoveActiveChannel (OpenALChannel *channel)
{
	std::vector<OpenALChannel *>::iterator found = std::find (ActiveChannels.begin (), ActiveChannels.end (), channel);
	if (found != ActiveChannels.end ())
	{
		ActiveChannels.erase (found);
	}
}

unsigned int OpenALSoundRenderer::CachePosition (OpenALChannel *channel)
{
	ALint position = 0;
	if (channel->FinalizeState == OALFINAL_Active)
	{
		alGetSourcei (channel->Source, AL_SAMPLE_OFFSET, &position);
		if (alGetError () == AL_NO_ERROR && position >= 0)
		{
			channel->CachedPosition = (unsigned int)position;
		}
	}
	return channel->CachedPosition;
}

void OpenALSoundRenderer::ApplyChannelGain (OpenALChannel *channel)
{
	channel->EffectiveGain = InactiveState == INACTIVE_Mute ? 0.f : channel->Gain * SfxVolume * channel->RolloffGain;
	alSourcef (channel->Source, AL_GAIN, channel->EffectiveGain);
}

float OpenALSoundRenderer::CalculateRolloffGain (FRolloffInfo &rolloff, float distanceScale, SoundListener *listener, const FVector3 &position, float *distance) const
{
	if (listener == NULL || !listener->valid)
	{
		*distance = 0.f;
		return S_GetRolloff (&rolloff, 0.f, true);
	}
	*distance = sqrtf ((float)(listener->position - position).LengthSquared ());
	return S_GetRolloff (&rolloff, *distance * distanceScale, true);
}

void OpenALSoundRenderer::ApplySpatialState (OpenALChannel *channel, SoundListener *listener, const FVector3 &position, const FVector3 &velocity)
{
	float distance;
	bool headRelative;
	FVector3 convertedPosition;
	FVector3 convertedVelocity;

	channel->RolloffGain = CalculateRolloffGain (channel->Rolloff, channel->DistanceScale, listener, position, &distance);
	channel->Distance = distance;
	// Center nearby area sounds as a bounded Phase 1A panning approximation.
	headRelative = listener != NULL && listener->valid &&
		(distance == 0.f || (channel->IsArea && distance <= 32.f));

	if (headRelative)
	{
		alSourcei (channel->Source, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f (channel->Source, AL_POSITION, 0.f, 0.f, 0.f);
		alSource3f (channel->Source, AL_VELOCITY, 0.f, 0.f, 0.f);
	}
	else
	{
		convertedPosition = ToOpenALCoordinates (position);
		convertedVelocity = ToOpenALCoordinates (velocity);
		alSourcei (channel->Source, AL_SOURCE_RELATIVE, AL_FALSE);
		alSource3f (channel->Source, AL_POSITION, convertedPosition.X, convertedPosition.Y, convertedPosition.Z);
		alSource3f (channel->Source, AL_VELOCITY, convertedVelocity.X, convertedVelocity.Y, convertedVelocity.Z);
	}
	ApplyChannelGain (channel);
}

void OpenALSoundRenderer::FinalizeChannel (OpenALChannel *channel, OpenALEndReason reason)
{
	OpenALSound *sound;
	if (channel == NULL || channel->FinalizeState == OALFINAL_Finalizing || channel->FinalizeState == OALFINAL_Finalized)
	{
		return;
	}
	if (reason == OALEND_Natural && channel->Sound != NULL)
	{
		channel->CachedPosition = channel->Sound->Frames;
	}
	else
	{
		CachePosition (channel);
		if (reason == OALEND_PoolEviction && channel->Sound != NULL && channel->CachedPosition >= channel->Sound->Frames && channel->Sound->Frames > 0)
		{
			channel->CachedPosition = channel->Sound->Frames - 1;
		}
	}
	channel->EndReason = reason;
	channel->FinalizeState = OALFINAL_Finalizing;
	RemoveActiveChannel (channel);
	RetiringSources.push_back (channel->Source);
	if (channel->Owner != NULL)
	{
		if (reason == OALEND_PoolEviction)
		{
			LogicalPosition *logicalPosition = const_cast<LogicalPosition *> (FindLogicalPosition (channel->Owner));
			if (logicalPosition != NULL)
			{
				logicalPosition->OwnerToken = AllocateLogicalPositionToken ();
				channel->Owner->StartTime.AsOne = logicalPosition->OwnerToken;
			}
		}
		else
		{
			ForgetLogicalPosition (channel->Owner);
		}
		S_ChannelEnded (channel->Owner);
	}
	alSourceStop (channel->Source);
	alSourcei (channel->Source, AL_BUFFER, 0);
	std::vector<unsigned int>::iterator retiring = std::find (RetiringSources.begin (), RetiringSources.end (), channel->Source);
	if (retiring != RetiringSources.end ())
	{
		RetiringSources.erase (retiring);
	}
	sound = channel->Sound;
	channel->Sound = NULL;
	channel->Owner = NULL;
	channel->FinalizeState = OALFINAL_Finalized;
	if (sound != NULL && --sound->References == 0 && sound->DeferredDelete)
	{
		DestroySound (sound);
	}
	delete channel;
}

FISoundChannel *OpenALSoundRenderer::Start2D (SoundHandle sfx, float volume, int pitch, int flags, int priority, FISoundChannel *reuseChan)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	OpenALChannel *channel;
	RestartState restart;
	float pitchRatio;
	unsigned int source;
	AdvanceClocks ();
	PendingStartNoPause = (flags & SNDF_NOPAUSE) != 0;
	if (!InitSuccess || sound == NULL || sound->DeferredDelete || reuseChan != NULL && reuseChan->SysChannel != NULL)
	{
		return NULL;
	}
	pitchRatio = snd_pitched ? pitch / 128.f : 1.f;
	if (!PrepareRestart (sound, pitchRatio, (flags & SNDF_LOOP) != 0, (flags & SNDF_NOPAUSE) != 0, reuseChan, flags, &restart))
	{
		return NULL;
	}
	source = AcquireSource (priority, volume * SfxVolume);
	if (source == 0)
	{
		return NULL;
	}
	channel = CreateChannel (source, sound, volume, pitchRatio, priority, flags);

	alGetError ();
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)sound->Buffer2D);
	alSourcei (source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f (source, AL_POSITION, 0.f, 0.f, 0.f);
	alSource3f (source, AL_VELOCITY, 0.f, 0.f, 0.f);
	alSourcei (source, AL_LOOPING, channel->Looping ? AL_TRUE : AL_FALSE);
	alSourcef (source, AL_PITCH, channel->Pitch);
	ApplyChannelGain (channel);
	return PublishChannel (channel, reuseChan, restart);
}

FISoundChannel *OpenALSoundRenderer::StartSound (SoundHandle sfx, float volume, int pitch, int priority, int flags, FISoundChannel *reuseChan)
{
	return Start2D (sfx, volume, pitch, flags, priority, reuseChan);
}

FISoundChannel *OpenALSoundRenderer::StartSound3D (SoundHandle sfx, SoundListener *listener, float volume, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int, int flags, FISoundChannel *reuseChan)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	OpenALChannel *channel;
	RestartState restart;
	float distance;
	float rolloffGain;
	float effectiveGain;
	float pitchRatio;
	unsigned int source;
	AdvanceClocks ();
	PendingStartNoPause = (flags & SNDF_NOPAUSE) != 0;
	if (!InitSuccess || sound == NULL || sound->DeferredDelete || rolloff == NULL || reuseChan != NULL && reuseChan->SysChannel != NULL)
	{
		return NULL;
	}
	rolloffGain = CalculateRolloffGain (*rolloff, distscale, listener, pos, &distance);
	effectiveGain = volume * SfxVolume * rolloffGain;
	pitchRatio = snd_pitched ? pitch / 128.f : 1.f;
	if (!PrepareRestart (sound, pitchRatio, (flags & SNDF_LOOP) != 0, (flags & SNDF_NOPAUSE) != 0, reuseChan, flags, &restart))
	{
		return NULL;
	}
	source = AcquireSource (priority, effectiveGain);
	if (source == 0)
	{
		return NULL;
	}
	channel = CreateChannel (source, sound, volume, pitchRatio, priority, flags);
	channel->RolloffGain = rolloffGain;
	channel->EffectiveGain = effectiveGain;
	channel->Distance = distance;
	channel->DistanceScale = distscale;
	channel->Is3D = true;
	channel->IsArea = (flags & SNDF_AREA) != 0;
	channel->Rolloff = *rolloff;

	alGetError ();
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)(sound->BufferMono != 0 ? sound->BufferMono : sound->Buffer2D));
	alSourcei (source, AL_LOOPING, channel->Looping ? AL_TRUE : AL_FALSE);
	alSourcef (source, AL_PITCH, channel->Pitch);
	ApplySpatialState (channel, listener, pos, vel);
	return PublishChannel (channel, reuseChan, restart);
}

void OpenALSoundRenderer::StopChannel (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active)
	{
		CachePosition (channel);
		alSourceStop (channel->Source);
		channel->EndReason = OALEND_ExplicitStop;
		channel->FinalizeState = OALFINAL_Pending;
	}
}

void OpenALSoundRenderer::ChannelVolume (FISoundChannel *owner, float volume)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active)
	{
		channel->Gain = volume;
		ApplyChannelGain (channel);
	}
}

void OpenALSoundRenderer::MarkStartTime (FISoundChannel *channel)
{
	AdvanceClocks ();
	if (channel != NULL)
	{
		ForgetLogicalPosition (channel);
		channel->StartTime.AsOne = GetChannelClock (PendingStartNoPause);
	}
	PendingStartNoPause = false;
}

unsigned int OpenALSoundRenderer::GetPosition (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	unsigned int position = 0;
	AdvanceClocks ();
	if (channel != NULL)
	{
		return CachePosition (channel);
	}
	return GetLogicalPosition (owner, &position) ? position : 0;
}

bool OpenALSoundRenderer::ResolveEvictedPosition (FISoundChannel *owner, unsigned int *position)
{
	const LogicalPosition *logicalPosition;
	if (owner == NULL || position == NULL || owner->SysChannel != NULL)
	{
		return false;
	}
	AdvanceClocks ();
	logicalPosition = FindLogicalPosition (owner);
	if (logicalPosition == NULL || logicalPosition->OwnerToken == 0 ||
		owner->StartTime.AsOne != logicalPosition->OwnerToken)
	{
		return false;
	}
	if (GetLogicalPosition (owner, position))
	{
		return true;
	}
	if (!logicalPosition->Looping && OutputRate > 0 && logicalPosition->SampleRate > 0 && logicalPosition->Pitch > 0.f)
	{
		*position = logicalPosition->Frames;
		return true;
	}
	return false;
}

float OpenALSoundRenderer::GetAudibility (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	return channel == NULL ? 0.f : channel->EffectiveGain;
}

void OpenALSoundRenderer::Sync (bool sync)
{
	AdvanceClocks ();
	if (SyncPaused == sync)
	{
		return;
	}
	if (sync)
	{
		for (size_t index = 0; index < ActiveChannels.size (); ++index)
		{
			CachePosition (ActiveChannels[index]);
		}
	}
	SyncPaused = sync;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (sync)
		{
			channel->PauseReasons |= OALPAUSE_Sync;
		}
		else
		{
			channel->PauseReasons &= ~OALPAUSE_Sync;
		}
		ApplyChannelPauseState (channel);
	}
}

void OpenALSoundRenderer::SetSfxPaused (bool paused, int slot)
{
	unsigned int bit;
	if (slot < 0 || slot >= 32)
	{
		return;
	}
	AdvanceClocks ();
	bit = 1u << slot;
	if (paused)
	{
		SfxPaused |= bit;
	}
	else
	{
		SfxPaused &= ~bit;
	}
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (!channel->NoPause)
		{
			if (SfxPaused != 0)
			{
				channel->PauseReasons |= OALPAUSE_Gameplay;
			}
			else
			{
				channel->PauseReasons &= ~OALPAUSE_Gameplay;
			}
			ApplyChannelPauseState (channel);
		}
	}
}

void OpenALSoundRenderer::SetInactive (EInactiveState inactive)
{
	AdvanceClocks ();
	InactiveState = inactive;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (inactive == INACTIVE_Complete)
		{
			channel->PauseReasons |= OALPAUSE_Inactive;
		}
		else
		{
			channel->PauseReasons &= ~OALPAUSE_Inactive;
		}
		ApplyChannelGain (channel);
		ApplyChannelPauseState (channel);
	}
}

void OpenALSoundRenderer::UpdateSoundParams3D (SoundListener *listener, FISoundChannel *owner, bool areasound, const FVector3 &pos, const FVector3 &vel)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active && channel->Is3D)
	{
		channel->IsArea = areasound;
		ApplySpatialState (channel, listener, pos, vel);
	}
}

void OpenALSoundRenderer::UpdateListener (SoundListener *listener)
{
	ALfloat orientation[6];
	FVector3 position;
	if (listener == NULL || !listener->valid)
	{
		return;
	}
	orientation[0] = cosf (listener->angle);
	orientation[1] = 0.f;
	orientation[2] = -sinf (listener->angle);
	orientation[3] = 0.f;
	orientation[4] = 1.f;
	orientation[5] = 0.f;
	position = ToOpenALCoordinates (listener->position);
	alListener3f (AL_POSITION, position.X, position.Y, position.Z);
	alListener3f (AL_VELOCITY, 0.f, 0.f, 0.f);
	alListenerfv (AL_ORIENTATION, orientation);
}

void OpenALSoundRenderer::UpdateSounds ()
{
	AdvanceClocks ();
	for (size_t index = 0; index < ActiveChannels.size (); )
	{
		OpenALChannel *channel = ActiveChannels[index];
		ALint state = AL_STOPPED;
		if (channel->FinalizeState == OALFINAL_Pending)
		{
			FinalizeChannel (channel, channel->EndReason);
			continue;
		}
		alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
		if (alGetError () != AL_NO_ERROR)
		{
			FinalizeChannel (channel, OALEND_BackendError);
			continue;
		}
		if (state == AL_STOPPED)
		{
			FinalizeChannel (channel, OALEND_Natural);
			continue;
		}
		++index;
	}
}

bool OpenALSoundRenderer::IsValid ()
{
	return InitSuccess;
}

void OpenALSoundRenderer::PrintStatus ()
{
	if (!InitSuccess)
	{
		Printf (TEXTCOLOR_RED "OpenAL sound module is not active.\n");
		return;
	}
	Printf ("Sound backend: " TEXTCOLOR_GREEN "OpenAL Soft\n");
	Printf ("Device: " TEXTCOLOR_GREEN "%s\n", DeviceName.GetChars());
	Printf ("Vendor: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_VENDOR));
	Printf ("Renderer: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_RENDERER));
	Printf ("Version: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_VERSION));
	if (OutputRate > 0)
	{
		Printf ("Output sample rate: " TEXTCOLOR_GREEN "%d\n", OutputRate);
	}
	Printf ("AL_SOFT_loop_points: " TEXTCOLOR_GREEN "available\n");
	Printf ("SFX sources: " TEXTCOLOR_GREEN "%d allocated / %d requested / %d free / %d active\n", AllocatedSources, RequestedSources, AllocatedSources - (int)ActiveChannels.size (), (int)ActiveChannels.size ());
}

void OpenALSoundRenderer::PrintDriversList ()
{
	const ALCchar *devices;
	ALCenum specifier;
	int index = 0;

	if (alcIsExtensionPresent (NULL, "ALC_ENUMERATE_ALL_EXT"))
	{
		specifier = ALC_ALL_DEVICES_SPECIFIER;
	}
	else if (alcIsExtensionPresent (NULL, "ALC_ENUMERATION_EXT"))
	{
		specifier = ALC_DEVICE_SPECIFIER;
	}
	else
	{
		Printf ("OpenAL device enumeration is not supported.\n");
		return;
	}

	devices = alcGetString (NULL, specifier);
	for (const ALCchar *device = devices; device != NULL && *device != '\0'; device += strlen (device) + 1)
	{
		Printf ("%d. %s\n", index++, device);
	}
}

FString OpenALSoundRenderer::GatherStats ()
{
	FString out;
	out.Format ("%d SFX sources, %d active, %d free, 0 streams", AllocatedSources, (int)ActiveChannels.size (), AllocatedSources - (int)ActiveChannels.size ());
	return out;
}