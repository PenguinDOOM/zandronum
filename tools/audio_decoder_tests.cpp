#include "audio_decoder.h"

#include <limits>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace
{
	int Failures = 0;

	void Check (bool condition, const char *name)
	{
		if (!condition)
		{
			fprintf (stderr, "FAILED: %s\n", name);
			++Failures;
		}
	}

	void AppendLE16 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		bytes.push_back ((unsigned char)value);
		bytes.push_back ((unsigned char)(value >> 8));
	}

	void AppendLE32 (std::vector<unsigned char> &bytes, unsigned int value)
	{
		AppendLE16 (bytes, value & 0xffff);
		AppendLE16 (bytes, value >> 16);
	}

	std::vector<unsigned char> MakeWave (unsigned int format)
	{
		std::vector<unsigned char> bytes;
		bytes.insert (bytes.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (bytes, 28);
		bytes.insert (bytes.end (), "WAVE", "WAVE" + 4);
		bytes.insert (bytes.end (), "fmt ", "fmt " + 4);
		AppendLE32 (bytes, 16);
		AppendLE16 (bytes, format);
		AppendLE16 (bytes, 2);
		AppendLE32 (bytes, 44100);
		AppendLE32 (bytes, 176400);
		AppendLE16 (bytes, 4);
		AppendLE16 (bytes, 16);
		return bytes;
	}

	std::vector<unsigned char> MakeMpeg ()
	{
		const unsigned char header[] = { 0xff, 0xfb, 0x90, 0 };
		std::vector<unsigned char> bytes (417, 0);
		memcpy (&bytes[0], header, sizeof (header));
		return bytes;
	}

	std::vector<unsigned char> MakeId3Mpeg (std::size_t tagLength = 0, bool withFooter = false)
	{
		std::vector<unsigned char> bytes;
		std::vector<unsigned char> tag (tagLength, 0);
		bytes.insert (bytes.end (), "ID3", "ID3" + 3);
		bytes.push_back (4);
		bytes.push_back (0);
		bytes.push_back (withFooter ? 0x10 : 0);
		bytes.push_back ((unsigned char)((tagLength >> 21) & 0x7f));
		bytes.push_back ((unsigned char)((tagLength >> 14) & 0x7f));
		bytes.push_back ((unsigned char)((tagLength >> 7) & 0x7f));
		bytes.push_back ((unsigned char)(tagLength & 0x7f));
		if (tagLength >= 11)
		{
			tag[0] = 'T';
			tag[1] = 'I';
			tag[2] = 'T';
			tag[3] = '2';
			tag[7] = 1;
			tag[10] = 'x';
		}
		bytes.insert (bytes.end (), tag.begin (), tag.end ());
		if (withFooter)
		{
			bytes.insert (bytes.end (), "3DI", "3DI" + 3);
			bytes.push_back (4);
			bytes.push_back (0);
			bytes.push_back (0x10);
			bytes.push_back ((unsigned char)((tagLength >> 21) & 0x7f));
			bytes.push_back ((unsigned char)((tagLength >> 14) & 0x7f));
			bytes.push_back ((unsigned char)((tagLength >> 7) & 0x7f));
			bytes.push_back ((unsigned char)(tagLength & 0x7f));
		}
		std::vector<unsigned char> mpeg = MakeMpeg ();
		bytes.insert (bytes.end (), mpeg.begin (), mpeg.end ());
		return bytes;
	}

	std::vector<unsigned char> MakeOgg (const char *identifier, std::size_t length)
	{
		std::vector<unsigned char> bytes (28, 0);
		bytes[0] = 'O';
		bytes[1] = 'g';
		bytes[2] = 'g';
		bytes[3] = 'S';
		bytes[26] = 1;
		bytes[27] = (unsigned char)length;
		bytes.insert (bytes.end (), identifier, identifier + length);
		return bytes;
	}

	class FailingSource : public AudioDataSource
	{
	public:
		FailingSource (const std::vector<unsigned char> &data, bool failRead, bool failRestore, std::size_t failReadOnCall = 0)
			: Data (data), Position (0), FailRead (failRead), FailRestore (failRestore), FailReadOnCall (failReadOnCall), ReadCalls (0)
		{
		}

		AudioSourceStatus Read (void *data, std::size_t bytes, std::size_t *bytesRead)
		{
			if (bytesRead == NULL || (data == NULL && bytes != 0))
			{
				return AUDIO_SOURCE_INVALID_ARGUMENT;
			}
			*bytesRead = 0;
			++ReadCalls;
			if (FailRead || ReadCalls == FailReadOnCall)
			{
				return AUDIO_SOURCE_IO_ERROR;
			}
			std::size_t available = Data.size () - Position;
			std::size_t count = bytes < available ? bytes : available;
			if (count != 0)
			{
				memcpy (data, &Data[Position], count);
				Position += count;
				*bytesRead = count;
			}
			return count == bytes ? AUDIO_SOURCE_OK : AUDIO_SOURCE_EOF;
		}

		AudioSourceStatus Seek (AudioSeekOrigin origin, long long offset)
		{
			if (FailRestore && origin == AUDIO_SEEK_BEGIN && offset == 0)
			{
				return AUDIO_SOURCE_IO_ERROR;
			}
			if (origin != AUDIO_SEEK_BEGIN || offset < 0 || (unsigned long long)offset > Data.size ())
			{
				return AUDIO_SOURCE_RANGE_ERROR;
			}
			Position = (std::size_t)offset;
			return AUDIO_SOURCE_OK;
		}

		std::size_t Tell () const
		{
			return Position;
		}

		std::size_t GetLength () const
		{
			return Data.size ();
		}

	private:
		std::vector<unsigned char> Data;
		std::size_t Position;
		bool FailRead;
		bool FailRestore;
		std::size_t FailReadOnCall;
		std::size_t ReadCalls;
	};

	class ContractDecoder : public AudioDecoder
	{
	public:
		ContractDecoder () : Position (0), HasData (true) {}

		AudioFormat GetEncodedFormat () const { return AUDIO_FORMAT_MPEG; }
		const AudioPCMMetadata &GetMetadata () const { return Metadata; }
		unsigned int GetNativeSampleRate () const { return 48000; }
		unsigned int GetOutputChannels () const { return 2; }
		bool GetTotalFrames (unsigned long long *frames) const { *frames = 1; return true; }
		unsigned long long TellFrame () const { return Position; }
		bool GetLoopRange (AudioFrameRange *range) const { range->Start = 0; range->End = 1; return true; }
		int GetDiagnosticCode () const { return 17; }
		const char *GetDiagnosticString () const { return "test diagnostic"; }
		AudioDecoderReadStatus ReadFrames (short *frames, std::size_t frameCount, std::size_t *framesRead)
		{
			if (frames == NULL || framesRead == NULL || frameCount == 0)
			{
				return AUDIO_DECODER_ERROR;
			}
			*framesRead = HasData ? 1 : 0;
			HasData = false;
			return *framesRead != 0 ? AUDIO_DECODER_DATA : AUDIO_DECODER_EOF;
		}
		AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
		{
			if (frame > 1)
			{
				return AUDIO_DECODER_SEEK_ERROR;
			}
			Position = frame;
			return AUDIO_DECODER_SEEK_OK;
		}

	private:
		AudioPCMMetadata Metadata;
		unsigned long long Position;
		bool HasData;
	};

	void TestMemoryOwnership ()
	{
		AudioMemorySource source;
		{
			std::vector<unsigned char> input;
			input.push_back (1);
			input.push_back (2);
			input.push_back (3);
			Check (source.Assign (&input[0], input.size ()) == AUDIO_SOURCE_OK, "copied memory assigned");
		}
		unsigned char output[3] = { 0, 0, 0 };
		std::size_t bytesRead = 0;
		Check (source.Read (output, sizeof (output), &bytesRead) == AUDIO_SOURCE_OK && bytesRead == sizeof (output) && output[2] == 3, "copied memory survives caller lifetime");
	}

	void TestFileSourceBoundaries (const char *path)
	{
		AudioFileSource source;
		unsigned char output[8] = { 0, };
		std::size_t bytesRead = 0;
		Check (source.OpenSlice (path, 2, 5) == AUDIO_SOURCE_OK && source.GetLength () == 5, "file slice opens");
		Check (source.Read (output, sizeof (output), &bytesRead) == AUDIO_SOURCE_EOF && bytesRead == 5 && memcmp (output, "23456", 5) == 0, "file slice excludes suffix");
		Check (source.Seek (AUDIO_SEEK_BEGIN, 0) == AUDIO_SOURCE_OK, "seek begin");
		Check (source.Seek (AUDIO_SEEK_CURRENT, 2) == AUDIO_SOURCE_OK && source.Tell () == 2, "seek current forward");
		Check (source.Seek (AUDIO_SEEK_CURRENT, -1) == AUDIO_SOURCE_OK && source.Tell () == 1, "seek current backward");
		Check (source.Seek (AUDIO_SEEK_END, -1) == AUDIO_SOURCE_OK && source.Tell () == 4, "seek end backward");
		Check (source.Read (output, 1, &bytesRead) == AUDIO_SOURCE_OK && bytesRead == 1 && output[0] == '6', "file slice read at end boundary");
		Check (source.Read (output, 1, &bytesRead) == AUDIO_SOURCE_EOF && bytesRead == 0, "file slice EOF");
		Check (source.Seek (AUDIO_SEEK_BEGIN, -1) == AUDIO_SOURCE_RANGE_ERROR, "seek before slice rejected");
		Check (source.Seek (AUDIO_SEEK_END, 1) == AUDIO_SOURCE_RANGE_ERROR, "seek after slice rejected");
		Check (source.Seek (AUDIO_SEEK_BEGIN, std::numeric_limits<long long>::max ()) == AUDIO_SOURCE_RANGE_ERROR, "positive seek overflow rejected");
		Check (source.Seek (AUDIO_SEEK_END, std::numeric_limits<long long>::min ()) == AUDIO_SOURCE_RANGE_ERROR, "negative seek overflow rejected");
	}

	void TestFileFailures (const char *path)
	{
		AudioFileSource source;
		Check (source.Open ("audio_decoder_missing_input.bin") == AUDIO_SOURCE_IO_ERROR, "missing file is non-fatal");
		Check (source.OpenSlice (path, std::numeric_limits<std::size_t>::max (), 1) == AUDIO_SOURCE_RANGE_ERROR, "slice offset overflow rejected");
		Check (source.OpenSlice (path, 0, std::numeric_limits<std::size_t>::max ()) == AUDIO_SOURCE_RANGE_ERROR, "slice length overflow rejected");
	}

	void TestProbeContracts ()
	{
		std::vector<unsigned char> wave = MakeWave (1);
		AudioMemorySource source (&wave[0], wave.size ());
		AudioProbeResult result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_WAVE_PCM && result.PCM.SampleRate == 44100 && result.PCM.Channels == 2, "PCM WAVE probe");
		std::vector<unsigned char> floatWave = MakeWave (3);
		source.Assign (&floatWave[0], floatWave.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_WAVE_FLOAT && result.PCM.FloatingPoint, "float WAVE probe");
		Check (source.Tell () == 0, "probe restores initial position");
		Check (source.Seek (AUDIO_SEEK_BEGIN, 1) == AUDIO_SOURCE_OK, "position restoration setup");
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_UNKNOWN && source.Tell () == 1, "probe restores nonzero position");

		std::vector<unsigned char> vorbis = MakeOgg ("\x01vorbis", 7);
		source.Assign (&vorbis[0], vorbis.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_OGG_VORBIS, "Ogg Vorbis probe");
		std::vector<unsigned char> opus = MakeOgg ("OpusHead", 8);
		source.Assign (&opus[0], opus.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_OGG_OPUS, "Ogg Opus probe");
		const unsigned char flac[] = { 'f', 'L', 'a', 'C' };
		source.Assign (flac, sizeof (flac));
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_FLAC, "FLAC probe");
		std::vector<unsigned char> mpeg = MakeMpeg ();
		source.Assign (&mpeg[0], mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG, "MPEG probe");
		std::vector<unsigned char> mpegHeaderOnly (mpeg.begin (), mpeg.begin () + 4);
		source.Assign (&mpegHeaderOnly[0], mpegHeaderOnly.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "MPEG header without frame remains unknown");
		std::vector<unsigned char> truncatedMpeg (mpeg.begin (), mpeg.end () - 1);
		source.Assign (&truncatedMpeg[0], truncatedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "truncated MPEG frame remains unknown");
		std::vector<unsigned char> reservedMpeg (mpeg);
		reservedMpeg[3] = 2;
		source.Assign (&reservedMpeg[0], reservedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "reserved MPEG emphasis rejected");
		const unsigned char falseMpeg[] = { 0xff, 0xe0, 0, 0 };
		source.Assign (falseMpeg, sizeof (falseMpeg));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "false MPEG sync rejected");
		const unsigned char truncatedRiff[] = { 'R', 'I', 'F', 'F' };
		source.Assign (truncatedRiff, sizeof (truncatedRiff));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated RIFF rejected");
		const unsigned char truncatedOgg[] = { 'O', 'g', 'g', 'S' };
		source.Assign (truncatedOgg, sizeof (truncatedOgg));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated Ogg rejected");
		source.Assign (NULL, 0);
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "empty input remains unknown");

		AudioDecoder *decoder = NULL;
		Check (CreateAudioDecoder (source, result, &decoder) == AUDIO_DECODE_UNSUPPORTED && decoder == NULL, "decoder factory explicitly unsupported");
		FailingSource readFailure (mpeg, true, false);
		Check (ProbeAudioFormat (readFailure).Status == AUDIO_PROBE_IO_ERROR && readFailure.Tell () == 0, "injected read failure restores source");
		FailingSource restoreFailure (mpeg, false, true);
		Check (ProbeAudioFormat (restoreFailure).Status == AUDIO_PROBE_IO_ERROR, "restore failure reported as I/O error");
	}

	void TestId3ProbeContracts ()
	{
		AudioMemorySource source;
		AudioProbeResult result;
		std::vector<unsigned char> id3Mpeg = MakeId3Mpeg ();
		source.Assign (&id3Mpeg[0], id3Mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG, "ID3 plus MPEG probe");
		std::vector<unsigned char> id3TruncatedMpeg (id3Mpeg.begin (), id3Mpeg.end () - 1);
		source.Assign (&id3TruncatedMpeg[0], id3TruncatedMpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "ID3 plus truncated MPEG remains unknown");
		std::vector<unsigned char> largeId3Mpeg = MakeId3Mpeg (8192, true);
		source.Assign (&largeId3Mpeg[0], largeId3Mpeg.size ());
		result = ProbeAudioFormat (source);
		Check (result.Status == AUDIO_PROBE_RECOGNIZED && result.Format == AUDIO_FORMAT_MPEG && source.Tell () == 0, "large ID3 frame plus footer and MPEG probe restores position");
		std::vector<unsigned char> boundedId3Mpeg = MakeId3Mpeg (1024 * 1024, true);
		source.Assign (&boundedId3Mpeg[0], boundedId3Mpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_RECOGNIZED, "maximum ID3 tag with footer probe");
		std::vector<unsigned char> oversizedId3Mpeg = MakeId3Mpeg (1024 * 1024 + 1);
		source.Assign (&oversizedId3Mpeg[0], oversizedId3Mpeg.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED && source.Tell () == 0, "oversized ID3 tag rejected");
		std::vector<unsigned char> truncatedLargeId3 = MakeId3Mpeg (8192);
		truncatedLargeId3.resize (10 + 8191);
		source.Assign (&truncatedLargeId3[0], truncatedLargeId3.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED && source.Tell () == 0, "truncated large ID3 rejected");
		const unsigned char id3Only[] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0 };
		source.Assign (id3Only, sizeof (id3Only));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_UNKNOWN, "ID3 without MPEG remains unknown");
		const unsigned char invalidId3[] = { 'I', 'D', '3', 4, 0, 0, 0x80, 0, 0, 0 };
		source.Assign (invalidId3, sizeof (invalidId3));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "invalid ID3 rejected");
		std::vector<unsigned char> badRevision = MakeId3Mpeg ();
		badRevision[4] = 0xff;
		source.Assign (&badRevision[0], badRevision.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 bad revision plus MPEG rejected");
		std::vector<unsigned char> missingFooter = MakeId3Mpeg (11);
		missingFooter[5] = 0x10;
		source.Assign (&missingFooter[0], missingFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 missing footer plus MPEG rejected");
		std::vector<unsigned char> badFooter = MakeId3Mpeg (11, true);
		badFooter[21] = 'X';
		source.Assign (&badFooter[0], badFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 bad footer identifier rejected");
		std::vector<unsigned char> mismatchedFooter = MakeId3Mpeg (11, true);
		mismatchedFooter[26] = 0;
		source.Assign (&mismatchedFooter[0], mismatchedFooter.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 mismatched footer flags rejected");
		std::vector<unsigned char> mismatchedFooterSize = MakeId3Mpeg (11, true);
		mismatchedFooterSize[29] = 12;
		source.Assign (&mismatchedFooterSize[0], mismatchedFooterSize.size ());
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "ID3 mismatched footer size rejected");
		std::vector<unsigned char> prefixedId3Mpeg (1, 0);
		prefixedId3Mpeg.insert (prefixedId3Mpeg.end (), largeId3Mpeg.begin (), largeId3Mpeg.end ());
		FailingSource extendedReadFailure (prefixedId3Mpeg, false, false, 2);
		Check (extendedReadFailure.Seek (AUDIO_SEEK_BEGIN, 1) == AUDIO_SOURCE_OK, "extended read nonzero position setup");
		Check (ProbeAudioFormat (extendedReadFailure).Status == AUDIO_PROBE_IO_ERROR && extendedReadFailure.Tell () == 1, "extended ID3 read failure restores nonzero position");
		const unsigned char truncatedId3[] = { 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 1 };
		source.Assign (truncatedId3, sizeof (truncatedId3));
		Check (ProbeAudioFormat (source).Status == AUDIO_PROBE_MALFORMED, "truncated ID3 rejected");
	}

	void TestDecoderContract ()
	{
		ContractDecoder decoder;
		AudioFrameRange loop;
		unsigned long long totalFrames = 0;
		short frames[2] = { 0, 0 };
		std::size_t framesRead = 0;
		Check (decoder.GetEncodedFormat () == AUDIO_FORMAT_MPEG && decoder.GetNativeSampleRate () == 48000 && decoder.GetOutputChannels () == 2, "decoder encoded and native format contract");
		Check (decoder.GetTotalFrames (&totalFrames) && totalFrames == 1 && decoder.GetLoopRange (&loop) && loop.Start == 0 && loop.End == 1, "decoder optional frame contracts");
		Check (decoder.GetDiagnosticCode () == 17 && strcmp (decoder.GetDiagnosticString (), "test diagnostic") == 0, "decoder diagnostic contract");
		Check (decoder.ReadFrames (frames, 2, &framesRead) == AUDIO_DECODER_DATA && framesRead == 1, "decoder partial final read contract");
		Check (decoder.ReadFrames (frames, 2, &framesRead) == AUDIO_DECODER_EOF && framesRead == 0, "decoder clean EOF contract");
		Check (decoder.SeekFrame (1) == AUDIO_DECODER_SEEK_OK && decoder.TellFrame () == 1, "decoder seek contract");
		Check (decoder.SeekFrame (2) == AUDIO_DECODER_SEEK_ERROR && decoder.TellFrame () == 1, "decoder failed seek preserves position");
	}
}

int main ()
{
	const char *path = "audio_decoder_tests_input.bin";
	FILE *file = fopen (path, "wb");
	if (file == NULL)
	{
		return 1;
	}
	fwrite ("0123456789", 1, 10, file);
	fclose (file);

	TestMemoryOwnership ();
	TestFileSourceBoundaries (path);
	TestFileFailures (path);
	TestProbeContracts ();
	TestId3ProbeContracts ();
	TestDecoderContract ();
	remove (path);
	return Failures == 0 ? 0 : 1;
}