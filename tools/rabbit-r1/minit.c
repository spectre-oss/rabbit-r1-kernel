// SPDX-License-Identifier: GPL-2.0-only
/* Reconstructed from the project's original embedded init. A pinned-toolchain
 * build reproduces SHA256 d4f101fa165966f533a823b7f70c4c26f0dff0ab1dd3c17b4e1be92bb07d24be.
 * Historical fallback only; external boot ramdisks provide the normal OS init.
 */
/* PID 1 for stock 4.19 (IKCONFIG): no DEVTMPFS, SELinux enforcing, no VT.
 * Do not use busybox init — it wants /dev/console and a VT. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>

static void mknod_ok(const char *p, mode_t mode, unsigned ma, unsigned mi)
{
	mknod(p, mode, makedev(ma, mi));
	chmod(p, mode & 0777);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	mkdir("/dev", 0755);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/tmp", 0755);

	mknod_ok("/dev/null", 0666 | S_IFCHR, 1, 3);
	mknod_ok("/dev/zero", 0666 | S_IFCHR, 1, 5);
	mknod_ok("/dev/full", 0666 | S_IFCHR, 1, 7);
	mknod_ok("/dev/random", 0666 | S_IFCHR, 1, 8);
	mknod_ok("/dev/urandom", 0666 | S_IFCHR, 1, 9);
	mknod_ok("/dev/console", 0600 | S_IFCHR, 5, 1);
	mknod_ok("/dev/tty", 0666 | S_IFCHR, 5, 0);
	mknod_ok("/dev/kmsg", 0622 | S_IFCHR, 1, 11);
	mknod_ok("/dev/fb0", 0666 | S_IFCHR, 29, 0);
	mknod_ok("/dev/watchdog", 0666 | S_IFCHR, 10, 130);
	mknod_ok("/dev/watchdog0", 0666 | S_IFCHR, 10, 130);
	mknod_ok("/dev/mem", 0600 | S_IFCHR, 1, 1);

	{
		int k = open("/dev/kmsg", O_WRONLY);
		if (k >= 0) {
			(void)write(k, "minit: pid1\n", 12);
			close(k);
		}
	}


	/* fbcon owns the panel. Do not let fbhello scribble over dmesg. */

	/* Do not open /dev/watchdog: open() starts the driver and
	 * re-arms TOPRGU. Kernel probe already disarmed it. */

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	mkdir("/sys/kernel/config", 0755);
	mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);

	if (fork() == 0) {
		execl("/etc/init.d/rcS", "rcS", (char *)NULL);
		_exit(1);
	}

	/* PTT blanks the panel in r1fb (KEY_POWER). Do not poweroff:
	 * USB-C then brings rabbitOS back. */

	if (fork() == 0) {
        for (;;) {
            pid_t pid = fork();
            if (pid == 0) {
                execl("/sbin/getty", "getty", "-L", "ttyS0", "921600", "vt100", (char *)NULL);
                execl("/bin/busybox", "busybox", "getty", "-L", "ttyS0", "921600", "vt100", (char *)NULL);
                _exit(1);
            }
            if (pid > 0) { int status; waitpid(pid, &status, 0); }
            sleep(1);
        }
    }

	if (fork() == 0) {
		execl("/usr/bin/r1cmd", "r1cmd", (char *)NULL);
		_exit(1);
	}

    if (fork() == 0) {
        for (int i=0; i<100 && access("/dev/ttyGS0", F_OK); i++) usleep(100000);
        for (;;) {
            pid_t pid = fork();
            if (pid == 0) {
                execl("/sbin/getty", "getty", "-L", "ttyGS0", "115200", "linux", (char *)NULL);
                execl("/bin/busybox", "busybox", "getty", "-L", "ttyGS0", "115200", "linux", (char *)NULL);
                _exit(1);
            }
            if (pid > 0) { int status; waitpid(pid, &status, 0); }
            sleep(1);
        }
    }

	for (;;)
		pause();
	return 0;
}
