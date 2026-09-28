#ifndef __CRASH_REPORT_BUFFER_H__
#define __CRASH_REPORT_BUFFER_H__

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

class CrashReportBuffer
{
public:
	CrashReportBuffer(char *buffer, char *end)
		: Buffer(buffer), End(end), Position(0), Stopped(false)
	{
	}

	bool Append(const char *format, ...)
	{
		va_list args;
		va_start(args, format);
		bool result = AppendV(format, args);
		va_end(args);
		return result;
	}

	int Finish()
	{
		ptrdiff_t remaining = End - Buffer - Position;
		if (remaining >= 2)
		{
			Buffer[Position++] = '\n';
			Buffer[Position++] = '\0';
		}
		else if (remaining == 1)
		{
			Buffer[Position++] = '\0';
		}
		return Position;
	}

private:
	bool AppendV(const char *format, va_list args)
	{
		ptrdiff_t available = End - Buffer - Position - 2;
		if (Stopped || available < 0)
		{
			Stopped = true;
			return false;
		}

		int written = vsnprintf(Buffer + Position, static_cast<size_t>(available + 1), format, args);
		if (written < 0 || written > available)
		{
			Stopped = true;
			return false;
		}

		Position += written;
		Stopped = Position >= End - Buffer - 2;
		return !Stopped;
	}

	char *Buffer;
	char *End;
	int Position;
	bool Stopped;
};

#endif