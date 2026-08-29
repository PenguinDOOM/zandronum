#include "oalsound.h"

#include <AL/al.h>
#include <chrono>
#include <limits>
#include <math.h>
#include <stdio.h>
#include <thread>
#include <vector>

OALTestIntCVar snd_channels (1);
OALTestStringCVar snd_openal_device ("default");
OALTestFloatCVar snd_sfxvolume (1.f);
OALTestBoolCVar snd_pitched (true);

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

	void TestStreams (OpenALSoundRenderer &renderer, SoundHandle sound)
	{
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
		Check (renderer.OpenStream ("unsupported.ogg", 0, 0, 0) == NULL, "encoded stream opening is rejected safely");
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
		FISoundChannel *reused = S_GetChannel (NULL);
		Check (reused == evicted && reused->SysChannel == NULL && renderer.GetPosition (reused) == 0,
			"deserialize-style owner reuse cannot serialize the old logical phase before MarkStartTime");
		reused->StartTime.AsOne = 321;
		renderer.InjectStartFailureForTest ();
		Check (renderer.StartSound (sound, 0.5f, 128, 81, SNDF_LOOP | SNDF_ABSTIME, reused) == NULL &&
			reused->SysChannel == NULL && renderer.GetPosition (reused) == 0,
			"failed deserialize-style ABSTIME restart keeps the old logical phase hidden before retry");
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
		Check (Events.size () == eventCount, "injected OpenAL start failure does not notify high-level channel end");
		Check (renderer.ActiveChannels.empty () && renderer.AllocatedSources - (int)renderer.ActiveChannels.size () == freeSources, "injected OpenAL start failure recovers source baseline");
		Check (openalSound->References == 0, "injected OpenAL start failure does not retain sound reference");
		renderer.SetSfxPaused (true, 1);
		AdvanceTestClock (renderer, 100);
		renderer.InjectStartFailureForTest ();
		FISoundChannel *failedNoPause = renderer.StartSound (sound, 0.5f, 128, 0, SNDF_LOOP | SNDF_NOPAUSE, NULL);
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

int main ()
{
	std::vector<BYTE> longSamples = MakeSamples (8000);
	std::vector<BYTE> shortSamples = MakeSamples (160);
	snd_channels.Value = 2;
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
	}
	snd_channels.Value = 1;
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
	TestCrossRendererResetHandoff ();

	renderer.UnloadSound (longSound);
	renderer.UnloadSound (shortSound);
	renderer.UnloadSound (stereoSound);
	return Failures == 0 ? 0 : 1;
}