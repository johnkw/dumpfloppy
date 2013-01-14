/*
   dumpfloppy: read a floppy disk using the PC controller

   Copyright (C) 2013 Adam Sampson

   This is based on the "How to identify an unknown disk" document from the
   fdutils project.
*/

// FIXME: Write an IMD-to-flat tool (i.e. an IMD loader and flat writer)
//   FIXME: with choice of interleaving in output formats (.dsd, .adl, .adf)

#include <errno.h>
#include <fcntl.h>
#include <linux/fd.h>
#include <linux/fdreg.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
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

static int sector_bytes(int code) {
    return 128 << code;
}

typedef enum {
    HEADS_NORMAL, // physical == logical
    HEADS_SEPARATE, // all are logical head 0
} head_mode_t;

typedef struct {
    uint8_t imd_mode;
    const char *name;
    int rate; // 0 to 3
    bool is_fm;
} data_mode_t;

// Following what the .IMD spec says, the rates here are the data transfer rate
// to the drive -- FM-500k transfers half as much data as MFM-500k owing to the
// less efficient encoding.
// FIXME: index this array by imd_mode, and have a separate probe list
const data_mode_t DATA_MODES[] = {
    // 5.25" DD/QD and 3.5" DD drives
    { 5, "MFM-250k", 2, false },
    { 2, "FM-250k", 2, true },

    // DD media in 5.25" HD drives
    { 4, "MFM-300k", 1, false },
    { 1, "FM-300k", 1, true },

    // 3.5" HD, 5.25" HD and 8" drives
    { 3, "MFM-500k", 0, false },
    { 0, "FM-500k", 0, true },

    // 3.5" ED drives
    { 6, "MFM-1000k", 3, false }, // FIXME: not in IMD spec
    // Rate 3 for FM isn't allowed.

    { -1, NULL, 0, false }
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
    uint8_t log_cyl;
    uint8_t log_head;
    uint8_t log_sector;
    unsigned char *data; // NULL if not read yet
} sector_t;

const sector_t EMPTY_SECTOR = {
    .log_cyl = 0xFF,
    .log_head = 0xFF,
    .log_sector = 0xFF,
    .data = NULL,
};

static void init_sector(sector_t *sector) {
    *sector = EMPTY_SECTOR;
}

static void free_sector(sector_t *sector) {
    free(sector->data);
    sector->data = NULL;
}

#define MAX_SECS 256
typedef struct {
    bool probed;
    const data_mode_t *data_mode;
    int phys_cyl;
    int phys_head;
    int num_sectors;
    int sector_size_code; // FDC code
    sector_t sectors[MAX_SECS]; // indexed by physical sector
} track_t;

const track_t EMPTY_TRACK = {
    .probed = false,
    .data_mode = NULL,
    .phys_cyl = -1,
    .phys_head = -1,
    .num_sectors = -1,
    .sector_size_code = -1,
};

static void init_track(int phys_cyl, int phys_head, track_t *track) {
    *track = EMPTY_TRACK;
    track->phys_cyl = phys_cyl;
    track->phys_head = phys_head;
    for (int i = 0; i < MAX_SECS; i++) {
        init_sector(&(track->sectors[i]));
    }
}

static void free_track(track_t *track) {
    track->probed = false;
    track->num_sectors = 0;
    for (int i = 0; i < MAX_SECS; i++) {
        free_sector(&(track->sectors[i]));
    }
}

#define MAX_CYLS 256
#define MAX_HEADS 2
typedef struct {
    int num_phys_cyls;
    int num_phys_heads;
    track_t tracks[MAX_CYLS][MAX_HEADS]; // indexed by physical cyl/head
    int cyl_step; // how many physical cyls to step for each logical one
    head_mode_t head_mode;
} disk_t;

const disk_t EMPTY_DISK = {
    .num_phys_cyls = -1,
    .num_phys_heads = 2,
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

static void free_disk(disk_t *disk) {
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            free_track(&(disk->tracks[cyl][head]));
        }
    }
}

static void copy_track_layout(const disk_t *disk,
                              const track_t *src, track_t *dest) {
    if (!src->probed) return;

    free_track(dest);

    dest->probed = true;
    dest->data_mode = src->data_mode;
    dest->num_sectors = src->num_sectors;
    dest->sector_size_code = src->sector_size_code;

    int cyl_diff = dest->phys_cyl - src->phys_cyl;
    for (int i = 0; i < src->num_sectors; i++) {
        const sector_t *src_sec = &(src->sectors[i]);
        sector_t *dest_sec = &(dest->sectors[i]);

        dest_sec->log_cyl = src_sec->log_cyl + cyl_diff;
        // FIXME: If we only ever copied from the same head, this wouldn't be
        // necessary
        switch (disk->head_mode) {
        case HEADS_NORMAL:
            dest_sec->log_head = dest->phys_head;
            break;
        case HEADS_SEPARATE:
            dest_sec->log_head = 0;
            break;
        }
        dest_sec->log_sector = src_sec->log_sector;
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
static bool fd_readid(const track_t *track, struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    cmd->cmd[0] = FD_READID;
    cmd->cmd[1] = drive_selector(track->phys_head);
    cmd->cmd_count = 2;
    cmd->flags = FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd->track = track->phys_cyl;
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

// Read data from sectors with consecutive logical sector IDs.
// The sector_t given is for the first sector to be read.
//
// Return true if all data was read. Upon return:
// cmd->reply[0]--[2] are ST0-ST2
// cmd->reply[3] is logical cyl
// cmd->reply[4] is logical head
// cmd->reply[5] is logical sector
// (128 << cmd->reply[6]) is sector size
static bool fd_read(const track_t *track, const sector_t *sector,
                    unsigned char *buf, size_t buf_size,
                    struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    // 0x80 is the MT (multiple tracks) bit.
    cmd->cmd[0] = FD_READ & ~0x80;
    cmd->cmd[1] = drive_selector(track->phys_head);
    cmd->cmd[2] = sector->log_cyl;
    cmd->cmd[3] = sector->log_head;
    cmd->cmd[4] = sector->log_sector;
    cmd->cmd[5] = track->sector_size_code;
    // End of track sector number.
    cmd->cmd[6] = 0xFF;
    // Intersector gap. There's a complex table of these for various formats in
    // the M1543C datasheet; the fdutils manual says it doesn't make any
    // difference for read. FIXME: hmm.
    cmd->cmd[7] = 0x1B;
    // Bytes in sector -- but only if size code is 0, else it should be 0xFF.
    if (track->sector_size_code == 0) {
        cmd->cmd[8] = sector_bytes(track->sector_size_code);
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

// Read a sector ID and append it to the sectors in the track.
static bool track_readid(track_t *track) {
    struct floppy_raw_cmd cmd;

    if (track->num_sectors >= MAX_SECS) {
        die("track_readid read too many sectors");
    }

    if (!fd_readid(track, &cmd)) {
        return false;
    }

    sector_t *sector = &(track->sectors[track->num_sectors]);
    sector->log_cyl = cmd.reply[3];
    sector->log_head = cmd.reply[4];
    sector->log_sector = cmd.reply[5];

    if (track->sector_size_code != -1
        && track->sector_size_code != cmd.reply[6]) {
        // FIXME: handle this better -- e.g. discard all but first?
        // or keep them and write multiple .IMDs?
        die("mixed sector formats within track");
    }
    track->sector_size_code = cmd.reply[6];

    track->num_sectors++;
    return true;
}

// Find the sectors with the lowest and highest logical IDs in a track,
// and whether the sectors have contiguous logical IDs.
static void track_scan_sectors(track_t *track,
                               sector_t **lowest, sector_t **highest,
                               bool *contiguous) {
    bool seen[MAX_SECS];
    for (int i = 0; i < MAX_SECS; i++) {
        seen[i] = false;
    }

    *lowest = NULL;
    int lowest_id = MAX_SECS;
    *highest = NULL;
    int highest_id = 0;
    for (int i = 0; i < track->num_sectors; i++) {
        sector_t *sector = &(track->sectors[i]);
        const int id = sector->log_sector;

        seen[sector->log_sector] = true;

        if (id < lowest_id) {
            lowest_id = id;
            *lowest = sector;
        }
        if (id > highest_id) {
            highest_id = id;
            *highest = sector;
        }
    }

    *contiguous = true;
    for (int i = lowest_id; i < highest_id; i++) {
        if (!seen[i]) {
            *contiguous = false;
        }
    }
}

static bool probe_track(track_t *track) {
    free_track(track);

    printf("Probing %02d.%d:", track->phys_cyl, track->phys_head);
    fflush(stdout);

    // Identify the data mode (assuming there's only one; we don't handle disks
    // with multiple modes per track).
    // Try all the possible modes until we can read a sector ID.
    track->num_sectors = 0;
    track->sector_size_code = -1;
    for (int i = 0; ; i++) {
        if (DATA_MODES[i].name == NULL) {
            printf(" unknown data mode\n");
            // FIXME: retry
            return false;
        }

        track->data_mode = &DATA_MODES[i];
        if (track_readid(track)) {
            printf(" %s", track->data_mode->name);
            fflush(stdout);
            break;
        }
    }

    // Identify the sector numbering scheme.
    // Read enough IDs for a few revolutions of the disk.
    for (int i = 0; i < 30; i++) {
        if (!track_readid(track)) {
            // FIXME: retry
            printf(" readid failed\n");
            return false;
        }

        // FIXME: we could get out of this loop sooner with a heuristic
        // (if we've seen every sector ID either not at all or at least twice,
        // then we've seen a full rotation, assuming we didn't miss any)
    }

    // We've now got a sample of the sequence of sectors, e.g.
    // 7 8 9 1 2 3 4 5 6 7 8 9 1 2 3 4 5 6 7 8 9 1 2 3
    //
    // We will find the last instance of the lowest sector:
    //                                           |
    //
    // And then take the sequence until the previous instance of it, which
    // should hopefully cover a complete rotation:
    //                         [----------------]
    //
    // FIXME: Ideally we would start from the index hole, not from the
    // lowest-numbered sector, but I don't know how to wait for the index
    // hole. (Maybe do a readid with a deliberately wrong format?)

    // Find the lowest-numbered sector.
    int end_sec = MAX_SECS;
    int end_pos = -1;
    for (int i = 0; i < track->num_sectors; i++) {
        const int sec = track->sectors[i].log_sector;
        if (sec < end_sec) {
            end_sec = sec;
            end_pos = i;
        }
    }

    // Scan backwards until we find it again.
    int start_pos = end_pos - 1;
    while (track->sectors[start_pos].log_sector != end_sec) {
        start_pos--;
        if (start_pos < 0) {
            printf(" lowest sector only seen once\n");
            return false;
        }
    }

    // FIXME: warn if there are any sector IDs in the sample that don't occur
    // in the range

    // Reduce the sector list to just the range we've chosen.
    track->num_sectors = end_pos - start_pos;
    memmove(&track->sectors[0], &track->sectors[start_pos],
            sizeof(track->sectors[0]) * track->num_sectors);

    printf(" %dx%d", track->num_sectors, sector_bytes(track->sector_size_code));

    sector_t *lowest, *highest;
    bool contiguous;
    track_scan_sectors(track, &lowest, &highest, &contiguous);
    if (contiguous) {
        printf(" %d-%d", lowest->log_sector, highest->log_sector);
    } else {
        for (int i = 0; i < track->num_sectors; i++) {
            printf(" %d", track->sectors[i].log_sector);
        }
    }
    printf("\n");

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

    printf("Reading %02d.%d:", track->phys_cyl, track->phys_head);
    fflush(stdout);

    sector_t *lowest_sector, *highest_sector;
    bool contiguous;
    track_scan_sectors(track, &lowest_sector, &highest_sector, &contiguous);

    const int sector_size = sector_bytes(track->sector_size_code);
    const int track_size = sector_size * track->num_sectors;
    unsigned char track_data[track_size];
    bool read_whole_track = false;

    if (contiguous) {
        // Try reading the whole track to start with.
        // If this works, it's a lot faster than reading sector-by-sector.
        // The resulting data will be ordered by *logical* ID.
        if (fd_read(track, lowest_sector, track_data, track_size, &cmd)) {
            read_whole_track = true;
            printf(" %d-%d+",
                   lowest_sector->log_sector, highest_sector->log_sector);
        }
    }

    // Get sectors in physical order.
    bool all_ok = true;
    for (int i = 0; i < track->num_sectors; i++) {
        sector_t *sector = &(track->sectors[i]);

        if (sector->data != NULL) {
            // Already got this one.
            printf(" (%d)", sector->log_sector);
            continue;
        }

        // Allocate the sector.
        sector->data = malloc(sector_size);
        if (sector->data == NULL) {
            die("malloc failed");
        }

        printf(" %d", sector->log_sector);
        fflush(stdout);

        if (read_whole_track) {
            // We read this sector as part of the whole track.
            const int rel_sec = sector->log_sector - lowest_sector->log_sector;
            memcpy(sector->data,
                   track_data + (sector_size * rel_sec),
                   sector_size);
            printf("=");
            continue;
        }

        // Read a single sector.
        if (!fd_read(track, sector, sector->data, sector_size, &cmd)) {
            // Failed -- throw it away.
            free(sector->data);
            sector->data = NULL;

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
    sector_t *sec0 = &(side0->sectors[0]);
    track_t *side1 = &(disk->tracks[cyl][1]);
    sector_t *sec1 = &(side0->sectors[0]);

    if (!(side0->probed || side1->probed)) {
        die("Cylinder 2 unreadable on either side");
    } else if (side0->probed && !side1->probed) {
        printf("Single-sided disk\n");
        disk->num_phys_heads = 1;
    } else if (sec0->log_head == 0 && sec1->log_head == 0) {
        printf("Double-sided disk with separate sides\n");
        disk->head_mode = HEADS_SEPARATE;
    } else {
        printf("Double-sided disk\n");
    }

    if (sec0->log_cyl * 2 == side0->phys_cyl) {
        printf("Doublestepping required (40T disk in 80T drive)\n");
        disk->cyl_step = 2;
    } else if (sec0->log_cyl == side0->phys_cyl * 2) {
        die("Can't read this disk (80T disk in 40T drive)");
    } else if (sec0->log_cyl != side0->phys_cyl) {
        printf("Mismatch between physical and logical cylinders\n");
    }
}

static void write_imd_header(FILE *image) {
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);

    fprintf(image, "IMD 1.18-%s-%s: %02d/%02d/%04d %02d:%02d:%02d\n",
            PACKAGE_NAME, PACKAGE_VERSION,
            local->tm_mday, local->tm_mon + 1, local->tm_year + 1900,
            local->tm_hour, local->tm_min, local->tm_sec);
    fputc(0x1A, image);
}

#define IMD_NEED_CYL_MAP 0x80
#define IMD_NEED_HEAD_MAP 0x40
#define IMD_SDR_UNAVAILABLE 0x00
#define IMD_SDR_NORMAL 0x01
static void write_imd_track(const track_t *track, FILE *image) {
    uint8_t flags = 0;

    uint8_t sec_map[track->num_sectors];
    uint8_t cyl_map[track->num_sectors];
    uint8_t head_map[track->num_sectors];
    for (int i = 0; i < track->num_sectors; i++) {
        const sector_t *sector = &(track->sectors[i]);

        // FIXME: when the .IMD spec says the sector map lists "the physical ID
        // for each sector", it means "the *logical* ID". (That is, if you
        // image a PC floppy with ImageDisk, it produces 01 02 ... 09 here; if
        // you image a BBC floppy it produces 00 01 .. 09.)
        sec_map[i] = sector->log_sector;
        cyl_map[i] = sector->log_cyl;
        head_map[i] = sector->log_head;

        if (cyl_map[i] != track->phys_cyl) {
            flags |= IMD_NEED_CYL_MAP;
        }
        if (head_map[i] != track->phys_head) {
            flags |= IMD_NEED_HEAD_MAP;
        }
    }

    const uint8_t header[] = {
        track->data_mode->imd_mode,
        track->phys_cyl,
        flags | track->phys_head,
        track->num_sectors,
        track->sector_size_code,
    };
    fwrite(header, 1, 5, image);

    fwrite(sec_map, 1, track->num_sectors, image);
    if (flags & IMD_NEED_CYL_MAP) {
        fwrite(cyl_map, 1, track->num_sectors, image);
    }
    if (flags & IMD_NEED_HEAD_MAP) {
        fwrite(head_map, 1, track->num_sectors, image);
    }

    const int sector_size = sector_bytes(track->sector_size_code);
    for (int i = 0; i < track->num_sectors; i++) {
        const sector_t *sector = &(track->sectors[i]);
        if (sector->data == NULL) {
            fputc(IMD_SDR_UNAVAILABLE, image);
        } else {
            // FIXME: compress if all bytes the same
            fputc(IMD_SDR_NORMAL, image);
            fwrite(sector->data, 1, sector_size, image);
        }
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

        write_imd_header(image);
    }

    for (int cyl = 0; cyl < disk.num_phys_cyls; cyl += disk.cyl_step) {
        for (int head = 0; head < disk.num_phys_heads; head++) {
            track_t *track = &(disk.tracks[cyl][head]);

            // FIXME: option to force probe
            if (head > 0) {
                // Try the mode from the previous head on the same cyl.
                copy_track_layout(&disk, &(disk.tracks[cyl][head - 1]), track);
            } else if (cyl > 0) {
                // Try the mode from the previous cyl on the same head.
                copy_track_layout(&disk, &(disk.tracks[cyl - 1][head]), track);
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

            if (image != NULL) {
                write_imd_track(track, image);
                fflush(image);
            }
        }
    }

    if (image != NULL) {
        fclose(image);
    }
    free_disk(&disk);
    close(dev_fd);
}

static void usage(void) {
    fprintf(stderr, "usage: dumpfloppy [-d NUM] [IMAGE-FILE]\n");
    fprintf(stderr, "  -d NUM     drive number to read from (default 0)\n");
    // FIXME: -t COUNT    drive has COUNT tracks
    // FIXME: -S SEC      ignore sectors with logical ID SEC (for RM disks)
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
