#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <getopt.h>
#include <errno.h>
#include <error.h>
#include <iconv.h>

#include "cfbf.h"

struct write_pub_text_state {
    iconv_t iconv_desc;
    FILE *out;
};

int
print_dir_entry(void *cookie, struct cfbf *cfbf, struct DirEntry *e,
        struct DirEntry *parent, unsigned long entry_id, int depth) {
    char name[129];
    char *in_p;
    int name_length;
    char *out_p;
    size_t in_left, out_left, ret;
    int indent = depth * 4;
    int retval = 1;
    char obj_type_str[10];
    FILE *out = (FILE *) cookie;

    iconv_t cd = iconv_open("UTF-8", "UTF-16");
    if (cd == (iconv_t) -1) {
        error(0, errno, "iconv_open");
        goto fail;
    }

    if (e->name_length > 64) {
        error(0, 0, "warning: dir entry %lu: name_length is %hu which is > 64", entry_id, (unsigned short) e->name_length);
        name_length = 64;
    }
    else {
        name_length = (int) e->name_length;
    }

    in_p = (char *) e->name;
    out_p = name;
    in_left = name_length;
    out_left = sizeof(name);
    ret = iconv(cd, &in_p, &in_left, &out_p, &out_left);
    if (ret == (size_t) -1) {
        error(0, errno, "dir entry %lu: failed to convert filename from UTF-16", entry_id);
        goto fail;
    }
    *out_p = '\0';

    cfbf_object_type_to_string(e->object_type, obj_type_str, sizeof(obj_type_str));

    fprintf(out, "%-8s %10lu%s %10llu    ", obj_type_str,
            (unsigned long) e->start_sector,
            cfbf_dir_stored_in_mini_stream(cfbf, e) ? "m" : " ",
            (unsigned long long) e->stream_size);

    fprintf(out, "%*s", indent, "");
    fprintf(out, "%s\n", name);

end:
    iconv_close(cd);

    return retval;

fail:
    retval = -1;
    goto end;
}

int
write_publisher_text(void *cookie, const char *data, size_t data_length) {
    struct write_pub_text_state *state = (struct write_pub_text_state *) cookie;
    if (state->iconv_desc == (iconv_t) -1) {
        /* We're not converting the character encoding, we're just writing
         * it all straight out */
        size_t ret = fwrite(data, 1, data_length, state->out);
        if (ret != data_length) {
            error(0, errno, "fwrite()");
            return -1;
        }
    }
    else {
        /* Convert text using iconv_desc before writing it out */
        char *in_p = (char *) data;
        char *out_p;
        size_t in_left, out_left;

        char buf[1024];

        in_left = data_length;

        while (in_left > 0) {
            size_t ret;
            out_left = sizeof(buf);
            out_p = buf;

            ret = iconv(state->iconv_desc, &in_p, &in_left, &out_p, &out_left);
            if (ret == (size_t) -1 && errno != E2BIG) {
                error(0, errno, "write_publisher_text()");
                return -1;
            }

            ret = fwrite(buf, 1, out_p - buf, state->out);
            if (ret != out_p - buf) {
                error(0, errno, "fwrite()");
                return -1;
            }
        }
    }

    return 0;
}

int
write_sector_to_file(void *cookie, const void *sector_data, int length,
        FSINDEX sector_index, int64_t file_offset) {
    FILE *out = (FILE *) cookie;
    size_t ret;

    ret = fwrite(sector_data, 1, length, out);
    if (ret != length) {
        error(0, errno, "write_sector_to_file()");
        return -1;
    }

    return 0;
}

void
print_help(FILE *out) {
    fprintf(out, "Compound File Binary File format analyser\n");
    fprintf(out, "Graeme Cole, 2019\n");
    fprintf(out, "Usage: cfbf [action] [options] file.pub\n");
    fprintf(out, "Actions:\n");
    fprintf(out, "    -h         Show this help\n");
    fprintf(out, "    -l         List directory tree\n");
    fprintf(out, "    -r <path>  Dump the object with this path to the output file\n");
    fprintf(out, "               (e.g. -r \"Root Entry/Quill/QuillSub/CONTENTS\")\n");
    fprintf(out, "    -t         Extract TEXT section from CONTENTS object, write to output file\n");
    fprintf(out, "    -w         Walk FAT structure, highlight any problems\n");
    fprintf(out, "Options:\n");
    fprintf(out, "    -c <path>  [with -t] Path to use for CONTENTS object\n");
    fprintf(out, "               (default is \"Root Entry/Quill/QuillSub/CONTENTS\")\n");
    fprintf(out, "    -o <file>  Output file name (default is stderr for -w, stdout otherwise)\n");
    fprintf(out, "    -q         Be less verbose\n");
    fprintf(out, "    -u         [with -t] Don't convert text to UTF-8 for output, keep as UTF-16\n");
    fprintf(out, "    -v         Be more verbose\n");
    fprintf(out, "\n");
    fprintf(out, "Use -t to extract text from a Microsoft Publisher file.\n");
    fprintf(out, "With no action arguments, cfbf will print information from the header and exit.\n");
}

int main(int argc, char **argv) {
    int c;
    char *input_filename = NULL;
    struct cfbf cfbf;
    int show_header = 0;
    char *dump_object_path = NULL;
    int print_dir_tree = 0;
    char *output_filename = NULL;
    FILE *out = NULL;
    int walk = 0;
    int extract_publisher_text = 0;
    int exit_status = 0;
    int num_command_options = 0;
    int verbosity = 0;
    char *publisher_contents_path = "Root Entry/Quill/QuillSub/CONTENTS";
    int convert_text_to_utf8 = 1;

    while ((c = getopt(argc, argv, "hlr:twc:o:quv")) != -1) {
        switch (c) {
            case 'h':
                print_help(stdout);
                exit(0);
                break;

            case 'r':
                ++num_command_options;
                dump_object_path = optarg;

                // Skip any leading slashes - we don't want them
                while (*dump_object_path == '/') {
                    ++dump_object_path;
                }
                break;

            case 'l':
                print_dir_tree = 1;
                ++num_command_options;
                break;

            case 'o':
                output_filename = optarg;
                break;

            case 'q':
                verbosity--;
                break;

            case 'w':
                walk = 1;
                ++num_command_options;
                break;

            case 't':
                extract_publisher_text = 1;
                ++num_command_options;
                break;

            case 'u':
                convert_text_to_utf8 = 0;
                break;

            case 'c':
                publisher_contents_path = optarg;
                break;

            case 'v':
                verbosity++;
                break;

            default:
                exit(1);
        }
    }

    if (num_command_options > 1) {
        error(1, 0, "Only one of -r, -l, -t and -w may be given. Use -h for help.");
    }
    if (num_command_options == 0) {
        show_header = 1;
    }

    if (optind < argc) {
        input_filename = argv[optind];
    }
    else {
        print_help(stderr);
        exit(1);
    }

    if (cfbf_open(input_filename, &cfbf) != 0) {
        exit(1);
    }

    if (output_filename == NULL || !strcmp(output_filename, "-")) {
        if (walk)
            out = stderr;
        else
            out = stdout;
    }
    else {
        out = fopen(output_filename, "w");
        if (out == NULL)
            error(1, errno, "%s", output_filename);
    }

    struct StructuredStorageHeader *header = cfbf.header;
    if (show_header) {
        fprintf(out, "DllVersion, MinorVersion:     %hu, %hu\n", (unsigned short) header->_uDllVersion, (unsigned short) header->_uMinorVersion);
        fprintf(out, "Byte-order mark:              %02X %02X\n", ((unsigned char *) header)[0x1c], ((unsigned char *) header)[0x1d]);
        fprintf(out, "Main FAT sector size:         2^%hu (%d)\n", (unsigned short) header->_uSectorShift, cfbf_get_sector_size(&cfbf));
        fprintf(out, "Mini-stream sector size:      2^%hu (%d)\n", (unsigned short) header->_uMiniSectorShift, cfbf_get_mini_fat_sector_size(&cfbf));
        fprintf(out, "FAT chain sector count:       %lu\n", (unsigned long) header->_csectFat);
        if (header->_uSectorShift >= 12)
            fprintf(out, "Directory chain sector count: %lu\n", (unsigned long) header->_csectDir);
        fprintf(out, "Directory chain first sector: %lu\n", (unsigned long) header->_sectDirStart);
        fprintf(out, "Max file size in mini-stream: %lu\n", (unsigned long) header->_ulMiniSectorCutoff);
        fprintf(out, "MiniFAT first sector, count:  %lu, %lu\n", (unsigned long) header->_sectMiniFatStart, (unsigned long) header->_csectMiniFat);
        fprintf(out, "DIFAT first sector, count:    %lu, %lu\n", (unsigned long) header->_sectDifStart, (unsigned long) header->_csectDif);
        fprintf(out, "\n");
    }
    else if (print_dir_tree) {
        fprintf(out, "%-8s %10s  %10s    NAME\n", "TYPE", "START SEC", "SIZE");

        int ret = cfbf_walk_dir_tree(&cfbf, print_dir_entry, out);
        if (ret < 0) {
            exit_status = 1;
        }
    }
    else if (walk) {
        if (cfbf_walk(&cfbf, out, verbosity))
            exit_status = 1;
    }
    else if (dump_object_path) {
        struct DirEntry *entry = cfbf_dir_entry_find_path(&cfbf, dump_object_path);
        if (entry == NULL) {
            error(0, 0, "object \"%s\" not found in %s", dump_object_path, input_filename);
            exit_status = 1;
        }
        else if (entry->object_type == 5) {
            error(0, 0, "you're not allowed to dump the root entry");
            exit_status = 1;
        }
        else if (entry->object_type != 2) {
            error(0, 0, "%s is not a stream object", dump_object_path);
            exit_status = 1;
        }
        else {
            int ret;

            ret = cfbf_follow_chain(&cfbf, entry->start_sector,
                    entry->stream_size,
                    entry->stream_size < header->_ulMiniSectorCutoff,
                    write_sector_to_file, out);
            if (ret) {
                error(0, 0, "failed to read %s", dump_object_path);
                exit_status = 1;
            }
        }
    }
    else if (extract_publisher_text) {
        struct DirEntry *entry = cfbf_dir_entry_find_path(&cfbf, publisher_contents_path);

        if (entry == NULL) {
            error(0, 0, "Can't extract text: no entry named \"%s\" in directory", publisher_contents_path);
            exit_status = 1;
        }
        else {
            void **contents_chain;
            int sector_size, chain_length;

            contents_chain = cfbf_dir_entry_get_sector_ptrs(&cfbf, entry, &chain_length, &sector_size);
            if (contents_chain == NULL) {
                exit_status = 1;
            }
            else {
                struct write_pub_text_state state;

                memset(&state, 0, sizeof(state));

                if (convert_text_to_utf8) {
                    state.iconv_desc = iconv_open("UTF-8", "UTF-16LE");
                    if (state.iconv_desc == (iconv_t) -1) {
                        error(1, errno, "failed to create iconv descriptor");
                    }
                }
                else {
                    state.iconv_desc = (iconv_t) -1;
                }
                state.out = out;

                if (extract_text_from_contents_chain(contents_chain,
                            chain_length, sector_size, entry->stream_size,
                            verbosity, write_publisher_text, &state) < 0) {
                    exit_status = 1;
                }


                if (state.iconv_desc != (iconv_t) -1)
                    iconv_close(state.iconv_desc);
            }
            free(contents_chain);
        }
    }

    if (out != NULL && out != stdout && out != stderr) {
        if (fclose(out) == EOF) {
            error(0, errno, "%s", output_filename);
            exit_status = 1;
        }
    }

    cfbf_close(&cfbf);

    return exit_status;
}
