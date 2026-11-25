#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE *log_fp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void log_open(void) {
	log_fp = fopen(LOG_FILE, "a");
	setvbuf(stdout, NULL, _IOLBF, 0);
}

void logf_(const char *fmt, ...) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	pthread_mutex_lock(&log_lock);
	printf("[%5ld.%03ld] recovery: %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, line);
	if (log_fp) {
		fprintf(log_fp, "[%5ld.%03ld] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000, line);
		fflush(log_fp);
	}
	pthread_mutex_unlock(&log_lock);
}
