#include "oalsound.h"

#include <AL/al.h>
#include <AL/alc.h>

#include "c_cvars.h"
#include "c_console.h"
#include "v_text.h"

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif

EXTERN_CVAR (Int, snd_channels)
EXTERN_CVAR (String, snd_openal_device)

OpenALSoundRenderer::OpenALSoundRenderer ()
	: Device (NULL), Context (NULL), Sources (NULL), RequestedSources (0),
	  AllocatedSources (0), OutputRate (0), InitSuccess (false)
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

void OpenALSoundRenderer::SetSfxVolume (float)
{
}

void OpenALSoundRenderer::SetMusicVolume (float)
{
}

SoundHandle OpenALSoundRenderer::LoadSound (BYTE *, int)
{
	SoundHandle handle = { NULL };
	return handle;
}

SoundHandle OpenALSoundRenderer::LoadSoundRaw (BYTE *, int, int, int, int, int, int)
{
	SoundHandle handle = { NULL };
	return handle;
}

void OpenALSoundRenderer::UnloadSound (SoundHandle)
{
}

unsigned int OpenALSoundRenderer::GetMSLength (SoundHandle)
{
	return 0;
}

unsigned int OpenALSoundRenderer::GetSampleLength (SoundHandle)
{
	return 0;
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

FISoundChannel *OpenALSoundRenderer::StartSound (SoundHandle, float, int, int, FISoundChannel *)
{
	return NULL;
}

FISoundChannel *OpenALSoundRenderer::StartSound3D (SoundHandle, SoundListener *, float, FRolloffInfo *, float, int, int, const FVector3 &, const FVector3 &, int, int, FISoundChannel *)
{
	return NULL;
}

void OpenALSoundRenderer::StopChannel (FISoundChannel *)
{
}

void OpenALSoundRenderer::ChannelVolume (FISoundChannel *, float)
{
}

void OpenALSoundRenderer::MarkStartTime (FISoundChannel *)
{
}

unsigned int OpenALSoundRenderer::GetPosition (FISoundChannel *)
{
	return 0;
}

float OpenALSoundRenderer::GetAudibility (FISoundChannel *)
{
	return 0.f;
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
	Printf ("SFX sources: " TEXTCOLOR_GREEN "%d allocated / %d requested / %d free / 0 active\n", AllocatedSources, RequestedSources, AllocatedSources);
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
	out.Format ("%d SFX sources, 0 active, %d free, 0 streams", AllocatedSources, AllocatedSources);
	return out;
}