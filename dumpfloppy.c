/*
   dumpfloppy: read a floppy disk using the PC controller

   Copyright (C) 2013 Adam Sampson

   This is based on the "How to identify an unknown disk" document from the
   fdutils project.
*/

#include <errno.h>
#include <fcntl.h>
#include <linux/fd.h>
#include <linux/fdreg.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int dev_fd;
static int drive;
static const char *image_filename;

static void die(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
#define die_errno(format, ...) \
    die(format ": %s", ##__VA_ARGS__, strerror(errno))

static int drive_selector(int head) {
    return (head << 2) | drive;
}

typedef struct {
    int rate; // 0 to 3
    bool is_fm;
} trackmode_t;

// Apply a mode specification to a floppy_raw_cmd -- which must contain only
// one command.
static void apply_mode(const trackmode_t *mode, struct floppy_raw_cmd *cmd) {
    cmd->rate = mode->rate;
    if (mode->is_fm) {
        cmd->cmd[0] &= ~0x40;
    }
}

// Seek the head back to track 0.
// Give up if it's stepped 80 tracks and not found track 0 (so you probably
// want to call this twice, in practice, in case someone's stepped to track
// 80+).
static void fd_recalibrate(struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    cmd->cmd[0] = FD_RECALIBRATE;
    cmd->cmd[1] = drive_selector(0);
    cmd->cmd_count = 2;
    cmd->flags = FD_RAW_INTR;

    if (ioctl(dev_fd, FDRAWCMD, cmd) < 0) {
        die_errno("FD_RECALIBRATE failed");
    }
}

// Read ID field of whatever sector reaches the head next.
// Give up after two index holes if nothing has been read.
//
// Return true if a sector ID was read. Upon return:
// cmd->reply[0]--[2] are ST0-ST2
// cmd->reply[3] is logical cyl
// cmd->reply[4] is logical head
// cmd->reply[5] is logical sector
// (128 << cmd->reply[6]) is sector size
static bool fd_readid(int cyl, int head, const trackmode_t *mode,
                      struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    cmd->cmd[0] = FD_READID;
    cmd->cmd[1] = drive_selector(head);
    cmd->cmd_count = 2;
    cmd->flags = FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd->track = cyl;
    apply_mode(mode, cmd);

    if (ioctl(dev_fd, FDRAWCMD, cmd) < 0) {
        die_errno("FD_READID failed");
    }
    if (cmd->reply_count < 7) {
        die("FD_READID returned short reply");
    }

    // If ST0 interrupt code is 00, success.
    return ((cmd->reply[0] >> 6) & 3) == 0;
}

static void probe_track_mode(int cyl, int head, trackmode_t *mode) {
    struct floppy_raw_cmd cmd;

    // Try all the possible modes until we can read a sector ID.
    for (int is_fm = 0; is_fm < 2; is_fm++) {
        mode->is_fm = is_fm;
        for (int rate = 0; rate < 4; rate++) {
            mode->rate = rate;
            printf("probe cyl %d head %d is_fm %d rate %d\n", cyl, head, is_fm, rate);

            if (fd_readid(cyl, head, mode, &cmd)) {
                printf("  success!\n");
                goto probe_secs;
            }
        }
    }

    // FIXME: fail (or retry)
    printf("failed to probe\n");
    return;

probe_secs:
    for (int i = 0; i < 30; i++) {
        fd_readid(cyl, head, mode, &cmd);
        printf("C %d H %d S %d size %d EOT %d\n", cmd.reply[3], cmd.reply[4], cmd.reply[5], cmd.reply[6], cmd.reply[1] & 0x80);
        // FIXME: if C == cyl, OK
        // if C == cyl / 2, ask for doublestepping
        // if C == cyl * 2, complain that this is the wrong kind of drive
    }
}

static void process_floppy(void) {
    char dev_filename[] = "/dev/fdX";
    dev_filename[7] = '0' + drive;

    dev_fd = open(dev_filename, O_ACCMODE | O_NONBLOCK);
    if (dev_fd == -1) {
        die_errno("cannot open %s", dev_filename);
    }

    // Reset the controller
    if (ioctl(dev_fd, FDRESET, (void *) FD_RESET_ALWAYS) < 0) {
        die_errno("cannot reset controller");
    }
    // FIXME: comment in fdrawcmd.1 says reset may block -- not O_NONBLOCK?

    // Return to track 0
    for (int i = 0; i < 2; i++) {
        struct floppy_raw_cmd cmd;
        fd_recalibrate(&cmd);
    }

    for (int cyl = 0; cyl < 80; cyl++) { // FIXME: num cylinders
        // FIXME: heads
        trackmode_t mode;
        probe_track_mode(cyl, 0, &mode);

        // ... read track
    }

    close(dev_fd);
}

static void usage(void) {
    fprintf(stderr, "usage: dumpfloppy [-d NUM] [IMAGE-FILE]\n");
    fprintf(stderr, "  -d NUM     drive number to read from (default 0)\n");
}

int main(int argc, char **argv) {
    dev_fd = -1;
    drive = 0;
    image_filename = NULL;

    while (true) {
        int opt = getopt(argc, argv, "d:");
        if (opt == -1) break;

        switch (opt) {
        case 'd':
            drive = atoi(optarg);
            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind == argc) {
        // No image file.
    } else if (optind + 1 == argc) {
        image_filename = argv[optind];
    } else {
        usage();
        return 0;
    }

    process_floppy();

    return 0;
}
