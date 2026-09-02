#ifndef __AUDIO_DECODER_H__
#define __AUDIO_DECODER_H__

#include <cstddef>
#include <stdio.h>
#include <vector>

enum AudioSourceStatus
{
	AUDIO_SOURCE_OK,
	AUDIO_SOURCE_EOF,
	AUDIO_SOURCE_IO_ERROR,
	AUDIO_SOURCE_INVALID_ARGUMENT,
	AUDIO_SOURCE_RANGE_ERROR
};

enum AudioSeekOrigin
{
	AUDIO_SEEK_BEGIN,
	AUDIO_SEEK_CURRENT,
	AUDIO_SEEK_END
};

class AudioDataSource
{
public:
	virtual ~AudioDataSource ();

	virtual AudioSourceStatus Read (void *data, std::size_t bytes, std::size_t *bytesRead) = 0;
	virtual AudioSourceStatus Seek (AudioSeekOrigin origin, long long offset) = 0;
	virtual std::size_t Tell () const = 0;
	virtual std::size_t GetLength () const = 0;
};

class AudioMemorySource : public AudioDataSource
{
public:
	AudioMemorySource ();
	AudioMemorySource (const void *data, std::size_t bytes);

	AudioSourceStatus Assign (const void *data, std::size_t bytes);
	AudioSourceStatus Read (void *data, std::size_t bytes, std::size_t *bytesRead);
	AudioSourceStatus Seek (AudioSeekOrigin origin, long long offset);
	std::size_t Tell () const;
	std::size_t GetLength () const;

private:
	std::vector<unsigned char> Data;
	std::size_t Position;
};

class AudioFileSource : public AudioDataSource
{
public:
	AudioFileSource ();
	~AudioFileSource ();

	AudioSourceStatus Open (const char *filename);
	AudioSourceStatus OpenSlice (const char *filename, std::size_t offset, std::size_t length);
	void Close ();
	bool IsOpen () const;

	AudioSourceStatus Read (void *data, std::size_t bytes, std::size_t *bytesRead);
	AudioSourceStatus Seek (AudioSeekOrigin origin, long long offset);
	std::size_t Tell () const;
	std::size_t GetLength () const;

private:
	AudioFileSource (const AudioFileSource &);
	AudioFileSource &operator= (const AudioFileSource &);

	FILE *File;
	std::size_t FileOffset;
	std::size_t Length;
	std::size_t Position;
};

enum AudioFormat
{
	AUDIO_FORMAT_UNKNOWN,
	AUDIO_FORMAT_WAVE_PCM,
	AUDIO_FORMAT_WAVE_FLOAT,
	AUDIO_FORMAT_FLAC,
	AUDIO_FORMAT_MPEG,
	AUDIO_FORMAT_OGG_VORBIS,
	AUDIO_FORMAT_OGG_OPUS
};

enum AudioProbeStatus
{
	AUDIO_PROBE_RECOGNIZED,
	AUDIO_PROBE_UNKNOWN,
	AUDIO_PROBE_MALFORMED,
	AUDIO_PROBE_IO_ERROR
};

struct AudioPCMMetadata
{
	AudioPCMMetadata ();

	unsigned int SampleRate;
	unsigned int Channels;
	unsigned int BitsPerSample;
	bool FloatingPoint;
};

struct AudioProbeResult
{
	AudioProbeResult ();

	AudioProbeStatus Status;
	AudioFormat Format;
	AudioPCMMetadata PCM;
};

AudioProbeResult ProbeAudioFormat (AudioDataSource &source);

enum AudioDecodeStatus
{
	AUDIO_DECODE_OK,
	AUDIO_DECODE_UNSUPPORTED,
	AUDIO_DECODE_INVALID_SOURCE,
	AUDIO_DECODE_IO_ERROR
};

enum AudioDecoderReadStatus
{
	AUDIO_DECODER_DATA,
	AUDIO_DECODER_EOF,
	AUDIO_DECODER_ERROR
};

enum AudioDecoderSeekStatus
{
	AUDIO_DECODER_SEEK_OK,
	AUDIO_DECODER_SEEK_ERROR,
	AUDIO_DECODER_SEEK_TERMINAL_ERROR
};

struct AudioFrameRange
{
	AudioFrameRange ();

	unsigned long long Start;
	unsigned long long End;
};

struct AudioDecodedPCM16
{
	AudioDecodedPCM16 ();

	unsigned int SampleRate;
	unsigned int Channels;
	std::vector<short> Samples;
};

class AudioDecoder
{
public:
	virtual ~AudioDecoder ();
	// Identifies the encoded source format selected by this decoder.
	virtual AudioFormat GetEncodedFormat () const = 0;
	virtual const AudioPCMMetadata &GetMetadata () const = 0;
	// The native decode rate and interleaved output channel count.
	virtual unsigned int GetNativeSampleRate () const = 0;
	virtual unsigned int GetOutputChannels () const = 0;
	// Total and current frame positions are precise decoded PCM frame counts.
	virtual bool GetTotalFrames (unsigned long long *frames) const = 0;
	virtual unsigned long long TellFrame () const = 0;
	// When present, the loop range is an optional forward half-open range: [Start, End).
	virtual bool GetLoopRange (AudioFrameRange *range) const = 0;
	// Adapter-owned diagnostics remain valid until the adapter changes them or is destroyed.
	virtual int GetDiagnosticCode () const = 0;
	virtual const char *GetDiagnosticString () const = 0;

	// Writes native-rate signed interleaved PCM16 samples for one or two channels.
	// DATA always reports the actual frame count, including a partial final read.
	// EOF reports no frames; ERROR provides adapter-owned diagnostic information.
	virtual AudioDecoderReadStatus ReadFrames (short *frames, std::size_t frameCount, std::size_t *framesRead) = 0;
	// A recoverable error atomically preserves TellFrame(). A failed restore is terminal.
	virtual AudioDecoderSeekStatus SeekFrame (unsigned long long frame) = 0;
};

class AudioDecoderFactory
{
public:
	virtual ~AudioDecoderFactory ();
	virtual AudioDecodeStatus Create (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder) = 0;
};

AudioDecodeStatus CreateAudioDecoder (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder);
AudioDecodeStatus DecodeAudioToPCM16 (AudioDataSource &source, const AudioProbeResult &probe, AudioDecodedPCM16 *decoded);

// Decoder adapters use this to take an independent, bounded copy of a logical source.
AudioDecodeStatus CopyAudioDecoderSource (AudioDataSource &source, std::vector<unsigned char> *data);

#ifdef AUDIO_DECODER_TESTING
bool AudioDecoderTestSupportsVorbisChannels (unsigned int channels);
bool AudioDecoderTestSupportsVorbisTotalFrames (unsigned int frames);
bool AudioDecoderTestVorbisCanReadFrames (unsigned int totalFrames, unsigned long long position, unsigned int frames);
bool AudioDecoderTestVorbisCanSeekFrame (unsigned int totalFrames, unsigned long long frame);
bool AudioDecoderTestVorbisIsLogicalEOF (unsigned int totalFrames, unsigned long long frame);
bool AudioDecoderTestVorbisCanUseLoopEndpoint (unsigned int totalFrames, unsigned long long endpoint);
bool AudioDecoderTestParseVorbisLoopComments (const char *const *comments, int commentCount, bool totalFramesKnown, unsigned long long totalFrames, unsigned int sampleRate, AudioFrameRange *range, int *diagnosticCode);
const char *AudioDecoderTestVorbisLoopDiagnosticString ();
#endif

#endif