#include "oaldata.h"

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

	void SetLE32 (std::vector<unsigned char> &bytes, size_t offset, unsigned int value)
	{
		bytes[offset] = (unsigned char)value;
		bytes[offset + 1] = (unsigned char)(value >> 8);
		bytes[offset + 2] = (unsigned char)(value >> 16);
		bytes[offset + 3] = (unsigned char)(value >> 24);
	}

	void SetLE16 (std::vector<unsigned char> &bytes, size_t offset, unsigned int value)
	{
		bytes[offset] = (unsigned char)value;
		bytes[offset + 1] = (unsigned char)(value >> 8);
	}

	void AppendChunk (std::vector<unsigned char> &bytes, const char *name, const std::vector<unsigned char> &contents)
	{
		bytes.insert (bytes.end (), name, name + 4);
		AppendLE32 (bytes, (unsigned int)contents.size ());
		bytes.insert (bytes.end (), contents.begin (), contents.end ());
		if (contents.size () & 1)
		{
			bytes.push_back (0);
		}
	}

	std::vector<unsigned char> MakeWave (const std::vector<unsigned char> &samples, unsigned int channels, unsigned int bits)
	{
		std::vector<unsigned char> format;
		std::vector<unsigned char> wave;
		AppendLE16 (format, 1);
		AppendLE16 (format, channels);
		AppendLE32 (format, 11025);
		AppendLE32 (format, 11025 * channels * bits / 8);
		AppendLE16 (format, channels * bits / 8);
		AppendLE16 (format, bits);
		wave.insert (wave.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (wave, 0);
		wave.insert (wave.end (), "WAVE", "WAVE" + 4);
		AppendChunk (wave, "fmt ", format);
		AppendChunk (wave, "data", samples);
		unsigned int size = (unsigned int)wave.size () - 8;
		wave[4] = (unsigned char)size;
		wave[5] = (unsigned char)(size >> 8);
		wave[6] = (unsigned char)(size >> 16);
		wave[7] = (unsigned char)(size >> 24);
		return wave;
	}

	std::vector<unsigned char> MakeExtensibleWave (const std::vector<unsigned char> &samples, bool pcmSubtype)
	{
		static const unsigned char pcmGuid[16] =
		{
			1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71
		};
		std::vector<unsigned char> format;
		std::vector<unsigned char> wave;
		AppendLE16 (format, 0xfffe);
		AppendLE16 (format, 1);
		AppendLE32 (format, 11025);
		AppendLE32 (format, 11025);
		AppendLE16 (format, 1);
		AppendLE16 (format, 8);
		AppendLE16 (format, 22);
		AppendLE16 (format, 8);
		AppendLE32 (format, 0);
		format.insert (format.end (), pcmGuid, pcmGuid + 16);
		if (!pcmSubtype)
		{
			format[24] = 3;
		}
		wave.insert (wave.end (), "RIFF", "RIFF" + 4);
		AppendLE32 (wave, 0);
		wave.insert (wave.end (), "WAVE", "WAVE" + 4);
		AppendChunk (wave, "fmt ", format);
		AppendChunk (wave, "data", samples);
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		return wave;
	}

	void TestUnsigned8Conversion ()
	{
		const unsigned char unsigned8[] = { 0, 128, 255 };
		OALPCMResult result = OALConvertRawPCM (unsigned8, sizeof (unsigned8), 11025, 1, 8, -1, -1);
		Check (result.IsValid () && result.Data.Samples[0] == -32768 && result.Data.Samples[1] == 0 && result.Data.Samples[2] == 32512, "unsigned 8-bit conversion");
	}

	void TestSigned8Conversion ()
	{
		const unsigned char signed8[] = { 0x80, 0xff, 0, 0x7f };
		OALPCMResult result;
		result = OALConvertRawPCM (signed8, sizeof (signed8), 11025, 1, -8, -1, -1);
		Check (result.IsValid () && result.Data.Samples.size () == 4 &&
			result.Data.Samples[0] == -32768 && result.Data.Samples[1] == -256 &&
			result.Data.Samples[2] == 0 && result.Data.Samples[3] == 32512, "signed 8-bit conversion boundaries");
	}

	void TestSigned16Conversion ()
	{
		const unsigned char signed16[] = { 0, 0x80, 0xff, 0xff, 0, 0, 0xff, 0x7f };
		OALPCMResult result;
		result = OALConvertRawPCM (signed16, sizeof (signed16), 11025, 1, -16, -1, -1);
		Check (result.IsValid () && result.Data.Samples.size () == 4 &&
			result.Data.Samples[0] == -32768 && result.Data.Samples[1] == -1 &&
			result.Data.Samples[2] == 0 && result.Data.Samples[3] == 32767, "signed 16-bit VOC conversion boundaries");
	}

	void TestSigned32Conversion ()
	{
		const unsigned char signed32[] =
		{
			0, 0, 0, 0x80, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 1, 0, 0xff, 0xff, 0xff, 0x7f
		};
		OALPCMResult result;
		result = OALConvertRawPCM (signed32, sizeof (signed32), 11025, 1, 32, -1, -1);
		Check (result.IsValid () && result.Data.Samples.size () == 7 &&
			result.Data.Samples[0] == -32768 && result.Data.Samples[1] == -1 &&
			result.Data.Samples[2] == -1 && result.Data.Samples[3] == 0 &&
			result.Data.Samples[4] == 0 && result.Data.Samples[5] == 1 &&
			result.Data.Samples[6] == 32767, "signed 32-bit conversion boundaries");
	}

	void TestWaveParsing ()
	{
		std::vector<unsigned char> samples;
		samples.push_back (0);
		samples.push_back (128);
		std::vector<unsigned char> wave = MakeWave (samples, 1, 8);
		OALPCMResult result = OALParseWavePCM (&wave[0], wave.size ());
		Check (result.IsValid () && result.Data.Frames == 2 && result.Data.SampleRate == 11025, "valid PCM WAVE");
		wave.resize (wave.size () - 1);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "truncated WAVE");
		wave = MakeWave (samples, 1, 8);
		wave[16] = 3;
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "unsupported WAVE format");
		wave = MakeWave (samples, 1, 8);
		wave[4] = 0xff;
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "RIFF size mismatch");
		wave = MakeWave (samples, 1, 8);
		wave[22] = 3;
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "unsupported channel count");
		wave = MakeWave (samples, 1, 8);
		wave[34] = 24;
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "unsupported bit depth");
		wave = MakeWave (samples, 1, 16);
		wave.push_back (0);
		SetLE32 (wave, 40, 3);
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "data frame mismatch");
	}

	void TestWaveChunkContracts ()
	{
		std::vector<unsigned char> samples;
		std::vector<unsigned char> oddChunk;
		std::vector<unsigned char> wave;
		samples.push_back (0);
		samples.push_back (128);
		oddChunk.push_back (0x42);
		wave = MakeWave (samples, 1, 8);
		std::vector<unsigned char> chunk;
		AppendChunk (chunk, "JUNK", oddChunk);
		wave.insert (wave.begin () + 12, chunk.begin (), chunk.end ());
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		Check (OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "complete odd-length unknown chunk accepted");
		wave = MakeWave (samples, 1, 8);
		SetLE32 (wave, 16, 0xffffffff);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "chunk-size overflow rejected");
		wave = MakeWave (samples, 1, 8);
		std::vector<unsigned char> dataChunk;
		AppendChunk (dataChunk, "data", samples);
		wave.insert (wave.end (), dataChunk.begin (), dataChunk.end ());
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "duplicate data chunk rejected");
		wave = MakeWave (samples, 1, 8);
		std::vector<unsigned char> formatChunk;
		formatChunk.insert (formatChunk.end (), wave.begin () + 12, wave.begin () + 36);
		wave.insert (wave.end (), formatChunk.begin (), formatChunk.end ());
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "duplicate format chunk rejected");
	}

	void TestExtensibleWaveParsing ()
	{
		std::vector<unsigned char> samples;
		std::vector<unsigned char> wave;
		samples.push_back (0);
		samples.push_back (128);
		wave = MakeExtensibleWave (samples, true);
		Check (OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "valid PCM extensible WAVE");
		wave = MakeExtensibleWave (samples, false);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "non-PCM extensible subtype rejected");
		wave = MakeExtensibleWave (samples, true);
		SetLE16 (wave, 36, 23);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "oversized extensible cbSize rejected");
	}

	void TestWaveLoop ()
	{
		std::vector<unsigned char> samples;
		std::vector<unsigned char> smpl (60, 0);
		std::vector<unsigned char> wave;
		samples.push_back (0);
		samples.push_back (128);
		samples.push_back (255);
		wave = MakeWave (samples, 1, 8);
		SetLE32 (smpl, 28, 1);
		SetLE32 (smpl, 44, 1);
		SetLE32 (smpl, 48, 2);
		wave.insert (wave.begin () + 36, "smpl", "smpl" + 4);
		std::vector<unsigned char> size;
		AppendLE32 (size, (unsigned int)smpl.size ());
		wave.insert (wave.begin () + 40, size.begin (), size.end ());
		wave.insert (wave.begin () + 44, smpl.begin (), smpl.end ());
		SetLE32 (wave, 4, (unsigned int)wave.size () - 8);
		OALPCMResult result = OALParseWavePCM (&wave[0], wave.size ());
		Check (result.IsValid () && result.Data.HasLoop && result.Data.LoopStart == 1 && result.Data.LoopEnd == 3, "first smpl loop is half-open");
		SetLE32 (wave, 44 + 48, 0xffffffff);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl loop overflow rejected");
		SetLE32 (wave, 44 + 44, (unsigned int)std::numeric_limits<int>::max ());
		SetLE32 (wave, 44 + 48, 2);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl INT_MAX start rejected by loop bounds");
		SetLE32 (wave, 44 + 44, (unsigned int)std::numeric_limits<int>::max () + 1u);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl INT_MAX plus one start rejected before narrowing");
		SetLE32 (wave, 44 + 44, std::numeric_limits<unsigned int>::max ());
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl UINT_MAX start rejected before narrowing");
		SetLE32 (wave, 44 + 44, 1);
		SetLE32 (wave, 44 + 48, (unsigned int)std::numeric_limits<int>::max () - 1u);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl INT_MAX minus one end rejected by loop bounds");
		SetLE32 (wave, 44 + 48, (unsigned int)std::numeric_limits<int>::max ());
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl INT_MAX end rejected before end increment");
		SetLE32 (wave, 44 + 48, (unsigned int)std::numeric_limits<int>::max () + 1u);
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl INT_MAX plus one end rejected before end increment");
		SetLE32 (wave, 44 + 48, std::numeric_limits<unsigned int>::max ());
		Check (!OALParseWavePCM (&wave[0], wave.size ()).IsValid (), "smpl UINT_MAX end rejected before end increment");
	}

	void TestLoopsAndDownmix ()
	{
		const unsigned char stereo[] = { 0, 0x80, 0xff, 0x7f, 0xff, 0x7f, 0, 0x80 };
		OALPCMResult result = OALConvertRawPCM (stereo, sizeof (stereo), 11025, 2, -16, 0, 2);
		Check (result.IsValid () && result.Data.HasLoop && result.Data.LoopEnd == 2, "valid half-open loop");
		Check (OALConvertRawPCM (stereo, sizeof (stereo), 11025, 2, -16, 0, -1).IsValid (), "raw loop defaults to sample end");
		Check (!OALConvertRawPCM (stereo, sizeof (stereo), 11025, 2, -16, 2, 2).IsValid (), "reversed loop rejected");
		result = OALDownmixToMono (result.Data);
		Check (result.IsValid () && result.Data.Channels == 1 && result.Data.Samples.size () == 2 && result.Data.Samples[0] == 0, "stereo downmix");
	}
}

int main ()
{
	TestUnsigned8Conversion ();
	TestSigned8Conversion ();
	TestSigned16Conversion ();
	TestSigned32Conversion ();
	TestWaveParsing ();
	TestWaveChunkContracts ();
	TestExtensibleWaveParsing ();
	TestWaveLoop ();
	TestLoopsAndDownmix ();
	return Failures == 0 ? 0 : 1;
}