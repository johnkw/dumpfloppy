/*
    disk.c: data structure representing an FM/MFM floppy disk

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

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

size_t sector_bytes(int code) {
    return 128 << code;
}

// Following what the .IMD spec says, the rates here are the data transfer rate
// to the drive -- FM-500k transfers half as much data as MFM-500k owing to the
// less efficient encoding.
const data_mode_t DATA_MODES[] = {
    // 5.25" DD/QD and 3.5" DD drives
    { 5, "MFM-250k", 2, false },
    { 2, "FM-250k", 2, true },

    // DD media in 5.25" HD drives
    { 4, "MFM-300k", 1, false },
    { 1, "FM-300k", 1, true },

    // 3.5" HD, 5.25" HD and 8" drives
    { 3, "MFM-500k", 0, false },
    { 0, "FM-500k", 0, true },

    // 3.5" ED drives
    { 6, "MFM-1000k", 3, false }, // FIXME: not in IMD spec
    // Rate 3 for FM isn't allowed.

    { 0, NULL, 0, false } // NULL represents end of array.
};

void init_sector(sector_t& sector) {
    sector.status = SECTOR_MISSING;
    sector.log_cyl = 0xFF;
    sector.log_head = 0xFF;
    sector.log_sector = 0xFF;
    sector.deleted = false;
    sector.datas.clear();
}

void assert_free_sector(const sector_t& sector) {
    assert(sector.status == SECTOR_MISSING);
    assert(sector.datas.empty());
}

void init_track(int phys_cyl, int phys_head, track_t& track) {
    track.status = TRACK_UNKNOWN,
    track.data_mode = NULL,
    track.phys_cyl = phys_cyl;
    track.phys_head = phys_head;
    track.num_sectors = 0;
    track.sector_size_code = UCHAR_MAX;
    for (int i = 0; i < MAX_SECS; i++) {
        init_sector(track.sectors[i]);
    }
}

void init_disk(disk_t& disk) {
    disk.comment = "";
    disk.num_phys_cyls = 0;
    disk.num_phys_heads = 0;
    for (int cyl = 0; cyl < MAX_CYLS; cyl++) {
        for (int head = 0; head < MAX_HEADS; head++) {
            init_track(cyl, head, disk.tracks[cyl][head]);
        }
    }
}

void make_disk_comment(const char *program, const char *version, disk_t& disk) {
    time_t now = time(NULL);
    const struct tm *local = localtime(&now);

    disk.comment = str_sprintf(
        "%s %s: %02d/%02d/%04d %02d:%02d:%02d\r\n",
        program, version,
        local->tm_mday, local->tm_mon + 1, local->tm_year + 1900,
        local->tm_hour, local->tm_min, local->tm_sec);
}

void copy_track_layout(const track_t& src, track_t& dest) {
    if (src.status == TRACK_UNKNOWN) return;

    dest.status = TRACK_GUESSED;
    dest.data_mode = src.data_mode;
    dest.num_sectors = src.num_sectors;
    dest.sector_size_code = src.sector_size_code;

    int cyl_diff = dest.phys_cyl - src.phys_cyl;
    for (int i = 0; i < src.num_sectors; i++) {
        const sector_t& src_sec = src.sectors[i];
        sector_t& dest_sec = dest.sectors[i];

        dest_sec.log_cyl = src_sec.log_cyl + cyl_diff;
        dest_sec.log_head = src_sec.log_head;
        dest_sec.log_sector = src_sec.log_sector;
    }
}

bool track_scan_sectors(const track_t& track, const sector_t** lowest) {
    bool seen[MAX_SECS] = {false};

    *lowest = NULL;
    int lowest_id = MAX_SECS;
    int highest_id = 0;
    for (int i = 0; i < track.num_sectors; i++) {
        const sector_t& sector = track.sectors[i];
        const int id = sector.log_sector;
        assert(id < MAX_SECS);
        assert(!seen[id]); // How would we handle getting the same sector id twice?
        seen[id] = true;

        if (id < lowest_id) {
            lowest_id = id;
            *lowest = &sector;
        }
        if (id > highest_id) {
            highest_id = id;
        }
    }

    for (int i = lowest_id; i < highest_id; i++) {
        if (!seen[i]) {
            return false; // not contiguous
        }
    }
    return true; // contiguous
}

bool same_sector_addr(const sector_t& a, const sector_t& b) {
    if (a.log_cyl != b.log_cyl) return false;
    if (a.log_head != b.log_head) return false;
    if (a.log_sector != b.log_sector) return false;
    return true;
}
