#ifndef __OALSOUND_TEST_SUPPORT_H__
#define __OALSOUND_TEST_SUPPORT_H__

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;

union OALTestQwordUnion
{
	struct { unsigned int Lo, Hi; };
	unsigned long long AsOne;
};

class FString
{
public:
	FString ()
	{
		Text[0] = '\0';
	}

	FString &operator= (const char *text)
	{
		strncpy (Text, text == NULL ? "" : text, sizeof (Text) - 1);
		Text[sizeof (Text) - 1] = '\0';
		return *this;
	}

	const char *GetChars () const
	{
		return Text;
	}

	void Format (const char *format, ...)
	{
		va_list arguments;
		va_start (arguments, format);
		vsnprintf (Text, sizeof (Text), format, arguments);
		va_end (arguments);
	}

private:
	char Text[1024];
};

struct SoundHandle
{
	void *data;
};

struct FISoundChannel
{
	FISoundChannel ()
		: SysChannel (NULL), StartTime (), Priority (0)
	{
		StartTime.AsOne = 0;
	}

	void *SysChannel;
	OALTestQwordUnion StartTime;
	int Priority;
};

class SoundStream
{
};

typedef void (*SoundStreamCallback) ();

class FVector3
{
public:
	FVector3 () : X (0.f), Y (0.f), Z (0.f) {}
	FVector3 (float x, float y, float z) : X (x), Y (y), Z (z) {}

	FVector3 operator- (const FVector3 &other) const
	{
		return FVector3 (X - other.X, Y - other.Y, Z - other.Z);
	}

	float LengthSquared () const
	{
		return X * X + Y * Y + Z * Z;
	}

	float X;
	float Y;
	float Z;
};

class SoundListener
{
public:
	SoundListener () : position (), velocity (), angle (0.f), underwater (false), valid (false) {}

	FVector3 position;
	FVector3 velocity;
	float angle;
	bool underwater;
	bool valid;
};

struct FRolloffInfo
{
	int RolloffType;
	float MinDistance;
	union { float MaxDistance; float RolloffFactor; };
};

enum
{
	ROLLOFF_Doom,
	ROLLOFF_Linear,
	ROLLOFF_Log,
	ROLLOFF_Custom
};

enum EInactiveState
{
	INACTIVE_Active,
	INACTIVE_Complete,
	INACTIVE_Mute
};

class SoundRenderer
{
public:
	virtual ~SoundRenderer () {}
	virtual void SetSfxVolume (float) = 0;
	virtual void SetMusicVolume (float) = 0;
	virtual SoundHandle LoadSound (BYTE *, int) = 0;
	virtual SoundHandle LoadSoundRaw (BYTE *, int, int, int, int, int, int) = 0;
	virtual void UnloadSound (SoundHandle) = 0;
	virtual unsigned int GetMSLength (SoundHandle) = 0;
	virtual unsigned int GetSampleLength (SoundHandle) = 0;
	virtual float GetOutputRate () = 0;
	virtual SoundStream *CreateStream (SoundStreamCallback, int, int, int, void *) = 0;
	virtual SoundStream *OpenStream (const char *, int, int, int) = 0;
	virtual FISoundChannel *StartSound (SoundHandle, float, int, int, int, FISoundChannel *) = 0;
	virtual FISoundChannel *StartSound3D (SoundHandle, SoundListener *, float, FRolloffInfo *, float, int, int, const FVector3 &, const FVector3 &, int, int, FISoundChannel *) = 0;
	virtual void StopChannel (FISoundChannel *) = 0;
	virtual void ChannelVolume (FISoundChannel *, float) = 0;
	virtual void MarkStartTime (FISoundChannel *) = 0;
	virtual unsigned int GetPosition (FISoundChannel *) = 0;
	virtual bool ResolveEvictedPosition (FISoundChannel *, unsigned int *) { return false; }
	virtual float GetAudibility (FISoundChannel *) = 0;
	virtual void Sync (bool) = 0;
	virtual void SetSfxPaused (bool, int) = 0;
	virtual void SetInactive (EInactiveState) = 0;
	virtual void UpdateSoundParams3D (SoundListener *, FISoundChannel *, bool, const FVector3 &, const FVector3 &) = 0;
	virtual void UpdateListener (SoundListener *) = 0;
	virtual void UpdateSounds () = 0;
	virtual bool IsValid () = 0;
	virtual void PrintStatus () = 0;
	virtual void PrintDriversList () = 0;
	virtual FString GatherStats () = 0;
};

class OALTestIntCVar
{
public:
	explicit OALTestIntCVar (int value = 0) : Value (value) {}
	operator int () const { return Value; }
	int Value;
};

class OALTestFloatCVar
{
public:
	explicit OALTestFloatCVar (float value = 0.f) : Value (value) {}
	operator float () const { return Value; }
	float Value;
};

class OALTestBoolCVar
{
public:
	explicit OALTestBoolCVar (bool value = false) : Value (value) {}
	operator bool () const { return Value; }
	bool Value;
};

class OALTestStringCVar
{
public:
	explicit OALTestStringCVar (const char *value = "") : Value (value) {}
	const char *operator* () const { return Value; }
	const char *Value;
};

#define EXTERN_CVAR(type, name) extern OALTest ## type ## CVar name;
#define SNDF_LOOP 1
#define SNDF_NOPAUSE 2
#define SNDF_AREA 4
#define SNDF_ABSTIME 8
#define TEXTCOLOR_RED ""
#define TEXTCOLOR_GREEN ""

inline int stricmp (const char *left, const char *right)
{
	while (*left != '\0' && *right != '\0')
	{
		char leftCharacter = *left >= 'A' && *left <= 'Z' ? *left + ('a' - 'A') : *left;
		char rightCharacter = *right >= 'A' && *right <= 'Z' ? *right + ('a' - 'A') : *right;
		if (leftCharacter != rightCharacter)
		{
			return leftCharacter < rightCharacter ? -1 : 1;
		}
		++left;
		++right;
	}
	return *left == *right ? 0 : (*left == '\0' ? -1 : 1);
}

void Printf (const char *format, ...);
void DPrintf (const char *format, ...);
unsigned int OALTestMilliseconds ();
FISoundChannel *S_GetChannel (void *syschan);
void S_ChannelEnded (FISoundChannel *channel);
float S_GetRolloff (FRolloffInfo *rolloff, float distance, bool logarithmic);

#endif