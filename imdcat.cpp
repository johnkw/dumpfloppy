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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <map>

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
    for(int var = (range).start; var < (range).end; var++)

static void update_range(int value, range& r) {
    if (value < r.start) {
        r.start = value;
    }
    if (value >= r.end) {
        r.end = value + 1;
    }
}

static void apply_range_option(const range& in, range& out) {
    if (in.start != -1) {
        out.start = in.start;
    }
    if (in.end != -1) {
        out.end = in.end;
    }
}

class SHC_t {
public:
    int cyl;
    int head;
    int sec;
    SHC_t(int c, int h, int s) : cyl(c), head(h), sec(s) {}
    bool operator <(const SHC_t& rhs) const {
        if (this->cyl  < rhs.cyl)  return true;
        if (this->cyl  > rhs.cyl)  return false;
        if (this->head < rhs.head) return true;
        if (this->head > rhs.head) return false;
        if (this->sec  < rhs.sec)  return true;
        return false;
    }
};

static void write_flat(const disk_t& disk, FILE *flat) {
    typedef std::map<SHC_t, data_t> disk_image_t;
    disk_image_t disk_image;

    // The range of C/H/S to use in the output image (based on what we load).
    range out_cyls = {MAX_CYLS, 0};
    range out_heads = {MAX_HEADS, 0};
    range out_sectors = {MAX_SECS, 0};
    int size_code = -1;
    bool did_bell = false;

    // Find the range of cylinders, heads and sectors to write.
    // For each real sector, add a lump.
    for_range (phys_cyl, args.in_cyls) {
        for_range (phys_head, args.in_heads) {
            const track_t& track = disk.tracks[phys_cyl][phys_head];

            for (int phys_sec = 0; phys_sec < track.num_sectors; phys_sec++) {
                const sector_t& sector = track.sectors[phys_sec];

                // Use physical cyl and head, but logical sector.
                // FIXME: Option to choose physical/logical values
                int cyl = phys_cyl;
                int head = phys_head;
                int sec = sector.log_sector;

                if (sec < args.in_sectors.start || sec >= args.in_sectors.end) {
                    continue;
                }

                update_range(cyl, out_cyls);
                update_range(head, out_heads);
                update_range(sec, out_sectors);

                // FIXME: Option to include/exclude bad/deleted sectors
                if (sector.status == SECTOR_MISSING) continue;

                SHC_t SHC(cyl, head, sec);
                if (disk_image.find(SHC) != disk_image.end() && !args.permissive) {
                    die("Two sectors found for cylinder %d head %d sector %d", cyl, head, sec);
                }

                size_t data_id = 0;
                if (sector.datas.size() != 1) {
                    // Find the highest read count for the default option.
                    int i = 0;
                    data_map_t::const_iterator default_iter = sector.datas.begin();
                    for (data_map_t::const_iterator iter = sector.datas.begin(); iter != sector.datas.end(); iter++) {
                        if (iter->second > default_iter->second) {
                            default_iter = iter;
                            data_id = i;
                        }
                        i++;
                    }
                    if (!did_bell) {
                        fprintf(stderr, "\x07");
                        did_bell = true;
                    }
                    fprintf(stderr, "Enter the 'IMD data id' to use for Logical C %d H %d S %d: [default: %ld, count: %d]: ",
                        sector.log_cyl, sector.log_head, sector.log_sector,
                        data_id, default_iter->second
                    );
                    char buf[100];
                    for (;;) {
                        if (fgets(buf, sizeof(buf), stdin) == NULL) {
                            die_errno("Error reading stdin");
                        }
                        //fprintf(stderr, "Read %s\n", buf);
                        if (strcmp(buf, "\n") == 0) {
                            fprintf(stderr, "Using default ID of %ld\n", data_id);
                            break;
                        } else if (sscanf(buf, "%zd", &data_id) == 1) {
                            if (data_id < sector.datas.size()) {
                                break;
                            } else {
                                fprintf(stderr, "Parsed invalid 'IMD data id': %zd. Must be less than %zd.\n: ", data_id, sector.datas.size());
                            }
                        } else {
                            fprintf(stderr, "Error parsing 'IMD data id': (%d:%s)\n: ", errno, strerror(errno));
                        }
                    }
                }
                data_map_t::const_iterator iter = sector.datas.begin();
                while (data_id--) { iter++; }

                disk_image[SHC] = iter->first;
                assert(disk_image[SHC].length() == sector_bytes(track.sector_size_code));

                // Sanity check that all the sectors are the same size. TODO: Is it really a problem if some are different sizes?
                if (size_code == -1) {
                    size_code = track.sector_size_code;
                } else if (track.sector_size_code != size_code) {
                    printf("Tracks have inconsistent sector sizes: %d != %d for %d,%d,%d (total sectors per track: %d)\n",
                        track.sector_size_code, size_code, cyl, head, sec, track.num_sectors);
                }
            }
        }
    }
    // Override output ranges as specified in options.
    apply_range_option(args.out_cyls, out_cyls);
    apply_range_option(args.out_heads, out_heads);
    apply_range_option(args.out_sectors, out_sectors);

    data_t dummy_data(sector_bytes(size_code), 0xFF); // Data to write where we don't have a real sector.

    // Go through the disk_image, and write all sectors out.
    for_range (cyl, out_cyls) {
        for_range (head, out_heads) {
            for_range (sec, out_sectors) {
                // For each sector that *should* exist, add a dummy lump.
                disk_image_t::iterator sec_data_it = disk_image.find(SHC_t(cyl, head, sec));
                fwrite(
                    sec_data_it == disk_image.end() ? dummy_data.data() : sec_data_it->second.data(),
                    1, sector_bytes(size_code), flat
                );
            }
        }
    }
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
static void parse_range(const char *in, range& out) {
    int value;
    char *next;
    const char *in_end = in + strlen(in);

    value = strtoul(in, &next, 10);
    if (next != in) {
        // Found a first value.
        out.start = value;
    }

    if (*next != ':') {
        // No - found.
        if (next != in && next == in_end) {
            // Just a single number.
            out.end = value + 1;
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
    out.end = value + 1;
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
            parse_range(optarg, args.in_cyls);
            break;
        case 'h':
            parse_range(optarg, args.in_heads);
            break;
        case 's':
            parse_range(optarg, args.in_sectors);
            break;
        case 'C':
            parse_range(optarg, args.out_cyls);
            break;
        case 'H':
            parse_range(optarg, args.out_heads);
            break;
        case 'S':
            parse_range(optarg, args.out_sectors);
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

    FILE *f = fopen(args.image_filename, "rb");
    if (f == NULL) {
        die_errno("cannot open %s", args.image_filename);
    }
    disk_t disk;
    read_imd(f, disk);
    fclose(f);

    if (args.show_comment && !args.verbose) {
        show_comment(disk, stdout);
    }

    if (args.verbose) {
        show_disk(disk, args.show_data, stdout);
    }

    if (args.flat_filename != NULL) {
        f = fopen(args.flat_filename, "wb");
        write_flat(disk, f);
        fclose(f);
    }

    return 0;
}
