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
    int max_tries;
    bool retry;
} args;
static int dev_fd;

static int drive_selector(int head) {
    return (head << 2) | args.drive;
}

// Apply a mode specification to a floppy_raw_cmd -- which must contain only
// one command.
static void apply_data_mode(const data_mode_t* mode,
                            struct floppy_raw_cmd& cmd) {
    assert(mode);
    cmd.rate = mode->rate;
    // 0x40 is the MFM bit.
    if (mode->is_fm) {
        cmd.cmd[0] &= ~0x40;
    } else {
        cmd.cmd[0] |= 0x40;
    }
}

// Seek the head back to track 0.
// Give up if it's stepped 80 tracks and not found track 0 (so you probably
// want to call this twice, in practice, in case someone's stepped to track
// 80+).
static void fd_recalibrate(struct floppy_raw_cmd& cmd) {
    memset(&cmd, 0, sizeof(cmd));

    // 0x07 is RECALIBRATE.
    cmd.cmd[0] = 0x07;
    cmd.cmd[1] = drive_selector(0);
    cmd.cmd_count = 2;
    cmd.flags = FD_RAW_INTR;

    if (ioctl(dev_fd, FDRAWCMD, &cmd) < 0) {
        die_errno("FD_RECALIBRATE failed");
    }
}

// Read ID field of whatever sector reaches the head next.
// Give up after two index holes if nothing has been read.
//
// Return true if a sector ID was read. Upon return:
// cmd.reply[0]--[2] are ST0-ST2
// cmd.reply[3] is logical cyl
// cmd.reply[4] is logical head
// cmd.reply[5] is logical sector
// (128 << cmd.reply[6]) is sector size
static bool fd_readid(const track_t& track, struct floppy_raw_cmd& cmd) {
    memset(&cmd, 0, sizeof(cmd));

    // 0x0A is READ ID.
    cmd.cmd[0] = 0x0A;
    cmd.cmd[1] = drive_selector(track.phys_head);
    cmd.cmd_count = 2;
    cmd.flags = FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd.track = track.phys_cyl * args.cyl_scale;
    apply_data_mode(track.data_mode, cmd);

    if (ioctl(dev_fd, FDRAWCMD, &cmd) < 0) {
        die_errno("FD_READID failed");
    }
    if (cmd.reply_count < 7) {
        die("FD_READID returned short reply");
    }

    // If ST0 interrupt code is 00, success.
    return ((cmd.reply[0] >> 6) & 3) == 0;
}

// See: https://web.archive.org/web/20140620002630/http://cpctech.cpc-live.com/docs/upd765a/necfdc.htm

// Read data from sectors with consecutive logical sector IDs.
// The sector_t given is for the first sector to be read.
//
// Return true if all data was read. Upon return:
// cmd.reply[0]--[2] are ST0-ST2
// cmd.reply[3] is logical cyl
// cmd.reply[4] is logical head
// cmd.reply[5] is logical sector
// (128 << cmd.reply[6]) is sector size
static bool fd_read(const track_t& track, const sector_t& sector,
                    unsigned char *buf, size_t buf_size,
                    struct floppy_raw_cmd& cmd) {
    memset(&cmd, 0, sizeof(cmd));

    // 0x06 is READ DATA.
    // (0x80 would be MT - span multiple tracks.)
    // (0x20 would be SK - skip deleted data.)
    cmd.cmd[0] = 0x06;
    cmd.cmd[1] = drive_selector(track.phys_head);
    cmd.cmd[2] = sector.log_cyl;
    cmd.cmd[3] = sector.log_head;
    cmd.cmd[4] = sector.log_sector;
    cmd.cmd[5] = track.sector_size_code;
    // End of track sector number.
    cmd.cmd[6] = 0xFF;
    // Intersector gap. There's a complex table of these for various formats in
    // the M1543C datasheet; the fdutils manual says it doesn't make any
    // difference for read. FIXME: hmm.
    cmd.cmd[7] = 0x1B;
    // Bytes in sector -- but only if size code is 0, else it should be 0xFF.
    if (track.sector_size_code == 0) {
        cmd.cmd[8] = sector_bytes(track.sector_size_code);
    } else {
        cmd.cmd[8] = 0xFF;
    }
    cmd.cmd_count = 9;
    cmd.flags = FD_RAW_READ | FD_RAW_INTR | FD_RAW_NEED_SEEK;
    cmd.track = track.phys_cyl * args.cyl_scale;
    cmd.data = buf;
    cmd.length = buf_size;
    apply_data_mode(track.data_mode, cmd);

    if (ioctl(dev_fd, FDRAWCMD, &cmd) < 0) {
        die_errno("FD_READID failed");
    }
    if (cmd.reply_count < 7) {
        die("FD_READID returned short reply");
    }

    // If we're reading multiple sectors but hit a deleted sector, then the
    // read will have stopped there -- fail.
    if (buf_size > sector_bytes(track.sector_size_code)
        && (cmd.reply[2] & 0x40) != 0) {
        return false;
    }

    // If ST0 interrupt code is 00, success.
    return ((cmd.reply[0] >> 6) & 3) == 0;
}

// Read a sector ID and append it to the sectors in the track.
static bool track_readid(track_t& track) {
    struct floppy_raw_cmd cmd;

    if (track.num_sectors == MAX_SECS-1) {
        die("track_readid read too many sectors");
    }

    do {
        if (!fd_readid(track, cmd)) {
            return false;
        }
    } while (args.ignore_sector == cmd.reply[5]);

    sector_t& sector = track.sectors[track.num_sectors];
    assert_free_sector(sector);
    sector.log_cyl = cmd.reply[3];
    sector.log_head = cmd.reply[4];
    sector.log_sector = cmd.reply[5];
    assert(cmd.reply[6] != UCHAR_MAX);

    if (track.sector_size_code == UCHAR_MAX) {
        //printf("Got new sector_size_code %d\n", cmd.reply[6]);
        track.sector_size_code = cmd.reply[6];
    } else if (track.sector_size_code != cmd.reply[6]) {
        // Apparently this can legitimately occur on original floppies that have incorporated bizarre copy-protection. The game "QIX" does this, for example.
        // There appears to be no way to convey this sort of track/sector mismatch information to the virtual machine (ie, VMWare) that you would play the disk image on.
        // We could store the track size in the sector_t, but sadly nothing would be able to make use of that information it seems.
        fprintf(stderr, "mixed sector formats within track %d != %d\n", track.sector_size_code, cmd.reply[6]);
        return false; // This sector will be flagged SECTOR_MISSING, and dumpfloppy will ultimately return non-zero on the command line.
    }

    track.num_sectors++;
    return true;
}

// Identify the data mode and sector layout of a track.
static bool probe_track(track_t& track) {
    assert(track.status == TRACK_UNKNOWN);

    printf("Probe %2d.%d:", track.phys_cyl, track.phys_head);
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
    track.data_mode = &DATA_MODES[1];
    track_readid(track);

    // Try all the possible data modes until we can read a sector ID.
    track.num_sectors = 0;
    track.sector_size_code = -1;
    for (int i = 0; ; i++) {
        if (DATA_MODES[i].name == NULL) {
            printf(" unknown data mode\n");
            return false;
        }

        track.data_mode = &DATA_MODES[i];
        if (track_readid(track)) {
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
        if (!track_readid(track)) {
            printf(" readid failed\n");
            return false;
        }
        sector_t& sector = track.sectors[track.num_sectors-1];

        seen_secs[sector.log_sector]++;

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
    while (!same_sector_addr(track.sectors[0], track.sectors[end_pos])) {
        end_pos++;
        if (end_pos == track.num_sectors) {
            printf(" couldn't find repeat of first sector\n");
            return false;
        }
    }

    // Check that the sequence repeated itself consistently after that.
    // If we're missing sectors, this has a reasonable chance of spotting it.
    // FIXME: There should be an option to override this for *really* dodgy
    // disks, and just assume the sectors are in order.
    for (int pos = end_pos; pos < track.num_sectors; pos++) {
        if (!same_sector_addr(track.sectors[pos % end_pos], track.sectors[pos])) {
            printf("  sector sequence did not repeat consistently\n");
            return false;
        }
    }

    // Cut the sequence to length.
    track.num_sectors = end_pos;

    // Show what we found.
    printf(" %s %dx%ld:",
           track.data_mode->name,
           track.num_sectors, sector_bytes(track.sector_size_code));
    for (int i = 0; i < track.num_sectors; i++) {
        printf(" %d", track.sectors[i].log_sector);
    }
    printf("\n");

    track.status = TRACK_PROBED;
    return true;
}

// Try to read any sectors in a track that haven't already been read.
// Returns true if everything has been read.
static bool read_track(track_t& track, const bool retrying) {
    struct floppy_raw_cmd cmd;

    if (track.status == TRACK_UNKNOWN) {
        if (!probe_track(track)) {
            return false;
        }
    }

    if (retrying) {
        bool have_everything = true;
        for (int i = 0; i < track.num_sectors; i++) {
            if (track.sectors[i].status != SECTOR_GOOD) {
                have_everything = false;
                break;
            }
        }
        if (have_everything) {
            return true; // Nothing else to do for this track. Avoid even printing the "Read..." line.
        }
    }
    printf("Read  %2d.%d:", track.phys_cyl, track.phys_head);
    fflush(stdout);

    const sector_t* lowest_sector;
    const bool contiguous = retrying || track_scan_sectors(track, &lowest_sector);

    const int sector_size = sector_bytes(track.sector_size_code);
    const int track_size = sector_size * track.num_sectors;
    unsigned char track_data[track_size];
    bool read_whole_track = false;

    // FIXME: Read with the flag set that means deleted sectors won't be
    // ignored (since we can't tell from readid whether the sectors were
    // regular or deleted).
    // FIXME: Describe read errors, with the phys/log context.

    if (contiguous && !retrying) {
        // Try reading the whole track to start with.
        // If this works, it's a lot faster than reading sector-by-sector.
        // The resulting data will be ordered by *logical* ID.
        if (fd_read(track, *lowest_sector, track_data, track_size, cmd)) {
            read_whole_track = true;
        }
    }

    // Get sectors in physical order.
    bool all_ok = true;
    for (int i = 0; i < track.num_sectors; i++) {
        sector_t& sector = track.sectors[i];

        if (sector.status == SECTOR_GOOD) {
            // Already got this one.
            printf("    ");
            continue;
        }

        printf("%3d", sector.log_sector);
        fflush(stdout);

        if (read_whole_track) {
            // We read this sector as part of the whole track. Success!
            const int rel_sec = sector.log_sector - lowest_sector->log_sector;

            sector.status = SECTOR_GOOD;

            // If this was previously part of a bad read, but on a subsequent track attempt we
            // read the whole track, then we start over with an empty sector and our one good read.
            sector.datas.clear();

            sector.datas[data_t(track_data + (sector_size * rel_sec), sector_size)] = 1; // 1 meaning we've seen this data 1 time now.
            sector.deleted = false;

            printf("*");
            continue;
        }

        uint8_t data_buf[sector_size];
        bool have_data = true;
        bool bad_data_new_read = true;

        // Read a single sector.
        if (!fd_read(track, sector, data_buf, sector_size, cmd)) {
            all_ok = false;
            if ((cmd.reply[2] & ST2_CRC) != 0) {
                // ST2_CRC (0x20) "CRC error in data field". Better than nothing, but we'll want to try again.
                sector.status = SECTOR_BAD;
                assert(!(cmd.reply[2] & (ST2_WC|ST2_SEH|ST2_SNS|ST2_BC|ST2_MAM)));
                assert(cmd.reply[1] == ST1_CRC);

                data_t data_str = data_t(data_buf, sector_size);
                data_map_t::iterator iter = sector.datas.find(data_str);
                if (iter == sector.datas.end()) {
                    sector.datas.insert(data_map_t::value_type(data_str, 1)); // 1 meaning we've seen this data 1 time now.
                } else {
                    if (iter->second != UINT32_MAX) {
                        iter->second++;
                    }
                    bad_data_new_read = false; // Got prior sector byte read again.
                }
            } else {
                have_data = false; // No data.
            }
        } else {
            // Success!
            sector.status = SECTOR_GOOD;
            // Normally the '1' means we've seen this data 1 time now. But if we've ever seen anything else, this successful
            // read should trump them all with the highest possible "seen count."
            sector.datas.insert(data_map_t::value_type(data_t(data_buf, sector_size), sector.datas.empty() ? 1 : UINT32_MAX));
        }

        if (have_data) {
            // ST2_CM (0x40) is Control Mark -- a deleted sector was read.
            sector.deleted = (cmd.reply[2] & ST2_CM) != 0;

            if (sector.status == SECTOR_BAD) {
                assert(!all_ok);
                printf(bad_data_new_read ? "?" : "@");
            } else if (sector.deleted) {
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

static void probe_disk(disk_t& disk) {
    // Probe both sides of cylinder 2 to figure out the disk geometry.
    // (Cylinder 2 because we need a physical cylinder greater than 0 to figure
    // out the logical-to-physical mapping, and because cylinder 0 may
    // reasonably be unformatted on disks where it's a bootblock.)

    const int cyl = 2;
    for (int head = 0; head < disk.num_phys_heads; head++) {
        probe_track(disk.tracks[cyl][head]);
    }

    track_t& side0 = disk.tracks[cyl][0];
    sector_t& sec0 = side0.sectors[0];
    track_t& side1 = disk.tracks[cyl][1];
    sector_t& sec1 = side1.sectors[0];

    if (side0.status == TRACK_UNKNOWN && side1.status == TRACK_UNKNOWN) {
        die("Cylinder 2 unreadable on either side");
    } else if (side1.status == TRACK_UNKNOWN) {
        printf("Single-sided disk\n");
        disk.num_phys_heads = 1;
    } else if (sec0.log_head == 0 && sec1.log_head == 0) {
        printf("Double-sided disk with separate sides\n");
    } else {
        printf("Double-sided disk\n");
    }

    if (sec0.log_cyl * 2 == side0.phys_cyl) {
        printf("Doublestepping required (40T disk in 80T drive)\n");
        args.cyl_scale = 2;
    } else if (sec0.log_cyl == side0.phys_cyl * 2) {
        die("Can't read this disk (80T disk in 40T drive)");
    } else if (sec0.log_cyl != side0.phys_cyl) {
        printf("Mismatch between physical and logical cylinders\n");
    }
}

static int process_floppy(void) {
    bool retrying = false;
    disk_t disk;
    assert(args.image_filename != NULL);

    mode_t image_file_mode = S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH;
    // If the image exists already, load it, and continue from there.
    if (access(args.image_filename, F_OK) != -1) {
        if (!args.retry) {
            die("File \"%s\" already exists. Specify \"-r\" to retry reads.", args.image_filename);
        }
        struct stat mystat;
        if (stat(args.image_filename, &mystat) != 0) {
            die_errno("stat failed on %s", args.image_filename);
        }
        image_file_mode = mystat.st_mode;
        FILE *f = fopen(args.image_filename, "rb");
        if (f == NULL) {
            die_errno("cannot open %s for reading", args.image_filename);
        }
        read_imd(f, disk);
        fclose(f);
        retrying = true;
        fprintf(stdout, "Loaded prior image. Retrying failed reads...\n");
    } else {
        init_disk(disk);
        make_disk_comment(PACKAGE_NAME, PACKAGE_VERSION, disk);
    }

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

            disk.comment.append(buf, count);
        }
    }

    std::string filename_in_progress = str_sprintf("%s.in_progress", args.image_filename);
    FILE* image = fdopen(open(filename_in_progress.c_str(), O_EXCL|O_CREAT|O_WRONLY, image_file_mode), "wb");
    if (image == NULL) {
        die_errno("cannot open %s for writing", filename_in_progress.c_str());
    }

    // Open the /dev/fd* file.
    {
        std::string dev_filename = str_sprintf("/dev/fd%d", args.drive);
        printf("opening %s\n", dev_filename.c_str());

        dev_fd = open(dev_filename.c_str(), O_ACCMODE | O_NONBLOCK);
        if (dev_fd == -1) {
            die_errno("cannot open %s", dev_filename.c_str());
        }
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
        fd_recalibrate(cmd);
    }

    if (retrying) {
        printf("Using previously probed disk cyls/heads from %s\n", args.image_filename);
    } else {
        if (args.tracks == -1) {
            disk.num_phys_cyls = drive_params.tracks;
        } else {
            disk.num_phys_cyls = args.tracks;
        }
        disk.num_phys_heads = 2;

        probe_disk(disk);
        disk.num_phys_cyls /= args.cyl_scale;
    }

    write_imd_header(disk, image);

    // FIXME: if retrying, ensure we've moved the head across the disk
    // FIXME: if retrying, turn the motor off and on (delay? close?) ioctl(fd,FDTWADDLE)?
    for (int cyl = 0; cyl < disk.num_phys_cyls; cyl++) {
        for (int head = 0; head < disk.num_phys_heads; head++) {
            track_t& track = disk.tracks[cyl][head];

            if (args.always_probe || retrying) {
                // Don't assume a layout.
            } else if (cyl > 0) {
                // Try the layout of the previous cyl on the same head.
                copy_track_layout(disk.tracks[cyl - 1][head], track);
            }

            for (int try_num = 0; try_num < args.max_tries; try_num++) {
                if (read_track(track, retrying)) {
                    // Success!
                    break;
                }

                if (track.status == TRACK_GUESSED) {
                    // Maybe we guessed wrong. Probe and try again.
                    assert(try_num == 0);
                    try_num = -1; // Ensure at least one "retry" (sort of a first real try in a way).
                    init_track(cyl, head, track);
                }
            }

            write_imd_track(track, image);
            fflush(image);
        }
    }

    fclose(image);
    close(dev_fd);

    long secstat[SECTOR_ENUM_HIGHEST+1] = {0};
    {
        for (int phys_cyl = 0; phys_cyl < disk.num_phys_cyls; phys_cyl++) {
            for (int phys_head = 0; phys_head < disk.num_phys_heads; phys_head++) {
                for (int phys_sec = 0; phys_sec < disk.tracks[phys_cyl][phys_head].num_sectors; phys_sec++) {
                    secstat[disk.tracks[phys_cyl][phys_head].sectors[phys_sec].status]++;
                }
            }
        }
        printf("\nSector statuses:\nGood:    %ld\nBad:     %ld\nMissing: %ld\n", secstat[SECTOR_GOOD], secstat[SECTOR_BAD], secstat[SECTOR_MISSING]);
    }

    if (rename(filename_in_progress.c_str(), args.image_filename) != 0) {
        die_errno("rename \"%s\" to \"%s\" failed", filename_in_progress.c_str(), args.image_filename);
    }

    return (secstat[SECTOR_BAD] || secstat[SECTOR_MISSING]) ? 1 : 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: dumpfloppy [OPTION]... IMAGE-FILE\n"
        "  -a         probe each track before reading\n"
        "  -d NUM     drive number to read from (default 0)\n"
        "  -t TRACKS  drive has TRACKS tracks (default autodetect)\n"
        "  -C         read comment from stdin\n"
        "  -S SEC     ignore sectors with logical ID SEC\n"
        "  -m NUM     max reads of a failed sector (default 10)\n"
        "  -r         perform retry on existing IMD file.\n"
    );

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
    args.max_tries = 10;

    while (true) {
        int opt = getopt(argc, argv, "ad:t:CS:m:r");
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
        case 'm':
            args.max_tries = atoi(optarg);
            break;
        case 'r':
            args.retry = true;
            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind + 1 == argc) {
        args.image_filename = argv[optind];
    } else {
        usage();
        return 1;
    }

    return process_floppy();
}
