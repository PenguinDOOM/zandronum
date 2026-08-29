#include "oaldata.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

namespace
{
	unsigned int ReadLE16 (const unsigned char *data)
	{
		return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
	}

	unsigned int ReadLE32 (const unsigned char *data)
	{
		return (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
			((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
	}

	bool HasBytes (std::size_t offset, std::size_t count, std::size_t length)
	{
		return offset <= length && count <= length - offset;
	}

	bool IsWaveHeader (const unsigned char *data, std::size_t length)
	{
		return data != 0 && length >= 12 && std::memcmp (data, "RIFF", 4) == 0 && std::memcmp (data + 8, "WAVE", 4) == 0;
	}

	bool IsPCMSubtype (const unsigned char *format)
	{
		static const unsigned char pcmGuid[16] =
		{
			1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71
		};
		for (int i = 0; i < 16; ++i)
		{
			if (format[i] != pcmGuid[i])
			{
				return false;
			}
		}
		return true;
	}

	OALPCMError ValidateFormat (unsigned int sampleRate, unsigned int channels, int bits, std::size_t length, std::size_t *frameBytes)
	{
		unsigned int bitsPerSample = bits < 0 ? 0u - (unsigned int)bits : (unsigned int)bits;
		if (sampleRate == 0 || (channels != 1 && channels != 2) || (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 32))
		{
			return OALPCM_UNSUPPORTED_FORMAT;
		}
		*frameBytes = channels * (bitsPerSample / 8);
		if (length == 0 || length % *frameBytes != 0)
		{
			return OALPCM_MALFORMED_DATA;
		}
		return OALPCM_OK;
	}

	OALPCMError ValidateLoop (unsigned int frames, int loopStart, int loopEnd, OALPCMData *output)
	{
		output->HasLoop = false;
		output->LoopStart = 0;
		output->LoopEnd = 0;
		if (loopStart < 0 && loopEnd < 0)
		{
			return OALPCM_OK;
		}
		if (loopStart >= 0 && loopEnd == -1)
		{
			loopEnd = (int)frames;
		}
		if (loopStart < 0 || loopEnd < 0)
		{
			return OALPCM_MALFORMED_DATA;
		}
		if ((unsigned int)loopStart >= frames || (unsigned int)loopEnd > frames || loopStart >= loopEnd)
		{
			return OALPCM_MALFORMED_DATA;
		}
		output->HasLoop = true;
		output->LoopStart = (unsigned int)loopStart;
		output->LoopEnd = (unsigned int)loopEnd;
		return OALPCM_OK;
	}

	short ClampToShort (int value)
	{
		if (value < -32768)
		{
			return -32768;
		}
		if (value > 32767)
		{
			return 32767;
		}
		return (short)value;
	}

	struct WaveFormat
	{
		unsigned int Tag;
		unsigned int Channels;
		unsigned int SampleRate;
		unsigned int BlockAlign;
		unsigned int Bits;
	};

	OALPCMError ParseWaveFormat (const unsigned char *data, std::size_t length, WaveFormat *format)
	{
		unsigned int extensionSize;
		if (length < 16)
		{
			return OALPCM_MALFORMED_DATA;
		}
		format->Tag = ReadLE16 (data);
		format->Channels = ReadLE16 (data + 2);
		format->SampleRate = ReadLE32 (data + 4);
		format->BlockAlign = ReadLE16 (data + 12);
		format->Bits = ReadLE16 (data + 14);
		if (format->Tag != 0xfffe)
		{
			return format->Tag == 1 ? OALPCM_OK : OALPCM_UNSUPPORTED_FORMAT;
		}
		if (length < 18)
		{
			return OALPCM_MALFORMED_DATA;
		}
		extensionSize = ReadLE16 (data + 16);
		if ((std::size_t)extensionSize > length - 18)
		{
			return OALPCM_MALFORMED_DATA;
		}
		if (extensionSize < 22 || !IsPCMSubtype (data + 24))
		{
			return OALPCM_UNSUPPORTED_FORMAT;
		}
		format->Tag = 1;
		return OALPCM_OK;
	}

	OALPCMError ParseWaveLoop (const unsigned char *data, std::size_t length, int *loopStart, int *loopEnd)
	{
		const unsigned int maximumLoopValue = (unsigned int)std::numeric_limits<int>::max ();
		unsigned int start;
		unsigned int end;
		if (length < 36)
		{
			return OALPCM_MALFORMED_DATA;
		}
		if (ReadLE32 (data + 28) == 0)
		{
			return OALPCM_OK;
		}
		if (length < 60)
		{
			return OALPCM_MALFORMED_DATA;
		}
		start = ReadLE32 (data + 44);
		end = ReadLE32 (data + 48);
		if (start > maximumLoopValue || end >= maximumLoopValue)
		{
			return OALPCM_MALFORMED_DATA;
		}
		*loopStart = (int)start;
		*loopEnd = (int)end + 1;
		return OALPCM_OK;
	}

	OALPCMError AdvanceWaveChunk (const unsigned char *data, std::size_t length, std::size_t offset, std::size_t *chunkData, std::size_t *chunkLength, std::size_t *next)
	{
		unsigned int size;
		if (!HasBytes (offset, 8, length))
		{
			return OALPCM_MALFORMED_DATA;
		}
		size = ReadLE32 (data + offset + 4);
		*chunkData = offset + 8;
		*chunkLength = size;
		if (!HasBytes (*chunkData, *chunkLength, length))
		{
			return OALPCM_MALFORMED_DATA;
		}
		*next = *chunkData + *chunkLength;
		if (size & 1)
		{
			if (!HasBytes (*next, 1, length))
			{
				return OALPCM_MALFORMED_DATA;
			}
			++*next;
		}
		return OALPCM_OK;
	}
}

OALPCMData::OALPCMData ()
	: SampleRate (0), Channels (0), Frames (0), HasLoop (false), LoopStart (0), LoopEnd (0)
{
}

OALPCMResult::OALPCMResult (OALPCMError error)
	: Error (error)
{
}

bool OALPCMResult::IsValid () const
{
	return Error == OALPCM_OK;
}

OALPCMResult OALConvertRawPCM (const unsigned char *data, std::size_t length, unsigned int sampleRate, unsigned int channels, int bits, int loopStart, int loopEnd)
{
	std::size_t frameBytes;
	OALPCMError error;
	OALPCMResult result;
	unsigned int bitsPerSample;

	if (data == 0)
	{
		return OALPCMResult (OALPCM_INVALID_ARGUMENT);
	}
	error = ValidateFormat (sampleRate, channels, bits, length, &frameBytes);
	if (error != OALPCM_OK)
	{
		return OALPCMResult (error);
	}
	if (length / frameBytes > std::numeric_limits<unsigned int>::max ())
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}

	result.Data.SampleRate = sampleRate;
	result.Data.Channels = channels;
	result.Data.Frames = (unsigned int)(length / frameBytes);
	error = ValidateLoop (result.Data.Frames, loopStart, loopEnd, &result.Data);
	if (error != OALPCM_OK)
	{
		return OALPCMResult (error);
	}

	bitsPerSample = bits < 0 ? 0u - (unsigned int)bits : (unsigned int)bits;
	try
	{
		result.Data.Samples.resize ((std::size_t)result.Data.Frames * channels);
	}
	catch (const std::bad_alloc &)
	{
		return OALPCMResult (OALPCM_OUT_OF_MEMORY);
	}

	for (std::size_t sample = 0, offset = 0; sample < result.Data.Samples.size (); ++sample, offset += bitsPerSample / 8)
	{
		if (bitsPerSample == 8)
		{
			int value = bits == 8 ? (int)data[offset] - 128 :
				(data[offset] < 0x80 ? (int)data[offset] : (int)data[offset] - 256);
			result.Data.Samples[sample] = (short)(value * 256);
		}
		else if (bitsPerSample == 16)
		{
			int value = (int)data[offset] | ((int)(data[offset + 1] & 0x7f) << 8);
			result.Data.Samples[sample] = (short)(data[offset + 1] & 0x80 ? value - 0x8000 : value);
		}
		else
		{
			int value = (int)data[offset + 2] | ((int)(data[offset + 3] & 0x7f) << 8);
			result.Data.Samples[sample] = (short)(data[offset + 3] & 0x80 ? value - 0x8000 : value);
		}
	}
	return result;
}

OALPCMResult OALDownmixToMono (const OALPCMData &source)
{
	OALPCMResult result;
	if (source.Channels != 1 && source.Channels != 2)
	{
		return OALPCMResult (OALPCM_INVALID_ARGUMENT);
	}
	if (source.Frames == 0 || source.Samples.size () != (std::size_t)source.Frames * source.Channels)
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}
	result.Data = source;
	result.Data.Channels = 1;
	if (source.Channels == 1)
	{
		return result;
	}
	try
	{
		result.Data.Samples.resize (source.Frames);
	}
	catch (const std::bad_alloc &)
	{
		return OALPCMResult (OALPCM_OUT_OF_MEMORY);
	}
	for (unsigned int frame = 0; frame < source.Frames; ++frame)
	{
		int left = source.Samples[(std::size_t)frame * 2];
		int right = source.Samples[(std::size_t)frame * 2 + 1];
		result.Data.Samples[frame] = ClampToShort ((left + right) / 2);
	}
	return result;
}

OALPCMResult OALParseWavePCM (const unsigned char *data, std::size_t length)
{
	WaveFormat format = { 0, 0, 0, 0, 0 };
	const unsigned char *sampleData = 0;
	std::size_t sampleLength = 0;
	bool foundFormat = false;
	bool foundData = false;
	bool foundLoop = false;
	int loopStart = -1;
	int loopEnd = -1;
	std::size_t offset = 12;

	if (!IsWaveHeader (data, length))
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}
	if ((std::size_t)ReadLE32 (data + 4) != length - 8)
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}

	while (offset < length)
	{
		std::size_t chunkData;
		std::size_t chunkLength;
		std::size_t next;
		OALPCMError error = AdvanceWaveChunk (data, length, offset, &chunkData, &chunkLength, &next);
		if (error != OALPCM_OK)
		{
			return OALPCMResult (error);
		}
		if (std::memcmp (data + offset, "fmt ", 4) == 0)
		{
			if (foundFormat)
			{
				return OALPCMResult (OALPCM_MALFORMED_DATA);
			}
			error = ParseWaveFormat (data + chunkData, chunkLength, &format);
			if (error != OALPCM_OK)
			{
				return OALPCMResult (error);
			}
			foundFormat = true;
		}
		else if (std::memcmp (data + offset, "data", 4) == 0)
		{
			if (foundData)
			{
				return OALPCMResult (OALPCM_MALFORMED_DATA);
			}
			sampleData = data + chunkData;
			sampleLength = chunkLength;
			foundData = true;
		}
		else if (std::memcmp (data + offset, "smpl", 4) == 0 && !foundLoop)
		{
			error = ParseWaveLoop (data + chunkData, chunkLength, &loopStart, &loopEnd);
			if (error != OALPCM_OK)
			{
				return OALPCMResult (error);
			}
			foundLoop = true;
		}
		offset = next;
	}

	if (!foundFormat || !foundData)
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}
	if (format.Channels == 0 || format.Bits == 0 || format.BlockAlign != format.Channels * (format.Bits / 8))
	{
		return OALPCMResult (OALPCM_MALFORMED_DATA);
	}
	return OALConvertRawPCM (sampleData, sampleLength, format.SampleRate, format.Channels, (int)format.Bits, loopStart, loopEnd);
}