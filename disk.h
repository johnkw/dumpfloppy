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
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <map>

// Convert sector_size_code to size in bytes.
size_t sector_bytes(int code);

typedef struct {
    uint8_t imd_mode;
    const char *name;
    int rate; // 0 to 3
    bool is_fm;
} data_mode_t;

// Possible data modes, in the order in which they will be tried when probing.
// (The last has name == NULL.)
extern const data_mode_t DATA_MODES[];

typedef enum {
    SECTOR_MISSING = 0,
    SECTOR_BAD,
    SECTOR_GOOD,
    SECTOR_ENUM_HIGHEST = SECTOR_GOOD // Ensure this is this highest entry (and enums values are monotonic), per usage elsewhere.
} sector_status_t;


typedef std::basic_string<uint8_t> data_t; // Data bytes content of a particular sector read.
typedef std::map<data_t, uint32_t> data_map_t;  // Data bytes maps to a counter of how many times we found that data.

typedef struct {
    sector_status_t status;
    uint8_t log_cyl;
    uint8_t log_head;
    uint8_t log_sector;
    bool deleted;
    data_map_t datas;
} sector_t;

void init_sector(sector_t& sector);
void assert_free_sector(const sector_t& sector);

typedef enum {
    TRACK_UNKNOWN = 0,
    TRACK_GUESSED,
    // FIXME: Call this TRACK_KNOWN
    TRACK_PROBED
} track_status_t;

#define MAX_SECS 256
typedef struct {
    track_status_t status;
    const data_mode_t *data_mode;
    uint8_t phys_cyl;
    uint8_t phys_head;
    uint8_t num_sectors;
    uint8_t sector_size_code; // FDC code
    sector_t sectors[MAX_SECS]; // indexed by physical sector
} track_t;

void init_track(const int phys_cyl, const int phys_head, track_t& track);

#define MAX_CYLS 256
#define MAX_HEADS 2
typedef struct {
    std::string comment;
    int num_phys_cyls;
    int num_phys_heads;
    track_t tracks[MAX_CYLS][MAX_HEADS]; // indexed by physical cyl/head
} disk_t;

void init_disk(disk_t& disk);

// Create a ImageDisk-style timestamp comment.
void make_disk_comment(const char *program, const char *version, disk_t& disk);

// Copy the layout of a track from another track on the same head.
void copy_track_layout(const track_t& src, track_t& dest);

// Find the sectors with the lowest and highest logical IDs in a track,
// and whether the sectors have contiguous logical IDs.
bool track_scan_sectors(const track_t& track, const sector_t **lowest);

// Return whether two sectors have the same logical address.
bool same_sector_addr(const sector_t& a, const sector_t& b);

#endif
