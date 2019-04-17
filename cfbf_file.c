#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>

#include "cfbf.h"

void
cfbf_close(struct cfbf *cfbf) {
    cfbf_fat_close(&cfbf->fat);
    cfbf_fat_close(&cfbf->mini_fat);
    free(cfbf->mini_stream);
    munmap(cfbf->file, cfbf->file_size);
    close(cfbf->fd);
}

int
cfbf_open(const char *filename, struct cfbf *cfbf) {
    struct stat st;
    struct DirEntry *root;

    memset(cfbf, 0, sizeof(*cfbf));

    if (stat(filename, &st) < 0) {
        error(0, errno, "%s", filename);
        return -1;
    }

    cfbf->fd = open(filename, O_RDONLY);
    if (cfbf->fd < 0) {
        error(0, errno, "%s", filename);
        goto fail;
    }

    cfbf->file_size = st.st_size;

    if (cfbf->file_size < sizeof(struct StructuredStorageHeader)) {
        error(0, 0, "%s is too small (%lld bytes) to contain a StructuredStorageHeader (%d bytes)", filename, cfbf->file_size, (int) sizeof(struct StructuredStorageHeader));
        goto fail;
    }

    cfbf->file = mmap(NULL, cfbf->file_size, PROT_READ, MAP_SHARED, cfbf->fd, 0);
    if (cfbf->file == NULL) {
        error(0, errno, "failed to mmap %s", filename);
        goto fail;
    }

    cfbf->header = (struct StructuredStorageHeader *) cfbf->file;

    if (memcmp(cfbf->header->_abSig, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) {
        error(0, errno, "%s: signature bytes not as expected", filename);
        goto fail;
    }

    unsigned long num_start_sectors;
    unsigned long num_fat_sectors = (unsigned long) cfbf->header->_csectFat;

    if (num_fat_sectors > 109)
        num_start_sectors = 109;
    else
        num_start_sectors = num_fat_sectors;

    if (cfbf_fat_open(&cfbf->fat, cfbf, cfbf->header->_sectFat,
                num_start_sectors, cfbf->header->_sectDifStart,
                (unsigned long) cfbf->header->_csectDif,
                num_fat_sectors) < 0) {
        error(0, 0, "%s: failed to load FAT", filename);
        goto fail;
    }

    if (cfbf_mini_fat_open(&cfbf->mini_fat, &cfbf->fat, cfbf,
                cfbf->header->_sectMiniFatStart, cfbf->header->_csectMiniFat) < 0) {
        error(0, 0, "%s: failed to load mini-FAT", filename);
        goto fail;
    }

    /* Load mini-stream - the start sector and length of this is given by the
     * RootEntry */
    root = cfbf_get_sector_ptr(cfbf, cfbf->header->_sectDirStart);
    if (root == NULL) {
        error(0, 0, "%s: failed to look up root entry", filename);
        goto fail;
    }

    if (memcmp(root->name, "R\0o\0o\0t\0 \0E\0n\0t\0r\0y\0\0\0", 22)) {
        error(0, 0, "%s: first directory entry is not called RootEntry", filename);
        goto fail;
    }

    cfbf->mini_stream = cfbf_alloc_chain_contents_from_fat(cfbf, root->start_sector, root->stream_size);
    if (cfbf->mini_stream == NULL) {
        error(0, 0, "%s: failed to load mini-stream", filename);
        goto fail;
    }
    cfbf->mini_stream_size = root->stream_size;

    return 0;

fail:
    cfbf_close(cfbf);
    return -1;
}

int
cfbf_get_sector_size(struct cfbf *cfbf) {
    return 1 << cfbf->header->_uSectorShift;
}

int
cfbf_get_mini_fat_sector_size(struct cfbf *cfbf) {
    return 1 << cfbf->header->_uMiniSectorShift;
}

int
cfbf_read_sector(struct cfbf *cfbf, SECT sect, void *dest) {
    int sect_size = cfbf_get_sector_size(cfbf);
    void *src = cfbf_get_sector_ptr(cfbf, sect);
    if (src == NULL)
        return -1;

    memcpy(dest, src, sect_size);

    return 0;
}

void *
cfbf_get_sector_ptr(struct cfbf *cfbf, SECT sect) {
    int sect_size = cfbf_get_sector_size(cfbf);
    unsigned long long offset = (sect + 1) * sect_size;

    if (offset >= cfbf->file_size) {
        error(0, 0, "can't get sector %lu - it's past the end of the file (file size %lld, sector size %d)", (unsigned long) sect, cfbf->file_size, sect_size);
        return NULL;
    }

    return cfbf->file + offset;
}

void *
cfbf_get_sector_ptr_in_mini_stream(struct cfbf *cfbf, SECT sector) {
    int mini_sector_size = cfbf_get_mini_fat_sector_size(cfbf);
    uint64_t offset = sector * mini_sector_size;
    if (offset > cfbf->mini_stream_size) {
        return NULL;
    }
    return (void *) ((char *) cfbf->mini_stream + offset);
}


int
cfbf_is_sector_in_file(struct cfbf *cfbf, SECT sect) {
    int sect_size = cfbf_get_sector_size(cfbf);
    if ((sect + 1) * sect_size + sect_size > cfbf->file_size)
        return 0;
    else
        return 1;
}
