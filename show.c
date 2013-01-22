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

#include <stdio.h>

void show_mode(const data_mode_t *mode, FILE *out) {
    if (mode == NULL) {
        fprintf(out, "-");
    } else {
        fprintf(out, "%s", mode->name);
    }
}

void show_sector(const sector_t *sector, FILE *out) {
    char label = ' ';
    switch (sector->status) {
    case SECTOR_MISSING:
        fprintf(out, "  . ");
        return;
    case SECTOR_BAD:
        label = '?';
        break;
    case SECTOR_GOOD:
        if (sector->deleted) {
            label = 'x';
        } else {
            label = '+';
        }
        break;
    }
    fprintf(out, "%3d%c", sector->log_sector, label);
}

void show_track(const track_t *track, FILE *out) {
    show_mode(track->data_mode, out);
    fprintf(out, " %dx%d",
            track->num_sectors,
            sector_bytes(track->sector_size_code));
    for (int phys_sec = 0; phys_sec < track->num_sectors; phys_sec++) {
        show_sector(&track->sectors[phys_sec], out);
    }
}

void show_disk(const disk_t *disk, FILE *out) {
    if (disk->comment) {
        fwrite(disk->comment, 1, disk->comment_len, out);
    }
    fprintf(out, "\n");
    for (int phys_cyl = 0; phys_cyl < disk->num_phys_cyls; phys_cyl++) {
        for (int phys_head = 0; phys_head < disk->num_phys_heads; phys_head++) {
            fprintf(out, "%2d.%d:", phys_cyl, phys_head);
            show_track(&disk->tracks[phys_cyl][phys_head], out);
            fprintf(out, "\n");
        }
    }
}
