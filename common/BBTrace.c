#include "BBTrace.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

void bb_trace(BBTraceProcess process, BBTraceEvent event)
{
    static const char *const names[] = {
        "loaded", "center-unavailable", "center-ready", "receiver-ready",
        "reply-submitting", "reply-submitted", "reply-failed",
        "enumeration-exception", "send-dispatched", "send-failed",
        "newchat-resolving", "newchat-resolved", "newchat-failed",
        "transport-listening", "transport-start-failed",
        "connection-accepted", "connection-refused", "connection-closed",
        "output-failed", "request-dispatched",
        "bridge-reply-received", "bridge-reply-rejected",
        "bridge-event-received", "request-expired", "session-opened",
        "session-closed"
    };
    if ((unsigned)event >= BBTraceEventCount) return;
    const char *path;
    if (process == BBTraceSpringBoard) path = "/tmp/bbdiag-sb.log";
    else if (process == BBTraceMobileSMS) path = "/tmp/bbdiag-sms.log";
    else return;

    /* Bounded metadata only. Never follow links or wait on another writer. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_nlink != 1 ||
        flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return;
    }
    if (fstat(fd, &info) == 0 && info.st_size < 65536 - 160) {
        struct timeval now;
        if (gettimeofday(&now, NULL) == 0) {
            char line[160];
            int length = snprintf(line, sizeof(line),
                "build=" BB_BUILD_VERSION " pid=%ld time=%ld.%06ld event=%s\n",
                (long)getpid(), (long)now.tv_sec, (long)now.tv_usec, names[event]);
            if (length > 0 && (size_t)length < sizeof(line)) {
                (void)write(fd, line, (size_t)length);
            }
        }
    }
    close(fd);
}
