// SPDX-License-Identifier: GPL-2.0-only
/* Only moves/kills this test's own children in a freshly created subtree.
 * Requires root. Bounded cases, not an exhaustive fork/migration stress test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t manager;
static char base[256], group_a[280], group_b[280], nested[300];
static bool created;

static void fail(const char *what, int line)
{
	fprintf(stderr, "FAIL line %d: %s errno=%d (%s)\n", line, what, errno, strerror(errno));
	if (getpid() == manager)
		exit(1);
	_exit(1);
}

#define CHECK(x) do { if (!(x)) fail(#x, __LINE__); } while (0)

static int put(const char *group, const char *file, const char *value)
{
	char path[512];
	int fd, saved;
	ssize_t ret;

	snprintf(path, sizeof(path), "%s/%s", group, file);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = write(fd, value, strlen(value));
	saved = errno;
	close(fd);
	errno = saved;
	return ret == (ssize_t)strlen(value) ? 0 : -1;
}

static bool event_is(const char *group, const char *event)
{
	char path[512], data[256] = {0};
	int fd;
	ssize_t count;

	snprintf(path, sizeof(path), "%s/cgroup.events", group);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd >= 0);
	count = read(fd, data, sizeof(data) - 1);
	CHECK(count >= 0);
	close(fd);
	return strstr(data, event) != NULL;
}

static void await_event(const char *group, const char *event)
{
	for (int i = 0; i < 200; i++) {
		if (event_is(group, event))
			return;
		usleep(5000);
	}
	fail(event, __LINE__);
}

static void cleanup(void)
{
	if (!created || getpid() != manager)
		return;
	put(base, "cgroup.kill", "1");
	for (int i = 0; i < 100; i++) {
		pid_t pid;

		do {
			pid = waitpid(-1, NULL, WNOHANG);
		} while (pid > 0);
		if (pid < 0 && errno == ECHILD)
			break;
		usleep(10000);
	}
	rmdir(nested);
	rmdir(group_b);
	rmdir(group_a);
	if (rmdir(base))
		fprintf(stderr, "CLEANUP: cannot remove %s: %s\n", base, strerror(errno));
}

static void join(const char *group)
{
	char pid[32];

	snprintf(pid, sizeof(pid), "%ld", syscall(SYS_gettid));
	CHECK(put(group, "cgroup.procs", pid) == 0);
}

static void send_byte(int fd, char byte)
{
	CHECK(write(fd, &byte, 1) == 1);
}

static void receive_byte(int fd, char want)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char got;

	CHECK(poll(&p, 1, 2000) == 1);
	CHECK(read(fd, &got, 1) == 1 && got == want);
}

static void wait_killed(pid_t pid)
{
	for (int i = 0; i < 200; i++) {
		int status;
		pid_t ret = waitpid(pid, &status, WNOHANG);

		CHECK(ret >= 0);
		if (ret == pid) {
			CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
			return;
		}
		usleep(5000);
	}
	fail("child was not killed", __LINE__);
}

static pid_t spawn(const char *group, bool forks)
{
	int ready[2];
	pid_t pid;

	CHECK(pipe2(ready, O_CLOEXEC) == 0);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		close(ready[0]);
		alarm(8);
		join(group);
		send_byte(ready[1], 'R');
		close(ready[1]);
		if (forks)
			for (int i = 0; i < 8; i++) {
				pid_t child = fork();

				CHECK(child >= 0);
				if (!child) {
					alarm(8);
					for (;;)
						pause();
				}
			}
		for (;;)
			pause();
	}
	close(ready[1]);
	receive_byte(ready[0], 'R');
	close(ready[0]);
	return pid;
}

static void recursive_case(bool frozen)
{
	pid_t first = spawn(group_a, false);
	pid_t second = spawn(nested, false);

	if (frozen) {
		CHECK(put(group_a, "cgroup.freeze", "1") == 0);
		await_event(group_a, "frozen 1\n");
	}
	CHECK(put(group_a, "cgroup.kill", "1") == 0);
	wait_killed(first);
	wait_killed(second);
	await_event(group_a, "populated 0\n");
	if (frozen)
		CHECK(put(group_a, "cgroup.freeze", "0") == 0);
	printf("PASS recursive kill, frozen=%d\n", frozen);
}

struct channel { int input, output; };

static void *migrating_thread(void *arg)
{
	struct channel *channel = arg;

	receive_byte(channel->input, 'M');
	join(group_b);
	send_byte(channel->output, 'M');
	receive_byte(channel->input, 'P');
	send_byte(channel->output, 'P');
	for (;;)
		pause();
	return NULL;
}

static void migrated_case(void)
{
	int to_child[2], from_child[2];
	pid_t pid;
	bool exited = false;

	CHECK(pipe2(to_child, O_CLOEXEC) == 0);
	CHECK(pipe2(from_child, O_CLOEXEC) == 0);
	pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		pthread_t worker;
		/* The leader exits, so the worker's argument must outlive its stack. */
		static struct channel channel;

		close(to_child[1]);
		close(from_child[0]);
		alarm(8);
		join(group_a);
		channel = (struct channel){ to_child[0], from_child[1] };
		CHECK(pthread_create(&worker, NULL, migrating_thread, &channel) == 0);
		pthread_exit(NULL);
	}
	close(to_child[0]);
	close(from_child[1]);
	for (int i = 0; i < 200; i++) {
		char path[64], data[1024] = {0}, *end;
		int fd;

		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		CHECK(fd >= 0);
		CHECK(read(fd, data, sizeof(data) - 1) > 0);
		close(fd);
		end = strrchr(data, ')');
		if (end && end[1] == ' ' && end[2] == 'Z') {
			exited = true;
			break;
		}
		usleep(5000);
	}
	CHECK(exited);
	send_byte(to_child[1], 'M');
	receive_byte(from_child[0], 'M');
	CHECK(put(group_a, "cgroup.kill", "1") == 0);
	send_byte(to_child[1], 'P');
	receive_byte(from_child[0], 'P');
	CHECK(put(group_b, "cgroup.kill", "1") == 0);
	wait_killed(pid);
	close(to_child[1]);
	close(from_child[0]);
	await_event(group_b, "populated 0\n");
	puts("PASS exited leader: migrated live thread survives old-group kill and is killed by destination-group kill");
}

int main(void)
{
	struct statfs fs;

	setbuf(stdout, NULL);
	manager = getpid();
	CHECK(geteuid() == 0);
	CHECK(statfs("/sys/fs/cgroup", &fs) == 0 && fs.f_type == 0x63677270);
	CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
	signal(SIGPIPE, SIG_IGN);
	alarm(25);
	snprintf(base, sizeof(base), "/sys/fs/cgroup/venus-a17-check-%d", manager);
	snprintf(group_a, sizeof(group_a), "%s/a", base);
	snprintf(group_b, sizeof(group_b), "%s/b", base);
	snprintf(nested, sizeof(nested), "%s/nested", group_a);
	CHECK(mkdir(base, 0700) == 0);
	created = true;
	CHECK(atexit(cleanup) == 0);
	CHECK(mkdir(group_a, 0700) == 0);
	CHECK(mkdir(group_b, 0700) == 0);
	CHECK(mkdir(nested, 0700) == 0);
	printf("temporary_subtree=%s manager_remains_outside=1\n", base);
	errno = 0;
	CHECK(put(group_a, "cgroup.kill", "0") == -1 && errno == ERANGE);
	recursive_case(false);
	recursive_case(true);
	migrated_case();
	for (int round = 0; round < 24; round++) {
		pid_t pid = spawn(group_a, true);

		usleep((round % 3) * 1000);
		CHECK(put(group_a, "cgroup.kill", "1") == 0);
		wait_killed(pid);
		await_event(group_a, "populated 0\n");
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	}
	puts("PASS 24 bounded fork/kill rounds (at most 8 forks per round)");
	return 0;
}
