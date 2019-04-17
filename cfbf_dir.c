#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <iconv.h>

#include "cfbf.h"

static size_t
strlen_utf16(uint16_t *s) {
    size_t count = 0;
    while (*s) {
        count++;
        s++;
    }
    return count;
}

static int
strncmp_utf16(const uint16_t *s1, const uint16_t *s2, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s1[i] < s2[i])
            return -1;
        else if (s1[i] > s2[i])
            return 1;
        else if (s1[i] == 0) {
            // found the end of the string
            return 0;
        }
    }
    return 0;
}

static uint16_t *
strchr_utf16(const uint16_t *s, int c) {
    while (*s) {
        if (*s == c)
            return (uint16_t *) s;
        ++s;
    }
    return NULL;
}

static struct DirEntry *
cfbf_find_path_in_tree(struct cfbf *cfbf, void **dir_chain,
        int dir_chain_length, int sector_size, int entries_per_sector,
        unsigned long entry_id, uint16_t *sought_path_utf16) {
    uint16_t *path_component_end;
    int path_component_length;
    int last_component = 0;
    SECT sector = entry_id / entries_per_sector;
    int entry_within_sector = entry_id % entries_per_sector;
    struct DirEntry *e;

    if (entry_id == CFBF_NOSTREAM)
        return NULL;
    
    e = ((struct DirEntry *) dir_chain[sector]) + entry_within_sector;

    path_component_end = strchr_utf16(sought_path_utf16, '/');
    if (path_component_end == NULL) {
        path_component_end = sought_path_utf16 + strlen_utf16(sought_path_utf16);
        last_component = 1;
    }

    path_component_length = path_component_end - sought_path_utf16;

    if (e->object_type != 1 && e->object_type != 2 && e->object_type != 5) {
        if (e->object_type != 0) {
            error(0, 0, "cfbf_find_path_in_tree(): entry ID %lu has invalid object type %d, skipping this entry", entry_id, (int) e->object_type);
        }
        return NULL;
    }
    else {
        /* If the name of this node matches sought_path_utf16 up to the first
         * slash, descend into this node's children. If there is no slash
         * in sought_path_utf16 and it matches the name of this entry,
         * return this entry. */
        if ((e->name_length - 1) / 2 == path_component_length &&
                !strncmp_utf16(sought_path_utf16, e->name, path_component_length)) {
            if (last_component) {
                /* We've found the entry referred to by the path. */
                return e;
            }
            else {
                return cfbf_find_path_in_tree(cfbf, dir_chain, dir_chain_length,
                        sector_size, entries_per_sector, e->child_id,
                        path_component_end + 1);
            }
        }
        else {
            /* This entry doesn't match, so try its siblings. */
            struct DirEntry *found;

            found = cfbf_find_path_in_tree(cfbf, dir_chain, dir_chain_length,
                    sector_size, entries_per_sector, e->left_sibling_id,
                    sought_path_utf16);
            if (!found) {
                found = cfbf_find_path_in_tree(cfbf, dir_chain,
                        dir_chain_length, sector_size, entries_per_sector,
                        e->right_sibling_id, sought_path_utf16);
            }
            return found;
        }
    }
}

struct DirEntry *
cfbf_dir_entry_find_path(struct cfbf *cfbf, char *sought_path_utf8) {
    void **dir_chain = NULL;
    int num_dir_secs;
    struct DirEntry *entry;
    uint16_t *sought_path_utf16 = NULL;
    int sought_path_utf16_max;
    char *in_ptr, *out_ptr;
    size_t in_left, out_left;
    iconv_t cd;

    dir_chain = cfbf_get_chain_ptrs(cfbf, cfbf->header->_sectDirStart, &num_dir_secs);

    if (dir_chain == NULL) {
        error(0, 0, "failed to read directory chain");
        return NULL;
    }

    /* Convert sought path to UTF-16 */
    sought_path_utf16_max = strlen(sought_path_utf8) * 4;
    sought_path_utf16 = malloc(sought_path_utf16_max);
    if (sought_path_utf16 == NULL) {
        error(0, errno, "cfbf_dir_entry_find_path() out of memory");
        goto nomem;
    }

    in_ptr = sought_path_utf8;
    out_ptr = (char *) sought_path_utf16;
    in_left = strlen(sought_path_utf8);
    out_left = sought_path_utf16_max;

    cd = iconv_open("UTF-16LE", "UTF-8");
    if (cd == (iconv_t) -1) {
        error(0, errno, "cfbf_dir_entry_find_path(): iconv_open failed");
        goto nomem;
    }

    if (iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left) == (size_t) -1) {
        iconv_close(cd);
        error(0, errno, "cfbf_dir_entry_find_path(): path charset conversion failed: %s", sought_path_utf8);
    }
    else {
        iconv_close(cd);
        *(uint16_t *) out_ptr = 0;
    }

    entry = cfbf_find_path_in_tree(cfbf, dir_chain, num_dir_secs,
            cfbf_get_sector_size(cfbf),
            cfbf_get_sector_size(cfbf) / sizeof(struct DirEntry), 0,
            sought_path_utf16);

end:
    free(dir_chain);
    free(sought_path_utf16);

    return entry;

nomem:
    entry = NULL;
    goto end;
}

static int
cfbf_walk_dir_tree_from_chain(struct cfbf *cfbf, void **dir_chain,
        int dir_chain_length, int sector_size, int entries_per_sector,
        unsigned long entry_id, struct DirEntry *parent,
        int depth, int (*callback)(void *, struct cfbf *, struct DirEntry *,
            struct DirEntry *, unsigned long, int), void *cookie) {
    SECT sector = entry_id / entries_per_sector;
    int entry_within_sector = entry_id % entries_per_sector;
    struct DirEntry *entry;
    int ret;

    if (sector >= dir_chain_length) {
        error(0, 0, "cfbf_walk_dir_tree_from_chain(): directory entry id %lu not in chain", (unsigned long) entry_id);
        return -1;
    }

    entry = &((struct DirEntry *) dir_chain[sector])[entry_within_sector];

    if (entry->object_type == 0) {
        error(0, 0, "cfbf_walk_dir_tree_from_chain(): directory entry id %lu is unused", (unsigned long) entry_id);
        return -1;
    }

    ret = callback(cookie, cfbf, entry, parent, entry_id, depth);
    if (ret == 0) {
        /* Give up but don't fail */
        return 0;
    }
    else if (ret < 0) {
        /* Give up and fail */
        return ret;
    }

    /* Visit children */
    if (entry->child_id != CFBF_NOSTREAM) {
        ret = cfbf_walk_dir_tree_from_chain(cfbf, dir_chain, dir_chain_length,
                sector_size, entries_per_sector, entry->child_id, entry,
                depth + 1, callback, cookie);
        if (ret <= 0)
            return ret;
    }

    /* Visit left siblings, and visit right siblings. The sibling links seem
     * to make a tree rather than a doubly-linked list, so do it recursively */
    if (entry->left_sibling_id != CFBF_NOSTREAM) {
        ret = cfbf_walk_dir_tree_from_chain(cfbf, dir_chain, dir_chain_length,
                sector_size, entries_per_sector, entry->left_sibling_id,
                parent, depth, callback, cookie);
        if (ret <= 0)
            return ret;
    }
    if (entry->right_sibling_id != CFBF_NOSTREAM) {
        ret = cfbf_walk_dir_tree_from_chain(cfbf, dir_chain, dir_chain_length,
                sector_size, entries_per_sector, entry->right_sibling_id,
                parent, depth, callback, cookie);
        if (ret <= 0)
            return ret;
    }
    
    return 1;
}

/* callback should return positive if all is well and to continue the walk,
 * zero to terminate the walk but not fail, and a negative number to terminate
 * the walk and fail.
 * If the callback ever returns zero or negative, cfbf_walk_dir_tree() returns
 * that value. Otherwise we return 1. */
int
cfbf_walk_dir_tree(struct cfbf *cfbf,
        int (*callback)(void *cookie, struct cfbf *cfbf, struct DirEntry *entry,
            struct DirEntry *parent, unsigned long entry_id, int depth),
        void *cookie) {
    int ret;
    int num_dir_secs;
    void **dir_chain;

    dir_chain = cfbf_get_chain_ptrs(cfbf, cfbf->header->_sectDirStart, &num_dir_secs);
    if (dir_chain == NULL) {
        error(0, 0, "failed to read directory chain");
        return -1;
    }

    ret = cfbf_walk_dir_tree_from_chain(cfbf, dir_chain, num_dir_secs,
            cfbf_get_sector_size(cfbf),
            cfbf_get_sector_size(cfbf) / sizeof(struct DirEntry),
            0, NULL, 0, callback, cookie);

    free(dir_chain);

    return ret;
}

void
cfbf_object_type_to_string(int object_type, char *dest, int dest_max) {
    switch (object_type) {
        case 0:
            strncpy(dest, "unused", dest_max);
            break;

        case 1:
            strncpy(dest, "storage", dest_max);
            break;

        case 2:
            strncpy(dest, "stream", dest_max);
            break;

        case 5:
            strncpy(dest, "root", dest_max);
            break;

        default:
            snprintf(dest, dest_max, "%02X", (unsigned char) object_type);
    }
    if (dest_max > 0)
        dest[dest_max - 1] = '\0';
}

int
cfbf_dir_stored_in_mini_stream(struct cfbf *cfbf, struct DirEntry *entry) {
    if (entry->object_type == 2 && entry->stream_size > 0 &&
            entry->stream_size < cfbf->header->_ulMiniSectorCutoff) {
        return 1;
    }
    else {
        return 0;
    }
}

void **
cfbf_dir_entry_get_sector_ptrs(struct cfbf *cfbf, struct DirEntry *entry, int *num_sectors_r, int *sector_size_r) {
    if (cfbf_dir_stored_in_mini_stream(cfbf, entry)) {
        if (sector_size_r)
            *sector_size_r = cfbf_get_mini_fat_sector_size(cfbf);
        return cfbf_get_chain_ptrs_from_mini_stream(cfbf, entry->start_sector, num_sectors_r);
    }
    else {
        if (sector_size_r)
            *sector_size_r = cfbf_get_sector_size(cfbf);
        return cfbf_get_chain_ptrs(cfbf, entry->start_sector, num_sectors_r);
    }
}
