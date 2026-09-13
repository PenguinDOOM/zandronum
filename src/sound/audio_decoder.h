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
void AudioDecoderTestMiniaudioPCMToS16 (short *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
void AudioDecoderTestMiniaudioPCMToF32 (float *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
void AudioDecoderTestMiniaudioPCMToS32 (int *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
void AudioDecoderTestMiniaudioIEEEToS16 (short *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
void AudioDecoderTestMiniaudioIEEEToF32 (float *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
void AudioDecoderTestMiniaudioIEEEToS32 (int *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample);
bool AudioDecoderTestMiniaudioAutoDetectMemory (const unsigned char *data, std::size_t bytes);
bool AudioDecoderTestMiniaudioAutoDetectFile (const char *path);
bool AudioDecoderTestMiniaudioAutoDetectWideFile (const wchar_t *path);
std::size_t AudioDecoderTestMiniaudioReadWavS16 (const unsigned char *data, std::size_t bytes, short *output, std::size_t frames);
bool AudioDecoderTestMiniaudioFlacAllocationLayout (unsigned int maxBlockSize, unsigned int channels, unsigned int seekpointCount, bool isOgg, std::size_t *allocationSize, std::size_t *decodedSamplesOffset, std::size_t *decodedSampleCount, std::size_t *seekpointsOffset);
bool AudioDecoderTestMiniaudioFlacAllocationOwnership (const unsigned char *data, std::size_t bytes, std::size_t *allocationCount, std::size_t *freeCount);
bool AudioDecoderTestMiniaudioFlacDecodeSeekpoint (const unsigned char *data, std::size_t bytes, unsigned long long *firstPCMFrame, unsigned long long *flacFrameOffset, unsigned int *pcmFrameCount);
bool AudioDecoderTestMiniaudioFlacOpenSeektable (const unsigned char *data, std::size_t bytes, bool withMetadata, unsigned int *seekpointCount, unsigned long long *firstPCMFrame, unsigned long long *flacFrameOffset, unsigned int *pcmFrameCount, unsigned int *metadataRawDataSize);
struct AudioDecoderTestMiniaudioFlacCallbackReport
{
	bool Opened;
	bool CanaryIntact;
	bool ReallocExistingPointer;
	unsigned int SeekpointCount;
	std::size_t MallocCount;
	std::size_t ReallocCount;
	std::size_t FreeCount;
	std::size_t ReadCount;
	std::size_t SeekCount;
	std::size_t MetadataSeektableCount;
	bool MetadataRawDataMatchesSeekpoints;
	std::size_t MetadataRawDataSize;
	unsigned int MetadataSeekpointCount;
	unsigned long long MetadataFirstPCMFrame;
	unsigned long long MetadataFlacFrameOffset;
	unsigned int MetadataPCMFrameCount;
	std::size_t ReadPositions[16];
	std::size_t SeekPositions[16];
	void *AllocationPointers[4];
	void *FreePointers[4];
};
bool AudioDecoderTestMiniaudioFlacCallbackOpen (const unsigned char *data, std::size_t bytes, bool withMetadata, bool reallocOnly, std::size_t failAllocationOrdinal, std::size_t failReadPosition, std::size_t failSeekOrdinal, std::size_t failSeekPosition, AudioDecoderTestMiniaudioFlacCallbackReport *report, bool corruptCanaryBeforeClose = false);
bool AudioDecoderTestMiniaudioOggFlacDecode (const unsigned char *data, std::size_t bytes, unsigned long long seekFrame, std::size_t failAllocationOrdinal, unsigned long long *totalPCMFrames, unsigned long long *pcmHash, short *firstSample, short *seekSample, AudioDecoderTestMiniaudioFlacCallbackReport *report);
extern "C" int stb_vorbis_test_memory_seek_sequence (const unsigned char *data, int length, const unsigned int *locations, int locationCount, int *success, int *eof, unsigned int *offsets);
extern "C" int stb_vorbis_test_temp_memory_required (int channels, int blocksize, int residueType, unsigned int begin, unsigned int end, unsigned int partSize, int classwords, unsigned int *required);
extern "C" int stb_vorbis_test_open_memory_scratch (const unsigned char *data, int length, char *arena, int arenaLength, int failureMode, unsigned int *arenaRequired, int *error, int *scratchFreeCount, int *allocationsStable, int *arenaOffsetStable);
extern "C" int stb_vorbis_test_residue_scratch_canary (int channels, int blocksize, int residueType, unsigned int begin, unsigned int end, unsigned int partSize, int classwords, char *arena, int arenaLength, unsigned int *required);
extern "C" int stb_vorbis_test_decode_residue_layout (int residueType, int channels, int blocksize, int n, unsigned int end, char *arena, int arenaLength, int *logicalPartitions, int *rowCapacity, int *channel0Samples, int *channel1Samples, int *bitsConsumed);
extern "C" int stb_vorbis_test_arena_deinit_offset (int tempOffset, int *offsetStable);
extern "C" int stb_vorbis_test_outofmem_error ();
#endif

#endif