#ifndef __TIMIDITY_PIPE_H__
#define __TIMIDITY_PIPE_H__

#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

typedef int (*TimidityPipeSelectFunc)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
typedef ssize_t (*TimidityPipeReadFunc)(int fd, void *buffer, size_t count);

static bool TimidityPipeFillBuffer(int *fd, void *buffer, int length, TimidityPipeSelectFunc selectFunc, TimidityPipeReadFunc readFunc)
{
	fd_set readfds;
	struct timeval timeout;
	ssize_t readLength;

	if (buffer == NULL || length < 0)
	{
		return false;
	}
	memset(buffer, 0, length);
	if (length == 0)
	{
		return true;
	}
	if (fd == NULL || *fd < 0 || *fd >= FD_SETSIZE)
	{
		return false;
	}

	FD_ZERO(&readfds);
	FD_SET(*fd, &readfds);
	timeout.tv_sec = 0;
	timeout.tv_usec = 50;
	int selectResult = selectFunc(*fd + 1, &readfds, NULL, NULL, &timeout);
	if (selectResult < 0)
	{
		if (errno == EBADF)
		{
			close(*fd);
			*fd = -1;
			return false;
		}
		return true;
	}
	if (selectResult == 0 || !FD_ISSET(*fd, &readfds))
	{
		return true;
	}

	readLength = readFunc(*fd, buffer, length);
	if (readLength > 0)
	{
		return true;
	}
	if (readLength < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
	{
		return true;
	}
	close(*fd);
	*fd = -1;
	return false;
}

#endif