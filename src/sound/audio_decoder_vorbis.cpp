#include "audio_decoder.h"
#include "audio_timetag.h"

extern "C"
{
#define STB_VORBIS_HEADER_ONLY
#include "thirdparty/stb/stb_vorbis.c"
}

#include <limits>
#include <new>
#include <string.h>

namespace
{
	const unsigned int MaxVorbisFrames = 0xfffffffdU;
	const unsigned int VorbisFrameSentinel = 0xfffffffeU;
	const int VorbisInvalidLoopDiagnostic = -1000;
	const char * const VorbisInvalidLoopDiagnosticString = "invalid Vorbis loop tags";

	class VorbisFrameContract
	{
	public:
		VorbisFrameContract (unsigned int totalFrames)
			: TotalFrames (totalFrames)
		{
		}

		bool HasUsableTotalFrames () const
		{
			return TotalFrames <= MaxVorbisFrames;
		}

		bool CanReadFrames (unsigned long long position, unsigned int frames) const
		{
			return HasUsableTotalFrames () && position <= TotalFrames && frames <= TotalFrames - position;
		}

		bool CanSeekFrame (unsigned long long frame) const
		{
			return HasUsableTotalFrames () && frame <= TotalFrames && frame < VorbisFrameSentinel;
		}

		bool IsLogicalEOF (unsigned long long frame) const
		{
			return CanSeekFrame (frame) && frame == TotalFrames;
		}

		bool CanUseLoopEndpoint (unsigned long long endpoint) const
		{
			return CanSeekFrame (endpoint);
		}

	private:
		unsigned int TotalFrames;
	};

	bool SupportsOutputChannels (unsigned int channels)
	{
		return channels >= 1 && channels <= 2;
	}

	bool HasUsableTotalFrames (unsigned int frames)
	{
		return VorbisFrameContract (frames).HasUsableTotalFrames ();
	}

	bool ConvertTimeTagToFrames (const AudioTimeTag &tag, unsigned int sampleRate, unsigned long long *frames)
	{
		if (frames == NULL)
		{
			return false;
		}
		if (tag.AsSamples)
		{
			*frames = tag.Value;
			return true;
		}
		if (sampleRate == 0 || tag.Value > std::numeric_limits<unsigned long long>::max () / sampleRate)
		{
			return false;
		}
		*frames = tag.Value * sampleRate / 1000;
		return true;
	}

	enum VorbisLoopTag
	{
		VORBIS_LOOP_TAG_NONE,
		VORBIS_LOOP_TAG_START,
		VORBIS_LOOP_TAG_END
	};

	VorbisLoopTag GetVorbisLoopTag (const char *comment, const char **value)
	{
		if (value == NULL)
		{
			return VORBIS_LOOP_TAG_NONE;
		}
		*value = NULL;
		if (comment == NULL)
		{
			return VORBIS_LOOP_TAG_NONE;
		}
		if (strncmp (comment, "LOOP_START=", 11) == 0 || strncmp (comment, "LOOPSTART=", 10) == 0)
		{
			*value = comment + (comment[4] == '_' ? 11 : 10);
			return VORBIS_LOOP_TAG_START;
		}
		if (strncmp (comment, "LOOP_END=", 9) == 0 || strncmp (comment, "LOOPEND=", 8) == 0)
		{
			*value = comment + (comment[4] == '_' ? 9 : 8);
			return VORBIS_LOOP_TAG_END;
		}
		return VORBIS_LOOP_TAG_NONE;
	}

	bool BuildVorbisLoopRange (const AudioTimeTag &start, bool hasStart, const AudioTimeTag &end, bool hasEnd, bool totalFramesKnown, unsigned long long totalFrames, unsigned int sampleRate, AudioFrameRange *range)
	{
		if (!totalFramesKnown || range == NULL || sampleRate == 0)
		{
			return false;
		}
		if (hasStart && !ConvertTimeTagToFrames (start, sampleRate, &range->Start))
		{
			return false;
		}
		if (hasEnd && !ConvertTimeTagToFrames (end, sampleRate, &range->End))
		{
			return false;
		}
		if (!hasStart)
		{
			range->Start = 0;
		}
		if (!hasEnd)
		{
			range->End = totalFrames;
		}
		return range->Start < range->End && VorbisFrameContract ((unsigned int)totalFrames).CanUseLoopEndpoint (range->End);
	}

	bool ParseVorbisLoopComment (const char *comment, AudioTimeTag *start, bool *hasStart, AudioTimeTag *end, bool *hasEnd)
	{
		const char *value = NULL;
		VorbisLoopTag tag = GetVorbisLoopTag (comment, &value);
		if (tag == VORBIS_LOOP_TAG_NONE)
		{
			return true;
		}
		if (!ParseAudioTimeTag (value, tag == VORBIS_LOOP_TAG_START ? start : end))
		{
			return false;
		}
		if (tag == VORBIS_LOOP_TAG_START)
		{
			*hasStart = true;
		}
		else
		{
			*hasEnd = true;
		}
		return true;
	}

	bool ParseVorbisLoopComments (const char *const *comments, int commentCount, bool totalFramesKnown, unsigned long long totalFrames, unsigned int sampleRate, AudioFrameRange *range, int *diagnosticCode)
	{
		AudioTimeTag start;
		AudioTimeTag end;
		bool hasStart = false;
		bool hasEnd = false;
		bool invalid = commentCount < 0 || (comments == NULL && commentCount != 0);
		if (diagnosticCode != NULL)
		{
			*diagnosticCode = 0;
		}
		for (int index = 0; !invalid && index < commentCount; ++index)
		{
			if (!ParseVorbisLoopComment (comments[index], &start, &hasStart, &end, &hasEnd))
			{
				invalid = true;
			}
		}
		if (!hasStart && !hasEnd && !invalid)
		{
			return false;
		}
		invalid = invalid || !BuildVorbisLoopRange (start, hasStart, end, hasEnd, totalFramesKnown, totalFrames, sampleRate, range);
		if (invalid && diagnosticCode != NULL)
		{
			*diagnosticCode = VorbisInvalidLoopDiagnostic;
		}
		return !invalid;
	}

	class VorbisDecoder : public AudioDecoder
	{
	public:
		VorbisDecoder (const std::vector<unsigned char> &data)
			: Data (data), Decoder (NULL), TotalFrames (0), Position (0), AtLogicalEOF (false), HasLoop (false), DiagnosticCode (0)
		{
			int error = 0;
			if (Data.size () > (std::size_t)std::numeric_limits<int>::max ())
			{
				DiagnosticCode = VORBIS_file_open_failure;
				return;
			}
			Decoder = stb_vorbis_open_memory (&Data[0], (int)Data.size (), &error, NULL);
			DiagnosticCode = error;
			if (Decoder != NULL)
			{
				stb_vorbis_info info = stb_vorbis_get_info (Decoder);
				if (!SupportsOutputChannels ((unsigned int)info.channels) || info.sample_rate == 0)
				{
					stb_vorbis_close (Decoder);
					Decoder = NULL;
					DiagnosticCode = VORBIS_invalid_stream;
					return;
				}
				Metadata.Channels = (unsigned int)info.channels;
				Metadata.SampleRate = info.sample_rate;
				Metadata.BitsPerSample = 16;
				Metadata.FloatingPoint = false;
				TotalFrames = stb_vorbis_stream_length_in_samples (Decoder);
				if (!HasUsableTotalFrames (TotalFrames))
				{
					stb_vorbis_close (Decoder);
					Decoder = NULL;
					DiagnosticCode = VORBIS_invalid_stream;
					return;
				}
				ParseLoopComments ();
			}
		}

		~VorbisDecoder ()
		{
			if (Decoder != NULL)
			{
				stb_vorbis_close (Decoder);
			}
		}

		bool IsValid () const { return Decoder != NULL; }
		AudioFormat GetEncodedFormat () const { return AUDIO_FORMAT_OGG_VORBIS; }
		const AudioPCMMetadata &GetMetadata () const { return Metadata; }
		unsigned int GetNativeSampleRate () const { return Metadata.SampleRate; }
		unsigned int GetOutputChannels () const { return Metadata.Channels; }
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
			if (!HasLoop || range == NULL)
			{
				return false;
			}
			*range = Loop;
			return true;
		}
		int GetDiagnosticCode () const { return DiagnosticCode; }
		const char *GetDiagnosticString () const { return DiagnosticCode == VorbisInvalidLoopDiagnostic ? VorbisInvalidLoopDiagnosticString : "stb_vorbis"; }

		AudioDecoderReadStatus ReadFrames (short *frames, std::size_t frameCount, std::size_t *framesRead)
		{
			std::size_t maximum = (std::size_t)std::numeric_limits<int>::max () / Metadata.Channels;
			VorbisFrameContract frameContract (TotalFrames);
			int read;
			if (framesRead == NULL || (frames == NULL && frameCount != 0) || frameCount > maximum)
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
			read = stb_vorbis_get_samples_short_interleaved (Decoder, (int)Metadata.Channels, frames, (int)(frameCount * Metadata.Channels));
			if (read < 0 || (unsigned int)read > frameCount || !frameContract.CanReadFrames (Position, (unsigned int)read))
			{
				DiagnosticCode = stb_vorbis_get_error (Decoder);
				return AUDIO_DECODER_ERROR;
			}
			if (read == 0)
			{
				DiagnosticCode = stb_vorbis_get_error (Decoder);
				if (DiagnosticCode == VORBIS__no_error)
				{
					AtLogicalEOF = true;
					return AUDIO_DECODER_EOF;
				}
				return AUDIO_DECODER_ERROR;
			}
			Position += (unsigned int)read;
			*framesRead = (std::size_t)read;
			return AUDIO_DECODER_DATA;
		}

		AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
		{
			bool originalAtLogicalEOF = AtLogicalEOF;
			VorbisFrameContract frameContract (TotalFrames);
			if (!frameContract.CanSeekFrame (frame))
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			if (frameContract.IsLogicalEOF (frame))
			{
				Position = frame;
				AtLogicalEOF = true;
				return AUDIO_DECODER_SEEK_OK;
			}
			if (stb_vorbis_seek (Decoder, (unsigned int)frame))
			{
				Position = frame;
				AtLogicalEOF = false;
				return AUDIO_DECODER_SEEK_OK;
			}
			DiagnosticCode = stb_vorbis_get_error (Decoder);
			if (originalAtLogicalEOF)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			if (stb_vorbis_seek (Decoder, (unsigned int)Position))
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			return AUDIO_DECODER_SEEK_TERMINAL_ERROR;
		}

	private:
		void ParseLoopComments ()
		{
			stb_vorbis_comment comments = stb_vorbis_get_comment (Decoder);
			int loopDiagnostic = 0;
			HasLoop = ParseVorbisLoopComments (comments.comment_list, comments.comment_list_length, true, TotalFrames, Metadata.SampleRate, &Loop, &loopDiagnostic);
			if (loopDiagnostic != 0)
			{
				DiagnosticCode = loopDiagnostic;
			}
		}

		std::vector<unsigned char> Data;
		stb_vorbis *Decoder;
		AudioPCMMetadata Metadata;
		unsigned int TotalFrames;
		unsigned long long Position;
		bool AtLogicalEOF;
		AudioFrameRange Loop;
		bool HasLoop;
		int DiagnosticCode;
	};
}

#ifdef AUDIO_DECODER_TESTING
bool AudioDecoderTestSupportsVorbisChannels (unsigned int channels)
{
	return SupportsOutputChannels (channels);
}

bool AudioDecoderTestSupportsVorbisTotalFrames (unsigned int frames)
{
	return HasUsableTotalFrames (frames);
}

bool AudioDecoderTestVorbisCanReadFrames (unsigned int totalFrames, unsigned long long position, unsigned int frames)
{
	return VorbisFrameContract (totalFrames).CanReadFrames (position, frames);
}

bool AudioDecoderTestVorbisCanSeekFrame (unsigned int totalFrames, unsigned long long frame)
{
	return VorbisFrameContract (totalFrames).CanSeekFrame (frame);
}

bool AudioDecoderTestVorbisIsLogicalEOF (unsigned int totalFrames, unsigned long long frame)
{
	return VorbisFrameContract (totalFrames).IsLogicalEOF (frame);
}

bool AudioDecoderTestVorbisCanUseLoopEndpoint (unsigned int totalFrames, unsigned long long endpoint)
{
	return VorbisFrameContract (totalFrames).CanUseLoopEndpoint (endpoint);
}

bool AudioDecoderTestParseVorbisLoopComments (const char *const *comments, int commentCount, bool totalFramesKnown, unsigned long long totalFrames, unsigned int sampleRate, AudioFrameRange *range, int *diagnosticCode)
{
	return ParseVorbisLoopComments (comments, commentCount, totalFramesKnown, totalFrames, sampleRate, range, diagnosticCode);
}

const char *AudioDecoderTestVorbisLoopDiagnosticString ()
{
	return VorbisInvalidLoopDiagnosticString;
}
#endif

AudioDecodeStatus CreateVorbisDecoder (AudioDataSource &source, const AudioProbeResult &, AudioDecoder **decoder)
{
	std::vector<unsigned char> data;
	AudioDecodeStatus status;
	VorbisDecoder *created;
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
	created = new (std::nothrow) VorbisDecoder (data);
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