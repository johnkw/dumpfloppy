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
#include <unistd.h>

static struct args {
    const char *image_filename;
    const char *flat_filename;
    bool verbose;
} args;

static void write_flat(const disk_t *disk, FILE *flat) {
    die("unimplemented");
    // ... find range of logical cyls
    // ... find range of logical heads
    // ... find range of logical sectors
}

static void usage(void) {
    fprintf(stderr, "usage: imdcat [OPTION]... IMAGE-FILE\n");
    fprintf(stderr, "  -o FILE    write sector data to flat file\n");
    fprintf(stderr, "  -v         describe loaded image (implied if no -o)\n");
    // FIXME: multiple input files, to be merged
    // FIXME: -h          sort flat file by LH, LC, LS (default: LC, LH, LS)
}

int main(int argc, char **argv) {
    args.flat_filename = NULL;
    args.verbose = false;

    while (true) {
        int opt = getopt(argc, argv, "o:v");
        if (opt == -1) break;

        switch (opt) {
        case 'o':
            args.flat_filename = optarg;
            break;
        case 'v':
            args.verbose = true;
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
    if (args.flat_filename == NULL) {
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

    if (args.verbose) {
        show_disk(&disk, stdout);
    }

    if (args.flat_filename != NULL) {
        f = fopen(args.flat_filename, "wb");
        write_flat(&disk, f);
        fclose(f);
    }

    free_disk(&disk);

    return 0;
}
