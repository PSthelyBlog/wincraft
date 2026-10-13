/*
 * wcctl - send a command to wcinit.
 *
 *   wcctl poweroff | reboot | halt
 *   wcctl priv <helper> [args...]
 *
 * SPDX-License-Identifier: MIT
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char buf[512];
    size_t len = 0;
    int fd, i;

    if (argc < 2) {
        fprintf(stderr, "usage: wcctl poweroff|reboot|halt|priv <helper> [args...]\n");
        return 2;
    }
    for (i = 1; i < argc; i++) {
        size_t n = strlen(argv[i]);

        if (len + n + 2 > sizeof(buf)) {
            fprintf(stderr, "wcctl: command too long\n");
            return 2;
        }
        if (i > 1)
            buf[len++] = ' ';
        memcpy(buf + len, argv[i], n);
        len += n;
    }
    buf[len++] = '\n';

    fd = open("/run/wcinit.ctl", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("wcctl: /run/wcinit.ctl");
        return 1;
    }
    if (write(fd, buf, len) != (ssize_t)len) {
        perror("wcctl: write");
        return 1;
    }
    return 0;
}
