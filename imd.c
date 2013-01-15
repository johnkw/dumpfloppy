/*
    imd.c: read and write ImageDisk .IMD files

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

#define _POSIX_C_SOURCE 200809L

#include "disk.h"
#include "imd.h"
#include "util.h"

#include <stdio.h>
#include <sys/types.h>

#define IMD_END_OF_COMMENT 0x1A

void read_imd(FILE *image, disk_t *disk) {
    free_disk(disk);

    // Read the comment.
    disk->comment = NULL;
    size_t dummy = 0;
    ssize_t count = getdelim(&disk->comment, &dummy, IMD_END_OF_COMMENT, image);
    disk->comment_len = count - 1;
    if (count < 0 || disk->comment[disk->comment_len] != IMD_END_OF_COMMENT) {
        die("Couldn't find IMD comment delimiter");
    }
    disk->comment[disk->comment_len] = '\0';

    // FIXME: rest of file
}

void write_imd_header(const disk_t *disk, FILE *image) {
    if (disk->comment != NULL) {
        fwrite(disk->comment, 1, disk->comment_len, image);
    }
    fputc(IMD_END_OF_COMMENT, image);
}

#define IMD_NEED_CYL_MAP 0x80
#define IMD_NEED_HEAD_MAP 0x40
// These Sector Data Record flags are combined by +, not |.
#define IMD_SDR_DATA 0x01
#define IMD_SDR_IS_COMPRESSED 0x01
#define IMD_SDR_IS_DELETED 0x02
#define IMD_SDR_IS_ERROR 0x04
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

        uint8_t type = 0;
        switch (sector->status) {
        case SECTOR_MISSING:
            break;
        case SECTOR_BAD:
            type = IMD_SDR_DATA + IMD_SDR_IS_ERROR;
            break;
        case SECTOR_GOOD:
            type = IMD_SDR_DATA;
            break;
        }

        // FIXME: compress if all bytes the same
        fputc(type, image);
        if (sector->data != NULL) {
            fwrite(sector->data, 1, sector_size, image);
        }
    }
}
