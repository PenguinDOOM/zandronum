#include "audio_decoder.h"

#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_VORBIS
#define MINIAUDIO_IMPLEMENTATION
#include "thirdparty/miniaudio/miniaudio.h"

#include <limits>
#include <new>
#include <stdint.h>
#include <stdlib.h>

namespace
{
	ma_encoding_format GetMiniaudioEncodingFormat (AudioFormat format)
	{
		if (format == AUDIO_FORMAT_WAVE_PCM || format == AUDIO_FORMAT_WAVE_FLOAT)
		{
			return ma_encoding_format_wav;
		}
		if (format == AUDIO_FORMAT_FLAC)
		{
			return ma_encoding_format_flac;
		}
		if (format == AUDIO_FORMAT_MPEG)
		{
			return ma_encoding_format_mp3;
		}
		return ma_encoding_format_unknown;
	}

	class MiniaudioDecoder : public AudioDecoder
	{
	public:
		MiniaudioDecoder (const std::vector<unsigned char> &data, AudioFormat format, const AudioPCMMetadata &metadata)
			: Data (data), Format (format), Metadata (metadata), Initialized (false), TotalFramesKnown (false), TotalFrames (0), Position (0), AtLogicalEOF (false), DiagnosticCode (MA_SUCCESS)
		{
			ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);
			ma_format outputFormat;
			ma_uint32 channels;
			ma_uint32 sampleRate;
			config.encodingFormat = GetMiniaudioEncodingFormat (Format);
			ma_result result = ma_decoder_init_memory (&Data[0], Data.size (), &config, &Decoder);
			if (result != MA_SUCCESS)
			{
				DiagnosticCode = result;
				return;
			}
			Initialized = true;
			result = ma_decoder_get_data_format (&Decoder, &outputFormat, &channels, &sampleRate, NULL, 0);
			if (result != MA_SUCCESS || outputFormat != ma_format_s16 || channels == 0 || channels > 2 || sampleRate == 0)
			{
				DiagnosticCode = result != MA_SUCCESS ? result : MA_INVALID_FILE;
				ma_decoder_uninit (&Decoder);
				Initialized = false;
				return;
			}
			Metadata.Channels = channels;
			Metadata.SampleRate = sampleRate;
			Metadata.BitsPerSample = 16;
			Metadata.FloatingPoint = false;
			if (ma_decoder_get_length_in_pcm_frames (&Decoder, &TotalFrames) == MA_SUCCESS)
			{
				TotalFramesKnown = true;
			}
		}

		~MiniaudioDecoder ()
		{
			if (Initialized)
			{
				ma_decoder_uninit (&Decoder);
			}
		}

		bool IsValid () const { return Initialized; }
		AudioFormat GetEncodedFormat () const { return Format; }
		const AudioPCMMetadata &GetMetadata () const { return Metadata; }
		unsigned int GetNativeSampleRate () const { return Metadata.SampleRate; }
		unsigned int GetOutputChannels () const { return Metadata.Channels; }
		bool GetTotalFrames (unsigned long long *frames) const
		{
			if (frames == NULL || !TotalFramesKnown)
			{
				return false;
			}
			*frames = TotalFrames;
			return true;
		}
		unsigned long long TellFrame () const { return Position; }
		bool GetLoopRange (AudioFrameRange *) const { return false; }
		int GetDiagnosticCode () const { return DiagnosticCode; }
		const char *GetDiagnosticString () const { return ma_result_description ((ma_result)DiagnosticCode); }

		AudioDecoderReadStatus ReadFrames (short *frames, std::size_t frameCount, std::size_t *framesRead)
		{
			ma_uint64 read = 0;
			ma_result result;
			if (framesRead == NULL || (frames == NULL && frameCount != 0) || frameCount > (std::size_t)(std::numeric_limits<ma_uint64>::max) ())
			{
				return AUDIO_DECODER_ERROR;
			}
			*framesRead = 0;
			if (frameCount == 0)
			{
				return AUDIO_DECODER_DATA;
			}
			if (AtLogicalEOF)
			{
				return AUDIO_DECODER_EOF;
			}
			result = ma_decoder_read_pcm_frames (&Decoder, frames, (ma_uint64)frameCount, &read);
			if ((result != MA_SUCCESS && result != MA_AT_END) || read > frameCount || (TotalFramesKnown && (Position > TotalFrames || read > TotalFrames - Position)))
			{
				DiagnosticCode = result;
				return AUDIO_DECODER_ERROR;
			}
			Position += read;
			*framesRead = (std::size_t)read;
			AtLogicalEOF = read == 0;
			return read == 0 ? AUDIO_DECODER_EOF : AUDIO_DECODER_DATA;
		}

		AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
		{
			ma_uint64 original;
			bool originalAtLogicalEOF = AtLogicalEOF;
			ma_result result;
			if (frame > (unsigned long long)(std::numeric_limits<ma_uint64>::max) ())
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			if (TotalFramesKnown && frame > TotalFrames)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			if (TotalFramesKnown && frame == TotalFrames)
			{
				Position = frame;
				AtLogicalEOF = true;
				return AUDIO_DECODER_SEEK_OK;
			}
			original = Position;
			result = ma_decoder_seek_to_pcm_frame (&Decoder, (ma_uint64)frame);
			if (result == MA_SUCCESS)
			{
				Position = frame;
				AtLogicalEOF = false;
				return AUDIO_DECODER_SEEK_OK;
			}
			DiagnosticCode = result;
			if (originalAtLogicalEOF)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			if (ma_decoder_seek_to_pcm_frame (&Decoder, original) == MA_SUCCESS)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			return AUDIO_DECODER_SEEK_TERMINAL_ERROR;
		}

	private:
		ma_decoder Decoder;
		std::vector<unsigned char> Data;
		AudioFormat Format;
		AudioPCMMetadata Metadata;
		bool Initialized;
		bool TotalFramesKnown;
		ma_uint64 TotalFrames;
		unsigned long long Position;
		bool AtLogicalEOF;
		int DiagnosticCode;
	};
}

AudioDecodeStatus CreateMiniaudioDecoder (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder)
{
	std::vector<unsigned char> data;
	AudioDecodeStatus status;
	MiniaudioDecoder *created;
	if (decoder == NULL)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	*decoder = NULL;
	status = CopyAudioDecoderSource (source, &data);
	if (status != AUDIO_DECODE_OK || data.empty ())
	{
		return status == AUDIO_DECODE_OK ? AUDIO_DECODE_INVALID_SOURCE : status;
	}
	created = new (std::nothrow) MiniaudioDecoder (data, probe.Format, probe.PCM);
	if (created == NULL)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	if (!created->IsValid ())
	{
		delete created;
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	*decoder = created;
	return AUDIO_DECODE_OK;
}

#ifdef AUDIO_DECODER_TESTING
void AudioDecoderTestMiniaudioPCMToS16 (short *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__pcm_to_s16 (output, input, sampleCount, bytesPerSample);
}

void AudioDecoderTestMiniaudioPCMToF32 (float *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__pcm_to_f32 (output, input, sampleCount, bytesPerSample);
}

void AudioDecoderTestMiniaudioPCMToS32 (int *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__pcm_to_s32 (output, input, sampleCount, bytesPerSample);
}

void AudioDecoderTestMiniaudioIEEEToS16 (short *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__ieee_to_s16 (output, input, sampleCount, bytesPerSample);
}

void AudioDecoderTestMiniaudioIEEEToF32 (float *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__ieee_to_f32 (output, input, sampleCount, bytesPerSample);
}

void AudioDecoderTestMiniaudioIEEEToS32 (int *output, const unsigned char *input, std::size_t sampleCount, unsigned int bytesPerSample)
{
	ma_dr_wav__ieee_to_s32 (output, input, sampleCount, bytesPerSample);
}

bool AudioDecoderTestMiniaudioAutoDetectMemory (const unsigned char *data, std::size_t bytes)
{
	ma_decoder decoder;
	ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);
	ma_result result = ma_decoder_init_memory (data, bytes, &config, &decoder);
	if (result != MA_SUCCESS)
	{
		return false;
	}
	ma_decoder_uninit (&decoder);
	return true;
}

bool AudioDecoderTestMiniaudioAutoDetectFile (const char *path)
{
	ma_decoder decoder;
	ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);
	ma_result result = ma_decoder_init_file (path, &config, &decoder);
	if (result != MA_SUCCESS)
	{
		return false;
	}
	ma_decoder_uninit (&decoder);
	return true;
}

bool AudioDecoderTestMiniaudioAutoDetectWideFile (const wchar_t *path)
{
	ma_decoder decoder;
	ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);
	ma_result result = ma_decoder_init_file_w (path, &config, &decoder);
	if (result != MA_SUCCESS)
	{
		return false;
	}
	ma_decoder_uninit (&decoder);
	return true;
}

std::size_t AudioDecoderTestMiniaudioReadWavS16 (const unsigned char *data, std::size_t bytes, short *output, std::size_t frames)
{
	ma_decoder decoder;
	ma_uint64 framesRead = 0;
	ma_decoder_config config = ma_decoder_config_init (ma_format_s16, 0, 0);
	if (ma_decoder_init_memory (data, bytes, &config, &decoder) != MA_SUCCESS)
	{
		return 0;
	}
	if (ma_decoder_read_pcm_frames (&decoder, output, frames, &framesRead) != MA_SUCCESS && framesRead == 0)
	{
		ma_decoder_uninit (&decoder);
		return 0;
	}
	ma_decoder_uninit (&decoder);
	return (std::size_t)framesRead;
}

bool AudioDecoderTestMiniaudioFlacAllocationLayout (unsigned int maxBlockSize, unsigned int channels, unsigned int seekpointCount, bool isOgg, std::size_t *allocationSize, std::size_t *decodedSamplesOffset, std::size_t *decodedSampleCount, std::size_t *seekpointsOffset)
{
	ma_dr_flac__allocation_layout layout;
	if (!ma_dr_flac__calculate_allocation_layout (NULL, maxBlockSize, channels, seekpointCount, isOgg ? MA_TRUE : MA_FALSE, &layout))
	{
		return false;
	}
	*allocationSize = layout.allocationSize;
	*decodedSamplesOffset = layout.decodedSamplesOffset;
	*decodedSampleCount = layout.decodedSampleCount;
	*seekpointsOffset = layout.seekpointsOffset;
	return true;
}

namespace
{
	struct FlacSeekpointReader
	{
		const unsigned char *Data;
		std::size_t Bytes;
		std::size_t Offset;
	};

	struct FlacMetadataProbe
	{
		std::size_t SeektableCount;
		bool RawDataMatchesSeekpoints;
		unsigned int SeekpointCount;
		std::size_t RawDataSize;
		unsigned long long FirstPCMFrame;
		unsigned long long FlacFrameOffset;
		unsigned int PCMFrameCount;
	};

	void FlacMetadataProbeCallback (void *userData, ma_dr_flac_metadata *metadata)
	{
		FlacMetadataProbe *probe = static_cast<FlacMetadataProbe *> (userData);
		if (metadata->type == MA_DR_FLAC_METADATA_BLOCK_TYPE_SEEKTABLE)
		{
			++probe->SeektableCount;
			probe->RawDataMatchesSeekpoints = metadata->pRawData == metadata->data.seektable.pSeekpoints;
			probe->SeekpointCount = metadata->data.seektable.seekpointCount;
			probe->RawDataSize = metadata->rawDataSize;
			if (probe->SeekpointCount > 0)
			{
				probe->FirstPCMFrame = metadata->data.seektable.pSeekpoints[0].firstPCMFrame;
				probe->FlacFrameOffset = metadata->data.seektable.pSeekpoints[0].flacFrameOffset;
				probe->PCMFrameCount = metadata->data.seektable.pSeekpoints[0].pcmFrameCount;
			}
		}
	}

	size_t FlacSeekpointReaderRead (void *userData, void *output, size_t bytes)
	{
		FlacSeekpointReader *reader = static_cast<FlacSeekpointReader *> (userData);
		std::size_t available = reader->Bytes - reader->Offset;
		std::size_t count = available < bytes ? available : bytes;
		memcpy (output, reader->Data + reader->Offset, count);
		reader->Offset += count;
		return count;
	}

	struct FlacAllocationProbe
	{
		std::size_t AllocationCount;
		std::size_t ReallocCount;
		std::size_t FreeCount;
		void *HostPointers[4];
		void *RawPointers[4];
		std::size_t AllocationSizes[4];
		void *FreePointers[4];
		bool CanaryIntact;
		bool ReallocExistingPointer;
		std::size_t FailAllocationOrdinal;
	};

	void *FlacAllocationProbeAllocate (std::size_t size, void *userData, bool reallocate, void *pointer)
	{
		FlacAllocationProbe *probe = static_cast<FlacAllocationProbe *> (userData);
		unsigned char *host;
		uintptr_t address;
		unsigned char *raw;
		std::size_t allocationOrdinal = probe->AllocationCount + probe->ReallocCount + 1;
		if (reallocate)
		{
			++probe->ReallocCount;
			probe->ReallocExistingPointer = probe->ReallocExistingPointer || pointer != NULL;
		}
		else
		{
			++probe->AllocationCount;
		}
		if (probe->FailAllocationOrdinal == allocationOrdinal)
		{
			return NULL;
		}
		if (allocationOrdinal > 4)
		{
			return NULL;
		}
		host = static_cast<unsigned char *> (malloc (size + 79));
		if (host == NULL)
		{
			return NULL;
		}
		address = ((uintptr_t)host + 15) & ~(uintptr_t)15;
		if (address % 64 == 0)
		{
			address += 16;
		}
		raw = reinterpret_cast<unsigned char *> (address);
		if (allocationOrdinal <= 4)
		{
			probe->HostPointers[allocationOrdinal - 1] = host;
			probe->RawPointers[allocationOrdinal - 1] = raw;
			probe->AllocationSizes[allocationOrdinal - 1] = size;
		}
		memset (raw + size, 0xa5, 8);
		return raw;
	}

	void *FlacAllocationProbeMalloc (std::size_t size, void *userData)
	{
		return FlacAllocationProbeAllocate (size, userData, false, NULL);
	}

	void *FlacAllocationProbeRealloc (void *pointer, std::size_t size, void *userData)
	{
		return FlacAllocationProbeAllocate (size, userData, true, pointer);
	}

	void FlacAllocationProbeFree (void *pointer, void *userData)
	{
		FlacAllocationProbe *probe = static_cast<FlacAllocationProbe *> (userData);
		std::size_t allocationIndex;
		++probe->FreeCount;
		if (probe->FreeCount <= 4)
		{
			probe->FreePointers[probe->FreeCount - 1] = pointer;
		}
		for (allocationIndex = 0; allocationIndex < 4; ++allocationIndex)
		{
			if (pointer == probe->RawPointers[allocationIndex] && probe->HostPointers[allocationIndex] != NULL)
			{
				probe->CanaryIntact = probe->CanaryIntact && memcmp (static_cast<unsigned char *> (pointer) + probe->AllocationSizes[allocationIndex], "\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5", 8) == 0;
				free (probe->HostPointers[allocationIndex]);
				probe->HostPointers[allocationIndex] = NULL;
				return;
			}
		}
		probe->CanaryIntact = false;
	}

	bool FlacAllocationProbeHasLiveAllocations (const FlacAllocationProbe *probe)
	{
		for (std::size_t allocationIndex = 0; allocationIndex < 4; ++allocationIndex)
		{
			if (probe->HostPointers[allocationIndex] != NULL)
			{
				return true;
			}
		}
		return false;
	}

	void FlacAllocationProbeReleaseLiveAllocations (FlacAllocationProbe *probe)
	{
		for (std::size_t allocationIndex = 0; allocationIndex < 4; ++allocationIndex)
		{
			if (probe->HostPointers[allocationIndex] != NULL)
			{
				free (probe->HostPointers[allocationIndex]);
				probe->HostPointers[allocationIndex] = NULL;
			}
		}
	}

	bool FlacAllocationProbeVerifyStaleRawPointerReuse ()
	{
		FlacAllocationProbe probe = { 0, 0, 0, { NULL }, { NULL }, { 0 }, { NULL }, true, false, 0 };
		unsigned char *host = static_cast<unsigned char *> (malloc (95));
		uintptr_t address;
		unsigned char *raw;
		bool released;
		if (host == NULL)
		{
			return false;
		}
		address = ((uintptr_t)host + 15) & ~(uintptr_t)15;
		if (address % 64 == 0)
		{
			address += 16;
		}
		raw = reinterpret_cast<unsigned char *> (address);
		memset (raw + 16, 0xa5, 8);
		probe.RawPointers[0] = raw;
		probe.AllocationSizes[0] = 16;
		probe.HostPointers[1] = host;
		probe.RawPointers[1] = raw;
		probe.AllocationSizes[1] = 16;
		FlacAllocationProbeFree (raw, &probe);
		released = !FlacAllocationProbeHasLiveAllocations (&probe) && probe.CanaryIntact;
		FlacAllocationProbeReleaseLiveAllocations (&probe);
		return released;
	}

	void FlacAllocationProbeCorruptCanary (FlacAllocationProbe *probe)
	{
		for (std::size_t allocationIndex = 0; allocationIndex < 4; ++allocationIndex)
		{
			if (probe->HostPointers[allocationIndex] != NULL && probe->RawPointers[allocationIndex] != NULL)
			{
				static_cast<unsigned char *> (probe->RawPointers[allocationIndex])[probe->AllocationSizes[allocationIndex]] ^= 0xff;
				return;
			}
		}
	}

	struct FlacCallbackProbe
	{
		FlacAllocationProbe Allocation;
		FlacMetadataProbe Metadata;
		const unsigned char *Data;
		std::size_t Bytes;
		std::size_t Position;
		std::size_t ReadCount;
		std::size_t SeekCount;
		std::size_t FailReadPosition;
		std::size_t FailSeekOrdinal;
		std::size_t FailSeekPosition;
		std::size_t ReadPositions[16];
		std::size_t SeekPositions[16];
	};

	size_t FlacCallbackProbeRead (void *userData, void *output, size_t bytes)
	{
		FlacCallbackProbe *probe = static_cast<FlacCallbackProbe *> (userData);
		std::size_t available = probe->Bytes - probe->Position;
		std::size_t count = available < bytes ? available : bytes;
		if (probe->ReadCount < 16)
		{
			probe->ReadPositions[probe->ReadCount] = probe->Position;
		}
		++probe->ReadCount;
		if (probe->FailReadPosition == probe->Position && count > 0)
		{
			--count;
		}
		memcpy (output, probe->Data + probe->Position, count);
		probe->Position += count;
		return count;
	}

	ma_bool32 FlacCallbackProbeSeek (void *userData, int offset, ma_dr_flac_seek_origin origin)
	{
		FlacCallbackProbe *probe = static_cast<FlacCallbackProbe *> (userData);
		long long position = origin == MA_DR_FLAC_SEEK_SET ? offset : (long long)probe->Position + offset;
		if (probe->SeekCount < 16 && position >= 0)
		{
			probe->SeekPositions[probe->SeekCount] = (std::size_t)position;
		}
		++probe->SeekCount;
		if (probe->FailSeekOrdinal == probe->SeekCount || (origin == MA_DR_FLAC_SEEK_SET && position >= 0 && probe->FailSeekPosition == (std::size_t)position) || position < 0 || (unsigned long long)position > probe->Bytes)
		{
			return MA_FALSE;
		}
		probe->Position = (std::size_t)position;
		return MA_TRUE;
	}

	ma_bool32 FlacCallbackProbeTell (void *userData, ma_int64 *position)
	{
		*position = (ma_int64)static_cast<FlacCallbackProbe *> (userData)->Position;
		return MA_TRUE;
	}

	void FlacCallbackProbeMetadata (void *userData, ma_dr_flac_metadata *metadata)
	{
		FlacCallbackProbe *probe = static_cast<FlacCallbackProbe *> (userData);
		if (metadata->type == MA_DR_FLAC_METADATA_BLOCK_TYPE_SEEKTABLE)
		{
			++probe->Metadata.SeektableCount;
			probe->Metadata.RawDataMatchesSeekpoints = metadata->pRawData == metadata->data.seektable.pSeekpoints;
			probe->Metadata.RawDataSize = metadata->rawDataSize;
			probe->Metadata.SeekpointCount = metadata->data.seektable.seekpointCount;
			if (metadata->data.seektable.seekpointCount > 0)
			{
				probe->Metadata.FirstPCMFrame = metadata->data.seektable.pSeekpoints[0].firstPCMFrame;
				probe->Metadata.FlacFrameOffset = metadata->data.seektable.pSeekpoints[0].flacFrameOffset;
				probe->Metadata.PCMFrameCount = metadata->data.seektable.pSeekpoints[0].pcmFrameCount;
			}
		}
	}
}

bool AudioDecoderTestMiniaudioFlacAllocationOwnership (const unsigned char *data, std::size_t bytes, std::size_t *allocationCount, std::size_t *freeCount)
{
	FlacAllocationProbe probe = { 0, 0, 0, { NULL }, { NULL }, { 0 }, { NULL }, true, false, 0 };
	ma_allocation_callbacks callbacks;
	ma_dr_flac *flac;
	bool aligned;
	callbacks.pUserData = &probe;
	callbacks.onMalloc = FlacAllocationProbeMalloc;
	callbacks.onRealloc = NULL;
	callbacks.onFree = FlacAllocationProbeFree;
	flac = ma_dr_flac_open_memory (data, bytes, &callbacks);
	if (flac == NULL)
	{
		*allocationCount = probe.AllocationCount;
		*freeCount = probe.FreeCount;
		return false;
	}
	aligned = ((std::size_t)flac->pDecodedSamples % 64) == 0 && reinterpret_cast<void *> (flac) == probe.RawPointers[0];
	ma_dr_flac_close (flac);
	*allocationCount = probe.AllocationCount;
	*freeCount = probe.FreeCount;
	return aligned && probe.CanaryIntact && probe.AllocationCount == 1 && probe.FreeCount == 1 && probe.FreePointers[0] == probe.RawPointers[0];
}

bool AudioDecoderTestMiniaudioFlacCallbackOpen (const unsigned char *data, std::size_t bytes, bool withMetadata, bool reallocOnly, std::size_t failAllocationOrdinal, std::size_t failReadPosition, std::size_t failSeekOrdinal, std::size_t failSeekPosition, AudioDecoderTestMiniaudioFlacCallbackReport *report, bool corruptCanaryBeforeClose)
{
	FlacCallbackProbe probe = { { 0, 0, 0, { NULL }, { NULL }, { 0 }, { NULL }, true, false, failAllocationOrdinal }, { 0, false, 0, 0, 0, 0, 0 }, data, bytes, 0, 0, 0, failReadPosition, failSeekOrdinal, failSeekPosition, { 0 }, { 0 } };
	ma_allocation_callbacks callbacks;
	ma_dr_flac *flac;
	if (data == NULL || report == NULL)
	{
		return false;
	}
	callbacks.pUserData = &probe;
	callbacks.onMalloc = reallocOnly ? NULL : FlacAllocationProbeMalloc;
	callbacks.onRealloc = reallocOnly ? FlacAllocationProbeRealloc : NULL;
	callbacks.onFree = FlacAllocationProbeFree;
	flac = withMetadata ? ma_dr_flac_open_with_metadata (FlacCallbackProbeRead, FlacCallbackProbeSeek, FlacCallbackProbeTell, FlacCallbackProbeMetadata, &probe, &callbacks) : ma_dr_flac_open (FlacCallbackProbeRead, FlacCallbackProbeSeek, FlacCallbackProbeTell, &probe, &callbacks);
	memset (report, 0, sizeof (*report));
	report->Opened = flac != NULL;
	report->ReallocExistingPointer = probe.Allocation.ReallocExistingPointer;
	report->SeekpointCount = flac == NULL ? 0 : flac->seekpointCount;
	report->MetadataSeektableCount = probe.Metadata.SeektableCount;
	report->MetadataRawDataMatchesSeekpoints = probe.Metadata.RawDataMatchesSeekpoints;
	report->MetadataRawDataSize = probe.Metadata.RawDataSize;
	report->MetadataSeekpointCount = probe.Metadata.SeekpointCount;
	report->MetadataFirstPCMFrame = probe.Metadata.FirstPCMFrame;
	report->MetadataFlacFrameOffset = probe.Metadata.FlacFrameOffset;
	report->MetadataPCMFrameCount = probe.Metadata.PCMFrameCount;
	if (flac != NULL)
	{
		if (corruptCanaryBeforeClose)
		{
			FlacAllocationProbeCorruptCanary (&probe.Allocation);
		}
		ma_dr_flac_close (flac);
	}
	report->CanaryIntact = probe.Allocation.CanaryIntact;
	report->MallocCount = probe.Allocation.AllocationCount;
	report->ReallocCount = probe.Allocation.ReallocCount;
	report->FreeCount = probe.Allocation.FreeCount;
	report->ReadCount = probe.ReadCount;
	report->SeekCount = probe.SeekCount;
	memcpy (report->ReadPositions, probe.ReadPositions, sizeof (report->ReadPositions));
	memcpy (report->SeekPositions, probe.SeekPositions, sizeof (report->SeekPositions));
	memcpy (report->AllocationPointers, probe.Allocation.RawPointers, sizeof (report->AllocationPointers));
	memcpy (report->FreePointers, probe.Allocation.FreePointers, sizeof (report->FreePointers));
	bool allocationsReleased = !FlacAllocationProbeHasLiveAllocations (&probe.Allocation);
	FlacAllocationProbeReleaseLiveAllocations (&probe.Allocation);
	return allocationsReleased && FlacAllocationProbeVerifyStaleRawPointerReuse ();
}

bool AudioDecoderTestMiniaudioOggFlacDecode (const unsigned char *data, std::size_t bytes, unsigned long long seekFrame, std::size_t failAllocationOrdinal, unsigned long long *totalPCMFrames, unsigned long long *pcmHash, short *firstSample, short *seekSample, AudioDecoderTestMiniaudioFlacCallbackReport *report)
{
	FlacAllocationProbe probe = { 0, 0, 0, { NULL }, { NULL }, { 0 }, { NULL }, true, false, failAllocationOrdinal };
	ma_allocation_callbacks callbacks;
	ma_dr_flac *flac;
	ma_int16 samples[64];
	ma_uint64 framesRead;
	ma_uint64 totalRead = 0;
	unsigned long long hash = 1469598103934665603ULL;
	if (data == NULL || totalPCMFrames == NULL || pcmHash == NULL || firstSample == NULL || seekSample == NULL || report == NULL)
	{
		return false;
	}
	callbacks.pUserData = &probe;
	callbacks.onMalloc = FlacAllocationProbeMalloc;
	callbacks.onRealloc = NULL;
	callbacks.onFree = FlacAllocationProbeFree;
	flac = ma_dr_flac_open_memory (data, bytes, &callbacks);
	memset (report, 0, sizeof (*report));
	report->Opened = flac != NULL;
	if (flac != NULL)
	{
		if (flac->container == ma_dr_flac_container_ogg)
		{
			*totalPCMFrames = flac->totalPCMFrameCount;
			while ((framesRead = ma_dr_flac_read_pcm_frames_s16 (flac, sizeof (samples) / sizeof (samples[0]), samples)) > 0)
			{
				for (ma_uint64 index = 0; index < framesRead; ++index)
				{
					unsigned short sample = (unsigned short)samples[index];
					hash = (hash ^ (sample & 0xff)) * 1099511628211ULL;
					hash = (hash ^ (sample >> 8)) * 1099511628211ULL;
					if (totalRead == 0)
					{
						*firstSample = samples[index];
					}
					totalRead++;
				}
			}
			if (totalRead == *totalPCMFrames && ma_dr_flac_read_pcm_frames_s16 (flac, 1, samples) == 0 && ma_dr_flac_seek_to_pcm_frame (flac, seekFrame) && ma_dr_flac_read_pcm_frames_s16 (flac, 1, seekSample) == 1 && ma_dr_flac_seek_to_pcm_frame (flac, *totalPCMFrames) && ma_dr_flac_read_pcm_frames_s16 (flac, 1, samples) == 0)
			{
				*pcmHash = hash;
			}
			else
			{
				report->Opened = false;
			}
		}
		else
		{
			report->Opened = false;
		}
		ma_dr_flac_close (flac);
	}
	report->CanaryIntact = probe.CanaryIntact;
	report->ReallocExistingPointer = probe.ReallocExistingPointer;
	report->MallocCount = probe.AllocationCount;
	report->ReallocCount = probe.ReallocCount;
	report->FreeCount = probe.FreeCount;
	memcpy (report->AllocationPointers, probe.RawPointers, sizeof (report->AllocationPointers));
	memcpy (report->FreePointers, probe.FreePointers, sizeof (report->FreePointers));
	return true;
}

bool AudioDecoderTestMiniaudioFlacDecodeSeekpoint (const unsigned char *data, std::size_t bytes, unsigned long long *firstPCMFrame, unsigned long long *flacFrameOffset, unsigned int *pcmFrameCount)
{
	FlacSeekpointReader reader = { data, bytes, 0 };
	ma_dr_flac_seekpoint seekpoint;
	if (data == NULL || firstPCMFrame == NULL || flacFrameOffset == NULL || pcmFrameCount == NULL || !ma_dr_flac__read_seekpoint (FlacSeekpointReaderRead, &reader, &seekpoint))
	{
		return false;
	}
	*firstPCMFrame = seekpoint.firstPCMFrame;
	*flacFrameOffset = seekpoint.flacFrameOffset;
	*pcmFrameCount = seekpoint.pcmFrameCount;
	return true;
}

bool AudioDecoderTestMiniaudioFlacOpenSeektable (const unsigned char *data, std::size_t bytes, bool withMetadata, unsigned int *seekpointCount, unsigned long long *firstPCMFrame, unsigned long long *flacFrameOffset, unsigned int *pcmFrameCount, unsigned int *metadataRawDataSize)
{
	FlacMetadataProbe metadata = { 0, false, 0, 0, 0, 0, 0 };
	ma_dr_flac *flac = withMetadata ? ma_dr_flac_open_memory_with_metadata (data, bytes, FlacMetadataProbeCallback, &metadata, NULL) : ma_dr_flac_open_memory (data, bytes, NULL);
	if (flac == NULL)
	{
		return false;
	}
	*seekpointCount = flac->seekpointCount;
	*firstPCMFrame = flac->seekpointCount > 0 ? flac->pSeekpoints[0].firstPCMFrame : metadata.FirstPCMFrame;
	*flacFrameOffset = flac->seekpointCount > 0 ? flac->pSeekpoints[0].flacFrameOffset : metadata.FlacFrameOffset;
	*pcmFrameCount = flac->seekpointCount > 0 ? flac->pSeekpoints[0].pcmFrameCount : metadata.PCMFrameCount;
	*metadataRawDataSize = metadata.RawDataSize;
	ma_dr_flac_close (flac);
	return true;
}
#endif