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

bool AudioDecoderTestMiniaudioFlacCuesheetAlignSize (std::size_t value, std::size_t alignment, std::size_t *result)
{
	return ma_dr_flac__cuesheet_align_size (value, alignment, result) != MA_FALSE;
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
		std::size_t CuesheetCount;
		bool CuesheetRawDataLive;
		std::size_t CuesheetRawDataSize;
		unsigned int CuesheetTrackCount;
		bool CuesheetTrackDataAligned;
		bool CuesheetIndexDataAligned;
		bool CuesheetIteratorNullOutputAdvanced;
		bool CuesheetIteratorEOF;
		unsigned long long CuesheetLeadInSampleCount;
		unsigned long long CuesheetFirstTrackOffset;
		unsigned long long CuesheetFirstIndexOffset;
		unsigned int CuesheetFirstTrackNumber;
		unsigned int CuesheetFirstIndexNumber;
		unsigned int CuesheetSecondTrackNumber;
		bool CuesheetFirstTrackIsAudio;
		bool CuesheetFirstTrackPreEmphasis;
		char CuesheetFirstTrackISRC[13];
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

	struct CuesheetAllocationProbe
	{
		std::size_t AllocationCount;
		std::size_t FreeCount;
		void *HostPointers[5];
		void *RawPointers[5];
		std::size_t AllocationSizes[5];
		void *FreePointers[5];
		bool CanaryIntact;
		std::size_t FailAllocationOrdinal;
	};

	void *CuesheetAllocationProbeMalloc (std::size_t size, void *userData)
	{
		CuesheetAllocationProbe *probe = static_cast<CuesheetAllocationProbe *> (userData);
		std::size_t allocationOrdinal = ++probe->AllocationCount;
		unsigned char *host;
		uintptr_t address;
		unsigned char *raw;
		if (probe->FailAllocationOrdinal == allocationOrdinal || allocationOrdinal > 5)
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
		probe->HostPointers[allocationOrdinal - 1] = host;
		probe->RawPointers[allocationOrdinal - 1] = raw;
		probe->AllocationSizes[allocationOrdinal - 1] = size;
		memset (raw + size, 0xa5, 8);
		return raw;
	}

	void CuesheetAllocationProbeFree (void *pointer, void *userData)
	{
		CuesheetAllocationProbe *probe = static_cast<CuesheetAllocationProbe *> (userData);
		++probe->FreeCount;
		if (probe->FreeCount <= 5)
		{
			probe->FreePointers[probe->FreeCount - 1] = pointer;
		}
		for (std::size_t allocationIndex = 0; allocationIndex < 5; ++allocationIndex)
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

	bool CuesheetAllocationProbeHasLiveAllocations (const CuesheetAllocationProbe *probe)
	{
		for (std::size_t allocationIndex = 0; allocationIndex < 5; ++allocationIndex)
		{
			if (probe->HostPointers[allocationIndex] != NULL)
			{
				return true;
			}
		}
		return false;
	}

	void CuesheetAllocationProbeReleaseLiveAllocations (CuesheetAllocationProbe *probe)
	{
		for (std::size_t allocationIndex = 0; allocationIndex < 5; ++allocationIndex)
		{
			if (probe->HostPointers[allocationIndex] != NULL)
			{
				free (probe->HostPointers[allocationIndex]);
				probe->HostPointers[allocationIndex] = NULL;
			}
		}
	}

	struct CuesheetCallbackProbe
	{
		CuesheetAllocationProbe Allocation;
		const unsigned char *Data;
		std::size_t Bytes;
		std::size_t Position;
		bool CallbackObserved;
		bool RawDataLive;
		unsigned int TrackCount;
		bool TrackDataAligned;
		bool IndexDataAligned;
		bool IteratorNullOutputAdvanced;
		bool IteratorEOF;
		const unsigned char *ExpectedRawData;
		std::size_t ExpectedRawDataSize;
		bool RawDataMatchesExpected;
		std::size_t RawDataSize;
		bool IsCD;
		char Catalog[129];
		unsigned long long LeadInSampleCount;
		unsigned long long FirstTrackOffset;
		unsigned long long FirstIndexOffset;
		unsigned int FirstTrackNumber;
		unsigned int FirstIndexNumber;
		unsigned int FirstTrackIndexCount;
		bool FirstTrackIndexPointerNull;
		bool FirstTrackIsAudio;
		bool FirstTrackPreEmphasis;
		char FirstTrackISRC[13];
		unsigned long long SecondTrackOffset;
		unsigned int SecondTrackNumber;
		unsigned int SecondTrackIndexCount;
		bool SecondTrackIndexPointerNull;
		bool SecondTrackIsAudio;
		bool SecondTrackPreEmphasis;
		char SecondTrackISRC[13];
	};

	struct FlacCallbackProbe
	{
		FlacAllocationProbe Allocation;
		FlacMetadataProbe Metadata;
		CuesheetCallbackProbe Cuesheet;
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

	bool IsLiveCuesheetRawData (const ma_dr_flac_metadata *metadata, const void *const *rawPointers, const void *const *hostPointers, std::size_t allocationCount)
	{
		for (std::size_t index = 0; index < allocationCount; ++index)
		{
			if (metadata->pRawData == rawPointers[index] && hostPointers[index] != NULL)
			{
				return true;
			}
		}
		return false;
	}

	bool IsLiveCuesheetRange (const void *pointer, std::size_t bytes, const void *const *rawPointers, const void *const *hostPointers, const std::size_t *allocationSizes, std::size_t allocationCount)
	{
		uintptr_t begin = reinterpret_cast<uintptr_t> (pointer);
		for (std::size_t index = 0; index < allocationCount; ++index)
		{
			uintptr_t allocationBegin = reinterpret_cast<uintptr_t> (rawPointers[index]);
			if (hostPointers[index] != NULL && begin >= allocationBegin && begin - allocationBegin <= allocationSizes[index] && bytes <= allocationSizes[index] - (begin - allocationBegin))
			{
				return true;
			}
		}
		return false;
	}

	void ObserveCuesheetMetadata (ma_dr_flac_metadata *metadata, bool rawDataLive, const void *const *rawPointers, const void *const *hostPointers, const std::size_t *allocationSizes, std::size_t allocationCount, CuesheetCallbackProbe *probe)
	{
		ma_dr_flac_cuesheet_track_iterator iterator;
		ma_dr_flac_cuesheet_track track;
		if (metadata->type != MA_DR_FLAC_METADATA_BLOCK_TYPE_CUESHEET)
		{
			return;
		}
		probe->CallbackObserved = true;
		probe->TrackCount = metadata->data.cuesheet.trackCount;
		probe->RawDataLive = rawDataLive;
		probe->RawDataSize = metadata->rawDataSize;
		if (!probe->RawDataLive)
		{
			return;
		}
		probe->RawDataMatchesExpected = probe->ExpectedRawData != NULL && probe->RawDataSize == probe->ExpectedRawDataSize && memcmp (metadata->pRawData, probe->ExpectedRawData, probe->RawDataSize) == 0;
		probe->IsCD = metadata->data.cuesheet.isCD != 0;
		probe->LeadInSampleCount = metadata->data.cuesheet.leadInSampleCount;
		memcpy (probe->Catalog, metadata->data.cuesheet.catalog, 128);
		probe->Catalog[128] = '\0';
		probe->TrackDataAligned = probe->TrackCount == 0 || (metadata->data.cuesheet.pTrackData != NULL && (reinterpret_cast<uintptr_t> (metadata->data.cuesheet.pTrackData) % alignof (ma_dr_flac_cuesheet_track)) == 0 && IsLiveCuesheetRange (metadata->data.cuesheet.pTrackData, (std::size_t)probe->TrackCount * sizeof (ma_dr_flac_cuesheet_track), rawPointers, hostPointers, allocationSizes, allocationCount));
		if (!probe->TrackDataAligned)
		{
			return;
		}
		ma_dr_flac_init_cuesheet_track_iterator (&iterator, probe->TrackCount, metadata->data.cuesheet.pTrackData);
		if (ma_dr_flac_next_cuesheet_track (&iterator, &track))
		{
			probe->FirstTrackOffset = track.offset;
			probe->FirstTrackNumber = track.trackNumber;
			probe->FirstTrackIndexCount = track.indexCount;
			probe->FirstTrackIndexPointerNull = track.pIndexPoints == NULL;
			probe->FirstTrackIsAudio = track.isAudio != 0;
			probe->FirstTrackPreEmphasis = track.preEmphasis != 0;
			memcpy (probe->FirstTrackISRC, track.ISRC, sizeof (track.ISRC));
			probe->FirstTrackISRC[sizeof (track.ISRC)] = '\0';
			if (track.indexCount == 0)
			{
				probe->IndexDataAligned = track.pIndexPoints == NULL;
			}
			else if (track.pIndexPoints != NULL && (reinterpret_cast<uintptr_t> (track.pIndexPoints) % alignof (ma_dr_flac_cuesheet_track_index)) == 0 && IsLiveCuesheetRange (track.pIndexPoints, (std::size_t)track.indexCount * sizeof (ma_dr_flac_cuesheet_track_index), rawPointers, hostPointers, allocationSizes, allocationCount))
			{
				probe->IndexDataAligned = true;
				probe->FirstIndexOffset = track.pIndexPoints[0].offset;
				probe->FirstIndexNumber = track.pIndexPoints[0].index;
			}
			else
			{
				return;
			}
		}
		ma_dr_flac_init_cuesheet_track_iterator (&iterator, probe->TrackCount, metadata->data.cuesheet.pTrackData);
		if (ma_dr_flac_next_cuesheet_track (&iterator, NULL) && ma_dr_flac_next_cuesheet_track (&iterator, &track))
		{
			probe->SecondTrackOffset = track.offset;
			probe->SecondTrackNumber = track.trackNumber;
			probe->SecondTrackIndexCount = track.indexCount;
			probe->SecondTrackIndexPointerNull = track.pIndexPoints == NULL;
			probe->SecondTrackIsAudio = track.isAudio != 0;
			probe->SecondTrackPreEmphasis = track.preEmphasis != 0;
			memcpy (probe->SecondTrackISRC, track.ISRC, sizeof (track.ISRC));
			probe->SecondTrackISRC[sizeof (track.ISRC)] = '\0';
			probe->IteratorNullOutputAdvanced = true;
		}
		probe->IteratorEOF = ma_dr_flac_next_cuesheet_track (&iterator, NULL) == MA_FALSE;
	}

	size_t CuesheetCallbackProbeRead (void *userData, void *output, size_t bytes)
	{
		CuesheetCallbackProbe *probe = static_cast<CuesheetCallbackProbe *> (userData);
		std::size_t available = probe->Bytes - probe->Position;
		std::size_t count = available < bytes ? available : bytes;
		memcpy (output, probe->Data + probe->Position, count);
		probe->Position += count;
		return count;
	}

	ma_bool32 CuesheetCallbackProbeSeek (void *userData, int offset, ma_dr_flac_seek_origin origin)
	{
		CuesheetCallbackProbe *probe = static_cast<CuesheetCallbackProbe *> (userData);
		long long position = origin == MA_DR_FLAC_SEEK_SET ? offset : (long long)probe->Position + offset;
		if (position < 0 || (unsigned long long)position > probe->Bytes)
		{
			return MA_FALSE;
		}
		probe->Position = (std::size_t)position;
		return MA_TRUE;
	}

	ma_bool32 CuesheetCallbackProbeTell (void *userData, ma_int64 *position)
	{
		*position = (ma_int64)static_cast<CuesheetCallbackProbe *> (userData)->Position;
		return MA_TRUE;
	}

	void CuesheetCallbackProbeMetadata (void *userData, ma_dr_flac_metadata *metadata)
	{
		CuesheetCallbackProbe *probe = static_cast<CuesheetCallbackProbe *> (userData);
				ObserveCuesheetMetadata (metadata, IsLiveCuesheetRawData (metadata, probe->Allocation.RawPointers, probe->Allocation.HostPointers, 5), probe->Allocation.RawPointers, probe->Allocation.HostPointers, probe->Allocation.AllocationSizes, 5, probe);
	}

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
		else if (metadata->type == MA_DR_FLAC_METADATA_BLOCK_TYPE_CUESHEET)
		{
			++probe->Metadata.CuesheetCount;
			ObserveCuesheetMetadata (metadata, IsLiveCuesheetRawData (metadata, probe->Allocation.RawPointers, probe->Allocation.HostPointers, 4), probe->Allocation.RawPointers, probe->Allocation.HostPointers, probe->Allocation.AllocationSizes, 4, &probe->Cuesheet);
		}
	}

	void CopyCuesheetMetadataReport (AudioDecoderTestMiniaudioFlacCallbackReport *report, const CuesheetCallbackProbe *probe)
	{
		report->MetadataCuesheetRawDataLive = probe->RawDataLive;
		report->MetadataCuesheetRawDataSize = probe->RawDataSize;
		report->MetadataCuesheetTrackCount = probe->TrackCount;
		report->MetadataCuesheetTrackDataAligned = probe->TrackDataAligned;
		report->MetadataCuesheetIndexDataAligned = probe->IndexDataAligned;
		report->MetadataCuesheetIteratorNullOutputAdvanced = probe->IteratorNullOutputAdvanced;
		report->MetadataCuesheetIteratorEOF = probe->IteratorEOF;
		report->MetadataCuesheetLeadInSampleCount = probe->LeadInSampleCount;
		report->MetadataCuesheetFirstTrackOffset = probe->FirstTrackOffset;
		report->MetadataCuesheetFirstIndexOffset = probe->FirstIndexOffset;
		report->MetadataCuesheetFirstTrackNumber = probe->FirstTrackNumber;
		report->MetadataCuesheetFirstIndexNumber = probe->FirstIndexNumber;
		report->MetadataCuesheetSecondTrackNumber = probe->SecondTrackNumber;
		report->MetadataCuesheetFirstTrackIsAudio = probe->FirstTrackIsAudio;
		report->MetadataCuesheetFirstTrackPreEmphasis = probe->FirstTrackPreEmphasis;
		memcpy (report->MetadataCuesheetFirstTrackISRC, probe->FirstTrackISRC, sizeof (report->MetadataCuesheetFirstTrackISRC));
	}

	void CopyCuesheetMetadataDetails (AudioDecoderTestMiniaudioFlacCallbackReport *report, const CuesheetCallbackProbe *probe)
	{
		report->MetadataCuesheetRawDataMatchesExpected = probe->RawDataMatchesExpected;
		report->MetadataCuesheetIsCD = probe->IsCD;
		memcpy (report->MetadataCuesheetCatalog, probe->Catalog, sizeof (report->MetadataCuesheetCatalog));
		report->MetadataCuesheetFirstTrackIndexCount = probe->FirstTrackIndexCount;
		report->MetadataCuesheetFirstTrackIndexPointerNull = probe->FirstTrackIndexPointerNull;
		report->MetadataCuesheetSecondTrackOffset = probe->SecondTrackOffset;
		report->MetadataCuesheetSecondTrackIsAudio = probe->SecondTrackIsAudio;
		report->MetadataCuesheetSecondTrackPreEmphasis = probe->SecondTrackPreEmphasis;
		memcpy (report->MetadataCuesheetSecondTrackISRC, probe->SecondTrackISRC, sizeof (report->MetadataCuesheetSecondTrackISRC));
		report->MetadataCuesheetSecondTrackIndexCount = probe->SecondTrackIndexCount;
		report->MetadataCuesheetSecondTrackIndexPointerNull = probe->SecondTrackIndexPointerNull;
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

bool AudioDecoderTestMiniaudioFlacCallbackOpen (const unsigned char *data, std::size_t bytes, bool withMetadata, bool reallocOnly, std::size_t failAllocationOrdinal, std::size_t failReadPosition, std::size_t failSeekOrdinal, std::size_t failSeekPosition, AudioDecoderTestMiniaudioFlacCallbackReport *report, bool corruptCanaryBeforeClose, const unsigned char *expectedCuesheetRawData, std::size_t expectedCuesheetRawDataSize)
{
	FlacCallbackProbe probe = {};
	ma_allocation_callbacks callbacks;
	ma_dr_flac *flac;
	if (data == NULL || report == NULL)
	{
		return false;
	}
	probe.Allocation.CanaryIntact = true;
	probe.Allocation.FailAllocationOrdinal = failAllocationOrdinal;
	probe.Data = data;
	probe.Bytes = bytes;
	probe.FailReadPosition = failReadPosition;
	probe.FailSeekOrdinal = failSeekOrdinal;
	probe.FailSeekPosition = failSeekPosition;
	probe.Cuesheet.ExpectedRawData = expectedCuesheetRawData;
	probe.Cuesheet.ExpectedRawDataSize = expectedCuesheetRawDataSize;
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
	report->MetadataCuesheetCount = probe.Metadata.CuesheetCount;
	CopyCuesheetMetadataReport (report, &probe.Cuesheet);
	CopyCuesheetMetadataDetails (report, &probe.Cuesheet);
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

bool AudioDecoderTestMiniaudioOggFlacCuesheetCallbackOpen (const unsigned char *data, std::size_t bytes, std::size_t failAllocationOrdinal, AudioDecoderTestMiniaudioCuesheetCallbackReport *report, const unsigned char *expectedCuesheetRawData, std::size_t expectedCuesheetRawDataSize)
{
	CuesheetCallbackProbe probe = {};
	ma_allocation_callbacks callbacks;
	ma_dr_flac *flac;
	if (data == NULL || report == NULL)
	{
		return false;
	}
	probe.Allocation.CanaryIntact = true;
	probe.Allocation.FailAllocationOrdinal = failAllocationOrdinal;
	probe.Data = data;
	probe.Bytes = bytes;
	probe.ExpectedRawData = expectedCuesheetRawData;
	probe.ExpectedRawDataSize = expectedCuesheetRawDataSize;
	callbacks.pUserData = &probe;
	callbacks.onMalloc = CuesheetAllocationProbeMalloc;
	callbacks.onRealloc = NULL;
	callbacks.onFree = CuesheetAllocationProbeFree;
	flac = ma_dr_flac_open_with_metadata (CuesheetCallbackProbeRead, CuesheetCallbackProbeSeek, CuesheetCallbackProbeTell, CuesheetCallbackProbeMetadata, &probe, &callbacks);
	memset (report, 0, sizeof (*report));
	report->Opened = flac != NULL;
	if (flac != NULL)
	{
		ma_dr_flac_close (flac);
	}
	report->CallbackObserved = probe.CallbackObserved;
	report->CanaryIntact = probe.Allocation.CanaryIntact;
	report->MallocCount = probe.Allocation.AllocationCount;
	report->FreeCount = probe.Allocation.FreeCount;
	report->MetadataRawDataLive = probe.RawDataLive;
	report->MetadataTrackCount = probe.TrackCount;
	report->MetadataTrackDataAligned = probe.TrackDataAligned;
	report->MetadataIndexDataAligned = probe.IndexDataAligned;
	report->MetadataIteratorNullOutputAdvanced = probe.IteratorNullOutputAdvanced;
	report->MetadataIteratorEOF = probe.IteratorEOF;
	report->MetadataRawDataMatchesExpected = probe.RawDataMatchesExpected;
	report->MetadataRawDataSize = probe.RawDataSize;
	report->MetadataIsCD = probe.IsCD;
	memcpy (report->MetadataCatalog, probe.Catalog, sizeof (report->MetadataCatalog));
	report->MetadataLeadInSampleCount = probe.LeadInSampleCount;
	report->MetadataFirstTrackOffset = probe.FirstTrackOffset;
	report->MetadataFirstIndexOffset = probe.FirstIndexOffset;
	report->MetadataFirstTrackNumber = probe.FirstTrackNumber;
	report->MetadataFirstIndexNumber = probe.FirstIndexNumber;
	report->MetadataFirstTrackIndexCount = probe.FirstTrackIndexCount;
	report->MetadataFirstTrackIndexPointerNull = probe.FirstTrackIndexPointerNull;
	report->MetadataFirstTrackIsAudio = probe.FirstTrackIsAudio;
	report->MetadataFirstTrackPreEmphasis = probe.FirstTrackPreEmphasis;
	memcpy (report->MetadataFirstTrackISRC, probe.FirstTrackISRC, sizeof (report->MetadataFirstTrackISRC));
	report->MetadataSecondTrackOffset = probe.SecondTrackOffset;
	report->MetadataSecondTrackNumber = probe.SecondTrackNumber;
	report->MetadataSecondTrackIndexCount = probe.SecondTrackIndexCount;
	report->MetadataSecondTrackIndexPointerNull = probe.SecondTrackIndexPointerNull;
	report->MetadataSecondTrackIsAudio = probe.SecondTrackIsAudio;
	report->MetadataSecondTrackPreEmphasis = probe.SecondTrackPreEmphasis;
	memcpy (report->MetadataSecondTrackISRC, probe.SecondTrackISRC, sizeof (report->MetadataSecondTrackISRC));
	memcpy (report->AllocationPointers, probe.Allocation.RawPointers, sizeof (report->AllocationPointers));
	memcpy (report->FreePointers, probe.Allocation.FreePointers, sizeof (report->FreePointers));
	bool allocationsReleased = !CuesheetAllocationProbeHasLiveAllocations (&probe.Allocation);
	CuesheetAllocationProbeReleaseLiveAllocations (&probe.Allocation);
	return allocationsReleased;
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
	FlacMetadataProbe metadata = {};
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