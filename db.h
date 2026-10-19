#pragma once
#include <stdint.h>

#define DB_MAGIC 0x4D594442 
#define DB_VERSION 1
#define DB_FILE "data.bin"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t record_count;
} DBHeader;

typedef struct {
    uint64_t id;
    char username[32];
    uint32_t balance;
} UserRecord;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(DBHeader) == 16, "DBHeader must stay 16 bytes");
_Static_assert(sizeof(UserRecord) == 48, "UserRecord must stay 48 bytes");
#endif
