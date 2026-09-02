#ifndef __AUDIO_TIMETAG_H__
#define __AUDIO_TIMETAG_H__

struct AudioTimeTag
{
	AudioTimeTag ();

	bool AsSamples;
	unsigned long long Value;
};

bool ParseAudioTimeTag (const char *tag, AudioTimeTag *value);
bool ParseAudioTimeTagToUInt (const char *tag, bool *asSamples, unsigned int *value);

#endif