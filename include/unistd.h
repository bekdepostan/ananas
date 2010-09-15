#include <machine/_types.h>
#include <sys/_types/uid.h>
#include <sys/_types/gid.h>
#include <sys/_types/pid.h>

#ifndef __UNISTD_H___
#define __UNISTD_H__

#define STDIN_FILENO	0
#define STDOUT_FILENO	1
#define STDERR_FILENO	2

void	_exit(int status);
ssize_t read(int fd, void* buf, size_t len);
ssize_t write(int fd, const void* buf, size_t len);
off_t	lseek(int fd, off_t offset, int whence);
pid_t	fork(void);
int	close(int filedes);
int	dup(int filedes);
int	dup2(int filedes, int filedes2);
int	access(const char* path, int amode);
uid_t	getuid(void);
pid_t	getpid(void);
pid_t	getppid(void);
gid_t	getgid(void);
uid_t	geteuid(void);
gid_t	getegid(void);
int	getgroups(int gidsetsize, gid_t grouplist[]);
int	fsync(int fildes);
int	link(const char* path1, const char* path2);
int	chdir(const char* path);
int	fchdir(int fildes);
int	fchown(int fildes, uid_t owner, gid_t group);
int	raise(int sig);
int	isatty(int fildes);
int	ftruncate(int fildes, off_t length);
int	rmdir(const char* path);
char*	getlogin(void);
unsigned alarm(unsigned seconds);
int	setpgid(pid_t pid, pid_t pgid);
char*	ttyname(int fildes);
int	setgid(gid_t gid);
int	setuid(uid_t uid);
char*	crypt(const char* key, const char* salt);

int	execvp(const char *path, char *const argv[]);
int	execv(const char *path, char *const argv[]);
int	execve(const char *path, char *const argv[], char *const envp[]);
int	execl(const char* path, const char* arg0, ...);
int	execlp(const char* path, const char* arg0, ...);

#define	R_OK 4
#define	W_OK 2
#define	X_OK 1
#define F_OK 8

extern int optind, opterr, optopt;
extern int optreset; /* XXX appears to be a bsd extension */
extern char* optarg;

extern char** environ;

#define _SC_PAGESIZE 1000
long	sysconf(int name);

/* legacy interfaces - should be nuked sometime */
int getdtablesize();

#endif /* __UNISTD_H__ */
