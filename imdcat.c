/*
    imdcat: process ImageDisk .IMD files

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

#include "disk.h"
#include "imd.h"
#include "show.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct args {
    const char *image_filename;
    int boot_cyls;
    bool show_comment;
    const char *flat_filename;
    int only_head;
    bool verbose;
    bool show_data;
} args;

static void update_range(int value, int *start, int *end) {
    if (value < *start) {
        *start = value;
    }
    if (value >= *end) {
        *end = value + 1;
    }
}

typedef struct {
    int cyl; // either log or phys, depending on options
    int head;
    int sector;
    const uint8_t *data; // NULL if this is a dummy
} lump_t;

static void add_lump(int cyl, int head, int sector, const uint8_t *data,
                     lump_t **list, int *num, int *size) {
    if (*num == *size) {
        // Expand the list.
        *size = (*size * 2) + 1;
        *list = realloc(*list, (*size) * (sizeof **list));
        if (*list == NULL) {
            die("realloc failed");
        }
    }

    lump_t *lump = &(*list)[*num];
    lump->cyl = cyl;
    lump->head = head;
    lump->sector = sector;
    lump->data = data;
    (*num)++;
}

static int cmp_lump_addr(const lump_t *left, const lump_t *right) {
    int d;

    d = left->cyl - right->cyl;
    if (d != 0) return d;

    d = left->head - right->head;
    if (d != 0) return d;

    d = left->sector - right->sector;
    if (d != 0) return d;

    return 0;
}

static int cmp_lump(const void *left_v, const void *right_v) {
    const lump_t *left = (const lump_t *) left_v;
    const lump_t *right = (const lump_t *) right_v;

    int d = cmp_lump_addr(left, right);
    if (d != 0) return d;

    // A lump with data should sort before a lump without data.
    if (left->data != NULL && right->data == NULL) return -1;
    if (left->data == NULL && right->data != NULL) return 1;

    return 0;
}

static void write_flat(const disk_t *disk, FILE *flat) {
    // The list of lumps to write.
    lump_t *lumps = NULL;
    int num_lumps = 0;
    int size_lumps = 0;

    // The range of physical heads to look for.
    int head_from = 0;
    int head_to = disk->num_phys_heads;
    if (args.only_head != -1) {
        if (args.only_head < head_from || args.only_head >= head_to) {
            die("Requested side %d has no data (only %d to %d)",
                args.only_head, head_from, head_to - 1);
        }
        head_from = args.only_head;
        head_to = args.only_head + 1;
    }

    // The range of C/H/S to use in the image (based on what we load).
    int cyl_start = MAX_CYLS, cyl_end = 0;
    int head_start = MAX_HEADS, head_end = 0;
    int sec_start = MAX_SECS, sec_end = 0;
    int size_code = -1;

    // Find the range of cylinders, heads and sectors to write.
    // For each real sector, add a lump.
    for (int phys_cyl = 0; phys_cyl < disk->num_phys_cyls; phys_cyl++) {
        for (int phys_head = head_from; phys_head < head_to; phys_head++) {
            const track_t *track = &disk->tracks[phys_cyl][phys_head];

            for (int phys_sec = 0; phys_sec < track->num_sectors; phys_sec++) {
                const sector_t *sector = &track->sectors[phys_sec];

                // Use physical cyl and head, but logical sector.
                // FIXME: Option to choose physical/logical values
                int cyl = phys_cyl;
                int head = phys_head;
                int sec = sector->log_sector;

                update_range(cyl, &cyl_start, &cyl_end);
                update_range(head, &head_start, &head_end);

                // Some formats have boot cylinders in a different format on
                // the first few tracks -- sometimes it's useful to pretend
                // these don't exist.
                if (cyl < args.boot_cyls) continue;

                // XXX: and when you're doing that, you *don't* want to count
                // the logical sectors on the track you're ignoring. This is
                // really ugly.
                update_range(sec, &sec_start, &sec_end);

                // FIXME: Option to include/exclude bad/deleted sectors
                if (sector->status == SECTOR_MISSING) continue;

                add_lump(cyl, head, sec, sector->data,
                         &lumps, &num_lumps, &size_lumps);

                if (size_code != -1 && track->sector_size_code != size_code) {
                    die("Tracks have inconsistent sector sizes");
                }
                size_code = track->sector_size_code;
            }
        }
    }

    // For each sector that *should* exist, add a dummy lump.
    for (int cyl = cyl_start; cyl < cyl_end; cyl++) {
        for (int head = head_start; head < head_end; head++) {
            for (int sec = sec_start; sec < sec_end; sec++) {
                add_lump(cyl, head, sec, NULL,
                         &lumps, &num_lumps, &size_lumps);
            }
        }
    }

    // Sort the whole lot.
    qsort(lumps, num_lumps, sizeof *lumps, cmp_lump);

    // Data to write where we don't have a real sector.
    const int sector_size = sector_bytes(size_code);
    uint8_t dummy_data[sector_size];
    memset(dummy_data, 0xFF, sector_size);

    // Go through the list, and write the first lump that comes up with each
    // address.
    for (int i = 0; i < num_lumps; i++) {
        const lump_t *lump = &lumps[i];

        if (i > 0 && cmp_lump_addr(&lumps[i - 1], lump) == 0) {
            // Duplicate address -- which is OK if this is a dummy one.
            // But not if, say, we're extracting a disk that uses the same head
            // number on both sides.
            if (lump->data != NULL) {
                die("Two sectors found for cylinder %d head %d sector %d",
                    lump->cyl, lump->head, lump->sector);
            }
            continue;
        }

        const uint8_t *data = lump->data;
        if (data == NULL) {
            data = dummy_data;
        }
        fwrite(data, 1, sector_size, flat);
    }

    free(lumps);
}

static void usage(void) {
    fprintf(stderr, "usage: imdcat [OPTION]... IMAGE-FILE\n");
    fprintf(stderr, "  -B CYLS    pretend the first CYLS cylinders are "
                    "unreadable when writing\n");
    fprintf(stderr, "  -c         write comment to stdout\n");
    fprintf(stderr, "  -o FILE    write sector data to flat file\n");
    fprintf(stderr, "  -s SIDE    only write side SIDE (default both)\n");
    fprintf(stderr, "  -v         describe loaded image (default action)\n");
    fprintf(stderr, "  -x         show hexdump of data in image\n");
    // FIXME: multiple input files, to be merged
    // FIXME: -h          sort flat file by LH, LC, LS (default: LC, LH, LS)
    // FIXME: specify ranges of input/output C/H/S (rather than -B)
}

int main(int argc, char **argv) {
    args.boot_cyls = 0;
    args.show_comment = false;
    args.flat_filename = NULL;
    args.only_head = -1;
    args.verbose = false;
    args.show_data = false;

    while (true) {
        int opt = getopt(argc, argv, "B:co:s:vx");
        if (opt == -1) break;

        switch (opt) {
        case 'B':
            args.boot_cyls = atoi(optarg);
            break;
        case 'c':
            args.show_comment = true;
            break;
        case 'o':
            args.flat_filename = optarg;
            break;
        case 's':
            args.only_head = atoi(optarg);
            break;
        case 'v':
            args.verbose = true;
            break;
        case 'x':
            args.show_data = true;
            break;
        default:
            usage();
            return 1;
        }
    }

    if (optind + 1 != argc) {
        usage();
        return 1;
    }
    args.image_filename = argv[optind];
    if (!args.show_comment && args.flat_filename == NULL) {
        args.verbose = true;
    }
    if (args.show_data) {
        args.verbose = true;
    }

    disk_t disk;
    init_disk(&disk);

    FILE *f = fopen(args.image_filename, "rb");
    if (f == NULL) {
        die_errno("cannot open %s", args.image_filename);
    }
    read_imd(f, &disk);
    fclose(f);

    if (args.show_comment && !args.verbose) {
        show_comment(&disk, stdout);
    }

    if (args.verbose) {
        show_disk(&disk, args.show_data, stdout);
    }

    if (args.flat_filename != NULL) {
        f = fopen(args.flat_filename, "wb");
        write_flat(&disk, f);
        fclose(f);
    }

    free_disk(&disk);

    return 0;
}
