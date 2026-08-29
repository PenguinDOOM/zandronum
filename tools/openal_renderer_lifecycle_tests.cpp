#include "oalsound.h"

#include <chrono>
#include <stdio.h>
#include <thread>
#include <vector>

OALTestIntCVar snd_channels (1);
OALTestStringCVar snd_openal_device ("default");
OALTestFloatCVar snd_sfxvolume (1.f);
OALTestBoolCVar snd_pitched (true);

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

	bool IsExpectedEvent (size_t index, OpenALEndReason reason)
	{
		return Events.size () > index && Events[index].Reason == reason && Events[index].SawAttached && Events[index].SawFinalizing;
	}

	void ReleaseOwner (FISoundChannel *owner)
	{
		delete owner;
	}

	void StopAndDrain (OpenALSoundRenderer &renderer, FISoundChannel *owner)
	{
		if (owner != NULL && owner->SysChannel != NULL)
		{
			renderer.StopChannel (owner);
		}
		renderer.UpdateSounds ();
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
	}


void Printf (const char *, ...)
{
}

void DPrintf (const char *, ...)
{
}

FISoundChannel *S_GetChannel (void *syschan)
{
	FISoundChannel *channel = new FISoundChannel;
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
	if (longSound.data == NULL || shortSound.data == NULL)
	{
		fprintf (stderr, "FAILED: OpenAL PCM buffers could not initialize\n");
		return 1;
	}

	TestEviction (renderer, longSound);
	TestExplicitStop (renderer, longSound);
	TestNaturalFinish (renderer, shortSound);
	TestImmediateReuseAfterExplicitStop (renderer, longSound, shortSound);
	TestFailedStartDoesNotPublish (renderer, longSound);

	renderer.UnloadSound (longSound);
	renderer.UnloadSound (shortSound);
	return Failures == 0 ? 0 : 1;
}