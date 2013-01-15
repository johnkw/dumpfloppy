/*
    imd.c: write ImageDisk .IMD files

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

#include <stdio.h>
#include <time.h>

void write_imd_header(FILE *image) {
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
void write_imd_track(const track_t *track, FILE *image) {
    uint8_t flags = 0;

    uint8_t sec_map[track->num_sectors];
    uint8_t cyl_map[track->num_sectors];
    uint8_t head_map[track->num_sectors];
    for (int i = 0; i < track->num_sectors; i++) {
        const sector_t *sector = &(track->sectors[i]);

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
