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

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define IMD_END_OF_COMMENT 0x1A

#define IMD_HEAD_MASK 0x03
#define IMD_NEED_CYL_MAP 0x80
#define IMD_NEED_HEAD_MAP 0x40
#define IMD_ALL_FLAGS (IMD_HEAD_MASK | IMD_NEED_CYL_MAP | IMD_NEED_HEAD_MAP)

// These Sector Data Record flags are combined by +, not |.
#define IMD_SDR_DATA 0x01
#define IMD_SDR_IS_COMPRESSED 0x01
#define IMD_SDR_IS_DELETED 0x02
#define IMD_SDR_IS_ERROR 0x04
#define IMD_SDR_ANOTHER_DATA_FOLLOWS 0x08 // Extension to original .IMD file format.
#define IMD_SDR_HAS_DATA_COUNT 0x10       // Extension to original .IMD file format.

// Read a track and add it to the disk. Return false on EOF.
static bool read_imd_track(FILE *image, disk_t *disk) {
    uint8_t header[5];
    int count = fread(header, 1, 5, image);
    if (count == 0 && feof(image)) {
        return false;
    }
    if (count != 5) {
        die("Couldn't read IMD track header");
    }

    int phys_cyl = header[1];
    if (phys_cyl >= MAX_CYLS) {
        die("IMD track cylinder value too large: %d", phys_cyl);
    }
    if (phys_cyl >= disk->num_phys_cyls) {
        disk->num_phys_cyls = phys_cyl + 1;
    }

    if ((header[2] & ~IMD_ALL_FLAGS) != 0) {
        die("IMD track has unsupported flags: %02x", header[2]);
    }

    int phys_head = header[2] & IMD_HEAD_MASK;
    if (phys_head >= MAX_HEADS) {
        die("IMD track head value too large: %d", phys_head);
    }
    if (phys_head >= disk->num_phys_heads) {
        disk->num_phys_heads = phys_head + 1;
    }

    track_t *track = &disk->tracks[phys_cyl][phys_head];
    track->status = TRACK_PROBED;
    for (int i = 0; ; i++) {
        if (DATA_MODES[i].name == NULL) {
            die("IMD track mode unknown: %d", header[0]);
        }
        if (DATA_MODES[i].imd_mode == header[0]) {
            track->data_mode = &DATA_MODES[i];
            break;
        }
    }
    track->phys_cyl = phys_cyl;
    track->phys_head = phys_head;
    size_t num_sectors = header[3];
    track->num_sectors = num_sectors;
    track->sector_size_code = header[4];
    if (track->num_sectors == 0) {
        return true; // Nothing else to do. (Note: a completely unreadable track will have no sectors and sector_size_code 0xFF.)
    }
    if (track->sector_size_code == 0xFF) {
        // FIXME: implement this (by having arbitrary sector sizes)
        die("IMD variable sector size extension not supported");
    }
    size_t sector_size = sector_bytes(track->sector_size_code);

    uint8_t sec_map[num_sectors];
    uint8_t cyl_map[num_sectors];
    uint8_t head_map[num_sectors];

    if (fread(sec_map, 1, num_sectors, image) != num_sectors) {
        die("Couldn't read IMD sector map");
    }
    if (header[2] & IMD_NEED_CYL_MAP) {
        if (fread(cyl_map, 1, num_sectors, image) != num_sectors) {
            die("Couldn't read IMD cylinder map");
        }
    } else {
        memset(cyl_map, phys_cyl, num_sectors);
    }
    if (header[2] & IMD_NEED_HEAD_MAP) {
        if (fread(head_map, 1, num_sectors, image) != num_sectors) {
            die("Couldn't read IMD head map");
        }
    } else {
        memset(head_map, phys_head, num_sectors);
    }

    for (size_t phys_sec = 0; phys_sec < num_sectors; phys_sec++) {
        sector_t *sector = &track->sectors[phys_sec];

        assert(sector->status == SECTOR_MISSING);
        sector->log_cyl = cyl_map[phys_sec];
        sector->log_head = head_map[phys_sec];
        sector->log_sector = sec_map[phys_sec];
        sector->deleted = false;
        sector->datas.clear();

        bool first_read = true;
        bool have_data_to_read = true;
        while (have_data_to_read) {
            have_data_to_read = false; // By default we only have one sector type to read.
            uint8_t type, orig_type;
            uint32_t count = 1;
            if (fread(&type, 1, 1, image) != 1) {
                die("Couldn't read IMD sector header");
            }
            orig_type = type;

            if (type > 0) {
                //printf("%s:%d got type %08x for (%d.%d.%d)\n", __FILE__, __LINE__, type, sector->log_cyl, sector->log_head, sector->log_sector);
                type -= IMD_SDR_DATA;

                if (type >= IMD_SDR_HAS_DATA_COUNT) {
                    type -= IMD_SDR_HAS_DATA_COUNT;
                    if (fread(&count, sizeof(count), 1, image) != 1) {
                        die_errno("Couldn't read IMD data count");
                    }
                    count = ntohl(count);
                    assert(count > 1);
                }

                if (type >= IMD_SDR_ANOTHER_DATA_FOLLOWS) {
                    type -= IMD_SDR_ANOTHER_DATA_FOLLOWS;
                    have_data_to_read = true; // This flag means another data follows this one.
                }

                if (first_read) {
                    if (type >= IMD_SDR_IS_ERROR) {
                        type -= IMD_SDR_IS_ERROR;
                        sector->status = SECTOR_BAD;
                    } else {
                        sector->status = SECTOR_GOOD;
                    }
                } else {
                    assert(type < IMD_SDR_IS_ERROR);
                }

                if (first_read) {
                    if (type >= IMD_SDR_IS_DELETED) {
                        type -= IMD_SDR_IS_DELETED;
                        sector->deleted = true;
                    }
                } else {
                    assert(type < IMD_SDR_IS_DELETED);
                }

                data_t this_data;
                if (type >= IMD_SDR_IS_COMPRESSED) {
                    type -= IMD_SDR_IS_COMPRESSED;
                    uint8_t fill;
                    if (fread(&fill, 1, 1, image) != 1) {
                        die("Couldn't read IMD compressed sector data");
                    }
                    this_data.assign(sector_size, fill);
                } else {
                    uint8_t data_buf[sector_size];
                    if (fread(data_buf, 1, sector_size, image) != sector_size) {
                        die("Couldn't read IMD sector data");
                    }
                    this_data.assign(data_buf, sector_size);
                }
                std::pair<data_map_t::iterator, bool> ret = sector->datas.insert(data_map_t::value_type(this_data, count));
                if (!ret.second) {
                    die("unexpected duplicate data");
                }

                if (type != 0) {
                    die("IMD sector has unsupported flags: %08x", orig_type);
                }
            }
            first_read = false;
        }
    }

    return true;
}

void read_imd(FILE *image, disk_t& disk) {
    init_disk(&disk);

    // Read the comment.
    char* read_buf;
    size_t dummy = 0;
    ssize_t count = getdelim(&read_buf, &dummy, IMD_END_OF_COMMENT, image);
    if (count < 0 || read_buf[count-1] != IMD_END_OF_COMMENT) {
        die("Couldn't find IMD comment delimiter");
    }
    disk.comment = std::string(read_buf, count-1);
    free(read_buf); // free in accordance with getdelim spec

    disk.num_phys_cyls = 0;
    disk.num_phys_heads = 0;

    while (read_imd_track(image, &disk)) {
        // Nothing.
    }
}

void write_imd_header(const disk_t *disk, FILE *image) {
    if (!disk->comment.empty()) {
        fwrite(disk->comment.c_str(), 1, disk->comment.length(), image);
    }
    fputc(IMD_END_OF_COMMENT, image);
}

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
        uint8_t(flags | track->phys_head),
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

    for (int i = 0; i < track->num_sectors; i++) {
        const sector_t *sector = &(track->sectors[i]);

        uint8_t type = 0;
        //printf("sector i %d status %d\n", i, sector->status);
        assert(sector->datas.empty() == (sector->status == SECTOR_MISSING));
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
        if (sector->deleted) {
            type += IMD_SDR_IS_DELETED;
            assert(!sector->datas.empty());
        }

        if (!sector->datas.empty()) {
            for (data_map_t::const_iterator iter = sector->datas.begin(); iter != sector->datas.end(); iter++) {
                assert(iter->first.length() == sector_bytes(track->sector_size_code));

                if (iter->second > 1) {
                    type += IMD_SDR_HAS_DATA_COUNT;
                }
                if (std::distance(iter, sector->datas.end()) != 1) {
                    type += IMD_SDR_ANOTHER_DATA_FOLLOWS;
                }

                // If every byte in the sector is identical, just store it once, with a "compressed" flag.
                const uint8_t first = iter->first[0];
                bool can_compress = true;
                type += IMD_SDR_IS_COMPRESSED;
                for (unsigned int i = 0; i < iter->first.length(); i++) {
                    if (iter->first[i] != first) {
                        can_compress = false;
                        type -= IMD_SDR_IS_COMPRESSED;
                        break;
                    }
                }

                //printf("%s:%d wrote type %08x for (%d.%d.%d)\n", __FILE__, __LINE__, type, sector->log_cyl, sector->log_head, sector->log_sector);
                fputc(type, image);

                if (iter->second > 1) {
                    uint32_t buf = htonl(iter->second);
                    size_t ret = fwrite(&buf, sizeof(buf), 1, image);
                    if (ret != 1) { die_errno("fwrite failed"); }
                }

                if (can_compress) {
                    fputc(first, image);
                } else {
                    fwrite(iter->first.data(), 1, iter->first.length(), image);
                }
                type = IMD_SDR_DATA; // Only the first 'type' contains error flags.
            }
        } else {
            //printf("%s:%d wrote type %08x for (%d.%d.%d)\n", __FILE__, __LINE__, type, sector->log_cyl, sector->log_head, sector->log_sector);
            fputc(type, image);
        }
    }
}
