#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "timidity_pipe.h"

static int SelectResult;
static bool SelectReady;
static ssize_t ReadResult;
static const char *ReadData;
static int SelectNfds;
static int ReadCalls;
static int Failures;

static int TestSelect(int nfds, fd_set *readfds, fd_set *, fd_set *, struct timeval *)
{
	SelectNfds = nfds;
	if (SelectResult > 0 && SelectReady)
	{
		return SelectResult;
	}
	FD_CLR(nfds - 1, readfds);
	return SelectResult;
}

static ssize_t TestRead(int, void *buffer, size_t length)
{
	ReadCalls++;
	if (ReadResult > 0 && ReadData != NULL)
	{
		memcpy(buffer, ReadData, ReadResult < (ssize_t)length ? ReadResult : (ssize_t)length);
	}
	return ReadResult;
}

static void Reset(int selectResult, bool selectReady, ssize_t readResult, const char *readData)
{
	SelectResult = selectResult;
	SelectReady = selectReady;
	ReadResult = readResult;
	ReadData = readData;
	SelectNfds = 0;
	ReadCalls = 0;
}

static void Check(bool condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "%s\n", message);
		Failures++;
	}
}

static void CheckFill(bool expected, int *fd, unsigned char *buffer)
{
	Check(TimidityPipeFillBuffer(fd, buffer, 8, TestSelect, TestRead) == expected, "unexpected fill result");
}

static void CheckPermanentReadError(unsigned char *buffer)
{
	int fd = 3;
	Reset(1, true, -1, NULL);
	errno = EIO;
	CheckFill(false, &fd, buffer);
	Check(fd == -1, "permanent read error did not latch terminal state");
	Reset(1, true, 3, "abc");
	CheckFill(false, &fd, buffer);
	Check(ReadCalls == 0, "latched terminal state retried read");
}

static void CheckEndOfFile(unsigned char *buffer)
{
	int fd = 3;
	Reset(1, true, 0, NULL);
	CheckFill(false, &fd, buffer);
	Check(fd == -1, "EOF did not latch terminal state");
	Reset(-1, false, -1, NULL);
	errno = EINTR;
	CheckFill(false, &fd, buffer);
	Check(ReadCalls == 0, "EOF terminal state revived after select interruption");
}

int main()
{
	unsigned char storage[10];
	unsigned char *buffer = storage + 1;
	int fd;

	memset(storage, 0xa5, sizeof(storage));
	CheckPermanentReadError(buffer);
	Check(storage[0] == 0xa5 && storage[9] == 0xa5, "fill wrote outside buffer");

	Reset(0, false, 0, NULL);
	memset(buffer, 0xa5, 8);
	fd = 3;
	CheckFill(true, &fd, buffer);
	Check(ReadCalls == 0, "timeout read unexpectedly");
	for (int i = 0; i < 8; ++i) Check(buffer[i] == 0, "timeout did not silence buffer");

	Reset(1, true, 3, "abc");
	fd = 3;
	CheckFill(true, &fd, buffer);
	Check(SelectNfds == 4 && ReadCalls == 1, "read readiness used wrong descriptor");
	Check(memcmp(buffer, "abc", 3) == 0, "read data missing");
	for (int i = 3; i < 8; ++i) Check(buffer[i] == 0, "short read did not silence tail");

	CheckEndOfFile(buffer);

	Reset(1, true, -1, NULL);
	errno = EAGAIN;
	fd = 3;
	CheckFill(true, &fd, buffer);
	errno = EINTR;
	CheckFill(true, &fd, buffer);

	Reset(-1, false, -1, NULL);
	errno = EBADF;
	fd = 3;
	CheckFill(false, &fd, buffer);
	Check(fd == -1, "select EBADF did not latch terminal state");

	Reset(1, true, 3, "abc");
	memset(buffer, 0xa5, 8);
	fd = -1;
	CheckFill(false, &fd, buffer);
	Check(ReadCalls == 0, "invalid descriptor read unexpectedly");
	for (int i = 0; i < 8; ++i) Check(buffer[i] == 0, "invalid descriptor did not silence buffer");
	fd = FD_SETSIZE;
	CheckFill(false, &fd, buffer);
	Check(ReadCalls == 0, "out-of-range descriptor read unexpectedly");
	return Failures == 0 ? 0 : 1;
}