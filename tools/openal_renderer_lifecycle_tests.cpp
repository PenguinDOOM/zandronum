#include <vector>

#define private public
#include "oalsound.h"
#undef private

#include "audio_decoder.h"

#include <AL/al.h>
#include <chrono>
#include <limits>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <thread>

OALTestIntCVar snd_channels (1);
OALTestStringCVar snd_openal_device ("default");
OALTestFloatCVar snd_sfxvolume (1.f);
OALTestBoolCVar snd_pitched (true);
OALTestBoolCVar snd_hrtf (false);
extern bool OALTestForceFloatPCM16Fallback;

namespace
{
	unsigned int TestMilliseconds = 0;
}

unsigned int OALTestMilliseconds ()
{
	return TestMilliseconds;
}

namespace
{
	struct CallbackEvent
	{
		CallbackEvent (OpenALEndReason reason, bool attached, bool finalizing)
			: Reason (reason), SawAttached (attached), SawFinalizing (finalizing)
		{
		}

		OpenALEndReason Reason;
		bool SawAttached;
		bool SawFinalizing;
	};

	std::vector<CallbackEvent> Events;
	int Failures = 0;
	FISoundChannel *ForcedNextOwner = NULL;
	const int OALTestStreamMono = 1;
	const int OALTestStreamBits8 = 2;
	const int OALTestStreamBits32 = 4;
	const int OALTestStreamFloat = 8;

	void Check (bool condition, const char *name)
	{
		if (!condition)
		{
			fprintf (stderr, "FAILED: %s\n", name);
			++Failures;
		}
	}

	std::vector<BYTE> MakeSamples (unsigned int frames)
	{
		return std::vector<BYTE> ((size_t)frames * 2, 0);
	}

	std::vector<BYTE> MakeStereoSamples (unsigned int frames)
	{
		return std::vector<BYTE> ((size_t)frames * 4, 0);
	}

	std::vector<BYTE> MakeManualNoiseSamples (unsigned int frames)
	{
		std::vector<BYTE> samples ((size_t)frames * 2);
		unsigned int state = 0x4f1bbcd3u;
		for (unsigned int frame = 0; frame < frames; ++frame)
		{
			state = state * 1664525u + 1013904223u;
			short sample = (short)(((int)(state >> 16) - 32768) / 7);
			unsigned int encoded = (unsigned short)sample;
			samples[(size_t)frame * 2] = (BYTE)encoded;
			samples[(size_t)frame * 2 + 1] = (BYTE)(encoded >> 8);
		}
		return samples;
	}

	void WriteLE16 (std::vector<BYTE> &bytes, size_t offset, unsigned int value)
	{
		bytes[offset] = (BYTE)value;
		bytes[offset + 1] = (BYTE)(value >> 8);
	}

	void WriteLE32 (std::vector<BYTE> &bytes, size_t offset, unsigned int value)
	{
		WriteLE16 (bytes, offset, value);
		WriteLE16 (bytes, offset + 2, value >> 16);
	}

	std::vector<BYTE> MakeStreamWave (unsigned int frames)
	{
		std::vector<BYTE> bytes (44 + (size_t)frames * 2, 0);
		memcpy (&bytes[0], "RIFF", 4);
		WriteLE32 (bytes, 4, (unsigned int)bytes.size () - 8);
		memcpy (&bytes[8], "WAVEfmt ", 8);
		WriteLE32 (bytes, 16, 16);
		WriteLE16 (bytes, 20, 1);
		WriteLE16 (bytes, 22, 1);
		WriteLE32 (bytes, 24, 8000);
		WriteLE32 (bytes, 28, 16000);
		WriteLE16 (bytes, 32, 2);
		WriteLE16 (bytes, 34, 16);
		memcpy (&bytes[36], "data", 4);
		WriteLE32 (bytes, 40, frames * 2);
		return bytes;
	}

	bool WriteBytes (const char *path, const std::vector<BYTE> &bytes)
	{
		FILE *file = fopen (path, "wb");
		if (file == NULL)
		{
			return false;
		}
		bool written = fwrite (&bytes[0], 1, bytes.size (), file) == bytes.size ();
		fclose (file);
		return written;
	}

	bool ReadFixture (const char *name, std::vector<BYTE> *bytes)
	{
		std::string path = std::string (AUDIO_DECODER_TESTDATA_DIR) + "/" + name;
		FILE *file = fopen (path.c_str (), "rb");
		long length;
		if (file == NULL || fseek (file, 0, SEEK_END) != 0 || (length = ftell (file)) <= 0 || fseek (file, 0, SEEK_SET) != 0)
		{
			if (file != NULL)
			{
				fclose (file);
			}
			return false;
		}
		bytes->assign ((size_t)length, 0);
		bool read = fread (&(*bytes)[0], 1, bytes->size (), file) == bytes->size ();
		fclose (file);
		return read;
	}

	std::string FixturePath (const char *name)
	{
		return std::string (AUDIO_DECODER_TESTDATA_DIR) + "/" + name;
	}

	bool NearlyEqual (float actual, float expected)
	{
		return fabsf (actual - expected) < 0.001f;
	}

	void CheckVector (const ALfloat *actual, float x, float y, float z, const char *name)
	{
		Check (NearlyEqual (actual[0], x) && NearlyEqual (actual[1], y) && NearlyEqual (actual[2], z), name);
	}

	FRolloffInfo MakeLinearRolloff (float minDistance, float maxDistance)
	{
		FRolloffInfo rolloff;
		rolloff.RolloffType = ROLLOFF_Linear;
		rolloff.MinDistance = minDistance;
		rolloff.MaxDistance = maxDistance;
		return rolloff;
	}

	bool IsExpectedEvent (size_t index, OpenALEndReason reason)
	{
		return Events.size () > index && Events[index].Reason == reason && Events[index].SawAttached && Events[index].SawFinalizing;
	}

	void ReleaseOwner (FISoundChannel *owner);
	void StopAndDrain (OpenALSoundRenderer &renderer, FISoundChannel *owner);
	ALint SourceState (FISoundChannel *owner);

	struct StreamFixture
	{
		explicit StreamFixture (int buffers) : BuffersRemaining (buffers), CallCount (0), Ready (true) {}
		StreamFixture (int buffers, bool ready) : BuffersRemaining (buffers), CallCount (0), Ready (ready) {}

		int BuffersRemaining;
		int CallCount;
		bool Ready;
	};

	bool StreamCallback (SoundStream *, void *buffer, int length, void *userData)
	{
		StreamFixture *fixture = (StreamFixture *)userData;
		++fixture->CallCount;
		if (!fixture->Ready)
		{
			return false;
		}
		if (fixture->BuffersRemaining-- <= 0)
		{
			return false;
		}
		memset (buffer, 0, (size_t)length);
		return true;
	}

	bool CallbackLoopAfterEOF (SoundStream *, void *buffer, int length, void *userData)
	{
		StreamFixture *fixture = (StreamFixture *)userData;
		++fixture->CallCount;
		if (fixture->CallCount == 2)
		{
			return false;
		}
		memset (buffer, 0, (size_t)length);
		return true;
	}

	bool CallbackAlwaysEOF (SoundStream *, void *, int, void *userData)
	{
		StreamFixture *fixture = (StreamFixture *)userData;
		++fixture->CallCount;
		return false;
	}

	struct FloatCallbackFixture
	{
		FloatCallbackFixture (const float *samples, size_t count) : Samples (samples, samples + count), CallCount (0) {}

		std::vector<float> Samples;
		int CallCount;
	};

	bool FloatCallback (SoundStream *, void *buffer, int length, void *userData)
	{
		FloatCallbackFixture *fixture = static_cast<FloatCallbackFixture *> (userData);
		++fixture->CallCount;
		if (fixture->CallCount != 1 || length != (int)(fixture->Samples.size () * sizeof (float)))
		{
			return false;
		}
		memcpy (buffer, &fixture->Samples[0], fixture->Samples.size () * sizeof (float));
		return true;
	}

	short FloatToPCM16 (float sample)
	{
		if (sample > 1.f)
		{
			sample = 1.f;
		}
		else if (sample < -1.f)
		{
			sample = -1.f;
		}
		return (short)(sample * 32767.f);
	}

	void TestFloatCallbackPCM (OpenALSoundRenderer &renderer, int flags, bool forcePCM16Fallback, const char *name)
	{
		const float samples[] = { -1.f, -0.5f, 0.25f, 1.f, -0.25f, 0.5f, 0.75f, -0.75f };
		const unsigned int channels = (flags & OALTestStreamMono) ? 1 : 2;
		const unsigned int frames = 4;
		const size_t sampleCount = (size_t)frames * channels;
		const unsigned int expectedOutputBits = forcePCM16Fallback ? 16 : 32;
		FloatCallbackFixture fixture (samples, sampleCount);
		if (!forcePCM16Fallback && !alIsExtensionPresent ("AL_EXT_FLOAT32"))
		{
			fprintf (stderr, "SKIPPED: %s (AL_EXT_FLOAT32 unavailable)\n", name);
			return;
		}
		OALTestForceFloatPCM16Fallback = forcePCM16Fallback;
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (
			reinterpret_cast<SoundStreamCallback> (FloatCallback), (int)(sampleCount * sizeof (float)), flags, 8000, &fixture));
		OALTestForceFloatPCM16Fallback = false;
		Check (stream != NULL && stream->InputBits == 32 && stream->InputIsFloat && stream->OutputBits == expectedOutputBits,
			name);
		if (stream == NULL)
		{
			return;
		}
		Check (stream->Play (false, 1.f) && fixture.CallCount >= 1 && stream->BufferFrames[0] == frames,
			"Float callback preserves frame count through the producer stream");
		Check (stream->OutputBuffer.size () == sampleCount * (expectedOutputBits / 8),
			"Float callback produces the expected uploaded PCM byte count");
		if (expectedOutputBits == 32)
		{
			Check (memcmp (&stream->OutputBuffer[0], samples, sampleCount * sizeof (float)) == 0,
				"Float callback preserves nonzero float32 PCM samples");
		}
		else
		{
			for (size_t index = 0; index < sampleCount; ++index)
			{
				short converted;
				memcpy (&converted, &stream->OutputBuffer[index * sizeof (converted)], sizeof (converted));
				Check (converted == FloatToPCM16 (samples[index]), "Float callback PCM16 fallback preserves sample values");
			}
		}
		delete stream;
	}

	void TestFloatCallbackPCMFormats (OpenALSoundRenderer &renderer)
	{
		TestFloatCallbackPCM (renderer, OALTestStreamMono | OALTestStreamFloat, false, "mono Float callback keeps float input format");
		TestFloatCallbackPCM (renderer, OALTestStreamFloat, false, "stereo Float callback keeps float input format");
		TestFloatCallbackPCM (renderer, OALTestStreamMono | OALTestStreamFloat, true, "mono Float callback converts to PCM16 fallback input format");
		TestFloatCallbackPCM (renderer, OALTestStreamFloat, true, "stereo Float callback converts to PCM16 fallback input format");
	}

	void TestProducerPCMFormat (OpenALSoundRenderer &renderer, int flags, unsigned int inputBits, unsigned int outputBits, const char *name)
	{
		const unsigned int channels = (flags & OALTestStreamMono) ? 1 : 2;
		const unsigned int frames = 4;
		StreamFixture fixture (1);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (
			reinterpret_cast<SoundStreamCallback> (StreamCallback), (int)(frames * channels * (inputBits / 8)), flags, 8000, &fixture));
		Check (stream != NULL && stream->InputBits == inputBits && !stream->InputIsFloat && stream->OutputBits == outputBits, name);
		if (stream != NULL)
		{
			Check (stream->Play (false, 1.f) && stream->BufferFrames[0] == frames, "PCM callback preserves producer frame count");
			delete stream;
		}
	}

	void TestProducerPCMFormats (OpenALSoundRenderer &renderer)
	{
		TestProducerPCMFormat (renderer, OALTestStreamMono | OALTestStreamBits8, 8, 8, "PCM8 callback keeps producer input format");
		TestProducerPCMFormat (renderer, OALTestStreamMono, 16, 16, "PCM16 callback keeps producer input format");
		TestProducerPCMFormat (renderer, OALTestStreamMono | OALTestStreamBits32, 32, 16, "PCM32 callback keeps producer input format");
	}

	void TestTypedPCM16ProducerStorage (OpenALSoundRenderer &renderer)
	{
		OpenALSoundStream *stream = renderer.CreatePatternStreamForTest (16, 0, 0, 8);
		Check (stream != NULL && stream->PCM16Buffer.size () == 4,
			"PCM16 producer receives setup-time typed sample storage");
		if (stream != NULL)
		{
			short *storage = &stream->PCM16Buffer[0];
			size_t capacity = stream->PCM16Buffer.capacity ();
			Check (stream->Play (false, 1.f) && storage == &stream->PCM16Buffer[0] &&
				capacity == stream->PCM16Buffer.capacity (),
				"PCM16 producer reuses setup-time storage while queuing buffers");
			delete stream;
		}
	}

	void DrainStream (OpenALSoundRenderer &renderer, OpenALSoundStream *stream)
	{
		for (int attempt = 0; attempt < 150 && !stream->IsEnded (); ++attempt)
		{
			std::this_thread::sleep_for (std::chrono::milliseconds (10));
			renderer.UpdateSounds ();
		}
	}

	void TestPrePlayEmptyStream (OpenALSoundRenderer &renderer)
	{
		StreamFixture fixture (2, false);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && fixture.CallCount == 0 && !stream->IsEnded (),
			"callback stream does not treat pre-Play empty data as EOF");
		if (stream != NULL)
		{
			fixture.Ready = true;
			Check (stream->Play (false, 1.f), "callback stream starts after caller-side producer readiness");
			DrainStream (renderer, stream);
			Check (stream->GetPosition () > 0 && stream->IsEnded () && fixture.CallCount == 3,
				"post-Play callback data plays and drains after pre-Play empty state");
			delete stream;
		}
	}

	void TestStoppedStreamReplay (OpenALSoundRenderer &renderer)
	{
		StreamFixture fixture (8);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && stream->Play (false, 0.5f), "callback stream starts for explicit stop");
		if (stream != NULL)
		{
			stream->Stop ();
			Check (!stream->IsEnded () && stream->GetPosition () == 0, "callback stream stop resets playback without ending the stream");
			Check (stream->Play (false, 0.5f), "callback stream replays after explicit stop");
			DrainStream (renderer, stream);
			Check (stream->GetPosition () > 0 && stream->IsEnded (), "replayed callback stream drains normally after explicit stop");
			delete stream;
		}
	}

	OpenALSoundStream *CreateStreamInState (OpenALSoundRenderer &renderer, OpenALStreamState state, StreamFixture **fixture)
	{
		OpenALSoundStream *stream;
		*fixture = new StreamFixture (8);
		stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, *fixture));
		if (stream == NULL || state == OALSTREAM_Stopped)
		{
			if (stream != NULL)
			{
				stream->AllowArbitrarySeekForTest ();
			}
			return stream;
		}
		stream->AllowArbitrarySeekForTest ();
		if (state == OALSTREAM_Draining)
		{
			(*fixture)->BuffersRemaining = 1;
		}
		if (state == OALSTREAM_Ended)
		{
			(*fixture)->BuffersRemaining = 0;
		}
		if (state == OALSTREAM_Failed)
		{
			stream->FailNextBufferUploadForTest ();
			stream->Play (false, 1.f);
			return stream;
		}
		stream->Play (false, 1.f);
		if (state == OALSTREAM_Paused)
		{
			stream->SetPaused (true);
		}
		else if (state == OALSTREAM_Ended)
		{
			(*fixture)->BuffersRemaining = 8;
		}
		return stream;
	}

	struct StreamStateContract
	{
		OpenALStreamState State;
		bool PlayResult;
		OpenALStreamState PlayState;
		bool SeekResult;
		OpenALStreamState SeekState;
		bool PauseResult;
		OpenALStreamState PauseState;
		bool ResumeResult;
		OpenALStreamState ResumeState;
	};

	const StreamStateContract StreamStateContracts[] =
	{
		{ OALSTREAM_Stopped, true, OALSTREAM_Playing, true, OALSTREAM_Stopped, false, OALSTREAM_Stopped, false, OALSTREAM_Stopped },
		{ OALSTREAM_Playing, true, OALSTREAM_Playing, true, OALSTREAM_Playing, true, OALSTREAM_Paused, true, OALSTREAM_Playing },
		{ OALSTREAM_Paused, true, OALSTREAM_Paused, true, OALSTREAM_Paused, true, OALSTREAM_Paused, true, OALSTREAM_Playing },
		{ OALSTREAM_Draining, true, OALSTREAM_Draining, true, OALSTREAM_Playing, true, OALSTREAM_Paused, true, OALSTREAM_Draining },
		{ OALSTREAM_Ended, true, OALSTREAM_Playing, true, OALSTREAM_Stopped, false, OALSTREAM_Ended, false, OALSTREAM_Ended },
		{ OALSTREAM_Failed, false, OALSTREAM_Failed, false, OALSTREAM_Failed, false, OALSTREAM_Failed, false, OALSTREAM_Failed }
	};

	OpenALSoundStream *CreateVerifiedStreamInState (OpenALSoundRenderer &renderer, const StreamStateContract &contract, StreamFixture **fixture)
	{
		OpenALSoundStream *stream = CreateStreamInState (renderer, contract.State, fixture);
		Check (stream != NULL && stream->GetStateForTest () == contract.State, "stream state contract creates requested state");
		return stream;
	}

	void TestStreamPlayContract (OpenALSoundRenderer &renderer, const StreamStateContract &contract)
	{
		StreamFixture *fixture;
		OpenALSoundStream *stream = CreateVerifiedStreamInState (renderer, contract, &fixture);
		if (stream != NULL)
		{
			unsigned int queued = stream->GetQueuedBufferCountForTest ();
			unsigned int position = stream->GetPosition ();
			bool result = stream->Play (false, 1.f);
			Check (result == contract.PlayResult && stream->GetStateForTest () == contract.PlayState &&
				(!result ? stream->GetQueuedBufferCountForTest () == 0 : stream->GetQueuedBufferCountForTest () > 0) &&
				(contract.State == OALSTREAM_Playing || contract.State == OALSTREAM_Paused || contract.State == OALSTREAM_Draining ?
					stream->GetQueuedBufferCountForTest () == queued && stream->GetPosition () == position : true),
				"stream state contract Play post-state and queue");
		}
		delete stream;
		delete fixture;
	}

	void TestStreamSeekContract (OpenALSoundRenderer &renderer, const StreamStateContract &contract)
	{
		StreamFixture *fixture;
		OpenALSoundStream *stream = CreateVerifiedStreamInState (renderer, contract, &fixture);
		if (stream != NULL)
		{
			fixture->BuffersRemaining = 8;
			bool result = stream->SetPosition (1);
			Check (result == contract.SeekResult && stream->GetStateForTest () == contract.SeekState &&
				(!result ? stream->GetQueuedBufferCountForTest () == 0 : stream->GetPosition () == 1) &&
				(contract.SeekState == OALSTREAM_Stopped || !result ? stream->GetQueuedBufferCountForTest () == 0 :
					stream->GetQueuedBufferCountForTest () > 0),
				"stream state contract SetPosition post-state, queue, and position");
		}
		delete stream;
		delete fixture;
	}

	void TestStreamPauseContract (OpenALSoundRenderer &renderer, const StreamStateContract &contract, bool paused)
	{
		StreamFixture *fixture;
		OpenALSoundStream *stream = CreateVerifiedStreamInState (renderer, contract, &fixture);
		if (stream != NULL)
		{
			unsigned int queued = stream->GetQueuedBufferCountForTest ();
			bool result = stream->SetPaused (paused);
			OpenALStreamState expectedState = paused ? contract.PauseState : contract.ResumeState;
			bool expectedResult = paused ? contract.PauseResult : contract.ResumeResult;
			Check (result == expectedResult && stream->GetStateForTest () == expectedState &&
				(expectedState == OALSTREAM_Paused || expectedState == OALSTREAM_Playing || expectedState == OALSTREAM_Draining ?
					stream->GetQueuedBufferCountForTest () > 0 : stream->GetQueuedBufferCountForTest () == queued),
				paused ? "stream state contract SetPaused(true) post-state and queue" : "stream state contract SetPaused(false) post-state and queue");
		}
		delete stream;
		delete fixture;
	}

	void TestStreamStopContract (OpenALSoundRenderer &renderer, const StreamStateContract &contract)
	{
		StreamFixture *fixture;
		OpenALSoundStream *stream = CreateVerifiedStreamInState (renderer, contract, &fixture);
		if (stream != NULL)
		{
			stream->Stop ();
			Check (stream->GetStateForTest () == (contract.State == OALSTREAM_Failed ? OALSTREAM_Failed : OALSTREAM_Stopped) &&
				(contract.State == OALSTREAM_Failed || (stream->GetQueuedBufferCountForTest () == 0 && stream->GetPosition () == 0)),
				"stream state contract Stop post-state, queue, and position");
		}
		delete stream;
		delete fixture;
	}

	void TestStreamStateContract (OpenALSoundRenderer &renderer)
	{
		for (unsigned int index = 0; index < sizeof (StreamStateContracts) / sizeof (StreamStateContracts[0]); ++index)
		{
			TestStreamPlayContract (renderer, StreamStateContracts[index]);
			TestStreamSeekContract (renderer, StreamStateContracts[index]);
			TestStreamPauseContract (renderer, StreamStateContracts[index], true);
			TestStreamPauseContract (renderer, StreamStateContracts[index], false);
			TestStreamStopContract (renderer, StreamStateContracts[index]);
		}
	}

	OpenALSoundStream *CreateKnownDurationStreamInState (OpenALSoundRenderer &renderer, OpenALStreamState state, unsigned int *duration)
	{
		unsigned int frames = state == OALSTREAM_Draining || state == OALSTREAM_Ended ? 1200 : 8000;
		OpenALSoundStream *stream = renderer.CreatePatternStreamForTest (frames, 0, 0, 800);
		*duration = frames / 8;
		if (stream == NULL || state == OALSTREAM_Stopped)
		{
			return stream;
		}
		stream->Play (false, 1.f);
		if (state == OALSTREAM_Paused)
		{
			stream->SetPaused (true);
		}
		else if (state == OALSTREAM_Ended)
		{
			stream->SetPosition (*duration);
		}
		return stream;
	}

	void TestExactDurationSeekMatrix (OpenALSoundRenderer &renderer)
	{
		const OpenALStreamState states[] = { OALSTREAM_Stopped, OALSTREAM_Playing, OALSTREAM_Paused, OALSTREAM_Draining, OALSTREAM_Ended };
		for (unsigned int index = 0; index < sizeof (states) / sizeof (states[0]); ++index)
		{
			unsigned int duration = 0;
			OpenALSoundStream *stream = CreateKnownDurationStreamInState (renderer, states[index], &duration);
			Check (stream != NULL && stream->GetStateForTest () == states[index], "exact-duration matrix creates known-duration source state");
			if (stream != NULL)
			{
				OpenALStreamState oldState = stream->GetStateForTest ();
				unsigned int oldQueue = stream->GetQueuedBufferCountForTest ();
				unsigned int oldPosition = stream->GetPosition ();
				Check (!stream->SetPosition (duration + 1) && stream->GetStateForTest () == oldState &&
					stream->GetQueuedBufferCountForTest () == oldQueue && stream->GetPosition () == oldPosition,
					"known-duration past-end seek preserves state, queue, and position");
				Check (stream->SetPosition (duration) && stream->GetStateForTest () == OALSTREAM_Ended && stream->IsEnded () &&
					stream->GetQueuedBufferCountForTest () == 0 && stream->GetPosition () == duration,
					"known-duration exact seek ends with no queued PCM at the media duration");
				Check (!stream->SetPaused (true) && !stream->SetPaused (false) && stream->GetStateForTest () == OALSTREAM_Ended,
					"exact-duration ended state ignores pause reasons");
				Check (stream->Play (false, 1.f) && stream->GetStateForTest () != OALSTREAM_Paused &&
					stream->GetQueuedBufferCountForTest () > 0, "exact-duration replay clears prior pause state");
				delete stream;
			}
		}
	}

	void TestCallbackLoopAndSeekFailures (OpenALSoundRenderer &renderer)
	{
		StreamFixture loopFixture (0);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (CallbackLoopAfterEOF), 1600, 1, 8000, &loopFixture));
		Check (stream != NULL && stream->Play (true, 1.f) && loopFixture.CallCount >= 3,
			"callback Play(true) starts and retries the producer after EOF");
		delete stream;

		StreamFixture fixture (16);
		stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && stream->Play (false, 1.f), "unknown-duration callback stream starts for seek contract");
		if (stream != NULL)
		{
			stream->AllowArbitrarySeekForTest ();
			unsigned int queued = stream->GetQueuedBufferCountForTest ();
			Check (stream->SetPosition (1) && stream->GetStateForTest () == OALSTREAM_Playing && stream->GetQueuedBufferCountForTest () > 0,
				"unknown-duration seek succeeds without a total-frame declaration");
			stream->FailNextRewindForTest (false);
			Check (!stream->SetPosition (2) && stream->GetStateForTest () == OALSTREAM_Playing && stream->GetQueuedBufferCountForTest () == queued,
				"recoverable unknown-duration seek failure preserves playing queue and state");
			stream->SetPaused (true);
			queued = stream->GetQueuedBufferCountForTest ();
			stream->FailNextRewindForTest (false);
			stream->Stop ();
			Check (stream->GetStateForTest () == OALSTREAM_Paused && stream->GetQueuedBufferCountForTest () == queued,
				"recoverable Stop rewind failure preserves paused queue and state");
			stream->SetPaused (false);
			queued = stream->GetQueuedBufferCountForTest ();
			stream->FailNextRewindForTest (false);
			stream->Stop ();
			Check (stream->GetStateForTest () == OALSTREAM_Playing && stream->GetQueuedBufferCountForTest () == queued,
				"recoverable Stop rewind failure preserves playing queue and state");
			delete stream;
		}
	}

	void TestTerminalStreamFailure (OpenALSoundRenderer &renderer)
	{
		{
			StreamFixture seekFixture (16);
			OpenALSoundStream *seekStream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &seekFixture));
			Check (seekStream != NULL && seekStream->Play (false, 1.f), "terminal seek failure stream starts");
			if (seekStream != NULL)
			{
				seekStream->FailNextRewindForTest (true);
				Check (!seekStream->SetPosition (1) && seekStream->GetStateForTest () == OALSTREAM_Failed && seekStream->IsEnded () &&
					seekStream->GetQueuedBufferCountForTest () == 0, "terminal decoder seek failure stops and unqueues stale PCM immediately");
				delete seekStream;
			}
		}
		StreamFixture fixture (16);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && stream->Play (false, 1.f), "terminal failure stream starts");
		if (stream != NULL)
		{
			unsigned int source = stream->Source;
			unsigned int buffer = stream->Buffers[0];
			stream->FailNextBufferUploadForTest ();
			alSourceStop (source);
			renderer.UpdateSounds ();
			Check (stream->GetStateForTest () == OALSTREAM_Failed && stream->IsEnded () && stream->GetQueuedBufferCountForTest () == 0,
				"terminal upload failure stops and unqueues stale PCM immediately");
			stream->Stop ();
			Check (stream->GetStateForTest () == OALSTREAM_Failed && stream->GetQueuedBufferCountForTest () == 0,
				"failed stream Stop remains an idempotent no-op");
			delete stream;
			Check (!alIsSource (source) && !alIsBuffer (buffer) && alGetError () == AL_NO_ERROR,
				"failed stream destruction releases resources exactly once without OpenAL errors");
		}
	}

	void TestTerminalALErrors (OpenALSoundRenderer &renderer)
	{
		const OpenALStreamTestALOperation operations[] =
		{
			OALTESTAL_SourceQuery, OALTESTAL_SourcePlay, OALTESTAL_SourcePause, OALTESTAL_BufferUpload,
			OALTESTAL_BufferQueue, OALTESTAL_BufferUnqueue, OALTESTAL_PositionQuery
		};
		const char *names[] = { "source query", "source play", "source pause", "buffer upload", "buffer queue", "buffer unqueue", "position query" };
		for (unsigned int index = 0; index < sizeof (operations) / sizeof (operations[0]); ++index)
		{
			StreamFixture fixture (16);
			OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
			if (stream == NULL)
			{
				Check (false, "terminal AL error stream allocation");
				continue;
			}
			if (operations[index] == OALTESTAL_SourcePause)
			{
				Check (stream->Play (false, 1.f), "terminal AL pause error stream starts");
				stream->FailNextALOperationForTest (operations[index]);
				Check (!stream->SetPaused (true), "terminal AL pause error reports failure");
			}
			else if (operations[index] == OALTESTAL_BufferUnqueue)
			{
				Check (stream->Play (false, 1.f), "terminal AL unqueue error stream starts");
				stream->FailNextALOperationForTest (operations[index]);
				Check (!stream->ProcessNextBufferForTest (), "terminal AL unqueue error reports failure");
			}
			else if (operations[index] == OALTESTAL_PositionQuery)
			{
				Check (stream->Play (false, 1.f), "terminal AL position error stream starts");
				stream->FailNextALOperationForTest (operations[index]);
				stream->GetPosition ();
			}
			else
			{
				stream->FailNextALOperationForTest (operations[index]);
				Check (!stream->Play (false, 1.f), "terminal AL query/play error reports failure");
			}
			ALint sourceState = AL_PLAYING;
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &sourceState);
			Check (stream->GetStateForTest () == OALSTREAM_Failed && stream->IsEnded () &&
				stream->GetQueuedBufferCountForTest () == 0 && sourceState != AL_PLAYING && sourceState != AL_PAUSED && alGetError () == AL_NO_ERROR,
				names[index]);
			Check (alIsSource (stream->Source) && alIsBuffer (stream->Buffers[0]), "failed AL stream keeps resources until destruction");
			stream->Stop ();
			Check (stream->GetStateForTest () == OALSTREAM_Failed && stream->GetQueuedBufferCountForTest () == 0 && !stream->Play (false, 1.f) &&
				!stream->SetPosition (1) && !stream->SetPaused (true) && !stream->SetPaused (false), "failed AL stream commands cannot resurrect it");
			unsigned int source = stream->Source;
			unsigned int buffer = stream->Buffers[0];
			delete stream;
			Check (!alIsSource (source) && !alIsBuffer (buffer) && alGetError () == AL_NO_ERROR,
				"failed AL stream destruction releases resources exactly once without OpenAL errors");
		}
	}

	void TestForwardLoopBoundary (OpenALSoundRenderer &renderer)
	{
		OpenALSoundStream *stream = renderer.CreatePatternStreamForTest (6, 2, 5, 8);
		Check (stream != NULL && stream->Play (true, 1.f), "controlled forward-loop stream starts");
		if (stream != NULL)
		{
			ALint tailBytes = 0;
			alGetBufferi (stream->Buffers[1], AL_SIZE, &tailBytes);
			Check (stream->GetBufferMediaStartForTest (0) == 0 && stream->GetBufferFramesForTest (0) == 4 &&
				stream->GetBufferMediaStartForTest (1) == 4 && stream->GetBufferFramesForTest (1) == 1 && tailBytes == 2 &&
				stream->GetBufferMediaStartForTest (2) == 2 && stream->GetBufferFramesForTest (2) == 3,
				"forward loop keeps tail and head in separate submitted buffers");
			delete stream;
		}
	}

	void TestStreamPauseReasonsAndBoundaries (OpenALSoundRenderer &renderer)
	{
		StreamFixture pauseFixture (32);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &pauseFixture));
		Check (stream != NULL && stream->Play (false, 1.f), "pause-reason stream starts");
		if (stream != NULL)
		{
			stream->AllowArbitrarySeekForTest ();
			renderer.SetInactive (INACTIVE_Complete);
			Check (stream->SetPosition (1) && stream->GetStateForTest () == OALSTREAM_Paused,
				"inactive-only paused seek remains paused");
			renderer.SetInactive (INACTIVE_Active);
			ALint sourceState = AL_STOPPED;
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &sourceState);
			Check (stream->GetStateForTest () == OALSTREAM_Playing && sourceState == AL_PLAYING,
				"clearing inactive pause resumes an inactive-only paused seek");
			Check (stream->SetPaused (true) && stream->SetPosition (2) && stream->GetStateForTest () == OALSTREAM_Paused,
				"user-paused seek remains user paused");
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &sourceState);
			Check (sourceState != AL_PLAYING, "user-paused seek does not auto-resume");
			Check (stream->SetPaused (false), "user-paused seek accepts explicit resume");
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &sourceState);
			Check (sourceState == AL_PLAYING, "user-paused seek resumes only after user resume");
			delete stream;
		}

		StreamFixture roundingFixture (32);
		stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 22050, 1, 11025, &roundingFixture));
		if (stream != NULL)
		{
			stream->AllowArbitrarySeekForTest ();
		}
		Check (stream != NULL && stream->Play (false, 1.f) && stream->SetPosition (1) &&
			stream->GetBufferMediaStartForTest (0) == 11 && stream->GetPosition () == 0,
			"non-integral stream seek uses floor frame conversion");
		delete stream;

		stream = renderer.CreatePatternStreamForTest (6000, 2000, 5000, 8000);
		Check (stream != NULL && stream->Play (true, 1.f), "runtime custom-loop stream starts");
		if (stream != NULL)
		{
			alSourceStop (stream->Source);
			renderer.UpdateSounds ();
			Check (stream->GetMediaFrameForTest () >= 2000 && stream->GetMediaFrameForTest () < 5000,
				"runtime custom-loop cursor wraps inside the loop range");
			delete stream;
		}

		StreamFixture eofFixture (0);
		stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (CallbackAlwaysEOF), 1600, 1, 8000, &eofFixture));
		Check (stream != NULL && !stream->Play (true, 1.f) && stream->IsEnded () &&
			stream->GetQueuedBufferCountForTest () == 0 && eofFixture.CallCount == 2,
			"looping callback reaches natural Ended after repeated EOF");
		delete stream;
	}

	void TestStreamPositionConversion (OpenALSoundRenderer &renderer)
	{
		StreamFixture fixture (1);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 48000, &fixture));
		if (stream != NULL)
		{
			stream->SetProcessedFramesForTest (48ull * 60 * 5 * 1000);
			Check (stream->GetPosition () == 300000, "48 kHz stream position reports several minutes without false saturation");
			stream->SetProcessedFramesForTest (std::numeric_limits<unsigned long long>::max ());
			Check (stream->GetPosition () == std::numeric_limits<unsigned int>::max (), "stream position safely saturates true millisecond overflow");
			delete stream;
		}
	}

	void TestEncodedStreams (OpenALSoundRenderer &renderer)
	{
		const char *filePath = "openal_lifecycle_stream.wav";
		const char *slicePath = "openal_lifecycle_stream_slice.bin";
		std::vector<BYTE> data = MakeStreamWave (8000);
		std::vector<BYTE> wrapped;
		OpenALSoundStream *stream;
		Check (WriteBytes (filePath, data), "encoded stream fixture writes");
		stream = static_cast<OpenALSoundStream *> (renderer.OpenStream (filePath, 0, 0, 0));
		Check (stream != NULL, "encoded file stream opens");
		if (stream != NULL)
		{
			Check (stream->SetPosition (500) && stream->GetPosition () == 500, "encoded stream seeks while stopped");
			Check (stream->Play (false, 1.f), "encoded stream plays after stopped seek");
			ALint uploadedBytes = 0;
			alGetBufferi (stream->Buffers[0], AL_SIZE, &uploadedBytes);
			Check (uploadedBytes == (8000 - 4000) * 2, "encoded final PCM buffer uploads only its partial frame count");
			stream->SetPaused (true);
			Check (stream->SetPosition (750) && stream->GetPosition () == 750, "encoded stream seeks while paused");
			stream->SetPaused (false);
			DrainStream (renderer, stream);
			Check (stream->IsEnded () && stream->GetPosition () == 1000, "encoded stream drains at exact duration");
			Check (stream->SetPosition (1000) && stream->IsEnded (), "encoded stream accepts exact-duration seek as ended");
			ALint replayState = AL_STOPPED;
			Check (stream->Play (false, 1.f) && stream->GetStateForTest () != OALSTREAM_Paused && stream->GetQueuedBufferCountForTest () > 0,
				"exact-duration replay clears the previous user pause state and queues playback");
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &replayState);
			Check (replayState == AL_PLAYING, "exact-duration replay resumes its OpenAL source");
			Check (!stream->SetPosition (1001) && !stream->SetPosition (std::numeric_limits<unsigned int>::max ()), "encoded stream rejects past-end and overflow seeks");
			stream->Stop ();
			Check (!stream->IsEnded () && stream->GetPosition () == 0, "encoded stream stop rewinds for replay");
			delete stream;
		}
		stream = static_cast<OpenALSoundStream *> (renderer.OpenStream ((const char *)&data[0], 0, -1, (int)data.size ()));
		Check (stream != NULL && stream->Play (false, 1.f), "encoded memory stream opens and plays");
		if (stream != NULL)
		{
			delete stream;
		}
		std::vector<BYTE> empty = MakeStreamWave (0);
		stream = static_cast<OpenALSoundStream *> (renderer.OpenStream ((const char *)&empty[0], 0, -1, (int)empty.size ()));
		Check (stream != NULL && !stream->Play (true, 1.f) && stream->IsEnded () && stream->GetQueuedBufferCountForTest () == 0,
			"zero-frame stream reaches Ended without queueing or retrying");
		delete stream;
		wrapped.insert (wrapped.end (), "head", "head" + 4);
		wrapped.insert (wrapped.end (), data.begin (), data.end ());
		wrapped.insert (wrapped.end (), "tail", "tail" + 4);
		Check (WriteBytes (slicePath, wrapped), "encoded stream slice fixture writes");
		stream = static_cast<OpenALSoundStream *> (renderer.OpenStream (slicePath, 0, 4, (int)data.size ()));
		Check (stream != NULL && stream->Play (true, 1.f) && !stream->IsEnded (), "encoded file slice opens and loops");
		if (stream != NULL)
		{
			delete stream;
		}
		remove (filePath);
		remove (slicePath);
	}

	void TestPhase1BDirectMemory (OpenALSoundRenderer &renderer)
	{
		std::vector<BYTE> bytes;
		SoundHandle sound = { NULL };
		Check (ReadFixture ("vorbis_mono.ogg", &bytes), "Phase 1B direct-memory fixture reads");
		if (bytes.empty ())
		{
			return;
		}
		sound = renderer.LoadSound (&bytes[0], (int)bytes.size ());
		Check (sound.data != NULL, "Phase 1B direct-memory fixture loads from exact Vorbis bytes");
		if (sound.data != NULL)
		{
			renderer.UnloadSound (sound);
			Check (alGetError () == AL_NO_ERROR, "Phase 1B direct-memory fixture releases OpenAL resources");
		}
	}

	void TestPhase1BFileSlice (OpenALSoundRenderer &renderer)
	{
		const std::string path = FixturePath ("float32_mono.wav");
		std::vector<BYTE> bytes;
		OpenALSoundStream *stream;
		Check (ReadFixture ("float32_mono.wav", &bytes), "Phase 1B file-slice fixture reads");
		if (bytes.empty ())
		{
			return;
		}
		stream = static_cast<OpenALSoundStream *> (renderer.OpenStream (path.c_str (), 0, 0, (int)bytes.size ()));
		Check (stream != NULL && stream->Play (false, 1.f), "Phase 1B bounded file slice opens from exact WAVE bytes");
		delete stream;
	}

	void TestEncodedSfxFixture (OpenALSoundRenderer &renderer, const char *name, unsigned int channels)
	{
		std::vector<BYTE> bytes;
		SoundHandle sound = { NULL };
		Check (ReadFixture (name, &bytes), "encoded SFX fixture reads");
		if (bytes.empty ())
		{
			return;
		}
		sound = renderer.LoadSound (&bytes[0], (int)bytes.size ());
		OpenALSound *openalSound = (OpenALSound *)sound.data;
		Check (openalSound != NULL && openalSound->Channels == channels && openalSound->Frames != 0,
			"identified encoded SFX loads into OpenAL PCM");
		if (openalSound != NULL)
		{
			FISoundChannel *channel = renderer.StartSound (sound, 0.5f, 128, 0, 0, NULL);
			ALint buffer = 0;
			Check (channel != NULL && SourceState (channel) == AL_PLAYING, "identified encoded SFX plays through an OpenAL source");
			if (channel != NULL)
			{
				alGetSourcei (((OpenALChannel *)channel->SysChannel)->Source, AL_BUFFER, &buffer);
				Check ((unsigned int)buffer == openalSound->Buffer2D, "encoded SFX 2D playback keeps the original channel buffer");
				StopAndDrain (renderer, channel);
				ReleaseOwner (channel);
			}
			if (channels == 2)
			{
				SoundListener listener;
				FRolloffInfo rolloff = MakeLinearRolloff (0.f, 100.f);
				FISoundChannel *spatial;
				listener.valid = true;
				spatial = renderer.StartSound3D (sound, &listener, 0.5f, &rolloff, 1.f, 128, 0, FVector3 (1.f, 0.f, 0.f), FVector3 (), 0, 0, NULL);
				Check (spatial != NULL && openalSound->BufferMono != 0, "stereo encoded SFX creates a mono spatial buffer");
				if (spatial != NULL)
				{
					alGetSourcei (((OpenALChannel *)spatial->SysChannel)->Source, AL_BUFFER, &buffer);
					Check ((unsigned int)buffer == openalSound->BufferMono, "stereo encoded SFX 3D playback selects the mono spatial buffer");
					StopAndDrain (renderer, spatial);
					ReleaseOwner (spatial);
				}
			}
			renderer.UnloadSound (sound);
			Check (alGetError () == AL_NO_ERROR, "encoded SFX unload leaves no OpenAL error");
		}
	}

	void TestDumbVorbisSample (OpenALSoundRenderer &renderer)
	{
		std::vector<BYTE> mono;
		std::vector<BYTE> stereo;
		AudioMemorySource source;
		AudioDecodedPCM16 decoded;
		short *output;
		const BYTE *outputBytes;
		Check (ReadFixture ("vorbis_mono.ogg", &mono) && ReadFixture ("vorbis_stereo.ogg", &stereo), "DUMB Vorbis fixtures read");
		if (mono.empty () || stereo.empty ())
		{
			return;
		}
		source.Assign (&mono[0], mono.size ());
		Check (DecodeAudioToPCM16 (source, ProbeAudioFormat (source), &decoded) == AUDIO_DECODE_OK && decoded.Channels == 1,
			"DUMB Vorbis expected PCM decodes");
		output = renderer.DecodeSample ((int)(decoded.Samples.size () * sizeof (short) + 4), &mono[0], (int)mono.size (), CODEC_Vorbis);
		outputBytes = reinterpret_cast<const BYTE *> (output);
		Check (output != NULL && memcmp (output, &decoded.Samples[0], decoded.Samples.size () * sizeof (short)) == 0 &&
			outputBytes[decoded.Samples.size () * sizeof (short)] == 0 && outputBytes[decoded.Samples.size () * sizeof (short) + 3] == 0,
			"DUMB Vorbis returns malloc PCM16 and zero-fills clean EOF");
		free (output);
		Check (renderer.DecodeSample (16, &stereo[0], (int)stereo.size (), CODEC_Vorbis) == NULL,
			"DUMB Vorbis rejects stereo PCM output");
		Check (renderer.DecodeSample (16, &mono[0], (int)mono.size (), CODEC_Unknown) == NULL,
			"DUMB decoder rejects an unsupported codec type");
		mono.resize (64);
		Check (renderer.DecodeSample (16, &mono[0], (int)mono.size (), CODEC_Vorbis) == NULL,
			"DUMB Vorbis frees and fails on truncated input");
	}

	void TestEncodedSfxAndDumb (OpenALSoundRenderer &renderer)
	{
		const unsigned int encodedInputCap = 64 * 1024 * 1024;
		Check (OALTestAcceptsEncodedInputSize (encodedInputCap), "encoded input size cap is accepted");
		Check (!OALTestAcceptsEncodedInputSize (encodedInputCap + 1), "encoded input size cap plus one is rejected");
		TestEncodedSfxFixture (renderer, "vorbis_mono.ogg", 1);
		TestEncodedSfxFixture (renderer, "vorbis_stereo.ogg", 2);
		TestEncodedSfxFixture (renderer, "mp3_mono.mp3", 1);
		TestEncodedSfxFixture (renderer, "flac_stereo.flac", 2);
		TestEncodedSfxFixture (renderer, "float32_mono.wav", 1);
		std::vector<BYTE> truncated;
		Check (ReadFixture ("vorbis_mono.ogg", &truncated), "truncated encoded SFX fixture reads");
		if (truncated.size () > 64)
		{
			truncated.resize (64);
			Check (renderer.LoadSound (&truncated[0], (int)truncated.size ()).data == NULL,
				"malformed encoded SFX falls back to an empty sound handle");
		}
		OALTestFailNextEncodedSourceAssign ();
		Check (renderer.LoadSound (&truncated[0], (int)truncated.size ()).data == NULL,
			"encoded SFX source allocation failure falls back to an empty sound handle");
		OALTestFailNextEncodedSourceAssign ();
		Check (renderer.DecodeSample (16, &truncated[0], (int)truncated.size (), CODEC_Vorbis) == NULL,
			"DUMB source allocation failure falls back to null");
		TestDumbVorbisSample (renderer);
	}

	void TestInitialStreamPosition (OpenALSoundRenderer &renderer)
	{
		StreamFixture fixture (1);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (
			reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && stream->GetMediaFrameForTest () == 0 && stream->GetPosition () == 0,
			"new callback stream reports the deterministic initial media position");
		renderer.SetInactive (INACTIVE_Complete);
		Check (stream != NULL && stream->Play (false, 0.8f) && stream->GetStateForTest () == OALSTREAM_Paused &&
			stream->GetMediaFrameForTest () == 0 && stream->GetPosition () == 0,
			"first callback stream playback preserves the exact paused initial position");
		renderer.SetInactive (INACTIVE_Active);
		delete stream;
	}

	void TestStreams (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		TestFloatCallbackPCMFormats (renderer);
		TestProducerPCMFormats (renderer);
		TestTypedPCM16ProducerStorage (renderer);
		TestInitialStreamPosition (renderer);
		StreamFixture fixture (5);
		OpenALSoundStream *stream = static_cast<OpenALSoundStream *> (renderer.CreateStream (reinterpret_cast<SoundStreamCallback> (StreamCallback), 1600, 1, 8000, &fixture));
		Check (stream != NULL && stream->Source != 0 && stream->Buffers[0] != 0 && stream->Buffers[1] != 0 &&
			stream->Buffers[2] != 0 && stream->Buffers[3] != 0 && renderer.ActiveStreams.size () == 1,
			"callback stream owns a dedicated source and four buffers");
		Check (stream != NULL && stream->Source != renderer.Sources[0], "callback stream does not borrow the SFX source pool");
		FISoundChannel *sfx = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		Check (sfx != NULL && sfx->SysChannel != NULL, "SFX allocation does not evict the callback stream");
		renderer.SetMusicVolume (0.5f);
		Check (stream != NULL && stream->Play (false, 0.8f), "callback stream begins playback from queued PCM");
		if (stream != NULL)
		{
			ALfloat gain = 0.f;
			alGetSourcef (stream->Source, AL_GAIN, &gain);
			Check (NearlyEqual (gain, 0.4f), "callback stream gain combines stream and global music volume");
			stream->SetPaused (true);
			ALint state = AL_STOPPED;
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &state);
			Check (state == AL_PAUSED, "callback stream pauses its dedicated source");
			stream->SetPaused (false);
			renderer.SetInactive (INACTIVE_Mute);
			alGetSourcef (stream->Source, AL_GAIN, &gain);
			Check (NearlyEqual (gain, 0.f), "inactive mute suppresses callback stream gain");
			renderer.SetInactive (INACTIVE_Complete);
			alGetSourcei (stream->Source, AL_SOURCE_STATE, &state);
			Check (state == AL_PAUSED, "complete inactive pauses callback stream");
			renderer.SetInactive (INACTIVE_Active);
			DrainStream (renderer, stream);
			Check (stream->GetPosition () > 0 && stream->IsEnded () && fixture.CallCount > 4,
				"callback stream refills processed buffers then drains EOF before ending");
			unsigned int streamSource = stream->Source;
			unsigned int streamBuffer = stream->Buffers[0];
			delete stream;
			Check (!alIsSource (streamSource) && !alIsBuffer (streamBuffer) && renderer.ActiveStreams.empty (),
				"ended callback stream releases dedicated OpenAL resources on destruction");
		}
		StopAndDrain (renderer, sfx);
		ReleaseOwner (sfx);
		renderer.SetMusicVolume (1.f);
		TestPrePlayEmptyStream (renderer);
		TestStoppedStreamReplay (renderer);
		TestStreamPositionConversion (renderer);
		TestStreamStateContract (renderer);
		TestExactDurationSeekMatrix (renderer);
		TestCallbackLoopAndSeekFailures (renderer);
		TestTerminalStreamFailure (renderer);
		TestTerminalALErrors (renderer);
		TestForwardLoopBoundary (renderer);
		TestStreamPauseReasonsAndBoundaries (renderer);
		TestEncodedStreams (renderer);
		Check (renderer.OpenStream ("https://example.invalid/music.ogg", 0, 0, 0) == NULL, "encoded URL stream opening is rejected safely");
	}

	void ReleaseOwner (FISoundChannel *owner)
	{
		delete owner;
	}

	void ReleaseOwnerForReuse (FISoundChannel *owner)
	{
		owner->SysChannel = NULL;
		owner->StartTime.AsOne = 0;
		owner->Priority = 0;
		ForcedNextOwner = owner;
	}

	void StopAndDrain (OpenALSoundRenderer &renderer, FISoundChannel *owner)
	{
		if (owner != NULL && owner->SysChannel != NULL)
		{
			renderer.StopChannel (owner);
		}
		renderer.UpdateSounds ();
	}

	ALint SourceState (FISoundChannel *owner)
	{
		ALint state = AL_STOPPED;
		OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
		if (channel != NULL)
		{
			alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
		}
		return state;
	}

	unsigned int SourceOffset (FISoundChannel *owner)
	{
		ALint offset = 0;
		OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
		if (channel != NULL)
		{
			alGetSourcei (channel->Source, AL_SAMPLE_OFFSET, &offset);
		}
		return offset < 0 ? 0 : (unsigned int)offset;
	}

	void AdvanceTestClock (OpenALSoundRenderer &renderer, unsigned int milliseconds)
	{
		TestMilliseconds += milliseconds;
		renderer.UpdateSounds ();
	}

	unsigned int ExpectedLoopPosition (unsigned long long sampleFrame, unsigned int loopStart, unsigned int loopEnd)
	{
		return sampleFrame < loopStart ? (unsigned int)sampleFrame :
			loopStart + (unsigned int)((sampleFrame - loopStart) % (loopEnd - loopStart));
	}

	void TestPauseReasonsAndClocks (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		FISoundChannel *normal = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *noPause = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP | SNDF_NOPAUSE, NULL);
		Check (normal != NULL && noPause != NULL, "pause fixture starts normal and NOPAUSE sources");
		renderer.SetSfxPaused (true, 0);
		Check (SourceState (normal) == AL_PAUSED && SourceState (noPause) == AL_PLAYING, "gameplay pause affects only pausable source");
		unsigned long long pausableBefore = renderer.PausableOutputFrames;
		unsigned long long nonPausableBefore = renderer.NonPausableOutputFrames;
		AdvanceTestClock (renderer, 2000);
		Check (renderer.PausableOutputFrames == pausableBefore, "two-second gameplay pause freezes pausable logical clock");
		Check (renderer.NonPausableOutputFrames == nonPausableBefore + (unsigned long long)renderer.GetOutputRate () * 2, "two-second gameplay pause advances nonpausable logical clock");
		renderer.Sync (true);
		Check (SourceState (normal) == AL_PAUSED && SourceState (noPause) == AL_PAUSED, "sync pauses every source");
		renderer.SetSfxPaused (false, 0);
		Check (SourceState (normal) == AL_PAUSED && SourceState (noPause) == AL_PAUSED, "sync prevents gameplay resume from restarting either source");
		renderer.Sync (false);
		Check (SourceState (normal) == AL_PLAYING && SourceState (noPause) == AL_PLAYING, "sync restore resumes sources with no remaining reason");
		renderer.SetInactive (INACTIVE_Complete);
		renderer.Sync (true);
		renderer.SetInactive (INACTIVE_Active);
		Check (SourceState (normal) == AL_PAUSED && SourceState (noPause) == AL_PAUSED, "sync retains sources paused after complete inactive reason clears");
		renderer.Sync (false);
		Check (SourceState (normal) == AL_PLAYING && SourceState (noPause) == AL_PLAYING, "sync restore resumes sources after nested inactive reason clears");
		renderer.SetInactive (INACTIVE_Complete);
		pausableBefore = renderer.PausableOutputFrames;
		nonPausableBefore = renderer.NonPausableOutputFrames;
		AdvanceTestClock (renderer, 2000);
		Check (renderer.PausableOutputFrames == pausableBefore && renderer.NonPausableOutputFrames == nonPausableBefore, "two-second complete inactive pause freezes both logical clocks");
		renderer.SetInactive (INACTIVE_Active);
		StopAndDrain (renderer, normal);
		StopAndDrain (renderer, noPause);
		ReleaseOwner (normal);
		ReleaseOwner (noPause);
	}

	void TestInactiveMuteAndComplete (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		FISoundChannel *channel = renderer.StartSound (sound, 0.75f, 128, 80, SNDF_LOOP, NULL);
		Check (channel != NULL, "inactive fixture starts a looping source");
		renderer.SetInactive (INACTIVE_Mute);
		ALfloat gain = 1.f;
		OpenALChannel *openalChannel = channel == NULL ? NULL : (OpenALChannel *)channel->SysChannel;
		if (openalChannel != NULL)
		{
			alGetSourcef (openalChannel->Source, AL_GAIN, &gain);
		}
		Check (SourceState (channel) == AL_PLAYING && NearlyEqual (gain, 0.f), "inactive mute preserves playback while suppressing effective gain");
		unsigned int mutedStart = SourceOffset (channel);
		std::this_thread::sleep_for (std::chrono::milliseconds (30));
		Check (SourceOffset (channel) > mutedStart, "inactive mute cursor continues advancing");
		renderer.SetInactive (INACTIVE_Active);
		renderer.SetInactive (INACTIVE_Complete);
		unsigned int completeOffset = renderer.GetPosition (channel);
		std::this_thread::sleep_for (std::chrono::milliseconds (30));
		Check (SourceState (channel) == AL_PAUSED && renderer.GetPosition (channel) == completeOffset, "inactive complete freezes source state and cursor");
		renderer.SetInactive (INACTIVE_Active);
		StopAndDrain (renderer, channel);
		ReleaseOwner (channel);
	}

	void TestEarlyClockAbstimeRestart (OpenALSoundRenderer &renderer)
	{
		std::vector<BYTE> samples = MakeSamples (8000);
		SoundHandle loopSound = renderer.LoadSoundRaw (&samples[0], (int)samples.size (), 8000, 1, -16, 100, 500);
		renderer.PausableOutputFrames = 0;
		renderer.PausableFrameRemainder = 0;
		renderer.NonPausableOutputFrames = 0;
		renderer.NonPausableFrameRemainder = 0;
		TestMilliseconds = 0;
		renderer.LastClockMilliseconds = 0;
		const unsigned int earlySavedFrame = 321;
		const unsigned int earlyElapsedMilliseconds = 137;
		FISoundChannel *earlySaved = new FISoundChannel;
		earlySaved->StartTime.AsOne = earlySavedFrame;
		FISoundChannel *earlyRestored = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP | SNDF_ABSTIME, earlySaved);
		Check (earlyRestored == earlySaved && abs ((int)SourceOffset (earlySaved) - (int)earlySavedFrame) <= 1,
			"early-clock SNDF_ABSTIME directly seeks the saved sample frame");
		AdvanceTestClock (renderer, earlyElapsedMilliseconds);
		FISoundChannel *earlyEvictor = renderer.StartSound (loopSound, 0.5f, 128, 81, SNDF_LOOP, NULL);
		unsigned long long elapsedOutputFramesAtEviction = (unsigned long long)renderer.GetOutputRate () * earlyElapsedMilliseconds / 1000;
		unsigned long long expectedEarlySamples = earlySavedFrame +
			(unsigned long long)((long double)elapsedOutputFramesAtEviction * 8000 / renderer.GetOutputRate ());
		unsigned int expected = ExpectedLoopPosition (expectedEarlySamples, 100, 500);
		unsigned int zeroOriginExpected = ExpectedLoopPosition (
			(unsigned long long)((long double)elapsedOutputFramesAtEviction * 8000 / renderer.GetOutputRate ()), 100, 500);
		Check (earlyEvictor != NULL && earlySaved->SysChannel == NULL, "early-clock ABSTIME source can be re-evicted");
		StopAndDrain (renderer, earlyEvictor);
		unsigned int serializedPosition = renderer.GetPosition (earlySaved);
		Check (abs ((int)serializedPosition - (int)expected) <= 2 && expected != earlySavedFrame && expected != zeroOriginExpected,
			"early-clock logical phase differs from both saved position and zero-origin phase");
		earlySaved->StartTime.AsOne = serializedPosition;
		FISoundChannel *earlyRestoredAgain = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP | SNDF_ABSTIME, earlySaved);
		Check (earlyRestoredAgain == earlySaved && abs ((int)SourceOffset (earlySaved) - (int)serializedPosition) <= 1,
			"evicted ABSTIME channel serializes and restores its logical phase");
		StopAndDrain (renderer, earlySaved);
		ReleaseOwner (earlySaved);
		ReleaseOwner (earlyEvictor);
		renderer.UnloadSound (loopSound);
	}

	void TestLongClockRestartBounds (OpenALSoundRenderer &renderer)
	{
		std::vector<BYTE> samples = MakeSamples (8000);
		SoundHandle loopSound = renderer.LoadSoundRaw (&samples[0], (int)samples.size (), 8000, 1, -16, 100, 500);
		renderer.PausableOutputFrames = 1ull << 63;
		renderer.PausableFrameRemainder = 0;
		renderer.LastClockMilliseconds = TestMilliseconds;
		FISoundChannel *boundary = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		Check (boundary != NULL && boundary->StartTime.AsOne == (1ull << 63), "bit-63 logical clock remains an ordinary clock value");
		AdvanceTestClock (renderer, 137);
		FISoundChannel *evictor = renderer.StartSound (loopSound, 0.5f, 128, 81, SNDF_LOOP, NULL);
		unsigned long long elapsedOutputFrames = (unsigned long long)renderer.GetOutputRate () * 137 / 1000;
		unsigned int expected = ExpectedLoopPosition ((unsigned long long)((long double)elapsedOutputFrames * 8000 / renderer.GetOutputRate ()), 100, 500);
		unsigned int serializedPosition = renderer.GetPosition (boundary);
		Check (evictor != NULL && boundary->SysChannel == NULL && abs ((int)serializedPosition - (int)expected) <= 2,
			"bit-63 eviction retains the ordinary logical phase");
		StopAndDrain (renderer, evictor);
		FISoundChannel *restarted = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP, boundary);
		Check (restarted == boundary && abs ((int)SourceOffset (boundary) - (int)expected) <= 2,
			"bit-63 regular restart does not decode a negative tagged origin");
		StopAndDrain (renderer, boundary);
		ReleaseOwner (boundary);
		ReleaseOwner (evictor);
		renderer.UnloadSound (loopSound);
		renderer.PausableOutputFrames = 0;
		renderer.PausableFrameRemainder = 0;
		renderer.LastClockMilliseconds = TestMilliseconds;
	}

	void TestRestartConversionBounds (OpenALSoundRenderer &renderer)
	{
		std::vector<BYTE> samples = MakeSamples (8000);
		SoundHandle sound = renderer.LoadSoundRaw (&samples[0], (int)samples.size (), 8000, 1, -16, -1);
		OpenALSound *openalSound = (OpenALSound *)sound.data;
		openalSound->SampleRate = std::numeric_limits<unsigned int>::max ();
		renderer.PausableOutputFrames = std::numeric_limits<unsigned long long>::max ();
		renderer.PausableFrameRemainder = 0;
		renderer.LastClockMilliseconds = TestMilliseconds;
		FISoundChannel *incumbent = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *nonLoop = new FISoundChannel;
		int activeBefore = (int)renderer.ActiveChannels.size ();
		Check (renderer.StartSound (sound, 0.5f, std::numeric_limits<int>::max (), 80, 0, nonLoop) == NULL &&
			incumbent != NULL && incumbent->SysChannel != NULL && (int)renderer.ActiveChannels.size () == activeBefore,
			"out-of-range restart conversion rejects non-loop sounds before source eviction");
		StopAndDrain (renderer, incumbent);
		FISoundChannel *loop = new FISoundChannel;
		loop->StartTime.AsOne = 0;
		FISoundChannel *looped = renderer.StartSound (sound, 0.5f, std::numeric_limits<int>::max (), 80, SNDF_LOOP, loop);
		unsigned int expected = (unsigned int)(std::numeric_limits<unsigned long long>::max () % openalSound->Frames);
		Check (looped == loop && abs ((int)SourceOffset (loop) - (int)expected) <= 1,
			"clamped restart conversion preserves looping phase semantics");
		StopAndDrain (renderer, loop);
		ReleaseOwner (incumbent);
		ReleaseOwner (nonLoop);
		ReleaseOwner (loop);
		renderer.UnloadSound (sound);
		renderer.PausableOutputFrames = 0;
		renderer.PausableFrameRemainder = 0;
		renderer.LastClockMilliseconds = TestMilliseconds;
	}

	void TestRestartPositions (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		std::vector<BYTE> samples = MakeSamples (8000);
		SoundHandle loopSound = renderer.LoadSoundRaw (&samples[0], (int)samples.size (), 8000, 1, -16, 100, 500);
		AdvanceTestClock (renderer, 1000);
		FISoundChannel *saved = new FISoundChannel;
		saved->StartTime.AsOne = 321;
		FISoundChannel *restored = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP | SNDF_ABSTIME, saved);
		Check (restored == saved && abs ((int)SourceOffset (saved) - 321) <= 1, "looping SNDF_ABSTIME seeks the direct saved sample frame within one frame");
		AdvanceTestClock (renderer, 100);
		FISoundChannel *evictor = renderer.StartSound (loopSound, 0.5f, 128, 81, SNDF_LOOP, NULL);
		Check (evictor != NULL && saved->SysChannel == NULL && renderer.GetPosition (saved) > 0,
			"restored ABSTIME channel retains a serializable logical phase after eviction");
		StopAndDrain (renderer, evictor);
		FISoundChannel *restoredAgain = renderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP, saved);
		unsigned long long elapsed = (unsigned long long)renderer.GetOutputRate () * 100 / 1000;
		unsigned long long elapsedSamples = 321 + (unsigned long long)((long double)elapsed * 8000 / renderer.GetOutputRate ());
		unsigned int expected = ExpectedLoopPosition (elapsedSamples, 100, 500);
		Check (restoredAgain == saved && abs ((int)SourceOffset (saved) - (int)expected) <= 2, "re-evicted ABSTIME loop restarts through elapsed clock semantics");
		StopAndDrain (renderer, saved);
		ReleaseOwner (saved);
		ReleaseOwner (evictor);

		FISoundChannel *evicted = renderer.StartSound (loopSound, 0.5f, 192, 80, SNDF_LOOP, NULL);
		Check (evicted != NULL, "pitched loop restart fixture starts");
		AdvanceTestClock (renderer, 101);
		StopAndDrain (renderer, evicted);
		unsigned long long clock = renderer.PausableOutputFrames;
		elapsed = clock - evicted->StartTime.AsOne;
		elapsedSamples = (unsigned long long)((long double)elapsed * 8000 * 1.5f / renderer.GetOutputRate ());
		expected = elapsedSamples < 100 ? (unsigned int)elapsedSamples : 100 + (unsigned int)((elapsedSamples - 100) % 400);
		FISoundChannel *restarted = renderer.StartSound (loopSound, 0.5f, 192, 80, SNDF_LOOP, evicted);
		int tolerance = (8000 + 34) / 35 + 2;
		Check (restarted == evicted && abs ((int)SourceOffset (evicted) - (int)expected) <= tolerance, "ordinary pitched loop restart preserves intro then loop phase with pitch conversion");
		StopAndDrain (renderer, evicted);
		ReleaseOwner (evicted);

		int outputRate = (int)renderer.GetOutputRate ();
		std::vector<BYTE> clockSamples = MakeSamples (1000);
		SoundHandle clockLoop = renderer.LoadSoundRaw (&clockSamples[0], (int)clockSamples.size (), outputRate, 1, -16, 100, 500);
		AdvanceTestClock (renderer, 1000);
		unsigned int elapsedCases[] = { 100, 500, 1301 };
		unsigned int expectedCases[] = { 100, 100, 101 };
		for (unsigned int index = 0; index < sizeof (elapsedCases) / sizeof (elapsedCases[0]); ++index)
		{
			FISoundChannel *loopRestart = new FISoundChannel;
			loopRestart->StartTime.AsOne = renderer.PausableOutputFrames - elapsedCases[index];
			FISoundChannel *looped = renderer.StartSound (clockLoop, 0.5f, 128, 80, SNDF_LOOP, loopRestart);
			Check (looped == loopRestart && abs ((int)SourceOffset (loopRestart) - (int)expectedCases[index]) <= 1, "ordinary custom-loop restart handles loop boundaries and multiple loops");
			StopAndDrain (renderer, loopRestart);
			ReleaseOwner (loopRestart);
		}
		renderer.UnloadSound (clockLoop);

		FISoundChannel *expired = new FISoundChannel;
		AdvanceTestClock (renderer, 2000);
		expired->StartTime.AsOne = 0;
		Check (renderer.StartSound (sound, 0.5f, 128, 80, 0, expired) == NULL, "ordinary non-loop restart rejects elapsed sounds past their end");
		ReleaseOwner (expired);

		FISoundChannel *incumbent = renderer.StartSound (loopSound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *invalidRestart = new FISoundChannel;
		invalidRestart->StartTime.AsOne = 0;
		int activeBefore = (int)renderer.ActiveChannels.size ();
		int freeBefore = renderer.AllocatedSources - activeBefore;
		Check (renderer.StartSound (sound, 0.5f, 128, 80, 0, invalidRestart) == NULL && incumbent != NULL && incumbent->SysChannel != NULL &&
			(int)renderer.ActiveChannels.size () == activeBefore && renderer.AllocatedSources - (int)renderer.ActiveChannels.size () == freeBefore,
			"invalid full-pool restart preserves the active candidate and source accounting");
		StopAndDrain (renderer, incumbent);
		ReleaseOwner (incumbent);
		ReleaseOwner (invalidRestart);
		renderer.UnloadSound (loopSound);
	}

	void TestClockWrap (OpenALSoundRenderer &renderer)
	{
		unsigned long long pausableBefore = renderer.PausableOutputFrames;
		unsigned long long nonPausableBefore = renderer.NonPausableOutputFrames;
		renderer.PausableFrameRemainder = 0;
		renderer.NonPausableFrameRemainder = 0;
		renderer.LastClockMilliseconds = 0xfffffff0u;
		TestMilliseconds = 0x00000010u;
		renderer.UpdateSounds ();
		unsigned long long expectedFrames = (unsigned long long)renderer.GetOutputRate () * 32 / 1000;
		Check (renderer.PausableOutputFrames == pausableBefore + expectedFrames && renderer.NonPausableOutputFrames == nonPausableBefore + expectedFrames, "wrapping host milliseconds advances both logical clocks by the unsigned delta");
	}

	void TestEqualPriorityOrdering (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		FISoundChannel *quieter = renderer.StartSound (sound, 0.2f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *louder = renderer.StartSound (sound, 0.8f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *middle = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		Check (quieter != NULL && louder != NULL && middle != NULL, "equal-priority middle-gain incoming sound starts");
		Check (quieter != NULL && quieter->SysChannel == NULL && louder != NULL && louder->SysChannel != NULL, "equal priority evicts the lower-gain active source first");
		StopAndDrain (renderer, louder);
		StopAndDrain (renderer, middle);
		ReleaseOwner (quieter);
		ReleaseOwner (louder);
		ReleaseOwner (middle);

		FISoundChannel *oldest = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *newest = renderer.StartSound (sound, 0.5f, 128, 80, SNDF_LOOP, NULL);
		louder = renderer.StartSound (sound, 0.6f, 128, 80, SNDF_LOOP, NULL);
		Check (oldest != NULL && newest != NULL && louder != NULL, "equal-priority higher-gain incoming sound starts");
		Check (oldest != NULL && oldest->SysChannel == NULL && newest != NULL && newest->SysChannel != NULL, "equal priority selects oldest allocation serial after gain tie");
		StopAndDrain (renderer, newest);
		StopAndDrain (renderer, louder);
		ReleaseOwner (oldest);
		ReleaseOwner (newest);
		ReleaseOwner (louder);
	}

	void TestPriorityOrdering (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		size_t eventCount = Events.size ();
		FISoundChannel *firstLow = renderer.StartSound (sound, 0.9f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *secondLow = renderer.StartSound (sound, 0.8f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *high = renderer.StartSound (sound, 0.01f, 128, 80, SNDF_LOOP, NULL);
		Check (renderer.AllocatedSources >= 2, "priority fixture allocates two physical sources");
		Check (firstLow != NULL && secondLow != NULL && high != NULL, "priority 80 starts through the production 2D path");
		Check (secondLow != NULL && secondLow->SysChannel == NULL, "priority 80 evicts lower priority regardless of gain");
		Check (IsExpectedEvent (eventCount, OALEND_PoolEviction), "priority eviction reports PoolEviction exactly once");
		StopAndDrain (renderer, firstLow);
		StopAndDrain (renderer, high);
		ReleaseOwner (firstLow);
		ReleaseOwner (secondLow);
		ReleaseOwner (high);

		FISoundChannel *firstHigh = renderer.StartSound (sound, 0.01f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *secondHigh = renderer.StartSound (sound, 0.02f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *rejected = renderer.StartSound (sound, 1.f, 128, 0, SNDF_LOOP, NULL);
		Check (firstHigh != NULL && secondHigh != NULL && rejected == NULL, "lower priority incoming sound cannot evict higher priority active sounds");
		Check (firstHigh != NULL && firstHigh->SysChannel != NULL && secondHigh != NULL && secondHigh->SysChannel != NULL, "higher priority active sources remain owned despite lower-priority gain");
		StopAndDrain (renderer, firstHigh);
		StopAndDrain (renderer, secondHigh);
		ReleaseOwner (firstHigh);
		ReleaseOwner (secondHigh);

		TestEqualPriorityOrdering (renderer, sound);

		FISoundChannel *restartVictim = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *restartBlocker = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *restartOwner = new FISoundChannel;
		restartOwner->Priority = 80;
		FISoundChannel *restarted = renderer.StartSound (sound, 0.1f, 128, restartOwner->Priority, SNDF_LOOP, restartOwner);
		Check (restartVictim != NULL && restartBlocker != NULL && restarted == restartOwner, "restart reuses its high-level owner through StartSound");
		Check ((restartVictim != NULL && restartVictim->SysChannel == NULL) || (restartBlocker != NULL && restartBlocker->SysChannel == NULL), "restart forwards preserved priority to eviction");
		StopAndDrain (renderer, restartVictim);
		StopAndDrain (renderer, restartBlocker);
		StopAndDrain (renderer, restartOwner);
		Check (renderer.ActiveChannels.empty () && renderer.AllocatedSources - (int)renderer.ActiveChannels.size () == renderer.AllocatedSources, "priority sequences recover source accounting baseline");
		ReleaseOwner (restartVictim);
		ReleaseOwner (restartBlocker);
		ReleaseOwner (restartOwner);
	}

	void TestEffectiveGainOrdering (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		SoundListener listener;
		FRolloffInfo rolloff = MakeLinearRolloff (0.f, 100.f);
		FVector3 position (90.f, 0.f, 0.f);
		FVector3 velocity;
		listener.position = FVector3 (0.f, 0.f, 0.f);
		listener.valid = true;
		renderer.SetSfxVolume (0.5f);
		FISoundChannel *spatial = renderer.StartSound3D (sound, &listener, 0.8f, &rolloff, 1.f, 128, 80, position, velocity, 0, SNDF_LOOP, NULL);
		FISoundChannel *blocker = renderer.StartSound (sound, 0.9f, 128, 80, SNDF_LOOP, NULL);
		FISoundChannel *incoming = renderer.StartSound (sound, 0.2f, 128, 80, SNDF_LOOP, NULL);
		Check (spatial != NULL && blocker != NULL && incoming != NULL, "equal-priority incoming compares against 3D effective gain");
		Check (spatial != NULL && spatial->SysChannel == NULL, "3D lower effective gain loses despite higher raw gain");
		StopAndDrain (renderer, blocker);
		StopAndDrain (renderer, incoming);
		ReleaseOwner (spatial);
		ReleaseOwner (blocker);
		ReleaseOwner (incoming);
		renderer.SetSfxVolume (1.f);
	}

	void Test3DState (OpenALSoundRenderer &renderer, SoundHandle stereoSound)
	{
		SoundListener listener;
		FRolloffInfo rolloff = MakeLinearRolloff (0.f, 100.f);
		FVector3 sourcePosition (50.f, 7.f, 9.f);
		FVector3 sourceVelocity (1.f, 2.f, 3.f);
		ALfloat values[6];
		ALint relative;
		ALint buffer;
		ALfloat gain;
		OpenALSound *sound = (OpenALSound *)stereoSound.data;
		listener.position = FVector3 (10.f, 20.f, 30.f);
		listener.velocity = FVector3 (4.f, 5.f, 6.f);
		listener.angle = 1.57079632679f;
		listener.valid = true;
		renderer.UpdateListener (&listener);
		alGetListenerfv (AL_POSITION, values);
		CheckVector (values, 10.f, 20.f, -30.f, "listener position uses OpenAL handedness");
		alGetListenerfv (AL_VELOCITY, values);
		CheckVector (values, 0.f, 0.f, 0.f, "listener velocity remains zero");
		alGetListenerfv (AL_ORIENTATION, values);
		CheckVector (values, 0.f, 0.f, -1.f, "listener forward uses OpenAL handedness");
		CheckVector (values + 3, 0.f, 1.f, 0.f, "listener up remains world up");

		FISoundChannel *channel = renderer.StartSound3D (stereoSound, &listener, 0.8f, &rolloff, 2.f, 128, 0, sourcePosition, sourceVelocity, 0, SNDF_LOOP | SNDF_AREA, NULL);
		Check (channel != NULL, "stereo 3D sound starts on an actual OpenAL source");
		if (channel != NULL)
		{
			OpenALChannel *openalChannel = (OpenALChannel *)channel->SysChannel;
			alGetSourcei (openalChannel->Source, AL_BUFFER, &buffer);
			Check (sound->BufferMono != 0 && (unsigned int)buffer == sound->BufferMono && sound->BufferMono != sound->Buffer2D, "stereo 3D sound selects its mono spatial buffer");
			alGetSourcei (openalChannel->Source, AL_SOURCE_RELATIVE, &relative);
			Check (relative == AL_FALSE, "non-coincident 3D source remains world relative");
			alGetSourcefv (openalChannel->Source, AL_POSITION, values);
			CheckVector (values, 50.f, 7.f, -9.f, "source position uses OpenAL handedness");
			alGetSourcefv (openalChannel->Source, AL_VELOCITY, values);
			CheckVector (values, 1.f, 2.f, -3.f, "source velocity uses OpenAL handedness");
			alGetSourcef (openalChannel->Source, AL_GAIN, &gain);
			Check (NearlyEqual (gain, 0.8f * (1.f - (sqrtf (2210.f) * 2.f) / 100.f)), "manual rolloff applies distance scale to AL gain");

			renderer.UpdateSoundParams3D (&listener, channel, false, listener.position, FVector3 ());
			alGetSourcei (openalChannel->Source, AL_SOURCE_RELATIVE, &relative);
			alGetSourcefv (openalChannel->Source, AL_POSITION, values);
			Check (relative == AL_TRUE, "listener-coincident source is head relative");
			CheckVector (values, 0.f, 0.f, 0.f, "listener-coincident source uses relative origin");

			renderer.UpdateSoundParams3D (&listener, channel, true, FVector3 (42.f, 20.f, 30.f), FVector3 ());
			alGetSourcei (openalChannel->Source, AL_SOURCE_RELATIVE, &relative);
			Check (relative == AL_TRUE, "area sound inside 32 units remains centered");
			renderer.UpdateSoundParams3D (&listener, channel, true, FVector3 (43.f, 20.f, 30.f), FVector3 ());
			alGetSourcei (openalChannel->Source, AL_SOURCE_RELATIVE, &relative);
			alGetSourcefv (openalChannel->Source, AL_POSITION, values);
			Check (relative == AL_FALSE, "area sound outside 32 units becomes positional");
			CheckVector (values, 43.f, 20.f, -30.f, "area sound outside 32 units uses world position");
			StopAndDrain (renderer, channel);
		}
		ReleaseOwner (channel);
	}

	void TestEviction (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		size_t eventCount = Events.size ();
		FISoundChannel *evicted = renderer.StartSound (sound, 0.1f, 128, 0, SNDF_LOOP, NULL);
		FISoundChannel *replacement = new FISoundChannel;
		Check (evicted != NULL, "lower-priority sound starts before pool exhaustion");
		replacement = renderer.StartSound (sound, 0.9f, 128, 80, SNDF_LOOP, replacement);
		Check (replacement != NULL, "higher-priority reused channel starts after pool exhaustion");
		Check (IsExpectedEvent (eventCount, OALEND_PoolEviction), "pool eviction identifies PoolEviction exactly once");
		Check (evicted != NULL && evicted->SysChannel == NULL, "pool eviction clears high-level ownership");
		Check (renderer.ActiveChannels.size () == 1 && renderer.AllocatedSources - (int)renderer.ActiveChannels.size () == 0, "replacement owns the only source");
		ReleaseOwner (evicted);

		renderer.StopChannel (replacement);
		Check (Events.size () == eventCount + 1 && renderer.ActiveChannels.size () == 1, "explicit stop defers callback until update");
		renderer.UpdateSounds ();
		Check (IsExpectedEvent (eventCount + 1, OALEND_ExplicitStop), "replacement cleanup identifies ExplicitStop exactly once");
		Check (replacement->SysChannel == NULL && renderer.ActiveChannels.empty () && renderer.AllocatedSources == 1, "eviction sequence recovers active=0/free=1");
		ReleaseOwner (replacement);
	}

	void TestExplicitStop (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		size_t eventCount = Events.size ();
		FISoundChannel *explicitStop = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		Check (explicitStop != NULL, "explicit-stop sound starts");
		renderer.StopChannel (explicitStop);
		Check (Events.size () == eventCount && renderer.ActiveChannels.size () == 1, "explicit stop remains pending before update");
		renderer.UpdateSounds ();
		Check (IsExpectedEvent (eventCount, OALEND_ExplicitStop), "explicit stop finalizes as ExplicitStop exactly once");
		Check (explicitStop->SysChannel == NULL && renderer.ActiveChannels.empty () && renderer.AllocatedSources == 1, "explicit stop recovers active=0/free=1");
		ReleaseOwner (explicitStop);
	}

	void TestNaturalFinish (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		size_t eventCount = Events.size ();
		FISoundChannel *natural = renderer.StartSound (sound, 0.5f, 128, 0, 0, NULL);
		Check (natural != NULL, "natural-finish sound starts");
		for (int attempt = 0; attempt < 100 && !renderer.ActiveChannels.empty (); ++attempt)
		{
			std::this_thread::sleep_for (std::chrono::milliseconds (10));
			renderer.UpdateSounds ();
		}
		Check (IsExpectedEvent (eventCount, OALEND_Natural), "natural finish identifies Natural exactly once");
		Check (natural->SysChannel == NULL && renderer.ActiveChannels.empty () && renderer.AllocatedSources == 1, "natural finish recovers active=0/free=1");
		ReleaseOwner (natural);
	}

	void TestImmediateReuseAfterExplicitStop (OpenALSoundRenderer &renderer, SoundHandle loopingSound, SoundHandle finishingSound)
	{
		size_t eventCount = Events.size ();
		FISoundChannel *stopped = renderer.StartSound (loopingSound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		Check (stopped != NULL, "looping sound starts before immediate reuse");
		renderer.StopChannel (stopped);
		FISoundChannel *replacement = renderer.StartSound (finishingSound, 0.5f, 128, 0, 0, NULL);
		Check (replacement != NULL, "same-priority sound starts immediately after explicit stop");
		Check (Events.size () == eventCount + 1 && IsExpectedEvent (eventCount, OALEND_ExplicitStop), "immediate reuse finalizes old sound as ExplicitStop exactly once");
		Check (stopped->SysChannel == NULL, "immediate reuse clears stopped sound ownership");
		for (int attempt = 0; attempt < 100 && !renderer.ActiveChannels.empty (); ++attempt)
		{
			std::this_thread::sleep_for (std::chrono::milliseconds (10));
			renderer.UpdateSounds ();
		}
		Check (Events.size () == eventCount + 2 && IsExpectedEvent (eventCount + 1, OALEND_Natural), "immediate replacement natural-finish identifies Natural exactly once");
		Check (replacement->SysChannel == NULL && renderer.ActiveChannels.empty () && renderer.AllocatedSources == 1, "immediate reuse sequence recovers active=0/free=1");
		ReleaseOwner (stopped);
		ReleaseOwner (replacement);
	}

	void TestEvictedOwnerReuseAfterFailedStart (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		FISoundChannel *evicted = renderer.StartSound (sound, 0.1f, 128, 0, SNDF_LOOP, NULL);
		AdvanceTestClock (renderer, 100);
		FISoundChannel *evictor = renderer.StartSound (sound, 0.9f, 128, 80, SNDF_LOOP, NULL);
		unsigned int savedPosition = renderer.GetPosition (evicted);
		Check (evicted != NULL && evictor != NULL && evicted->SysChannel == NULL && savedPosition > 0,
			"evicted looping owner retains a nonzero logical phase for saving");
		StopAndDrain (renderer, evictor);
		FISoundChannel *restored = renderer.StartSound (sound, 0.1f, 128, 0, SNDF_LOOP, evicted);
		Check (restored == evicted && abs ((int)SourceOffset (restored) - (int)savedPosition) <= 2,
			"valid evicted owner restarts at its saved logical phase");

		FISoundChannel *replacement = renderer.StartSound (sound, 0.9f, 128, 80, SNDF_LOOP, NULL);
		Check (replacement != NULL && evicted->SysChannel == NULL, "restart owner can be evicted again before logical replacement");
		ReleaseOwner (evictor);
		ReleaseOwnerForReuse (evicted);
		renderer.InjectStartFailureForTest ();
		Check (renderer.StartSound (sound, 0.5f, 128, 81, SNDF_LOOP, NULL) == NULL,
			"injected physical start failure reaches high-level looping fallback");
		Check (alGetError () == AL_NO_ERROR, "injected looping start failure leaves no OpenAL validation error");
		FISoundChannel *reused = S_GetChannel (NULL);
		Check (reused == evicted && reused->SysChannel == NULL && renderer.GetPosition (reused) == 0,
			"deserialize-style owner reuse cannot serialize the old logical phase before MarkStartTime");
		reused->StartTime.AsOne = 321;
		renderer.InjectStartFailureForTest ();
		Check (renderer.StartSound (sound, 0.5f, 128, 81, SNDF_LOOP | SNDF_ABSTIME, reused) == NULL &&
			reused->SysChannel == NULL && renderer.GetPosition (reused) == 0,
			"failed deserialize-style ABSTIME restart keeps the old logical phase hidden before retry");
		Check (alGetError () == AL_NO_ERROR, "injected ABSTIME restart failure leaves no OpenAL validation error");
		FISoundChannel *retried = renderer.StartSound (sound, 0.5f, 128, 81, SNDF_LOOP | SNDF_ABSTIME, reused);
		Check (retried == reused && abs ((int)SourceOffset (retried) - 321) <= 1 &&
			abs ((int)renderer.GetPosition (retried) - 321) <= 1,
			"retried deserialize-style ABSTIME restart uses its direct saved position");
		StopAndDrain (renderer, reused);
		ReleaseOwner (reused);
		ReleaseOwner (replacement);
	}
}
	void TestFailedStartDoesNotPublish (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
		size_t eventCount = Events.size ();
		int freeSources = renderer.AllocatedSources - (int)renderer.ActiveChannels.size ();
		OpenALSound *openalSound = (OpenALSound *)sound.data;
		renderer.InjectStartFailureForTest ();
		FISoundChannel *failed = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP, NULL);
		Check (failed == NULL, "injected OpenAL start failure returns null");
		Check (alGetError () == AL_NO_ERROR, "injected start failure leaves no OpenAL validation error");
		Check (Events.size () == eventCount, "injected OpenAL start failure does not notify high-level channel end");
		Check (renderer.ActiveChannels.empty () && renderer.AllocatedSources - (int)renderer.ActiveChannels.size () == freeSources, "injected OpenAL start failure recovers source baseline");
		Check (openalSound->References == 0, "injected OpenAL start failure does not retain sound reference");
		renderer.SetSfxPaused (true, 1);
		AdvanceTestClock (renderer, 100);
		renderer.InjectStartFailureForTest ();
		FISoundChannel *failedNoPause = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP | SNDF_NOPAUSE, NULL);
		Check (alGetError () == AL_NO_ERROR, "injected NOPAUSE start failure leaves no OpenAL validation error");
		FISoundChannel *evictedLoop = new FISoundChannel;
		renderer.MarkStartTime (evictedLoop);
		Check (failedNoPause == NULL && evictedLoop->StartTime.AsOne == renderer.NonPausableOutputFrames, "failed NOPAUSE start records the immediate nonpausable clock class");
		renderer.SetSfxPaused (false, 1);
		ReleaseOwner (evictedLoop);
	}

	static bool TestDelayedLoopHandoff (std::vector<BYTE> &loopSamples)
	{
		FISoundChannel *loopOwner = NULL;
		unsigned int loopCursor = 0;
		{
			OpenALSoundRenderer oldRenderer;
			Check (oldRenderer.IsValid (), "old renderer initializes for cross-renderer reset handoff");
			if (!oldRenderer.IsValid ())
			{
				return false;
			}
			SoundHandle loopSound = oldRenderer.LoadSoundRaw (&loopSamples[0], (int)loopSamples.size (), 8000, 1, -16, 100, 500);
			loopOwner = oldRenderer.StartSound (loopSound, 0.5f, 192, 0, SNDF_LOOP, NULL);
			TestMilliseconds += 137;
			FISoundChannel *loopEvictor = oldRenderer.StartSound (loopSound, 0.5f, 128, 80, SNDF_LOOP, NULL);
			Check (loopOwner != NULL && loopEvictor != NULL && loopOwner->SysChannel == NULL,
				"pool eviction leaves the pitched loop resolver owner detached");
			TestMilliseconds += 25;
			unsigned long long delayedOutputFrames = (unsigned long long)oldRenderer.GetOutputRate () * 162 / 1000;
			unsigned long long delayedSampleFrames = delayedOutputFrames * 8000 * 3 / (2 * (unsigned int)oldRenderer.GetOutputRate ());
			unsigned int expectedLoopCursor = ExpectedLoopPosition (delayedSampleFrames, 100, 500);
			Check (oldRenderer.ResolveEvictedPosition (loopOwner, &loopCursor) && abs ((int)loopCursor - (int)expectedLoopCursor) <= 2,
				"old renderer resolves the delayed pitched custom-loop cursor");
			StopAndDrain (oldRenderer, loopEvictor);
			ReleaseOwner (loopEvictor);
			oldRenderer.UnloadSound (loopSound);
		}
		if (loopOwner != NULL)
		{
			OpenALSoundRenderer freshRenderer;
			Check (freshRenderer.IsValid (), "fresh renderer initializes for cross-renderer loop restore");
			if (freshRenderer.IsValid ())
			{
				SoundHandle loopSound = freshRenderer.LoadSoundRaw (&loopSamples[0], (int)loopSamples.size (), 8000, 1, -16, 100, 500);
				loopOwner->StartTime.AsOne = loopCursor;
				FISoundChannel *restored = freshRenderer.StartSound (loopSound, 0.5f, 192, 80, SNDF_LOOP | SNDF_ABSTIME, loopOwner);
				Check (restored == loopOwner && abs ((int)SourceOffset (loopOwner) - (int)loopCursor) <= 2,
					"fresh renderer restores the durable loop cursor within two source frames");
				StopAndDrain (freshRenderer, loopOwner);
				freshRenderer.UnloadSound (loopSound);
			}
			ReleaseOwner (loopOwner);
		}
		return true;
	}

	static void TestDelayedOneShotHandoff (std::vector<BYTE> &oneShotSamples)
	{
		FISoundChannel *oneShotOwner = NULL;
		unsigned int expiredPosition = 0;
		{
			OpenALSoundRenderer oldRenderer;
			Check (oldRenderer.IsValid (), "old renderer initializes for expired one-shot handoff");
			if (!oldRenderer.IsValid ())
			{
				return;
			}
			SoundHandle oneShotSound = oldRenderer.LoadSoundRaw (&oneShotSamples[0], (int)oneShotSamples.size (), 8000, 1, -16, -1);
			oneShotOwner = oldRenderer.StartSound (oneShotSound, 0.5f, 128, 0, 0, NULL);
			TestMilliseconds += 10;
			FISoundChannel *oneShotEvictor = oldRenderer.StartSound (oneShotSound, 0.5f, 128, 80, SNDF_LOOP, NULL);
			Check (oneShotOwner != NULL && oneShotEvictor != NULL && oneShotOwner->SysChannel == NULL,
				"pool eviction leaves the expired one-shot resolver owner detached");
			TestMilliseconds += 20;
			Check (oldRenderer.ResolveEvictedPosition (oneShotOwner, &expiredPosition) && expiredPosition == 160,
				"old renderer resolves the delayed expired one-shot sample-frame sentinel");
			StopAndDrain (oldRenderer, oneShotEvictor);
			ReleaseOwner (oneShotEvictor);
			oldRenderer.UnloadSound (oneShotSound);
		}
		if (oneShotOwner != NULL)
		{
			OpenALSoundRenderer freshRenderer;
			Check (freshRenderer.IsValid (), "fresh renderer initializes for expired one-shot restore");
			if (freshRenderer.IsValid ())
			{
				SoundHandle oneShotSound = freshRenderer.LoadSoundRaw (&oneShotSamples[0], (int)oneShotSamples.size (), 8000, 1, -16, -1);
				oneShotOwner->StartTime.AsOne = expiredPosition;
				Check (freshRenderer.StartSound (oneShotSound, 0.5f, 128, 80, SNDF_ABSTIME, oneShotOwner) == NULL,
					"fresh renderer rejects the expired one-shot sample-frame sentinel");
				freshRenderer.UnloadSound (oneShotSound);
			}
			ReleaseOwner (oneShotOwner);
		}
	}

	void TestCrossRendererResetHandoff ()
	{
		std::vector<BYTE> loopSamples = MakeSamples (8000);
		std::vector<BYTE> oneShotSamples = MakeSamples (160);
		if (!TestDelayedLoopHandoff (loopSamples))
		{
			return;
		}
		TestDelayedOneShotHandoff (oneShotSamples);
	}


void Printf (const char *, ...)
{
}

void DPrintf (const char *, ...)
{
}

float S_GetRolloff (FRolloffInfo *rolloff, float distance, bool)
{
	if (rolloff == NULL || distance >= rolloff->MaxDistance)
	{
		return 0.f;
	}
	if (distance <= rolloff->MinDistance)
	{
		return 1.f;
	}
	return (rolloff->MaxDistance - distance) / (rolloff->MaxDistance - rolloff->MinDistance);
}

FISoundChannel *S_GetChannel (void *syschan)
{
	FISoundChannel *channel = ForcedNextOwner;
	ForcedNextOwner = NULL;
	if (channel == NULL)
	{
		channel = new FISoundChannel;
	}
	channel->SysChannel = syschan;
	return channel;
}

void S_ChannelEnded (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL)
	{
		Events.push_back (CallbackEvent (channel->EndReason, owner->SysChannel == channel,
			channel->FinalizeState == OALFINAL_Finalizing));
		owner->SysChannel = NULL;
	}
}

static int RunPriorityRendererTests (std::vector<BYTE> &longSamples)
{
	OpenALSoundRenderer priorityRenderer;
	if (!priorityRenderer.IsValid ())
	{
		if (priorityRenderer.Device == NULL && priorityRenderer.Context == NULL)
		{
			fprintf (stderr, "SKIP: OpenAL device/context could not initialize\n");
			return 77;
		}
		fprintf (stderr, "FAILED: OpenAL renderer could not initialize after context creation\n");
		return 1;
	}
	SoundHandle prioritySound = priorityRenderer.LoadSoundRaw (&longSamples[0], (int)longSamples.size (), 8000, 1, -16, -1);
	if (prioritySound.data == NULL)
	{
		fprintf (stderr, "FAILED: OpenAL priority PCM buffer could not initialize\n");
		return 1;
	}
	TestPriorityOrdering (priorityRenderer, prioritySound);
	TestEffectiveGainOrdering (priorityRenderer, prioritySound);
	TestPauseReasonsAndClocks (priorityRenderer, prioritySound);
	priorityRenderer.UnloadSound (prioritySound);
	return 0;
}

static void TestPhase2ContextInitialization ()
{
	OpenALContextTestResult extensionAbsent = OALTestRunContextInitialization (false, true, OALCONTEXTTEST_NoFailure);
	Check (extensionAbsent.Success && !extensionAbsent.AttributesApplied && extensionAbsent.OpenCount == 1 &&
		extensionAbsent.CreateCount == 1 && !extensionAbsent.FirstAttributesWereHRTF,
		"phase2 absent HRTF extension keeps normal OpenAL context initialization");
	OpenALContextTestResult hrtfRejected = OALTestRunContextInitialization (true, true, OALCONTEXTTEST_FirstCreateFailure);
	Check (hrtfRejected.Success && !hrtfRejected.AttributesApplied && hrtfRejected.OpenCount == 2 && hrtfRejected.CreateCount == 2 &&
		hrtfRejected.DestroyCount == 1 && hrtfRejected.CloseCount == 2 && hrtfRejected.HRTFExtensionQueriedOnDevice &&
		hrtfRejected.FirstAttributesWereHRTF && hrtfRejected.SecondAttributesWereNull &&
		hrtfRejected.HRTFFailure == OALHRTFCONTEXT_CreateFailed,
		"phase2 rejected HRTF context closes the first device and retries once without attributes");
	Check (OALTestRunContextInitialization (true, true, OALCONTEXTTEST_NoFailure).HRTFSpecifierParameter == 0x1995,
		"phase2 HRTF specifier query uses ALC_HRTF_SPECIFIER_SOFT");
	OpenALContextTestResult hrtfMakeCurrentFailure = OALTestRunContextInitialization (true, true, OALCONTEXTTEST_FirstMakeCurrentFailure);
	Check (hrtfMakeCurrentFailure.Success && hrtfMakeCurrentFailure.DestroyCount == 2 && hrtfMakeCurrentFailure.CloseCount == 2 &&
		hrtfMakeCurrentFailure.SecondAttributesWereNull,
		"phase2 HRTF make-current failure destroys its context before the attribute-free retry");
}

static FVector3 MakePhase2BackendPosition (float gameX, float gameY, float gameZ)
{
	return FVector3 (gameX, gameZ, gameY);
}

struct Phase2DirectionCase
{
	const char *ID;
	const char *ExpectedDirection;
	float GameX;
	float GameY;
	float GameZ;
};

static void TestPhase2ManualDirectionMapping ()
{
	FVector3 left = MakePhase2BackendPosition (0.f, 64.f, 0.f);
	FVector3 right = MakePhase2BackendPosition (0.f, -64.f, 0.f);
	FVector3 front = MakePhase2BackendPosition (64.f, 0.f, 0.f);
	FVector3 back = MakePhase2BackendPosition (-64.f, 0.f, 0.f);
	FVector3 up = MakePhase2BackendPosition (0.f, 0.f, 64.f);
	FVector3 down = MakePhase2BackendPosition (0.f, 0.f, -64.f);
	Check (left.X == 0.f && left.Y == 0.f && left.Z == 64.f && right.Z == -64.f &&
		front.X == 64.f && back.X == -64.f && up.Y == 64.f && down.Y == -64.f,
		"phase2 manual direction mapping preserves game x,z,y listener coordinates");
}

static void TestPhase2ContextFailureCleanup ()
{
	OpenALContextTestResult basicFailure = OALTestRunContextInitialization (false, true, OALCONTEXTTEST_FirstCreateFailure);
	Check (!basicFailure.Success && basicFailure.OpenCount == 1 && basicFailure.CreateCount == 1 && basicFailure.DestroyCount == 0 &&
		basicFailure.CloseCount == 1 && !basicFailure.FirstAttributesWereHRTF,
		"phase2 basic OpenAL failure closes once without an HRTF retry");
	OpenALContextTestResult retryFailure = OALTestRunContextInitialization (true, true, OALCONTEXTTEST_BothMakeCurrentFailures);
	Check (!retryFailure.Success, "phase2 failed attribute-free retry returns failure");
	Check (retryFailure.OpenCount == 2 && retryFailure.CreateCount == 2 && retryFailure.MakeCurrentCount == 2,
		"phase2 failed attribute-free retry stops after one retry");
	Check (retryFailure.DestroyCount == 2 && retryFailure.CloseCount == 2,
		"phase2 failed attribute-free retry cleans both contexts");
}

static void TestPhase2HRTFCapabilities ()
{
	int attributes[3] = { 0, 0, 0 };
	Check (!OALBuildHRTFContextAttributes (false, true, attributes),
		"phase2 HRTF attributes are absent when the extension is unavailable");
	Check (OALBuildHRTFContextAttributes (true, true, attributes) && attributes[0] == 0x1992 && attributes[1] == 1 && attributes[2] == 0,
		"phase2 HRTF startup request explicitly enables advertised HRTF");
	Check (OALBuildHRTFContextAttributes (true, false, attributes) && attributes[1] == 0,
		"phase2 HRTF startup request explicitly disables advertised HRTF");
	OpenALEFXFunctions missing;
	OpenALCapabilities absent = OALBuildCapabilities (false, false, false, false, 0, false, missing, false);
	Check (!absent.HRTFAdvertised && !absent.EFXAdvertised && !absent.EFXCallable && !absent.RadiusAdvertised,
		"phase2 absent extensions remain unavailable");
	OpenALCapabilities denied = OALBuildCapabilities (true, true, false, true, 0x0002, false, missing, false);
	OpenALCapabilities unsupported = OALBuildCapabilities (true, true, false, true, 0x0005, false, missing, false);
	Check (denied.HRTFAdvertised && denied.HRTFStatusKnown && !denied.HRTFActive &&
		unsupported.HRTFAdvertised && unsupported.HRTFStatusKnown && !unsupported.HRTFActive,
		"phase2 rejected or unsupported HRTF status remains an active renderer capability, not HRTF activation");
	OpenALCapabilities specifier;
	Check (OALTestCopyHRTFSpecifier ("Test HRTF", true, &specifier) && strcmp (specifier.HRTFSpecifier.GetChars (), "Test HRTF") == 0 &&
		!OALTestCopyHRTFSpecifier (NULL, true, &specifier) && !OALTestCopyHRTFSpecifier ("ignored", false, &specifier) &&
		strcmp (specifier.HRTFSpecifier.GetChars (), "Test HRTF") == 0,
		"phase2 HRTF specifier is copied only after a successful non-null query");
	OpenALCapabilities unknown = OALBuildCapabilities (true, true, true, true, 777, false, missing, false);
	Check (unknown.HRTFStatusKnown && unknown.HRTFStatus == 777 && unknown.HRTFActive,
		"phase2 unknown HRTF numerical status is retained");
}

static void TestPhase2HRTFActualQuery ()
{
	OpenALHRTFQueryTestResult required = OALTestQueryHRTFCapabilities (true, true, true, true, 0x0003);
	OpenALHRTFQueryTestResult headphones = OALTestQueryHRTFCapabilities (true, true, true, true, 0x0004);
	OpenALHRTFQueryTestResult inactive = OALTestQueryHRTFCapabilities (true, true, false, true, 0x0001);
	Check (required.Capabilities.HRTFActiveKnown && required.Capabilities.HRTFActive && required.Capabilities.HRTFStatus == 0x0003 &&
		headphones.Capabilities.HRTFActiveKnown && headphones.Capabilities.HRTFActive && headphones.Capabilities.HRTFStatus == 0x0004 &&
		inactive.Capabilities.HRTFActiveKnown && !inactive.Capabilities.HRTFActive && inactive.Capabilities.HRTFStatus == 0x0001,
		"phase2 HRTF activity is independent from the status reason");
	OpenALHRTFQueryTestResult activeError = OALTestQueryHRTFCapabilities (true, false, true, true, 777);
	OpenALHRTFQueryTestResult statusError = OALTestQueryHRTFCapabilities (true, true, false, false, 777);
	OpenALHRTFQueryTestResult extensionAbsent = OALTestQueryHRTFCapabilities (false, true, true, true, 777);
	Check (!activeError.Capabilities.HRTFActiveKnown && activeError.Capabilities.HRTFStatusKnown && activeError.Capabilities.HRTFStatus == 777 &&
		statusError.Capabilities.HRTFActiveKnown && !statusError.Capabilities.HRTFActive && !statusError.Capabilities.HRTFStatusKnown &&
		extensionAbsent.ActiveQueryCount == 0 && extensionAbsent.StatusQueryCount == 0,
		"phase2 HRTF activity and status query errors remain separate and absent extensions are not queried");
}


static void TestPhase2EFXCapabilities ()
{
	OpenALEFXFunctions partial;
	partial.GenEffects = reinterpret_cast<OALGenEffects> (static_cast<uintptr_t> (1));
	OpenALCapabilities missingPointer = OALBuildCapabilities (false, false, false, false, 0, true, partial, false);
	Check (missingPointer.EFXAdvertised && !missingPointer.EFXCallable && !missingPointer.EFXUsable,
		"phase2 missing EFX pointer is not callable or usable");

	OpenALEFXFunctions callable;
	callable.GenEffects = reinterpret_cast<OALGenEffects> (static_cast<uintptr_t> (1));
	callable.DeleteEffects = reinterpret_cast<OALDeleteEffects> (static_cast<uintptr_t> (1));
	callable.Effecti = reinterpret_cast<OALEffecti> (static_cast<uintptr_t> (1));
	callable.Effectf = reinterpret_cast<OALEffectf> (static_cast<uintptr_t> (1));
	callable.Effectfv = reinterpret_cast<OALEffectfv> (static_cast<uintptr_t> (1));
	callable.GenAuxiliaryEffectSlots = reinterpret_cast<OALGenAuxiliaryEffectSlots> (static_cast<uintptr_t> (1));
	callable.DeleteAuxiliaryEffectSlots = reinterpret_cast<OALDeleteAuxiliaryEffectSlots> (static_cast<uintptr_t> (1));
	callable.AuxiliaryEffectSloti = reinterpret_cast<OALAuxiliaryEffectSloti> (static_cast<uintptr_t> (1));
	callable.AuxiliaryEffectSlotf = reinterpret_cast<OALAuxiliaryEffectSlotf> (static_cast<uintptr_t> (1));
	callable.GenFilters = reinterpret_cast<OALGenFilters> (static_cast<uintptr_t> (1));
	callable.DeleteFilters = reinterpret_cast<OALDeleteFilters> (static_cast<uintptr_t> (1));
	callable.Filteri = reinterpret_cast<OALFilteri> (static_cast<uintptr_t> (1));
	callable.Filterf = reinterpret_cast<OALFilterf> (static_cast<uintptr_t> (1));
	OpenALCapabilities known = OALBuildCapabilities (true, true, true, true, 1, true, callable, true);
	Check (known.HRTFAdvertised && known.HRTFActiveKnown && known.HRTFStatusKnown && known.HRTFStatus == 1 && known.HRTFActive && known.EFXCallable && !known.EFXUsable &&
		known.EFXSendCount < 0 && known.RadiusAdvertised && !known.RadiusApplied && !known.DopplerApplied,
		"phase2 known status separates callable from applied state");
}

static void TestPhase2Capabilities ()
{
	TestPhase2ContextInitialization ();
	TestPhase2ManualDirectionMapping ();
	TestPhase2ContextFailureCleanup ();
	TestPhase2HRTFCapabilities ();
	TestPhase2HRTFActualQuery ();
	TestPhase2EFXCapabilities ();
}

static void TestPhase2RuntimeHRTFRequest (OpenALSoundRenderer &renderer)
{
	bool requestedAtInit = renderer.HRTFRequestedEnabled;
	snd_hrtf.Value = !requestedAtInit;
	Check (renderer.HRTFRequestedEnabled == requestedAtInit,
		"phase2 current HRTF cvar remains pending until renderer reinitialization");
	snd_hrtf.Value = requestedAtInit;
}

static bool IsKnownOperation (const char *operation)
{
	return operation == NULL || strcmp (operation, "--phase2-unit-only") == 0 ||
		strcmp (operation, "--phase2-status") == 0 || strcmp (operation, "--phase1b-direct-memory") == 0 ||
		strcmp (operation, "--phase1b-file-slice") == 0 || strcmp (operation, "--phase2-listen-six-directions") == 0 ||
		strcmp (operation, "--phase2-status-hrtf-on") == 0;
}

static int PrintUsage (const char *program)
{
	fprintf (stderr, "Usage: %s [--phase2-unit-only|--phase2-status|--phase2-status-hrtf-on|--phase1b-direct-memory|--phase1b-file-slice|--phase2-listen-six-directions]\n", program);
	return 2;
}

static int RunPhase2UnitOperation (const char *operation)
{
	if (operation == NULL || strcmp (operation, "--phase2-unit-only") != 0)
	{
		return -1;
	}
	TestPhase2Capabilities ();
	return Failures == 0 ? 0 : 1;
}

static bool RunInitialOperation (const char *operation, const char *program, int *result)
{
	if (!IsKnownOperation (operation))
	{
		*result = PrintUsage (program);
		return true;
	}
	*result = RunPhase2UnitOperation (operation);
	return *result >= 0;
}

static bool PrintPhase2StatusIfRequested (const char *operation, OpenALSoundRenderer &renderer)
{
	if (operation == NULL || (strcmp (operation, "--phase2-status") != 0 && strcmp (operation, "--phase2-status-hrtf-on") != 0))
	{
		return false;
	}
	renderer.PrintStatus ();
	fprintf (stdout, "HRTF: advertised=%d, attributes-at-init=%d, request-at-init=%d, active-known=%d, active=%s, status-known=%d, status=%d, init-result=%d, specifier=%s\n",
		renderer.Capabilities.HRTFAdvertised ? 1 : 0, renderer.HRTFAttributesApplied ? 1 : 0,
		renderer.HRTFRequestedEnabled ? 1 : 0, renderer.Capabilities.HRTFActiveKnown ? 1 : 0,
		renderer.Capabilities.HRTFActiveKnown ? (renderer.Capabilities.HRTFActive ? "1" : "0") : "unknown",
		renderer.Capabilities.HRTFStatusKnown ? 1 : 0, renderer.Capabilities.HRTFStatus,
		renderer.HRTFFailure, renderer.Capabilities.HRTFSpecifier.GetChars ());
	fprintf (stdout, "Stats: %s\n", renderer.GatherStats ().GetChars ());
	return true;
}

static bool IsPhase2ListeningOperation (const char *operation)
{
	return operation != NULL && strcmp (operation, "--phase2-listen-six-directions") == 0;
}

static bool PlayPhase2ListeningBurst (OpenALSoundRenderer &renderer, SoundHandle sound, SoundListener &listener,
	FRolloffInfo &rolloff, const Phase2DirectionCase &testCase)
{
	FVector3 backendPosition = MakePhase2BackendPosition (testCase.GameX, testCase.GameY, testCase.GameZ);
	FVector3 openALPosition (backendPosition.X, backendPosition.Y, -backendPosition.Z);
	FISoundChannel *channel;
	fprintf (stdout, "%s expected=%s game=(%.0f,%.0f,%.0f) backend=(%.0f,%.0f,%.0f) OpenAL=(%.0f,%.0f,%.0f)\n",
		testCase.ID, testCase.ExpectedDirection, testCase.GameX, testCase.GameY, testCase.GameZ,
		backendPosition.X, backendPosition.Y, backendPosition.Z, openALPosition.X, openALPosition.Y, openALPosition.Z);
	channel = renderer.StartSound3D (sound, &listener, 0.6f, &rolloff, 1.f, 128, 0, backendPosition, FVector3 (), 0, 0, NULL);
	if (channel == NULL)
	{
		fprintf (stderr, "FAILED: %s could not start a 3D source\n", testCase.ID);
		return false;
	}
	for (int elapsed = 0; elapsed < 2000 && channel->SysChannel != NULL; elapsed += 20)
	{
		std::this_thread::sleep_for (std::chrono::milliseconds (20));
		renderer.UpdateSounds ();
	}
	StopAndDrain (renderer, channel);
	ReleaseOwner (channel);
	return true;
}

static int RunPhase2SixDirectionListening (OpenALSoundRenderer &renderer)
{
	static const Phase2DirectionCase cases[] =
	{
		{ "M01", "left", 0.f, 64.f, 0.f },
		{ "M02", "right", 0.f, -64.f, 0.f },
		{ "M03", "front", 64.f, 0.f, 0.f },
		{ "M04", "back", -64.f, 0.f, 0.f },
		{ "M05", "up", 0.f, 0.f, 64.f },
		{ "M06", "down", 0.f, 0.f, -64.f }
	};
	const unsigned int sampleRate = 22050;
	const unsigned int burstFrames = sampleRate * 2;
	SoundListener listener;
	FRolloffInfo rolloff = MakeLinearRolloff (64.f, 128.f);
	std::vector<BYTE> samples;
	SoundHandle sound;
	int index = 0;

	if (!renderer.Capabilities.HRTFActiveKnown || !renderer.Capabilities.HRTFActive)
	{
		fprintf (stderr, "SKIP: HRTF actual activity is not available (known=%d active=%d); manual listening did not run\n",
			renderer.Capabilities.HRTFActiveKnown ? 1 : 0, renderer.Capabilities.HRTFActive ? 1 : 0);
		return 77;
	}

	samples = MakeManualNoiseSamples (burstFrames);
	sound = renderer.LoadSoundRaw (&samples[0], (int)samples.size (), sampleRate, 1, -16, -1);
	if (sound.data == NULL)
	{
		fprintf (stderr, "FAILED: manual listening PCM buffer could not initialize\n");
		return 1;
	}
	listener.position = FVector3 (0.f, 0.f, 0.f);
	listener.velocity = FVector3 ();
	listener.angle = 0.f;
	listener.valid = true;
	renderer.UpdateListener (&listener);
	fprintf (stdout, "HRTF active. Listener game=(0,0,0) backend=(0,0,0) OpenAL=(0,0,0) forward=(+1,0,0).\n");

	while (index >= 0 && index < (int)(sizeof (cases) / sizeof (cases[0])))
	{
		char command[16];

		if (!PlayPhase2ListeningBurst (renderer, sound, listener, rolloff, cases[index]))
		{
			renderer.UnloadSound (sound);
			return 1;
		}
		fprintf (stdout, "Press Enter for next case, r then Enter to replay, or b then Enter to go back: ");
		fflush (stdout);
		if (fgets (command, sizeof (command), stdin) == NULL)
		{
			fprintf (stderr, "FAILED: manual listening input closed before completion\n");
			renderer.UnloadSound (sound);
			return 1;
		}
		if (command[0] == 'r' || command[0] == 'R')
		{
			continue;
		}
		if ((command[0] == 'b' || command[0] == 'B') && index > 0)
		{
			--index;
		}
		else
		{
			++index;
		}
	}
	renderer.UnloadSound (sound);
	fprintf (stdout, "Manual listening sequence completed; no hearing result was recorded by this program.\n");
	return 0;
}

int main (int argc, char **argv)
{
	const char *phase1bOperation = argc == 2 ? argv[1] : NULL;
	int initialResult;
	if (RunInitialOperation (phase1bOperation, argv[0], &initialResult))
	{
		return initialResult;
	}
	if (IsPhase2ListeningOperation (phase1bOperation) || (phase1bOperation != NULL && strcmp (phase1bOperation, "--phase2-status-hrtf-on") == 0)) snd_hrtf.Value = true;
	std::vector<BYTE> longSamples = MakeSamples (8000);
	std::vector<BYTE> shortSamples = MakeSamples (160);
	snd_channels.Value = 2;
	int priorityResult = RunPriorityRendererTests (longSamples);
	if (priorityResult != 0)
	{
		return priorityResult;
	}
	snd_channels.Value = 1;
	{
	OpenALSoundRenderer renderer;
	if (!renderer.IsValid ())
	{
		if (renderer.Device == NULL && renderer.Context == NULL)
		{
			fprintf (stderr, "SKIP: OpenAL device/context could not initialize\n");
			return 77;
		}
		fprintf (stderr, "FAILED: OpenAL renderer could not initialize after context creation\n");
		return 1;
	}
	if (IsPhase2ListeningOperation (phase1bOperation)) return RunPhase2SixDirectionListening (renderer);
	if (PrintPhase2StatusIfRequested (phase1bOperation, renderer))
	{
		return 0;
	}
	TestPhase2RuntimeHRTFRequest (renderer);
	if (phase1bOperation != NULL)
	{
		if (strcmp (phase1bOperation, "--phase1b-direct-memory") == 0)
		{
			TestPhase1BDirectMemory (renderer);
		}
		else
		{
			TestPhase1BFileSlice (renderer);
		}
		return Failures == 0 ? 0 : 1;
	}

	SoundHandle longSound = renderer.LoadSoundRaw (&longSamples[0], (int)longSamples.size (), 8000, 1, -16, -1);
	SoundHandle shortSound = renderer.LoadSoundRaw (&shortSamples[0], (int)shortSamples.size (), 8000, 1, -16, -1);
	std::vector<BYTE> stereoSamples = MakeStereoSamples (8000);
	SoundHandle stereoSound = renderer.LoadSoundRaw (&stereoSamples[0], (int)stereoSamples.size (), 8000, 2, -16, -1);
	if (longSound.data == NULL || shortSound.data == NULL || stereoSound.data == NULL)
	{
		fprintf (stderr, "FAILED: OpenAL PCM buffers could not initialize\n");
		return 1;
	}

	TestEviction (renderer, longSound);
	TestExplicitStop (renderer, longSound);
	TestNaturalFinish (renderer, shortSound);
	TestImmediateReuseAfterExplicitStop (renderer, longSound, shortSound);
	TestEvictedOwnerReuseAfterFailedStart (renderer, longSound);
	TestFailedStartDoesNotPublish (renderer, longSound);
	Test3DState (renderer, stereoSound);
	TestInactiveMuteAndComplete (renderer, longSound);
	TestRestartPositions (renderer, longSound);
	TestEarlyClockAbstimeRestart (renderer);
	TestLongClockRestartBounds (renderer);
	TestRestartConversionBounds (renderer);
	TestClockWrap (renderer);
	TestStreams (renderer, longSound);
	TestEncodedSfxAndDumb (renderer);

	renderer.UnloadSound (longSound);
	renderer.UnloadSound (shortSound);
	renderer.UnloadSound (stereoSound);
	Check (alGetError () == AL_NO_ERROR, "normal renderer sound unloads leave no OpenAL error");
	}
	TestCrossRendererResetHandoff ();
	return Failures == 0 ? 0 : 1;
}