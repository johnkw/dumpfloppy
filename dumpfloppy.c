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
    const char *name;
    int rate; // 0 to 3
    bool is_fm;
} data_mode_t;

// Following what the .IMD spec says, the rates here are the data transfer rate
// to the drive -- FM-500k transfers half as much data as MFM-500k owing to the
// less efficient encoding.
const data_mode_t data_modes[] = {
    // 5.25" DD/QD and 3.5" DD drives
    { "MFM-250k", 2, false },
    { "FM-250k", 2, true },

    // DD media in 5.25" HD drives
    { "MFM-300k", 1, false },
    { "FM-300k", 1, true },

    // 3.5" HD, 5.25" HD and 8" drives
    { "MFM-500k", 0, false },
    { "FM-500k", 0, true },

    // 3.5" ED drives
    { "MFM-1000k", 3, false },
    // Rate 3 for FM isn't allowed.

    { NULL, 0, false }
};

// Apply a mode specification to a floppy_raw_cmd -- which must contain only
// one command.
static void apply_data_mode(const data_mode_t *mode,
                            struct floppy_raw_cmd *cmd) {
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
static bool fd_readid(int cyl, int head, const data_mode_t *mode,
                      struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    cmd->cmd[0] = FD_READID;
    cmd->cmd[1] = drive_selector(head);
    cmd->cmd_count = 2;
    cmd->flags = FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd->track = cyl;
    apply_data_mode(mode, cmd);

    if (ioctl(dev_fd, FDRAWCMD, cmd) < 0) {
        die_errno("FD_READID failed");
    }
    if (cmd->reply_count < 7) {
        die("FD_READID returned short reply");
    }

    // If ST0 interrupt code is 00, success.
    return ((cmd->reply[0] >> 6) & 3) == 0;
}

static void probe_track_mode(int cyl, int head) {
    const data_mode_t *mode = NULL;
    struct floppy_raw_cmd cmd;

    printf("%02d.%d: ", cyl, head);
    fflush(stdout);

    // Try all the possible modes until we can read a sector ID.
    for (int i = 0; ; i++) {
        if (data_modes[i].name == NULL) {
            printf("unknown data mode\n");
            // FIXME: fail or retry
            return;
        }

        if (fd_readid(cyl, head, &data_modes[i], &cmd)) {
            mode = &data_modes[i];
            printf("%s ", mode->name);
            fflush(stdout);
            break;
        }
    }

    // See what sectors are available.
    for (int i = 0; i < 30; i++) {
        fd_readid(cyl, head, mode, &cmd);
        // FIXME: if this fails, exit
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
        probe_track_mode(cyl, 0);

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
