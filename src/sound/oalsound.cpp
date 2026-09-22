#include "oalsound.h"

#include "audio_decoder.h"
#include "oaldata.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/efx.h>

#include <algorithm>
#include <stdint.h>
#include <limits>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#ifdef OAL_LIFECYCLE_TEST
#include "oalsound_test_support.h"
#else
#include "c_cvars.h"
#include "c_console.h"
#include "i_system.h"
#include "s_sound.h"
#include "v_text.h"
#endif

namespace
{
	const std::size_t MaxOpenALEncodedInputBytes = 64 * 1024 * 1024;

	bool AcceptsOpenALEncodedInputSize (std::size_t bytes)
	{
		return bytes <= MaxOpenALEncodedInputBytes;
	}

#ifdef OAL_LIFECYCLE_TEST
	bool FailNextOpenALEncodedSourceAssign = false;
#endif

	bool AssignOpenALEncodedSource (AudioMemorySource &source, const void *data, std::size_t bytes)
	{
		if (!AcceptsOpenALEncodedInputSize (bytes))
		{
			return false;
		}
		try
		{
	#ifdef OAL_LIFECYCLE_TEST
			if (FailNextOpenALEncodedSourceAssign)
			{
				FailNextOpenALEncodedSourceAssign = false;
				throw std::bad_alloc ();
			}
	#endif
			return source.Assign (data, bytes) == AUDIO_SOURCE_OK;
		}
		catch (const std::bad_alloc &)
		{
			return false;
		}
	}
}

#ifdef OAL_LIFECYCLE_TEST
void OALTestFailNextEncodedSourceAssign ()
{
	FailNextOpenALEncodedSourceAssign = true;
}

bool OALTestAcceptsEncodedInputSize (unsigned int bytes)
{
	return AcceptsOpenALEncodedInputSize (bytes);
}
#endif

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif

#ifndef AL_LOOP_POINTS_SOFT
#define AL_LOOP_POINTS_SOFT 0x2015
#endif

#ifndef AL_FORMAT_MONO_FLOAT32
#define AL_FORMAT_MONO_FLOAT32 0x10010
#define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif

#ifndef AL_AUXILIARY_SEND_FILTER
#define AL_AUXILIARY_SEND_FILTER 0x20006
#define AL_AIR_ABSORPTION_FACTOR 0x20007
#define AL_DIRECT_FILTER_GAINHF_AUTO 0x2000A
#define AL_AUXILIARY_SEND_FILTER_GAIN_AUTO 0x2000B
#define AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO 0x2000C
#define AL_DIRECT_FILTER 0x20005
#endif

#ifndef AL_FILTER_LOWPASS
#define AL_FILTER_LOWPASS 0x0001
#define AL_LOWPASS_GAIN 0x0001
#define AL_LOWPASS_GAINHF 0x0002
#endif

#ifndef AL_SOURCE_RADIUS
#define AL_SOURCE_RADIUS 0x1031
#endif

#ifndef ALC_SOFT_HRTF
#define ALC_SOFT_HRTF 1
#define ALC_HRTF_SOFT 0x1992
#define ALC_HRTF_STATUS_SOFT 0x1993
#define ALC_HRTF_SPECIFIER_SOFT 0x1995
#define ALC_HRTF_DISABLED_SOFT 0x0000
#define ALC_HRTF_ENABLED_SOFT 0x0001
#define ALC_HRTF_DENIED_SOFT 0x0002
#define ALC_HRTF_REQUIRED_SOFT 0x0003
#define ALC_HRTF_HEADPHONES_DETECTED_SOFT 0x0004
#define ALC_HRTF_UNSUPPORTED_FORMAT_SOFT 0x0005
#endif

EXTERN_CVAR (Int, snd_channels)
EXTERN_CVAR (String, snd_openal_device)
EXTERN_CVAR (Float, snd_sfxvolume)
EXTERN_CVAR (Bool, snd_pitched)
EXTERN_CVAR (Bool, snd_hrtf)
EXTERN_CVAR (Bool, snd_waterreverb)
EXTERN_CVAR (Float, snd_waterlp)

namespace
{
	float ClampReverbValue (float value, float defaultValue, float minimum, float maximum)
	{
		if (isnan (value))
		{
			return defaultValue;
		}
		if (isinf (value))
		{
			return value < 0.f ? minimum : maximum;
		}
		return std::max (minimum, std::min (value, maximum));
	}

	float ReverbGain (float millibels, float defaultValue, float maximum)
	{
		return ClampReverbValue (powf (10.f, float (millibels) / 2000.f), defaultValue, 0.f, maximum);
	}

	bool NormalizeReverbPan (float pan[3])
	{
		if (!isfinite (pan[0]) || !isfinite (pan[1]) || !isfinite (pan[2]))
		{
			pan[0] = pan[1] = pan[2] = 0.f;
			return true;
		}
		double length = sqrt (double (pan[0]) * pan[0] + double (pan[1]) * pan[1] + double (pan[2]) * pan[2]);
		if (length > 1.0)
		{
			pan[0] = float (pan[0] / length);
			pan[1] = float (pan[1] / length);
			pan[2] = float (pan[2] / length);
		}
		return false;
	}
}

OpenALReverbParameters OALBuildReverbParameters (const REVERB_PROPERTIES &properties)
{
	OpenALReverbParameters output;
	output.Gain = ReverbGain (float (properties.Room), AL_EAXREVERB_DEFAULT_GAIN, 1.f);
	output.GainHF = ReverbGain (float (properties.RoomHF), AL_EAXREVERB_DEFAULT_GAINHF, 1.f);
	output.GainLF = ReverbGain (float (properties.RoomLF), AL_EAXREVERB_DEFAULT_GAINLF, 1.f);
	output.DecayTime = ClampReverbValue (properties.DecayTime, AL_EAXREVERB_DEFAULT_DECAY_TIME, AL_EAXREVERB_MIN_DECAY_TIME, AL_EAXREVERB_MAX_DECAY_TIME);
	output.DecayHFRatio = ClampReverbValue (properties.DecayHFRatio, AL_EAXREVERB_DEFAULT_DECAY_HFRATIO, AL_EAXREVERB_MIN_DECAY_HFRATIO, AL_EAXREVERB_MAX_DECAY_HFRATIO);
	output.DecayLFRatio = ClampReverbValue (properties.DecayLFRatio, AL_EAXREVERB_DEFAULT_DECAY_LFRATIO, AL_EAXREVERB_MIN_DECAY_LFRATIO, AL_EAXREVERB_MAX_DECAY_LFRATIO);
	output.ReflectionsGain = ReverbGain (float (properties.Reflections), AL_EAXREVERB_DEFAULT_REFLECTIONS_GAIN, AL_EAXREVERB_MAX_REFLECTIONS_GAIN);
	output.ReflectionsDelay = ClampReverbValue (properties.ReflectionsDelay, AL_EAXREVERB_DEFAULT_REFLECTIONS_DELAY, AL_EAXREVERB_MIN_REFLECTIONS_DELAY, AL_EAXREVERB_MAX_REFLECTIONS_DELAY);
	output.ReflectionsPan[0] = properties.ReflectionsPan0; output.ReflectionsPan[1] = properties.ReflectionsPan1; output.ReflectionsPan[2] = properties.ReflectionsPan2;
	output.HasInvalidPan = NormalizeReverbPan (output.ReflectionsPan);
	output.LateReverbGain = ReverbGain (float (properties.Reverb), AL_EAXREVERB_DEFAULT_LATE_REVERB_GAIN, AL_EAXREVERB_MAX_LATE_REVERB_GAIN);
	output.LateReverbDelay = ClampReverbValue (properties.ReverbDelay, AL_EAXREVERB_DEFAULT_LATE_REVERB_DELAY, AL_EAXREVERB_MIN_LATE_REVERB_DELAY, AL_EAXREVERB_MAX_LATE_REVERB_DELAY);
	output.LateReverbPan[0] = properties.ReverbPan0; output.LateReverbPan[1] = properties.ReverbPan1; output.LateReverbPan[2] = properties.ReverbPan2;
	output.HasInvalidPan = NormalizeReverbPan (output.LateReverbPan) || output.HasInvalidPan;
	output.EchoTime = ClampReverbValue (properties.EchoTime, AL_EAXREVERB_DEFAULT_ECHO_TIME, AL_EAXREVERB_MIN_ECHO_TIME, AL_EAXREVERB_MAX_ECHO_TIME);
	output.EchoDepth = ClampReverbValue (properties.EchoDepth, AL_EAXREVERB_DEFAULT_ECHO_DEPTH, AL_EAXREVERB_MIN_ECHO_DEPTH, AL_EAXREVERB_MAX_ECHO_DEPTH);
	output.ModulationTime = ClampReverbValue (properties.ModulationTime, AL_EAXREVERB_DEFAULT_MODULATION_TIME, AL_EAXREVERB_MIN_MODULATION_TIME, AL_EAXREVERB_MAX_MODULATION_TIME);
	output.ModulationDepth = ClampReverbValue (properties.ModulationDepth, AL_EAXREVERB_DEFAULT_MODULATION_DEPTH, AL_EAXREVERB_MIN_MODULATION_DEPTH, AL_EAXREVERB_MAX_MODULATION_DEPTH);
	output.AirAbsorptionGainHF = ReverbGain (properties.AirAbsorptionHF, AL_EAXREVERB_DEFAULT_AIR_ABSORPTION_GAINHF, 1.f);
	output.AirAbsorptionGainHF = ClampReverbValue (output.AirAbsorptionGainHF, AL_EAXREVERB_DEFAULT_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF, 1.f);
	output.HFReference = ClampReverbValue (properties.HFReference, AL_EAXREVERB_DEFAULT_HFREFERENCE, AL_EAXREVERB_MIN_HFREFERENCE, AL_EAXREVERB_MAX_HFREFERENCE);
	output.LFReference = ClampReverbValue (properties.LFReference, AL_EAXREVERB_DEFAULT_LFREFERENCE, AL_EAXREVERB_MIN_LFREFERENCE, AL_EAXREVERB_MAX_LFREFERENCE);
	output.RoomRolloffFactor = ClampReverbValue (properties.RoomRolloffFactor, AL_EAXREVERB_DEFAULT_ROOM_ROLLOFF_FACTOR, AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR, AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR);
	output.Diffusion = ClampReverbValue (properties.Diffusion / 100.f, AL_EAXREVERB_DEFAULT_DIFFUSION, AL_EAXREVERB_MIN_DIFFUSION, AL_EAXREVERB_MAX_DIFFUSION);
	output.Density = ClampReverbValue (properties.Density / 100.f, AL_EAXREVERB_DEFAULT_DENSITY, AL_EAXREVERB_MIN_DENSITY, AL_EAXREVERB_MAX_DENSITY);
	output.DecayHFLimit = (properties.Flags & REVERB_FLAGS_DECAYHFLIMIT) != 0;
	return output;
}

enum
{
	OALPAUSE_Gameplay = 1,
	OALPAUSE_Inactive = 2,
	OALPAUSE_Sync = 4
};

namespace
{
	template <typename FunctionType>
	FunctionType ResolveOpenALFunction (const char *name)
	{
		return reinterpret_cast<FunctionType> (alGetProcAddress (name));
	}

	OpenALEFXFunctions ResolveEFXFunctions ()
	{
		OpenALEFXFunctions functions;
		functions.GenEffects = ResolveOpenALFunction<OALGenEffects> ("alGenEffects");
		functions.DeleteEffects = ResolveOpenALFunction<OALDeleteEffects> ("alDeleteEffects");
		functions.Effecti = ResolveOpenALFunction<OALEffecti> ("alEffecti");
		functions.Effectf = ResolveOpenALFunction<OALEffectf> ("alEffectf");
		functions.Effectfv = ResolveOpenALFunction<OALEffectfv> ("alEffectfv");
		functions.GenAuxiliaryEffectSlots = ResolveOpenALFunction<OALGenAuxiliaryEffectSlots> ("alGenAuxiliaryEffectSlots");
		functions.DeleteAuxiliaryEffectSlots = ResolveOpenALFunction<OALDeleteAuxiliaryEffectSlots> ("alDeleteAuxiliaryEffectSlots");
		functions.AuxiliaryEffectSloti = ResolveOpenALFunction<OALAuxiliaryEffectSloti> ("alAuxiliaryEffectSloti");
		functions.AuxiliaryEffectSlotf = ResolveOpenALFunction<OALAuxiliaryEffectSlotf> ("alAuxiliaryEffectSlotf");
		functions.GenFilters = ResolveOpenALFunction<OALGenFilters> ("alGenFilters");
		functions.DeleteFilters = ResolveOpenALFunction<OALDeleteFilters> ("alDeleteFilters");
		functions.Filteri = ResolveOpenALFunction<OALFilteri> ("alFilteri");
		functions.Filterf = ResolveOpenALFunction<OALFilterf> ("alFilterf");
		return functions;
	}

	bool InitializeEFXResources (OpenALCapabilities *capabilities, OALuint *effect, OALuint *slot,
		OALuint *filter, bool *usesEAX, int sends, OALGetError getError)
	{
		OpenALEFXFunctions &efx = capabilities->EFX;
		if (capabilities->EFXFilterCallable)
		{
			getError ();
			efx.GenFilters (1, filter);
			if (getError () == AL_NO_ERROR && *filter != 0)
			{
				getError ();
				efx.Filteri (*filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
				if (getError () == AL_NO_ERROR)
				{
					capabilities->EFXFilterUsable = true;
				}
				else
				{
					efx.DeleteFilters (1, filter);
					*filter = 0;
				}
			}
			else if (*filter != 0)
			{
				efx.DeleteFilters (1, filter);
				*filter = 0;
			}
		}
		if (!capabilities->EFXCallable || sends <= 0)
		{
			return false;
		}
		bool typeSelected = false;
		getError ();
		efx.GenEffects (1, effect);
		if (getError () == AL_NO_ERROR && *effect != 0)
		{
			efx.Effecti (*effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
			if (getError () == AL_NO_ERROR)
			{
				*usesEAX = true;
				typeSelected = true;
			}
			else
			{
				efx.Effecti (*effect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
				typeSelected = getError () == AL_NO_ERROR;
			}
		}
		if (typeSelected)
		{
			getError ();
			efx.GenAuxiliaryEffectSlots (1, slot);
			if (getError () == AL_NO_ERROR && *slot != 0)
			{
				getError ();
				efx.AuxiliaryEffectSloti (*slot, AL_EFFECTSLOT_EFFECT, *effect);
				if (getError () == AL_NO_ERROR)
				{
					capabilities->EFXUsable = true;
				}
			}
		}
		return capabilities->EFXUsable;
	}

	void ReleaseEFXReverbResources (OpenALCapabilities *capabilities, OALuint *effect, OALuint *slot, bool *usesEAX)
	{
		OpenALEFXFunctions &efx = capabilities->EFX;
		if (*slot != 0 && capabilities->EFXCallable)
		{
			efx.AuxiliaryEffectSloti (*slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
			efx.DeleteAuxiliaryEffectSlots (1, slot);
		}
		*slot = 0;
		if (*effect != 0 && capabilities->EFXCallable)
		{
			efx.DeleteEffects (1, effect);
		}
		*effect = 0;
		capabilities->EFXUsable = false;
		capabilities->EFXApplied = false;
		*usesEAX = false;
	}

	void FinalizeEFXInitialization (OpenALCapabilities *capabilities, OALuint *effect, OALuint *slot, bool *usesEAX)
	{
		if (!capabilities->EFXUsable)
		{
			ReleaseEFXReverbResources (capabilities, effect, slot, usesEAX);
		}
	}

	void ReleaseEFXResources (OpenALCapabilities *capabilities, OALuint *effect, OALuint *slot,
		OALuint *filter, bool *usesEAX)
	{
		OpenALEFXFunctions &efx = capabilities->EFX;
		if (*filter != 0 && capabilities->EFXFilterCallable)
		{
			efx.DeleteFilters (1, filter);
		}
		*filter = 0;
		capabilities->EFXFilterUsable = false;
		ReleaseEFXReverbResources (capabilities, effect, slot, usesEAX);
	}

	void InitializeRendererEFXResources (OpenALCapabilities *capabilities, ALCdevice *device,
		OALuint *effect, OALuint *slot, OALuint *filter, bool *usesEAX)
	{
		ALCint sends = 0;
		if (capabilities->EFXCallable)
		{
			alcGetError (device);
			alcGetIntegerv (device, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
			if (alcGetError (device) == ALC_NO_ERROR)
			{
				capabilities->EFXSendCount = sends;
			}
		}
		if (capabilities->EFXCallable || capabilities->EFXFilterCallable)
		{
			InitializeEFXResources (capabilities, effect, slot, filter, usesEAX, sends, alGetError);
		}
	}

	bool CopyHRTFSpecifier (OpenALCapabilities *capabilities, const ALCchar *specifier, bool querySucceeded);
	const ALCchar *GetHRTFSpecifier (ALCdevice *device);
	bool GetHRTFInteger (ALCdevice *device, ALCenum parameter, ALCint *value);

#ifdef OAL_LIFECYCLE_TEST
	struct OpenALHRTFQueryTestState
	{
		bool ActiveQuerySucceeded;
		bool Active;
		bool StatusQuerySucceeded;
		int Status;
		int ActiveQueryCount;
		int StatusQueryCount;
	};

	OpenALHRTFQueryTestState *HRTFQueryTestState = NULL;
#endif

	OpenALCapabilities CollectOpenALCapabilities (ALCdevice *device)
	{
		bool hrtfAdvertised = alcIsExtensionPresent (device, "ALC_SOFT_HRTF") == ALC_TRUE;
		bool hrtfActiveKnown = false;
		bool hrtfActive = false;
		bool hrtfStatusKnown = false;
		int hrtfStatus = 0;
		if (hrtfAdvertised)
		{
			ALCint active = ALC_FALSE;
			hrtfActiveKnown = GetHRTFInteger (device, ALC_HRTF_SOFT, &active);
			hrtfActive = active == ALC_TRUE;
			hrtfStatusKnown = GetHRTFInteger (device, ALC_HRTF_STATUS_SOFT, &hrtfStatus);
		}
		bool efxAdvertised = alcIsExtensionPresent (device, "ALC_EXT_EFX") == ALC_TRUE;
		OpenALEFXFunctions efx;
		if (efxAdvertised)
		{
			efx = ResolveEFXFunctions ();
		}
		bool radiusAdvertised = alIsExtensionPresent ("AL_EXT_SOURCE_RADIUS") == AL_TRUE;
		OpenALCapabilities capabilities = OALBuildCapabilities (hrtfAdvertised, hrtfActiveKnown, hrtfActive,
			hrtfStatusKnown, hrtfStatus, efxAdvertised, efx, radiusAdvertised);
		if (hrtfAdvertised)
		{
			const ALCchar *specifier = GetHRTFSpecifier (device);
			CopyHRTFSpecifier (&capabilities, specifier, alcGetError (device) == ALC_NO_ERROR);
		}
		return capabilities;
	}

	bool GetHRTFInteger (ALCdevice *device, ALCenum parameter, ALCint *value)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (HRTFQueryTestState != NULL)
		{
			if (parameter == ALC_HRTF_SOFT)
			{
				++HRTFQueryTestState->ActiveQueryCount;
				*value = HRTFQueryTestState->Active ? ALC_TRUE : ALC_FALSE;
				return HRTFQueryTestState->ActiveQuerySucceeded;
			}
			if (parameter == ALC_HRTF_STATUS_SOFT)
			{
				++HRTFQueryTestState->StatusQueryCount;
				*value = HRTFQueryTestState->Status;
				return HRTFQueryTestState->StatusQuerySucceeded;
			}
		}
#endif
		alcGetIntegerv (device, parameter, 1, value);
		return alcGetError (device) == ALC_NO_ERROR;
	}

	bool CopyHRTFSpecifier (OpenALCapabilities *capabilities, const ALCchar *specifier, bool querySucceeded)
	{
		if (specifier == NULL || !querySucceeded)
		{
			return false;
		}
		capabilities->HRTFSpecifier = specifier;
		return true;
	}

	const char *HRTFStatusName (const OpenALCapabilities &capabilities)
	{
		if (!capabilities.HRTFStatusKnown)
		{
			return "unknown";
		}
		switch (capabilities.HRTFStatus)
		{
		case ALC_HRTF_DISABLED_SOFT: return "disabled";
		case ALC_HRTF_ENABLED_SOFT: return "enabled";
		case ALC_HRTF_DENIED_SOFT: return "denied";
		case ALC_HRTF_REQUIRED_SOFT: return "required";
		case ALC_HRTF_HEADPHONES_DETECTED_SOFT: return "headphones-detected";
		case ALC_HRTF_UNSUPPORTED_FORMAT_SOFT: return "unsupported-format";
		default: return "unknown";
		}
	}

	const char *HRTFActiveName (const OpenALCapabilities &capabilities)
	{
		if (!capabilities.HRTFActiveKnown)
		{
			return "unknown";
		}
		return capabilities.HRTFActive ? "yes" : "no";
	}

	const char *HRTFContextFailureName (OpenALContextHRTFFailure failure)
	{
		switch (failure)
		{
		case OALHRTFCONTEXT_ExtensionAbsent: return "extension absent";
		case OALHRTFCONTEXT_CreateFailed: return "attribute context creation failed; retried without attributes";
		case OALHRTFCONTEXT_MakeCurrentFailed: return "attribute context current activation failed; retried without attributes";
		default: return "attributes accepted";
		}
	}

	const char *EFXFailureName (const OpenALCapabilities &capabilities, const ReverbContainer *lastAppliedEnvironment, OpenALEFXFailure failure)
	{
		const char *name = !capabilities.EFXAdvertised || !capabilities.EFXCallable ? "unavailable" :
			lastAppliedEnvironment == DefaultEnvironments[0] ? "dry-off" : "none";
		switch (failure)
		{
		case OALEFXFAIL_Properties: return "properties";
		case OALEFXFAIL_SlotAttach: return "slot-attach";
		case OALEFXFAIL_WetSend: return "wet-send";
		case OALEFXFAIL_DrySend: return "dry-send";
		case OALEFXFAIL_AirAbsorption: return "air-absorption";
		case OALEFXFAIL_DirectFilterAuto: return "direct-filter-auto";
		case OALEFXFAIL_SendGainAuto: return "send-gain-auto";
		case OALEFXFAIL_SendGainHFAuto: return "send-gainhf-auto";
		case OALEFXFAIL_DirectFilter: return "direct-filter";
		case OALEFXFAIL_SourceRadius: return "source-radius";
		case OALEFXFAIL_Unavailable: return "unavailable";
		default: return name;
		}
	}

	struct OpenALContextState
	{
		ALCdevice *Device;
		ALCcontext *Context;
		bool HRTFAttributesApplied;
		OpenALContextHRTFFailure HRTFFailure;

		OpenALContextState () : Device (NULL), Context (NULL), HRTFAttributesApplied (false), HRTFFailure (OALHRTFCONTEXT_NoFailure) {}
	};

#ifdef OAL_LIFECYCLE_TEST
	struct OpenALContextTestState
	{
		OpenALContextTestFailure Failure;
		OpenALContextTestResult Result;
	};

	OpenALContextTestState *ContextTestState = NULL;

	ALCdevice *OpenContextTestDevice (const ALCchar *)
	{
		++ContextTestState->Result.OpenCount;
		if (ContextTestState->Failure == OALCONTEXTTEST_SecondOpenFailure && ContextTestState->Result.OpenCount == 2)
		{
			return NULL;
		}
		return reinterpret_cast<ALCdevice *> ((size_t)ContextTestState->Result.OpenCount);
	}

	ALCboolean IsContextTestExtensionPresent (ALCdevice *device, const ALCchar *)
	{
		ContextTestState->Result.HRTFExtensionQueriedOnDevice = device != NULL;
		return device != NULL && ContextTestState->Result.HRTFAdvertised ? ALC_TRUE : ALC_FALSE;
	}

	const ALCchar *GetContextTestHRTFSpecifier (ALCdevice *, ALCenum parameter)
	{
		ContextTestState->Result.HRTFSpecifierParameter = parameter;
		return NULL;
	}

	ALCcontext *CreateContextTestContext (ALCdevice *, const ALCint *attributes)
	{
		++ContextTestState->Result.CreateCount;
		if (ContextTestState->Result.CreateCount == 1)
		{
			ContextTestState->Result.FirstAttributesWereHRTF = attributes != NULL && attributes[0] == ALC_HRTF_SOFT;
			ContextTestState->Result.FirstAttributesRequestedAuxiliarySend = attributes != NULL &&
				((attributes[0] == ALC_MAX_AUXILIARY_SENDS && attributes[1] == 1) ||
				 (attributes[2] == ALC_MAX_AUXILIARY_SENDS && attributes[3] == 1));
		}
		else
		{
			ContextTestState->Result.SecondAttributesWereNull = attributes == NULL;
		}
		if ((ContextTestState->Failure == OALCONTEXTTEST_FirstCreateFailure && ContextTestState->Result.CreateCount == 1) ||
			(ContextTestState->Failure == OALCONTEXTTEST_SecondCreateFailure && ContextTestState->Result.CreateCount == 2))
		{
			return NULL;
		}
		return reinterpret_cast<ALCcontext *> ((size_t)ContextTestState->Result.CreateCount);
	}

	ALCboolean MakeContextTestCurrent (ALCcontext *)
	{
		++ContextTestState->Result.MakeCurrentCount;
		return !((ContextTestState->Failure == OALCONTEXTTEST_FirstMakeCurrentFailure && ContextTestState->Result.MakeCurrentCount == 1) ||
			(ContextTestState->Failure == OALCONTEXTTEST_SecondMakeCurrentFailure && ContextTestState->Result.MakeCurrentCount == 2) ||
			(ContextTestState->Failure == OALCONTEXTTEST_BothMakeCurrentFailures && ContextTestState->Result.MakeCurrentCount <= 2));
	}

	void DestroyContextTestContext (ALCcontext *) { ++ContextTestState->Result.DestroyCount; }
	ALCboolean CloseContextTestDevice (ALCdevice *) { ++ContextTestState->Result.CloseCount; return ALC_TRUE; }
#endif

	ALCdevice *OpenContextDevice (const ALCchar *name)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return OpenContextTestDevice (name);
		}
#endif
		return alcOpenDevice (name);
	}

	ALCboolean IsContextExtensionPresent (ALCdevice *device, const ALCchar *extension)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return IsContextTestExtensionPresent (device, extension);
		}
#endif
		return alcIsExtensionPresent (device, extension);
	}

	const ALCchar *GetHRTFSpecifier (ALCdevice *device)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return GetContextTestHRTFSpecifier (device, ALC_HRTF_SPECIFIER_SOFT);
		}
#endif
		return alcGetString (device, ALC_HRTF_SPECIFIER_SOFT);
	}

	ALCcontext *CreateContext (ALCdevice *device, const ALCint *attributes)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return CreateContextTestContext (device, attributes);
		}
#endif
		return alcCreateContext (device, attributes);
	}

	ALCboolean MakeContextCurrent (ALCcontext *context)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return MakeContextTestCurrent (context);
		}
#endif
		return alcMakeContextCurrent (context);
	}

	void DestroyContext (ALCcontext *context)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			DestroyContextTestContext (context);
			return;
		}
#endif
		alcDestroyContext (context);
	}

	ALCboolean CloseContextDevice (ALCdevice *device)
	{
#ifdef OAL_LIFECYCLE_TEST
		if (ContextTestState != NULL)
		{
			return CloseContextTestDevice (device);
		}
#endif
		return alcCloseDevice (device);
	}

	void CloseOpenALContext (ALCdevice *device, ALCcontext *context)
	{
		if (context != NULL)
		{
			if (alcGetCurrentContext () == context)
			{
				alcMakeContextCurrent (NULL);
			}
			DestroyContext (context);
		}
		if (device != NULL)
		{
			CloseContextDevice (device);
		}
	}

	bool CreateOpenALContext (const ALCchar *deviceName, bool hrtfEnabled, OpenALContextState *state)
	{
		ALCint hrtfAttributes[5];
		ALCdevice *device = OpenContextDevice (deviceName);
		if (device == NULL)
		{
			return false;
		}
		bool hrtfAdvertised = IsContextExtensionPresent (device, "ALC_SOFT_HRTF") == ALC_TRUE;
		bool hrtfAttributesRequested = OALBuildHRTFContextAttributes (hrtfAdvertised, hrtfEnabled, (int *)hrtfAttributes);
		if (!hrtfAttributesRequested)
		{
			state->HRTFFailure = OALHRTFCONTEXT_ExtensionAbsent;
			hrtfAttributes[0] = ALC_MAX_AUXILIARY_SENDS;
			hrtfAttributes[1] = 1;
			hrtfAttributes[2] = 0;
		}
		ALCcontext *context = CreateContext (device, hrtfAttributes);
		if (context != NULL && MakeContextCurrent (context))
		{
			state->Device = device;
			state->Context = context;
			state->HRTFAttributesApplied = hrtfAttributesRequested;
			return true;
		}
		if (hrtfAttributesRequested)
		{
			state->HRTFFailure = context == NULL ? OALHRTFCONTEXT_CreateFailed : OALHRTFCONTEXT_MakeCurrentFailed;
		}
		CloseOpenALContext (device, context);
		device = OpenContextDevice (deviceName);
		if (device == NULL)
		{
			return false;
		}
		context = CreateContext (device, NULL);
		if (context == NULL || !MakeContextCurrent (context))
		{
			CloseOpenALContext (device, context);
			return false;
		}
		state->Device = device;
		state->Context = context;
		return true;
	}
}

#ifdef OAL_LIFECYCLE_TEST
OpenALContextTestResult OALTestRunContextInitialization (bool hrtfAdvertised, bool hrtfEnabled,
	OpenALContextTestFailure failure)
{
	OpenALContextTestState testState;
	OpenALContextState contextState;
	testState.Failure = failure;
	memset (&testState.Result, 0, sizeof (testState.Result));
	testState.Result.HRTFAdvertised = hrtfAdvertised;
	ContextTestState = &testState;
	testState.Result.Success = CreateOpenALContext (NULL, hrtfEnabled, &contextState);
	if (testState.Result.Success)
	{
		testState.Result.AttributesApplied = contextState.HRTFAttributesApplied;
		testState.Result.HRTFFailure = contextState.HRTFFailure;
		GetHRTFSpecifier (contextState.Device);
		CloseOpenALContext (contextState.Device, contextState.Context);
	}
	ContextTestState = NULL;
	return testState.Result;
}

bool OALTestCopyHRTFSpecifier (const char *specifier, bool querySucceeded, OpenALCapabilities *capabilities)
{
	return CopyHRTFSpecifier (capabilities, specifier, querySucceeded);
}

OpenALHRTFQueryTestResult OALTestQueryHRTFCapabilities (bool hrtfAdvertised,
	bool activeQuerySucceeded, bool active, bool statusQuerySucceeded, int status)
{
	OpenALHRTFQueryTestResult result;
	OpenALHRTFQueryTestState testState;
	OpenALEFXFunctions efx;
	bool activeKnown = false;
	bool statusKnown = false;
	ALCint activeValue = ALC_FALSE;
	ALCint statusValue = 0;

	memset (&testState, 0, sizeof (testState));
	testState.ActiveQuerySucceeded = activeQuerySucceeded;
	testState.Active = active;
	testState.StatusQuerySucceeded = statusQuerySucceeded;
	testState.Status = status;
	HRTFQueryTestState = &testState;
	if (hrtfAdvertised)
	{
		activeKnown = GetHRTFInteger (NULL, ALC_HRTF_SOFT, &activeValue);
		statusKnown = GetHRTFInteger (NULL, ALC_HRTF_STATUS_SOFT, &statusValue);
	}
	HRTFQueryTestState = NULL;
	result.Capabilities = OALBuildCapabilities (hrtfAdvertised, activeKnown, activeValue == ALC_TRUE,
		statusKnown, statusValue, false, efx, false);
	result.ActiveQueryCount = testState.ActiveQueryCount;
	result.StatusQueryCount = testState.StatusQueryCount;
	return result;
}

OpenALEFXResourceTestResult OALTestInitializeEFXResources (const OpenALEFXFunctions &functions,
	bool filterCallable, int sends, OALGetError getError)
{
	OpenALEFXResourceTestResult result;
	OpenALCapabilities capabilities;
	capabilities.EFX = functions;
	capabilities.EFXCallable = functions.IsCallable ();
	capabilities.EFXFilterCallable = filterCallable && functions.IsFilterCallable ();
	if (capabilities.EFXCallable || capabilities.EFXFilterCallable)
	{
		InitializeEFXResources (&capabilities, &result.Effect, &result.Slot, &result.Filter,
			&result.UsesEAX, sends, getError);
	}
	result.Usable = capabilities.EFXUsable;
	return result;
}

void OALTestFinalizeEFXInitialization (const OpenALEFXFunctions &functions, OpenALEFXResourceTestResult *resources)
{
	OpenALCapabilities capabilities;
	capabilities.EFX = functions;
	capabilities.EFXCallable = functions.IsCallable ();
	capabilities.EFXFilterCallable = functions.IsFilterCallable ();
	capabilities.EFXFilterUsable = resources->Filter != 0;
	capabilities.EFXUsable = resources->Usable;
	FinalizeEFXInitialization (&capabilities, &resources->Effect, &resources->Slot, &resources->UsesEAX);
	resources->Usable = capabilities.EFXUsable;
}

void OALTestReleaseEFXResources (const OpenALEFXFunctions &functions, OpenALEFXResourceTestResult *resources)
{
	OpenALCapabilities capabilities;
	capabilities.EFX = functions;
	capabilities.EFXCallable = functions.IsCallable ();
	capabilities.EFXFilterCallable = functions.IsFilterCallable ();
	capabilities.EFXUsable = resources->Usable;
	ReleaseEFXResources (&capabilities, &resources->Effect, &resources->Slot, &resources->Filter, &resources->UsesEAX);
	resources->Usable = capabilities.EFXUsable;
}
#endif

enum
{
	OALSTREAM_Mono = 1,
	OALSTREAM_Bits8 = 2,
	OALSTREAM_Bits32 = 4,
	OALSTREAM_Float = 8
};

#ifdef OAL_LIFECYCLE_TEST
bool OALTestForceFloatPCM16Fallback = false;
#endif

enum
{
	OALAL_SourceQuery = 1,
	OALAL_SourcePlay,
	OALAL_SourcePause,
	OALAL_BufferUpload,
	OALAL_BufferQueue,
	OALAL_BufferUnqueue,
	OALAL_SourceStop,
	OALAL_SourceGain,
	OALAL_SourcePositionQuery
};

enum OpenALProducerReadStatus
{
	OALPRODUCER_Data,
	OALPRODUCER_EOF,
	OALPRODUCER_Error
};

static_assert(!std::is_copy_constructible<OpenALSoundStream>::value,
	"OpenAL streams must not be copy constructible");
static_assert(!std::is_copy_assignable<OpenALSoundStream>::value,
	"OpenAL streams must not be copy assignable");

class OpenALStreamProducer
{
public:
	virtual ~OpenALStreamProducer () {}
	virtual OpenALProducerReadStatus ReadFrames (OpenALSoundStream *stream, BYTE *data, short *pcm16Data, unsigned int frameCount, unsigned int bytesPerFrame, unsigned int *framesRead) = 0;
	virtual bool UsesTypedPCM16Destination () const { return false; }
	virtual bool GetTotalFrames (unsigned long long *frames) const = 0;
	virtual unsigned long long TellFrame () const = 0;
	virtual bool GetLoopRange (AudioFrameRange *range) const = 0;
	virtual AudioDecoderSeekStatus SeekFrame (unsigned long long frame) = 0;
#ifdef OAL_LIFECYCLE_TEST
	virtual void SetTestAllowArbitrarySeek () {}
#endif
};

class CallbackStreamProducer : public OpenALStreamProducer
{
public:
	CallbackStreamProducer (SoundStreamCallback callback, void *userData)
		: Callback (callback), UserData (userData), Position (0)
#ifdef OAL_LIFECYCLE_TEST
		, TestAllowArbitrarySeek (false)
#endif
	{
	}

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *stream, BYTE *data, short *, unsigned int frameCount, unsigned int bytesPerFrame, unsigned int *framesRead)
	{
		bool hasData;
		if (framesRead == NULL || Callback == NULL)
		{
			return OALPRODUCER_Error;
		}
#ifdef OAL_LIFECYCLE_TEST
		hasData = ((bool (*)(SoundStream *, void *, int, void *))Callback) (stream, data, (int)(frameCount * bytesPerFrame), UserData);
#else
		hasData = Callback (stream, data, (int)(frameCount * bytesPerFrame), UserData);
#endif
		if (!hasData)
		{
			*framesRead = 0;
			return OALPRODUCER_EOF;
		}
		*framesRead = frameCount;
		Position = std::numeric_limits<unsigned long long>::max () - Position < frameCount ?
			std::numeric_limits<unsigned long long>::max () : Position + frameCount;
		return OALPRODUCER_Data;
	}

	bool GetTotalFrames (unsigned long long *) const { return false; }
	unsigned long long TellFrame () const { return Position; }
	bool GetLoopRange (AudioFrameRange *) const { return false; }
	AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
	{
		if (frame != 0
#ifdef OAL_LIFECYCLE_TEST
			&& !TestAllowArbitrarySeek
#endif
			)
		{
			return AUDIO_DECODER_SEEK_ERROR;
		}
		Position = frame;
		return AUDIO_DECODER_SEEK_OK;
	}

#ifdef OAL_LIFECYCLE_TEST
	void SetTestAllowArbitrarySeek ()
	{
		TestAllowArbitrarySeek = true;
	}
#endif

private:
	SoundStreamCallback Callback;
	void *UserData;
	unsigned long long Position;
#ifdef OAL_LIFECYCLE_TEST
	bool TestAllowArbitrarySeek;
#endif
};

class DecoderStreamProducer : public OpenALStreamProducer
{
public:
	explicit DecoderStreamProducer (AudioDecoder *decoder) : Decoder (decoder) {}
	~DecoderStreamProducer () { delete Decoder; }

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *, BYTE *, short *pcm16Data, unsigned int frameCount, unsigned int, unsigned int *framesRead)
	{
		std::size_t read = 0;
		AudioDecoderReadStatus status;
		if (Decoder == NULL || pcm16Data == NULL || framesRead == NULL)
		{
			return OALPRODUCER_Error;
		}
		status = Decoder->ReadFrames (pcm16Data, frameCount, &read);
		if (read > frameCount)
		{
			return OALPRODUCER_Error;
		}
		*framesRead = (unsigned int)read;
		return status == AUDIO_DECODER_DATA ? OALPRODUCER_Data :
			status == AUDIO_DECODER_EOF ? OALPRODUCER_EOF : OALPRODUCER_Error;
	}

	bool GetTotalFrames (unsigned long long *frames) const { return Decoder != NULL && Decoder->GetTotalFrames (frames); }
	bool UsesTypedPCM16Destination () const { return true; }
	unsigned long long TellFrame () const { return Decoder == NULL ? 0 : Decoder->TellFrame (); }
	bool GetLoopRange (AudioFrameRange *range) const { return Decoder != NULL && Decoder->GetLoopRange (range); }
	AudioDecoderSeekStatus SeekFrame (unsigned long long frame) { return Decoder == NULL ? AUDIO_DECODER_SEEK_TERMINAL_ERROR : Decoder->SeekFrame (frame); }

private:
	DecoderStreamProducer (const DecoderStreamProducer &);
	DecoderStreamProducer &operator= (const DecoderStreamProducer &);
	AudioDecoder *Decoder;
};

static_assert(!std::is_copy_constructible<DecoderStreamProducer>::value,
	"Decoder producers must not be copy constructible");
static_assert(!std::is_copy_assignable<DecoderStreamProducer>::value,
	"Decoder producers must not be copy assignable");

#ifdef OAL_LIFECYCLE_TEST
class PatternStreamProducer : public OpenALStreamProducer
{
public:
	PatternStreamProducer (unsigned int totalFrames, unsigned int loopStart, unsigned int loopEnd)
		: TotalFrames (totalFrames), LoopStart (loopStart), LoopEnd (loopEnd), Position (0)
	{
	}

	OpenALProducerReadStatus ReadFrames (OpenALSoundStream *, BYTE *, short *pcm16Data, unsigned int frameCount, unsigned int, unsigned int *framesRead)
	{
		unsigned int available;
		unsigned int count;
		short *samples;
		if (pcm16Data == NULL || framesRead == NULL)
		{
			return OALPRODUCER_Error;
		}
		if (Position >= TotalFrames)
		{
			*framesRead = 0;
			return OALPRODUCER_EOF;
		}
		available = TotalFrames - Position;
		count = frameCount < available ? frameCount : available;
		samples = pcm16Data;
		for (unsigned int index = 0; index < count; ++index)
		{
			samples[index] = (short)(Position + index);
		}
		Position += count;
		*framesRead = count;
		return OALPRODUCER_Data;
	}

	bool GetTotalFrames (unsigned long long *frames) const
	{
		if (frames == NULL)
		{
			return false;
		}
		*frames = TotalFrames;
		return true;
	}

	bool UsesTypedPCM16Destination () const { return true; }

	unsigned long long TellFrame () const { return Position; }
	bool GetLoopRange (AudioFrameRange *range) const
	{
		if (range == NULL || LoopStart >= LoopEnd || LoopEnd > TotalFrames)
		{
			return false;
		}
		range->Start = LoopStart;
		range->End = LoopEnd;
		return true;
	}

	AudioDecoderSeekStatus SeekFrame (unsigned long long frame)
	{
		if (frame > TotalFrames)
		{
			return AUDIO_DECODER_SEEK_ERROR;
		}
		Position = (unsigned int)frame;
		return AUDIO_DECODER_SEEK_OK;
	}

private:
	unsigned int TotalFrames;
	unsigned int LoopStart;
	unsigned int LoopEnd;
	unsigned int Position;
};
#endif

static AudioDataSource *OpenEncodedMusicSource (const char *filename, int offset, int length, AudioFileSource *file, AudioMemorySource *memory)
{
	if (offset == -1)
	{
		if (length == 0 || memory->Assign (filename, (size_t)length) != AUDIO_SOURCE_OK)
		{
			Printf (TEXTCOLOR_RED "OpenAL could not read encoded music data.\n");
			return NULL;
		}
		return memory;
	}
	if ((offset == 0 && length == 0 && file->Open (filename) == AUDIO_SOURCE_OK) ||
		(length > 0 && file->OpenSlice (filename, (size_t)offset, (size_t)length) == AUDIO_SOURCE_OK))
	{
		return file;
	}
	Printf (TEXTCOLOR_RED "OpenAL could not open encoded music data.\n");
	return NULL;
}

static DecoderStreamProducer *CreateEncodedMusicProducer (AudioDataSource *source, unsigned int *channels, unsigned int *sampleRate)
{
	AudioProbeResult probe = ProbeAudioFormat (*source);
	AudioDecoder *decoder = NULL;
	AudioDecodeStatus status = CreateAudioDecoder (*source, probe, &decoder);
	DecoderStreamProducer *producer;
	if (status != AUDIO_DECODE_OK || decoder == NULL || decoder->GetNativeSampleRate () == 0 ||
		(decoder->GetOutputChannels () != 1 && decoder->GetOutputChannels () != 2))
	{
		delete decoder;
		Printf (TEXTCOLOR_RED "OpenAL does not support this encoded music stream.\n");
		return NULL;
	}
	*channels = decoder->GetOutputChannels ();
	*sampleRate = decoder->GetNativeSampleRate ();
	producer = new (std::nothrow) DecoderStreamProducer (decoder);
	if (producer == NULL)
	{
		delete decoder;
	}
	return producer;
}

static unsigned long long SaturatingAdd (unsigned long long left, unsigned long long right)
{
	return std::numeric_limits<unsigned long long>::max () - left < right ?
		std::numeric_limits<unsigned long long>::max () : left + right;
}

static bool ConvertOutputFramesToSampleFrames (unsigned long long outputFrames, unsigned int sampleRate, float pitch, int outputRate, unsigned long long *sampleFrames)
{
	long double converted;
	if (sampleRate == 0 || outputRate <= 0 || pitch != pitch || pitch <= 0.f || pitch > std::numeric_limits<float>::max ())
	{
		return false;
	}
	converted = ((long double)outputFrames * sampleRate * pitch) / outputRate;
	if (converted != converted || converted >= (long double)std::numeric_limits<unsigned long long>::max ())
	{
		*sampleFrames = std::numeric_limits<unsigned long long>::max ();
	}
	else
	{
		*sampleFrames = (unsigned long long)converted;
	}
	return true;
}

static bool CalculateRestartPosition (unsigned long long startPosition, unsigned long long elapsedOutputFrames, unsigned int sampleRate, float pitch, int outputRate, bool looping, unsigned int frames, unsigned int loopStart, unsigned int loopEnd, unsigned int *position)
{
	unsigned long long elapsedSampleFrames;
	unsigned long long sampleFrame;
	if (!ConvertOutputFramesToSampleFrames (elapsedOutputFrames, sampleRate, pitch, outputRate, &elapsedSampleFrames))
	{
		return false;
	}
	sampleFrame = SaturatingAdd (startPosition, elapsedSampleFrames);
	if (!looping)
	{
		if (sampleFrame >= frames)
		{
			return false;
		}
		*position = (unsigned int)sampleFrame;
		return true;
	}
	if (loopEnd <= loopStart)
	{
		return false;
	}
	*position = sampleFrame < loopStart ? (unsigned int)sampleFrame :
		loopStart + (unsigned int)((sampleFrame - loopStart) % (loopEnd - loopStart));
	return true;
}

static unsigned int GetHostMilliseconds ()
{
#ifdef OAL_LIFECYCLE_TEST
	return OALTestMilliseconds ();
#else
	return I_MSTime ();
#endif
}

static FVector3 ToOpenALCoordinates (const FVector3 &vector)
{
	FVector3 converted;
	converted.X = vector.X;
	converted.Y = vector.Y;
	converted.Z = -vector.Z;
	return converted;
}

OpenALSound::OpenALSound ()
	: Buffer2D (0), BufferMono (0), SampleRate (0), Frames (0), Channels (0),
	  HasLoop (false), LoopStart (0), LoopEnd (0), References (0), DeferredDelete (false)
{
}

OpenALChannel::OpenALChannel ()
	: Source (0), Sound (NULL), Owner (NULL), Gain (0.f), Pitch (1.f), Priority (0),
	  RolloffGain (1.f), EffectiveGain (0.f), Distance (0.f), DistanceScale (1.f),
		  CachedPosition (0), LogicalStartFrame (0), AllocationSerial (0), Looping (false), NoPause (false), NoReverb (false), Is3D (false), IsArea (false), HeadRelative (false),
	  WasPlayingBeforePause (false), PauseReasons (0), Rolloff (), EndReason (OALEND_None),
	  FinalizeState (OALFINAL_Active)
{
}

OpenALSoundStream::OpenALSoundStream (OpenALSoundRenderer *owner, SoundStreamCallback callback, int bufferBytes, int flags, int sampleRate, void *userData)
	: Source (0), Owner (owner), Producer (new (std::nothrow) CallbackStreamProducer (callback, userData)), SampleRate ((unsigned int)sampleRate),
	  StreamChannels ((flags & OALSTREAM_Mono) ? 1 : 2), InputBits ((flags & OALSTREAM_Float) ? 32 : (flags & OALSTREAM_Bits32) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16),
	  OutputBits (0), OutputFormat (0), MediaFrame (0), LoopStart (0), LoopEnd (0), Volume (1.f), EndOfInput (false), Looping (false), HasLoopRange (false),
	  UserPaused (false), InactivePaused (false), InputIsFloat ((flags & OALSTREAM_Float) != 0), ResourcesReleased (false), State (OALSTREAM_Stopped)
#ifdef OAL_LIFECYCLE_TEST
	  , TestFailNextRewind (false), TestRewindTerminal (false), TestFailNextBufferUpload (false), TestFailNextALOperation (0)
#endif
{
	InitializeBuffers (bufferBytes, flags);
}

OpenALSoundStream::OpenALSoundStream (OpenALSoundRenderer *owner, OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate)
	: Source (0), Owner (owner), Producer (producer), SampleRate ((unsigned int)sampleRate),
	  StreamChannels ((flags & OALSTREAM_Mono) ? 1 : 2), InputBits ((flags & OALSTREAM_Float) ? 32 : (flags & OALSTREAM_Bits32) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16),
	  OutputBits (0), OutputFormat (0), MediaFrame (0), LoopStart (0), LoopEnd (0), Volume (1.f), EndOfInput (false), Looping (false), HasLoopRange (false),
	  UserPaused (false), InactivePaused (false), InputIsFloat ((flags & OALSTREAM_Float) != 0),
	  ResourcesReleased (false), State (OALSTREAM_Stopped)
#ifdef OAL_LIFECYCLE_TEST
	  , TestFailNextRewind (false), TestRewindTerminal (false), TestFailNextBufferUpload (false), TestFailNextALOperation (0)
#endif
{
	InitializeBuffers (bufferBytes, flags);
}

void OpenALSoundStream::InitializeBuffers (int bufferBytes, int flags)
{
	unsigned int bytesPerInputFrame = StreamChannels * (InputBits / 8);
	memset (Buffers, 0, sizeof (Buffers));
	memset (BufferFrames, 0, sizeof (BufferFrames));
	memset (BufferMediaStart, 0, sizeof (BufferMediaStart));
	InputBuffer.resize ((size_t)bufferBytes);
	if (InputBits == 16)
	{
		PCM16Buffer.resize ((size_t)bufferBytes / sizeof (short));
	}
	if (flags & OALSTREAM_Float)
	{
		OutputBits =
#ifdef OAL_LIFECYCLE_TEST
			!OALTestForceFloatPCM16Fallback &&
#endif
			alIsExtensionPresent ("AL_EXT_FLOAT32") ? 32 : 16;
	}
	else
	{
		OutputBits = InputBits == 32 ? 16 : InputBits;
	}
	if (OutputBits == 32)
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO_FLOAT32 : AL_FORMAT_STEREO_FLOAT32;
	}
	else if (OutputBits == 16)
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	}
	else
	{
		OutputFormat = StreamChannels == 1 ? AL_FORMAT_MONO8 : AL_FORMAT_STEREO8;
	}
	OutputBuffer.resize ((size_t)(bufferBytes / bytesPerInputFrame) * StreamChannels * (OutputBits / 8));
}

OpenALSoundStream::~OpenALSoundStream ()
{
	ReleaseResources ();
	delete Producer;
	Producer = NULL;
	if (Owner != NULL)
	{
		Owner->DestroyStream (this);
	}
}

bool OpenALSoundStream::ConvertBuffer (unsigned int frames)
{
	size_t samples = (size_t)frames * StreamChannels;
	if (InputBits == OutputBits && InputBits != 8)
	{
		if (Producer != NULL && Producer->UsesTypedPCM16Destination ())
		{
			memcpy (&OutputBuffer[0], &PCM16Buffer[0], samples * sizeof (short));
		}
		else
		{
			memcpy (&OutputBuffer[0], &InputBuffer[0], samples * (InputBits / 8));
		}
		return true;
	}
	if (InputBits == 8)
	{
		for (size_t index = 0; index < samples; ++index)
		{
			OutputBuffer[index] = (BYTE)(InputBuffer[index] + 128);
		}
		return true;
	}
	for (size_t index = 0; index < samples; ++index)
	{
		short converted;
		if (InputBits == 32 && OutputBits == 16 && !InputBuffer.empty ())
		{
			if (InputIsFloat)
			{
				float value;
				memcpy (&value, &InputBuffer[index * sizeof (float)], sizeof (value));
				if (value > 1.f)
				{
					value = 1.f;
				}
				else if (value < -1.f)
				{
					value = -1.f;
				}
				converted = (short)(value * 32767.f);
			}
			else
			{
				int32_t value;
				memcpy (&value, &InputBuffer[index * sizeof (value)], sizeof (value));
				converted = (short)(value >> 16);
			}
			memcpy (&OutputBuffer[index * sizeof (converted)], &converted, sizeof (converted));
		}
	}
	return true;
}

bool OpenALSoundStream::QueueBuffer (unsigned int buffer)
{
	unsigned int bufferIndex = 0;
	unsigned int bytesPerFrame = StreamChannels * (InputBits / 8);
	unsigned int framesRead = 0;
	unsigned int requestedFrames = (unsigned int)(InputBuffer.size () / bytesPerFrame);
	unsigned long long mediaStart;
	if (!FindBufferIndex (buffer, &bufferIndex) || Producer == NULL || EndOfInput || State == OALSTREAM_Failed)
	{
		return false;
	}
	if (!ReadBufferFrames (requestedFrames, bytesPerFrame, &mediaStart, &framesRead))
	{
		return false;
	}
	if (!ConvertBuffer (framesRead))
	{
		SetFailed ();
		return false;
	}
	return SubmitBuffer (buffer, bufferIndex, mediaStart, framesRead);
}

bool OpenALSoundStream::FindBufferIndex (unsigned int buffer, unsigned int *bufferIndex) const
{
	for (*bufferIndex = 0; *bufferIndex < 4 && Buffers[*bufferIndex] != buffer; ++*bufferIndex)
	{
	}
	return *bufferIndex < 4;
}

bool OpenALSoundStream::ReadBufferFrames (unsigned int requestedFrames, unsigned int bytesPerFrame, unsigned long long *mediaStart, unsigned int *framesRead)
{
	OpenALProducerReadStatus status;
	*mediaStart = Producer->TellFrame ();
	if (Looping && HasLoopRange && *mediaStart >= LoopEnd)
	{
		if (!RewindProducer (LoopStart))
		{
			return false;
		}
		*mediaStart = Producer->TellFrame ();
	}
	if (Looping && HasLoopRange && *mediaStart < LoopEnd && requestedFrames > LoopEnd - *mediaStart)
	{
		requestedFrames = (unsigned int)(LoopEnd - *mediaStart);
	}
	short *pcm16Data = Producer->UsesTypedPCM16Destination () && !PCM16Buffer.empty () ? &PCM16Buffer[0] : NULL;
	status = Producer->ReadFrames (this, &InputBuffer[0], pcm16Data, requestedFrames, bytesPerFrame, framesRead);
	if (status == OALPRODUCER_EOF && Looping)
	{
		if (!RewindProducer (HasLoopRange ? LoopStart : 0))
		{
			return false;
		}
		*mediaStart = Producer->TellFrame ();
		status = Producer->ReadFrames (this, &InputBuffer[0], pcm16Data, requestedFrames, bytesPerFrame, framesRead);
	}
	if (status == OALPRODUCER_EOF)
	{
		EndOfInput = true;
		return false;
	}
	if (status != OALPRODUCER_Data || *framesRead == 0 || *framesRead > requestedFrames)
	{
		SetFailed ();
		return false;
	}
	return true;
}

bool OpenALSoundStream::SubmitBuffer (unsigned int buffer, unsigned int bufferIndex, unsigned long long mediaStart, unsigned int framesRead)
{
#ifdef OAL_LIFECYCLE_TEST
	if (TestFailNextBufferUpload || ConsumeTestALFailure (OALAL_BufferUpload))
	{
		TestFailNextBufferUpload = false;
		SetFailed ();
		return false;
	}
#endif
	alBufferData (buffer, (ALenum)OutputFormat, &OutputBuffer[0], (ALsizei)(framesRead * StreamChannels * (OutputBits / 8)), (ALsizei)SampleRate);
	if (!CheckALError (OALAL_BufferUpload))
	{
		return false;
	}
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (OALAL_BufferQueue))
	{
		return false;
	}
#endif
	alSourceQueueBuffers (Source, 1, (ALuint *)&buffer);
	if (!CheckALError (OALAL_BufferQueue))
	{
		return false;
	}
	BufferFrames[bufferIndex] = framesRead;
	BufferMediaStart[bufferIndex] = mediaStart;
	QueueOrder.push_back (buffer);
	return true;
}

bool OpenALSoundStream::RecycleProcessedBuffer ()
{
	ALuint buffer;
	unsigned int bufferIndex = 0;
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (OALAL_BufferUnqueue))
	{
		return false;
	}
#endif
	alSourceUnqueueBuffers (Source, 1, &buffer);
	if (!CheckALError (OALAL_BufferUnqueue))
	{
		return false;
	}
	if (FindBufferIndex (buffer, &bufferIndex))
	{
		MediaFrame = NormalizeMediaFrame (BufferMediaStart[bufferIndex] + BufferFrames[bufferIndex]);
		BufferFrames[bufferIndex] = 0;
		if (!QueueOrder.empty ())
		{
			QueueOrder.erase (QueueOrder.begin ());
		}
		QueueBuffer (buffer);
		if (State == OALSTREAM_Failed)
		{
			return false;
		}
	}
	return true;
}

void OpenALSoundStream::UpdatePlaybackState ()
{
	ALint queued = 0;
	ALint state = AL_STOPPED;
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if (EndOfInput && queued == 0)
	{
		State = OALSTREAM_Ended;
		return;
	}
	if (EndOfInput && State != OALSTREAM_Paused)
	{
		State = OALSTREAM_Draining;
	}
	alGetSourcei (Source, AL_SOURCE_STATE, &state);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if ((State == OALSTREAM_Playing || State == OALSTREAM_Draining) && !UserPaused && !InactivePaused && queued > 0 && state != AL_PLAYING)
	{
		alSourcePlay (Source);
		CheckALError (OALAL_SourcePlay);
	}
}

void OpenALSoundStream::Update ()
{
	ALint processed = 0;
	if (ResourcesReleased || State == OALSTREAM_Ended || State == OALSTREAM_Failed)
	{
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_PROCESSED, &processed);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	while (processed-- > 0)
	{
		if (!RecycleProcessedBuffer ())
		{
			return;
		}
	}
	UpdatePlaybackState ();
}

bool OpenALSoundStream::CheckALError (unsigned int operation)
{
#ifdef OAL_LIFECYCLE_TEST
	if (ConsumeTestALFailure (operation)) return false;
#endif
	if (alGetError () == AL_NO_ERROR)
	{
		return true;
	}
	SetFailed ();
	return false;
}

#ifdef OAL_LIFECYCLE_TEST
bool OpenALSoundStream::ConsumeTestALFailure (unsigned int operation)
{
	if (TestFailNextALOperation != operation)
	{
		return false;
	}
	TestFailNextALOperation = 0;
	SetFailed ();
	return true;
}
#endif

void OpenALSoundStream::SetFailed ()
{
	if (State == OALSTREAM_Failed)
	{
		return;
	}
	State = OALSTREAM_Failed;
	EndOfInput = true;
	ClearQueuedBuffers (false);
}

bool OpenALSoundStream::RewindProducer (unsigned long long frame)
{
	#ifdef OAL_LIFECYCLE_TEST
	if (TestFailNextRewind)
	{
		bool terminal = TestRewindTerminal;
		TestFailNextRewind = false;
		if (terminal)
		{
			SetFailed ();
		}
		return false;
	}
	#endif
	AudioDecoderSeekStatus status = Producer == NULL ? AUDIO_DECODER_SEEK_TERMINAL_ERROR : Producer->SeekFrame (frame);
	if (status == AUDIO_DECODER_SEEK_OK)
	{
		return true;
	}
	if (status == AUDIO_DECODER_SEEK_TERMINAL_ERROR)
	{
		SetFailed ();
	}
	return false;
}

bool OpenALSoundStream::ConfigureLoop ()
{
	unsigned long long total = 0;
	AudioFrameRange range;
	HasLoopRange = false;
	LoopStart = 0;
	LoopEnd = 0;
	if (!Looping || Producer == NULL)
	{
		return true;
	}
	if (!Producer->GetTotalFrames (&total) || total == 0)
	{
		return true;
	}
	if (Producer->GetLoopRange (&range) && range.Start < range.End && range.End <= total)
	{
		LoopStart = range.Start;
		LoopEnd = range.End;
	}
	else
	{
		LoopEnd = total;
	}
	HasLoopRange = true;
	return true;
}

unsigned long long OpenALSoundStream::NormalizeMediaFrame (unsigned long long frame) const
{
	if (Looping && HasLoopRange && frame >= LoopStart && LoopEnd > LoopStart)
	{
		return LoopStart + (frame - LoopStart) % (LoopEnd - LoopStart);
	}
	return frame;
}

bool OpenALSoundStream::ClearQueuedBuffers (bool failOnError)
{
	ALint queued = 0;
	ALenum error;
	if (ResourcesReleased)
	{
		return false;
	}
	alGetError ();
	alSourceStop (Source);
	error = alGetError ();
	if (error != AL_NO_ERROR)
	{
		if (failOnError)
		{
			SetFailed ();
		}
		return false;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	error = alGetError ();
	if (error != AL_NO_ERROR)
	{
		if (failOnError)
		{
			SetFailed ();
		}
		return false;
	}
	while (queued-- > 0)
	{
		ALuint buffer;
		alSourceUnqueueBuffers (Source, 1, &buffer);
		if (alGetError () != AL_NO_ERROR)
		{
			if (failOnError)
			{
				SetFailed ();
			}
			return false;
		}
	}
	memset (BufferFrames, 0, sizeof (BufferFrames));
	memset (BufferMediaStart, 0, sizeof (BufferMediaStart));
	QueueOrder.clear ();
	return true;
}

unsigned long long OpenALSoundStream::GetCurrentMediaFrame ()
{
	ALint offset = 0;
	if (!ResourcesReleased && (State == OALSTREAM_Playing || State == OALSTREAM_Draining) && !QueueOrder.empty ())
	{
		unsigned int buffer = QueueOrder[0];
		unsigned int index = 0;
		for (; index < 4 && Buffers[index] != buffer; ++index) {}
		alGetSourcei (Source, AL_SAMPLE_OFFSET, &offset);
		if (!CheckALError (OALAL_SourcePositionQuery))
		{
			return MediaFrame;
		}
		if (index < 4 && offset > 0)
		{
			unsigned int limited = (unsigned int)offset > BufferFrames[index] ? BufferFrames[index] : (unsigned int)offset;
			MediaFrame = NormalizeMediaFrame (BufferMediaStart[index] + limited);
		}
	}
	return MediaFrame;
}

void OpenALSoundStream::ApplyGain ()
{
	bool muted = false;
	if (Owner != NULL)
	{
#ifdef OAL_LIFECYCLE_TEST
		muted = Owner->InactiveState == INACTIVE_Mute;
#else
		muted = Owner->InactiveState == SoundRenderer::INACTIVE_Mute;
#endif
	}
	float gain = InactivePaused || muted ? 0.f : Volume * Owner->MusicVolume;
	if (!ResourcesReleased)
	{
		alSourcef (Source, AL_GAIN, gain);
		CheckALError (OALAL_SourceGain);
	}
}

void OpenALSoundStream::ApplyPauseState ()
{
	ALint queued = 0;
	if ((State != OALSTREAM_Playing && State != OALSTREAM_Draining && State != OALSTREAM_Paused) || ResourcesReleased)
	{
		return;
	}
	if (UserPaused || InactivePaused)
	{
		alSourcePause (Source);
		if (!CheckALError (OALAL_SourcePause))
		{
			return;
		}
		State = OALSTREAM_Paused;
		return;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return;
	}
	if (queued > 0)
	{
		alSourcePlay (Source);
		if (!CheckALError (OALAL_SourcePlay))
		{
			return;
		}
		if (State == OALSTREAM_Paused)
		{
			State = EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing;
		}
	}
}

bool OpenALSoundStream::Play (bool looping, float volume)
{
	ALint queued = 0;
	if (ResourcesReleased || State == OALSTREAM_Failed || Producer == NULL)
	{
		return false;
	}
	if (State == OALSTREAM_Ended)
	{
		if (!RewindProducer (0))
		{
			return false;
		}
		EndOfInput = false;
		MediaFrame = 0;
		UserPaused = false;
		State = OALSTREAM_Stopped;
	}
	Looping = looping;
	if (!ConfigureLoop ())
	{
		return false;
	}
	Volume = volume;
	ApplyGain ();
	if (State == OALSTREAM_Failed)
	{
		return false;
	}
	alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	if (!CheckALError (OALAL_SourceQuery))
	{
		return false;
	}
	if (queued == 0)
	{
		for (unsigned int index = 0; index < 4; ++index)
		{
			QueueBuffer (Buffers[index]);
			if (State == OALSTREAM_Failed)
			{
				return false;
			}
		}
		alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
		if (!CheckALError (OALAL_SourceQuery))
		{
			return false;
		}
		if (queued == 0)
		{
			State = EndOfInput ? OALSTREAM_Ended : OALSTREAM_Stopped;
			return false;
		}
	}
	State = EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing;
	ApplyPauseState ();
	return State != OALSTREAM_Failed;
}

void OpenALSoundStream::Stop ()
{
	if (ResourcesReleased || State == OALSTREAM_Failed)
	{
		return;
	}
	if (!RewindProducer (0))
	{
		return;
	}
	if (!ClearQueuedBuffers ())
	{
		return;
	}
	MediaFrame = 0;
	EndOfInput = false;
	Looping = false;
	HasLoopRange = false;
	UserPaused = false;
	State = OALSTREAM_Stopped;
}

void OpenALSoundStream::SetVolume (float volume)
{
	Volume = volume;
	ApplyGain ();
}

bool OpenALSoundStream::SetPaused (bool paused)
{
	if (ResourcesReleased || State == OALSTREAM_Failed || State == OALSTREAM_Ended ||
		(State != OALSTREAM_Playing && State != OALSTREAM_Draining && State != OALSTREAM_Paused))
	{
		return false;
	}
	GetCurrentMediaFrame ();
	if (State == OALSTREAM_Failed)
	{
		return false;
	}
	UserPaused = paused;
	ApplyPauseState ();
	return State != OALSTREAM_Failed;
}

unsigned int OpenALSoundStream::GetPosition ()
{
	unsigned long long frames = GetCurrentMediaFrame ();
	if (SampleRate == 0)
	{
		return 0;
	}
	unsigned long long seconds = frames / SampleRate;
	unsigned long long remainderFrames = frames % SampleRate;
	if (seconds > std::numeric_limits<unsigned int>::max () / 1000)
	{
		return std::numeric_limits<unsigned int>::max ();
	}
	unsigned long long milliseconds = seconds * 1000 + (remainderFrames * 1000) / SampleRate;
	return milliseconds > std::numeric_limits<unsigned int>::max () ?
		std::numeric_limits<unsigned int>::max () : (unsigned int)milliseconds;
}

bool OpenALSoundStream::IsEnded ()
{
	return State == OALSTREAM_Ended || State == OALSTREAM_Failed;
}

#ifdef OAL_LIFECYCLE_TEST
void OpenALSoundStream::SetProcessedFramesForTest (unsigned long long frames)
{
	MediaFrame = frames;
}

void OpenALSoundStream::FailNextRewindForTest (bool terminal)
{
	TestFailNextRewind = true;
	TestRewindTerminal = terminal;
}

void OpenALSoundStream::FailNextBufferUploadForTest ()
{
	TestFailNextBufferUpload = true;
}

void OpenALSoundStream::FailNextALOperationForTest (OpenALStreamTestALOperation operation)
{
	switch (operation)
	{
	case OALTESTAL_SourceQuery: TestFailNextALOperation = OALAL_SourceQuery; break;
	case OALTESTAL_SourcePlay: TestFailNextALOperation = OALAL_SourcePlay; break;
	case OALTESTAL_SourcePause: TestFailNextALOperation = OALAL_SourcePause; break;
	case OALTESTAL_BufferUpload: TestFailNextALOperation = OALAL_BufferUpload; break;
	case OALTESTAL_BufferQueue: TestFailNextALOperation = OALAL_BufferQueue; break;
	case OALTESTAL_BufferUnqueue: TestFailNextALOperation = OALAL_BufferUnqueue; break;
	case OALTESTAL_PositionQuery: TestFailNextALOperation = OALAL_SourcePositionQuery; break;
	default: TestFailNextALOperation = 0; break;
	}
}

bool OpenALSoundStream::ProcessNextBufferForTest ()
{
	return RecycleProcessedBuffer ();
}

void OpenALSoundStream::AllowArbitrarySeekForTest ()
{
	if (Producer != NULL)
	{
		Producer->SetTestAllowArbitrarySeek ();
	}
}

OpenALStreamState OpenALSoundStream::GetStateForTest () const
{
	return State;
}

unsigned int OpenALSoundStream::GetQueuedBufferCountForTest () const
{
	ALint queued = 0;
	if (!ResourcesReleased)
	{
		alGetSourcei (Source, AL_BUFFERS_QUEUED, &queued);
	}
	return queued > 0 ? (unsigned int)queued : 0;
}

unsigned long long OpenALSoundStream::GetBufferMediaStartForTest (unsigned int buffer) const
{
	return buffer < 4 ? BufferMediaStart[buffer] : 0;
}

unsigned int OpenALSoundStream::GetBufferFramesForTest (unsigned int buffer) const
{
	return buffer < 4 ? BufferFrames[buffer] : 0;
}

unsigned long long OpenALSoundStream::GetMediaFrameForTest () const
{
	return MediaFrame;
}
#endif

bool OpenALSoundStream::SetPosition (unsigned int milliseconds)
{
	unsigned long long total;
	unsigned long long frame;
	OpenALStreamState oldState;
	bool wasUserPaused;
	bool wasInactivePaused;
	bool hasTotal;
	if (ResourcesReleased || State == OALSTREAM_Failed || Producer == NULL || SampleRate == 0)
	{
		return false;
	}
	frame = ((unsigned long long)milliseconds * SampleRate) / 1000;
	hasTotal = Producer->GetTotalFrames (&total);
	if (hasTotal && frame > total)
	{
		return false;
	}
	if (!RewindProducer (frame))
	{
		return false;
	}
	oldState = State;
	wasUserPaused = UserPaused;
	wasInactivePaused = InactivePaused;
	if (!ClearQueuedBuffers ())
	{
		return false;
	}
	MediaFrame = frame;
	EndOfInput = hasTotal && frame == total;
	if (EndOfInput)
	{
		State = OALSTREAM_Ended;
		return true;
	}
	State = oldState == OALSTREAM_Ended ? OALSTREAM_Stopped : oldState;
	if (State == OALSTREAM_Stopped)
	{
		return true;
	}
	for (unsigned int index = 0; index < 4; ++index)
	{
		QueueBuffer (Buffers[index]);
		if (State == OALSTREAM_Failed)
		{
			return false;
		}
	}
	UserPaused = wasUserPaused;
	InactivePaused = wasInactivePaused;
	State = UserPaused || InactivePaused ? OALSTREAM_Paused : (EndOfInput ? OALSTREAM_Draining : OALSTREAM_Playing);
	ApplyPauseState ();
	return true;
}

void OpenALSoundStream::SetInactive (bool paused)
{
	InactivePaused = paused;
	ApplyGain ();
	ApplyPauseState ();
}

void OpenALSoundStream::ReleaseResources ()
{
	if (ResourcesReleased)
	{
		return;
	}
	ResourcesReleased = true;
	if (Source != 0)
	{
		alSourceStop (Source);
		alDeleteSources (1, (ALuint *)&Source);
		Source = 0;
	}
	alDeleteBuffers (4, (ALuint *)Buffers);
	memset (Buffers, 0, sizeof (Buffers));
}

OpenALSoundRenderer::OpenALSoundRenderer ()
	: Device (NULL), Context (NULL), Capabilities (), EFXEffect (0), EFXSlot (0), EFXFilter (0), Sources (NULL), RequestedSources (0),
	  AllocatedSources (0), OutputRate (0), InitSuccess (false), HRTFAttributesApplied (false), HRTFRequestedEnabled (false), HRTFFailure (OALHRTFCONTEXT_NoFailure), SfxVolume (1.f),
		MusicVolume (1.f), NextAllocationSerial (0), NextLogicalPositionToken (~0ull), PausableOutputFrames (0),
	  NonPausableOutputFrames (0), PausableFrameRemainder (0), NonPausableFrameRemainder (0),
	  LastClockMilliseconds (0), SfxPaused (0), InactiveState (INACTIVE_Active),
	  SyncPaused (false), PendingStartNoPause (false), WaterPitchActive (false), WaterFilterActive (false), WaterFilterGainHF (0.f), EFXEnvironmentInitialized (false), EFXFailureDraining (false),
	  LastAttemptedEnvironment (NULL), LastAppliedEnvironment (NULL), EFXFailure (OALEFXFAIL_None), EFXUsesEAX (false), InvalidReverbPanWarned (false)
#ifdef OAL_LIFECYCLE_TEST
	, FailNextStart (false), FailNextStartSetup (false), FailNextPositionQuery (false), FailNextSpatialState (false), FailNextSpatialRadius (false), PersistentSpatialRadiusFailure (false), SpatialRadiusFailureCalls (0), SpatialRadiusFailureCallLimitExceeded (false), FailNextEFXSourceAssign (OALEFXFAIL_None), PersistentEFXSourceFailure (false),
	  FailEFXSourceAssignSource (0), EFXSourceFailureCalls (0), EFXSourceFailureCallLimitExceeded (false), LastEFXSource (0), LastEFXSlot (0),
	  LastEFXSend (0), LastEFXFilter (0), LastEFXSourceError (AL_NO_ERROR), LastEFXSourceFailureError (AL_NO_ERROR),
	  LastDirectFilterSource (0), LastDirectFilter (0), LastDirectFilterError (AL_NO_ERROR)
#endif
{
	InitSuccess = Init ();
	LastClockMilliseconds = GetHostMilliseconds ();
}

OpenALSoundRenderer::~OpenALSoundRenderer ()
{
	Shutdown ();
}

bool OpenALSoundRenderer::Init ()
{
	InvalidReverbPanWarned = false;
	const char *requestedDevice = *snd_openal_device;
	const char *deviceName = NULL;
	OpenALContextState contextState;
	ALCdevice *device;
	ALCcontext *context;

	if (stricmp (requestedDevice, "default") != 0)
	{
		deviceName = FindDeviceName (requestedDevice);
		if (deviceName == NULL)
		{
			Printf (TEXTCOLOR_RED "OpenAL device '%s' was not found. Falling back to FMOD.\n", requestedDevice);
			return false;
		}
	}

	if (!CreateOpenALContext (deviceName, snd_hrtf, &contextState))
	{
		Printf (TEXTCOLOR_RED "OpenAL could not create a current context for device '%s'. Falling back to FMOD.\n", requestedDevice);
		return false;
	}
	device = contextState.Device;
	context = contextState.Context;
	Device = device;
	DeviceName = alcGetString (device, ALC_DEVICE_SPECIFIER);
	Context = context;
	HRTFAttributesApplied = contextState.HRTFAttributesApplied;
	HRTFRequestedEnabled = snd_hrtf;
	HRTFFailure = contextState.HRTFFailure;
	const ALchar *version = alGetString (AL_VERSION);
	OpenALVersion = version != NULL ? version : "unknown";
	Capabilities = CollectOpenALCapabilities (device);
	InitializeRendererEFXResources (&Capabilities, device, &EFXEffect, &EFXSlot, &EFXFilter, &EFXUsesEAX);
	FinalizeEFXInitialization (&Capabilities, &EFXEffect, &EFXSlot, &EFXUsesEAX);

	if (!alIsExtensionPresent ("AL_SOFT_loop_points"))
	{
		Printf (TEXTCOLOR_RED "OpenAL device '%s' lacks AL_SOFT_loop_points. Falling back to FMOD.\n", DeviceName.GetChars());
		return false;
	}

	alDistanceModel (AL_NONE);
	alDopplerFactor (0.f);
	if (alGetError () != AL_NO_ERROR)
	{
		Printf (TEXTCOLOR_RED "OpenAL could not configure distance attenuation and Doppler. Falling back to FMOD.\n");
		return false;
	}

	RequestedSources = snd_channels > 0 ? snd_channels : 1;
	Sources = new unsigned int[RequestedSources];
	for (int i = 0; i < RequestedSources; ++i)
	{
		ALuint source;
		alGetError ();
		alGenSources (1, &source);
		if (alGetError () != AL_NO_ERROR)
		{
			break;
		}
		Sources[AllocatedSources++] = source;
	}

	if (AllocatedSources == 0)
	{
		Printf (TEXTCOLOR_RED "OpenAL could not allocate an SFX source. Falling back to FMOD.\n");
		return false;
	}

	alcGetIntegerv (device, ALC_FREQUENCY, 1, &OutputRate);
	if (alcGetError (device) != ALC_NO_ERROR)
	{
		OutputRate = 0;
	}
	return true;
}

void OpenALSoundRenderer::Shutdown ()
{
	ALCcontext *context = (ALCcontext *)Context;
	ALCdevice *device = (ALCdevice *)Device;

	if (context != NULL)
	{
		alcMakeContextCurrent (context);
		while (!ActiveStreams.empty ())
		{
			delete ActiveStreams.back ();
		}
		while (!ActiveChannels.empty ())
		{
			FinalizeChannel (ActiveChannels[0], OALEND_BackendError);
		}
		if (Sources != NULL && AllocatedSources > 0)
		{
			alDeleteSources (AllocatedSources, (ALuint *)Sources);
		}
		ReleaseEFXResources ();
	}
	EFXEffect = 0;
	EFXSlot = 0;
	EFXFilter = 0;
	EFXUsesEAX = false;
	InvalidReverbPanWarned = false;
	EFXEnvironmentInitialized = false;
	LastAttemptedEnvironment = NULL;
	LastAppliedEnvironment = NULL;
	delete[] Sources;
	Sources = NULL;
	AllocatedSources = 0;
	RequestedSources = 0;

	if (context != NULL)
	{
		alcMakeContextCurrent (NULL);
		alcDestroyContext (context);
	}
	Context = NULL;
	if (device != NULL)
	{
		alcCloseDevice (device);
	}
	Device = NULL;
	InitSuccess = false;
}

const char *OpenALSoundRenderer::FindDeviceName (const char *name) const
{
	const ALCchar *devices;
	ALCenum specifier;

	if (alcIsExtensionPresent (NULL, "ALC_ENUMERATE_ALL_EXT"))
	{
		specifier = ALC_ALL_DEVICES_SPECIFIER;
	}
	else if (alcIsExtensionPresent (NULL, "ALC_ENUMERATION_EXT"))
	{
		specifier = ALC_DEVICE_SPECIFIER;
	}
	else
	{
		Printf (TEXTCOLOR_RED "OpenAL cannot enumerate named devices on this system.\n");
		return NULL;
	}

	devices = alcGetString (NULL, specifier);
	for (const ALCchar *device = devices; device != NULL && *device != '\0'; device += strlen (device) + 1)
	{
		if (strcmp (device, name) == 0)
		{
			return device;
		}
	}
	return NULL;
}

void OpenALSoundRenderer::SetSfxVolume (float volume)
{
	SfxVolume = volume;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		ApplyChannelGain (ActiveChannels[index]);
	}
}

void OpenALSoundRenderer::SetMusicVolume (float volume)
{
	MusicVolume = volume;
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->ApplyGain ();
	}
}

SoundHandle OpenALSoundRenderer::CreateSound (const OALPCMData &data)
{
	SoundHandle handle = { NULL };
	OpenALSound *sound;
	ALenum format;
	ALint loopPoints[2];

	if (data.SampleRate > (unsigned int)std::numeric_limits<ALsizei>::max () ||
		data.Samples.size () > (size_t)std::numeric_limits<ALsizei>::max () / sizeof (short))
	{
		return handle;
	}
	sound = new OpenALSound;
	sound->SampleRate = data.SampleRate;
	sound->Frames = data.Frames;
	sound->Channels = data.Channels;
	sound->HasLoop = data.HasLoop;
	sound->LoopStart = data.LoopStart;
	sound->LoopEnd = data.LoopEnd;
	format = data.Channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

	alGetError ();
	alGenBuffers (1, (ALuint *)&sound->Buffer2D);
	alBufferData (sound->Buffer2D, format, &data.Samples[0], (ALsizei)(data.Samples.size () * sizeof (short)), (ALsizei)data.SampleRate);
	if (data.HasLoop)
	{
		loopPoints[0] = (ALint)data.LoopStart;
		loopPoints[1] = (ALint)data.LoopEnd;
		alBufferiv (sound->Buffer2D, AL_LOOP_POINTS_SOFT, loopPoints);
	}
	if (alGetError () != AL_NO_ERROR)
	{
		DestroySound (sound);
		return handle;
	}
	if (data.Channels == 2)
	{
		OALPCMResult mono = OALDownmixToMono (data);
		if (!mono.IsValid ())
		{
			DestroySound (sound);
			return handle;
		}
		alGenBuffers (1, (ALuint *)&sound->BufferMono);
		alBufferData (sound->BufferMono, AL_FORMAT_MONO16, &mono.Data.Samples[0], (ALsizei)(mono.Data.Samples.size () * sizeof (short)), (ALsizei)mono.Data.SampleRate);
		if (data.HasLoop)
		{
			alBufferiv (sound->BufferMono, AL_LOOP_POINTS_SOFT, loopPoints);
		}
		if (alGetError () != AL_NO_ERROR)
		{
			DestroySound (sound);
			return handle;
		}
	}
	handle.data = sound;
	return handle;
}

SoundHandle OpenALSoundRenderer::LoadSound (BYTE *sfxdata, int length)
{
	SoundHandle handle = { NULL };
	AudioMemorySource source;
	AudioProbeResult probe;
	AudioDecodedPCM16 decoded;
	OALPCMData data;
	if (length <= 0 || !AcceptsOpenALEncodedInputSize ((std::size_t)length))
	{
		return handle;
	}
	OALPCMResult result = OALParseWavePCM (sfxdata, (size_t)length);
	if (result.IsValid ())
	{
		return CreateSound (result.Data);
	}
	if (!AssignOpenALEncodedSource (source, sfxdata, (size_t)length))
	{
		return handle;
	}
	probe = ProbeAudioFormat (source);
	if (probe.Status != AUDIO_PROBE_RECOGNIZED ||
		(probe.Format != AUDIO_FORMAT_OGG_VORBIS && probe.Format != AUDIO_FORMAT_MPEG &&
			probe.Format != AUDIO_FORMAT_FLAC && probe.Format != AUDIO_FORMAT_WAVE_FLOAT) ||
		DecodeAudioToPCM16 (source, probe, &decoded) != AUDIO_DECODE_OK || decoded.SampleRate == 0 ||
		(decoded.Channels != 1 && decoded.Channels != 2) || decoded.Samples.empty () ||
		decoded.Samples.size () / decoded.Channels > (size_t)std::numeric_limits<unsigned int>::max ())
	{
		DPrintf ("OpenAL could not load encoded sample: error %d\n", result.Error);
		return handle;
	}
	data.Samples.swap (decoded.Samples);
	data.SampleRate = decoded.SampleRate;
	data.Channels = decoded.Channels;
	data.Frames = (unsigned int)(data.Samples.size () / data.Channels);
	return CreateSound (data);
}

short *OpenALSoundRenderer::DecodeSample (int outlen, const void *coded, int sizebytes, ECodecType type)
{
	AudioMemorySource source;
	AudioProbeResult probe;
	AudioDecodedPCM16 decoded;
	short *outbuf;
	std::size_t outputBytes;
	std::size_t decodedBytes;

	if (outlen <= 0 || coded == NULL || sizebytes <= 0 || type != CODEC_Vorbis ||
		!AssignOpenALEncodedSource (source, coded, (size_t)sizebytes))
	{
		return NULL;
	}
	probe = ProbeAudioFormat (source);
	if (probe.Status != AUDIO_PROBE_RECOGNIZED || probe.Format != AUDIO_FORMAT_OGG_VORBIS ||
		DecodeAudioToPCM16 (source, probe, &decoded) != AUDIO_DECODE_OK || decoded.Channels != 1)
	{
		return NULL;
	}
	outputBytes = (std::size_t)outlen;
	outbuf = (short *)malloc (outputBytes);
	if (outbuf == NULL)
	{
		return NULL;
	}
	memset (outbuf, 0, outputBytes);
	decodedBytes = decoded.Samples.size () * sizeof (short);
	if (decodedBytes != 0)
	{
		memcpy (outbuf, &decoded.Samples[0], decodedBytes < outputBytes ? decodedBytes : outputBytes);
	}
	return outbuf;
}

SoundHandle OpenALSoundRenderer::LoadSoundRaw (BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
	SoundHandle handle = { NULL };
	if (length <= 0)
	{
		return handle;
	}
	OALPCMResult result = OALConvertRawPCM (sfxdata, (size_t)length, (unsigned int)frequency, (unsigned int)channels, bits, loopstart, loopend);
	if (!result.IsValid ())
	{
		DPrintf ("OpenAL could not load raw sample: error %d\n", result.Error);
		return handle;
	}
	return CreateSound (result.Data);
}

void OpenALSoundRenderer::DestroySound (OpenALSound *sound)
{
	if (sound == NULL)
	{
		return;
	}
	if (sound->BufferMono != 0 && sound->BufferMono != sound->Buffer2D)
	{
		alDeleteBuffers (1, (ALuint *)&sound->BufferMono);
	}
	if (sound->Buffer2D != 0)
	{
		alDeleteBuffers (1, (ALuint *)&sound->Buffer2D);
	}
	delete sound;
}

void OpenALSoundRenderer::UnloadSound (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	if (sound == NULL)
	{
		return;
	}
	for (size_t index = 0; index < LogicalPositions.size (); )
	{
		if (LogicalPositions[index].Sound == sound)
		{
			LogicalPositions.erase (LogicalPositions.begin () + index);
		}
		else
		{
			++index;
		}
	}
	sound->DeferredDelete = true;
	if (sound->References == 0)
	{
		DestroySound (sound);
	}
}

unsigned int OpenALSoundRenderer::GetMSLength (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	if (sound == NULL || sound->SampleRate == 0)
	{
		return 0;
	}
	return (unsigned int)(((unsigned long long)sound->Frames * 1000) / sound->SampleRate);
}

unsigned int OpenALSoundRenderer::GetSampleLength (SoundHandle sfx)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	return sound == NULL ? 0 : sound->Frames;
}

float OpenALSoundRenderer::GetOutputRate ()
{
	return (float)OutputRate;
}

SoundStream *OpenALSoundRenderer::CreateStream (SoundStreamCallback callback, int bufferBytes, int flags, int sampleRate, void *userData)
{
	unsigned int inputBits;
	unsigned int channels;
	unsigned int bytesPerFrame;
	if (!InitSuccess || callback == NULL || bufferBytes <= 0 || sampleRate <= 0 ||
		(flags & ~(OALSTREAM_Mono | OALSTREAM_Bits8 | OALSTREAM_Bits32 | OALSTREAM_Float)) != 0 ||
		((flags & OALSTREAM_Bits8) != 0 && (flags & (OALSTREAM_Bits32 | OALSTREAM_Float)) != 0) ||
		((flags & OALSTREAM_Bits32) != 0 && (flags & OALSTREAM_Float) != 0))
	{
		return NULL;
	}
	channels = (flags & OALSTREAM_Mono) ? 1 : 2;
	inputBits = (flags & (OALSTREAM_Bits32 | OALSTREAM_Float)) ? 32 : (flags & OALSTREAM_Bits8) ? 8 : 16;
	bytesPerFrame = channels * (inputBits / 8);
	if (bufferBytes % (int)bytesPerFrame != 0)
	{
		return NULL;
	}
	return CreateStreamWithProducer (new (std::nothrow) CallbackStreamProducer (callback, userData), bufferBytes, flags, sampleRate);
}

OpenALSoundStream *OpenALSoundRenderer::CreateStreamWithProducer (OpenALStreamProducer *producer, int bufferBytes, int flags, int sampleRate)
{
	OpenALSoundStream *stream;
	ALuint source = 0;
	ALuint buffers[4];
	if (producer == NULL)
	{
		return NULL;
	}
	memset (buffers, 0, sizeof (buffers));
	alGetError ();
	alGenSources (1, &source);
	alGenBuffers (4, buffers);
	if (alGetError () != AL_NO_ERROR)
	{
		if (source != 0)
		{
			alDeleteSources (1, &source);
		}
		alDeleteBuffers (4, buffers);
		delete producer;
		return NULL;
	}
	stream = new (std::nothrow) OpenALSoundStream (this, producer, bufferBytes, flags, sampleRate);
	if (stream == NULL)
	{
		alDeleteSources (1, &source);
		alDeleteBuffers (4, buffers);
		delete producer;
		return NULL;
	}
	stream->Source = source;
	memcpy (stream->Buffers, buffers, sizeof (buffers));
	if (!ResetEFXSource (source))
	{
		delete stream;
		return NULL;
	}
	stream->SetInactive (InactiveState == INACTIVE_Complete);
	ActiveStreams.push_back (stream);
	return stream;
}

#ifdef OAL_LIFECYCLE_TEST
OpenALSoundStream *OpenALSoundRenderer::CreatePatternStreamForTest (unsigned int totalFrames, unsigned int loopStart, unsigned int loopEnd, int bufferBytes)
{
	return CreateStreamWithProducer (new (std::nothrow) PatternStreamProducer (totalFrames, loopStart, loopEnd), bufferBytes, OALSTREAM_Mono, 8000);
}
#endif

SoundStream *OpenALSoundRenderer::OpenStream (const char *filename, int flags, int offset, int length)
{
	AudioFileSource file;
	AudioMemorySource memory;
	AudioDataSource *source;
	DecoderStreamProducer *producer;
	unsigned int channels = 0;
	unsigned int sampleRate = 0;
	if (!InitSuccess || filename == NULL || (flags & ~SoundStream::Loop) != 0 || length < 0 || offset < -1 ||
		(offset == 0 && length == 0 && strstr (filename, "://") != NULL))
	{
		Printf (TEXTCOLOR_RED "OpenAL cannot open this encoded music stream.\n");
		return NULL;
	}
	source = OpenEncodedMusicSource (filename, offset, length, &file, &memory);
	if (source == NULL)
	{
		return NULL;
	}
	producer = CreateEncodedMusicProducer (source, &channels, &sampleRate);
	if (producer == NULL)
	{
		return NULL;
	}
	return CreateStreamWithProducer (producer, 4096 * channels * (int)sizeof (short),
		channels == 1 ? OALSTREAM_Mono : 0, (int)sampleRate);
}

bool OpenALSoundRenderer::IsSourceReserved (unsigned int source) const
{
	return std::find (RetiringSources.begin (), RetiringSources.end (), source) != RetiringSources.end ();
}

void OpenALSoundRenderer::AdvanceClocks ()
{
	unsigned int now = GetHostMilliseconds ();
	unsigned int elapsed = now - LastClockMilliseconds;
	LastClockMilliseconds = now;
	if (elapsed == 0 || OutputRate <= 0)
	{
		return;
	}
	if (SfxPaused == 0 && InactiveState != INACTIVE_Complete && !SyncPaused)
	{
		unsigned long long frames = (unsigned long long)elapsed * OutputRate + PausableFrameRemainder;
		PausableFrameRemainder = (unsigned int)(frames % 1000);
		frames /= 1000;
		PausableOutputFrames = std::numeric_limits<unsigned long long>::max () - PausableOutputFrames < frames ?
			std::numeric_limits<unsigned long long>::max () : PausableOutputFrames + frames;
	}
	if (InactiveState != INACTIVE_Complete && !SyncPaused)
	{
		unsigned long long frames = (unsigned long long)elapsed * OutputRate + NonPausableFrameRemainder;
		NonPausableFrameRemainder = (unsigned int)(frames % 1000);
		frames /= 1000;
		NonPausableOutputFrames = std::numeric_limits<unsigned long long>::max () - NonPausableOutputFrames < frames ?
			std::numeric_limits<unsigned long long>::max () : NonPausableOutputFrames + frames;
	}
}

void OpenALSoundRenderer::InitializePauseState (OpenALChannel *channel)
{
	channel->PauseReasons = (!channel->NoPause && SfxPaused != 0 ? OALPAUSE_Gameplay : 0) |
		(InactiveState == INACTIVE_Complete ? OALPAUSE_Inactive : 0) | (SyncPaused ? OALPAUSE_Sync : 0);
}

void OpenALSoundRenderer::ApplyChannelPauseState (OpenALChannel *channel)
{
	ALint state = AL_STOPPED;
	if (channel == NULL || channel->FinalizeState != OALFINAL_Active)
	{
		return;
	}
	if (channel->PauseReasons != 0)
	{
		alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
		if (state == AL_PLAYING)
		{
			channel->WasPlayingBeforePause = true;
			alSourcePause (channel->Source);
		}
	}
	else if (channel->WasPlayingBeforePause)
	{
		channel->WasPlayingBeforePause = false;
		alSourcePlay (channel->Source);
	}
}

unsigned long long OpenALSoundRenderer::GetChannelClock (bool noPause) const
{
	return noPause ? NonPausableOutputFrames : PausableOutputFrames;
}

bool OpenALSoundRenderer::PrepareRestart (OpenALSound *sound, float pitch, bool looping, bool noPause, FISoundChannel *reuseChan, int flags, RestartState *restart) const
{
	unsigned long long selectedClock = GetChannelClock (noPause);
	unsigned long long elapsedOutputFrames;
	unsigned long long savedPosition;
	const LogicalPosition *logicalPosition;
	unsigned int loopStart = sound->HasLoop ? sound->LoopStart : 0;
	unsigned int loopEnd = sound->HasLoop ? sound->LoopEnd : sound->Frames;
	restart->Position = 0;
	restart->ClockFrame = selectedClock;
	if (reuseChan == NULL)
	{
		return true;
	}
	if (OutputRate <= 0 || sound->SampleRate == 0 || pitch <= 0.f)
	{
		return false;
	}
	if (flags & SNDF_ABSTIME)
	{
		savedPosition = reuseChan->StartTime.AsOne;
		if (savedPosition >= sound->Frames)
		{
			return false;
		}
		restart->Position = (unsigned int)savedPosition;
		return true;
	}
	logicalPosition = FindLogicalPosition (reuseChan);
	if (logicalPosition != NULL && logicalPosition->OwnerToken != 0 && logicalPosition->Sound == sound &&
		reuseChan->StartTime.AsOne == logicalPosition->OwnerToken)
	{
		elapsedOutputFrames = selectedClock >= logicalPosition->StartClock ? selectedClock - logicalPosition->StartClock : 0;
		return CalculateRestartPosition (logicalPosition->StartPosition, elapsedOutputFrames, logicalPosition->SampleRate,
			logicalPosition->Pitch, OutputRate, logicalPosition->Looping, logicalPosition->Frames,
			logicalPosition->LoopStart, logicalPosition->LoopEnd, &restart->Position);
	}
	elapsedOutputFrames = selectedClock >= reuseChan->StartTime.AsOne ? selectedClock - reuseChan->StartTime.AsOne : 0;
	return CalculateRestartPosition (0, elapsedOutputFrames, sound->SampleRate, pitch, OutputRate, looping,
		sound->Frames, loopStart, loopEnd, &restart->Position);
}

float OpenALSoundRenderer::GetEffectivePitch (float basePitch, bool noPause) const
{
	return WaterPitchActive && !noPause ? basePitch * 0.7937005f : basePitch;
}

void OpenALSoundRenderer::RebaseLogicalPosition (LogicalPosition *logicalPosition, unsigned int position, float pitch)
{
	if (logicalPosition != NULL)
	{
		logicalPosition->StartPosition = position;
		logicalPosition->StartClock = GetChannelClock (logicalPosition->NoPause);
		logicalPosition->Pitch = pitch;
	}
}

OpenALChannel *OpenALSoundRenderer::CreateChannel (unsigned int source, OpenALSound *sound, float volume, float pitch, int priority, int flags)
{
	OpenALChannel *channel = new OpenALChannel;
	channel->Source = source;
	channel->Sound = sound;
	channel->Gain = volume;
	channel->Pitch = pitch;
	channel->Priority = priority;
	channel->Looping = (flags & SNDF_LOOP) != 0;
	channel->NoPause = (flags & SNDF_NOPAUSE) != 0;
	channel->NoReverb = (flags & SNDF_NOREVERB) != 0;
	channel->AllocationSerial = ++NextAllocationSerial;
	InitializePauseState (channel);
	return channel;
}

unsigned int OpenALSoundRenderer::AcquireSource (int priority, float effectiveGain)
{
	unsigned int source = FindFreeSource ();
	OpenALChannel *candidate;
	if (source == 0 && FinalizePendingStopForReuse ())
	{
		source = FindFreeSource ();
	}
	if (source != 0)
	{
		return source;
	}
	candidate = FindEvictionCandidate ();
	if (!IncomingWins (candidate, priority, effectiveGain))
	{
		return 0;
	}
	FinalizeChannel (candidate, OALEND_PoolEviction);
	return FindFreeSource ();
}

unsigned long long OpenALSoundRenderer::AllocateLogicalPositionToken ()
{
	if (NextLogicalPositionToken <= 0xffffffffull)
	{
		NextLogicalPositionToken = ~0ull;
	}
	return NextLogicalPositionToken--;
}

bool OpenALSoundRenderer::ApplyRestartPosition (OpenALChannel *channel, const RestartState &restart)
{
	alSourcei (channel->Source, AL_SAMPLE_OFFSET, (ALint)restart.Position);
	if (alGetError () != AL_NO_ERROR)
	{
		return false;
	}
	channel->CachedPosition = restart.Position;
	channel->LogicalStartFrame = restart.ClockFrame;
	return true;
}

const OpenALSoundRenderer::LogicalPosition *OpenALSoundRenderer::FindLogicalPosition (FISoundChannel *owner) const
{
	for (size_t index = 0; index < LogicalPositions.size (); ++index)
	{
		if (LogicalPositions[index].Owner == owner)
		{
			return &LogicalPositions[index];
		}
	}
	return NULL;
}

void OpenALSoundRenderer::RememberLogicalPosition (FISoundChannel *owner, OpenALChannel *channel, const RestartState &restart)
{
	LogicalPosition *logicalPosition = const_cast<LogicalPosition *> (FindLogicalPosition (owner));
	if (logicalPosition == NULL)
	{
		LogicalPositions.push_back (LogicalPosition ());
		logicalPosition = &LogicalPositions.back ();
	}
	logicalPosition->Owner = owner;
	logicalPosition->Sound = channel->Sound;
	logicalPosition->StartClock = restart.ClockFrame;
	logicalPosition->StartPosition = restart.Position;
	logicalPosition->OwnerToken = 0;
	logicalPosition->SampleRate = channel->Sound->SampleRate;
	logicalPosition->Frames = channel->Sound->Frames;
	logicalPosition->LoopStart = channel->Sound->HasLoop ? channel->Sound->LoopStart : 0;
	logicalPosition->LoopEnd = channel->Sound->HasLoop ? channel->Sound->LoopEnd : channel->Sound->Frames;
	logicalPosition->Pitch = channel->Pitch;
	logicalPosition->Looping = channel->Looping;
	logicalPosition->NoPause = channel->NoPause;
}

void OpenALSoundRenderer::ForgetLogicalPosition (FISoundChannel *owner)
{
	for (size_t index = 0; index < LogicalPositions.size (); ++index)
	{
		if (LogicalPositions[index].Owner == owner)
		{
			LogicalPositions.erase (LogicalPositions.begin () + index);
			return;
		}
	}
}

bool OpenALSoundRenderer::GetLogicalPosition (FISoundChannel *owner, unsigned int *position) const
{
	const LogicalPosition *logicalPosition = FindLogicalPosition (owner);
	unsigned long long clock;
	if (logicalPosition == NULL || logicalPosition->OwnerToken == 0 || owner == NULL ||
		owner->StartTime.AsOne != logicalPosition->OwnerToken)
	{
		return false;
	}
	clock = GetChannelClock (logicalPosition->NoPause);
	return CalculateRestartPosition (logicalPosition->StartPosition,
		clock >= logicalPosition->StartClock ? clock - logicalPosition->StartClock : 0,
		logicalPosition->SampleRate, logicalPosition->Pitch, OutputRate, logicalPosition->Looping,
		logicalPosition->Frames, logicalPosition->LoopStart, logicalPosition->LoopEnd, position);
}

FISoundChannel *OpenALSoundRenderer::PublishChannel (OpenALChannel *channel, FISoundChannel *reuseChan, const RestartState &restart)
{
	FISoundChannel *owner;
	ALenum error;
	bool startFailed = false;
	if (!ApplyRestartPosition (channel, restart))
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	alSourcePlay (channel->Source);
#ifdef OAL_LIFECYCLE_TEST
	if (FailNextStart)
	{
		FailNextStart = false;
		startFailed = true;
	}
#endif
	error = alGetError ();
	if (startFailed || error != AL_NO_ERROR)
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	owner = reuseChan != NULL ? reuseChan : S_GetChannel (channel);
	if (owner == NULL)
	{
		alSourceStop (channel->Source);
		alSourcei (channel->Source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	channel->Owner = owner;
	owner->SysChannel = channel;
	owner->StartTime.AsOne = restart.ClockFrame;
	RememberLogicalPosition (owner, channel, restart);
	PendingStartNoPause = false;
	++channel->Sound->References;
	ActiveChannels.push_back (channel);
	ApplyChannelPauseState (channel);
	return owner;
}

#ifdef OAL_LIFECYCLE_TEST
void OpenALSoundRenderer::InjectStartFailureForTest ()
{
	FailNextStart = true;
}

void OpenALSoundRenderer::InjectStartSetupFailureForTest ()
{
	FailNextStartSetup = true;
}

void OpenALSoundRenderer::InjectPositionQueryFailureForTest ()
{
	FailNextPositionQuery = true;
}

void OpenALSoundRenderer::InjectSpatialStateFailureForTest ()
{
	FailNextSpatialState = true;
}

void OpenALSoundRenderer::InjectSpatialRadiusFailureForTest (bool persistent)
{
	FailNextSpatialRadius = true;
	PersistentSpatialRadiusFailure = persistent;
	SpatialRadiusFailureCalls = 0;
	SpatialRadiusFailureCallLimitExceeded = false;
}

void OpenALSoundRenderer::ClearSpatialRadiusFailureForTest ()
{
	FailNextSpatialRadius = false;
	PersistentSpatialRadiusFailure = false;
}

bool OpenALSoundRenderer::InjectSpatialRadiusFailure ()
{
	if (!FailNextSpatialRadius)
	{
		return false;
	}
	++SpatialRadiusFailureCalls;
	if (PersistentSpatialRadiusFailure && SpatialRadiusFailureCalls > 32)
	{
		SpatialRadiusFailureCallLimitExceeded = true;
		ClearSpatialRadiusFailureForTest ();
		return false;
	}
	if (!PersistentSpatialRadiusFailure)
	{
		ClearSpatialRadiusFailureForTest ();
	}
	return true;
}

void OpenALSoundRenderer::InjectEFXSourceFailureForTest (OpenALEFXFailure failure, bool persistent, unsigned int source)
{
	FailNextEFXSourceAssign = failure;
	PersistentEFXSourceFailure = persistent;
	FailEFXSourceAssignSource = source;
	EFXSourceFailureCalls = 0;
	EFXSourceFailureCallLimitExceeded = false;
}

void OpenALSoundRenderer::ClearEFXSourceFailureForTest ()
{
	FailNextEFXSourceAssign = OALEFXFAIL_None;
	PersistentEFXSourceFailure = false;
	FailEFXSourceAssignSource = 0;
}

bool OpenALSoundRenderer::InjectEFXSourceFailure (unsigned int source, OpenALEFXFailure failure)
{
	if (FailNextEFXSourceAssign != failure ||
		(FailEFXSourceAssignSource != 0 && FailEFXSourceAssignSource != source))
	{
		return false;
	}
	++EFXSourceFailureCalls;
	if (PersistentEFXSourceFailure && EFXSourceFailureCalls > 32)
	{
		EFXSourceFailureCallLimitExceeded = true;
		ClearEFXSourceFailureForTest ();
		return false;
	}
	if (!PersistentEFXSourceFailure)
	{
		ClearEFXSourceFailureForTest ();
	}
	return true;
}
#endif

unsigned int OpenALSoundRenderer::FindFreeSource () const
{
	for (int index = 0; index < AllocatedSources; ++index)
	{
		bool inUse = IsSourceReserved (Sources[index]);
		for (size_t channel = 0; !inUse && channel < ActiveChannels.size (); ++channel)
		{
			inUse = ActiveChannels[channel]->Source == Sources[index];
		}
		if (!inUse)
		{
			return Sources[index];
		}
	}
	return 0;
}

OpenALChannel *OpenALSoundRenderer::FindEvictionCandidate () const
{
	OpenALChannel *candidate = NULL;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (channel->FinalizeState != OALFINAL_Active)
		{
			continue;
		}
		if (candidate == NULL || channel->Priority < candidate->Priority ||
			(channel->Priority == candidate->Priority && (channel->EffectiveGain < candidate->EffectiveGain ||
			(channel->EffectiveGain == candidate->EffectiveGain && channel->AllocationSerial < candidate->AllocationSerial))))
		{
			candidate = channel;
		}
	}
	return candidate;
}

bool OpenALSoundRenderer::FinalizePendingStopForReuse ()
{
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (channel->FinalizeState == OALFINAL_Pending)
		{
			FinalizeChannel (channel, channel->EndReason);
			return true;
		}
	}
	return false;
}

bool OpenALSoundRenderer::IncomingWins (const OpenALChannel *candidate, int priority, float effectiveGain) const
{
	return candidate != NULL && (priority > candidate->Priority ||
		(priority == candidate->Priority && effectiveGain > candidate->EffectiveGain));
}

void OpenALSoundRenderer::RemoveActiveChannel (OpenALChannel *channel)
{
	std::vector<OpenALChannel *>::iterator found = std::find (ActiveChannels.begin (), ActiveChannels.end (), channel);
	if (found != ActiveChannels.end ())
	{
		ActiveChannels.erase (found);
	}
}

bool OpenALSoundRenderer::ResolveChannelPosition (OpenALChannel *channel, unsigned int *position) const
{
	ALint offset = 0;
	ALint state = AL_INITIAL;
	if (channel == NULL || position == NULL || channel->Sound == NULL)
	{
		return false;
	}
#ifdef OAL_LIFECYCLE_TEST
	if (const_cast<OpenALSoundRenderer *> (this)->FailNextPositionQuery)
	{
		const_cast<OpenALSoundRenderer *> (this)->FailNextPositionQuery = false;
		return false;
	}
#endif
	alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
	if (alGetError () != AL_NO_ERROR)
	{
		return false;
	}
	if (state == AL_STOPPED)
	{
		if (!channel->Looping && channel->EndReason == OALEND_None)
		{
			*position = channel->Sound->Frames;
			return true;
		}
		return false;
	}
	if (state != AL_PLAYING && state != AL_PAUSED)
	{
		return false;
	}
	alGetSourcei (channel->Source, AL_SAMPLE_OFFSET, &offset);
	if (alGetError () != AL_NO_ERROR)
	{
		return false;
	}
	alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
	if (alGetError () != AL_NO_ERROR || (state != AL_PLAYING && state != AL_PAUSED))
	{
		return false;
	}
	if (offset < 0 || (unsigned int)offset >= channel->Sound->Frames)
	{
		return false;
	}
	*position = (unsigned int)offset;
	return true;
}

unsigned int OpenALSoundRenderer::CachePosition (OpenALChannel *channel)
{
	unsigned int position = 0;
	if (ResolveChannelPosition (channel, &position))
	{
		channel->CachedPosition = position;
	}
	return channel->CachedPosition;
}

void OpenALSoundRenderer::ApplyChannelGain (OpenALChannel *channel)
{
	channel->EffectiveGain = InactiveState == INACTIVE_Mute ? 0.f : channel->Gain * SfxVolume * channel->RolloffGain;
	alSourcef (channel->Source, AL_GAIN, channel->EffectiveGain);
}

float OpenALSoundRenderer::CalculateRolloffGain (FRolloffInfo &rolloff, float distanceScale, SoundListener *listener, const FVector3 &position, float *distance) const
{
	if (listener == NULL || !listener->valid)
	{
		*distance = 0.f;
		return S_GetRolloff (&rolloff, 0.f, true);
	}
	*distance = sqrtf ((float)(listener->position - position).LengthSquared ());
	return S_GetRolloff (&rolloff, *distance * distanceScale, true);
}

bool OpenALSoundRenderer::ApplyChannelRadius (OpenALChannel *channel, bool headRelative, bool updateStatus)
{
	bool applied;
	int radius;
	if (!Capabilities.RadiusAdvertised)
	{
		if (updateStatus) Capabilities.RadiusApplied = false;
		return true;
	}
	radius = channel->IsArea && !headRelative ? 32 : 0;
	applied = ResetEFXSourceProperty (channel->Source, AL_SOURCE_RADIUS, radius, true, OALEFXFAIL_SourceRadius, updateStatus);
	if (updateStatus) Capabilities.RadiusApplied = applied;
	if (!applied && radius != 0)
	{
				return ResetEFXSourceProperty (channel->Source, AL_SOURCE_RADIUS, 0, true, OALEFXFAIL_SourceRadius, updateStatus);
	}
	return applied;
}

bool OpenALSoundRenderer::ApplySpatialState (OpenALChannel *channel, SoundListener *listener, const FVector3 &position, const FVector3 &velocity)
{
	float distance;
	bool headRelative;
	bool radiusSafe;
	bool gainApplied;
	bool spatialApplied;
	FVector3 convertedPosition;
	FVector3 convertedVelocity;

	channel->RolloffGain = CalculateRolloffGain (channel->Rolloff, channel->DistanceScale, listener, position, &distance);
	channel->Distance = distance;
	// Center nearby area sounds as a bounded Phase 1A panning approximation.
	headRelative = listener != NULL && listener->valid &&
		(distance == 0.f || (channel->IsArea && distance <= 32.f));
	channel->HeadRelative = headRelative;

	if (headRelative)
	{
		alSourcei (channel->Source, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f (channel->Source, AL_POSITION, 0.f, 0.f, 0.f);
		alSource3f (channel->Source, AL_VELOCITY, 0.f, 0.f, 0.f);
	}
	else
	{
		convertedPosition = ToOpenALCoordinates (position);
		convertedVelocity = ToOpenALCoordinates (velocity);
		alSourcei (channel->Source, AL_SOURCE_RELATIVE, AL_FALSE);
		alSource3f (channel->Source, AL_POSITION, convertedPosition.X, convertedPosition.Y, convertedPosition.Z);
		alSource3f (channel->Source, AL_VELOCITY, convertedVelocity.X, convertedVelocity.Y, convertedVelocity.Z);
	}
 #ifdef OAL_LIFECYCLE_TEST
	if (FailNextSpatialState)
	{
		FailNextSpatialState = false;
		alSource3f (0, AL_POSITION, 0.f, 0.f, 0.f);
	}
 #endif
	spatialApplied = alGetError () == AL_NO_ERROR;
	radiusSafe = ApplyChannelRadius (channel, headRelative, true);
	ApplyChannelGain (channel);
	gainApplied = alGetError () == AL_NO_ERROR;
	return spatialApplied && radiusSafe && gainApplied;
}

void OpenALSoundRenderer::FinalizeChannel (OpenALChannel *channel, OpenALEndReason reason)
{
	OpenALSound *sound;
	bool preserveTerminalPosition = false;
	if (channel == NULL || channel->FinalizeState == OALFINAL_Finalizing || channel->FinalizeState == OALFINAL_Finalized)
	{
		return;
	}
	if (reason == OALEND_Natural && channel->Sound != NULL)
	{
		channel->CachedPosition = channel->Sound->Frames;
	}
	else
	{
		CachePosition (channel);
		if (reason == OALEND_PoolEviction && !channel->Looping && channel->Sound != NULL &&
			channel->CachedPosition >= channel->Sound->Frames && channel->Sound->Frames > 0)
		{
			reason = OALEND_Natural;
			preserveTerminalPosition = true;
		}
	}
	channel->EndReason = reason;
	channel->FinalizeState = OALFINAL_Finalizing;
	RemoveActiveChannel (channel);
	RetiringSources.push_back (channel->Source);
	if (channel->Owner != NULL)
	{
		if (reason == OALEND_PoolEviction || preserveTerminalPosition)
		{
			LogicalPosition *logicalPosition = const_cast<LogicalPosition *> (FindLogicalPosition (channel->Owner));
			if (logicalPosition != NULL)
			{
				RebaseLogicalPosition (logicalPosition, channel->CachedPosition, channel->Pitch);
				logicalPosition->OwnerToken = AllocateLogicalPositionToken ();
				channel->Owner->StartTime.AsOne = logicalPosition->OwnerToken;
			}
		}
		else
		{
			ForgetLogicalPosition (channel->Owner);
		}
		S_ChannelEnded (channel->Owner);
	}
	alSourceStop (channel->Source);
	alSourcei (channel->Source, AL_BUFFER, 0);
	std::vector<unsigned int>::iterator retiring = std::find (RetiringSources.begin (), RetiringSources.end (), channel->Source);
	if (retiring != RetiringSources.end ())
	{
		RetiringSources.erase (retiring);
	}
	sound = channel->Sound;
	channel->Sound = NULL;
	channel->Owner = NULL;
	channel->FinalizeState = OALFINAL_Finalized;
	if (sound != NULL && --sound->References == 0 && sound->DeferredDelete)
	{
		DestroySound (sound);
	}
	delete channel;
}

FISoundChannel *OpenALSoundRenderer::Start2D (SoundHandle sfx, float volume, int pitch, int flags, int priority, FISoundChannel *reuseChan)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	OpenALChannel *channel;
	RestartState restart;
	float pitchRatio;
	unsigned int source;
	AdvanceClocks ();
	PendingStartNoPause = (flags & SNDF_NOPAUSE) != 0;
	if (!InitSuccess || sound == NULL || sound->DeferredDelete || reuseChan != NULL && reuseChan->SysChannel != NULL)
	{
		return NULL;
	}
	pitchRatio = GetEffectivePitch (snd_pitched ? pitch / 128.f : 1.f, (flags & SNDF_NOPAUSE) != 0);
	if (!PrepareRestart (sound, pitchRatio, (flags & SNDF_LOOP) != 0, (flags & SNDF_NOPAUSE) != 0, reuseChan, flags, &restart))
	{
		return NULL;
	}
	source = AcquireSource (priority, volume * SfxVolume);
	if (source == 0)
	{
		return NULL;
	}
	channel = CreateChannel (source, sound, volume, pitchRatio, priority, flags);

	alGetError ();
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)sound->Buffer2D);
	alSourcei (source, AL_SOURCE_RELATIVE, AL_TRUE);
	alSource3f (source, AL_POSITION, 0.f, 0.f, 0.f);
	alSource3f (source, AL_VELOCITY, 0.f, 0.f, 0.f);
	alSourcei (source, AL_LOOPING, channel->Looping ? AL_TRUE : AL_FALSE);
	alSourcef (source, AL_PITCH, channel->Pitch);
	ApplyChannelGain (channel);
#ifdef OAL_LIFECYCLE_TEST
	if (FailNextStartSetup)
	{
		FailNextStartSetup = false;
		alSourcei (0, AL_BUFFER, 0);
	}
#endif
	if (alGetError () != AL_NO_ERROR)
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	if (!ApplyChannelEFX (channel))
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	return PublishChannel (channel, reuseChan, restart);
}

FISoundChannel *OpenALSoundRenderer::StartSound (SoundHandle sfx, float volume, int pitch, int priority, int flags, FISoundChannel *reuseChan)
{
	return Start2D (sfx, volume, pitch, flags, priority, reuseChan);
}

FISoundChannel *OpenALSoundRenderer::StartSound3D (SoundHandle sfx, SoundListener *listener, float volume, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int, int flags, FISoundChannel *reuseChan)
{
	OpenALSound *sound = (OpenALSound *)sfx.data;
	OpenALChannel *channel;
	RestartState restart;
	float distance;
	float rolloffGain;
	float effectiveGain;
	float pitchRatio;
	unsigned int source;
	AdvanceClocks ();
	PendingStartNoPause = (flags & SNDF_NOPAUSE) != 0;
	if (!InitSuccess || sound == NULL || sound->DeferredDelete || rolloff == NULL || reuseChan != NULL && reuseChan->SysChannel != NULL)
	{
		return NULL;
	}
	rolloffGain = CalculateRolloffGain (*rolloff, distscale, listener, pos, &distance);
	effectiveGain = volume * SfxVolume * rolloffGain;
	pitchRatio = GetEffectivePitch (snd_pitched ? pitch / 128.f : 1.f, (flags & SNDF_NOPAUSE) != 0);
	if (!PrepareRestart (sound, pitchRatio, (flags & SNDF_LOOP) != 0, (flags & SNDF_NOPAUSE) != 0, reuseChan, flags, &restart))
	{
		return NULL;
	}
	source = AcquireSource (priority, effectiveGain);
	if (source == 0)
	{
		return NULL;
	}
	channel = CreateChannel (source, sound, volume, pitchRatio, priority, flags);
	channel->RolloffGain = rolloffGain;
	channel->EffectiveGain = effectiveGain;
	channel->Distance = distance;
	channel->DistanceScale = distscale;
	channel->Is3D = true;
	channel->IsArea = (flags & SNDF_AREA) != 0;
	channel->Rolloff = *rolloff;

	alGetError ();
	alSourceStop (source);
	alSourcei (source, AL_BUFFER, (ALint)(sound->BufferMono != 0 ? sound->BufferMono : sound->Buffer2D));
	alSourcei (source, AL_LOOPING, channel->Looping ? AL_TRUE : AL_FALSE);
	alSourcef (source, AL_PITCH, channel->Pitch);
#ifdef OAL_LIFECYCLE_TEST
	if (FailNextStartSetup)
	{
		FailNextStartSetup = false;
		alSourcei (0, AL_BUFFER, 0);
	}
#endif
	if (alGetError () != AL_NO_ERROR)
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	if (!ApplyChannelEFX (channel))
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	if (!ApplySpatialState (channel, listener, pos, vel))
	{
		alSourceStop (source);
		alSourcei (source, AL_BUFFER, 0);
		delete channel;
		return NULL;
	}
	return PublishChannel (channel, reuseChan, restart);
}

void OpenALSoundRenderer::StopChannel (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active)
	{
		CachePosition (channel);
		alSourceStop (channel->Source);
		channel->EndReason = OALEND_ExplicitStop;
		channel->FinalizeState = OALFINAL_Pending;
	}
}

void OpenALSoundRenderer::ChannelVolume (FISoundChannel *owner, float volume)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active)
	{
		channel->Gain = volume;
		ApplyChannelGain (channel);
	}
}

void OpenALSoundRenderer::MarkStartTime (FISoundChannel *channel)
{
	AdvanceClocks ();
	if (channel != NULL)
	{
		ForgetLogicalPosition (channel);
		channel->StartTime.AsOne = GetChannelClock (PendingStartNoPause);
	}
	PendingStartNoPause = false;
}

void OpenALSoundRenderer::MarkVirtualStart (FISoundChannel *channel, SoundHandle handle, int pitch, int flags)
{
	OpenALSound *sound = (OpenALSound *)handle.data;
	LogicalPosition *logicalPosition;
	AdvanceClocks ();
	if (channel == NULL || sound == NULL || sound->DeferredDelete)
	{
		MarkStartTime (channel);
		return;
	}
	ForgetLogicalPosition (channel);
	LogicalPositions.push_back (LogicalPosition ());
	logicalPosition = &LogicalPositions.back ();
	logicalPosition->Owner = channel;
	logicalPosition->Sound = sound;
	logicalPosition->StartClock = GetChannelClock ((flags & SNDF_NOPAUSE) != 0);
	logicalPosition->StartPosition = 0;
	logicalPosition->OwnerToken = AllocateLogicalPositionToken ();
	logicalPosition->SampleRate = sound->SampleRate;
	logicalPosition->Frames = sound->Frames;
	logicalPosition->LoopStart = sound->HasLoop ? sound->LoopStart : 0;
	logicalPosition->LoopEnd = sound->HasLoop ? sound->LoopEnd : sound->Frames;
	logicalPosition->Pitch = GetEffectivePitch (snd_pitched ? pitch / 128.f : 1.f, (flags & SNDF_NOPAUSE) != 0);
	logicalPosition->Looping = (flags & SNDF_LOOP) != 0;
	logicalPosition->NoPause = (flags & SNDF_NOPAUSE) != 0;
	channel->StartTime.AsOne = logicalPosition->OwnerToken;
	PendingStartNoPause = false;
}

unsigned int OpenALSoundRenderer::GetPosition (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	unsigned int position = 0;
	AdvanceClocks ();
	if (channel != NULL)
	{
		return CachePosition (channel);
	}
	return ResolveEvictedPosition (owner, &position) ? position : 0;
}

bool OpenALSoundRenderer::ResolveEvictedPosition (FISoundChannel *owner, unsigned int *position)
{
	const LogicalPosition *logicalPosition;
	if (owner == NULL || position == NULL || owner->SysChannel != NULL)
	{
		return false;
	}
	AdvanceClocks ();
	logicalPosition = FindLogicalPosition (owner);
	if (logicalPosition == NULL || logicalPosition->OwnerToken == 0 ||
		owner->StartTime.AsOne != logicalPosition->OwnerToken)
	{
		return false;
	}
	if (GetLogicalPosition (owner, position))
	{
		return true;
	}
	if (!logicalPosition->Looping && OutputRate > 0 && logicalPosition->SampleRate > 0 && logicalPosition->Pitch > 0.f)
	{
		*position = logicalPosition->Frames;
		return true;
	}
	return false;
}

float OpenALSoundRenderer::GetAudibility (FISoundChannel *owner)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	return channel == NULL ? 0.f : channel->EffectiveGain;
}

void OpenALSoundRenderer::Sync (bool sync)
{
	AdvanceClocks ();
	if (SyncPaused == sync)
	{
		return;
	}
	if (sync)
	{
		for (size_t index = 0; index < ActiveChannels.size (); ++index)
		{
			CachePosition (ActiveChannels[index]);
		}
	}
	SyncPaused = sync;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (sync)
		{
			channel->PauseReasons |= OALPAUSE_Sync;
		}
		else
		{
			channel->PauseReasons &= ~OALPAUSE_Sync;
		}
		ApplyChannelPauseState (channel);
	}
}

void OpenALSoundRenderer::SetSfxPaused (bool paused, int slot)
{
	unsigned int bit;
	if (slot < 0 || slot >= 32)
	{
		return;
	}
	AdvanceClocks ();
	bit = 1u << slot;
	if (paused)
	{
		SfxPaused |= bit;
	}
	else
	{
		SfxPaused &= ~bit;
	}
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (!channel->NoPause)
		{
			if (SfxPaused != 0)
			{
				channel->PauseReasons |= OALPAUSE_Gameplay;
			}
			else
			{
				channel->PauseReasons &= ~OALPAUSE_Gameplay;
			}
			ApplyChannelPauseState (channel);
		}
	}
}

void OpenALSoundRenderer::SetInactive (EInactiveState inactive)
{
	AdvanceClocks ();
	InactiveState = inactive;
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (inactive == INACTIVE_Complete)
		{
			channel->PauseReasons |= OALPAUSE_Inactive;
		}
		else
		{
			channel->PauseReasons &= ~OALPAUSE_Inactive;
		}
		ApplyChannelGain (channel);
		ApplyChannelPauseState (channel);
	}
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->SetInactive (inactive == INACTIVE_Complete);
	}
}

void OpenALSoundRenderer::UpdateSoundParams3D (SoundListener *listener, FISoundChannel *owner, bool areasound, const FVector3 &pos, const FVector3 &vel)
{
	OpenALChannel *channel = owner == NULL ? NULL : (OpenALChannel *)owner->SysChannel;
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active && channel->Is3D)
	{
		channel->IsArea = areasound;
		if (!ApplySpatialState (channel, listener, pos, vel)) RetireEFXSource (channel);
	}
}

void OpenALSoundRenderer::ReleaseEFXResources ()
{
	::ReleaseEFXResources (&Capabilities, &EFXEffect, &EFXSlot, &EFXFilter, &EFXUsesEAX);
}

bool OpenALSoundRenderer::ApplyEFXEnvironment (const ReverbContainer *environment)
{
	if (!Capabilities.EFXUsable || environment == NULL)
	{
		return false;
	}
	OpenALReverbParameters parameters = OALBuildReverbParameters (environment->Properties);
	if (!InvalidReverbPanWarned && parameters.HasInvalidPan)
	{
		Printf ("Warning: OpenAL EFX non-finite reverb pan converted to zero.\n");
		InvalidReverbPanWarned = true;
	}
	OpenALEFXFunctions &efx = Capabilities.EFX;
	alGetError ();
	if (EFXUsesEAX)
	{
		efx.Effectf (EFXEffect, AL_EAXREVERB_GAIN, parameters.Gain);
		efx.Effectf (EFXEffect, AL_EAXREVERB_GAINHF, parameters.GainHF);
		efx.Effectf (EFXEffect, AL_EAXREVERB_GAINLF, parameters.GainLF);
		efx.Effectf (EFXEffect, AL_EAXREVERB_DECAY_TIME, parameters.DecayTime);
		efx.Effectf (EFXEffect, AL_EAXREVERB_DECAY_HFRATIO, parameters.DecayHFRatio);
		efx.Effectf (EFXEffect, AL_EAXREVERB_DECAY_LFRATIO, parameters.DecayLFRatio);
		efx.Effectf (EFXEffect, AL_EAXREVERB_REFLECTIONS_GAIN, parameters.ReflectionsGain);
		efx.Effectf (EFXEffect, AL_EAXREVERB_REFLECTIONS_DELAY, parameters.ReflectionsDelay);
		efx.Effectfv (EFXEffect, AL_EAXREVERB_REFLECTIONS_PAN, parameters.ReflectionsPan);
		efx.Effectf (EFXEffect, AL_EAXREVERB_LATE_REVERB_GAIN, parameters.LateReverbGain);
		efx.Effectf (EFXEffect, AL_EAXREVERB_LATE_REVERB_DELAY, parameters.LateReverbDelay);
		efx.Effectfv (EFXEffect, AL_EAXREVERB_LATE_REVERB_PAN, parameters.LateReverbPan);
		efx.Effectf (EFXEffect, AL_EAXREVERB_ECHO_TIME, parameters.EchoTime);
		efx.Effectf (EFXEffect, AL_EAXREVERB_ECHO_DEPTH, parameters.EchoDepth);
		efx.Effectf (EFXEffect, AL_EAXREVERB_MODULATION_TIME, parameters.ModulationTime);
		efx.Effectf (EFXEffect, AL_EAXREVERB_MODULATION_DEPTH, parameters.ModulationDepth);
		efx.Effectf (EFXEffect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, parameters.AirAbsorptionGainHF);
		efx.Effectf (EFXEffect, AL_EAXREVERB_HFREFERENCE, parameters.HFReference);
		efx.Effectf (EFXEffect, AL_EAXREVERB_LFREFERENCE, parameters.LFReference);
		efx.Effectf (EFXEffect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, parameters.RoomRolloffFactor);
		efx.Effectf (EFXEffect, AL_EAXREVERB_DENSITY, parameters.Density);
		efx.Effectf (EFXEffect, AL_EAXREVERB_DIFFUSION, parameters.Diffusion);
		efx.Effecti (EFXEffect, AL_EAXREVERB_DECAY_HFLIMIT, parameters.DecayHFLimit ? AL_TRUE : AL_FALSE);
	}
	else
	{
		efx.Effectf (EFXEffect, AL_REVERB_GAIN, parameters.Gain);
		efx.Effectf (EFXEffect, AL_REVERB_GAINHF, parameters.GainHF);
		efx.Effectf (EFXEffect, AL_REVERB_DECAY_TIME, parameters.DecayTime);
		efx.Effectf (EFXEffect, AL_REVERB_DECAY_HFRATIO, parameters.DecayHFRatio);
		efx.Effectf (EFXEffect, AL_REVERB_REFLECTIONS_GAIN, parameters.ReflectionsGain);
		efx.Effectf (EFXEffect, AL_REVERB_REFLECTIONS_DELAY, parameters.ReflectionsDelay);
		efx.Effectf (EFXEffect, AL_REVERB_LATE_REVERB_GAIN, parameters.LateReverbGain);
		efx.Effectf (EFXEffect, AL_REVERB_LATE_REVERB_DELAY, parameters.LateReverbDelay);
		efx.Effectf (EFXEffect, AL_REVERB_AIR_ABSORPTION_GAINHF, parameters.AirAbsorptionGainHF);
		efx.Effectf (EFXEffect, AL_REVERB_ROOM_ROLLOFF_FACTOR, parameters.RoomRolloffFactor);
		efx.Effectf (EFXEffect, AL_REVERB_DENSITY, parameters.Density);
		efx.Effectf (EFXEffect, AL_REVERB_DIFFUSION, parameters.Diffusion);
		efx.Effecti (EFXEffect, AL_REVERB_DECAY_HFLIMIT, parameters.DecayHFLimit ? AL_TRUE : AL_FALSE);
	}
	if (alGetError () != AL_NO_ERROR)
	{
		Capabilities.EFXApplied = false;
		RecordEFXFailure (OALEFXFAIL_Properties);
		return false;
	}
	efx.AuxiliaryEffectSloti (EFXSlot, AL_EFFECTSLOT_EFFECT, EFXEffect);
	Capabilities.EFXApplied = alGetError () == AL_NO_ERROR;
	if (!Capabilities.EFXApplied)
	{
		RecordEFXFailure (OALEFXFAIL_SlotAttach);
	}
	return Capabilities.EFXApplied;
}

bool OpenALSoundRenderer::SetEFXSourceSend (unsigned int source, int slot, int send, int filter)
{
	alGetError ();
	alSource3i (source, AL_AUXILIARY_SEND_FILTER, slot, send, filter);
	ALenum error = alGetError ();
#ifdef OAL_LIFECYCLE_TEST
	LastEFXSource = source;
	LastEFXSlot = slot;
	LastEFXSend = send;
	LastEFXFilter = filter;
	if (InjectEFXSourceFailure (source, slot != 0 ? OALEFXFAIL_WetSend : OALEFXFAIL_DrySend))
	{
		alSource3i (0, AL_AUXILIARY_SEND_FILTER, slot, send, filter);
		error = alGetError ();
	}
	LastEFXSourceError = error;
	OpenALEFXSourceAssignment assignment = { source, slot, send, filter, error };
	EFXSourceAssignments.push_back (assignment);
	if (error != AL_NO_ERROR)
	{
		LastEFXSourceFailureError = error;
	}
#endif
	if (error != AL_NO_ERROR)
	{
		RecordEFXFailure (slot != 0 ? OALEFXFAIL_WetSend : OALEFXFAIL_DrySend);
	}
	return error == AL_NO_ERROR;
}

void OpenALSoundRenderer::RecordEFXFailure (OpenALEFXFailure failure)
{
	if (failure != OALEFXFAIL_None)
	{
		EFXFailure = failure;
	}
}

bool OpenALSoundRenderer::EnsureEFXSourceDry (unsigned int source)
{
	return !Capabilities.EFXCallable || SetEFXSourceSend (source, 0, 0, 0);
}

bool OpenALSoundRenderer::ClearWaterFilter (OpenALChannel *channel)
{
	if (channel == NULL || !Capabilities.EFXFilterCallable)
	{
		return true;
	}
	alGetError ();
	alSourcei (channel->Source, AL_DIRECT_FILTER, 0);
#ifdef OAL_LIFECYCLE_TEST
	LastDirectFilterSource = channel->Source;
	LastDirectFilter = 0;
	LastDirectFilterError = alGetError ();
	if (LastDirectFilterError == AL_NO_ERROR)
#else
	if (alGetError () == AL_NO_ERROR)
#endif
	{
		return true;
	}
	RecordEFXFailure (OALEFXFAIL_DirectFilter);
	return false;
}

bool OpenALSoundRenderer::ResetEFXSourceProperty (unsigned int source, OALenum property, int value, bool floating, OpenALEFXFailure failure, bool spatialRadius)
{
	alGetError ();
	if (floating) alSourcef (source, property, (float)value);
	else alSourcei (source, property, value);
#ifdef OAL_LIFECYCLE_TEST
	if (InjectEFXSourceFailure (source, failure) || (spatialRadius && InjectSpatialRadiusFailure ()))
	{
		if (floating) alSourcef (0, property, (float)value);
		else alSourcei (0, property, value);
	}
#endif
	if (alGetError () == AL_NO_ERROR)
	{
		return true;
	}
	RecordEFXFailure (failure);
	return false;
}

bool OpenALSoundRenderer::ResetEFXSource (unsigned int source)
{
	bool reset = EnsureEFXSourceDry (source);
	if (Capabilities.EFXCallable)
	{
		reset = ResetEFXSourceProperty (source, AL_AIR_ABSORPTION_FACTOR, 0, true, OALEFXFAIL_AirAbsorption) && reset;
		reset = ResetEFXSourceProperty (source, AL_DIRECT_FILTER_GAINHF_AUTO, AL_FALSE, false, OALEFXFAIL_DirectFilterAuto) && reset;
		reset = ResetEFXSourceProperty (source, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE, false, OALEFXFAIL_SendGainAuto) && reset;
		reset = ResetEFXSourceProperty (source, AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO, AL_FALSE, false, OALEFXFAIL_SendGainHFAuto) && reset;
	}
	if (Capabilities.EFXFilterCallable)
	{
		reset = ResetEFXSourceProperty (source, AL_DIRECT_FILTER, 0, false, OALEFXFAIL_DirectFilter) && reset;
	}
	if (Capabilities.RadiusAdvertised)
	{
		reset = ResetEFXSourceProperty (source, AL_SOURCE_RADIUS, 0, true, OALEFXFAIL_SourceRadius) && reset;
	}
	return reset;
}

void OpenALSoundRenderer::RetireEFXSource (OpenALChannel *channel)
{
	if (channel != NULL && channel->FinalizeState == OALFINAL_Active)
	{
		CachePosition (channel);
		alSourceStop (channel->Source);
		channel->EndReason = OALEND_BackendError;
		channel->FinalizeState = OALFINAL_Pending;
	}
}

bool OpenALSoundRenderer::ApplyChannelEFX (OpenALChannel *channel)
{
	int filter = 0;
	bool waterFilterApplied;
	if (channel == NULL || channel->FinalizeState != OALFINAL_Active)
	{
		return true;
	}
	if (!ResetEFXSource (channel->Source))
	{
		RetireEFXSource (channel);
		FailEFXEnvironment ();
		return false;
	}
	waterFilterApplied = ApplyWaterFilter (channel);
	if (!waterFilterApplied)
	{
		if (!ClearWaterFilter (channel) || !EnsureEFXSourceDry (channel->Source))
		{
			RetireEFXSource (channel);
			return false;
		}
	}
	if (waterFilterApplied && WaterFilterActive && !channel->NoPause && Capabilities.EFXFilterUsable)
	{
		filter = (int)EFXFilter;
	}
	if (Capabilities.EFXApplied && LastAttemptedEnvironment != NULL &&
		LastAttemptedEnvironment != DefaultEnvironments[0] && !channel->NoReverb)
	{
		if (!SetEFXSourceSend (channel->Source, (ALint)EFXSlot, 0, filter))
		{
			FailEFXEnvironment ();
			if (channel->Owner == NULL && !EnsureEFXSourceDry (channel->Source)) RetireEFXSource (channel);
			return false;
		}
	}
	if (!ApplyChannelRadius (channel, channel->HeadRelative, false))
	{
		RetireEFXSource (channel);
		return false;
	}
	return true;
}

static bool WaterFilterGain (int outputRate, float requestedCutoff, float *gain)
{
	float sampleRate = (float)outputRate;
	float cutoff = std::min (requestedCutoff, .49f * sampleRate);
	float reference = std::min (5000.f, .49f * sampleRate);
	float ratio;
	float t;
	float rawGain;
	if (sampleRate <= 0.f || cutoff <= 0.f || reference <= 0.f)
	{
		return false;
	}
	ratio = tanf (3.14159265358979323846f * cutoff / sampleRate) / tanf (3.14159265358979323846f * reference / sampleRate);
	t = ratio * ratio * ratio * ratio;
	rawGain = sqrtf (2.f * t / (1.f + sqrtf (1.f + 8.f * t * t)));
	if (reference < 5000.f && rawGain < .001f)
	{
		return false;
	}
	if (gain != NULL)
	{
		*gain = std::max (.001f, std::min (1.f, rawGain));
	}
	return true;
}

bool OpenALSoundRenderer::ApplyWaterFilter (OpenALChannel *channel)
{
	OpenALEFXFunctions &efx = Capabilities.EFX;
	if (channel == NULL || !Capabilities.EFXFilterUsable || EFXFilter == 0)
	{
		return true;
	}
	alGetError ();
	if (!WaterFilterActive || channel->NoPause)
	{
		return ClearWaterFilter (channel);
	}
	alGetError ();
	efx.Filterf (EFXFilter, AL_LOWPASS_GAIN, 1.f);
	efx.Filterf (EFXFilter, AL_LOWPASS_GAINHF, WaterFilterGainHF);
	if (alGetError () != AL_NO_ERROR)
	{
		RecordEFXFailure (OALEFXFAIL_DirectFilter);
		return false;
	}
	alGetError ();
	alSourcei (channel->Source, AL_DIRECT_FILTER, (ALint)EFXFilter);
#ifdef OAL_LIFECYCLE_TEST
	LastDirectFilterSource = channel->Source;
	LastDirectFilter = (int)EFXFilter;
	LastDirectFilterError = alGetError ();
	if (LastDirectFilterError != AL_NO_ERROR)
#else
	if (alGetError () != AL_NO_ERROR)
#endif
	{
		RecordEFXFailure (OALEFXFAIL_DirectFilter);
		return false;
	}
	return true;
}

bool OpenALSoundRenderer::ApplyEFXEnvironmentToChannels ()
{
	bool applied = true;
	for (size_t index = 0; index < ActiveChannels.size (); )
	{
		OpenALChannel *channel = ActiveChannels[index];
		applied = ApplyChannelEFX (channel) && applied;
		if (index < ActiveChannels.size () && ActiveChannels[index] == channel)
		{
			++index;
		}
	}
	return applied;
}

void OpenALSoundRenderer::DrainEFXEnvironmentFailure ()
{
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (channel != NULL && channel->FinalizeState == OALFINAL_Active && !EnsureEFXSourceDry (channel->Source))
		{
			RetireEFXSource (channel);
		}
	}
}

void OpenALSoundRenderer::FailEFXEnvironment ()
{
	Capabilities.EFXApplied = false;
	LastAppliedEnvironment = NULL;
	if (!EFXFailureDraining)
	{
		EFXFailureDraining = true;
		DrainEFXEnvironmentFailure ();
		EFXFailureDraining = false;
	}
}

void OpenALSoundRenderer::UpdateEFXEnvironment (SoundListener *listener)
{
	ReverbContainer *environment;
	bool modified;
	if (listener == NULL || !listener->valid)
	{
		return;
	}
	environment = ForcedEnvironment != NULL ? ForcedEnvironment :
		(listener->Environment != NULL ? listener->Environment : DefaultEnvironments[0]);
	if (environment == NULL)
	{
		return;
	}
	modified = environment->Modified;
	environment->Modified = false;
	if (EFXEnvironmentInitialized && environment == LastAttemptedEnvironment && !modified)
	{
		return;
	}
	EFXEnvironmentInitialized = true;
	LastAttemptedEnvironment = environment;
	if (environment == DefaultEnvironments[0])
	{
		Capabilities.EFXApplied = false;
		EFXFailure = OALEFXFAIL_None;
		LastAppliedEnvironment = environment;
		if (!ApplyEFXEnvironmentToChannels ())
		{
			LastAppliedEnvironment = NULL;
		}
		return;
	}
	EFXFailure = OALEFXFAIL_None;
	if (ApplyEFXEnvironment (environment) && ApplyEFXEnvironmentToChannels ())
	{
		LastAppliedEnvironment = environment;
	}
	else
	{
		FailEFXEnvironment ();
	}
}

void OpenALSoundRenderer::UpdateActiveWaterChannels (bool pitchChanged, bool wasPitchActive)
{
	for (size_t index = 0; index < ActiveChannels.size (); ++index)
	{
		OpenALChannel *channel = ActiveChannels[index];
		if (pitchChanged)
		{
			float basePitch = channel->Pitch / (wasPitchActive && !channel->NoPause ? 0.7937005f : 1.f);
			float effectivePitch = GetEffectivePitch (basePitch, channel->NoPause);
			CachePosition (channel);
			RebaseLogicalPosition (const_cast<LogicalPosition *> (FindLogicalPosition (channel->Owner)), channel->CachedPosition, effectivePitch);
			channel->Pitch = effectivePitch;
			alSourcef (channel->Source, AL_PITCH, effectivePitch);
		}
		ApplyChannelEFX (channel);
	}
}

void OpenALSoundRenderer::UpdateVirtualWaterChannels (bool wasPitchActive)
{
	for (size_t index = 0; index < LogicalPositions.size (); ++index)
	{
		LogicalPosition *logicalPosition = &LogicalPositions[index];
		if (logicalPosition->Owner != NULL && logicalPosition->Owner->SysChannel == NULL)
		{
			unsigned int position = 0;
			float basePitch = logicalPosition->Pitch / (wasPitchActive && !logicalPosition->NoPause ? 0.7937005f : 1.f);
			if (ResolveEvictedPosition (logicalPosition->Owner, &position))
			{
				RebaseLogicalPosition (logicalPosition, position, GetEffectivePitch (basePitch, logicalPosition->NoPause));
			}
		}
	}
}

void OpenALSoundRenderer::UpdateWaterState (SoundListener *listener)
{
	const ReverbContainer *environment;
	bool pitchActive;
	bool filterActive;
	bool filterChanged;
	bool wasPitchActive;
	float filterGainHF = 0.f;
	if (listener == NULL || !listener->valid)
	{
		return;
	}
	environment = ForcedEnvironment != NULL ? ForcedEnvironment :
		(listener->Environment != NULL ? listener->Environment : DefaultEnvironments[0]);
	pitchActive = (listener->underwater && snd_waterlp != 0) || (environment != NULL && environment->SoftwareWater);
	filterActive = pitchActive && WaterFilterGain (OutputRate, snd_waterlp, &filterGainHF);
	filterChanged = filterActive != WaterFilterActive || (filterActive && filterGainHF != WaterFilterGainHF);
	if (pitchActive == WaterPitchActive && !filterChanged)
	{
		return;
	}
	AdvanceClocks ();
	wasPitchActive = WaterPitchActive;
	WaterPitchActive = pitchActive;
	WaterFilterActive = filterActive;
	WaterFilterGainHF = filterActive ? filterGainHF : 0.f;
	UpdateActiveWaterChannels (pitchActive != wasPitchActive, wasPitchActive);
	if (pitchActive != wasPitchActive) UpdateVirtualWaterChannels (wasPitchActive);
}

void OpenALSoundRenderer::UpdateListener (SoundListener *listener)
{
	ALfloat orientation[6];
	FVector3 position;
	if (listener == NULL || !listener->valid)
	{
		return;
	}
	orientation[0] = cosf (listener->angle);
	orientation[1] = 0.f;
	orientation[2] = -sinf (listener->angle);
	orientation[3] = 0.f;
	orientation[4] = 1.f;
	orientation[5] = 0.f;
	position = ToOpenALCoordinates (listener->position);
	alListener3f (AL_POSITION, position.X, position.Y, position.Z);
	alListener3f (AL_VELOCITY, 0.f, 0.f, 0.f);
	alListenerfv (AL_ORIENTATION, orientation);
	UpdateWaterState (listener);
	UpdateEFXEnvironment (listener);
}

void OpenALSoundRenderer::UpdateSounds ()
{
	AdvanceClocks ();
	for (size_t index = 0; index < ActiveChannels.size (); )
	{
		OpenALChannel *channel = ActiveChannels[index];
		ALint state = AL_STOPPED;
		if (channel->FinalizeState == OALFINAL_Pending)
		{
			FinalizeChannel (channel, channel->EndReason);
			continue;
		}
		alGetSourcei (channel->Source, AL_SOURCE_STATE, &state);
		if (alGetError () != AL_NO_ERROR)
		{
			FinalizeChannel (channel, OALEND_BackendError);
			continue;
		}
		if (state == AL_STOPPED)
		{
			FinalizeChannel (channel, channel->Looping ? OALEND_BackendError : OALEND_Natural);
			continue;
		}
		++index;
	}
	for (size_t index = 0; index < ActiveStreams.size (); ++index)
	{
		ActiveStreams[index]->Update ();
	}
}

bool OpenALSoundRenderer::IsValid ()
{
	return InitSuccess;
}

void OpenALSoundRenderer::PrintStatus ()
{
	OpenALEFXStatusSnapshot efxStatus = OALGetEFXStatusSnapshot (Capabilities, EFXUsesEAX);
	const char *efxFailureName = EFXFailureName (Capabilities, LastAppliedEnvironment, EFXFailure);
	if (!InitSuccess)
	{
		Printf (TEXTCOLOR_RED "OpenAL sound module is not active.\n");
		return;
	}
	Printf ("Sound backend: " TEXTCOLOR_GREEN "OpenAL Soft\n");
	Printf ("Device: " TEXTCOLOR_GREEN "%s\n", DeviceName.GetChars());
	Printf ("Vendor: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_VENDOR));
	Printf ("Renderer: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_RENDERER));
	Printf ("Version: " TEXTCOLOR_GREEN "%s\n", alGetString (AL_VERSION));
	if (OutputRate > 0)
	{
		Printf ("Output sample rate: " TEXTCOLOR_GREEN "%d\n", OutputRate);
	}
	Printf ("AL_SOFT_loop_points: " TEXTCOLOR_GREEN "available\n");
	Printf ("ALC_SOFT_HRTF: %s, attributes at init: %s, request at init: %s, current cvar: %s, active: %s, status: %s (%d), init result: %s%s%s\n",
		Capabilities.HRTFAdvertised ? "advertised" : "absent", HRTFAttributesApplied ? "applied" : "not applied",
		HRTFRequestedEnabled ? "on" : "off", snd_hrtf ? "on" : "off",
		HRTFActiveName (Capabilities), HRTFStatusName (Capabilities), Capabilities.HRTFStatus,
			HRTFContextFailureName (HRTFFailure),
		Capabilities.HRTFSpecifier.GetChars ()[0] != '\0' ? ", specifier: " : "",
		Capabilities.HRTFSpecifier.GetChars ()[0] != '\0' ? Capabilities.HRTFSpecifier.GetChars () : "");
	Printf ("ALC_EXT_EFX: %s, entrypoints: %s, reverb: %s, filter: %s, applied: %s, failure: %s\n",
		Capabilities.EFXAdvertised ? "advertised" : "absent", Capabilities.EFXCallable ? "callable" : "missing",
		efxStatus.ReverbMode == OALEFXREVERB_EAX ? "EAX" : (efxStatus.ReverbMode == OALEFXREVERB_Standard ? "standard" : "dry"),
		efxStatus.FilterState == OALEFXFILTER_Available ? "available" : (efxStatus.FilterState == OALEFXFILTER_Failed ? "failed" : "absent"),
		efxStatus.Applied ? "yes" : "no", efxFailureName);
	if (efxStatus.SendCount < 0)
	{
		Printf ("EFX sends: unknown\n");
	}
	else
	{
		Printf ("EFX sends: %d\n", efxStatus.SendCount);
	}
	Printf ("EFX room rolloff: reverb parameter only; source-distance attenuation is not applied.\n");
	PrintWaterStatus ();
	Printf ("AL_EXT_SOURCE_RADIUS: %s, applied: %s\n", Capabilities.RadiusAdvertised ? "advertised" : "absent",
		Capabilities.RadiusApplied ? "yes" : "no");
	Printf ("Doppler: applied: %s (factor remains 0)\n", Capabilities.DopplerApplied ? "yes" : "no");
	Printf ("SFX sources: " TEXTCOLOR_GREEN "%d allocated / %d requested / %d free / %d active\n", AllocatedSources, RequestedSources, AllocatedSources - (int)ActiveChannels.size (), (int)ActiveChannels.size ());
}

void OpenALSoundRenderer::PrintWaterStatus () const
{
	Printf ("Water pitch: %s, low-pass: %s, legacy water reverb: not implemented (snd_waterreverb %s).\n",
		WaterPitchActive ? "active" : "off", WaterFilterActive ? "active" : "off", snd_waterreverb ? "on" : "off");
}

void OpenALSoundRenderer::PrintDriversList ()
{
	const ALCchar *devices;
	ALCenum specifier;
	int index = 0;

	if (alcIsExtensionPresent (NULL, "ALC_ENUMERATE_ALL_EXT"))
	{
		specifier = ALC_ALL_DEVICES_SPECIFIER;
	}
	else if (alcIsExtensionPresent (NULL, "ALC_ENUMERATION_EXT"))
	{
		specifier = ALC_DEVICE_SPECIFIER;
	}
	else
	{
		Printf ("OpenAL device enumeration is not supported.\n");
		return;
	}

	devices = alcGetString (NULL, specifier);
	for (const ALCchar *device = devices; device != NULL && *device != '\0'; device += strlen (device) + 1)
	{
		Printf ("%d. %s\n", index++, device);
	}
}

FString OpenALSoundRenderer::GatherStats ()
{
	FString out;
	FString sends;
	OpenALEFXStatusSnapshot efxStatus = OALGetEFXStatusSnapshot (Capabilities, EFXUsesEAX);
	if (efxStatus.SendCount < 0)
	{
		sends = "unknown";
	}
	else
	{
		sends.Format ("%d", efxStatus.SendCount);
	}
	out.Format ("device %s, version %s, %d SFX sources, %d active, %d free, %d streams, HRTF %s (%d), EFX %s/%s reverb %s filter %s applied %s sends %s, radius %s",
		DeviceName.GetChars (), OpenALVersion.GetChars (),
		AllocatedSources, (int)ActiveChannels.size (), AllocatedSources - (int)ActiveChannels.size (), (int)ActiveStreams.size (),
		Capabilities.HRTFAdvertised ? HRTFStatusName (Capabilities) : "absent", Capabilities.HRTFStatus,
		Capabilities.EFXAdvertised ? "advertised" : "absent", Capabilities.EFXCallable ? "callable" : "missing",
		efxStatus.ReverbMode == OALEFXREVERB_EAX ? "EAX" : (efxStatus.ReverbMode == OALEFXREVERB_Standard ? "standard" : "dry"),
		efxStatus.FilterState == OALEFXFILTER_Available ? "available" : (efxStatus.FilterState == OALEFXFILTER_Failed ? "failed" : "absent"),
		efxStatus.Applied ? "yes" : "no", sends.GetChars (), Capabilities.RadiusAdvertised ? "advertised" : "absent");
	return out;
}

void OpenALSoundRenderer::DestroyStream (OpenALSoundStream *stream)
{
	std::vector<OpenALSoundStream *>::iterator found = std::find (ActiveStreams.begin (), ActiveStreams.end (), stream);
	if (found != ActiveStreams.end ())
	{
		ActiveStreams.erase (found);
	}
}

OpenALEFXFunctions::OpenALEFXFunctions ()
	: GenEffects (NULL), DeleteEffects (NULL), Effecti (NULL), Effectf (NULL), Effectfv (NULL),
	  GenAuxiliaryEffectSlots (NULL), DeleteAuxiliaryEffectSlots (NULL), AuxiliaryEffectSloti (NULL),
	  AuxiliaryEffectSlotf (NULL), GenFilters (NULL), DeleteFilters (NULL), Filteri (NULL), Filterf (NULL)
{
}

bool OpenALEFXFunctions::IsCallable () const
{
	return GenEffects != NULL && DeleteEffects != NULL && Effecti != NULL && Effectf != NULL && Effectfv != NULL &&
		GenAuxiliaryEffectSlots != NULL && DeleteAuxiliaryEffectSlots != NULL && AuxiliaryEffectSloti != NULL &&
		AuxiliaryEffectSlotf != NULL;
}

bool OpenALEFXFunctions::IsFilterCallable () const
{
	return GenFilters != NULL && DeleteFilters != NULL && Filteri != NULL && Filterf != NULL;
}

OpenALCapabilities::OpenALCapabilities ()
	: HRTFAdvertised (false), HRTFActiveKnown (false), HRTFActive (false), HRTFStatusKnown (false), HRTFStatus (0), HRTFSpecifier (), EFXAdvertised (false), EFX (),
	  EFXCallable (false), EFXFilterCallable (false), EFXFilterUsable (false), EFXUsable (false), EFXApplied (false), EFXSendCount (-1), RadiusAdvertised (false),
	  RadiusApplied (false), DopplerApplied (false)
{
}

OpenALEFXStatusSnapshot OALGetEFXStatusSnapshot (const OpenALCapabilities &capabilities, bool usesEAX)
{
	OpenALEFXStatusSnapshot result;
	result.SendCount = capabilities.EFXSendCount;
	result.ReverbMode = !capabilities.EFXUsable ? OALEFXREVERB_Dry : (usesEAX ? OALEFXREVERB_EAX : OALEFXREVERB_Standard);
	result.FilterState = capabilities.EFXFilterUsable ? OALEFXFILTER_Available :
		(capabilities.EFXFilterCallable ? OALEFXFILTER_Failed : OALEFXFILTER_Absent);
	result.Applied = capabilities.EFXApplied;
	return result;
}

OpenALCapabilities OALBuildCapabilities (bool hrtfAdvertised, bool hrtfActiveKnown, bool hrtfActive,
	bool hrtfStatusKnown, int hrtfStatus,
	bool efxAdvertised, const OpenALEFXFunctions &efx, bool radiusAdvertised)
{
	OpenALCapabilities capabilities;
	capabilities.HRTFAdvertised = hrtfAdvertised;
	capabilities.HRTFActiveKnown = hrtfActiveKnown;
	capabilities.HRTFActive = hrtfActive;
	capabilities.HRTFStatusKnown = hrtfStatusKnown;
	capabilities.HRTFStatus = hrtfStatus;
	capabilities.EFXAdvertised = efxAdvertised;
	capabilities.EFX = efx;
	capabilities.EFXCallable = efx.IsCallable ();
	capabilities.EFXFilterCallable = efx.IsFilterCallable ();
	capabilities.RadiusAdvertised = radiusAdvertised;
	return capabilities;
}

bool OALBuildHRTFContextAttributes (bool hrtfAdvertised, bool hrtfEnabled, int attributes[5])
{
	if (!hrtfAdvertised)
	{
		return false;
	}
	attributes[0] = ALC_HRTF_SOFT;
	attributes[1] = hrtfEnabled ? ALC_TRUE : ALC_FALSE;
	attributes[2] = ALC_MAX_AUXILIARY_SENDS;
	attributes[3] = 1;
	attributes[4] = 0;
	return true;
}