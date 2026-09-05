#include "audio_timetag.h"

#include <cstddef>
#include <limits>

namespace
{
	bool AppendDigit (unsigned long long *value, unsigned int digit)
	{
		if (*value > (std::numeric_limits<unsigned long long>::max () - digit) / 10)
		{
			return false;
		}
		*value = *value * 10 + digit;
		return true;
	}

	bool MultiplyAdd (unsigned long long *value, unsigned long long multiplier, unsigned long long addend)
	{
		if (*value > (std::numeric_limits<unsigned long long>::max () - addend) / multiplier)
		{
			return false;
		}
		*value = *value * multiplier + addend;
		return true;
	}

	bool SetTimeTagValue (bool hasColon, unsigned int part, unsigned long long milliseconds, unsigned int fractionalDigits, unsigned long long *parts, AudioTimeTag *value)
	{
		unsigned long long wholeSeconds = 0;
		if (!hasColon)
		{
			value->AsSamples = true;
			value->Value = parts[0];
			return true;
		}
		while (fractionalDigits < 3)
		{
			milliseconds *= 10;
			++fractionalDigits;
		}
		for (unsigned int index = 0; index <= part; ++index)
		{
			if (!MultiplyAdd (&wholeSeconds, 60, parts[index]))
			{
				return false;
			}
		}
		if (!MultiplyAdd (&wholeSeconds, 1000, milliseconds))
		{
			return false;
		}
		value->AsSamples = false;
		value->Value = wholeSeconds;
		return true;
	}
}

AudioTimeTag::AudioTimeTag ()
	: AsSamples (true), Value (0)
{
}

bool ParseAudioTimeTag (const char *tag, AudioTimeTag *value)
{
	unsigned long long parts[3] = { 0, 0, 0 };
	unsigned long long milliseconds = 0;
	unsigned int fractionalDigits = 0;
	unsigned int part = 0;
	bool hasColon = false;
	bool fractional = false;
	const char *character;
	if (tag == NULL || value == NULL)
	{
		return false;
	}
	for (character = tag; *character != '\0'; ++character)
	{
		if (*character >= '0' && *character <= '9')
		{
			if (fractional)
			{
				if (fractionalDigits < 3)
				{
					milliseconds = milliseconds * 10 + (unsigned int)(*character - '0');
					++fractionalDigits;
				}
			}
			else if (!AppendDigit (&parts[part], (unsigned int)(*character - '0')))
			{
				return false;
			}
		}
		else if (*character == ':')
		{
			if (fractional || part == 2)
			{
				return false;
			}
			hasColon = true;
			++part;
		}
		else if (*character == '.')
		{
			if (!hasColon || fractional)
			{
				return false;
			}
			fractional = true;
		}
		else
		{
			return false;
		}
	}
	return SetTimeTagValue (hasColon, part, milliseconds, fractionalDigits, parts, value);
}

bool ParseAudioTimeTagToUInt (const char *tag, bool *asSamples, unsigned int *value)
{
	AudioTimeTag parsed;
	if (asSamples == NULL || value == NULL || !ParseAudioTimeTag (tag, &parsed) || parsed.Value > std::numeric_limits<unsigned int>::max ())
	{
		return false;
	}
	*asSamples = parsed.AsSamples;
	*value = (unsigned int)parsed.Value;
	return true;
}