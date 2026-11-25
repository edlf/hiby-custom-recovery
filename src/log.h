#ifndef LOG_H
#define LOG_H

// Everything goes to the console (UART2, 3 Mbaud) and to LOG_FILE, which an
// update copies next to the .upt on the card when it ends, so a failed update
// can be read without a serial cable.
#define LOG_FILE "/tmp/zees_recovery.log"

void log_open(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

#endif /* LOG_H */
