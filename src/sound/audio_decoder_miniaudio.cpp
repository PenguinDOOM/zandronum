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