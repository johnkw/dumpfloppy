/*
    disk.h: data structure representing an FM/MFM floppy disk

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

/*
    This data model is based on the description of the .IMD file format in the
    documentation for Dave Dunfield's ImageDisk program:
      http://www.classiccmp.org/dunfield/img/index.htm

    FIXME: IMD 1.18 doesn't define a mode number for 1000k MFM (used by ED
    disks).

    FIXME: when the IMD 1.18 spec says the sector map lists "the physical ID
    for each sector", it means "the *logical* ID". (That is, if you image a PC
    floppy with ImageDisk, it writes 01 02 ... 09 to the map; if you image a
    BBC floppy it writes 00 01 .. 09.)

    FIXME: Is the first line of the IMD comment meant to indicate the version
    of the format, or the application that created it?  (That is, should
    dumpfloppy write "IMD 1.18" or "dumpfloppy 42" at the start of the line?)
*/

#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stdint.h>

// Convert sector_size_code to size in bytes.
int sector_bytes(int code);

typedef struct {
    uint8_t imd_mode;
    const char *name;
    int rate; // 0 to 3
    bool is_fm;
} data_mode_t;

// Possible data modes, in the order in which they will be tried when probing.
// (The last has name == NULL.)
extern const data_mode_t DATA_MODES[];

typedef struct {
    uint8_t log_cyl;
    uint8_t log_head;
    uint8_t log_sector;
    unsigned char *data; // NULL if not read yet; allocate with malloc
} sector_t;

void init_sector(sector_t *sector);
void free_sector(sector_t *sector);

#define MAX_SECS 256
typedef struct {
    bool probed;
    const data_mode_t *data_mode;
    int phys_cyl;
    int phys_head;
    int num_sectors;
    int sector_size_code; // FDC code
    sector_t sectors[MAX_SECS]; // indexed by physical sector
} track_t;

void init_track(int phys_cyl, int phys_head, track_t *track);
void free_track(track_t *track);

#define MAX_CYLS 256
#define MAX_HEADS 2
typedef struct {
    char *comment; // allocate with malloc
    int comment_len; // in bytes, not including terminator
    int num_phys_cyls;
    int num_phys_heads;
    track_t tracks[MAX_CYLS][MAX_HEADS]; // indexed by physical cyl/head
    int cyl_step; // how many physical cyls to step for each logical one
} disk_t;

void init_disk(disk_t *disk);
void free_disk(disk_t *disk);

// Create a ImageDisk-style timestamp comment.
void make_disk_comment(const char *program, const char *version, disk_t *disk);

// Copy the layout of a track from another track on the same head.
void copy_track_layout(const disk_t *disk, const track_t *src, track_t *dest);

// Find the sectors with the lowest and highest logical IDs in a track,
// and whether the sectors have contiguous logical IDs.
void track_scan_sectors(track_t *track,
                        sector_t **lowest, sector_t **highest,
                        bool *contiguous);

#endif
