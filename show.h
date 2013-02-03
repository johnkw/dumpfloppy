/*
    show.h: print summaries of disk contents

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

#ifndef SHOW_H
#define SHOW_H

#include <stdbool.h>
#include <stdio.h>

void show_mode(const data_mode_t *mode, FILE *out);
void show_sector(const sector_t *sector, FILE *out);
void show_track(const track_t *track, FILE *out);
void show_track_data(const track_t *track, FILE *out);
void show_comment(const disk_t *disk, FILE *out);
void show_disk(const disk_t *disk, bool with_data, FILE *out);

#endif
