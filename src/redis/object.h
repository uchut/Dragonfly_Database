#ifndef __REDIS_OBJECT_H
#define __REDIS_OBJECT_H

#include <stddef.h>

#include "dict.h"
#include "sds.h"
#include "quicklist.h"

/* The actual Redis Object */
#define OBJ_STRING 0U    /* String object. */
#define OBJ_LIST 1U      /* List object. */
#define OBJ_SET 2U       /* Set object. */
#define OBJ_ZSET 3U      /* Sorted set object. */
#define OBJ_HASH 4U      /* Hash object. */
#define OBJ_MODULE 5U    /* Module object. */
#define OBJ_STREAM 6U    /* Stream object. */

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define OBJ_ENCODING_RAW 0U     /* Raw representation */
#define OBJ_ENCODING_INT 1U     /* Encoded as integer */
#define OBJ_ENCODING_HT 2U      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3U  /* Encoded as zipmap */
#define OBJ_ENCODING_LINKEDLIST 4U /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5U /* Encoded as ziplist */
#define OBJ_ENCODING_INTSET 6U  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7U  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8U  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9U /* Encoded as linked list of ziplists */
#define OBJ_ENCODING_STREAM 10U /* Encoded as a radix tree of listpacks */
#define OBJ_ENCODING_LISTPACK 11 /* Encoded as a listpack */
#define OBJ_ENCODING_COMPRESS_INTERNAL 15U  /* Kept as lzf compressed, to pass compressed blob to another thread */

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */


#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2


#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_REFCOUNT INT_MAX     /* Global object never destroyed. */
#define OBJ_STATIC_REFCOUNT (INT_MAX-1) /* Object allocated in the stack. */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT


/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

typedef struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:24; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    int refcount;
    void *ptr;
} robj;


/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
int getLongLongFromObject(robj *o, long long *target);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStreamObject(void);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
unsigned long long estimateObjectIdleTime(const robj *o);
uint8_t LFUDecrAndReturn(time_t epoch_sec, const robj *o);
void listTypeConvert(robj *subject, int enc);
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier);


robj *setTypeCreate(sds value);
int setTypeAdd(robj *subject, sds value);
int setTypeRemove(robj *subject, sds value);
int setTypeIsMember(const robj *subject, sds value);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(const robj *subject);
void setTypeConvert(robj *subject, int enc);


static inline int sdsEncodedObject(const robj *o) {
    return o->encoding == OBJ_ENCODING_RAW || o->encoding == OBJ_ENCODING_EMBSTR;
}

/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator */
    dictIterator *di;
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    robj *subject;
    int encoding;

    unsigned char *fptr, *vptr;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction */
    quicklistIter *iter;
} listTypeIterator;

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;
    quicklistEntry entry; /* Entry in quicklist */
} listTypeEntry;

const char *strEncoding(int encoding);


#define serverAssertWithInfo(x, y, z) serverAssert(z)


#endif
