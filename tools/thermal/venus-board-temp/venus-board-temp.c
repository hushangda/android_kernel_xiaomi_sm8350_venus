// SPDX-License-Identifier: GPL-2.0
/* Venus board-temperature producer. All temperatures and offsets are in mC.
 * This does not control charging, cooling devices, frequencies, or brightness.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define MAX_SENSORS 16
#define THERMAL_ROOT "/sys/class/thermal"
#define OUTPUT THERMAL_ROOT "/thermal_message/board_sensor_temp"
#define COMPENSATION THERMAL_ROOT "/thermal_message/board_sensor_temp_comp"
#define CONFIG "/vendor/etc/venus-board-temp.conf"
struct sensor { char name[64], path[PATH_MAX]; int weight; };
struct model {
	struct sensor sensors[MAX_SENSORS];
	int count, divisor, offset, interval;
};
static volatile sig_atomic_t stopping;

static void stop_handler(int sig) { (void)sig; stopping = 1; }

static int read_text(const char *path, char *text, size_t capacity)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;
	if (fd < 0) return -errno;
	do { n = read(fd, text, capacity - 1); } while (n < 0 && errno == EINTR);
	int error = errno;
	close(fd);
	if (n < 0) return -error;
	if ((size_t)n == capacity - 1) return -EOVERFLOW;
	text[n] = 0;
	while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
	return 0;
}

static int parse_int(const char *text, int *value)
{
	char *end;
	errno = 0;
	long n = strtol(text, &end, 10);
	if (errno || end == text || n < INT_MIN || n > INT_MAX) return -ERANGE;
	while (isspace((unsigned char)*end)) end++;
	if (*end) return -EINVAL;
	*value = (int)n;
	return 0;
}

static int read_int(const char *path, int *value)
{
	char text[64];
	int rc = read_text(path, text, sizeof(text));
	return rc ? rc : parse_int(text, value);
}

static int read_model(const char *path, struct model *m)
{
	FILE *f = fopen(path, "re");
	char line[256], key[64], arg[64], extra[64], trailing[2];
	unsigned int seen = 0;
	int rc = -EINVAL, weight;
	if (!f) return -errno;
	memset(m, 0, sizeof(*m));
	while (fgets(line, sizeof(line), f)) {
		if (!strchr(line, '\n') && !feof(f)) goto out;
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (!*p || *p == '#') continue;
		int fields = sscanf(p, "%63s %63s %63s %1s", key, arg, extra, trailing);
		if (!strcmp(key, "sensor")) {
			if (fields != 3 || m->count == MAX_SENSORS ||
			    parse_int(extra, &weight) || !weight ||
			    weight < -10000 || weight > 10000) goto out;
			for (int i = 0; i < m->count; i++)
				if (!strcmp(arg, m->sensors[i].name)) goto out;
			/* A sensor name is a type, never an arbitrary path. */
			for (p = arg; *p; p++)
				if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') goto out;
			strcpy(m->sensors[m->count].name, arg);
			m->sensors[m->count++].weight = weight;
		} else {
			int *dest, bit;
			if (fields != 2) goto out;
			if (!strcmp(key, "divisor")) { dest = &m->divisor; bit = 1; }
			else if (!strcmp(key, "offset_mc")) { dest = &m->offset; bit = 2; }
			else if (!strcmp(key, "interval_ms")) { dest = &m->interval; bit = 4; }
			else goto out;
			if ((seen & bit) || parse_int(arg, dest)) goto out;
			seen |= bit;
		}
	}
	if (ferror(f)) { rc = -EIO; goto out; }
	if (seen == 7 && m->count && m->divisor > 0 &&
	    m->offset >= -40000 && m->offset <= 40000 &&
	    m->interval >= 1000 && m->interval <= 20000) rc = 0;
out:
	fclose(f);
	return rc;
}

static int discover(struct model *m, const char *root)
{
	DIR *dir = opendir(root);
	struct dirent *entry;
	int found = 0, rc = 0;
	char path[PATH_MAX], type[128];
	if (!dir) return -errno;
	for (int i = 0; i < m->count; i++) m->sensors[i].path[0] = 0;
	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "thermal_zone", 12)) continue;
		if (snprintf(path, sizeof(path), "%s/%s/type", root, entry->d_name) >= (int)sizeof(path)) {
			rc = -ENAMETOOLONG; break;
		}
		if (read_text(path, type, sizeof(type))) continue;
		for (int i = 0; i < m->count; i++) {
			if (strcmp(type, m->sensors[i].name)) continue;
			if (m->sensors[i].path[0]) { rc = -EEXIST; goto out; }
			if (snprintf(m->sensors[i].path, PATH_MAX, "%s/%s/temp", root, entry->d_name) >= PATH_MAX) {
				rc = -ENAMETOOLONG; goto out;
			}
			found++;
		}
	}
out:
	closedir(dir);
	return rc ? rc : (found == m->count ? 0 : -ENOENT);
}

static int sample(const struct model *m, const char *comp_path, int *result)
{
	int comp = 0;
	int rc = read_int(comp_path, &comp);
	/* Stock Venus defines a zero runtime offset when this optional node is absent. */
	if (rc && rc != -ENOENT) return rc;
	if (comp < -40000 || comp > 40000) return -ERANGE;
	int64_t sum = 0;
	for (int i = 0; i < m->count; i++) {
		int temp = 0;
		for (int attempt = 0; attempt < 2; attempt++) {
			rc = read_int(m->sensors[i].path, &temp);
			if (!rc && temp > -40000 && temp < 125000 && temp != 0) break;
			if (!rc) rc = -ENODATA;
		}
		if (rc) return rc;
		sum += (int64_t)temp * m->sensors[i].weight;
	}
	sum = sum / m->divisor + m->offset + comp;
	if (sum <= -40000 || sum >= 125000) return -ERANGE;
	*result = (int)sum;
	return 0;
}

static int publish(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return -errno;
	ssize_t n;
	do { n = write(fd, text, strlen(text)); } while (n < 0 && errno == EINTR);
	int rc = n < 0 ? -errno : ((size_t)n == strlen(text) ? 0 : -EIO);
	close(fd);
	return rc;
}

static void delay_ms(int ms)
{
	struct timespec delay = { ms / 1000, (ms % 1000) * 1000000L };
	while (!stopping && nanosleep(&delay, &delay) && errno == EINTR) { }
}

#ifndef VENUS_BOARD_TEMP_TEST
int main(int argc, char **argv)
{
	bool once = argc >= 2 && !strcmp(argv[1], "--sample");
	bool publish_once = argc >= 2 && !strcmp(argv[1], "--publish-once");
	bool daemon = argc >= 2 && !strcmp(argv[1], "--publish");
	const char *config = argc == 3 ? argv[2] : CONFIG;
	if (argc < 2 || argc > 3 || (!once && !publish_once && !daemon)) {
		fprintf(stderr, "usage: %s --sample|--publish-once|--publish [config]\n", argv[0]);
		return 2;
	}
#ifdef __ANDROID__
	char device[PROP_VALUE_MAX];
	__system_property_get("ro.product.vendor.device", device);
	if (strcmp(device, "venus")) { fprintf(stderr, "unsupported device: %s\n", device); return 1; }
#endif
	struct model m;
	int lock = -1;
	/* Every write mode shares the lock, including a one-shot diagnostic. */
	if (daemon || publish_once) {
		lock = open("/data/vendor/thermal/venus-board-temp.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { perror("publisher lock"); return 1; }
		if (daemon) {
			int clear_rc = publish(OUTPUT, "invalid\n");
			if (clear_rc) { fprintf(stderr, "invalidate: %s\n", strerror(-clear_rc)); close(lock); return 1; }
		}
	}
	int rc = read_model(config, &m);
	if (!rc) rc = discover(&m, THERMAL_ROOT);
	if (rc) { fprintf(stderr, "model/sensors: %s\n", strerror(-rc)); if (lock >= 0) close(lock); return 1; }
	int temp;
	if (once || publish_once) {
		rc = sample(&m, COMPENSATION, &temp);
		if (rc) { fprintf(stderr, "sample: %s\n", strerror(-rc)); if (lock >= 0) close(lock); return 1; }
		if (publish_once) {
			char text[32];
			snprintf(text, sizeof(text), "%d\n", temp);
			rc = publish(OUTPUT, text);
			if (rc) { fprintf(stderr, "publish: %s\n", strerror(-rc)); close(lock); return 1; }
		}
		printf("%d\n", temp);
		if (lock >= 0) close(lock);
		return 0;
	}
	struct sigaction sa = { .sa_handler = stop_handler };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	int previous = 0;
	bool have_previous = false, published = false;
	while (!stopping) {
		for (int retry = 0; ; retry++) {
			rc = sample(&m, COMPENSATION, &temp);
			if (rc || !have_previous || llabs((long long)temp - previous) <= 10000 || retry == 3) break;
			delay_ms(1000);
			if (stopping) break;
		}
		if (stopping) break;
		char text[32];
		if (!rc) snprintf(text, sizeof(text), "%d\n", temp);
		else {
			fprintf(stderr, "sample rejected: %s\n", strerror(-rc));
			/* Never retain a stale value or report a failed sensor as zero. */
			strcpy(text, "invalid\n");
		}
		int write_rc = publish(OUTPUT, text);
		if (write_rc) { fprintf(stderr, "publish: %s\n", strerror(-write_rc)); rc = write_rc; break; }
		published = true;
		if (!rc) { previous = temp; have_previous = true; }
		delay_ms(m.interval);
	}
	if (published) publish(OUTPUT, "invalid\n");
	close(lock);
	return rc ? 1 : 0;
}
#endif
