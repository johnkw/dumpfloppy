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

typedef struct {
    int start;
    int end;
} range;

static struct args {
    const char *image_filename;
    bool show_comment;
    const char *flat_filename;
    bool verbose;
    bool show_data;
    bool permissive;
    range in_cyls, in_heads, in_sectors;
    range out_cyls, out_heads, out_sectors;
} args;

#define for_range(var, range) \
    for(int var = (range)->start; var < (range)->end; var++)

static void update_range(int value, range *r) {
    if (value < r->start) {
        r->start = value;
    }
    if (value >= r->end) {
        r->end = value + 1;
    }
}

static void apply_range_option(const range *in, range *out) {
    if (in->start != -1) {
        out->start = in->start;
    }
    if (in->end != -1) {
        out->end = in->end;
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
        *list = (lump_t*)realloc(*list, (*size) * (sizeof **list));
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

    // The range of C/H/S to use in the output image (based on what we load).
    range out_cyls = {MAX_CYLS, 0};
    range out_heads = {MAX_HEADS, 0};
    range out_sectors = {MAX_SECS, 0};
    int size_code = -1;

    // Find the range of cylinders, heads and sectors to write.
    // For each real sector, add a lump.
    for_range (phys_cyl, &args.in_cyls) {
        for_range (phys_head, &args.in_heads) {
            const track_t *track = &disk->tracks[phys_cyl][phys_head];

            for (int phys_sec = 0; phys_sec < track->num_sectors; phys_sec++) {
                const sector_t *sector = &track->sectors[phys_sec];

                // Use physical cyl and head, but logical sector.
                // FIXME: Option to choose physical/logical values
                int cyl = phys_cyl;
                int head = phys_head;
                int sec = sector->log_sector;

                if (sec < args.in_sectors.start || sec >= args.in_sectors.end) {
                    continue;
                }

                update_range(cyl, &out_cyls);
                update_range(head, &out_heads);
                update_range(sec, &out_sectors);

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

    // Override output ranges as specified in options.
    apply_range_option(&args.out_cyls, &out_cyls);
    apply_range_option(&args.out_heads, &out_heads);
    apply_range_option(&args.out_sectors, &out_sectors);

    // For each sector that *should* exist, add a dummy lump.
    for_range (cyl, &out_cyls) {
        for_range (head, &out_heads) {
            for_range (sec, &out_sectors) {
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
            if (lump->data != NULL && !args.permissive) {
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
    fprintf(stderr, "\n");
    fprintf(stderr, "  -n         write comment to stdout\n");
    fprintf(stderr, "  -o FILE    write sector data to flat file\n");
    fprintf(stderr, "  -v         describe loaded image (default action)\n");
    fprintf(stderr, "  -x         show hexdump of data in image\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options for use with -o:\n");
    fprintf(stderr, "  -p         ignore duplicated input sectors\n");
    fprintf(stderr, "  -c RANGE   limit input cylinders (default all)\n");
    fprintf(stderr, "  -h RANGE   limit input heads (default all)\n");
    fprintf(stderr, "  -s RANGE   limit input sectors (default all)\n");
    fprintf(stderr, "  -C RANGE   output cylinders (default autodetect)\n");
    fprintf(stderr, "  -H RANGE   output heads (default autodetect)\n");
    fprintf(stderr, "  -S RANGE   output sectors (default autodetect)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Ranges are in the form FIRST:LAST, FIRST:, :LAST or "
                    "ONLY, inclusive.\n");
    // FIXME: multiple input files, to be merged
    // FIXME: sort flat file by LH, LC, LS (default: LC, LH, LS)
    // FIXME: make the input limit options work with -x, etc.
    exit(1);
}

// Parse a range argument in the form "10:20" -- which would be parsed as
// (10, 21).
static void parse_range(const char *in, range *out) {
    int value;
    char *next;
    const char *in_end = in + strlen(in);

    value = strtoul(in, &next, 10);
    if (next != in) {
        // Found a first value.
        out->start = value;
    }

    if (*next != ':') {
        // No - found.
        if (next != in && next == in_end) {
            // Just a single number.
            out->end = value + 1;
            return;
        } else {
            usage();
        }
    }
    ++next;

    if (next == in_end) {
        // No second value found.
        return;
    }
    value = strtoul(next, &next, 10);
    if (next != in_end) {
        usage();
    }
    out->end = value + 1;
}

int main(int argc, char **argv) {
    args.show_comment = false;
    args.flat_filename = NULL;
    args.verbose = false;
    args.show_data = false;
    args.permissive = false;
    args.in_cyls.start = 0;
    args.in_cyls.end = MAX_CYLS;
    args.in_heads.start = 0;
    args.in_heads.end = MAX_HEADS;
    args.in_sectors.start = 0;
    args.in_sectors.end = MAX_SECS; // XXX logical sectors?
    args.out_cyls.start = args.out_cyls.end = -1;
    args.out_heads.start = args.out_heads.end = -1;
    args.out_sectors.start = args.out_sectors.end = -1;

    while (true) {
        int opt = getopt(argc, argv, "no:vxpc:h:s:C:H:S:");
        if (opt == -1) break;

        switch (opt) {
        case 'n':
            args.show_comment = true;
            break;
        case 'o':
            args.flat_filename = optarg;
            break;
        case 'v':
            args.verbose = true;
            break;
        case 'x':
            args.show_data = true;
            break;

        case 'p':
            args.permissive = true;
            break;
        case 'c':
            parse_range(optarg, &args.in_cyls);
            break;
        case 'h':
            parse_range(optarg, &args.in_heads);
            break;
        case 's':
            parse_range(optarg, &args.in_sectors);
            break;
        case 'C':
            parse_range(optarg, &args.out_cyls);
            break;
        case 'H':
            parse_range(optarg, &args.out_heads);
            break;
        case 'S':
            parse_range(optarg, &args.out_sectors);
            break;

        default:
            usage();
        }
    }

    if (optind + 1 != argc) {
        usage();
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
