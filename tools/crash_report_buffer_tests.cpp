#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "crash_report_buffer.h"

static int Failures;

static void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "%s\n", message);
		Failures++;
	}
}

static void CheckCanaries(const char *storage, int size, const char *message)
{
	Check(storage[0] == '\xA5' && storage[size - 1] == '\xA5', message);
}

static void CheckNormalReport()
{
	char storage[66];
	char *buffer = storage + 1;
	const char expected[] = "Zandronum version test\nCommand line: -file test.wad\n";
	memset(storage, 0xA5, sizeof(storage));
	CrashReportBuffer report(buffer, buffer + 64);
	Check(report.Append("Zandronum version test"), "normal version append failed");
	Check(report.Append("\nCommand line: -file test.wad"), "normal command append failed");
	int length = report.Finish();
	Check(length == (int)sizeof(expected), "normal report return length changed");
	Check(length == (int)sizeof(expected) && memcmp(buffer, expected, sizeof(expected)) == 0, "normal report wording changed");
	CheckCanaries(storage, sizeof(storage), "normal report wrote outside buffer");
}

static void CheckExactCapacity()
{
	char storage[7];
	char *buffer = storage + 1;
	memset(storage, 0xA5, sizeof(storage));
	CrashReportBuffer report(buffer, buffer + 5);
	Check(!report.Append("abc"), "exact-capacity append did not stop collection");
	Check(report.Finish() == 5, "exact-capacity return length wrong");
	Check(memcmp(buffer, "abc\n\0", 5) == 0, "exact-capacity output wrong");
	CheckCanaries(storage, sizeof(storage), "exact-capacity report wrote outside buffer");
}

static void CheckTruncationStopsCollection()
{
	char storage[18];
	char *buffer = storage + 1;
	memset(storage, 0xA5, sizeof(storage));
	CrashReportBuffer report(buffer, buffer + 16);
	Check(!report.Append("Command line: %s", "an-oversized-command-line"), "oversized command line did not stop collection");
	Check(!report.Append("\nWad 0: %s", "an-oversized-wad-name"), "oversized WAD append resumed collection");
	report.Finish();
	Check(buffer[1] == '\0', "truncated report was not NUL terminated");
	Check(strstr(buffer, "Wad 0") == NULL, "truncated report retained later WAD data");
	CheckCanaries(storage, sizeof(storage), "truncated report wrote outside buffer");
}

static void CheckFormattingError()
{
	char storage[18];
	char *buffer = storage + 1;
	wchar_t invalid[] = { 0xd800, 0 };
	memset(storage, 0xA5, sizeof(storage));
	CrashReportBuffer report(buffer, buffer + 16);
	Check(!report.Append("%ls", invalid), "formatting error did not stop collection");
	report.Finish();
	Check(buffer[1] == '\0', "formatting error did not preserve final terminator");
	CheckCanaries(storage, sizeof(storage), "formatting error wrote outside buffer");
}

int main()
{
	CheckNormalReport();
	CheckExactCapacity();
	CheckTruncationStopsCollection();
	CheckFormattingError();
	return Failures == 0 ? 0 : 1;
}