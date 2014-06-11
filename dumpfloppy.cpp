/*
    dumpfloppy: read a floppy disk using the PC controller

    Copyright (C) 2013 Adam Sampson <ats@offog.org>

    Permission to use, copy, modify, and/or distribute this software for
    any purpose with or without fee is hereby granted, provided that the
    above copyright notice and this permission notice appear in all
    copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
    WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
    AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
    DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
    PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
    TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
    PERFORMANCE OF THIS SOFTWARE.
*/

/*
    The techniques used here are based on the "How to identify an
    unknown disk" document from the fdutils project:
      http://www.fdutils.linux.lu/disk-id.html
*/

#include "disk.h"
#include "imd.h"
#include "util.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fd.h>
#include <linux/fdreg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static struct args {
    bool always_probe;
    int drive;
    int tracks;
    int cyl_scale;
    bool read_comment;
    int ignore_sector;
    const char *image_filename;
} args;
static int dev_fd;

static int drive_selector(int head) {
    return (head << 2) | args.drive;
}

// Apply a mode specification to a floppy_raw_cmd -- which must contain only
// one command.
static void apply_data_mode(const data_mode_t *mode,
                            struct floppy_raw_cmd *cmd) {
    cmd->rate = mode->rate;
    // 0x40 is the MFM bit.
    if (mode->is_fm) {
        cmd->cmd[0] &= ~0x40;
    } else {
        cmd->cmd[0] |= 0x40;
    }
}

// Seek the head back to track 0.
// Give up if it's stepped 80 tracks and not found track 0 (so you probably
// want to call this twice, in practice, in case someone's stepped to track
// 80+).
static void fd_recalibrate(struct floppy_raw_cmd *cmd) {
    memset(cmd, 0, sizeof *cmd);

    // 0x07 is RECALIBRATE.
    cmd->cmd[0] = 0x07;
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

    // 0x0A is READ ID.
    cmd->cmd[0] = 0x0A;
    cmd->cmd[1] = drive_selector(track->phys_head);
    cmd->cmd_count = 2;
    cmd->flags = FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd->track = track->phys_cyl * args.cyl_scale;
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

    // 0x06 is READ DATA.
    // (0x80 would be MT - span multiple tracks.)
    // (0x20 would be SK - skip deleted data.)
    cmd->cmd[0] = 0x06;
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
    cmd->track = track->phys_cyl * args.cyl_scale;
    cmd->data = buf;
    cmd->length = buf_size;
    apply_data_mode(track->data_mode, cmd);

    if (ioctl(dev_fd, FDRAWCMD, cmd) < 0) {
        die_errno("FD_READID failed");
    }
    if (cmd->reply_count < 7) {
        die("FD_READID returned short reply");
    }

    // If we're reading multiple sectors but hit a deleted sector, then the
    // read will have stopped there -- fail.
    if (buf_size > sector_bytes(track->sector_size_code)
        && (cmd->reply[2] & 0x40) != 0) {
        return false;
    }

    // If ST0 interrupt code is 00, success.
    return ((cmd->reply[0] >> 6) & 3) == 0;
}

// Read a sector ID and append it to the sectors in the track.
static sector_t *track_readid(track_t *track) {
    struct floppy_raw_cmd cmd;

    if (track->num_sectors == MAX_SECS-1) {
        die("track_readid read too many sectors");
    }

    do {
        if (!fd_readid(track, &cmd)) {
            return NULL;
        }
    } while (args.ignore_sector == cmd.reply[5]);

    sector_t *sector = &(track->sectors[track->num_sectors]);
    free_sector(sector);
    sector->log_cyl = cmd.reply[3];
    sector->log_head = cmd.reply[4];
    sector->log_sector = cmd.reply[5];
    assert(cmd.reply[6] != UCHAR_MAX);

    if (track->sector_size_code != UCHAR_MAX
        && track->sector_size_code != cmd.reply[6]) {
        // FIXME: handle this better -- e.g. discard all but first?
        // or keep them and write multiple .IMDs?
        die("mixed sector formats within track");
    }
    track->sector_size_code = cmd.reply[6];

    track->num_sectors++;
    return sector;
}

// Identify the data mode and sector layout of a track.
static bool probe_track(track_t *track) {
    free_track(track);

    printf("Probe %2d.%d:", track->phys_cyl, track->phys_head);
    fflush(stdout);

    // We want to make sure that we start reading sector IDs from the index
    // hole. However, there isn't really a good way of finding out where the
    // hole is -- other than getting the controller to do a failing read,
    // where it'll give up when it sees the index hole for the Nth time.
    //
    // So we need to ensure that we've done at least one readid that failed
    // before we have a successful one -- that way, the successful one will
    // definitely be at the start of the track!
    //
    // The first readid we'll do in the loop below will be with DATA_MODES[0],
    // so do a different one to ensure that at least one of them will fail.
    track->data_mode = &DATA_MODES[1];
    track_readid(track);

    // Try all the possible data modes until we can read a sector ID.
    track->num_sectors = 0;
    track->sector_size_code = -1;
    for (int i = 0; ; i++) {
        if (DATA_MODES[i].name == NULL) {
            printf(" unknown data mode\n");
            return false;
        }

        track->data_mode = &DATA_MODES[i];
        if (track_readid(track) != NULL) {
            // This succeeded -- so we're at the start of the track
            // (see above).
            break;
        }
    }

    // Track how many times we've seen each logical sector.
    int seen_secs[MAX_SECS];
    for (int i = 0; i < MAX_SECS; i++) {
        seen_secs[i] = 0;
    }

    // FIXME: if the first sector wasn't the lowest-numbered one, this is
    // highly suspicious -- dump it and start again unless it does the same
    // thing a couple of times

    // Read sector IDs until we've seen the complete sequence several times.
    int count = 0;
    while (true) {
        sector_t *sector = track_readid(track);
        if (sector == NULL) {
            printf(" readid failed\n");
            return false;
        }

        seen_secs[sector->log_sector]++;

        // We can be reasonably confident that we've got them all once we've
        // seen each sector at least min_seen times.
        const int min_seen = 3;
        bool seen_all = true;
        for (int i = 0; i < MAX_SECS; i++) {
            if (seen_secs[i] != 0 && seen_secs[i] < min_seen) {
                seen_all = false;
            }
        }
        if (seen_all) break;

        // Make sure we don't get stuck in this loop forever (although this is
        // highly unlikely).
        const int max_count = 100;
        if (count > max_count) {
            printf(" spent too long looking for sector IDs\n");
            return false;
        }
    }

    // Find where the first sector repeats, and cut the sequence off there.
    int end_pos = 1;
    while (!same_sector_addr(&(track->sectors[0]),
                             &(track->sectors[end_pos]))) {
        end_pos++;
        if (end_pos == track->num_sectors) {
            printf(" couldn't find repeat of first sector\n");
            return false;
        }
    }

    // Check that the sequence repeated itself consistently after that.
    // If we're missing sectors, this has a reasonable chance of spotting it.
    // FIXME: There should be an option to override this for *really* dodgy
    // disks, and just assume the sectors are in order.
    for (int pos = end_pos; pos < track->num_sectors; pos++) {
        if (!same_sector_addr(&(track->sectors[pos % end_pos]),
                              &(track->sectors[pos]))) {
            printf("  sector sequence did not repeat consistently\n");
            return false;
        }
    }

    // Cut the sequence to length.
    track->num_sectors = end_pos;

    // Show what we found.
    printf(" %s %dx%d:",
           track->data_mode->name,
           track->num_sectors, sector_bytes(track->sector_size_code));
    for (int i = 0; i < track->num_sectors; i++) {
        printf(" %d", track->sectors[i].log_sector);
    }
    printf("\n");

    track->status = TRACK_PROBED;
    return true;
}

// Try to read any sectors in a track that haven't already been read.
// Returns true if everything has been read.
static bool read_track(track_t *track) {
    struct floppy_raw_cmd cmd;

    if (track->status == TRACK_UNKNOWN) {
        if (!probe_track(track)) {
            return false;
        }
    }

    printf("Read  %2d.%d:", track->phys_cyl, track->phys_head);
    fflush(stdout);

    sector_t *lowest_sector, *highest_sector;
    bool contiguous;
    track_scan_sectors(track, &lowest_sector, &highest_sector, &contiguous);

    const int sector_size = sector_bytes(track->sector_size_code);
    const int track_size = sector_size * track->num_sectors;
    unsigned char track_data[track_size];
    bool read_whole_track = false;

    // FIXME: Read with the flag set that means deleted sectors won't be
    // ignored (since we can't tell from readid whether the sectors were
    // regular or deleted).
    // FIXME: Describe read errors, with the phys/log context.

    if (contiguous) {
        // Try reading the whole track to start with.
        // If this works, it's a lot faster than reading sector-by-sector.
        // The resulting data will be ordered by *logical* ID.
        if (fd_read(track, lowest_sector, track_data, track_size, &cmd)) {
            read_whole_track = true;
        }
    }

    // Get sectors in physical order.
    bool all_ok = true;
    for (int i = 0; i < track->num_sectors; i++) {
        sector_t *sector = &(track->sectors[i]);

        if (sector->status == SECTOR_GOOD) {
            // Already got this one.
            printf("    ");
            continue;
        }

        uint8_t *data = (uint8_t*)malloc(sector_size);
        if (data == NULL) {
            die("malloc failed");
        }
        memset(data, 0, sector_size);

        printf("%3d", sector->log_sector);
        fflush(stdout);

        if (read_whole_track) {
            // We read this sector as part of the whole track. Success!
            const int rel_sec = sector->log_sector - lowest_sector->log_sector;
            memcpy(data, track_data + (sector_size * rel_sec), sector_size);

            sector->status = SECTOR_GOOD;
            free(sector->data);
            sector->data = data;
            sector->deleted = false;

            printf("*");
            continue;
        }

        // Read a single sector.
        if (!fd_read(track, sector, data, sector_size, &cmd)) {
            // 0x20 is CRC Error in Data Field.
            if ((cmd.reply[2] & 0x20) != 0) {
                // Bad data. Better than nothing, but we'll want to try again.
                sector->status = SECTOR_BAD;
            } else {
                // No data.
                free(data);
                data = NULL;
            }
            all_ok = false;
        } else {
            // Success!
            sector->status = SECTOR_GOOD;
        }

        if (data != NULL) {
            // 0x40 is Control Mark -- a deleted sector was read.
            sector->deleted = (cmd.reply[2] & 0x40) != 0;

            free(sector->data);
            sector->data = data;

            if (sector->status == SECTOR_BAD) {
                printf("?");
            } else if (sector->deleted) {
                printf("x");
            } else {
                printf("+");
            }
        } else {
            printf("-");
        }
        fflush(stdout);
    }

    printf("\n");
    return all_ok;
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
    sector_t *sec1 = &(side1->sectors[0]);

    if (side0->status == TRACK_UNKNOWN && side1->status == TRACK_UNKNOWN) {
        die("Cylinder 2 unreadable on either side");
    } else if (side1->status == TRACK_UNKNOWN) {
        printf("Single-sided disk\n");
        disk->num_phys_heads = 1;
    } else if (sec0->log_head == 0 && sec1->log_head == 0) {
        printf("Double-sided disk with separate sides\n");
    } else {
        printf("Double-sided disk\n");
    }

    if (sec0->log_cyl * 2 == side0->phys_cyl) {
        printf("Doublestepping required (40T disk in 80T drive)\n");
        args.cyl_scale = 2;
    } else if (sec0->log_cyl == side0->phys_cyl * 2) {
        die("Can't read this disk (80T disk in 40T drive)");
    } else if (sec0->log_cyl != side0->phys_cyl) {
        printf("Mismatch between physical and logical cylinders\n");
    }
}

static void process_floppy(void) {
    char dev_filename[] = "/dev/fdX";
    dev_filename[7] = '0' + args.drive;

    dev_fd = open(dev_filename, O_ACCMODE | O_NONBLOCK);
    if (dev_fd == -1) {
        die_errno("cannot open %s", dev_filename);
    }

    // Get BIOS parameters for drive.
    // These aren't necessarily accurate (e.g. there's no BIOS type for an
    // 80-track 5.25" DD drive)...
    struct floppy_drive_params drive_params;
    if (ioctl(dev_fd, FDGETDRVPRM, &drive_params) < 0) {
        die_errno("cannot get drive parameters");
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
    make_disk_comment(PACKAGE_NAME, PACKAGE_VERSION, &disk);

    if (args.read_comment) {
        if (isatty(0)) {
            fprintf(stderr, "Enter comment, terminated by EOF\n");
        }

        while (true) {
            char buf[4096];
            ssize_t count = read(0, buf, sizeof buf);
            if (count == 0) break;
            if (count < 0) {
                die("read from stdin failed");
            }

            alloc_append(buf, count, &disk.comment, &disk.comment_len);
        }
    }

    if (args.tracks == -1) {
        disk.num_phys_cyls = drive_params.tracks;
    } else {
        disk.num_phys_cyls = args.tracks;
    }
    disk.num_phys_heads = 2;

    probe_disk(&disk);
    disk.num_phys_cyls /= args.cyl_scale;

    FILE *image = NULL;
    if (args.image_filename != NULL) {
        // FIXME: if the image exists already, load it
        // (so the comment is preserved)

        image = fopen(args.image_filename, "wb");
        if (image == NULL) {
            die_errno("cannot open %s", args.image_filename);
        }

        write_imd_header(&disk, image);
    }

    // FIXME: retry disk if not complete -- option for number of retries
    // FIXME: if retrying, ensure we've moved the head across the disk
    // FIXME: if retrying, turn the motor off and on (delay? close?)
    // FIXME: pull this out to a read_disk function
    for (int cyl = 0; cyl < disk.num_phys_cyls; cyl++) {
        for (int head = 0; head < disk.num_phys_heads; head++) {
            track_t *track = &(disk.tracks[cyl][head]);

            if (args.always_probe) {
                // Don't assume a layout.
            } else if (cyl > 0) {
                // Try the layout of the previous cyl on the same head.
                copy_track_layout(&(disk.tracks[cyl - 1][head]), track);
            }

            // FIXME: option for this
            const int max_tries = 10;
            for (int try_num = 0; ; try_num++) {
                if (try_num == max_tries) {
                    // Tried too many times; give up.
                    break;
                }

                if (read_track(track)) {
                    // Success!
                    break;
                }

                if (track->status == TRACK_GUESSED) {
                    // Maybe we guessed wrong. Probe and try again.
                    track->status = TRACK_UNKNOWN;
                }
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
    fprintf(stderr, "usage: dumpfloppy [OPTION]... [IMAGE-FILE]\n");
    fprintf(stderr, "  -a         probe each track before reading\n");
    fprintf(stderr, "  -d NUM     drive number to read from (default 0)\n");
    fprintf(stderr, "  -t TRACKS  drive has TRACKS tracks (default autodetect)\n");
    fprintf(stderr, "  -C         read comment from stdin\n");
    fprintf(stderr, "  -S SEC     ignore sectors with logical ID SEC\n");
    // FIXME: -h HEAD     read single-sided image from head HEAD
}

int main(int argc, char **argv) {
    dev_fd = -1;
    args.always_probe = false;
    args.drive = 0;
    args.tracks = -1;
    args.cyl_scale = 1;
    args.read_comment = false;
    args.ignore_sector = -1;
    args.image_filename = NULL;

    while (true) {
        int opt = getopt(argc, argv, "ad:t:CS:");
        if (opt == -1) break;

        switch (opt) {
        case 'a':
            args.always_probe = true;
            break;
        case 'd':
            args.drive = atoi(optarg);
            break;
        case 't':
            args.tracks = atoi(optarg);
            break;
        case 'C':
            args.read_comment = true;
            break;
        case 'S':
            args.ignore_sector = atoi(optarg);
            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind == argc) {
        // No image file.
    } else if (optind + 1 == argc) {
        args.image_filename = argv[optind];
    } else {
        usage();
        return 0;
    }

    process_floppy();

    return 0;
}
