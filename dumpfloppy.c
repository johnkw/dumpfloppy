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

typedef enum {
    HEADS_NORMAL, // physical == logical
    HEADS_SEPARATE, // all are logical head 0
} head_mode_t;

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
    unsigned char *data; // NULL if not read yet
} sector_t;

const sector_t EMPTY_SECTOR = {
    .data = NULL,
};

static void init_sector(sector_t *sector) {
    *sector = EMPTY_SECTOR;
}

#define MAX_SECS 256
typedef struct {
    bool probed;
    int phys_cyl;
    int phys_head;
    int log_cyl;
    int log_head;
    const data_mode_t *data_mode;
    int sector_size_code; // FDC code
    int sector_size; // derived from sector_size
    int first_sector;
    int last_sector;
    sector_t sectors[MAX_SECS]; // indexed by logical sector
} track_t;

const track_t EMPTY_TRACK = {
    .probed = false,
    .phys_cyl = -1,
    .phys_head = -1,
    .log_cyl = -1,
    .log_head = -1,
    .data_mode = NULL,
    .sector_size_code = -1,
    .sector_size = -1,
    .first_sector = -1,
    .last_sector = -1,
};

static void init_track(int phys_cyl, int phys_head, track_t *track) {
    *track = EMPTY_TRACK;
    track->phys_cyl = phys_cyl;
    track->phys_head = phys_head;
    for (int i = 0; i < MAX_SECS; i++) {
        init_sector(&(track->sectors[i]));
    }
}

#define MAX_CYLS 256
#define MAX_HEADS 2
typedef struct {
    int num_phys_cyls;
    int num_phys_heads;
    int first_log_cyl; // what physical cyl 0 corresponds to
    int cyl_step; // how many physical cyls to step for each logical one
    head_mode_t head_mode;
    track_t tracks[MAX_CYLS][MAX_HEADS]; // indexed by physical cyl/head
} disk_t;

const disk_t EMPTY_DISK = {
    .num_phys_cyls = -1,
    .num_phys_heads = 2,
    .first_log_cyl = 0,
    .cyl_step = 1,
    .head_mode = HEADS_NORMAL,
};

static void init_disk(disk_t *disk) {
    *disk = EMPTY_DISK;
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            init_track(cyl, head, &(disk->tracks[cyl][head]));
        }
    }
}

static void copy_track_mode(const disk_t *disk,
                            const track_t *src, track_t *dest) {
    if (!src->probed) return;

    dest->probed = true;

    dest->log_cyl = dest->phys_cyl + disk->first_log_cyl;
    switch (disk->head_mode) {
    case HEADS_NORMAL:
        dest->log_head = dest->phys_head;
        break;
    case HEADS_SEPARATE:
        dest->log_head = 0;
        break;
    }

    dest->data_mode = src->data_mode;
    dest->sector_size_code = src->sector_size_code;
    dest->sector_size = src->sector_size;
    dest->first_sector = src->first_sector;
    dest->last_sector = src->last_sector;
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

// Read sector data.
//
// Return true if all data was read. Upon return:
// cmd->reply[0]--[2] are ST0-ST2
// cmd->reply[3] is logical cyl
// cmd->reply[4] is logical head
// cmd->reply[5] is logical sector
// (128 << cmd->reply[6]) is sector size
static bool fd_read(const track_t *track, int log_sector,
                    unsigned char *buf, size_t buf_size,
                    struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    // 0x80 is the MT (multiple tracks) bit.
    cmd->cmd[0] = FD_READ & ~0x80;
    cmd->cmd[1] = drive_selector(track->phys_head);
    cmd->cmd[2] = track->log_cyl;
    cmd->cmd[3] = track->log_head;
    cmd->cmd[4] = log_sector;
    cmd->cmd[5] = track->sector_size_code;
    // End of track sector number.
    cmd->cmd[6] = 0xFF;
    // Intersector gap. There's a complex table of these for various formats in
    // the M1543C datasheet; the fdutils manual says it doesn't make any
    // difference for read. FIXME: hmm.
    cmd->cmd[7] = 0x1B;
    // Bytes in sector -- but only if size code is 0, else it should be 0xFF.
    if (track->sector_size_code == 0) {
        cmd->cmd[8] = track->sector_size;
    } else {
        cmd->cmd[8] = 0xFF;
    }
    cmd->cmd_count = 9;
    cmd->flags = FD_RAW_READ | FD_RAW_INTR | FD_RAW_NEED_SEEK;
    // FIXME: check if Linux is doing an implied seek (i.e. phys == log)
    cmd->track = track->phys_cyl;
    cmd->data = buf;
    cmd->length = buf_size;
    apply_data_mode(track->data_mode, cmd);

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
            track->sector_size_code = cmd.reply[6];
            track->sector_size = 128 << track->sector_size_code;
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
            || (cmd.reply[6] != track->sector_size_code)) {
            // FIXME: handle better
            printf(" mixed sector formats\n");
            return false;
        }

        // FIXME: we could get out of this loop sooner with a heuristic
        // (if we've seen every sector ID either not at all or at least twice,
        // then we've seen a full rotation, assuming we didn't miss any)
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
    track->last_sector = last;

    printf(" %dx%d (%d-%d)\n", (last + 1) - first,
                               track->sector_size,
                               first, last);

    track->probed = true;
    return true;
}

// Try to read any sectors in a track that haven't already been read.
// Returns true if everything has been read.
static bool read_track(track_t *track) {
    struct floppy_raw_cmd cmd;

    if (!track->probed) {
        if (!probe_track(track)) {
            return false;
        }
    }

    printf("Reading phys %02d.%d log %02d.%d:",
           track->phys_cyl, track->phys_head, track->log_cyl, track->log_head);
    fflush(stdout);

    // Try reading the whole track to start with.
    // If this works, it's a lot faster than reading sector-by-sector.
    const int num_sectors = (track->last_sector + 1) - track->first_sector;
    const int track_size = track->sector_size * num_sectors;
    unsigned char track_buf[track_size];
    bool read_track = true;
    if (!fd_read(track, track->first_sector, track_buf, track_size, &cmd)) {
        read_track = false;
        printf(" *-");
        fflush(stdout);
    }

    // Put each sector into place.
    bool all_ok = true;
    for (int sec = track->first_sector; sec <= track->last_sector; sec++) {
        if (track->sectors[sec].data != NULL) {
            // Already got this one.
            printf(" (%d)", sec);
            continue;
        }

        printf(" %d", sec);
        fflush(stdout);

        // Allocate the sector.
        track->sectors[sec].data = malloc(track->sector_size);
        if (track->sectors[sec].data == NULL) {
            die("malloc failed");
        }

        if (read_track) {
            // Get it from the whole-track read.
            // FIXME: this is awfully ugly
            memcpy(track->sectors[sec].data,
                   track_buf + (track->sector_size * (sec - track->first_sector)),
                   track->sector_size);
            printf("=");
        } else if (!fd_read(track, sec, track->sectors[sec].data, track->sector_size, &cmd)) {
            // Failed -- throw it away.
            free(track->sectors[sec].data);
            track->sectors[sec].data = NULL;

            printf("-");
            all_ok = false;
        } else {
            printf("+");
        }
        fflush(stdout);
    }

    if (all_ok) {
        printf(" OK\n");
        return true;
    } else {
        printf("\n");
        return false;
    }
}

static void probe_disk(disk_t *disk) {
    // Probe both sides of cylinder 2 to figure out the disk geometry.
    // (Cylinder 2 because we need a physical cylinder greater than 0 to figure
    // out the logical-to-physical mapping, and because cylinder 0 may
    // reasonably be unformatted on disks where it's a bootblock.)

    const int cyl = 2;
    for (int head = 0; head < disk->num_phys_heads; head++) {
        probe_track(&(disk->tracks[cyl][head]));
    }

    track_t *side0 = &(disk->tracks[cyl][0]);
    track_t *side1 = &(disk->tracks[cyl][1]);

    if (!(side0->probed || side1->probed)) {
        die("Cylinder 2 unreadable on either side");
    } else if (side0->probed && !side1->probed) {
        printf("Single-sided disk\n");
        disk->num_phys_heads = 1;
    } else if (side0->log_head == 0 && side1->log_head == 0) {
        printf("Double-sided disk with separate sides\n");
        disk->head_mode = HEADS_SEPARATE;
    } else {
        printf("Double-sided disk\n");
    }

    if (side0->log_cyl * 2 == side0->phys_cyl) {
        printf("Doublestepping required (40T disk in 80T drive)\n");
        disk->cyl_step = 2;
    } else if (side0->log_cyl == side0->phys_cyl * 2) {
        die("Can't read this disk (80T disk in 40T drive)");
    } else if (side0->log_cyl != side0->phys_cyl) {
        printf("Mismatch between physical and logical cylinders\n");
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

    disk_t disk;
    init_disk(&disk);

    disk.num_phys_cyls = 80; // FIXME: option for this
    disk.num_phys_heads = 2;

    probe_disk(&disk);

    FILE *image = NULL;
    if (image_filename != NULL) {
        image = fopen(image_filename, "wb");
        if (image == NULL) {
            die_errno("cannot open %s", image_filename);
        }
    }

    for (int cyl = 0; cyl < disk.num_phys_cyls; cyl += disk.cyl_step) {
        for (int head = 0; head < disk.num_phys_heads; head++) {
            track_t *track = &(disk.tracks[cyl][head]);

            // FIXME: option to force probe
            if (head > 0) {
                // Try the mode from the previous head on the same cyl.
                copy_track_mode(&disk, &(disk.tracks[cyl][head - 1]), track);
            } else if (cyl > 0) {
                // Try the mode from the previous cyl on the same head.
                copy_track_mode(&disk, &(disk.tracks[cyl - 1][head]), track);
            }

            const int max_tries = 10;
            for (int try = 0; ; try++) {
                // FIXME: seek the head around, turn the motor on/off...

                if (try == max_tries) {
                    // Tried too many times; give up.
                    die("Track failed to read after retrying");
                }

                if (read_track(track)) {
                    // Success!
                    break;
                }

                // Failed; reprobe and try again.
                track->probed = false;
            }

            // Write sectors to the image file.
            // FIXME: storing complete tracks would simplify this
            if (image != NULL) {
                for (int sec = track->first_sector; sec <= track->last_sector; sec++) {
                    fwrite(track->sectors[sec].data, 1, track->sector_size, image);
                }
                fflush(image);
            }
        }
    }

    if (image != NULL) {
        fclose(image);
    }
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
