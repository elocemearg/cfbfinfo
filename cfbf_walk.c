#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <iconv.h>

#include "cfbf.h"

struct walk_sector {
    struct DirEntry *entry;
    FSINDEX sector_index;
    unsigned int is_difsect : 1;
    unsigned int is_fatsect : 1;
    unsigned int is_visited : 1;
};

int
utf16_to_utf8(char *in, size_t in_size, char *out, size_t out_size) {
    iconv_t cd = iconv_open("UTF-8", "UTF-16");
    if (cd == (iconv_t) -1) {
        error(0, errno, "iconv_open()");
        return -1;
    }

    if (iconv(cd, &in, &in_size, &out, &out_size) == (size_t) -1) {
        error(0, errno, "iconv()");
        iconv_close(cd);
        return -1;
    }
    iconv_close(cd);

    *out = '\0';

    return 0;
}

/* Mark this sector as visited in the map, and complain if something else has
 * already visited it */
int
visit_sector(struct walk_sector *sector_map, SECT num_sectors, SECT sector_num,
        FSINDEX sector_index, struct DirEntry *ent, int special) {
    struct walk_sector *sector;

    if (sector_num >= num_sectors) {
        error(0, 0, "sector %lu is off the end of the map (%d)", (unsigned long) sector_num, num_sectors);
        return -1;
    }
    
    sector = &sector_map[sector_num];

    if (sector->entry) {
        error(0, 0, "sector %lu: this is already in use by another entry! (start sector %lu)", (unsigned long) sector_num, (unsigned long) sector->entry->start_sector);
        return -1;
    }

    if (sector->is_visited) {
        error(0, 0, "sector %lu: this sector has already been visited! entry %p, sector_index %lu, is_difsect %u, is_fatsect %u, is_visited %u",
                (unsigned long) sector_num, sector->entry,
                (unsigned long) sector->sector_index,
                (unsigned int) sector->is_difsect,
                (unsigned int) sector->is_fatsect,
                (unsigned int) sector->is_visited);
        return -1;
    }

    if (ent) {
        sector->entry = ent;
        sector->sector_index = sector_index;
    }
    else if (special == CFBF_FATSECT) {
        sector->is_fatsect = 1;
    }
    else if (special == CFBF_DIFSECT) {
        sector->is_difsect = 1;
    }
    sector->is_visited = 1;

    return 0;
}

int
cfbf_walk_entry(struct cfbf *cfbf, struct walk_sector *sector_map,
        SECT num_sectors, struct DirEntry *ent, FILE *out, int verbosity) {
    SECT sect, last_sect = CFBF_END_OF_CHAIN;
    struct cfbf_fat *fat;
    int64_t bytes_read = 0;
    int use_mini = 0;
    FSINDEX sector_index = 0;

    /* 0 means this is the directory chain and ent is a fake entry, and 5
     * means it's the root entry. Either way, the chain is in the main FAT
     * and not the mini one. */
    if (ent->object_type != 0 && ent->object_type != 5 &&
            ent->stream_size < cfbf->header->_ulMiniSectorCutoff) {
        fat = &cfbf->mini_fat;
        use_mini = 1;
    }
    else {
        fat = &cfbf->fat;
    }

    if (verbosity > 0)
        fprintf(out, "  first sector %lu%s\n", (unsigned long) ent->start_sector, use_mini ? " (mini-FAT)" : "");

    for (sect = ent->start_sector; sect != CFBF_END_OF_CHAIN; sect = cfbf_fat_get_sector_entry(fat, sect)) {
        if (verbosity > 1)
            fprintf(stderr, "  sector %10lu\r", (unsigned long) sect);
        if (!use_mini) {
            if (visit_sector(sector_map, num_sectors, sect, sector_index, ent, 0) < 0) {
                return -1;
            }
        }
        if (bytes_read >= ent->stream_size) {
            error(0, 0, "  read %lld bytes already but there are more sectors? sector %lu\n", (long long) bytes_read, (unsigned long) sect);
            return -1;
        }
        last_sect = sect;
        sector_index++;
        if (ent->stream_size - bytes_read < fat->sector_size) 
            bytes_read += ent->stream_size - bytes_read;
        else
            bytes_read += fat->sector_size;
    }
    if (verbosity > 1)
        fprintf(stderr, "                      \r");

    if (verbosity > 0)
        fprintf(out, "  last sector %lu%s\n", (unsigned long) last_sect, use_mini ? " (mini-FAT)" : "");

    if (bytes_read != ent->stream_size) {
        error(0, 0, "  read %lld bytes, expected %lld\n", (long long) bytes_read, (long long) ent->stream_size);
        return -1;
    }

    return 0;
}

int
cfbf_walk(struct cfbf *cfbf, FILE *out, int verbosity) {
    void **dir_chain;
    int num_dir_secs;
    int sector_size;
    int retval = 0;
    struct walk_sector *sector_map;
    SECT num_sectors;
    struct DirEntry fake_dir_entry_for_dir_chain;

    if (out == NULL)
        out = stderr;

    sector_size = cfbf_get_sector_size(cfbf);

    num_sectors = (cfbf->file_size - sector_size) / sector_size;

    sector_map = malloc(sizeof(struct walk_sector) * num_sectors);
    if (cfbf->file_size > cfbf->fat.sector_entries_count * sector_size + sizeof(struct StructuredStorageHeader)) {
        error(0, 0, "warning: sector count in FAT, %lld, is less than what we'd expect from file size %lld", (long long) cfbf->fat.sector_entries_count, (long long) cfbf->file_size);
    }

    memset(sector_map, 0, sizeof(struct walk_sector) * num_sectors);
    
    dir_chain = cfbf_get_chain_ptrs(cfbf, cfbf->header->_sectDirStart, &num_dir_secs);
    if (dir_chain == NULL) {
        return -1;
    }

    memset(&fake_dir_entry_for_dir_chain, 0, sizeof(fake_dir_entry_for_dir_chain));
    fake_dir_entry_for_dir_chain.start_sector = cfbf->header->_sectDirStart;
    fake_dir_entry_for_dir_chain.stream_size = num_dir_secs * sector_size;
    fake_dir_entry_for_dir_chain.object_type = 0;

    /* Walk the chain from _sectDirStart, in order to mark those sectors
     * as visited */
    if (verbosity >= 0)
        fprintf(out, "Walking directory chain, %d sectors...\n", num_dir_secs);

    if (cfbf_walk_entry(cfbf, sector_map, num_sectors, &fake_dir_entry_for_dir_chain, out, verbosity) < 0) {
        goto fail;
    }
    if (verbosity >= 0)
        fprintf(out, "Done.\n");

    for (int sec = 0; sec < num_dir_secs; ++sec) {
        int entries_per_sec = sector_size / sizeof(struct DirEntry);

        for (int i = 0; i < entries_per_sec; ++i) {
            struct DirEntry *ent = ((struct DirEntry *) dir_chain[sec]) + i;
            if (ent->object_type == 0) {
                continue;
            }
            else if (ent->object_type == 1 || ent->object_type == 2 || ent->object_type == 5) {
                char name[129];
                if (utf16_to_utf8((char *) ent->name, ent->name_length, name, sizeof(name)) < 0) {
                    goto fail;
                }
                if (ent->object_type == 1) {
                    if (verbosity > 0)
                        fprintf(out, "Skipping storage object \"%s\"\n", name);
                }
                else {
                    if (verbosity > 0)
                        fprintf(out, "Walking entry \"%s\", size %lld\n", name, (long long) ent->stream_size);
                    if (cfbf_walk_entry(cfbf, sector_map, num_sectors, ent, out, verbosity) < 0)
                        goto fail;
                }
            }
            else if (ent->object_type == 5) {
                if (verbosity > 0)
                    fprintf(out, "Skipping root entry\n");
            }
            else {
                error(0, 0, "Invalid object type %d, skipping", (int) ent->object_type);
                retval = -1;
            }
        }
    }

    int num_start_fat_sectors = 109;
    if (num_start_fat_sectors > cfbf->header->_csectFat)
        num_start_fat_sectors = cfbf->header->_csectFat;

    if (verbosity >= 0)
        fprintf(out, "Walking FAT chain, expecting %lu sectors...\n", (unsigned long) cfbf->header->_csectFat);

    for (int i = 0; i < num_start_fat_sectors; ++i) {
        SECT sect = cfbf->header->_sectFat[i];
        SECT fat_entry = cfbf_fat_get_sector_entry(&cfbf->fat, sect);
        if (visit_sector(sector_map, num_sectors, sect, 0, NULL, CFBF_FATSECT) < 0) {
            retval = -1;
        }
        if (verbosity > 1)
            fprintf(stderr, "  FAT sector %lu\r", (unsigned long) sect);

        if (fat_entry != CFBF_FATSECT) {
            error(0, 0, "FAT entry for sector %lu is %lu, expected CFBF_FATSECT (%lu)", (unsigned long) sect, (unsigned long) fat_entry, (unsigned long) CFBF_FATSECT);
            retval = -1;
        }
    }
    if (verbosity > 1)
        fprintf(stderr, "                               \r");

    /* If the FAT covers more than 109 sectors, then the extra pages of
     * FAT sector numbers are given by the DIFAT chain. */
    SECT difat_sect = cfbf->header->_sectDifStart;
    int num_difat_sectors_seen = 0;
    int num_entries_in_sect = sector_size / sizeof(SECT);
    int num_fat_sectors_seen = num_start_fat_sectors;

    if (difat_sect == CFBF_END_OF_CHAIN) {
        if (verbosity >= 0)
            fprintf(out, "  Not walking DIFAT chain because it is empty.\n");
    }
    else {
        if (verbosity >= 0)
            fprintf(out, "  Moving on to DIFAT chain, %lu sectors of more FAT sector numbers\n", (unsigned long) cfbf->header->_csectDif);
    }

    while (difat_sect != CFBF_END_OF_CHAIN) {
        SECT *difat_sect_ptr;

        if (verbosity > 0)
            fprintf(out, "  Reading DIFAT sector %lu...\n", (unsigned long) difat_sect);

        if (visit_sector(sector_map, num_sectors, difat_sect, 0, NULL, CFBF_DIFSECT) < 0) {
            retval = -1;
        }

        difat_sect_ptr = cfbf_get_sector_ptr(cfbf, difat_sect);
        if (difat_sect_ptr == NULL) {
            retval = -1;
            break;
        }

        for (int i = 0; i < num_entries_in_sect - 1; ++i) {
            SECT fat_sect = difat_sect_ptr[i];
            if (fat_sect == CFBF_FREESECT && num_fat_sectors_seen >= cfbf->header->_csectFat) {
                // this is fine - these are just entries to pad out the
                // DIFAT sector, they would otherwise refer to past the end
                // of the file
            }
            else {
                if (visit_sector(sector_map, num_sectors, fat_sect, 0, NULL, CFBF_FATSECT) < 0) {
                    retval = -1;
                }
                num_fat_sectors_seen++;
                if (verbosity > 1)
                    fprintf(stderr, "  FAT sector %lu\r", (unsigned long) fat_sect);
            }
        }
        if (verbosity > 1)
            fprintf(stderr, "                               \r");

        if (verbosity > 0)
            fprintf(out, "  Finished reading DIFAT sector %lu, %d FAT sector numbers seen so far.\n", (unsigned long) difat_sect, num_fat_sectors_seen);

        difat_sect = difat_sect_ptr[num_entries_in_sect - 1];
        num_difat_sectors_seen++;
    }

    if (num_difat_sectors_seen != cfbf->header->_csectDif) {
        error(0, 0, "expected %d sectors in DIFAT chain, but found %d", (int) cfbf->header->_csectDif, num_difat_sectors_seen);
        retval = -1;
    }

    if (num_fat_sectors_seen != cfbf->header->_csectFat) {
        error(0, 0, "expected %d sectors in FAT chain, but found %d", (int) cfbf->header->_csectFat, num_fat_sectors_seen);
        retval = -1;
    }

    if (verbosity >= 0)
        fprintf(out, "Done - visited %d FAT sectors.\n", num_fat_sectors_seen);

    /* Now look at what we have in the sector map */
    int num_unvisited_sectors = 0;
    int num_unvisited_not_unused = 0;

    for (SECT sec = 0; sec < num_sectors; ++sec) {
        struct walk_sector *s = &sector_map[sec];
        if (!s->is_visited) {
            SECT fat_entry = cfbf_fat_get_sector_entry(&cfbf->fat, sec);
            if (verbosity >= 0) {
                if (num_unvisited_sectors > 0)
                    fprintf(out, ", ");
                else
                    fprintf(out, "Unvisited sectors: ");
            }
            if (verbosity >= 0)
                fprintf(out, "%lu", (unsigned long) sec);
            ++num_unvisited_sectors;
            if (fat_entry != CFBF_FREESECT) {
                if (verbosity >= 0)
                    fprintf(out, " (%lu)", (unsigned long) fat_entry);
                ++num_unvisited_not_unused;
            }
        }
    }
    if (num_unvisited_sectors == 0) {
        if (verbosity > 0)
            fprintf(out, "No unvisited sectors.\n");
    }
    else {
        if (verbosity >= 0)
            fprintf(out, "\n");
    }
    if (verbosity > 0) {
        fprintf(out, "%d unvisited, of which %d not marked as unused.\n", num_unvisited_sectors, num_unvisited_not_unused);
        fprintf(out, "Done.\n");
    }

end:
    free(dir_chain);
    free(sector_map);
    return retval;

fail:
    retval = -1;
    goto end;
}
