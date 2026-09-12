#include "music_mididevice_policy.h"

#include <stdio.h>

namespace
{
	static_assert(MDEV_DEFAULT == -1, "MDEV_DEFAULT value changed");
	static_assert(MDEV_MMAPI == 0, "MDEV_MMAPI value changed");
	static_assert(MDEV_OPL == 1, "MDEV_OPL value changed");
	static_assert(MDEV_FMOD == 2, "MDEV_FMOD value changed");
	static_assert(MDEV_TIMIDITY == 3, "MDEV_TIMIDITY value changed");
	static_assert(MDEV_FLUIDSYNTH == 4, "MDEV_FLUIDSYNTH value changed");
	static_assert(MDEV_GUS == 5, "MDEV_GUS value changed");

	int Failures = 0;

	void Check(bool condition, const char *name)
	{
		if (!condition)
		{
			fprintf(stderr, "FAILED: %s\n", name);
			++Failures;
		}
	}

	void CheckDevice(EMidiDevice actual, EMidiDevice expected, const char *name)
	{
		Check(actual == expected, name);
	}
}

int main()
{
	MusicMIDIDevicePolicy::DeviceSelection selection;

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_FMOD, -3, false, false);
	Check(selection.Origin == MusicMIDIDevicePolicy::SELECTION_EXPLICIT,
		"explicit FMOD request records explicit origin");
	CheckDevice(selection.ResolvedDevice, MDEV_FMOD,
		"explicit FMOD request reaches final resolution");
	CheckDevice(selection.FinalDevice, MDEV_OPL,
		"explicit FMOD request falls back to OPL on an incapable renderer");
	Check(selection.DiagnosticCount == 1, "explicit FMOD fallback emits one diagnostic");

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_DEFAULT, -1, false, false);
	Check(selection.Origin == MusicMIDIDevicePolicy::SELECTION_DEFAULT,
		"default FMOD request records default origin");
	CheckDevice(selection.ResolvedDevice, MDEV_FMOD,
		"default FMOD request resolves from snd_mididevice");
	CheckDevice(selection.FinalDevice, MDEV_OPL,
		"default FMOD request falls back to OPL on an incapable renderer");
	Check(selection.DiagnosticCount == 1, "default FMOD fallback emits one diagnostic");

	const bool openALSupportsGeneratedSMFStreaming = false;
	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_FMOD, -3, false,
		openALSupportsGeneratedSMFStreaming);
	CheckDevice(selection.FinalDevice, MDEV_OPL, "OpenAL incapable selection uses OPL");
	Check(selection.DiagnosticCount == 1, "OpenAL fallback emits one diagnostic");

	const bool nullSupportsGeneratedSMFStreaming = false;
	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_FMOD, -3, false,
		nullSupportsGeneratedSMFStreaming);
	CheckDevice(selection.FinalDevice, MDEV_OPL, "Null incapable selection uses OPL");
	Check(selection.DiagnosticCount == 1, "Null fallback emits one diagnostic");

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_FMOD, -3, false, true);
	CheckDevice(selection.FinalDevice, MDEV_FMOD,
		"FMOD-capable selection remains FMOD");
	Check(selection.DiagnosticCount == 0, "FMOD-capable selection remains silent");

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_DEFAULT, 0, true, false);
	CheckDevice(selection.ResolvedDevice, MDEV_MMAPI,
		"Windows default selection resolves to MMAPI");
	CheckDevice(selection.FinalDevice, MDEV_MMAPI,
		"Windows platform-resolved MMAPI remains unchanged");
	Check(selection.DiagnosticCount == 0, "platform-resolved MMAPI selection remains silent");

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_MMAPI, 0, false, false);
	CheckDevice(selection.ResolvedDevice, MDEV_FMOD,
		"non-Windows MMAPI request resolves to FMOD");
	CheckDevice(selection.FinalDevice, MDEV_OPL,
		"non-Windows FMOD platform resolution falls back to OPL");
	Check(selection.DiagnosticCount == 1, "non-Windows FMOD platform fallback emits one diagnostic");

	selection = MusicMIDIDevicePolicy::ResolveSelection(MDEV_OPL, -1, false, false);
	CheckDevice(selection.FinalDevice, MDEV_OPL,
		"non-FMOD selection remains unchanged");
	Check(selection.DiagnosticCount == 0, "non-FMOD selection remains silent");

	Check(MusicMIDIDevicePolicy::CanRetryFMOD(true),
		"retry to FMOD is allowed when generated-SMF streaming is supported");
	Check(!MusicMIDIDevicePolicy::CanRetryFMOD(false),
		"retry to FMOD is blocked when generated-SMF streaming is unavailable");

	return Failures == 0 ? 0 : 1;
}