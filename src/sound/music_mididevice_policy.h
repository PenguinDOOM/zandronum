#ifndef MUSIC_MIDIDEVICE_POLICY_H
#define MUSIC_MIDIDEVICE_POLICY_H

#include <cstddef>

#include "music_mididevice.h"

namespace MusicMIDIDevicePolicy
{
	enum ESelectionOrigin
	{
		SELECTION_EXPLICIT,
		SELECTION_DEFAULT
	};

	struct DeviceSelection
	{
		EMidiDevice ResolvedDevice;
		EMidiDevice FinalDevice;
		ESelectionOrigin Origin;
		bool RemappedFromFMOD;
		unsigned int DiagnosticCount;
	};

	inline EMidiDevice ResolveRequestedDevice(EMidiDevice requestedDevice, int configuredDevice,
		bool isWindowsPlatform)
	{
		if (!isWindowsPlatform && requestedDevice == MDEV_MMAPI)
		{
			requestedDevice = MDEV_FMOD;
		}
		if (requestedDevice != MDEV_DEFAULT)
		{
			return requestedDevice;
		}
		switch (configuredDevice)
		{
		case -1: return MDEV_FMOD;
		case -2: return MDEV_TIMIDITY;
		case -3: return MDEV_OPL;
		case -4: return MDEV_GUS;
#ifdef HAVE_FLUIDSYNTH
		case -5: return MDEV_FLUIDSYNTH;
#endif
		default: return isWindowsPlatform ? MDEV_MMAPI : MDEV_FMOD;
		}
	}

	inline EMidiDevice ResolveFinalDevice(EMidiDevice resolvedDevice, bool supportsGeneratedSMFStreaming,
		bool *remappedFromFMOD = NULL)
	{
		const bool remapped = resolvedDevice == MDEV_FMOD && !supportsGeneratedSMFStreaming;
		if (remappedFromFMOD != NULL)
		{
			*remappedFromFMOD = remapped;
		}
		if (remapped)
		{
			return MDEV_OPL;
		}
		return resolvedDevice;
	}

	inline DeviceSelection ResolveSelection(EMidiDevice requestedDevice, int configuredDevice,
		bool isWindowsPlatform, bool supportsGeneratedSMFStreaming)
	{
		DeviceSelection selection;
		selection.Origin = requestedDevice == MDEV_DEFAULT ? SELECTION_DEFAULT : SELECTION_EXPLICIT;
		selection.ResolvedDevice = ResolveRequestedDevice(requestedDevice, configuredDevice, isWindowsPlatform);
		selection.FinalDevice = ResolveFinalDevice(selection.ResolvedDevice,
			supportsGeneratedSMFStreaming, &selection.RemappedFromFMOD);
		selection.DiagnosticCount = selection.RemappedFromFMOD ? 1 : 0;
		return selection;
	}

	inline bool CanRetryFMOD(bool supportsGeneratedSMFStreaming)
	{
		return supportsGeneratedSMFStreaming;
	}
}

#endif