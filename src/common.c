// What the device and the simulator (sim_device.c) share.

#include "device.h"

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

int run(const char *fmt, ...) {
	char cmd[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	LOG("run: %s", cmd);
	int rc = system(cmd);
	if (rc == -1 || !WIFEXITED(rc)) {
		LOG("run: did not complete (%d)", rc);
		return -1;
	}
	if (WEXITSTATUS(rc) != 0) {
		LOG("run: exit status %d", WEXITSTATUS(rc));
	}
	return WEXITSTATUS(rc);
}

const char *boot_target_name(boot_target_t t) {
	switch (t) {
	case BOOT_NORMAL:
		return "system (ota:kernel)";
	case BOOT_RECOVERY:
		return "recovery (ota:kernel2)";
	default:
		return "unknown";
	}
}

const char *shq(const char *s, char *out, size_t size) {
	size_t o = 0;
	if (size < 3) {
		return "''";
	}
	out[o++] = '\'';
	for (; *s; s++) {
		if (*s == '\'') {
			if (o + 5 >= size) {
				return "''";
			}
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			if (o + 2 >= size) {
				return "''";
			}
			out[o++] = *s;
		}
	}
	out[o++] = '\'';
	out[o] = '\0';
	return out;
}
