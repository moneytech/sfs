#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include "sfs.h"

/* Index Data Area Entry Types */
#define SFS_ENTRY_VOL_ID 0x01
#define SFS_ENTRY_START 0x02
#define SFS_ENTRY_UNUSED 0x10
#define SFS_ENTRY_DIR 0x11
#define SFS_ENTRY_FILE 0x12
#define SFS_ENTRY_UNUSABLE 0x18
#define SFS_ENTRY_DIR_DEL 0x19
#define SFS_ENTRY_FILE_DEL 0x1A

#define SFS_VERSION 0x11

#define SFS_SUPER_START 0x18e
#define SFS_SUPER_SIZE 42
#define SFS_VOL_NAME_LEN 52
#define SFS_ENTRY_SIZE 64
#define SFS_DIR_NAME_LEN 53
#define SFS_FILE_NAME_LEN 29

struct sfs_super {
    int64_t time_stamp;
    uint64_t data_size;
    uint64_t index_size;
    uint64_t total_blocks;
    uint32_t rsvd_blocks;
    uint8_t block_size;
};

struct sfs_block_list {
    uint64_t start_block;
    uint64_t length;
    struct sfs_entry *delfile;
    struct sfs_block_list *next;
};

struct sfs {
    FILE *file;
    int block_size;
    int del_file_count;
    struct sfs_super *super;
    struct sfs_entry *volume;
    struct sfs_entry *entry_list;
    struct sfs_block_list *free_list;
    struct sfs_block_list *free_last;
    struct sfs_entry *iter_curr;
};

/* The Volume ID Entry Data */
struct sfs_vol {
    int64_t time_stamp;
    char *name;
};

/* The Directory Entry Data */
struct sfs_dir {
    uint8_t num_cont;
    int64_t time_stamp;
    char *name;
};

/* The File Entry Data */
struct sfs_file {
    uint8_t num_cont;
    int64_t time_stamp;
    uint64_t start_block;
    uint64_t end_block;
    uint64_t file_len;
    char *name;
};

/* The Unusable Entry Data */
struct sfs_unusable {
    uint64_t start_block;
    uint64_t end_block;
};

struct sfs_entry {
    uint8_t type;
    long int offset;
    union {
        struct sfs_vol *volume_data;
        struct sfs_dir *dir_data;
        struct sfs_file *file_data;
        struct sfs_unusable *unusable_data;
    } data;
    struct sfs_entry *next;
};

/* The Volume ID Entry */
struct sfs_volume {
    uint8_t type;
    uint8_t crc;
    uint16_t resvd;
    int64_t time_stamp;
    uint8_t name[SFS_VOL_NAME_LEN];
};

/* The Start Marker Entry */
struct S_SFS_START {
    uint8_t type;
    uint8_t crc;
    uint8_t resvd[62];
};

/* The Unused Entry */
struct S_SFS_UNUSED {
    uint8_t type;
    uint8_t crc;
    uint8_t resvd[62];
};

/* The Directory Entry */
struct S_SFS_DIR {
    uint8_t type;
    uint8_t crc;
    uint8_t num_cont;
    uint8_t *name;
    int64_t time_stamp;
};

/* The File Entry */
struct S_SFS_FILE {
    uint8_t type;
    uint8_t crc;
    uint8_t num_cont;
    int64_t time_stamp;
    long f_offset;
    uint64_t start_block;
    uint64_t end_block;
    uint64_t file_len;
    FILE *file;
    char *file_buf;
    uint8_t *name;
    int del_number;
    struct sfs *sfs;
};

/* The Unusable Entry */
struct S_SFS_UNUSABLE {
    uint8_t type;
    uint8_t crc;
    long f_offset;
    uint8_t resv0[8];
    uint64_t start_block;
    uint64_t end_block;
    uint8_t resv1[38];
};

uint64_t timespec_to_time_stamp(struct timespec *timespec)
{
    time_t s = timespec->tv_sec;
    long n = timespec->tv_nsec;
    // timestamp = secs * 65536
    // (n / 1000000000 + s) * 65536
    // n*128/1953125 + 65536*s
    uint64_t timestamp = round((n << 7) / 1953125.0) + (s << 16);
    return timestamp;
}

uint64_t sfs_make_time_stamp()
{
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return timespec_to_time_stamp(&spec);
}

void sfs_fill_timespec(uint64_t time_stamp, struct timespec *timespec)
{
    uint64_t sec = time_stamp >> 16;

    // 1/65536 of sec
    uint64_t rest = time_stamp & 0x10000;

    // convert 1/65536 to 1/1000000000
    uint64_t nsec = round((rest * 1953125) / 128.0);

    timespec->tv_sec = sec;
    timespec->tv_nsec = nsec;
}

int check_crc(uint8_t *buf, int sz)
{
    uint8_t sum = 0;
    for (int i = 0; i < sz; i++)
        sum += buf[i];
    if (sum != 0) {
        fprintf(stderr, "crc error\n");
        return 0;
    }
    return 1;
}

struct sfs_super *sfs_read_super(struct sfs *sfs)
{
    sfs->super = malloc(sizeof(struct sfs_super));
    uint8_t buf[SFS_SUPER_SIZE];
    uint8_t *cbuf = buf;

    if (fseek(sfs->file, SFS_SUPER_START, SEEK_SET) != 0) {
        fprintf(stderr, "fseek error\n");
        return NULL;
    }

    fread(buf, SFS_SUPER_SIZE, 1, sfs->file);
    memcpy(&sfs->super->time_stamp, cbuf, sizeof(sfs->super->time_stamp));
    cbuf += sizeof(sfs->super->time_stamp);
    memcpy(&sfs->super->data_size, cbuf, sizeof(sfs->super->data_size));
    cbuf += sizeof(sfs->super->data_size);
    memcpy(&sfs->super->index_size, cbuf, sizeof(sfs->super->index_size));
    cbuf += sizeof(sfs->super->index_size);
    int m = cbuf - buf;

    char magic[3];
    memcpy(&magic, cbuf, 3);
    cbuf += 3;
    char version = *cbuf;
    cbuf += 1;
    if (strncmp(magic, "SFS", 3) != 0 || version != SFS_VERSION) {
        printf("!!!magic=%c%c%c, version=0x%x\n", magic[0], magic[1], magic[2], version);
        return NULL;
    }    

    memcpy(&sfs->super->total_blocks, cbuf, sizeof(sfs->super->total_blocks));
    cbuf += sizeof(sfs->super->total_blocks);
    memcpy(&sfs->super->rsvd_blocks, cbuf, sizeof(sfs->super->rsvd_blocks));
    cbuf += sizeof(sfs->super->rsvd_blocks);
    memcpy(&sfs->super->block_size, cbuf, sizeof(sfs->super->block_size));
    cbuf += sizeof(sfs->super->block_size);
    sfs->block_size = 1 << (sfs->super->block_size + 7);

    if (!check_crc(buf + m, SFS_SUPER_SIZE - m)) {
        return NULL;
    }    
    return sfs->super;
}

void sfs_write_super(FILE *file, struct sfs_super *super) {
    char buf[SFS_SUPER_SIZE];
    super->time_stamp = sfs_make_time_stamp();
    memcpy(&buf[0], &super->time_stamp, 8);
    memcpy(&buf[8], &super->data_size, 8);
    memcpy(&buf[16], &super->index_size, 8);
    memcpy(&buf[24], "SFS", 3);
    buf[27] = SFS_VERSION;
    memcpy(&buf[28], &super->total_blocks, 8);
    memcpy(&buf[36], &super->rsvd_blocks, 4);
    memcpy(&buf[40], &super->block_size, 1);
    int sum = 0;
    for (int i = 24; i < SFS_SUPER_SIZE - 1; ++i) {
        sum += buf[i];
    }
    buf[41] = 0x100 - (char)(sum % 0x100);
    fseek(file, SFS_SUPER_START, SEEK_SET);
    fwrite(buf, SFS_SUPER_SIZE, 1, file);
}

struct sfs_entry *sfs_read_volume_data(uint8_t *buf, struct sfs_entry *entry)
{
    struct sfs_vol *volume_data = malloc(sizeof(struct sfs_vol));
    uint8_t *cbuf = buf + 4; /* skip type, crc, and resvd */
    memcpy(&volume_data->time_stamp, cbuf, sizeof(volume_data->time_stamp));
    cbuf += sizeof(volume_data->time_stamp);
    volume_data->name = malloc(SFS_VOL_NAME_LEN);
    memcpy(volume_data->name, cbuf, SFS_VOL_NAME_LEN);
    entry->data.volume_data = volume_data;
    if (!check_crc(buf, SFS_ENTRY_SIZE)) {
        return NULL;
    }
    return entry;
}

struct sfs_entry *sfs_read_dir_data(uint8_t *buf, struct sfs_entry *entry, FILE *file)
{
    uint8_t *b = buf;
    struct sfs_dir *dir_data = malloc(sizeof(struct sfs_dir));

    memcpy(&dir_data->num_cont, &buf[2], 1);
    memcpy(&dir_data->time_stamp, &buf[3], 8);

    const int cont_len = dir_data->num_cont * SFS_ENTRY_SIZE;
    const int name_len = SFS_DIR_NAME_LEN + cont_len;
    printf("dir: name_len=%d ", name_len);

    dir_data->name = malloc(name_len);
    int bufsz = SFS_ENTRY_SIZE * (1 + dir_data->num_cont);
    uint8_t buf2[bufsz];
    if (dir_data->num_cont != 0) {
        b = buf2;
        memcpy(buf2, buf, SFS_ENTRY_SIZE);  /* copy from old buffer to new */
        fread(buf2 + SFS_ENTRY_SIZE, cont_len, 1, file); /* copy into new */
    }
    memcpy(dir_data->name, &b[11], name_len);
    if (!check_crc(b, bufsz)) {
        return NULL;
    }
    entry->data.dir_data = dir_data;
    printf(" name=%s\n", dir_data->name);
    return entry;   
}

struct sfs_entry *sfs_read_file_data(uint8_t *buf, struct sfs_entry *entry, FILE *file)
{
    uint8_t *b = buf;
    struct sfs_file *file_data = malloc(sizeof(struct sfs_file));

    memcpy(&file_data->num_cont, &buf[2], 1);
    memcpy(&file_data->time_stamp, &buf[3], 8);
    memcpy(&file_data->start_block, &buf[11], 8);
    memcpy(&file_data->end_block, &buf[19], 8);
    memcpy(&file_data->file_len, &buf[27], 8);

    const int cont_len = file_data->num_cont * SFS_ENTRY_SIZE;
    const int name_len = SFS_FILE_NAME_LEN + cont_len;
    printf("file: name_len=%d ", name_len);

    file_data->name = malloc(name_len);
    int bufsz = SFS_ENTRY_SIZE * (1 + file_data->num_cont);
    uint8_t buf2[bufsz];
    if (file_data->num_cont != 0) {
        b = buf2;
        memcpy(buf2, buf, SFS_ENTRY_SIZE);  /* copy from old buffer to new */
        fread(buf2 + SFS_ENTRY_SIZE, cont_len, 1, file); /* copy into new */
    }
    memcpy(file_data->name, &b[35], name_len);
    if (!check_crc(b, bufsz)) {
        return NULL;
    }
    entry->data.file_data = file_data;
    printf("name=%s\n", file_data->name);
    return entry;   
}

struct sfs_entry *sfs_read_unusable_data(uint8_t *buf, struct sfs_entry *entry)
{
    struct sfs_unusable *unusable_data = malloc(sizeof(struct sfs_unusable));
    memcpy(&unusable_data->start_block, &buf[10], 8);
    memcpy(&unusable_data->end_block, &buf[18], 8);
    entry->data.unusable_data = unusable_data;
    if (!check_crc(buf, SFS_ENTRY_SIZE)) {
        return NULL;
    }
    return entry;
}

/* read entry, at current location */
struct sfs_entry *sfs_read_entry(SFS *sfs)
{

    uint8_t buf[SFS_ENTRY_SIZE];

    struct sfs_entry *entry = malloc(sizeof(struct sfs_entry));
    entry->offset = ftell(sfs->file);
    if (entry->offset == -1) {
        return NULL;
    }
    fread(buf, SFS_ENTRY_SIZE, 1, sfs->file);
    entry->type = buf[0];
    entry->next = NULL;
    switch (entry->type) {
    case SFS_ENTRY_VOL_ID:
        return sfs_read_volume_data(buf, entry);
    case SFS_ENTRY_DIR:
    case SFS_ENTRY_DIR_DEL:
        return sfs_read_dir_data(buf, entry, sfs->file);
    case SFS_ENTRY_FILE:
    case SFS_ENTRY_FILE_DEL:
        return sfs_read_file_data(buf, entry, sfs->file);
    case SFS_ENTRY_UNUSABLE:
        return sfs_read_unusable_data(buf, entry);
    default:
        printf("entry->type=%x\n", entry->type);
        return entry;
    }
}

struct sfs_entry *sfs_read_volume(SFS *sfs)
{
    int vol_offset = sfs->block_size * sfs->super->total_blocks - SFS_ENTRY_SIZE;
    if (fseek(sfs->file, vol_offset, SEEK_SET) != 0) {
        fprintf(stderr, "fseek error\n");
        return NULL;
    }

    struct sfs_entry *volume = sfs_read_entry(sfs);
    if (volume->type != SFS_ENTRY_VOL_ID) {
        return NULL;
    }

    return volume;
}

struct sfs_entry *sfs_read_entries(SFS *sfs)
{
    int offset = sfs->block_size * sfs->super->total_blocks - sfs->super->index_size;
    printf("bs=0x%x, tt=0x%lxH, is=0x%lx, of=0x%x\n",
        sfs->block_size, sfs->super->total_blocks, sfs->super->index_size, offset);
    if (fseek(sfs->file, offset, SEEK_SET) != 0) {
        fprintf(stderr, "fseek error\n");
        return NULL;
    }
    struct sfs_entry *entry = sfs_read_entry(sfs);
    struct sfs_entry *head = entry;
    while (entry->type != SFS_ENTRY_VOL_ID) {
        struct sfs_entry *prev = entry;
        entry = sfs_read_entry(sfs);
        prev->next = entry;
    }
    sfs->volume = entry;
    return head;
}

struct sfs_block_list *block_list_from_entries(struct sfs_entry *entry_list)
{
    struct sfs_block_list *list = NULL;
    struct sfs_entry *entry = entry_list;
    while (entry != NULL) {
        if (entry->type == SFS_ENTRY_FILE
                || entry->type == SFS_ENTRY_FILE_DEL
                || entry->type == SFS_ENTRY_UNUSABLE) {
            struct sfs_block_list *item = malloc(sizeof(struct sfs_block_list));
            item->start_block = entry->data.unusable_data->start_block;
            item->next = list;
            list = item;
            switch (entry->type) {
            case SFS_ENTRY_FILE:
                item->start_block = entry->data.file_data->start_block;
                item->length = entry->data.file_data->end_block + 1 - item->start_block;
                item->delfile = NULL;
                break;
            case SFS_ENTRY_UNUSABLE:
                item->start_block = entry->data.unusable_data->start_block;
                item->length = entry->data.unusable_data->end_block + 1 - item->start_block;
                item->delfile = NULL;
                break;
            case SFS_ENTRY_FILE_DEL:
                item->start_block = entry->data.file_data->start_block;
                item->length = entry->data.file_data->end_block + 1 - item->start_block;
                item->delfile = entry;
                break;
            }
        }
        entry = entry->next;
    }
    return list;
}

struct sfs_block_list **conquer(struct sfs_block_list **p1, struct sfs_block_list **p2, int sz)
{
    int i1 = 0;
    int i2 = 0;
    while (i1 != sz || (i2 != sz && (*p2) != NULL)) {
        if ((i1 != sz) &&
                (*p2 == NULL
                 || i2 == sz
                 || (*p1)->start_block <= (*p2)->start_block)) {
            i1++;
            p1 = &(*p1)->next;
        } else {
            i2++;
            if (i1 != sz) {
                struct sfs_block_list *tmp = *p2;
                (*p2) = tmp->next;
                tmp->next = (*p1);
                *p1 = tmp;
                p1 = &(*p1)->next;
            } else {
                p2 = &(*p2)->next;
            }
        }
    }
    return p2;  // return the tail
}

void print_block_list(char *info, struct sfs_block_list *list)
{
    printf("%s", info);
    while (list != NULL) {
        char *d = list->delfile == NULL ? "" : "d";
        printf("(%s%03lx,%03lx) ", d, list->start_block, list->length);
        list = list->next;
    }
    printf("\n");
}

void sort_block_list(struct sfs_block_list **plist)
{
    int sz = 1;
    int n;
    do {
        n = 0;
        struct sfs_block_list **plist1 = plist;
        while (*plist1 != NULL) {
            n = n + 1;
            struct sfs_block_list **plist2 = plist1;
            int i = 0;
            plist2 = plist1;
            while (i < sz && *plist2 != NULL) {
                i++;
                plist2 = &(*plist2)->next;
            }
            plist1 = conquer(plist1, plist2, sz);
        }
        sz = sz * 2;
    } while (n > 1);
}

void block_list_to_free_list(plist, first_block, total_blocks, free_last)
    struct sfs_block_list **plist;
    uint64_t first_block;
    uint64_t total_blocks;
    struct sfs_block_list **free_last;
{
    if (*plist == NULL) {
        uint64_t gap = total_blocks - first_block;
        struct sfs_block_list *item = malloc(sizeof(struct sfs_block_list));
        item->start_block = first_block;
        item->length = gap;
        item->delfile = NULL;
        item->next = NULL;
        *plist = item;
        return;
    }

    /* if gap before the first block, insert one block structure */
    struct sfs_block_list **pprev;
    struct sfs_block_list *curr;
    if (first_block < (*plist)->start_block) {
        struct sfs_block_list *b0 = malloc(sizeof(struct sfs_block_list));
        b0->start_block = first_block;
        b0->length = (*plist)->start_block - first_block;
        b0->next =  (*plist);
        b0->delfile = NULL;
        *plist = b0;
        pprev = &(*plist)->next;
    } else {
        pprev = plist;
    }
    curr = (*pprev)->next;

    while ((*pprev) != NULL) {
        uint64_t prev_end = (*pprev)->start_block + (*pprev)->length;
        uint64_t gap = (curr != 0 ? curr->start_block : total_blocks) - prev_end;
        if ((*pprev)->delfile == NULL) {
            if (gap == 0) {
                struct sfs_block_list *tmp = (*pprev);
                *pprev = tmp->next;
                if ((*pprev)->next == NULL) {
                    *free_last = NULL;
                }
                free(tmp);
            } else {
                (*pprev)->start_block += (*pprev)->length;
                (*pprev)->length = gap;
                if ((*pprev)->next == NULL) {
                    *free_last = *pprev;
                }
                pprev = &(*pprev)->next;
            }
        } else {
            if (gap > 0) {
                struct sfs_block_list *item = malloc(sizeof(struct sfs_block_list));
                item->start_block = (*pprev)->start_block + (*pprev)->length;
                item->length = gap;
                item->next = curr;
                item->delfile = NULL;
                (*pprev)->next = item;
                pprev = &item->next;
            } else {
                pprev = &(*pprev)->next;
            }
            if ((*pprev)->next == NULL) {
                *free_last = *pprev;
            }
        }
        if (curr != NULL) {
            curr = curr->next;
        }
    }
}

struct sfs_block_list *make_free_list(sfs, entry_list, free_last)
    struct sfs *sfs;
    struct sfs_entry *entry_list;
    struct sfs_block_list **free_last;
{
    struct sfs_super *super = sfs->super;
    struct sfs_block_list *block_list = block_list_from_entries(entry_list);
    print_block_list("not sorted: ", block_list);

    sort_block_list(&block_list);
    print_block_list("sorted:     ", block_list);

/*
    uint64_t data_size;
    uint64_t total_blocks;
    uint32_t rsvd_blocks;
*/
    uint64_t first_block = super->rsvd_blocks;  // !! includes the superblock
    uint64_t data_blocks = super->total_blocks;
    block_list_to_free_list(&block_list, first_block, data_blocks, free_last);
    print_block_list("free:     ", block_list);

    return block_list;
}

SFS *sfs_init(const char *filename)
{
    SFS *sfs = malloc(sizeof(SFS));
    sfs->file = fopen(filename, "r+");
    if (sfs->file == NULL) {
        perror("sfs_init error");
        fprintf(stderr, "sfs_init: file error \"%s\"\n", filename);
        return NULL;
    }
    sfs->super = sfs_read_super(sfs);
    if (sfs->super == NULL) {
        fprintf(stderr, "sfs_init: error reading the superblock\n");
        exit(7);
    }
    sfs->entry_list = sfs_read_entries(sfs);
    sfs->free_last = NULL;
    sfs->free_list = make_free_list(sfs, sfs->entry_list, &sfs->free_last);
    if (sfs->free_last == NULL) {
        fprintf(stderr, "sfs_init: error: free_last is null\n");
        exit(7);
    }
    return sfs;
}

void free_entry(struct sfs_entry *entry)
{
//    printf("freeing: %x\n", entry->type);
    switch (entry->type) {
    case SFS_ENTRY_VOL_ID:
        free(entry->data.volume_data->name);
        free(entry->data.volume_data);
        break;
    case SFS_ENTRY_DIR:
    case SFS_ENTRY_DIR_DEL:
        free(entry->data.dir_data->name);
        free(entry->data.dir_data);
        break;
    case SFS_ENTRY_FILE:
    case SFS_ENTRY_FILE_DEL:
        free(entry->data.file_data->name);
        free(entry->data.file_data);
        break;
    case SFS_ENTRY_UNUSABLE:
        free(entry->data.unusable_data);
        break;
    default:
        break;
    }
    free(entry);
}

void free_entry_list(struct sfs_entry *entry_list)
{
    struct sfs_entry *prev = entry_list;
    struct sfs_entry *curr = prev->next;
    while (curr != NULL) {
        free_entry(prev);
        prev = curr;
        curr = curr->next;
    }
    free_entry(prev);
}

void free_free_list(struct sfs_block_list *free_list)
{
    struct sfs_block_list *prev = free_list;
    struct sfs_block_list *curr = prev->next;
    while (curr != NULL) {
        free(prev);
        prev = curr;
        curr = curr->next;
    }
    free(prev);
}

int sfs_terminate(SFS *sfs)
{
    free_entry_list(sfs->entry_list);
    free_free_list(sfs->free_list);
    free(sfs->super);
    fclose(sfs->file);
    free(sfs);
    return 0;
}

char *fix_name(const char *path)
{
    char *result = (char *)path;
    while (*result == '/')
        result++;
    return result;
}

uint64_t sfs_get_file_size(SFS *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    struct sfs_entry *entry = sfs->entry_list;
    while (entry != NULL) {
        if (entry->type == SFS_ENTRY_FILE
                && strcmp(fxpath, entry->data.file_data->name) == 0) {
            return entry->data.file_data->file_len;
        }
        entry = entry->next;
    }
    return 0;
}

// do not return deleted files and directories
struct sfs_entry *get_entry_by_name(SFS *sfs, char *path) {
    struct sfs_entry *entry = sfs->entry_list;
    while (entry != NULL) {
        switch (entry->type) {
        case SFS_ENTRY_DIR:
            if (strcmp(path, entry->data.dir_data->name) == 0) {
                return entry;
            }
            break;
        case SFS_ENTRY_FILE:
            if (strcmp(path, entry->data.file_data->name) == 0) {
                return entry;
            }
            break;
        }
        entry = entry->next;
    }
    return NULL;
}

struct sfs_entry *get_dir_by_name(SFS *sfs, char *path) {
    struct sfs_entry *entry = sfs->entry_list;
    while (entry != NULL) {
        if (entry->type == SFS_ENTRY_DIR
                && strcmp(path, entry->data.dir_data->name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

struct sfs_entry *get_file_by_name(SFS *sfs, char *path) {
    struct sfs_entry *entry = sfs->entry_list;
    while (entry != NULL) {
        if (entry->type == SFS_ENTRY_FILE
                && strcmp(path, entry->data.file_data->name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

int sfs_is_dir(SFS *sfs, const char *path)
{
    char *fxpath = fix_name(path);
//    printf("@@@@\tsfs_is_dir: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_dir_by_name(sfs, fxpath);
    if (entry != NULL) {
        return 1;
    } else {
        return 0;
    }
}

int sfs_is_file(SFS *sfs, const char *path)
{
    char *fxpath = fix_name(path);
//    printf("@@@@\tsfs_is_file: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_file_by_name(sfs, fxpath);
    if (entry != NULL) {
        return 1;
    } else {
        return 0;
    }
}

struct sfs_entry *find_entry_from(struct sfs_entry *entry, char *path)
{
    int len = strlen(path);
    while (entry != NULL) {
        if (entry->type == SFS_ENTRY_DIR) {
            char *name = entry->data.dir_data->name;
            int name_len = strlen(name);
            if ((len == 0 || (name_len > len + 1 && name[len] == '/'))
                    && strncmp(path, name, len) == 0 && strchr(&name[len+1], '/') == NULL) {
                return entry;
            }
        } else if (entry->type == SFS_ENTRY_FILE) {
            char *name = entry->data.file_data->name;
            int name_len = strlen(name);
            if ((len == 0 || (name_len > len + 1 && name[len] == '/'))
                    && strncmp(path, name, len) == 0 && strchr(&name[len+1], '/') == NULL) {
                return entry;
            }
        }
        entry = entry->next;
    }
    return NULL;
}

char *get_basename(char *full_name)
{
    char *p = full_name;
    int i = 0;
    int last_slash = -1;
    while (p[i] != 0) {
        if (p[i] == '/') {
            last_slash = i;
        }
        i = i + 1;
    }
    return &p[last_slash + 1];
}

char *get_entry_basename(struct sfs_entry *entry)
{
    switch (entry->type) {
    case SFS_ENTRY_DIR:
    case SFS_ENTRY_DIR_DEL:
        return get_basename((char*)entry->data.dir_data->name);
    case SFS_ENTRY_FILE:
    case SFS_ENTRY_FILE_DEL:
        return get_basename((char*)entry->data.file_data->name);
    default:
        return NULL;
    }
}

char *sfs_first(SFS *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    struct sfs_entry *entry = find_entry_from(sfs->entry_list, fxpath);
    if (entry != NULL) {
        sfs->iter_curr = entry->next;
        return get_entry_basename(entry);
    }
    sfs->iter_curr = NULL;
    return NULL;
}

char *sfs_next(SFS *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    struct sfs_entry *entry = find_entry_from(sfs->iter_curr, fxpath);
    if (entry != NULL) {
        sfs->iter_curr = entry->next;
        return get_entry_basename(entry);
    }
    sfs->iter_curr = NULL;
    return NULL;
}

int sfs_read(SFS *sfs, const char *path, char *buf, size_t size, off_t offset)
{
    char *fxpath = fix_name(path);
//    printf("@@@@\tsfs_read: path=\"%s\", size:0x%lx, offset:0x%lx\n", fxpath, size, offset);
    struct sfs_entry *entry = get_file_by_name(sfs, fxpath);
    if (entry != NULL) {
        uint64_t sz;		// number of bytes to be read
        uint64_t len = entry->data.file_data->file_len;
        if ((uint64_t)offset > len) {
            return 0;
        }
        if (offset + size > len) {
            sz = len - offset;
        } else {
            sz = size;
        }
        uint64_t data_offset = sfs->block_size * entry->data.file_data->start_block;
        uint64_t read_from = data_offset + offset;
        fseek(sfs->file, read_from, SEEK_SET);
        fread(buf, sz, 1, sfs->file);
        return sz;
    } else {
        return -1;
    }
}

int get_num_cont(struct sfs_entry *entry)
{
    switch (entry->type) {
    case SFS_ENTRY_DIR:
    case SFS_ENTRY_DIR_DEL:
        return entry->data.dir_data->num_cont;
    case SFS_ENTRY_FILE:
    case SFS_ENTRY_FILE_DEL:
        return entry->data.file_data->num_cont;
    default:
        return 0;
    }
}

int get_entry_usable_space(struct sfs_entry *entry)
{
    switch (entry->type) {
    case SFS_ENTRY_DIR_DEL:
        return 1 + entry->data.dir_data->num_cont;
    case SFS_ENTRY_FILE_DEL:
        return 1 + entry->data.file_data->num_cont;
    case SFS_ENTRY_UNUSED:
        return 1;
    default:
        return 0;
    }
}

void write_volume_data(char *buf, struct sfs_vol *vol_data)
{
    memcpy(&buf[4], &vol_data->time_stamp, 8);
    strncpy(&buf[12], vol_data->name, SFS_VOL_NAME_LEN);
}

void write_dir_data(char *buf, struct sfs_dir *dir_data)
{
    memcpy(&buf[2], &dir_data->num_cont, 1);
    memcpy(&buf[3], &dir_data->time_stamp, 8);
    uint64_t max_len = SFS_DIR_NAME_LEN + SFS_ENTRY_SIZE * dir_data->num_cont;
    strncpy(&buf[11], dir_data->name, max_len);
}

void write_file_data(char *buf, struct sfs_file *file_data)
{
    memcpy(&buf[2], &file_data->num_cont, 1);
    memcpy(&buf[3], &file_data->time_stamp, 8);
    memcpy(&buf[11], &file_data->start_block, 8);
    memcpy(&buf[19], &file_data->end_block, 8);
    memcpy(&buf[27], &file_data->file_len, 8);
    uint64_t max_len = SFS_FILE_NAME_LEN + SFS_ENTRY_SIZE * file_data->num_cont;
    strncpy(&buf[35], file_data->name, max_len);
}

void write_unusable_data(char *buf, struct sfs_unusable *unusable_data)
{
    memcpy(&buf[10], &unusable_data->start_block, 8);
    memcpy(&buf[18], &unusable_data->end_block, 8);
}


/* Writes the entry (with its continuations) to the Index Area.
 * Returns 0 on success and -1 on error.
 */
int write_entry(SFS *sfs, struct sfs_entry *entry)
{
    printf("=== WRITING ENTRY ===\n");
    int num_cont = get_num_cont(entry);
    int size = (1 + num_cont) * SFS_ENTRY_SIZE;
    char buf[size];
    memset(buf, 0, size);
    buf[0] = entry->type;
    buf[1] = 0;
    switch (entry->type) {
    case SFS_ENTRY_VOL_ID:
        write_volume_data(buf, entry->data.volume_data);
        break;
    case SFS_ENTRY_DIR:
    case SFS_ENTRY_DIR_DEL:
        write_dir_data(buf, entry->data.dir_data);
        break;
    case SFS_ENTRY_FILE:
    case SFS_ENTRY_FILE_DEL:
        write_file_data(buf, entry->data.file_data);
        break;
    case SFS_ENTRY_UNUSABLE:
        write_unusable_data(buf, entry->data.unusable_data);
        break;
    case SFS_ENTRY_START:
    case SFS_ENTRY_UNUSED:
        break;
    default:
        fprintf(stderr, "write_entry error: unknown entry type: 0x%02x\n", entry->type);
        printf("=== WRITING ENTRY: ERROR ===\n");
        return -1;
    }
    int sum = 0;
    for (int i = 0; i < size; ++i) {
        sum += buf[i];
    }
    buf[1] = 0x100 - sum % 0x100;

    printf("writing %d bytes at 0x%06lx\n", size, entry->offset);
    if (fseek(sfs->file, entry->offset, SEEK_SET) == -1) {
        fprintf(stderr, "write_entry error: couldn't fseek to %06lx\n", entry->offset);
        printf("=== WRITING ENTRY: ERROR ===\n");
        return -1;
    }
    int tmp;
    if ((tmp=fwrite(buf, size, 1, sfs->file)) != 1) {
        fprintf(stderr, "write_entry error: written %d bytes instead of %d bytes\n", tmp, size);
        printf("=== WRITING ENTRY: ERROR ===\n");
        return -1;
    }
    printf("=== WRITING ENTRY: OK ===\n");
    return 0;
}

struct sfs_block_list **find_delfile(pfree_list, delfile)
    struct sfs_block_list **pfree_list;
    struct sfs_entry *delfile;
{
    struct sfs_block_list **pfree = pfree_list;
    while (*pfree != NULL && (*pfree)->delfile != delfile) {
        pfree = &(*pfree)->next;
    }
    return pfree;
}

void delete_entries(pfree_list, from, to)
    struct sfs_block_list **pfree_list;
    struct sfs_entry *from;
    struct sfs_entry *to;
{
    struct sfs_block_list **p_free_item = pfree_list;
    struct sfs_entry *p_entry = from;
    while (p_entry != to) {
        if (p_entry->type == SFS_ENTRY_FILE_DEL) {
            p_free_item = find_delfile(p_free_item, p_entry); //if not exist => error
            struct sfs_block_list *item_to_delete = *p_free_item;
            *p_free_item = (*p_free_item)->next;
            free(item_to_delete);
        }
        struct sfs_entry *tmp = p_entry;
        p_entry = p_entry->next;
        free_entry(tmp);
    }
}

/* Insert n unused entries before tail and returns the head */
struct sfs_entry *insert_unused(sfs, offset, n, tail)
    struct sfs *sfs;
    uint64_t offset;
    int n;
    struct sfs_entry *tail;
{
    struct sfs_entry *entry;
    struct sfs_entry *next = tail;
    for (int i = 0; i < n; ++i) {
        entry = malloc(sizeof(struct sfs_entry));
        entry->offset = offset + SFS_ENTRY_SIZE * (n - i - 1);
        entry->type = SFS_ENTRY_UNUSED;
        entry->next = next;
        if (write_entry(sfs, entry) != 0) {
            return NULL;
        }
        next = entry;
    }
    return next;
}

/* Finds space for the entry and inserts it.
 * Writes changes to the Index Area.
 * Return 0 on success, -1 on error
 *
 * k: amount of space needed
 * l: amount of consecutive space found so that (l >= k)
 * The entries taking l space are deleted.
 * The new entry is inserted.
 * The remaining (l - k) is filled with unused entries.
 * If the space could not be found, -1 is returned, otherwise 0 is returned.
 */
int insert_entry(struct sfs *sfs, struct sfs_entry *new_entry)
{
    printf("=== INSERT ENTRY ===\n");
    int space_needed = 1 + get_num_cont(new_entry); // in number of simple entries
    printf("\tneeded: %d\n", space_needed);
    int space_found = 0;
    struct sfs_entry **p_entry = &sfs->entry_list;
    struct sfs_entry **pfirst_usable = NULL;
    while (*p_entry != NULL) {
//        printf("\ttype=0x%02x\n", curr_entry->type);
        int usable_space = get_entry_usable_space(*p_entry);
//        printf("\tusable: %d\n", usable_space);
        if (usable_space > 0) {
            if (pfirst_usable == NULL) {
                pfirst_usable = p_entry;
                space_found += usable_space;
                printf("\tfound: %d\n", space_found);
            }
            if (space_found >= space_needed) {
                int start = (*pfirst_usable)->offset;
                int end = start + SFS_ENTRY_SIZE * space_needed;
                struct sfs_entry *next = (*p_entry)->next;
                delete_entries(&sfs->free_list, *pfirst_usable, next);
                new_entry->offset = start;
                int l = space_found - space_needed;
                new_entry->next = insert_unused(sfs, end, l, next);
                *pfirst_usable = new_entry;
                printf("=== INSERT ENTRY: OK ===\n");
                if (write_entry(sfs, new_entry) != 0) {
                    return -1;
                } else {
                    return 0;
                }
            }
        } else if (pfirst_usable != NULL) {
            pfirst_usable = NULL;
            space_found = 0;
        }
        p_entry = &(*p_entry)->next;
    }
    printf("insert_entry: couldn't find (%d * 64) bytes\n", space_needed);
    printf("=== INSERT ENTRY: ERROR ===\n");
    return -1;
}

/* Prepends the entry to the list of entries.
 * The entry is inserted after the start marker.
 * In the Index Area, the start marker is moved in the direction of the superblock.
 * The entry is written to the Index Area after the start marker.
 * On success 0 is returned, -1 on error.
 */
int prepend_entry(struct sfs *sfs, struct sfs_entry *entry)
{
    printf("=== PREPEND ENTRY ===\n");
    struct sfs_entry *start = sfs->entry_list;
    uint64_t entry_size = SFS_ENTRY_SIZE * (1 + get_num_cont(entry));
    uint64_t start_size = SFS_ENTRY_SIZE * (1 + get_num_cont(start));

    /* check available space in the free area and update free list */
    if (sfs->free_last == NULL) {
        printf("free_last is NULL!!!\n");
        return -1;
    }

    if (sfs->free_last != NULL  && sfs->free_last->length >= entry_size) {
        uint64_t new_isz = sfs->super->index_size + entry_size;
        uint64_t iblk_rest = (sfs->block_size - sfs->super->index_size) % sfs->block_size;
        uint64_t ibt = sfs->super->index_size + iblk_rest; // index with rest in bytes
        uint64_t fbt = sfs->free_last->length * sfs->block_size; // free blocks in bytes
        printf("\tblock size: 0x%06x\n", sfs->block_size);
        printf("\toriginal index size: 0x%06lx\n", sfs->super->index_size);
        printf("\toriginal free blocks: 0x%06lx\n", sfs->free_last->length);
        printf("\tindex blocks (bytes): 0x%06lx\n", ibt);
        printf("\tfree blocks (bytes): 0x%06lx\n", fbt);
        printf("\tentry size: 0x%06lx\n", entry_size);
        printf("\tnew index size: 0x%06lx\n", new_isz);
        if (new_isz > ibt) {
            // update free list
            if (new_isz - ibt > fbt) {
                fprintf(stderr, "Error: could not prepend entry: no more free space\n");
                return -1;
            }
            sfs->free_last->length -= (new_isz - ibt + sfs->block_size - 1) / sfs->block_size;
            printf("\tupdate free_last: 0x%06lx\n", sfs->free_last->length);
        }
        sfs->super->index_size = new_isz;
        printf("\tupdate index size: 0x%06lx\n", new_isz);
        sfs_write_super(sfs->file, sfs->super);
    } else {
        fprintf(stderr, "prepend_entry: free list error\n");
        return -1;
    }

    start->type = SFS_ENTRY_START;
    start->offset -= entry_size;
    entry->offset = start->offset + start_size;
    if (write_entry(sfs, entry) == -1) {
        fprintf(stderr, "prepend_entry: write new entry error\n");
        return -1;
    }
    if (write_entry(sfs, start) == -1) {
        fprintf(stderr, "prepend_entry: write start marker entry error\n");
        return -1;
    }
    // update pointers only if write successful
    entry->next = start->next;
    start->next = entry;
    printf("=== PREPEND: OK! ===\n");
    return 0;
}

/* Puts a new entry into the entry list, updating the index area:
 * -> finds a place in the list with needed number of continuations
 * -> writes changes to the index area
 * -> updates free list if deleted files overwritten
 * -> on success returns 0
 *    otherwise returns -1
 */
int put_new_entry(struct sfs *sfs, struct sfs_entry *new_entry)
{
    if (insert_entry(sfs, new_entry) != 0) {
        return prepend_entry(sfs, new_entry);
    }
    return 0;
}

// check if path valid and does not exist
int check_valid_new(struct sfs *sfs, char *path)
{
    printf("check valid as new \"%s\"s:", path);
    // if path exists, not valid as a new name
    struct sfs_entry *entry = get_dir_by_name(sfs, path);
    if (entry != NULL) {
        printf(" no (already exists)\n");
        return 0;
    }

    int path_len = strlen(path);
    char *basename = get_basename(path);
    int basename_len = strlen(basename);
    if (basename_len == 0) {
        printf(" empty basename\n");
        return 0;
    }

    /* check if prent dir exists */
    if (path_len > basename_len) {
        char parent[path_len];
        memcpy(parent, path, path_len);
        parent[path_len - basename_len - 1] = '\0';
        struct sfs_entry *parent_entry = get_dir_by_name(sfs, parent);
        if (parent_entry == NULL) {
            printf(" no (parent \"%s\" does exists)\n", parent);
            return 0;
        }
        printf(" parent=\"%s\"", parent);
    }
    printf(" basename=\"%s\"\n", basename);

    return 1;
}

int sfs_mkdir(struct sfs *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    int path_len = strlen(fxpath);
    printf("@@@\tsfs_mkdir: create new directory \"%s\"\n", fxpath);
    if (!check_valid_new(sfs, fxpath)) {
        return -1;
    }

    struct sfs_entry *dir_entry = malloc(sizeof(struct sfs_entry));
    dir_entry->type = SFS_ENTRY_DIR;
    int num_cont;
    if (path_len < SFS_DIR_NAME_LEN) {
        num_cont = 0;
    } else {
        int cont_str_len = path_len - SFS_DIR_NAME_LEN + 1;
        num_cont = (cont_str_len + SFS_ENTRY_SIZE - 1) / SFS_ENTRY_SIZE;
    }
    dir_entry->data.dir_data = malloc(sizeof(struct sfs_dir));
    dir_entry->data.dir_data->num_cont = num_cont;
    dir_entry->data.dir_data->time_stamp = sfs_make_time_stamp();
    dir_entry->data.dir_data->name = strdup(fxpath);

    if (put_new_entry(sfs, dir_entry) == -1) {
        printf("\tsfs_mkdir put new entry error\n");
        free_entry(dir_entry);
    }

    return 0;
}

int sfs_create(struct sfs *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    int path_len = strlen(fxpath);
    printf("@@@\tsfs_create: create new empty file \"%s\"\n", fxpath);
    if (!check_valid_new(sfs, fxpath)) {
        return -1;
    }

    struct sfs_entry *file_entry = malloc(sizeof(struct sfs_entry));
    file_entry->type = SFS_ENTRY_FILE;
    int num_cont;
    if (path_len < SFS_FILE_NAME_LEN) {
        num_cont = 0;
    } else {
        int cont_str_len = path_len - SFS_FILE_NAME_LEN + 1;
        num_cont = (cont_str_len + SFS_ENTRY_SIZE - 1) / SFS_ENTRY_SIZE;
    }

    file_entry->data.file_data = malloc(sizeof(struct sfs_file));
    file_entry->data.file_data->num_cont = num_cont;
    file_entry->data.file_data->time_stamp = sfs_make_time_stamp();
    file_entry->data.file_data->start_block = sfs->super->rsvd_blocks;
    file_entry->data.file_data->end_block = sfs->super->rsvd_blocks - 1;
    file_entry->data.file_data->file_len = 0;
    file_entry->data.file_data->name = strdup(fxpath);

    if (put_new_entry(sfs, file_entry) == -1) {
        printf("\tsfs_file put new entry error\n");
        free_entry(file_entry);
    }

    return 0;
}

int sfs_is_dir_empty(struct sfs *sfs, char *path) {
    struct sfs_entry *entry = sfs->entry_list;
    int path_len = strlen(path);
    while (entry != NULL) {
        if ((entry->type == SFS_ENTRY_DIR
                && strncmp(path, entry->data.dir_data->name, path_len) == 0
                && entry->data.dir_data->name[path_len] == '/')
        || (entry->type == SFS_ENTRY_FILE
                && strncmp(path, entry->data.file_data->name, path_len) == 0
                && entry->data.file_data->name[path_len] == '/')) {
            return 0;
        }
        entry = entry->next;
    }
    return 1;
}

int sfs_rmdir(struct sfs *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_rmdir: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_dir_by_name(sfs, fxpath);
    if (entry == NULL) {
        fprintf(stderr, "no directory \"%s\" exists\n", fxpath);
        return -1;
    }
    if (!sfs_is_dir_empty(sfs, fxpath)) {
        fprintf(stderr, "directory \"%s\" is not empty\n", fxpath);
        return -1;
    }
    /* Deleted children: do not remove, unreachable
     * If the parent is deleted, the children cannot be restored
     * => on restore: check that the parent exists
     */
    entry->type = SFS_ENTRY_DIR_DEL;
    if (write_entry(sfs, entry) == 0) {
        printf("\trmdir(%s): ok\n", fxpath);
        return 0;
    } else {
        return -1;
    }
}

/* Insert a deleted file into the free list */
void free_list_insert(struct sfs *sfs, struct sfs_entry *delfile)
{
    struct sfs_block_list **p = &sfs->free_list;
    while ((*p) != NULL) {
        if ((*p)->start_block > delfile->data.file_data->start_block) {
            // insert before *p
            struct sfs_block_list *item = malloc(sizeof(struct sfs_block_list));
            item->start_block = delfile->data.file_data->start_block;
            item->length = 1 + get_num_cont(delfile);
            item->delfile = delfile;
            item->next = (*p);
            *p = item;
            break;
        }
        p = &(*p)->next;
    }
}

// assume that the entry can be deleted
void delete_entry(struct sfs *sfs, struct sfs_entry *entry)
{
    struct sfs_entry **p_entry = &sfs->entry_list;
    int entry_length = 1 + get_num_cont(entry);
    struct sfs_entry *tail = entry->next;
    while (*p_entry != NULL) {
        if (*p_entry == entry) {
            *p_entry = insert_unused(sfs, entry->offset, entry_length, tail);
            free_entry(entry);
            break;
        }
        p_entry = &(*p_entry)->next;
    }
}

int sfs_delete(struct sfs *sfs, const char *path)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_delete: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_file_by_name(sfs, fxpath);
    if (entry == NULL) {
        fprintf(stderr, "file \"%s\" does not exists\n", fxpath);
        return -1;
    }

    // do not insert empty files into the free list
    if (entry->data.file_data->file_len == 0) {
        delete_entry(sfs, entry);
        return 0;
    }

    entry->type = SFS_ENTRY_FILE_DEL;
    free_list_insert(sfs, entry);
    if (write_entry(sfs, entry) == 0) {
        printf("\tdelete(%s): ok\n", fxpath);
        return 0;
    } else {
        return -1;
    }
}

int sfs_get_sfs_time(SFS *sfs, struct timespec *timespec)
{
    sfs_fill_timespec(sfs->super->time_stamp, timespec);
    return 0;
}

int sfs_get_dir_time(SFS *sfs, const char *path, struct timespec *timespec)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_get_dir_time: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_dir_by_name(sfs, fxpath);
    if (entry == NULL) {
        fprintf(stderr, "directory \"%s\" does not exists\n", fxpath);
        return -1;
    }
    sfs_fill_timespec(entry->data.dir_data->time_stamp, timespec);
    return 0;
}

int sfs_get_file_time(SFS *sfs, const char *path, struct timespec *timespec)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_get_file_time: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_file_by_name(sfs, fxpath);
    if (entry == NULL) {
        fprintf(stderr, "file \"%s\" does not exists\n", fxpath);
        return -1;
    }
    sfs_fill_timespec(entry->data.file_data->time_stamp, timespec);
    return 0;
}

int sfs_set_time(SFS *sfs, const char *path, struct timespec *timespec)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_set_time: name=\"%s\"\n", fxpath);
    struct sfs_entry *entry = get_entry_by_name(sfs, fxpath);
    if (entry == NULL) {
        fprintf(stderr, "File or directory\"%s\" does not exists\n", fxpath);
        return -1;
    }
    uint64_t time_stamp = timespec_to_time_stamp(timespec);
    switch (entry->type) {
    case SFS_ENTRY_DIR:
        entry->data.dir_data->time_stamp = time_stamp;
        break;
    case SFS_ENTRY_FILE:
        entry->data.file_data->time_stamp = time_stamp;
        break;
    }
    return write_entry(sfs, entry);
}

void rename_entry(struct sfs_entry *entry, char *name)
{
    switch (entry->type) {
    case SFS_ENTRY_DIR:
        free(entry->data.dir_data->name);
        entry->data.dir_data->name = name;
        break;
    case SFS_ENTRY_FILE:
        free(entry->data.file_data->name);
        entry->data.file_data->name = name;
        break;
    }
}

/* Assume: there is no entry with name dest_path.
 * Rename entry <source_path> to <dest_path>.
 * Replace in every file and directory entry that starts with <source_path>/
 * <source_path> with <dest_path>.  Write everything to index area.
 * Return 0 on success and -1 on error.
 */
int move_dir(sfs, source_path, dest_path)
    struct sfs *sfs;
    char *source_path;
    char *dest_path;
{
    struct sfs_entry *entry = sfs->entry_list;
    int src_len = strlen(source_path);
    int dest_len = strlen(dest_path);
    while (entry != NULL) {
        char *name = NULL;
        switch (entry->type) {
        case SFS_ENTRY_DIR:
            name = entry->data.dir_data->name;
            break;
        case SFS_ENTRY_FILE:
            name = entry->data.file_data->name;
            break;
        }
        if (name != NULL) {
            int name_len = strlen(name);
            if (name_len >= src_len && (name_len == src_len || name[src_len] == '/')
                    && strncmp(source_path, name, src_len) == 0) {
                char *new_name = malloc(dest_len + name_len - src_len + 1);
                strncpy(new_name, dest_path, dest_len);
                strncpy(&new_name[dest_len], &name[src_len], name_len - src_len + 1);
                rename_entry(entry, new_name);
                if (write_entry(sfs, entry) == -1) {
                    return -1;
                }
            }
        }
        entry = entry->next;
    }
    return 0;
}

int sfs_rename(sfs, source_path, dest_path, replace)
    SFS *sfs;
    const char *source_path;
    const char *dest_path;
    int replace;
{
    char *fx_source = fix_name(source_path);
    char *fx_dest = fix_name(dest_path);
    printf("@@@@\tsfs_rename: \"%s\"->\"%s\"\n", fx_source, fx_dest);
    if (strcmp(fx_source, fx_dest) == 0) {
        return 0;
    }
    struct sfs_entry *entry = get_entry_by_name(sfs, fx_source);
    if (entry == NULL) {
        fprintf(stderr, "Source \"%s\" does not exist\n", fx_source);
        return -1;
    }
    if (!check_valid_new(sfs, fx_dest)) {
        fprintf(stderr, "Destination name not valid\n");
        return -1;
    }
    struct sfs_entry *dest_entry = get_entry_by_name(sfs, fx_dest);
    if (dest_entry != NULL) {
        if (replace == 0) {
            fprintf(stderr, "Cannot replace existing file \"%s\"\n", fx_dest);
            return -1;
        } else {
            if (entry->type != dest_entry->type) {
                fprintf(stderr, "Source and destination of different types\n");
                return -1;
            }
            if (dest_entry->type == SFS_ENTRY_DIR) {
                // check if directory is empty
                if (!sfs_is_dir_empty(sfs, fx_dest)) {
                    fprintf(stderr, "directory \"%s\" is not empty\n", fx_dest);
                    return -1;
                }
            } 
            // delete entry from entry list
            delete_entry(sfs, dest_entry);
        }
    }
    if (entry->type == SFS_ENTRY_DIR) {
        if (move_dir(sfs, fx_source, fx_dest) != 0) {
            return -1;
        }
    } else if (entry->type == SFS_ENTRY_FILE) {
        rename_entry(entry, strdup(fx_dest));
        if (write_entry(sfs, entry) != 0) {
            return -1;
        }
    }
    return 0;
}

int sfs_write(SFS *sfs, const char *path, const char *buf, size_t size, off_t offset)
{
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_write: path=\"%s\", size:0x%lx, offset:0x%lx\n", fxpath, size, offset);
    struct sfs_entry *entry = get_file_by_name(sfs, fxpath);
    if (entry != NULL) {
        uint64_t sz;		// number of bytes to write
        uint64_t len = entry->data.file_data->file_len;
        printf("\toffset=0x%06lx\n", offset);
        printf("\tlen=0x%06lx\n", len);
        if ((uint64_t)offset > len) {
            return 0;
        }
        if (offset + size > len) {
            sz = len - offset;
        } else {
            sz = size;
        }
        uint64_t data_offset = sfs->block_size * entry->data.file_data->start_block;
        uint64_t write_start = data_offset + offset;
        printf("\tdata_offset=0x%06lx\n", data_offset);
        printf("\twrite_start=0x%06lx\n", write_start);
        if (fseek(sfs->file, write_start, SEEK_SET) != 0) {
            fprintf(stderr, "f!! seek error\n");
            return -1;
        }
        if (fwrite(buf, sz, 1, sfs->file) != 1) {
            fprintf(stderr, "!! fwrite error\n");
            return -1;
        }
        return sz;
    } else {
        fprintf(stderr, "!! no file error\n");
        return -1;
    }
}

struct sfs_block_list **free_list_find(sfs, start_block, length)
    SFS *sfs;
    uint64_t start_block;
    uint64_t length;
{
    struct sfs_block_list **p = &sfs->free_list;
    struct sfs_block_list **pfirst = p;
    uint64_t tot = 0;
    uint64_t next = 0;
    while (*p != NULL && tot < length) {
        if(next != (*p)->start_block) {
            pfirst = p;
            tot = 0;
        }
        tot += (*p)->length;
        next = (*p)->start_block + (*p)->length;
        p = &(*p)->next;
    }
    if (tot >= length) {
        return pfirst;
    }
    return NULL;
}

int free_list_add(SFS *sfs, uint64_t start, uint64_t len)
{
    struct sfs_block_list **p = free_list_find(sfs, start, len);
    if (*p == NULL) {
        return -1;
    }
    if ((*p)->delfile == NULL && (*p)->start_block == start + len) {
        (*p)->start_block = start;
        (*p)->length += len;
    } else {
        struct sfs_block_list *item = malloc(sizeof(struct sfs_block_list));
        item->start_block = start;
        item->length = len;
        item->delfile = NULL;
        item->next = (*p);
        *p = item;
    }
    return 0;
}

int free_list_del(SFS *sfs, struct sfs_block_list **p_from, uint64_t length)
{
    uint64_t rest = length;
    struct sfs_block_list **p = p_from;
    while (*p != NULL && (*p)->length <= rest) {
        struct sfs_block_list *tmp = (*p);
        rest -= (*p)->length;
        *p = (*p)->next;
        if (tmp->delfile != NULL) {
            delete_entries(&sfs->free_list, tmp->delfile, tmp->delfile->next);
        }
        free(tmp);
    }
    if (*p == NULL) {
        return -1;
    }
    (*p)->length -= rest;
    (*p)->start_block += rest;
    return 0;
}

/****f*
 *  NAME
 *    sfs_resize -- resize a file
 *  DESCRIPTION
 *    Resize a file *path* to size *len*.  Truncate the file if its size
 *    is less than *len*.  Otherwise, fills with null characters.
 *  PARAMETERS
 *    sfs - the SFS variable
 *    path - the absolute path of the file
 *    len - the new size for the file
 *  RETURN VALUE
 *    Returns 0 on success and -1 on error.
 ****
 * Pseudocode:
 *   l0 - initial file size
 *   l1 - final file size
 *   b0 - initial number of blocks used by the file
 *   b1 - number of block needed for its new size
 *   s0 - the first block of the file
 *   file_entry - file entry in the Index Area
 *
 *   if b1 > b0 then    // file is to small
 *     p_next = free_list_find(sfs, s0 + l0, b1 - b0)
 *     if next <> null then     // enough space right after the file
 *       free_list_del(sfs, p_next, b1 - b0)
 *     else                     // not enough space: find some blocks
 *       p_blocks = free_list_find(sfs, 0, b1)
 *       copy the file contents: b0*bs bytes from s0 to start of p_blocks
 *     end if
 *     set file_entry start: s0
 *   else if b0 > b1    // file is to big => free b0-b1 blocks after the file
 *     free_list_add(sfs, s0 + b0, b0 - b1)
 *   end if
 *   if l1 > l0 then
 *     fill l1 - l0 bytes after the file contents with '\0'
 *   end if
 *   write_entry(sfs, file_entry)
 */
int sfs_resize(SFS *sfs, const char *path, off_t len)
{
    const uint64_t bs = sfs->block_size;
    char *fxpath = fix_name(path);
    printf("@@@@\tsfs_resize: name=\"%s\" length=%ld\n", fxpath, len);
    struct sfs_entry *file_entry = get_file_by_name(sfs, fxpath);
    if (file_entry == NULL) {
        fprintf(stderr, "file \"%s\" does not exists\n", fxpath);
        return -1;
    }
    if (file_entry->type != SFS_ENTRY_FILE) {
        fprintf(stderr, "\"%s\" is not a file\n", fxpath);
        return -1;
    }
    const uint64_t l0 = file_entry->data.file_data->file_len;
    const uint64_t l1 = (uint64_t)len;
    const uint64_t b0 = (l0 + bs - 1) / bs;
    const uint64_t b1 = (len + bs - 1) / bs;
    const uint64_t s0 = file_entry->data.file_data->start_block;
    if (b1 > b0) {
        struct sfs_block_list **p_next = free_list_find(sfs, s0 + l0, b1 - b0);
        if (*p_next != NULL) {
            free_list_del(sfs, p_next, b1 - b0);
        } else {
            struct sfs_block_list **p_blocks = free_list_find(sfs, 0, b1);
            if (*p_blocks == NULL) {
                return -1;
            }
            if (free_list_del(sfs, p_blocks, b1) != 0) {
                return -1;
            }
            if (free_list_add(sfs, s0, b0) != 0) {
                return -1;
            }
            for (uint64_t i = 0; i < b0; ++i) {
                char buf[bs];
                fseek(sfs->file, (file_entry->data.file_data->file_len + i) * bs, SEEK_SET);
                fread(buf, bs, 1, sfs->file);
                fseek(sfs->file, ((*p_blocks)->start_block + i) * bs, SEEK_SET);
                fwrite(buf, bs, 1, sfs->file);
            }
            file_entry->data.file_data->start_block = (*p_blocks)->start_block;
        }
    } else if (b0 > b1) {
        if (free_list_add(sfs, s0 + b0, b0 - b1)) {
            return -1;
        }
    }
    if (l1 > l0) {
        char c = '\0';
        fseek(sfs->file, (s0 + len) * bs, SEEK_SET);
        fwrite(&c, 1, l1 - l0, sfs->file);
    }
    file_entry->data.file_data->file_len = l1;
    if (write_entry(sfs, file_entry) != 0) {
        return -1;
    }
    return 0;
}
