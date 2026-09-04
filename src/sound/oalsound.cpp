#include "oalsound.h"

#include "audio_decoder.h"
#include "oaldata.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <stdint.h>
#include <limits>
#include <math.h>
#include <new>
#include <stdlib.h>
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

namespace
{
	const std::size_t MaxOpenALEncodedInputBytes = 64 * 1024 * 1024;

	bool AcceptsOpenALEncodedInputSize (std::size_t bytes)
	{
		return bytes <= MaxOpenALEncodedInputBytes;
	}

#ifdef OAL_LIFECYCLE_TEST
	bool FailNextOpenALEncodedSourceAssign = false;
#endif

	bool AssignOpenALEncodedSource (AudioMemorySource &source, const void *data, std::size_t bytes)
	{
		if (!AcceptsOpenALEncodedInputSize (bytes))
		{
			return false;
		}
		try
		{
	#ifdef OAL_LIFECYCLE_TEST
			if (FailNextOpenALEncodedSourceAssign)
			{
				FailNextOpenALEncodedSourceAssign = false;
				throw std::bad_alloc ();
			}
	#endif
			return source.Assign (data, bytes) == AUDIO_SOURCE_OK;
		}
		catch (const std::bad_alloc &)
		{
			return false;
		}
	}
}

#ifdef OAL_LIFECYCLE_TEST
void OALTestFailNextEncodedSourceAssign ()
{
	FailNextOpenALEncodedSourceAssign = true;
}

bool OALTestAcceptsEncodedInputSize (unsigned int bytes)
{
	return AcceptsOpenALEncodedInputSize (bytes);
}
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

enum
{
	OALAL_SourceQuery = 1,
	OALAL_SourcePlay,
	OALAL_SourcePause,
	OALAL_BufferUpload,
	OALAL_BufferQueue,
	OALAL_BufferUnqueue,
	OALAL_SourceStop,
	OALAL_SourceGain,
	OALAL_SourcePositionQuery
};

enum OpenALProducerReadStatus
{
	OALPRODUCER_Data,
	OALPRODUCER_EOF,
	OALPRODUCER_Error
};

class OpenALStreamProducer
{
public:
	virtual ~OpenALStreamProducer () {}
	virtual OpenALProducerReadStatus ReadFrames (OpenALSoundStream *stream, BYTE *data, unsigned int frameCount, unsigned int bytesPerFrame, unsigned int *framesRead) = 0;
	virtual bool GetTotalFrames (unsigned long long *frames) const = 0;
	virtual unsigned long long TellFrame () const = 0;
	virtual bool GetLoopRange (AudioFrameRange *range) const = 0;
	virtual AudioDecoderSeekStatus SeekFrame (unsigned long long frame) = 0;
#ifdef OAL_LIFECYCLE_TEST
	virtual void SetTestAllowArbitrarySeek () {}
#endif
};

class CallbackStreamProducer : public OpenALStreamProducer
{
public:
	CallbackStreamProducer (SoundStreamCallback callback, void *userData)
		: Callback (callback), UserData (userData), Position (0)
#ifdef OAL_LIFECYCLE_TEST
		, TestAllowArbitrarySeek (false)
#endif
	{
	}

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *stream, BYTE *data, unsigned int frameCount, unsigned int bytesPerFrame, unsigned int *framesRead)
	{
		bool hasData;
		if (framesRead == NULL || Callback == NULL)
		{
			return OALPRODUCER_Error;
		}
#ifdef OAL_LIFECYCLE_TEST
		hasData = ((bool (*)(SoundStream *, void *, int, void *))Callback) (stream, data, (int)(frameCount * bytesPerFrame), UserData);
#else
		hasData = Callback (stream, data, (int)(frameCount * bytesPerFrame), UserData);
#endif
		if (!hasData)
		{
			*framesRead = 0;
			return OALPRODUCER_EOF;
		}
		*framesRead = frameCount;
		Position = std::numeric_limits<unsigned long long>::max () - Position < frameCount ?
			std::numeric_limits<unsigned long long>::max () : Position + frameCount;
		return OALPRODUCER_Data;
	}

	bool GetTotalFrames (unsigned long long *) const { return false; }
	unsigned long long TellFrame () const { return Position; }
	bool GetLoopRange (AudioFrameRange *) const { return false; }
	AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
	{
		if (frame != 0
#ifdef OAL_LIFECYCLE_TEST
			&& !TestAllowArbitrarySeek
#endif
			)
		{
			return AUDIO_DECODER_SEEK_ERROR;
		}
		Position = frame;
		return AUDIO_DECODER_SEEK_OK;
	}

#ifdef OAL_LIFECYCLE_TEST
	void SetTestAllowArbitrarySeek ()
	{
		TestAllowArbitrarySeek = true;
	}
#endif

private:
	SoundStreamCallback Callback;
	void *UserData;
	unsigned long long Position;
#ifdef OAL_LIFECYCLE_TEST
	bool TestAllowArbitrarySeek;
#endif
};

class DecoderStreamProducer : public OpenALStreamProducer
{
public:
	explicit DecoderStreamProducer (AudioDecoder *decoder) : Decoder (decoder) {}
	~DecoderStreamProducer () { delete Decoder; }

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *, BYTE *data, unsigned int frameCount, unsigned int, unsigned int *framesRead)
	{
		std::size_t read = 0;
		AudioDecoderReadStatus status;
		if (Decoder == NULL || framesRead == NULL)
		{
			return OALPRODUCER_Error;
		}
		status = Decoder->ReadFrames ((short *)data, frameCount, &read);
		if (read > frameCount)
		{
			return OALPRODUCER_Error;
		}
		*framesRead = (unsigned int)read;
		return status == AUDIO_DECODER_DATA ? OALPRODUCER_Data :
			status == AUDIO_DECODER_EOF ? OALPRODUCER_EOF : OALPRODUCER_Error;
	}

	bool GetTotalFrames (unsigned long long *frames) const { return Decoder != NULL && Decoder->GetTotalFrames (frames); }
	unsigned long long TellFrame () const { return Decoder == NULL ? 0 : Decoder->TellFrame (); }
	bool GetLoopRange (AudioFrameRange *range) const { return Decoder != NULL && Decoder->GetLoopRange (range); }
	AudioDecoderSeekStatus SeekFrame (unsigned long long frame) { return Decoder == NULL ? AUDIO_DECODER_SEEK_TERMINAL_ERROR : Decoder->SeekFrame (frame); }

private:
	AudioDecoder *Decoder;
};

#ifdef OAL_LIFECYCLE_TEST
class PatternStreamProducer : public OpenALStreamProducer
{
public:
	PatternStreamProducer (unsigned int totalFrames, unsigned int loopStart, unsigned int loopEnd)
		: TotalFrames (totalFrames), LoopStart (loopStart), LoopEnd (loopEnd), Position (0)
	{
	}

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *, BYTE *data, unsigned int frameCount, unsigned int, unsigned int *framesRead)
	{
		unsigned int available;
		unsigned int count;
		short *samples;
		if (data == NULL || framesRead == NULL)
		{
			return OALPRODUCER_Error;
		}
		if (Position >= TotalFrames)
		{
			*framesRead = 0;
			return OALPRODUCER_EOF;
		}
		available = TotalFrames - Position;
		count = frameCount < available ? frameCount : available;
		samples = (short *)data;
		for (unsigned int index = 0; index < count; ++index)
		{
			samples[index] = (short)(Position + index);
		}
		Position += count;
		*framesRead = count;
		return OALPRODUCER_Data;
	}

	bool GetTotalFrames (unsigned long long *frames) const
	{
		if (frames == NULL)
		{
			return false;
		}
		*frames = TotalFrames;
		return true;
	}

	unsigned long long TellFrame () const { return Position; }
	bool GetLoopRange (AudioFrameRange *range) const
	{
		if (range == NULL || LoopStart >= LoopEnd || LoopEnd > TotalFrames)
		{
			return false;
		}
		range->Start = LoopStart;
		range->End = LoopEnd;
		return true;
	}

	AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
	{
		if (frame > TotalFrames)
		{
			return AUDIO_DECODER_SEEK_ERROR;
		}
		Position = (unsigned int)frame;
		return AUDIO_DECODER_SEEK_OK;
	}

private:
	unsigned int TotalFrames;
	unsigned int LoopStart;
	unsigned int LoopEnd;
	unsigned int Position;
};
#endif

static AudioDataSource *OpenEncodedMusicSource (const char *filename, int offset, int length, AudioFileSource *file, AudioMemorySource *memory)
{
	if (offset == -1)
	{
		if (length == 0 || memory->Assign (filename, (size_t)length) != AUDIO_SOURCE_OK)
		{
			Printf (TEXTCOLOR_RED "OpenAL could not read encoded music data.\n");
			return NULL;
		}
		return memory;
	}
	if ((offset == 0 && length == 0 && file->Open (filename) == AUDIO_SOURCE_OK) ||
		(length > 0 && file->OpenSlice (filename, (size_t)offset, (size_t)length) == AUDIO_SOURCE_OK))
	{
		return file;
	}
	Printf (TEXTCOLOR_RED "OpenAL could not open encoded music data.\n");
	return NULL;
}

static DecoderStreamProducer *CreateEncodedMusicProducer (AudioDataSource *source, unsigned int *channels, unsigned int *sampleRate)
{
	AudioProbeResult probe = ProbeAudioFormat (*source);
	AudioDecoder *decoder = NULL;
	AudioDecodeStatus status = CreateAudioDecoder (*source, probe, &decoder);
	DecoderStreamProducer *producer;
	if (status != AUDIO_DECODE_OK || decoder == NULL || decoder->GetNativeSampleRate () == 0 ||
		(decoder->GetOutputChannels () != 1 && decoder->GetOutputChannels () != 2))
	{
		delete decoder;
		Printf (TEXTCOLOR_RED "OpenAL does not support this encoded music stream.\n");
		return NULL;
	}
	*channels = decoder->GetOutputChannels ();
	*sampleRate = decoder->GetNativeSampleRate ();
	producer = new (std::nothrow) DecoderStreamProducer (decoder);
	if (producer == NULL)
	{
		delete decoder;
	}
	return producer;
}

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
	: Source (0), Owner (owner), Producer (new (std::nothrow) CallbackStreamProducer (callback, userData)), SampleRate ((unsigned int)sampleRate),
	  StreamChannels ((flags & OALSTREAM_Mono) ? 1 : 2), InputBits ((flags & OALSTREAM_Float) ? 32 : (flags & OALSTREAM_Bits32) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16),
	  OutputBits (0), OutputFormat (0), MediaFrame (0), LoopStart (0), LoopEnd (0), Volume (1.f), EndOfInput (false), Looping (false), HasLoopRange (false),
	  UserPaused (false), InactivePaused (false), InputIsFloat ((flags & OALSTREAM_Float) != 0), ResourcesReleased (false), State (OALSTREAM_Stopped)
#ifdef OAL_LIFECYCLE_TEST
	  , TestFailNextRewind (false), TestRewindTerminal (false), TestFailNextBufferUpload (false), TestFailNextALOperation (0)
#endif
{
	InitializeBuffers (bufferBytes, flags);
}

OpenALSoundStream::OpenALSoundStream (OpenALSoundRenderer *owner, OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate)
	: Source (0), Owner (owner), Producer (producer), SampleRate ((unsigned int)sampleRate),
	  StreamChannels ((flags & OALSTREAM_Mono) ? 1 : 2), InputBits (16), OutputBits (0), OutputFormat (0), MediaFrame (0), LoopStart (0), LoopEnd (0),
	  Volume (1.f), EndOfInput (false), Looping (false), HasLoopRange (false), UserPaused (false), InactivePaused (false), InputIsFloat (false),
	  ResourcesReleased (false), State (OALSTREAM_Stopped)
#ifdef OAL_LIFECYCLE_TEST
	  , TestFailNextRewind (false), TestRewindTerminal (false), TestFailNextBufferUpload (false), TestFailNextALOperation (0)
#endif
{
	InitializeBuffers (bufferBytes, flags);
}

void OpenALSoundStream::InitializeBuffers (int bufferBytes, int flags)
{
	unsigned int bytesPerInputFrame = StreamChannels * (InputBits / 8);
	memset (Buffers, 0, sizeof (Buffers));
	memset (BufferFrames, 0, sizeof (BufferFrames));
	memset (BufferMediaStart, 0, sizeof (BufferMediaStart));
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
	delete Producer;
	Producer = NULL;
	if (Owner != NULL)
	{
		Owner->DestroyStream (this);
	}
}

bool OpenALSoundStream::ConvertBuffer (unsigned int frames)
{
	size_t samples = (size_t)frames * StreamChannels;
	if (InputBits == OutputBits && InputBits != 8)
	{
		memcpy (&OutputBuffer[0], &InputBuffer[0], samples * (InputBits / 8));
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
	unsigned int bufferIndex = 0;
	unsigned int bytesPerFrame = StreamChannels * (InputBits / 8);
	unsigned int framesRead = 0;
	unsigned int requestedFrames = (unsigned int)(InputBuffer.size () / bytesPerFrame);
	unsigned long long mediaStart;
	if (!FindBufferIndex (buffer, &bufferIndex) || Producer == NULL || EndOfInput || State == OALSTREAM_Failed)
	{
		return false;
	}
	if (!ReadBufferFrames (requestedFrames, bytesPerFrame, &mediaStart, &framesRead))
	{
		return false;
	}
	if (!ConvertBuffer (framesRead))
	{
		SetFailed ();
		return false;
	}
	return SubmitBuffer (buffer, bufferIndex, mediaStart, framesRead);
}

bool OpenALSoundStream::FindBufferIndex (unsigned int buffer, unsigned int *bufferIndex) const
{
	for (*bufferIndex = 0; *bufferIndex < 4 && Buffers[*bufferIndex] != buffer; ++*bufferIndex)
	{
	}
	return *bufferIndex < 4;
}

bool OpenALSoundStream::ReadBufferFrames (unsigned int requestedFrames, unsigned int bytesPerFrame, unsigned long long *mediaStart, unsigned int *framesRead)
{
	OpenALProducerReadStatus status;
	*mediaStart = Producer->TellFrame ();
	if (Looping && HasLoopRange && *mediaStart >= LoopEnd)
	{
		if (!RewindProducer (LoopStart))
		{
			return false;
		}
		*mediaStart = Producer->TellFrame ();
	}
	if (Looping && HasLoopRange && *mediaStart < LoopEnd && requestedFrames > LoopEnd - *mediaStart)
	{
		requestedFrames = (unsigned int)(LoopEnd - *mediaStart);
	}
	status = Producer->ReadFrames (this, &InputBuffer[0], requestedFrames, bytesPerFrame, framesRead);
	if (status == OALPRODUCER_EOF && Looping)
	{
		if (!RewindProducer (HasLoopRange ? LoopStart : 0))
		{
			return false;
		}
		*mediaStart = Producer->TellFrame ();
		status = Producer->ReadFrames (this, &InputBuffer[0], requestedFrames, bytesPerFrame, framesRead);
	}
	if (status == OALPRODUCER_EOF)
	{
		EndOfInput = true;
		return false;
	}
	if (status != OALPRODUCER_Data || *framesRead == 0 || *framesRead > requestedFrames)
	{
		SetFailed ();
		return false;
	}
	return true;
}

bool OpenALSoundStream::SubmitBuffer (unsigned int buffer, unsigned int bufferIndex, unsigned long long mediaStart, unsigned int framesRead)
{
#ifdef OAL_LIFECYCLE_TEST
	if (TestFailNextBufferUpload || ConsumeTestALFailure (OALAL_BufferUpload))
	{
		TestFailNextBufferUpload = false;
		SetFailed ();
		return false;
	}
#endif
	alBufferData (buffer, (ALenum)OutputFormat, &OutputBuffer[0], (ALsizei)(framesRead * StreamChannels * (OutputBits / 8)), (ALsizei)SampleRate);
	if (!CheckALError (OALAL_BufferUpload))
	{
		return false;
	}
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (OALAL_BufferQueue))
	{
		return false;
	}
#endif
	alSourceQueueBuffers (Source, 1, (ALuint *)&buffer);
	if (!CheckALError (OALAL_BufferQueue))
	{
		return false;
	}
	BufferFrames[bufferIndex] = framesRead;
	BufferMediaStart[bufferIndex] = mediaStart;
	QueueOrder.push_back (buffer);
	return true;
}

bool OpenALSoundStream::RecycleProcessedBuffer ()
{
	ALuint buffer;
	unsigned int bufferIndex = 0;
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (OALAL_BufferUnqueue))
	{
		return false;
	}
#endif
	alSourceUnqueueBuffers (Source, 1, &buffer);
	if (!CheckALError (OALAL_BufferUnqueue))
	{
		return false;
	}
	if (FindBufferIndex (buffer, &bufferIndex))
	{
		MediaFrame = NormalizeMediaFrame (BufferMediaStart[bufferIndex] + BufferFrames[bufferIndex]);
		BufferFrames[bufferIndex] = 0;
		if (!QueueOrder.empty ())
		{
			QueueOrder.erase (QueueOrder.begin ());
		}
		QueueBuffer (buffer);
		if (State == OALSTREAM_Failed)
		{
			return false;
		}
	}
	return true;
}

void OpenALSoundStream::UpdatePlaybackState ()
{
	ALint queued = 0;
	ALint state = AL_STOPPED;
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if (EndOfInput && queued == 0)
	{
		State = OALSTREAM_Ended;
		return;
	}
	if (EndOfInput && State != OALSTREAM_Paused)
	{
		State = OALSTREAM_Draining;
	}
	alGetSourcei (Source, AL_SOURCE_STATE, &state);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if ((State == OALSTREAM_Playing || State == OALSTREAM_Draining) && !UserPaused && !InactivePaused && queued > 0 && state != AL_PLAYING)
	{
		alSourcePlay (Source);
		CheckALError (OALAL_SourcePlay);
	}
}

void OpenALSoundStream::Update ()
{
	ALint processed = 0;
	if (ResourcesReleased || State == OALSTREAM_Ended || State == OALSTREAM_Failed)
	{
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_PROCESSED, &processed);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	while (processed-- > 0)
	{
		if (!RecycleProcessedBuffer ())
		{
			return;
		}
	}
	UpdatePlaybackState ();
}

bool OpenALSoundStream::CheckALError (unsigned int operation)
{
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (operation)) return false;
#endif
	if (alGetError () == AL_NO_ERROR)
	{
		return true;
	}
	SetFailed ();
	return false;
}

#ifdef OAL_LIFECYCLE_TEST
bool OpenALSoundStream::ConsumeTestALFailure (unsigned int operation)
{
	if (TestFailNextALOperation != operation)
	{
		return false;
	}
	TestFailNextALOperation = 0;
	SetFailed ();
	return true;
}
#endif

void OpenALSoundStream::SetFailed ()
{
	if (State == OALSTREAM_Failed)
	{
		return;
	}
	State = OALSTREAM_Failed;
	EndOfInput = true;
	ClearQueuedBuffers (false);
}

bool OpenALSoundStream::RewindProducer (unsigned long long frame)
{
	#ifdef OAL_LIFECYCLE_TEST
	if (TestFailNextRewind)
	{
		bool terminal = TestRewindTerminal;
		TestFailNextRewind = false;
		if (terminal)
		{
			SetFailed ();
		}
		return false;
	}
	#endif
	AudioDecoderSeekStatus status = Producer == NULL ? AUDIO_DECODER_SEEK_TERMINAL_ERROR : Producer->SeekFrame (frame);
	if (status == AUDIO_DECODER_SEEK_OK)
	{
		return true;
	}
	if (status == AUDIO_DECODER_SEEK_TERMINAL_ERROR)
	{
		SetFailed ();
	}
	return false;
}

bool OpenALSoundStream::ConfigureLoop ()
{
	unsigned long long total = 0;
	AudioFrameRange range;
	HasLoopRange = false;
	LoopStart = 0;
	LoopEnd = 0;
	if (!Looping || Producer == NULL)
	{
		return true;
	}
	if (!Producer->GetTotalFrames (&total) || total == 0)
	{
		return true;
	}
	if (Producer->GetLoopRange (&range) && range.Start < range.End && range.End <= total)
	{
		LoopStart = range.Start;
		LoopEnd = range.End;
	}
	else
	{
		LoopEnd = total;
	}
	HasLoopRange = true;
	return true;
}

unsigned long long OpenALSoundStream::NormalizeMediaFrame (unsigned long long frame) const
{
	if (Looping && HasLoopRange && frame >= LoopStart && LoopEnd > LoopStart)
	{
		return LoopStart + (frame - LoopStart) % (LoopEnd - LoopStart);
	}
	return frame;
}

bool OpenALSoundStream::ClearQueuedBuffers (bool failOnError)
{
	ALint queued = 0;
	ALenum error;
	if (ResourcesReleased)
	{
		return false;
	}
	alGetError ();
	alSourceStop (Source);
	error = alGetError ();
	if (error != AL_NO_ERROR)
	{
		if (failOnError)
		{
			SetFailed ();
		}
		return false;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	error = alGetError ();
	if (error != AL_NO_ERROR)
	{
		if (failOnError)
		{
			SetFailed ();
		}
		return false;
	}
	while (queued-- > 0)
	{
		ALuint buffer;
		alSourceUnqueueBuffers (Source, 1, &buffer);
		if (alGetError () != AL_NO_ERROR)
		{
			if (failOnError)
			{
				SetFailed ();
			}
			return false;
		}
	}
	memset (BufferFrames, 0, sizeof (BufferFrames));
	memset (BufferMediaStart, 0, sizeof (BufferMediaStart));
	QueueOrder.clear ();
	return true;
}

unsigned long long OpenALSoundStream::GetCurrentMediaFrame ()
{
	ALint offset = 0;
	if (!ResourcesReleased && (State == OALSTREAM_Playing || State == OALSTREAM_Draining) && !QueueOrder.empty ())
	{
		unsigned int buffer = QueueOrder[0];
		unsigned int index = 0;
		for (; index < 4 && Buffers[index] != buffer; ++index) {}
		alGetSourcei (Source, AL_SAMPLE_OFFSET, &offset);
		if (!CheckALError (OALAL_SourcePositionQuery))
		{
			return MediaFrame;
		}
		if (index < 4 && offset > 0)
		{
			unsigned int limited = (unsigned int)offset > BufferFrames[index] ? BufferFrames[index] : (unsigned int)offset;
			MediaFrame = NormalizeMediaFrame (BufferMediaStart[index] + limited);
		}
	}
	return MediaFrame;
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
		CheckALError (OALAL_SourceGain);
	}
}

void OpenALSoundStream::ApplyPauseState ()
{
	ALint queued = 0;
	if ((State != OALSTREAM_Playing && State != OALSTREAM_Draining && State != OALSTREAM_Paused) || ResourcesReleased)
	{
		return;
	}
	if (UserPaused || InactivePaused)
	{
		alSourcePause (Source);
		if (!CheckALError (OALAL_SourcePause))
		{
			return;
		}
		State = OALSTREAM_Paused;
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if (queued > 0)
	{
		alSourcePlay (Source);
		if (!CheckALError (OALAL_SourcePlay))
		{
			return;
		}
		if (State == OALSTREAM_Paused)
		{
			State = EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing;
		}
	}
}

bool OpenALSoundStream::Play (bool looping, float volume)
{
	ALint queued = 0;
	if (ResourcesReleased || State == OALSTREAM_Failed || Producer == NULL)
	{
		return false;
	}
	if (State == OALSTREAM_Ended)
	{
		if (!RewindProducer (0))
		{
			return false;
		}
		EndOfInput = false;
		MediaFrame = 0;
		UserPaused = false;
		State = OALSTREAM_Stopped;
	}
	Looping = looping;
	if (!ConfigureLoop ())
	{
		return false;
	}
	Volume = volume;
	ApplyGain ();
	if (State == OALSTREAM_Failed)
	{
		return false;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return false;
	}
	if (queued == 0)
	{
		for (unsigned int index = 0; index < 4; ++index)
		{
			QueueBuffer (Buffers[index]);
			if (State == OALSTREAM_Failed)
			{
				return false;
			}
		}
		alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
		if (!CheckALError (OALAL_SourceQuery))
		{
			return false;
		}
		if (queued == 0)
		{
			State = EndOfInput ? OALSTREAM_Ended : OALSTREAM_Stopped;
			return false;
		}
	}
	State = EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing;
	ApplyPauseState ();
	return State != OALSTREAM_Failed;
}

void OpenALSoundStream::Stop ()
{
	if (ResourcesReleased || State == OALSTREAM_Failed)
	{
		return;
	}
	if (!RewindProducer (0))
	{
		return;
	}
	if (!ClearQueuedBuffers ())
	{
		return;
	}
	MediaFrame = 0;
	EndOfInput = false;
	Looping = false;
	HasLoopRange = false;
	UserPaused = false;
	State = OALSTREAM_Stopped;
}

void OpenALSoundStream::SetVolume (float volume)
{
	Volume = volume;
	ApplyGain ();
}

bool OpenALSoundStream::SetPaused (bool paused)
{
	if (ResourcesReleased || State == OALSTREAM_Failed || State == OALSTREAM_Ended ||
		(State != OALSTREAM_Playing && State != OALSTREAM_Draining && State != OALSTREAM_Paused))
	{
		return false;
	}
	GetCurrentMediaFrame ();
	if (State == OALSTREAM_Failed)
	{
		return false;
	}
	UserPaused = paused;
	ApplyPauseState ();
	return State != OALSTREAM_Failed;
}

unsigned int OpenALSoundStream::GetPosition ()
{
	unsigned long long frames = GetCurrentMediaFrame ();
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
	return State == OALSTREAM_Ended || State == OALSTREAM_Failed;
}

#ifdef OAL_LIFECYCLE_TEST
void OpenALSoundStream::SetProcessedFramesForTest (unsigned long long frames)
{
	MediaFrame = frames;
}

void OpenALSoundStream::FailNextRewindForTest (bool terminal)
{
	TestFailNextRewind = true;
	TestRewindTerminal = terminal;
}

void OpenALSoundStream::FailNextBufferUploadForTest ()
{
	TestFailNextBufferUpload = true;
}

void OpenALSoundStream::FailNextALOperationForTest (OpenALStreamTestALOperation operation)
{
	switch (operation)
	{
	case OALTESTAL_SourceQuery: TestFailNextALOperation = OALAL_SourceQuery; break;
	case OALTESTAL_SourcePlay: TestFailNextALOperation = OALAL_SourcePlay; break;
	case OALTESTAL_SourcePause: TestFailNextALOperation = OALAL_SourcePause; break;
	case OALTESTAL_BufferUpload: TestFailNextALOperation = OALAL_BufferUpload; break;
	case OALTESTAL_BufferQueue: TestFailNextALOperation = OALAL_BufferQueue; break;
	case OALTESTAL_BufferUnqueue: TestFailNextALOperation = OALAL_BufferUnqueue; break;
	case OALTESTAL_PositionQuery: TestFailNextALOperation = OALAL_SourcePositionQuery; break;
	default: TestFailNextALOperation = 0; break;
	}
}

bool OpenALSoundStream::ProcessNextBufferForTest ()
{
	return RecycleProcessedBuffer ();
}

void OpenALSoundStream::AllowArbitrarySeekForTest ()
{
	if (Producer != NULL)
	{
		Producer->SetTestAllowArbitrarySeek ();
	}
}

OpenALStreamState OpenALSoundStream::GetStateForTest () const
{
	return State;
}

unsigned int OpenALSoundStream::GetQueuedBufferCountForTest () const
{
	ALint queued = 0;
	if (!ResourcesReleased)
	{
		alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	}
	return queued > 0 ? (unsigned int)queued : 0;
}

unsigned long long OpenALSoundStream::GetBufferMediaStartForTest (unsigned int buffer) const
{
	return buffer < 4 ? BufferMediaStart[buffer] : 0;
}

unsigned int OpenALSoundStream::GetBufferFramesForTest (unsigned int buffer) const
{
	return buffer < 4 ? BufferFrames[buffer] : 0;
}

unsigned long long OpenALSoundStream::GetMediaFrameForTest () const
{
	return MediaFrame;
}
#endif

bool OpenALSoundStream::SetPosition (unsigned int milliseconds)
{
	unsigned long long total;
	unsigned long long frame;
	OpenALStreamState oldState;
	bool wasUserPaused;
	bool wasInactivePaused;
	bool hasTotal;
	if (ResourcesReleased || State == OALSTREAM_Failed || Producer == NULL || SampleRate == 0)
	{
		return false;
	}
	frame = ((unsigned long long)milliseconds * SampleRate) / 1000;
	hasTotal = Producer->GetTotalFrames (&total);
	if (hasTotal && frame > total)
	{
		return false;
	}
	if (!RewindProducer (frame))
	{
		return false;
	}
	oldState = State;
	wasUserPaused = UserPaused;
	wasInactivePaused = InactivePaused;
	if (!ClearQueuedBuffers ())
	{
		return false;
	}
	MediaFrame = frame;
	EndOfInput = hasTotal && frame == total;
	if (EndOfInput)
	{
		State = OALSTREAM_Ended;
		return true;
	}
	State = oldState == OALSTREAM_Ended ? OALSTREAM_Stopped : oldState;
	if (State == OALSTREAM_Stopped)
	{
		return true;
	}
	for (unsigned int index = 0; index < 4; ++index)
	{
		QueueBuffer (Buffers[index]);
		if (State == OALSTREAM_Failed)
		{
			return false;
		}
	}
	UserPaused = wasUserPaused;
	InactivePaused = wasInactivePaused;
	State = UserPaused || InactivePaused ? OALSTREAM_Paused : (EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing);
	ApplyPauseState ();
	return true;
}

void OpenALSoundStream::SetInactive (bool paused)
{
	InactivePaused = paused;
	ApplyGain ();
	ApplyPauseState ();
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
	AudioMemorySource source;
	AudioProbeResult probe;
	AudioDecodedPCM16 decoded;
	OALPCMData data;
	if (length <= 0 || !AcceptsOpenALEncodedInputSize ((std::size_t)length))
	{
		return handle;
	}
	OALPCMResult result = OALParseWavePCM (sfxdata, (size_t)length);
	if (result.IsValid ())
	{
		return CreateSound (result.Data);
	}
	if (!AssignOpenALEncodedSource (source, sfxdata, (size_t)length))
	{
		return handle;
	}
	probe = ProbeAudioFormat (source);
	if (probe.Status != AUDIO_PROBE_RECOGNIZED ||
		(probe.Format != AUDIO_FORMAT_OGG_VORBIS && probe.Format != AUDIO_FORMAT_MPEG &&
			probe.Format != AUDIO_FORMAT_FLAC && probe.Format != AUDIO_FORMAT_WAVE_FLOAT) ||
		DecodeAudioToPCM16 (source, probe, &decoded) != AUDIO_DECODE_OK || decoded.SampleRate == 0 ||
		(decoded.Channels != 1 && decoded.Channels != 2) || decoded.Samples.empty () ||
		decoded.Samples.size () / decoded.Channels > (size_t)std::numeric_limits<unsigned int>::max ())
	{
		DPrintf ("OpenAL could not load encoded sample: error %d\n", result.Error);
		return handle;
	}
	data.Samples.swap (decoded.Samples);
	data.SampleRate = decoded.SampleRate;
	data.Channels = decoded.Channels;
	data.Frames = (unsigned int)(data.Samples.size () / data.Channels);
	return CreateSound (data);
}

short *OpenALSoundRenderer::DecodeSample (int outlen, const void *coded, int sizebytes, ECodecType type)
{
	AudioMemorySource source;
	AudioProbeResult probe;
	AudioDecodedPCM16 decoded;
	short *outbuf;
	std::size_t outputBytes;
	std::size_t decodedBytes;

	if (outlen <= 0 || coded == NULL || sizebytes <= 0 || type != CODEC_Vorbis ||
		!AssignOpenALEncodedSource (source, coded, (size_t)sizebytes))
	{
		return NULL;
	}
	probe = ProbeAudioFormat (source);
	if (probe.Status != AUDIO_PROBE_RECOGNIZED || probe.Format != AUDIO_FORMAT_OGG_VORBIS ||
		DecodeAudioToPCM16 (source, probe, &decoded) != AUDIO_DECODE_OK || decoded.Channels != 1)
	{
		return NULL;
	}
	outputBytes = (std::size_t)outlen;
	outbuf = (short *)malloc (outputBytes);
	if (outbuf == NULL)
	{
		return NULL;
	}
	memset (outbuf, 0, outputBytes);
	decodedBytes = decoded.Samples.size () * sizeof (short);
	if (decodedBytes != 0)
	{
		memcpy (outbuf, &decoded.Samples[0], decodedBytes < outputBytes ? decodedBytes : outputBytes);
	}
	return outbuf;
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
	unsigned int inputBits;
	unsigned int channels;
	unsigned int bytesPerFrame;
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
	return CreateStreamWithProducer (new (std::nothrow) CallbackStreamProducer (callback, userData), bufferBytes, flags, sampleRate);
}

OpenALSoundStream *OpenALSoundRenderer::CreateStreamWithProducer (OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate)
{
	OpenALSoundStream *stream;
	ALuint source = 0;
	ALuint buffers[4];
	if (producer == NULL)
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
		delete producer;
		return NULL;
	}
	stream = new (std::nothrow) OpenALSoundStream (this, producer, bufferBytes, flags, sampleRate);
	if (stream == NULL)
	{
		alDeleteSources (1, &source);
		alDeleteBuffers (4, buffers);
		delete producer;
		return NULL;
	}
	stream->Source = source;
	memcpy (stream->Buffers, buffers, sizeof (buffers));
	stream->SetInactive (InactiveState == INACTIVE_Complete);
	ActiveStreams.push_back (stream);
	return stream;
}

#ifdef OAL_LIFECYCLE_TEST
OpenALSoundStream *OpenALSoundRenderer::CreatePatternStreamForTest (unsigned int totalFrames, unsigned int loopStart, unsigned int loopEnd, int bufferBytes)
{
	return CreateStreamWithProducer (new (std::nothrow) PatternStreamProducer (totalFrames, loopStart, loopEnd), bufferBytes, OALSTREAM_Mono, 8000);
}
#endif

SoundStream *OpenALSoundRenderer::OpenStream (const char *filename, int flags, int offset, int length)
{
	AudioFileSource file;
	AudioMemorySource memory;
	AudioDataSource *source;
	DecoderStreamProducer *producer;
	unsigned int channels = 0;
	unsigned int sampleRate = 0;
	if (!InitSuccess || filename == NULL || (flags & ~SoundStream::Loop) != 0 || length < 0 || offset < -1 ||
		(offset == 0 && length == 0 && strstr (filename, "://") != NULL))
	{
		Printf (TEXTCOLOR_RED "OpenAL cannot open this encoded music stream.\n");
		return NULL;
	}
	source = OpenEncodedMusicSource (filename, offset, length, &file, &memory);
	if (source == NULL)
	{
		return NULL;
	}
	producer = CreateEncodedMusicProducer (source, &channels, &sampleRate);
	if (producer == NULL)
	{
		return NULL;
	}
	return CreateStreamWithProducer (producer, 4096 * channels * (int)sizeof (short),
		channels == 1 ? OALSTREAM_Mono : 0, (int)sampleRate);
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
	ALenum error;
	bool startFailed = false;
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
		startFailed = true;
	}
#endif
	error = alGetError ();
	if (startFailed || error != AL_NO_ERROR)
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