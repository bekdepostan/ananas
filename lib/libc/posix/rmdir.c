#include <sys/stat.h>
#include <fcntl.h>
#include <sys/handle.h>
#include <sys/stat.h>
#include <syscalls.h>

int rmdir(const char* path)
{
	void* handle = sys_open(path, 0 /* XXX */);
	if (handle == NULL)
		return -1;
	sys_remove(handle);
	return 0;
}
