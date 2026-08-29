#include "oalsound.h"

#include "oaldata.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <stdint.h>
#include <limits>
#include <math.h>
#include <string.h>

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

#ifndef AL_FORMAT_MONO_FLOAT32
#define AL_FORMAT_MONO_FLOAT32 0x10010
#define AL_FORMAT_STEREO_FLOAT32 0x10011
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

enum
{
	OALSTREAM_Mono = 1,
	OALSTREAM_Bits8 = 2,
	OALSTREAM_Bits32 = 4,
	OALSTREAM_Float = 8
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

OpenALSoundStream::OpenALSoundStream (OpenALSoundRenderer *owner, SoundStreamCallback callback, int bufferBytes, int flags, int sampleRate, void *userData)
	: Source (0), Owner (owner), Callback (callback), UserData (userData), SampleRate ((unsigned int)sampleRate),
	  StreamChannels ((flags & OALSTREAM_Mono) ? 1 : 2), InputBits ((flags & OALSTREAM_Float) ? 32 : (flags & OALSTREAM_Bits32) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16),
	  OutputBits (0), OutputFormat (0), ProcessedFrames (0), Volume (1.f), EndOfInput (false), Failed (false), Ended (false),
	  Playing (false), UserPaused (false), InactivePaused (false), InputIsFloat ((flags & OALSTREAM_Float) != 0), ResourcesReleased (false)
{
	unsigned int bytesPerInputFrame = StreamChannels * (InputBits / 8);
	memset (Buffers, 0, sizeof (Buffers));
	memset (BufferFrames, 0, sizeof (BufferFrames));
	InputBuffer.resize ((size_t)bufferBytes);
	if (flags & OALSTREAM_Float)
	{
		OutputBits = alIsExtensionPresent ("AL_EXT_FLOAT32") ? 32 : 16;
	}
	else
	{
		OutputBits = InputBits == 32 ? 16 : InputBits;
	}
	if (OutputBits == 32)
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
	}
	else if (OutputBits == 16)
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	}
	else
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8;
	}
	OutputBuffer.resize ((size_t)(bufferBytes / bytesPerInputFrame) * StreamChannels * (OutputBits / 8));
}

OpenALSoundStream::~OpenALSoundStream ()
{
	ReleaseResources ();
	if (Owner != NULL)
	{
		Owner->DestroyStream (this);
	}
}

bool OpenALSoundStream::ConvertBuffer ()
{
	size_t samples = (size_t)InputBuffer.size () / (InputBits / 8);
	if (InputBits == OutputBits && InputBits != 8)
	{
		OutputBuffer = InputBuffer;
		return true;
	}
	if (InputBits == 8)
	{
		for (size_t index = 0; index < samples; ++index)
		{
			OutputBuffer[index] = (BYTE)(InputBuffer[index] + 128);
		}
		return true;
	}
	for (size_t index = 0; index < samples; ++index)
	{
		short converted;
		if (InputBits == 32 && OutputBits == 16 && !InputBuffer.empty ())
		{
			if (InputIsFloat)
			{
				float value;
				memcpy (&value, &InputBuffer[index * sizeof (float)], sizeof (value));
				if (value > 1.f)
				{
					value = 1.f;
				}
				else if (value < -1.f)
				{
					value = -1.f;
				}
				converted = (short)(value * 32767.f);
			}
			else
			{
				int32_t value;
				memcpy (&value, &InputBuffer[index * sizeof (value)], sizeof (value));
				converted = (short)(value >> 16);
			}
			memcpy (&OutputBuffer[index * sizeof (converted)], &converted, sizeof (converted));
		}
	}
	return true;
}

bool OpenALSoundStream::QueueBuffer (unsigned int buffer)
{
	bool hasData;
	unsigned int bufferIndex = 0;
	unsigned int bytesPerFrame = StreamChannels * (InputBits / 8);
	for (; bufferIndex < 4 && Buffers[bufferIndex] != buffer; ++bufferIndex)
	{
	}
	if (bufferIndex == 4 || Callback == NULL || EndOfInput)
	{
		return false;
	}
#ifdef OAL_LIFECYCLE_TEST
	hasData = ((bool (*)(SoundStream *, void *, int, void *))Callback) (this, &InputBuffer[0], (int)InputBuffer.size (), UserData);
#else
	hasData = Callback (this, &InputBuffer[0], (int)InputBuffer.size (), UserData);
#endif
	if (!hasData)
	{
		EndOfInput = true;
		return false;
	}
	if (!ConvertBuffer ())
	{
		EndOfInput = true;
		return false;
	}
	alBufferData (buffer, (ALenum)OutputFormat, &OutputBuffer[0], (ALsizei)OutputBuffer.size (), (ALsizei)SampleRate);
	if (alGetError () != AL_NO_ERROR)
	{
		Failed = true;
		return false;
	}
	alSourceQueueBuffers (Source, 1, (ALuint *)&buffer);
	if (alGetError () != AL_NO_ERROR)
	{
		Failed = true;
		return false;
	}
	BufferFrames[bufferIndex] = (unsigned int)(InputBuffer.size () / bytesPerFrame);
	return true;
}

void OpenALSoundStream::ApplyGain ()
{
	bool muted = false;
	if (Owner != NULL)
	{
#ifdef OAL_LIFECYCLE_TEST
		muted = Owner->InactiveState == INACTIVE_Mute;
#else
		muted = Owner->InactiveState == SoundRenderer::INACTIVE_Mute;
#endif
	}
	float gain = InactivePaused || muted ? 0.f : Volume * Owner->MusicVolume;
	if (!ResourcesReleased)
	{
		alSourcef (Source, AL_GAIN, gain);
	}
}

void OpenALSoundStream::ApplyPauseState ()
{
	ALint queued = 0;
	if (!Playing || Ended || ResourcesReleased)
	{
		return;
	}
	if (UserPaused || InactivePaused)
	{
		alSourcePause (Source);
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (queued > 0)
	{
		alSourcePlay (Source);
	}
}

bool OpenALSoundStream::Play (bool, float volume)
{
	ALint queued = 0;
	if (ResourcesReleased || Ended || Failed)
	{
		return false;
	}
	Volume = volume;
	ApplyGain ();
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (queued == 0)
	{
		for (unsigned int index = 0; index < 4; ++index)
		{
			QueueBuffer (Buffers[index]);
			if (Failed)
			{
				return false;
			}
		}
		alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
		if (queued == 0)
		{
			Ended = EndOfInput;
			return false;
		}
	}
	Playing = true;
	ApplyPauseState ();
	return alGetError () == AL_NO_ERROR;
}

void OpenALSoundStream::Stop ()
{
	ALint queued = 0;
	if (ResourcesReleased)
	{
		return;
	}
	alSourceStop (Source);
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	while (queued-- > 0)
	{
		ALuint buffer;
		alSourceUnqueueBuffers (Source, 1, &buffer);
	}
	memset (BufferFrames, 0, sizeof (BufferFrames));
	ProcessedFrames = 0;
	Playing = false;
	EndOfInput = false;
	Failed = false;
	Ended = false;
	UserPaused = false;
}

void OpenALSoundStream::SetVolume (float volume)
{
	Volume = volume;
	ApplyGain ();
}

bool OpenALSoundStream::SetPaused (bool paused)
{
	if (ResourcesReleased || Ended || !Playing)
	{
		return false;
	}
	UserPaused = paused;
	ApplyPauseState ();
	return alGetError () == AL_NO_ERROR;
}

unsigned int OpenALSoundStream::GetPosition ()
{
	ALint offset = 0;
	unsigned long long frames = ProcessedFrames;
	if (!ResourcesReleased && Playing && !Ended)
	{
		alGetSourcei (Source, AL_SAMPLE_OFFSET, &offset);
		if (offset > 0)
		{
			frames = SaturatingAdd (frames, (unsigned int)offset);
		}
	}
	if (SampleRate == 0)
	{
		return 0;
	}
	unsigned long long seconds = frames / SampleRate;
	unsigned long long remainderFrames = frames % SampleRate;
	if (seconds > std::numeric_limits<unsigned int>::max () / 1000)
	{
		return std::numeric_limits<unsigned int>::max ();
	}
	unsigned long long milliseconds = seconds * 1000 + (remainderFrames * 1000) / SampleRate;
	return milliseconds > std::numeric_limits<unsigned int>::max () ?
		std::numeric_limits<unsigned int>::max () : (unsigned int)milliseconds;
}

bool OpenALSoundStream::IsEnded ()
{
	return Ended;
}

#ifdef OAL_LIFECYCLE_TEST
void OpenALSoundStream::SetProcessedFramesForTest (unsigned long long frames)
{
	ProcessedFrames = frames;
}
#endif

void OpenALSoundStream::SetInactive (bool paused)
{
	InactivePaused = paused;
	ApplyGain ();
	ApplyPauseState ();
}

void OpenALSoundStream::Update ()
{
	ALint processed = 0;
	ALint queued = 0;
	ALint state = AL_STOPPED;
	if (ResourcesReleased || Ended)
	{
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_PROCESSED, &processed);
	while (processed-- > 0)
	{
		ALuint buffer;
		unsigned int bufferIndex = 0;
		alSourceUnqueueBuffers (Source, 1, &buffer);
		if (alGetError () != AL_NO_ERROR)
		{
			Ended = true;
			Playing = false;
			return;
		}
		for (; bufferIndex < 4 && Buffers[bufferIndex] != buffer; ++bufferIndex)
		{
		}
		if (bufferIndex < 4)
		{
			ProcessedFrames = SaturatingAdd (ProcessedFrames, BufferFrames[bufferIndex]);
			BufferFrames[bufferIndex] = 0;
			QueueBuffer (buffer);
			if (Failed)
			{
				Ended = true;
				Playing = false;
				return;
			}
		}
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (EndOfInput && queued == 0)
	{
		Ended = true;
		Playing = false;
		return;
	}
	alGetSourcei (Source, AL_SOURCE_STATE, &state);
	if (Playing && !UserPaused && !InactivePaused && queued > 0 && state != AL_PLAYING)
	{
		alSourcePlay (Source);
	}
}

void OpenALSoundStream::ReleaseResources ()
{
	if (ResourcesReleased)
	{
		return;
	}
	ResourcesReleased = true;
	if (Source != 0)
	{
		alSourceStop (Source);
		alDeleteSources (1, (ALuint *)&Source);
		Source = 0;
	}
	alDeleteBuffers (4, (ALuint *)Buffers);
	memset (Buffers, 0, sizeof (Buffers));
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
		while (!ActiveStreams.empty ())
		{
			delete ActiveStreams.back ();
		}
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
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->ApplyGain ();
	}
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

SoundStream *OpenALSoundRenderer::CreateStream (SoundStreamCallback callback, int bufferBytes, int flags, int sampleRate, void *userData)
{
	OpenALSoundStream *stream;
	unsigned int inputBits;
	unsigned int channels;
	unsigned int bytesPerFrame;
	ALuint source = 0;
	ALuint buffers[4];
	ALint queued = 0;
	if (!InitSuccess || callback == NULL || bufferBytes <= 0 || sampleRate <= 0 ||
		(flags & ~(OALSTREAM_Mono | OALSTREAM_Bits8 | OALSTREAM_Bits32 | OALSTREAM_Float)) != 0 ||
		((flags & OALSTREAM_Bits8) != 0 && (flags & (OALSTREAM_Bits32 | OALSTREAM_Float)) != 0) ||
		((flags & OALSTREAM_Bits32) != 0 && (flags & OALSTREAM_Float) != 0))
	{
		return NULL;
	}
	channels = (flags & OALSTREAM_Mono) ? 1 : 2;
	inputBits = (flags & (OALSTREAM_Bits32 | OALSTREAM_Float)) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16;
	bytesPerFrame = channels * (inputBits / 8);
	if (bufferBytes % (int)bytesPerFrame != 0)
	{
		return NULL;
	}
	memset (buffers, 0, sizeof (buffers));
	alGetError ();
	alGenSources (1, &source);
	alGenBuffers (4, buffers);
	if (alGetError () != AL_NO_ERROR)
	{
		if (source != 0)
		{
			alDeleteSources (1, &source);
		}
		alDeleteBuffers (4, buffers);
		return NULL;
	}
	stream = new OpenALSoundStream (this, callback, bufferBytes, flags, sampleRate, userData);
	stream->Source = source;
	memcpy (stream->Buffers, buffers, sizeof (buffers));
	stream->SetInactive (InactiveState == INACTIVE_Complete);
	ActiveStreams.push_back (stream);
	return stream;
}

SoundStream *OpenALSoundRenderer::OpenStream (const char *, int, int, int)
{
	Printf (TEXTCOLOR_RED "OpenAL does not support encoded music streams; use a software PCM music renderer.\n");
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
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->SetInactive (inactive == INACTIVE_Complete);
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
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->Update ();
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
	out.Format ("%d SFX sources, %d active, %d free, %d streams", AllocatedSources, (int)ActiveChannels.size (), AllocatedSources - (int)ActiveChannels.size (), (int)ActiveStreams.size ());
	return out;
}

void OpenALSoundRenderer::DestroyStream (OpenALSoundStream *stream)
{
	std::vector<OpenALSoundStream *>::iterator found = std::find (ActiveStreams.begin (), ActiveStreams.end (), stream);
	if (found != ActiveStreams.end ())
	{
		ActiveStreams.erase (found);
	}
}