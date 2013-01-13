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

static int sector_bytes(int size) {
    return 128 << size;
}

typedef struct {
    const char *name;
    int rate; // 0 to 3
    bool is_fm;
} data_mode_t;

// Following what the .IMD spec says, the rates here are the data transfer rate
// to the drive -- FM-500k transfers half as much data as MFM-500k owing to the
// less efficient encoding.
const data_mode_t DATA_MODES[] = {
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

typedef struct {
    bool probed;
    int phys_cyl;
    int phys_head;
    int log_cyl;
    int log_head;
    const data_mode_t *data_mode;
    int sector_size; // FDC code; decode with sector_bytes
    int first_sector;
    int num_sectors;
} track_t;

#define MAX_SECS 256
const track_t EMPTY_TRACK = {
    .probed = false,
    .phys_cyl = -1,
    .phys_head = -1,
    .log_cyl = -1,
    .log_head = -1,
    .data_mode = NULL,
    .sector_size = -1,
    .first_sector = -1,
    .num_sectors = -1,
};

static void init_track(int phys_cyl, int phys_head, track_t *track) {
    *track = EMPTY_TRACK;
    track->phys_cyl = phys_cyl;
    track->phys_head = phys_head;
}

#define MAX_CYLS 256
#define MAX_HEADS 2
typedef struct {
    int num_phys_cyls;
    int num_phys_heads;
    int first_log_cyl; // what physical cyl 0 corresponds to
    int cyl_step; // how many physical cyls to step for each logical one
    // FIXME: head numbering?
    track_t tracks[MAX_CYLS][MAX_HEADS];
} disk_t;

const disk_t EMPTY_DISK = {
    .num_phys_cyls = -1,
    .num_phys_heads = 2,
    .first_log_cyl = 0,
    .cyl_step = 1,
};

static void init_disk(disk_t *disk) {
    *disk = EMPTY_DISK;
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            init_track(cyl, head, &(disk->tracks[cyl][head]));
        }
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

static bool probe_track(track_t *track) {
    struct floppy_raw_cmd cmd;

    track->probed = false;

    printf("Probing %02d.%d:", track->phys_cyl, track->phys_head);
    fflush(stdout);

    // Identify the data mode (assuming there's only one; we don't handle disks
    // with multiple formats per track).
    // Try all the possible modes until we can read a sector ID.
    for (int i = 0; ; i++) {
        if (DATA_MODES[i].name == NULL) {
            printf(" unknown data mode\n");
            // FIXME: retry
            return false;
        }

        if (fd_readid(track->phys_cyl, track->phys_head, &DATA_MODES[i],
                      &cmd)) {
            track->log_cyl = cmd.reply[3];
            track->log_head = cmd.reply[4];
            track->sector_size = cmd.reply[6];
            track->data_mode = &DATA_MODES[i];
            printf(" %s", track->data_mode->name);
            fflush(stdout);
            break;
        }
    }

    // Identify the sector numbering scheme.
    // Keep track of which sectors we've seen.
    bool seen_sec[MAX_SECS];
    for (int i = 0; i < MAX_SECS; i++) {
        seen_sec[i] = false;
    }
    seen_sec[cmd.reply[5]] = true;

    // Read enough IDs for a few revolutions of the disk.
    for (int i = 0; i < 30; i++) {
        if (!fd_readid(track->phys_cyl, track->phys_head, track->data_mode,
                       &cmd)) {
            // FIXME: retry
            printf(" readid failed\n");
            return false;
        }

        seen_sec[cmd.reply[5]] = true;

        // Check the other values are consistent with what we saw first time.
        if ((cmd.reply[3] != track->log_cyl)
            || (cmd.reply[4] != track->log_head)
            || (cmd.reply[6] != track->sector_size)) {
            // FIXME: handle better
            printf(" mixed sector formats\n");
            return false;
        }

        // FIXME: we could get out of this loop sooner with a heuristic
    }

    // Find the range of sectors involved.
    // We must have seen at least one sector by this point.
    int first = MAX_SECS;
    int last = 0;
    for (int i = 0; i < MAX_SECS; i++) {
        if (seen_sec[i]) {
            if (i < first) {
                first = i;
            }
            if (i > last) {
                last = i;
            }
        }
    }

    // Check it's contiguous. (We don't handle cases where it isn't.)
    for (int i = first; i <= last; i++) {
        if (!seen_sec[i]) {
            // FIXME: this is a bad idea on dodgy disks
            printf(" sector numbering not contiguous\n");
            return false;
        }
    }

    track->first_sector = first;
    track->num_sectors = (last + 1) - first;

    printf(" %dx%d (%d-%d)\n", track->num_sectors,
                               sector_bytes(track->sector_size),
                               track->first_sector, last);

    track->probed = true;
    return true;
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

    disk_t disk;
    init_disk(&disk);

    disk.num_phys_cyls = 80; // FIXME: option for this
    disk.num_phys_heads = 2;

    for (int cyl = 0; cyl < disk.num_phys_cyls; cyl++) {
        for (int head = 0; head < disk.num_phys_heads; head++) {
            track_t *track = &(disk.tracks[cyl][head]);

            if (!probe_track(track)) {
                printf("probe failed\n");
            }

            // ... read track
        }
    }

    // FIXME: if C == cyl, OK
    // if C == cyl / 2, ask for doublestepping
    // if C == cyl * 2, complain that this is the wrong kind of drive

    close(dev_fd);
}

static void usage(void) {
    fprintf(stderr, "usage: dumpfloppy [-d NUM] [IMAGE-FILE]\n");
    fprintf(stderr, "  -d NUM     drive number to read from (default 0)\n");
    // FIXME: choice of interleaving in output formats (.dsd, .adl, .adf)
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
