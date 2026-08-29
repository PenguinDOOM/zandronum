#ifndef __OALSOUND_H__
#define __OALSOUND_H__

#include "i_sound.h"

class OpenALSoundRenderer : public SoundRenderer
{
public:
	OpenALSoundRenderer ();
	~OpenALSoundRenderer ();

	void SetSfxVolume (float volume);
	void SetMusicVolume (float volume);
	SoundHandle LoadSound (BYTE *sfxdata, int length);
	SoundHandle LoadSoundRaw (BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend = -1);
	void UnloadSound (SoundHandle sfx);
	unsigned int GetMSLength (SoundHandle sfx);
	unsigned int GetSampleLength (SoundHandle sfx);
	float GetOutputRate ();

	SoundStream *CreateStream (SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata);
	SoundStream *OpenStream (const char *filename, int flags, int offset, int length);

	FISoundChannel *StartSound (SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan);
	FISoundChannel *StartSound3D (SoundHandle sfx, SoundListener *listener, float vol, FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel, int channum, int chanflags, FISoundChannel *reuse_chan);
	void StopChannel (FISoundChannel *chan);
	void ChannelVolume (FISoundChannel *chan, float volume);
	void MarkStartTime (FISoundChannel *chan);
	unsigned int GetPosition (FISoundChannel *chan);
	float GetAudibility (FISoundChannel *chan);
	void Sync (bool sync);
	void SetSfxPaused (bool paused, int slot);
	void SetInactive (EInactiveState inactive);
	void UpdateSoundParams3D (SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel);
	void UpdateListener (SoundListener *listener);
	void UpdateSounds ();

	bool IsValid ();
	void PrintStatus ();
	void PrintDriversList ();
	FString GatherStats ();

private:
	bool Init ();
	void Shutdown ();
	const char *FindDeviceName (const char *name) const;

	void *Device;
	void *Context;
	unsigned int *Sources;
	int RequestedSources;
	int AllocatedSources;
	int OutputRate;
	bool InitSuccess;
	FString DeviceName;
};

#endif