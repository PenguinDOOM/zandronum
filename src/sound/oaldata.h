#ifndef __OALDATA_H__
#define __OALDATA_H__

#include <cstddef>
#include <vector>

enum OALPCMError
{
	OALPCM_OK,
	OALPCM_INVALID_ARGUMENT,
	OALPCM_UNSUPPORTED_FORMAT,
	OALPCM_MALFORMED_DATA,
	OALPCM_OUT_OF_MEMORY
};

struct OALPCMData
{
	std::vector<short> Samples;
	unsigned int SampleRate;
	unsigned int Channels;
	unsigned int Frames;
	bool HasLoop;
	unsigned int LoopStart;
	unsigned int LoopEnd;

	OALPCMData ();
};

struct OALPCMResult
{
	OALPCMError Error;
	OALPCMData Data;

	explicit OALPCMResult (OALPCMError error = OALPCM_OK);
	bool IsValid () const;
};

OALPCMResult OALConvertRawPCM (const unsigned char *data, std::size_t length, unsigned int sampleRate, unsigned int channels, int bits, int loopStart, int loopEnd);
OALPCMResult OALParseWavePCM (const unsigned char *data, std::size_t length);
OALPCMResult OALDownmixToMono (const OALPCMData &source);

#endif