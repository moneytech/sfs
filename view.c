#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

#include "sfs.h"

void print_time_stamp(int64_t time_stamp)
{
    time_t time = time_stamp >> 16;
    struct tm tm;
    if (gmtime_r(&time, &tm) == NULL) {
        printf("<time error>");
    } else {
        printf("%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

void print_super(struct sfs_super *super)
{
    printf("super:\n");
    printf("    time_stamp: ");
    print_time_stamp(super->time_stamp);
    printf("\n");
    printf("    data_size: %" PRIx64 "\n", super->data_size);
    printf("    index_size: %" PRIx64 "\n", super->index_size);
    printf("    magic: 0x%02x%02x%02x\n", super->magic[0],
        super->magic[1], super->magic[2]);
    printf("    version: %02x\n", super->version);
    printf("    total_blocks: %" PRIx64 "\n", super->total_blocks);
    printf("    rsvd_blocks: %" PRIx32 "\n", super->rsvd_blocks);
    printf("    block_size: %d\n", 1 << (super->block_size + 7));
    printf("    crc: %02x\n", super->crc);
}

void print_volume(struct S_SFS_VOL_ID *volume)
{
    printf("volume:\n");
    printf("    type: %02x\n", volume->type);
    printf("    crc: %02x\n", volume->crc);
    printf("    resvd: %04x\n", volume->resvd);
    printf("    time_stamp: ");
    print_time_stamp(volume->time_stamp);
    printf("\n");
    printf("    name: %s\n", volume->name);
}

void print_dir_entry(struct S_SFS_DIR *dir)
{
    printf("dir:");
    printf("%32s\t", dir->name);
    print_time_stamp(dir->time_stamp);
    printf("\n");
}

void print_file_entry(struct S_SFS_FILE *file)
{
    printf("file:");
    printf("%32s\t", file->name);
    print_time_stamp(file->time_stamp);
//    printf("\tblocks:[%" PRIx64 "..", file->start_block);
//    printf("%" PRIx64 "]", file->end_block);
    printf("\tsize: %ld", file->file_len);
    printf("\n");
}

/* compare command and return arguments */
char *test_cmd_get_args(char *cmd, char *line)
{
    while (*cmd == *line) {
        cmd++;
        line++;
    }
    if (*line != ' ')
        return NULL;
    while (*line == ' ')
        line++;
    if (*line == '\n')
        return NULL;
    char *res = line;
    while (*line != '\n')
        line++;
    *line = '\0';
    return res;
}

void export_file(struct sfs *sfs, char *args)
{
    printf("export: args=%s\n", args);
    struct S_SFS_FILE *sfs_file = sfs_get_file_by_name(sfs, args);
    if (sfs_file == NULL) {
        printf("file not found\n");
        return;
    }

    char *buf = NULL;
    size_t buflen = 0;
    printf("export to: ");
    if (getline(&buf, &buflen, stdin) <= 1) {
        printf("export file name reading error\n");
        return;
    }
    char *pc = buf;
    while (*pc != '\n')
        pc++;
    *pc = 0;

    sfs_open_file(sfs_file);
    FILE *infile = sfs_file->file;
    FILE *outfile = fopen(buf, "w");
    if (outfile == NULL) {
        printf("export file open error\n");
        return;
    }
    char c[10];
    while (fread(c, 1, 1, infile) == 1)
        fwrite(c, 1, 1, outfile);
    fclose(outfile);
    fclose(infile);
}

int loop(struct sfs *sfs)
{
    int cont;
    printf(">");
    char *buf = NULL;
    size_t buflen = 0;
    
    if (getline(&buf, &buflen, stdin) > 1) {
        printf("line=%s", buf);
        char *args;
        if ((args = test_cmd_get_args("export", buf)) != NULL)
            export_file(sfs, args);
        cont = 1;
    } else {
        cont = 0;
    }
    free(buf);
    return cont;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <image file name>\n", argv[0]);
        return 1;
    }

    struct sfs *sfs = sfs_make(argv[1]);
    struct sfs_super *super = sfs_get_super(sfs);
    print_super(super);

    struct S_SFS_VOL_ID *volume = sfs_get_volume(sfs);
    print_volume(volume);

    struct sfs_entry_list *list = sfs_get_entry_list(sfs);
    while (list != NULL) {
        if (list->type == 0x12) //SFS_ENTRY_FILE
            print_file_entry(list->entry.file);
        else if (list->type == 0x11) //SFS_ENTRY_DIR
            print_dir_entry(list->entry.dir);
        else if (list->type != 0x01 //SFS_ENTRY_VOL_ID
                && list->type != 0x02) //SFS_ENTRY_START
            printf("<entry of type 0x%02x>\n", list->type);
        list = list->next;
    }
    int is_loop = loop(sfs);
    while (is_loop)
        is_loop = loop(sfs);
    sfs_terminate(sfs);
    return 0;
}
