#include "oalsound.h"

#include "oaldata.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <limits>

#ifdef OAL_LIFECYCLE_TEST
#include "oalsound_test_support.h"
#else
#include "c_cvars.h"
#include "c_console.h"
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

OpenALSound::OpenALSound ()
	: Buffer2D (0), BufferMono (0), SampleRate (0), Frames (0), Channels (0),
	  HasLoop (false), LoopStart (0), LoopEnd (0), References (0), DeferredDelete (false)
{
}

OpenALChannel::OpenALChannel ()
	: Source (0), Sound (NULL), Owner (NULL), Gain (0.f), Pitch (1.f), Priority (0),
	  CachedPosition (0), AllocationSerial (0), Looping (false), EndReason (OALEND_None),
	  FinalizeState (OALFINAL_Active)
{
}

OpenALSoundRenderer::OpenALSoundRenderer ()
	: Device (NULL), Context (NULL), Sources (NULL), RequestedSources (0),
	  AllocatedSources (0), OutputRate (0), InitSuccess (false), SfxVolume (1.f),
	  MusicVolume (1.f), NextAllocationSerial (0)
#ifdef OAL_LIFECYCLE_TEST
	  , FailNextStart (false)
#endif
{
	InitSuccess = Init ();
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
			(channel->Priority == candidate->Priority && (channel->Gain < candidate->Gain ||
			(channel->Gain == candidate->Gain && channel->AllocationSerial < candidate->AllocationSerial))))
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

bool OpenALSoundRenderer::IncomingWins (const OpenALChannel *candidate, int priority, float gain) const
{
	return candidate != NULL && (priority > candidate->Priority ||
		(priority == candidate->Priority && gain > candidate->Gain));
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
	alSourcef (channel->Source, AL_GAIN, channel->Gain * SfxVolume);
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
	FISoundChannel *owner = NULL;
	unsigned int source;
	if (!InitSuccess || sound == NULL || sound->DeferredDelete || reuseChan != NULL && reuseChan->SysChannel != NULL)
	{
		return NULL;
	}
	source = FindFreeSource ();
	if (source == 0 && FinalizePendingStopForReuse ())
	{
		source = FindFreeSource ();
	}
	if (source == 0)
	{
		OpenALChannel *candidate = FindEvictionCandidate ();
		if (!IncomingWins (candidate, priority, volume))
		{
			return NULL;
		}
		FinalizeChannel (candidate, OALEND_PoolEviction);
		source = FindFreeSource ();
		if (source == 0)
		{
			return NULL;
		}
	}
	channel = new OpenALChannel;
	channel->Source = source;
	channel->Sound = sound;
	channel->Owner = owner;
	channel->Gain = volume;
	channel->Pitch = snd_pitched ? pitch / 128.f : 1.f;
	channel->Priority = priority;
	channel->Looping = (flags & SNDF_LOOP) != 0;
	channel->AllocationSerial = ++NextAllocationSerial;

	alGetError ();
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)sound->Buffer2D);
	alSourcei (source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f (source, AL_POSITION, 0.f, 0.f, 0.f);
	alSource3f (source, AL_VELOCITY, 0.f, 0.f, 0.f);
	alSourcei (source, AL_LOOPING, channel->Looping ? AL_TRUE : AL_FALSE);
	alSourcef (source, AL_PITCH, channel->Pitch);
	ApplyChannelGain (channel);
	alSourcePlay (source);
#ifdef OAL_LIFECYCLE_TEST
	if (FailNextStart)
	{
		FailNextStart = false;
		alSourcei (source, 0, 0);
	}
#endif
	if (alGetError () != AL_NO_ERROR)
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	owner = reuseChan != NULL ? reuseChan : S_GetChannel (channel);
	if (owner == NULL)
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	channel->Owner = owner;
	owner->SysChannel = channel;
	++sound->References;
	ActiveChannels.push_back (channel);
	return owner;
}

FISoundChannel *OpenALSoundRenderer::StartSound (SoundHandle sfx, float volume, int pitch, int priority, int flags, FISoundChannel *reuseChan)
{
	return Start2D (sfx, volume, pitch, flags, priority, reuseChan);
}

FISoundChannel *OpenALSoundRenderer::StartSound3D (SoundHandle, SoundListener *, float, FRolloffInfo *, float, int, int, const FVector3 &, const FVector3 &, int, int, FISoundChannel *)
{
	return NULL;
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

void OpenALSoundRenderer::MarkStartTime (FISoundChannel *)
{
}

unsigned int OpenALSoundRenderer::GetPosition (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	return channel == NULL ? 0 : CachePosition (channel);
}

float OpenALSoundRenderer::GetAudibility (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	return channel == NULL ? 0.f : channel->Gain * SfxVolume;
}

void OpenALSoundRenderer::Sync (bool)
{
}

void OpenALSoundRenderer::SetSfxPaused (bool, int)
{
}

void OpenALSoundRenderer::SetInactive (EInactiveState)
{
}

void OpenALSoundRenderer::UpdateSoundParams3D (SoundListener *, FISoundChannel *, bool, const FVector3 &, const FVector3 &)
{
}

void OpenALSoundRenderer::UpdateListener (SoundListener *)
{
}

void OpenALSoundRenderer::UpdateSounds ()
{
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