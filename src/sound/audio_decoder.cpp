#include "audio_decoder.h"

#include <limits>
#include <string.h>

namespace
{
	const std::size_t ProbeLimit = 4096;
	const std::size_t MaxId3TagBytes = 1024 * 1024;
	const std::size_t MaxId3ProbeBytes = MaxId3TagBytes + 20 + 4096;
	const std::size_t MaxEncodedBytes = 64 * 1024 * 1024;
	const std::size_t MaxPCMBytes = (std::size_t)std::numeric_limits<int>::max ();

	bool HasBytes (std::size_t offset, std::size_t count, std::size_t length)
	{
		return offset <= length && count <= length - offset;
	}

	unsigned int ReadLE16 (const unsigned char *data)
	{
		return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
	}

	unsigned int ReadLE32 (const unsigned char *data)
	{
		return ReadLE16 (data) | (ReadLE16 (data + 2) << 16);
	}

	bool Matches (const std::vector<unsigned char> &data, std::size_t offset, const char *text, std::size_t length)
	{
		return HasBytes (offset, length, data.size ()) && memcmp (&data[offset], text, length) == 0;
	}

	bool IsPartialSignature (const std::vector<unsigned char> &data, const char *text, std::size_t length)
	{
		return !data.empty () && data.size () < length && memcmp (&data[0], text, data.size ()) == 0;
	}

	bool SeekWithinSource (std::size_t base, long long offset, std::size_t length, std::size_t *position)
	{
		if (offset >= 0)
		{
			std::size_t amount = (std::size_t)offset;
			if ((long long)amount != offset || amount > length - base)
			{
				return false;
			}
			*position = base + amount;
			return true;
		}

		unsigned long long amount = (unsigned long long)(-(offset + 1)) + 1;
		if (amount > (unsigned long long)std::numeric_limits<std::size_t>::max () || amount > base)
		{
			return false;
		}
		*position = base - (std::size_t)amount;
		return true;
	}

	AudioProbeResult MalformedResult ()
	{
		AudioProbeResult result;
		result.Status = AUDIO_PROBE_MALFORMED;
		return result;
	}

	AudioProbeResult ProbeWaveFormat (const std::vector<unsigned char> &data, std::size_t chunkData, std::size_t chunkLength)
	{
		AudioProbeResult result;
		unsigned int format;
		if (chunkLength < 16)
		{
			return MalformedResult ();
		}
		format = ReadLE16 (&data[chunkData]);
		if (format != 1 && format != 3)
		{
			return result;
		}
		result.PCM.Channels = ReadLE16 (&data[chunkData + 2]);
		result.PCM.SampleRate = ReadLE32 (&data[chunkData + 4]);
		result.PCM.BitsPerSample = ReadLE16 (&data[chunkData + 14]);
		result.PCM.FloatingPoint = format == 3;
		if (result.PCM.Channels == 0 || result.PCM.SampleRate == 0 || result.PCM.BitsPerSample == 0)
		{
			return MalformedResult ();
		}
		result.Status = AUDIO_PROBE_RECOGNIZED;
		result.Format = format == 1 ? AUDIO_FORMAT_WAVE_PCM : AUDIO_FORMAT_WAVE_FLOAT;
		return result;
	}

	AudioProbeResult ProbeWave (const std::vector<unsigned char> &data, std::size_t available)
	{
		AudioProbeResult result;
		std::size_t offset = 12;
		std::size_t riffEnd;

		if (!Matches (data, 8, "WAVE", 4) || !HasBytes (4, 4, data.size ()))
		{
			return MalformedResult ();
		}
		if ((std::size_t)ReadLE32 (&data[4]) > std::numeric_limits<std::size_t>::max () - 8)
		{
			return MalformedResult ();
		}
		riffEnd = (std::size_t)ReadLE32 (&data[4]) + 8;
		if (riffEnd < 12 || riffEnd > available)
		{
			return MalformedResult ();
		}

		while (offset < riffEnd)
		{
			std::size_t chunkLength;
			std::size_t chunkData;
			if (!HasBytes (offset, 8, data.size ()) || offset > riffEnd - 8)
			{
				return MalformedResult ();
			}
			chunkLength = (std::size_t)ReadLE32 (&data[offset + 4]);
			chunkData = offset + 8;
			if (chunkLength > riffEnd - chunkData || !HasBytes (chunkData, chunkLength, data.size ()))
			{
				return MalformedResult ();
			}
			if (Matches (data, offset, "fmt ", 4))
			{
				return ProbeWaveFormat (data, chunkData, chunkLength);
			}
			if (chunkLength == riffEnd - chunkData)
			{
				break;
			}
			offset = chunkData + chunkLength;
			if (offset & 1)
			{
				if (offset == riffEnd)
				{
					return MalformedResult ();
				}
				++offset;
			}
		}
		return MalformedResult ();
	}

	AudioProbeResult ProbeOgg (const std::vector<unsigned char> &data)
	{
		AudioProbeResult result;
		std::size_t segmentCount;
		std::size_t payloadOffset;
		std::size_t packetLength = 0;
		std::size_t segment;

		if (data.size () < 27 || data[4] != 0)
		{
			return MalformedResult ();
		}
		segmentCount = data[26];
		payloadOffset = 27 + segmentCount;
		if (segmentCount == 0 || payloadOffset > data.size ())
		{
			return MalformedResult ();
		}
		for (segment = 0; segment < segmentCount; ++segment)
		{
			std::size_t length = data[27 + segment];
			if (length > data.size () - payloadOffset - packetLength)
			{
				return MalformedResult ();
			}
			packetLength += length;
			if (length < 255)
			{
				break;
			}
		}
		if (segment == segmentCount)
		{
			return MalformedResult ();
		}
		if (packetLength >= 7 && memcmp (&data[payloadOffset], "\x01vorbis", 7) == 0)
		{
			result.Status = AUDIO_PROBE_RECOGNIZED;
			result.Format = AUDIO_FORMAT_OGG_VORBIS;
		}
		else if (packetLength >= 8 && memcmp (&data[payloadOffset], "OpusHead", 8) == 0)
		{
			result.Status = AUDIO_PROBE_RECOGNIZED;
			result.Format = AUDIO_FORMAT_OGG_OPUS;
		}
		return result;
	}

	bool GetMpegFrameLength (const std::vector<unsigned char> &data, std::size_t offset, std::size_t *frameLength)
	{
		static const unsigned int mpeg1Layer1Bitrates[] = { 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 };
		static const unsigned int mpeg1Layer2Bitrates[] = { 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
		static const unsigned int mpeg1Layer3Bitrates[] = { 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
		static const unsigned int mpeg2Layer1Bitrates[] = { 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 };
		static const unsigned int mpeg2Layer2Or3Bitrates[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
		static const unsigned int sampleRates[4][3] = {
			{ 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 }
		};
		static const unsigned int frameSamples[4][4] = {
			{ 0, 72, 144, 12 }, { 0, 0, 0, 0 }, { 0, 72, 144, 12 }, { 0, 144, 144, 12 }
		};
		static const unsigned int frameMultipliers[] = { 0, 1, 1, 4 };
		static const unsigned int * const bitrates[4][4] = {
			{ NULL, mpeg2Layer2Or3Bitrates, mpeg2Layer2Or3Bitrates, mpeg2Layer1Bitrates },
			{ NULL, NULL, NULL, NULL },
			{ NULL, mpeg2Layer2Or3Bitrates, mpeg2Layer2Or3Bitrates, mpeg2Layer1Bitrates },
			{ NULL, mpeg1Layer3Bitrates, mpeg1Layer2Bitrates, mpeg1Layer1Bitrates }
		};
		unsigned int version;
		unsigned int layer;
		unsigned int bitrateIndex;
		unsigned int sampleRateIndex;
		unsigned int padding;
		std::size_t bitrate;

		if (frameLength == NULL || !HasBytes (offset, 4, data.size ()) || data[offset] != 0xff || (data[offset + 1] & 0xe0) != 0xe0 ||
			(data[offset + 3] & 3) == 2)
		{
			return false;
		}
		version = (data[offset + 1] >> 3) & 3;
		layer = (data[offset + 1] >> 1) & 3;
		bitrateIndex = (data[offset + 2] >> 4) & 15;
		sampleRateIndex = (data[offset + 2] >> 2) & 3;
		padding = (data[offset + 2] >> 1) & 1;
		if (version == 1 || layer == 0 || bitrateIndex == 0 || bitrateIndex == 15 || sampleRateIndex == 3)
		{
			return false;
		}
		bitrate = (std::size_t)bitrates[version][layer][bitrateIndex - 1] * 1000;
		*frameLength = ((std::size_t)frameSamples[version][layer] * bitrate) / sampleRates[version][sampleRateIndex] + padding;
		*frameLength *= frameMultipliers[layer];
		return *frameLength != 0;
	}

	bool HasCompleteMpegFrame (const std::vector<unsigned char> &data, std::size_t offset, std::size_t available)
	{
		std::size_t frameLength;
		return GetMpegFrameLength (data, offset, &frameLength) && HasBytes (offset, frameLength, data.size ()) &&
			HasBytes (offset, frameLength, available);
	}

	bool IsValidId3Header (const std::vector<unsigned char> &data)
	{
		unsigned char allowedFlags;
		if (data.size () < 10 || data[3] < 2 || data[3] > 4 || data[4] == 0xff)
		{
			return false;
		}
		allowedFlags = data[3] == 2 ? 0xc0 : data[3] == 3 ? 0xe0 : 0xf0;
		return (data[5] & ~allowedFlags) == 0 && (data[6] & 0x80) == 0 && (data[7] & 0x80) == 0 &&
			(data[8] & 0x80) == 0 && (data[9] & 0x80) == 0;
	}

	bool GetId3FrameOffset (const std::vector<unsigned char> &data, std::size_t available, std::size_t *frameOffset)
	{
		std::size_t tagLength = ((std::size_t)data[6] << 21) | ((std::size_t)data[7] << 14) |
			((std::size_t)data[8] << 7) | (std::size_t)data[9];
		std::size_t tagEnd;
		if (frameOffset == NULL || tagLength > std::numeric_limits<std::size_t>::max () - 10)
		{
			return false;
		}
		tagEnd = 10 + tagLength;
		*frameOffset = tagEnd;
		if (data[3] == 4 && (data[5] & 0x10) != 0)
		{
			if (tagEnd > std::numeric_limits<std::size_t>::max () - 10)
			{
				return false;
			}
			*frameOffset = tagEnd + 10;
		}
		return *frameOffset <= available;
	}

	bool HasValidId3Footer (const std::vector<unsigned char> &data, std::size_t frameOffset)
	{
		std::size_t footerOffset;
		if (frameOffset < 10)
		{
			return false;
		}
		footerOffset = frameOffset - 10;
		return Matches (data, footerOffset, "3DI", 3) && data[footerOffset + 3] == data[3] &&
			data[footerOffset + 4] == data[4] && data[footerOffset + 5] == data[5] &&
			data[footerOffset + 6] == data[6] && data[footerOffset + 7] == data[7] &&
			data[footerOffset + 8] == data[8] && data[footerOffset + 9] == data[9];
	}

	bool IsId3FooterRequired (const std::vector<unsigned char> &data)
	{
		return data[3] == 4 && (data[5] & 0x10) != 0;
	}

	bool IsId3TagWithinProbeLimit (const std::vector<unsigned char> &data, std::size_t frameOffset)
	{
		std::size_t maximumOffset = MaxId3TagBytes + (IsId3FooterRequired (data) ? 20 : 10);
		return frameOffset <= maximumOffset;
	}

	AudioSourceStatus ExtendProbe (AudioDataSource &source, std::vector<unsigned char> *data, std::size_t required)
	{
		std::size_t originalSize;
		std::size_t bytesRead = 0;
		AudioSourceStatus status;
		if (data == NULL || required <= data->size ())
		{
			return AUDIO_SOURCE_OK;
		}
		originalSize = data->size ();
		data->resize (required);
		status = source.Read (&(*data)[originalSize], required - originalSize, &bytesRead);
		data->resize (originalSize + bytesRead);
		return status;
	}

	AudioSourceStatus ExtendId3Probe (AudioDataSource &source, std::vector<unsigned char> *data, std::size_t available, bool *exceedsLimit)
	{
		std::size_t frameOffset;
		std::size_t frameLength;
		AudioSourceStatus status;
		*exceedsLimit = false;
		if (!Matches (*data, 0, "ID3", 3) || !IsValidId3Header (*data) || !GetId3FrameOffset (*data, available, &frameOffset))
		{
			return AUDIO_SOURCE_OK;
		}
		if (!IsId3TagWithinProbeLimit (*data, frameOffset))
		{
			*exceedsLimit = true;
			return AUDIO_SOURCE_OK;
		}
		status = ExtendProbe (source, data, frameOffset + 4);
		if (status == AUDIO_SOURCE_OK && GetMpegFrameLength (*data, frameOffset, &frameLength) &&
			frameLength <= MaxId3ProbeBytes - frameOffset)
		{
			status = ExtendProbe (source, data, frameOffset + frameLength);
		}
		return status;
	}

	AudioProbeResult ProbeMpeg (const std::vector<unsigned char> &data, std::size_t available)
	{
		AudioProbeResult result;
		std::size_t frameOffset;
		if (!Matches (data, 0, "ID3", 3))
		{
			if (HasCompleteMpegFrame (data, 0, available))
			{
				result.Status = AUDIO_PROBE_RECOGNIZED;
				result.Format = AUDIO_FORMAT_MPEG;
			}
			return result;
		}

		if (!IsValidId3Header (data) || !GetId3FrameOffset (data, available, &frameOffset) ||
			(IsId3FooterRequired (data) && !HasValidId3Footer (data, frameOffset)))
		{
			return MalformedResult ();
		}
		if (HasCompleteMpegFrame (data, frameOffset, available))
		{
			result.Status = AUDIO_PROBE_RECOGNIZED;
			result.Format = AUDIO_FORMAT_MPEG;
		}
		return result;
	}

	bool HasPartialKnownSignature (const std::vector<unsigned char> &data)
	{
		return IsPartialSignature (data, "RIFF", 4) || IsPartialSignature (data, "fLaC", 4) ||
			IsPartialSignature (data, "OggS", 4) || IsPartialSignature (data, "ID3", 3);
	}

	AudioSourceStatus OpenFile (const char *filename, std::size_t offset, std::size_t length, bool wholeFile, FILE **file, std::size_t *fileLength)
	{
		FILE *opened;
		long end;
		std::size_t total;

		if (filename == NULL || file == NULL || fileLength == NULL)
		{
			return AUDIO_SOURCE_INVALID_ARGUMENT;
		}
		opened = fopen (filename, "rb");
		if (opened == NULL)
		{
			return AUDIO_SOURCE_IO_ERROR;
		}
		if (fseek (opened, 0, SEEK_END) != 0 || (end = ftell (opened)) < 0 ||
			(unsigned long long)end > (unsigned long long)std::numeric_limits<std::size_t>::max ())
		{
			fclose (opened);
			return AUDIO_SOURCE_IO_ERROR;
		}
		total = (std::size_t)end;
		if (wholeFile)
		{
			offset = 0;
			length = total;
		}
		if (offset > total || length > total - offset || offset > (std::size_t)std::numeric_limits<long>::max ())
		{
			fclose (opened);
			return AUDIO_SOURCE_RANGE_ERROR;
		}
		if (fseek (opened, (long)offset, SEEK_SET) != 0)
		{
			fclose (opened);
			return AUDIO_SOURCE_IO_ERROR;
		}
		*file = opened;
		*fileLength = length;
		return AUDIO_SOURCE_OK;
	}
}

AudioDecodeStatus CreateMiniaudioDecoder (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder);
AudioDecodeStatus CreateVorbisDecoder (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder);

AudioDataSource::~AudioDataSource ()
{
}

AudioMemorySource::AudioMemorySource ()
	: Position (0)
{
}

AudioMemorySource::AudioMemorySource (const void *data, std::size_t bytes)
	: Position (0)
{
	Assign (data, bytes);
}

AudioSourceStatus AudioMemorySource::Assign (const void *data, std::size_t bytes)
{
	if (data == NULL && bytes != 0)
	{
		return AUDIO_SOURCE_INVALID_ARGUMENT;
	}
	Data.clear ();
	if (bytes != 0)
	{
		const unsigned char *input = static_cast<const unsigned char *> (data);
		Data.assign (input, input + bytes);
	}
	Position = 0;
	return AUDIO_SOURCE_OK;
}

AudioSourceStatus AudioMemorySource::Read (void *data, std::size_t bytes, std::size_t *bytesRead)
{
	std::size_t available;
	std::size_t count;
	bool eof;
	if (bytesRead == NULL || (data == NULL && bytes != 0))
	{
		return AUDIO_SOURCE_INVALID_ARGUMENT;
	}
	*bytesRead = 0;
	available = Data.size () - Position;
	eof = bytes > available;
	count = eof ? available : bytes;
	if (count != 0)
	{
		memcpy (data, &Data[Position], count);
		Position += count;
		*bytesRead = count;
	}
	return eof ? AUDIO_SOURCE_EOF : AUDIO_SOURCE_OK;
}

AudioSourceStatus AudioMemorySource::Seek (AudioSeekOrigin origin, long long offset)
{
	std::size_t base;
	std::size_t position;
	if (origin == AUDIO_SEEK_BEGIN)
	{
		base = 0;
	}
	else if (origin == AUDIO_SEEK_CURRENT)
	{
		base = Position;
	}
	else if (origin == AUDIO_SEEK_END)
	{
		base = Data.size ();
	}
	else
	{
		return AUDIO_SOURCE_INVALID_ARGUMENT;
	}
	if (!SeekWithinSource (base, offset, Data.size (), &position))
	{
		return AUDIO_SOURCE_RANGE_ERROR;
	}
	Position = position;
	return AUDIO_SOURCE_OK;
}

std::size_t AudioMemorySource::Tell () const
{
	return Position;
}

std::size_t AudioMemorySource::GetLength () const
{
	return Data.size ();
}

AudioFileSource::AudioFileSource ()
	: File (NULL), FileOffset (0), Length (0), Position (0)
{
}

AudioFileSource::~AudioFileSource ()
{
	Close ();
}

AudioSourceStatus AudioFileSource::Open (const char *filename)
{
	FILE *opened = NULL;
	std::size_t length = 0;
	AudioSourceStatus status;
	Close ();
	status = OpenFile (filename, 0, 0, true, &opened, &length);
	if (status == AUDIO_SOURCE_OK)
	{
		File = opened;
		Length = length;
	}
	return status;
}

AudioSourceStatus AudioFileSource::OpenSlice (const char *filename, std::size_t offset, std::size_t length)
{
	FILE *opened = NULL;
	std::size_t actualLength = 0;
	AudioSourceStatus status;
	Close ();
	status = OpenFile (filename, offset, length, false, &opened, &actualLength);
	if (status == AUDIO_SOURCE_OK)
	{
		File = opened;
		FileOffset = offset;
		Length = actualLength;
	}
	return status;
}

void AudioFileSource::Close ()
{
	if (File != NULL)
	{
		fclose (File);
	}
	File = NULL;
	FileOffset = 0;
	Length = 0;
	Position = 0;
}

bool AudioFileSource::IsOpen () const
{
	return File != NULL;
}

AudioSourceStatus AudioFileSource::Read (void *data, std::size_t bytes, std::size_t *bytesRead)
{
	std::size_t available;
	std::size_t count;
	bool eof;
	if (File == NULL)
	{
		return AUDIO_SOURCE_IO_ERROR;
	}
	if (bytesRead == NULL || (data == NULL && bytes != 0))
	{
		return AUDIO_SOURCE_INVALID_ARGUMENT;
	}
	*bytesRead = 0;
	available = Length - Position;
	eof = bytes > available;
	count = eof ? available : bytes;
	if (count != 0)
	{
		std::size_t physicalPosition = FileOffset + Position;
		if (physicalPosition > (std::size_t)std::numeric_limits<long>::max () || fseek (File, (long)physicalPosition, SEEK_SET) != 0)
		{
			return AUDIO_SOURCE_IO_ERROR;
		}
		*bytesRead = fread (data, 1, count, File);
		Position += *bytesRead;
		if (*bytesRead != count)
		{
			return AUDIO_SOURCE_IO_ERROR;
		}
	}
	return eof ? AUDIO_SOURCE_EOF : AUDIO_SOURCE_OK;
}

AudioSourceStatus AudioFileSource::Seek (AudioSeekOrigin origin, long long offset)
{
	std::size_t base;
	std::size_t position;
	if (File == NULL)
	{
		return AUDIO_SOURCE_IO_ERROR;
	}
	if (origin == AUDIO_SEEK_BEGIN)
	{
		base = 0;
	}
	else if (origin == AUDIO_SEEK_CURRENT)
	{
		base = Position;
	}
	else if (origin == AUDIO_SEEK_END)
	{
		base = Length;
	}
	else
	{
		return AUDIO_SOURCE_INVALID_ARGUMENT;
	}
	if (!SeekWithinSource (base, offset, Length, &position))
	{
		return AUDIO_SOURCE_RANGE_ERROR;
	}
	Position = position;
	return AUDIO_SOURCE_OK;
}

std::size_t AudioFileSource::Tell () const
{
	return Position;
}

std::size_t AudioFileSource::GetLength () const
{
	return Length;
}

AudioPCMMetadata::AudioPCMMetadata ()
	: SampleRate (0), Channels (0), BitsPerSample (0), FloatingPoint (false)
{
}

AudioFrameRange::AudioFrameRange ()
	: Start (0), End (0)
{
}

AudioDecodedPCM16::AudioDecodedPCM16 ()
	: SampleRate (0), Channels (0)
{
}

AudioProbeResult::AudioProbeResult ()
	: Status (AUDIO_PROBE_UNKNOWN), Format (AUDIO_FORMAT_UNKNOWN)
{
}

AudioProbeResult ProbeAudioFormat (AudioDataSource &source)
{
	AudioProbeResult result;
	std::size_t originalPosition = source.Tell ();
	std::size_t remaining = source.GetLength () - originalPosition;
	std::size_t requested = remaining < ProbeLimit ? remaining : ProbeLimit;
	std::vector<unsigned char> data (requested);
	std::size_t bytesRead = 0;
	AudioSourceStatus status = AUDIO_SOURCE_OK;
	bool id3ExceedsProbeLimit = false;

	if (requested != 0)
	{
		status = source.Read (&data[0], requested, &bytesRead);
		data.resize (bytesRead);
	}
	if (status == AUDIO_SOURCE_OK)
	{
		status = ExtendId3Probe (source, &data, remaining, &id3ExceedsProbeLimit);
	}
	if (source.Seek (AUDIO_SEEK_BEGIN, (long long)originalPosition) != AUDIO_SOURCE_OK)
	{
		result.Status = AUDIO_PROBE_IO_ERROR;
		return result;
	}
	if (id3ExceedsProbeLimit)
	{
		return MalformedResult ();
	}
	if (status == AUDIO_SOURCE_IO_ERROR || status == AUDIO_SOURCE_INVALID_ARGUMENT || status == AUDIO_SOURCE_RANGE_ERROR)
	{
		result.Status = AUDIO_PROBE_IO_ERROR;
		return result;
	}
	if (HasPartialKnownSignature (data))
	{
		return MalformedResult ();
	}
	if (Matches (data, 0, "RIFF", 4))
	{
		return ProbeWave (data, remaining);
	}
	if (Matches (data, 0, "fLaC", 4))
	{
		result.Status = AUDIO_PROBE_RECOGNIZED;
		result.Format = AUDIO_FORMAT_FLAC;
		return result;
	}
	if (Matches (data, 0, "OggS", 4))
	{
		return ProbeOgg (data);
	}
	return ProbeMpeg (data, remaining);
}

AudioDecoder::~AudioDecoder ()
{
}

AudioDecoderFactory::~AudioDecoderFactory ()
{
}

AudioDecodeStatus CreateAudioDecoder (AudioDataSource &source, const AudioProbeResult &probe, AudioDecoder **decoder)
{
	if (decoder == NULL)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	*decoder = NULL;
	if (probe.Status != AUDIO_PROBE_RECOGNIZED)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	if (probe.Format == AUDIO_FORMAT_OGG_VORBIS)
	{
		return CreateVorbisDecoder (source, probe, decoder);
	}
	if (probe.Format == AUDIO_FORMAT_WAVE_PCM || probe.Format == AUDIO_FORMAT_WAVE_FLOAT ||
		probe.Format == AUDIO_FORMAT_FLAC || probe.Format == AUDIO_FORMAT_MPEG)
	{
		return CreateMiniaudioDecoder (source, probe, decoder);
	}
	return AUDIO_DECODE_UNSUPPORTED;
}

AudioDecodeStatus CopyAudioDecoderSource (AudioDataSource &source, std::vector<unsigned char> *data)
{
	std::size_t originalPosition;
	std::size_t length;
	std::size_t offset = 0;
	AudioDecodeStatus result = AUDIO_DECODE_OK;
	if (data == NULL)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	originalPosition = source.Tell ();
	length = source.GetLength ();
	if (originalPosition > length || length > MaxEncodedBytes || source.Seek (AUDIO_SEEK_BEGIN, 0) != AUDIO_SOURCE_OK)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	try
	{
		data->assign (length, 0);
	}
	catch (...)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	while (offset < length)
	{
		std::size_t bytesRead = 0;
		AudioSourceStatus status = source.Read (&(*data)[offset], length - offset, &bytesRead);
		if (bytesRead > length - offset || (status != AUDIO_SOURCE_OK && status != AUDIO_SOURCE_EOF))
		{
			result = AUDIO_DECODE_IO_ERROR;
			break;
		}
		offset += bytesRead;
		if (offset != length && (status == AUDIO_SOURCE_EOF || bytesRead == 0))
		{
			result = AUDIO_DECODE_INVALID_SOURCE;
			break;
		}
	}
	if (source.Seek (AUDIO_SEEK_BEGIN, (long long)originalPosition) != AUDIO_SOURCE_OK)
	{
		data->clear ();
		return AUDIO_DECODE_IO_ERROR;
	}
	if (result != AUDIO_DECODE_OK)
	{
		data->clear ();
	}
	return result;
}

AudioDecodeStatus DecodeAudioToPCM16 (AudioDataSource &source, const AudioProbeResult &probe, AudioDecodedPCM16 *decoded)
{
	AudioDecoder *decoder = NULL;
	AudioDecodeStatus status;
	unsigned long long totalFrames;
	std::size_t maxSamples = MaxPCMBytes / sizeof (short);
	if (decoded == NULL)
	{
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	*decoded = AudioDecodedPCM16 ();
	status = CreateAudioDecoder (source, probe, &decoder);
	if (status != AUDIO_DECODE_OK)
	{
		return status;
	}
	decoded->SampleRate = decoder->GetNativeSampleRate ();
	decoded->Channels = decoder->GetOutputChannels ();
	if (decoded->SampleRate == 0 || decoded->Channels == 0 || decoded->Channels > 2 ||
		(decoder->GetTotalFrames (&totalFrames) && (totalFrames > maxSamples / decoded->Channels)))
	{
		delete decoder;
		*decoded = AudioDecodedPCM16 ();
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	try
	{
		if (decoder->GetTotalFrames (&totalFrames))
		{
			decoded->Samples.reserve ((std::size_t)totalFrames * decoded->Channels);
		}
		for (;;)
		{
			short frames[8192];
			std::size_t framesRead = 0;
			std::size_t capacity = 8192 / decoded->Channels;
			AudioDecoderReadStatus readStatus = decoder->ReadFrames (frames, capacity, &framesRead);
			if (readStatus == AUDIO_DECODER_ERROR || framesRead > capacity ||
				framesRead > (maxSamples - decoded->Samples.size ()) / decoded->Channels)
			{
				delete decoder;
				*decoded = AudioDecodedPCM16 ();
				return AUDIO_DECODE_INVALID_SOURCE;
			}
			decoded->Samples.insert (decoded->Samples.end (), frames, frames + framesRead * decoded->Channels);
			if (readStatus == AUDIO_DECODER_EOF)
			{
				break;
			}
			if (framesRead == 0)
			{
				delete decoder;
				*decoded = AudioDecodedPCM16 ();
				return AUDIO_DECODE_INVALID_SOURCE;
			}
		}
	}
	catch (...)
	{
		delete decoder;
		*decoded = AudioDecodedPCM16 ();
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	if (decoder->GetTotalFrames (&totalFrames) && decoded->Samples.size () / decoded->Channels != totalFrames)
	{
		delete decoder;
		*decoded = AudioDecodedPCM16 ();
		return AUDIO_DECODE_INVALID_SOURCE;
	}
	delete decoder;
	return AUDIO_DECODE_OK;
}