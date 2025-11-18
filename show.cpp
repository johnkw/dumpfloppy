/*
    show.c: print summaries of disk contents

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
#include "util.h"

#include <stdio.h>

void show_mode(const data_mode_t *mode, FILE *out) {
    if (mode == NULL) {
        fprintf(out, "-");
    } else {
        fprintf(out, "%s", mode->name);
    }
}

void show_sector(const sector_t& sector, FILE *out) {
    char label = ' ';
    switch (sector.status) {
    case SECTOR_MISSING:
        fprintf(out, "  . ");
        return;
    case SECTOR_BAD:
        label = '?';
        break;
    case SECTOR_GOOD:
        if (sector.deleted) {
            label = 'x';
        } else {
            label = '+';
        }
        break;
    }
    fprintf(out, "%3d%c", sector.log_sector, label);
}

void show_track(const track_t& track, FILE *out) {
    show_mode(track.data_mode, out);
    fprintf(out, " %dx%ld",
            track.num_sectors,
            sector_bytes(track.sector_size_code));
    for (int phys_sec = 0; phys_sec < track.num_sectors; phys_sec++) {
        show_sector(track.sectors[phys_sec], out);
    }
}

void show_track_data(const track_t& track, FILE* const out) {
    for (int phys_sec = 0; phys_sec < track.num_sectors; phys_sec++) {
        const sector_t& sector = track.sectors[phys_sec];
        if (sector.status == SECTOR_MISSING) continue;

        const int data_len = sector_bytes(track.sector_size_code);
        fprintf(out, "Physical C %d H %d S %d, Logical C %d H %d S %d",
                track.phys_cyl, track.phys_head, phys_sec,
                sector.log_cyl, sector.log_head, sector.log_sector);
        if (sector.datas.size() > 1) {
            fprintf(out, ": (unique read datas: %ld)", sector.datas.size());
        }
        fprintf(out, ":\n");

        size_t data_id = 0;
        for (data_map_t::const_iterator iter = sector.datas.begin(); iter != sector.datas.end(); iter++) {
            if ((sector.datas.size() > 1) || (iter->second > 1)) {
                fprintf(out, "IMD data id: %zd. Repeat count: %d.\n", data_id++, iter->second);
            }

            // The format here is based on "hexdump -C".
            // (Although it's not smart enough to fold identical data.)
            const int line_len = 16;
            for (int i = 0; i < data_len; i += line_len) {
                fprintf(out, "%2d%2d%2d %04x ", sector.log_cyl, sector.log_head, sector.log_sector, i);

                for (int j = 0; j < line_len; j++) {
                    const int pos = i + j;
                    if (pos < data_len) {
                        fprintf(out, " %02x", iter->first[pos]);
                    } else {
                        fprintf(out, "   ");
                    }
                }

                fprintf(out, "  |");
                for (int j = 0; j < line_len; j++) {
                    const int pos = i + j;
                    if (pos < data_len) {
                        const uint8_t c = iter->first[pos];
                        if (c >= 32 && c < 127) {
                            fprintf(out, "%c", c);
                        } else {
                            fprintf(out, ".");
                        }
                    } else {
                        fprintf(out, " ");
                    }
                }

                fprintf(out, "|\n");
            }
        }

        fprintf(out, "\n");
    }
}

void show_comment(const disk_t& disk, FILE *out) {
    if (!disk.comment.empty()) {
        fwrite(disk.comment.c_str(), 1, disk.comment.length(), out);
    }
}

void show_disk(const disk_t& disk, bool with_data, FILE *out) {
    show_comment(disk, out);
    fprintf(out, "\n");
    for (int phys_cyl = 0; phys_cyl < disk.num_phys_cyls; phys_cyl++) {
        for (int phys_head = 0; phys_head < disk.num_phys_heads; phys_head++) {
            fprintf(out, "%2d.%d:", phys_cyl, phys_head);
            show_track(disk.tracks[phys_cyl][phys_head], out);
            fprintf(out, "\n");

            if (with_data) {
                fprintf(out, "\n");
                show_track_data(disk.tracks[phys_cyl][phys_head], out);
            }
        }
    }
}
