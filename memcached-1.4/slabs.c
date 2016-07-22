/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

//#define DEBUG_SLAB_MOVER
/* powers-of-N allocation structures */

typedef struct {  //可以参考slabs_init
	//slab分配器分配的item的大小，包括item结构头部
    unsigned int size;      /* sizes of items */
	//每个slab分配器能分配多少个item
    unsigned int perslab;   /* how many items per slab */

	//指向空闲item链表
    void *slots;           /* list of item ptrs */
	//空闲item的个数  每取走一个item减1，见do_slabs_alloc
    unsigned int sl_curr;   /* total free items in list */

	//这个是已经分配了内存的slabs个数。list_size是这个slab数组(slab_list)的大小
	//表示有多少个该类型的slab，可以参考http://blog.csdn.net/yxnyxnyxnyxnyxn/article/details/7869900
    unsigned int slabs;     /* how many slabs were allocated for this class */

    //grow_slab_list中赋初值，数组指针      //grow_slab_list中创建空间和赋值
	//slab数组，数组的每一个元素就是一个slab分配器，这些分配器都分配相同尺寸的内存
	//用于管理多个trunk相同的slabs,例如chunk都是128的slab,已经用了3个chunk为128的slab，那么这3个slab就通过slab_list管理  见grow_slab_list
    void **slab_list;       /* array of slab pointers */ //例如多个1M大小chunk的slab，就通过该list管理

	//slab数组的大小，list_size >= slabs  grow_slab_list中赋初值
    unsigned int list_size; /* size of prev array */ //grow_slab_list中创建空间和赋值

	//本slabclass_t分配出去的总字节数 do_slabs_alloc   实际占用的chunk中的实际使用字节数，实际上要比chunk少，因为一般不会刚好存储key-value长度刚好为chunk
    size_t requested; /* The number of requested bytes */
} slabclass_t;


static slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES]; //不同大小的chunk块内存页保存到对应的数组中
//用户设置的内存最大限制 也就是settings.maxbytes
static size_t mem_limit = 0;
static size_t mem_malloced = 0;
/* If the memory limit has been hit once. Used as a hint to decide when to
 * early-wake the LRU maintenance thread */
static bool mem_limit_reached = false;
static int power_largest; //chunk最大的item对应的slabclass[]id号，也就是chunk等于settings.item_size_max的item

//如果程序要求预先分配内存，而不是到了需要的时候才分配内存，那么
//mem_base就是指向那块预先分配的内存
//mem_current指向还可以使用的内存的开始位置
//mem_avail指明还有多少内存可以使用  参考memory_allocate
static void *mem_base = NULL;  //如果不为NULL，则为已启动就把该memcached进程允许的最大使用空间一次性分配好，见slabs_init
//实际malloc的内存空间，见memory_allocate   mem_current指向还可以使用的内存的开始位置
static void *mem_current = NULL;
static size_t mem_avail = 0; //mem_avail指明还有多少内存可以使用

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Forward Declarations
 */
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST; //res的初始值为1

	//返回0表示查找失败，因为slabclass数组中，第一个元素是没有使用的
    if (size == 0)
        return 0;
	//因为slabclass数组中各个元素能分配的item大小是升序的
	//所以从小到大直接判断可在数组找到最小但又能满足的元素
    while (size > slabclass[res].size)
        if (res++ == power_largest)     /* won't fit in the biggest slab */
            return 0;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
 */ //http://xenojoshua.com/2011/04/deep-in-memcached-how-it-works/ 
 //参数factor是扩容因子，默认值是1.25
void slabs_init(const size_t limit, const double factor, const bool prealloc) {
    int i = POWER_SMALLEST - 1;
	//settings.chunk_size默认值为48，可以在启动memcached的时候通过-n选项设置
	//size由两部分组成: item结构体本身和这个item对应的数据
	//这里的数据也就是set、add命令中的那个数据，后面的循环可以看到这个size变量
	//会根据扩容因子factor慢慢扩大，所以能存储的数据长度也会变大的
    unsigned int size = sizeof(item) + settings.chunk_size;

	//用户设置或默认的内存大小限制
    mem_limit = limit;

	//用户要求预分配一块的内存，以后需要内存，就向这块内存申请
    if (prealloc) { //默认false
        /* Allocate everything in a big chunk with malloc */
        mem_base = malloc(mem_limit);
        if (mem_base != NULL) {
            mem_current = mem_base;
            mem_avail = mem_limit;
        } else {
            fprintf(stderr, "Warning: Failed to allocate requested memory in"
                    " one large chunk.\nWill allocate in smaller chunks\n");
        }
    }

	//初始化数组，这个操作很重要，数组中所有元素的成员变量都为0了
    memset(slabclass, 0, sizeof(slabclass));

	//slabclass数组中的第一个元素并不使用
	//settings.item_size_max是memecached支持的最大item尺寸，默认为1M
	//也就是网上所说的memcahced存储的数据最大为1MB
    while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1 && size <= settings.item_size_max / factor) {
        /* Make sure items are always n-byte aligned */
        if (size % CHUNK_ALIGN_BYTES) //8字节对齐
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

		//这个slabclass的slab分配器能分配的item的大小
        slabclass[i].size = size;
		//这个slabclass的slab分配器最多能分配多少个item(也决定了最多分配多少内存)
        slabclass[i].perslab = settings.item_size_max / slabclass[i].size;
		//扩容
        size *= factor;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, slabclass[i].size, slabclass[i].perslab);
        }
    }
	//最大的item
    power_largest = i;
    slabclass[power_largest].size = settings.item_size_max;
    slabclass[power_largest].perslab = 1;
    if (settings.verbose > 1) {
        fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                i, slabclass[i].size, slabclass[i].perslab);
    }

    /* for the test suite:  faking of how much we've already malloc'd */
    {
        char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
        if (t_initial_malloc) {
            mem_malloced = (size_t)atol(t_initial_malloc);
        }

    }
	//预先分配内存
    if (prealloc) {
        slabs_preallocate(power_largest);
    }
}

//参数值为使用到的slabclass数组元素个数
//为slabclass数组的每一个元素(使用到的元素)分配内存
static void slabs_preallocate (const unsigned int maxslabs) {
    int i;
    unsigned int prealloc = 0;

    /* pre-allocate a 1MB slab in every size class so people don't get
       confused by non-intuitive "SERVER_ERROR out of memory"
       messages.  this is the most common question on the mailing
       list.  if you really don't want this, you can rebuild without
       these three lines.  */

	//遍历slabclass数组
    for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
		//当然只是遍历使用了的数组元素
        if (++prealloc > maxslabs)
            return;
        if (do_slabs_newslab(i) == 0) {
			//为每一个slabclass_t分配一个内存页
			//如果分配失败，将退出程序。因为这个预分配的内存是后面程序运行的基础
			//如果这里分配失败了，后面的代码无从执行。所以直接退出程序。
            fprintf(stderr, "Error while preallocating slab memory!\n"
                "If using -L or other prealloc options, max memory must be "
                "at least %d megabytes.\n", power_largest);
            exit(1);
        }
    }

}

//增加slab_list成员指向的内存，也就是增大slab_list数组。使得可以有更多的slab分配器
//除非内存分配失败，否则都是返回-1，无论是否真正增大了
static int grow_slab_list (const unsigned int id) { //为该类型chunk块的slab多分配一个slab页
    slabclass_t *p = &slabclass[id];
    if (p->slabs == p->list_size) {//用完了之前申请到的slab_list数组的所有元素
        size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
        void *new_list = realloc(p->slab_list, new_size * sizeof(void *));
        if (new_list == 0) return 0;
        p->list_size = new_size; 
        p->slab_list = new_list; //新的slab页添加到slab_list头部，这样所有相同size的slab页就统一管理起来了
    }
    return 1;
}

//把一个slab，例如1M空间切割成perslab个chunk，通过next和prev指针链接在一起
static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    int x;
    for (x = 0; x < p->perslab; x++) {
        do_slabs_free(ptr, 0, id);
        ptr += p->size;
    }
}

/* Fast FIFO queue */
static void *get_page_from_global_pool(void) {
    slabclass_t *p = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    if (p->slabs < 1) {
        return NULL;
    }
    char *ret = p->slab_list[p->slabs - 1];
    p->slabs--;
    return ret;
}
//slabclass_t中slab的数目是慢慢增多的。该函数的作用是为slabclass_t申请多一个slab
//参数id指明是slabclass数组中的那个slabclass_t
static int do_slabs_newslab(const unsigned int id) {
    slabclass_t *p = &slabclass[id];
    slabclass_t *g = &slabclass[SLAB_GLOBAL_PAGE_POOL];
    int len = settings.slab_reassign ? settings.item_size_max
        : p->size * p->perslab;//setting.slab_rassingn的默认值为false，这里就采用false
    char *ptr;

	//mem_malloced的值通过环境变量设置，默认为0
    if ((mem_limit && mem_malloced + len > mem_limit && p->slabs > 0
         && g->slabs == 0)) {
        mem_limit_reached = true;
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

        (grow_slab_list(id) == 0) || //增长slab_list(失败返回0)。一般会成功，除非无法分配内存
        (((ptr = get_page_from_global_pool()) == NULL) &&
        ((ptr = memory_allocate((size_t)len)) == 0)) { //分配len字节内存(也就是一个页)

        MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
        return 0;
    }

    memset(ptr, 0, (size_t)len);//清空内存块是必须的
	//将这块内存切成一个个的item，当然item的大小由id所控制
    split_slab_page_into_freelist(ptr, id);

	//将分配得到的内存页交由slab_list掌管
    p->slab_list[p->slabs++] = ptr; //salb_list[0] = ptr,然后slabs++后为1，也就是[0]指向第一个item
    MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

    return 1;
}

/*@null@*/
//向slabclass申请一个item。在调用函数之前，已经调用slabs_clsid函数确定
//本次申请是向哪个slabclass_t申请item了，参数id就是指明是向哪个slabclass_t
//申请item。如果该slabclass_t是有空闲item，那么就从空闲的item队列分配一个
//如果没有空闲item，那么就申请一个内存页。再从新申请的页中分配一个item
// 返回值为得到的item，如果没有内存了，返回NULL
static void *do_slabs_alloc(const size_t size, unsigned int id, unsigned int *total_chunks,
        unsigned int flags) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > power_largest) {//下标越界
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }
    p = &slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);
	//如果p->sl_curr等于0，就说明该slabclass_t没有空闲的item了。
	//此时需要调用do_slabs_newslab申请一个内存页
    if (total_chunks != NULL) {
        *total_chunks = p->slabs * p->perslab;
    }
    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (p->sl_curr == 0 && flags != SLABS_ALLOC_NO_NEWPAGE) {
        do_slabs_newslab(id);
    }

    if (p->sl_curr != 0) {
        /* return off our freelist */
		//除非do_slabs_newslab调用失败，否则都会来到这里。无论一开始sl_curr是否为0.
		//p->slots指向第一个空闲的item，此时要把第一个空闲的item分配出去
        it = (item *)p->slots;
        p->slots = it->next;//slots指向下一个空闲的item
        if (it->next) it->next->prev = 0;
        /* Kill flag and initialize refcount here for lock safety in slab
         * mover's freeness detection. */
        it->it_flags &= ~ITEM_SLABBED;
        it->refcount = 1;
        p->sl_curr--; //空闲数目减一
        ret = (void *)it;
    } else {
        ret = NULL;
    }

    if (ret) {
        p->requested += size;//增加slabclass分配出去的字节数
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

////创建空闲item ，挂载到对应slabclass的空闲链表中  
static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= power_largest);
    if (id < POWER_SMALLEST || id > power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &slabclass[id];

    it = (item *)ptr;
	//为item的it_flags添加ITEM_SLABBED属性，标明这个item是在slab中没有被分配出去
    it->it_flags = ITEM_SLABBED;
    it->slabs_clsid = 0;
	//由split_slab_page_into_freelist调用时，下面4行的作用是
	//让这些item的prev和next相互指向，把这些item连起来，
	//当本函数是在worker线程向内存池归还内存时调用，那么下面4行的作用是，
	//使用链表头插法把该item插入到空闲item链表中
    it->prev = 0;
    it->next = p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it; //slot变量指向第一个空闲可以使用的item

    p->sl_curr++; //空闲可以使用的item数量
    p->requested -= size;//减少这个slabclass_t分配出去的字节数
    return;
}

static int nz_strcmp(int nzlength, const char *nz, const char *z) {
    int zlength=strlen(z);
    return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    if (add_stats != NULL) {
        if (!stat_type) {
            /* prepare general statistics for the engine */
            STATS_LOCK();
            APPEND_STAT("bytes", "%llu", (unsigned long long)stats.curr_bytes);
            APPEND_STAT("curr_items", "%u", stats.curr_items);
            APPEND_STAT("total_items", "%u", stats.total_items);
            STATS_UNLOCK();
            if (settings.slab_automove > 0) {
                pthread_mutex_lock(&slabs_lock);
                APPEND_STAT("slab_global_page_pool", "%u", slabclass[SLAB_GLOBAL_PAGE_POOL].slabs);
                pthread_mutex_unlock(&slabs_lock);
            }
            item_stats_totals(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "items") == 0) {
            item_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "slabs") == 0) {
            slabs_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes") == 0) {
            item_stats_sizes(add_stats, c);
        } else {
            ret = false;
        }
    } else {
        ret = false;
    }

    return ret;
}

/*
stats slabs
STAT 1:chunk_size 96
STAT 1:chunks_per_page 10922
STAT 1:total_pages 1
STAT 1:total_chunks 10922
STAT 1:used_chunks 4
STAT 1:free_chunks 10918
STAT 1:free_chunks_end 0
STAT 1:mem_requested 300
STAT 1:get_hits 2
STAT 1:cmd_set 6
STAT 1:delete_hits 0
STAT 1:incr_hits 0
STAT 1:decr_hits 0
STAT 1:cas_hits 0
STAT 1:cas_badval 0
STAT 1:touch_hits 0
STAT 4:chunk_size 192
STAT 4:chunks_per_page 5461
STAT 4:total_pages 1
STAT 4:total_chunks 5461
STAT 4:used_chunks 11
STAT 4:free_chunks 5450
STAT 4:free_chunks_end 0
STAT 4:mem_requested 2041
STAT 4:get_hits 0
STAT 4:cmd_set 16169
STAT 4:delete_hits 0
STAT 4:incr_hits 0
STAT 4:decr_hits 0
STAT 4:cas_hits 0
STAT 4:cas_badval 0
STAT 4:touch_hits 0
STAT active_slabs 2
STAT total_malloced 2097024
END
*/
/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;
    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);

    total = 0;
    for(i = POWER_SMALLEST; i <= power_largest; i++) {
        slabclass_t *p = &slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            APPEND_NUM_STAT(i, "used_chunks", "%u",
                            slabs*perslab - p->sl_curr);
            APPEND_NUM_STAT(i, "free_chunks", "%u", p->sl_curr);
            /* Stat is dead, but displaying zero instead of removing it. */
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", 0);
            APPEND_NUM_STAT(i, "mem_requested", "%llu",
                            (unsigned long long)p->requested);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)mem_malloced);
    add_stats(NULL, 0, NULL, 0, c);
}


//申请分配内存，如果程序是有预分配内存块的，就向预分配内存块申请内存
//否则调用malloc分配内存
static void *memory_allocate(size_t size) { //这里的size一般是settings.item_size_max大小也就是默认1M
    void *ret;

	//如果程序要求预先分配内存，而不是到了需要的时候才分配内存，那么
	//mem_base就指向那块预先分配的内存
	//mem_current指向还可以使用的内存的开始位置
	//mem_avail指明还有多少内存是可以使用的
    if (mem_base == NULL) { //不是预分配的内存

        /* We are not using a preallocated large memory chunk */
        ret = malloc(size);
    } else {
        ret = mem_current;

		//在字节对齐中，最后几个用于对齐的字节本身就是没有意义
		//所以这里先计算size是否比可用的内存大，然后才计算对齐
        if (size > mem_avail) { //没有足够的内存可用
            return NULL;
        }

		//现在考虑对齐的问题，如果对齐后size比mem_avail大也是无所谓的
		//因为最后几个用于对齐的字节不会真正使用
        /* mem_current pointer _must_ be aligned!!! */
        if (size % CHUNK_ALIGN_BYTES) { //字节对齐，保证size是CHUNK_ALIGN_BYTES(8)的倍数
            size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
        }

        mem_current = ((char*)mem_current) + size;
        if (size < mem_avail) {
            mem_avail -= size;
        } else {//此时，size比mem_avail大也无所谓
            mem_avail = 0;
        }
    }
    mem_malloced += size;

    return ret;
}

//从id对应的slabclass[id]中的一个chunk中获取所需的size空间
void *slabs_alloc(size_t size, unsigned int id, unsigned int *total_chunks,
        unsigned int flags) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id, total_chunks, flags);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_stats(add_stats, c);
    pthread_mutex_unlock(&slabs_lock);
}

//新的item直接霸占旧的item就会调用这个函数   重新计算一下这个slabclass_t分配出去的内存大小  
void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)
{
    pthread_mutex_lock(&slabs_lock);
    slabclass_t *p;
    if (id < POWER_SMALLEST || id > power_largest) {
        fprintf(stderr, "Internal error! Invalid slab class\n");
        abort();
    }

    p = &slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&slabs_lock);
}

unsigned int slabs_available_chunks(const unsigned int id, bool *mem_flag,
        unsigned int *total_chunks, unsigned int *chunks_perslab) {
    unsigned int ret;
    slabclass_t *p;

    pthread_mutex_lock(&slabs_lock);
    p = &slabclass[id];
    ret = p->sl_curr;
    if (mem_flag != NULL)
        *mem_flag = mem_limit_reached;
    if (total_chunks != NULL)
        *total_chunks = p->slabs * p->perslab;
    if (chunks_perslab != NULL)
        *chunks_perslab = p->perslab;
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

static pthread_cond_t slab_rebalance_cond = PTHREAD_COND_INITIALIZER;
static volatile int do_run_slab_thread = 1;
static volatile int do_run_slab_rebalance_thread = 1;

#define DEFAULT_SLAB_BULK_CHECK 1
int slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;

static int slab_rebalance_start(void) { //这里是获取指定类型slab数组的第一个slab页
    slabclass_t *s_cls;
    int no_go = 0;

    pthread_mutex_lock(&slabs_lock);

    if (slab_rebal.s_clsid < POWER_SMALLEST ||
        slab_rebal.s_clsid > power_largest  ||
        slab_rebal.d_clsid < SLAB_GLOBAL_PAGE_POOL ||
        slab_rebal.d_clsid > power_largest  ||
        slab_rebal.s_clsid == slab_rebal.d_clsid) //非法下标索引  
        no_go = -2;

    s_cls = &slabclass[slab_rebal.s_clsid];

    //为这个目标slab class增加一个页表项都失败，那么就  
    //根本无法为之增加一个页了  
    if (!grow_slab_list(slab_rebal.d_clsid)) {
        no_go = -1;
    }

    if (s_cls->slabs < 2) //源slab class页数太少了，无法分一个页给别人  
        no_go = -3;

    if (no_go != 0) {
        pthread_mutex_unlock(&slabs_lock);
        return no_go; /* Should use a wrapper function... */
    }

    /* Always kill the first available slab page as it is most likely to
     * contain the oldest items
     */
    //记录要移动的页的信息。slab_start指向页的开始位置。slab_end指向页  
    //的结束位置。slab_pos则记录当前处理的位置(item)  
    slab_rebal.slab_start = s_cls->slab_list[0]; //start和end指向的是该类型slab数组的第一个slab页
    slab_rebal.slab_end   = (char *)slab_rebal.slab_start +
        (s_cls->size * s_cls->perslab);
    slab_rebal.slab_pos   = slab_rebal.slab_start;
    slab_rebal.done       = 0;

    /* Also tells do_item_get to search for items in this slab */
    //在do_item_get中如果获取的key-value刚好就是该src中对应的item，则会进行一些特殊处理，参考do_item_get
    slab_rebalance_signal = 2; //要rebalance线程接下来进行内存页移动  

    if (settings.verbose > 1) {
        fprintf(stderr, "Started a slab rebalance\n");
    }

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slab_reassign_running = true;
    STATS_UNLOCK();

    return 0;
}

/* CALLED WITH slabs_lock HELD */
static void *slab_rebalance_alloc(const size_t size, unsigned int id) {
    slabclass_t *s_cls;
    s_cls = &slabclass[slab_rebal.s_clsid];
    int x;
    item *new_it = NULL;

    for (x = 0; x < s_cls->perslab; x++) {
        new_it = do_slabs_alloc(size, id, NULL, SLABS_ALLOC_NO_NEWPAGE);
        /* check that memory isn't within the range to clear */
        if (new_it == NULL) {
            break;
        }
        if ((void *)new_it >= slab_rebal.slab_start
            && (void *)new_it < slab_rebal.slab_end) {
            /* Pulled something we intend to free. Mark it as freed since
             * we've already done the work of unlinking it from the freelist.
             */
            s_cls->requested -= size;
            new_it->refcount = 0;
            new_it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
            new_it = NULL;
            slab_rebal.inline_reclaim++;
        } else {
            break;
        }
    }
    return new_it;
}

enum move_status {
    MOVE_PASS=0, MOVE_FROM_SLAB, MOVE_FROM_LRU, 
	MOVE_BUSY, //此时还有另外一个worker线程在归还这个item  
	MOVE_LOCKED
};

/* refcount == 0 is safe since nobody can incr while item_lock is held.
 * refcount != 0 is impossible since flags/etc can be modified in other
 * threads. instead, note we found a busy one and bail. logic in do_item_get
 * will prevent busy items from continuing to be busy
 * NOTE: This is checking it_flags outside of an item lock. I believe this
 * works since it_flags is 8 bits, and we're only ever comparing a single bit
 * regardless. ITEM_SLABBED bit will always be correct since we're holding the
 * lock which modifies that bit. ITEM_LINKED won't exist if we're between an
 * item having ITEM_SLABBED removed, and the key hasn't been added to the item
 * yet. The memory barrier from the slabs lock should order the key write and the
 * flags to the item?
 * If ITEM_LINKED did exist and was just removed, but we still see it, that's
 * still safe since it will have a valid key, which we then lock, and then
 * recheck everything.
 * This may not be safe on all platforms; If not, slabs_alloc() will need to
 * seed the item key while holding slabs_lock.
 */

/*
slab_rebalance_move函数的名字取得不好，因为实现的不是移动(迁移)，而是把内存页中的item删除从哈希表和LRU队列中删除。
如果处理完内存页的所有item，那么就会slab_rebal.done++，标志处理完成。在线程函数slab_rebalance_thread中，如
果slab_rebal.done为真就会调用slab_rebalance_finish函数完成真正的内存页迁移操作，把一个内存页从一个slab class 
转移到另外一个slab class中。
*/

/*
    回过头继续看rebalance线程。前面说到已经标注了源slab class的一个内存页。标注完rebalance线程就会调用
slab_rebalance_move函数完成真正的内存页迁移操作。源slab class上的内存页是有item的，那么在迁移的时候怎
么处理这些item呢？memcached的处理方式是很粗暴的：直接删除。如果这个item还有worker线程在使用，rebalance
线程就等你一下。如果这个item没有worker线程在引用，那么即使这个item没有过期失效也将直接删除。
    因为一个内存页可能会有很多个item，所以memcached也采用分期处理的方法，每次只处理少量的item(默认为一个)。所
以呢，slab_rebalance_move函数会在slab_rebalance_thread线程函数中多次调用，直到处理了所有的item。
*/// slab_rebalance_move只是把源slab数组的第一个slab页中的所有item进行清除，slab_rebalance_finish把源的slab空间全部迁移给目的slab是在这个函数中完成的
static int slab_rebalance_move(void) {
    slabclass_t *s_cls;
    int x;
    int was_busy = 0;//was_busy就标志了是否有worker线程在引用内存页中的一个item
    int refcount = 0;
    uint32_t hv;
    void *hold_lock;
    enum move_status status = MOVE_PASS;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &slabclass[slab_rebal.s_clsid];

    //会在start_slab_maintenance_thread函数中读取环境变量设置slab_bulk_check  
    //默认值为1.同样这里也是采用分期处理的方案处理一个页上的多个item  
    for (x = 0; x < slab_bulk_check; x++) { //默认为1，
        hv = 0;
        hold_lock = NULL;
        //检查该slab页的第一个item
        item *it = slab_rebal.slab_pos; //参考slab_rebalance_start  
        status = MOVE_PASS;
        /* ITEM_FETCHED when ITEM_SLABBED is overloaded to mean we've cleared
         * the chunk for move. Only these two flags should exist.
         */
        if (it->it_flags != (ITEM_SLABBED|ITEM_FETCHED)) {
            /* ITEM_SLABBED can only be added/removed under the slabs_lock */
                    //如果it_flags&ITEM_SLABBED为真，那么就说明这个item  
                    //根本就没有分配出去。如果为假，那么说明这个item被分配  
                    //出去了，但处于归还途中。参考do_item_get函数里面的  
                    //判断语句，有slab_rebalance_signal作为判断条件的那个。 
            if (it->it_flags & ITEM_SLABBED) {
                /* remove from slab freelist */
                if (s_cls->slots == it) {
                    s_cls->slots = it->next;
                }
                if (it->next) it->next->prev = it->prev;
                if (it->prev) it->prev->next = it->next;
                s_cls->sl_curr--;
                status = MOVE_FROM_SLAB;
            } else if ((it->it_flags & ITEM_LINKED) != 0) {
                /* If it doesn't have ITEM_SLABBED, the item could be in any
                 * state on its way to being freed or written to. If no
                 * ITEM_SLABBED, but it's had ITEM_LINKED, it must be active
                 * and have the key written to it already.
                 */
                hv = hash(ITEM_key(it), it->nkey);
                if ((hold_lock = item_trylock(hv)) == NULL) {
                    status = MOVE_LOCKED;
                } else {
                    refcount = refcount_incr(&it->refcount);
                    if (refcount == 2) { /* item is linked but not busy */
                        /* Double check ITEM_LINKED flag here, since we're
                         * past a memory barrier from the mutex. *///没有worker线程引用这个item  
                        if ((it->it_flags & ITEM_LINKED) != 0) {
                            status = MOVE_FROM_LRU;
                    } else { //现在有worker线程正在引用这个item  
                            /* refcount == 1 + !ITEM_LINKED means the item is being
                             * uploaded to, or was just unlinked but hasn't been freed
                             * yet. Let it bleed off on its own and try again later */
                            status = MOVE_BUSY;
                        }
                    } else {
                        if (settings.verbose > 2) {
                            fprintf(stderr, "Slab reassign hit a busy item: refcount: %d (%d -> %d)\n",
                                it->refcount, slab_rebal.s_clsid, slab_rebal.d_clsid);
                        }
                        status = MOVE_BUSY;
                    }
                    /* Item lock must be held while modifying refcount */
                    if (status == MOVE_BUSY) {
                        refcount_decr(&it->refcount);
                        item_trylock_unlock(hold_lock);
                    }
                }
            } else {
                /* See above comment. No ITEM_SLABBED or ITEM_LINKED. Mark
                 * busy and wait for item to complete its upload. */
                status = MOVE_BUSY;
            }
        }

        int save_item = 0;
        item *new_it = NULL;
        size_t ntotal = 0;
        switch (status) {
            case MOVE_FROM_LRU:
                /* Lock order is LRU locks -> slabs_lock. unlink uses LRU lock.
                 * We only need to hold the slabs_lock while initially looking
                 * at an item, and at this point we have an exclusive refcount
                 * (2) + the item is locked. Drop slabs lock, drop item to
                 * refcount 1 (just our own, then fall through and wipe it
                 */
                /* Check if expired or flushed */
                ntotal = ITEM_ntotal(it);
                /* REQUIRES slabs_lock: CHECK FOR cls->sl_curr > 0 */
                if ((it->exptime != 0 && it->exptime < current_time)
                    || item_is_flushed(it)) {
                    /* TODO: maybe we only want to save if item is in HOT or
                     * WARM LRU?
                     */
                    save_item = 0;
                } else if ((new_it = slab_rebalance_alloc(ntotal, slab_rebal.s_clsid)) == NULL) {
                    save_item = 0;
                    slab_rebal.evictions_nomem++;
                } else {
                    save_item = 1;
                }
                pthread_mutex_unlock(&slabs_lock);
                if (save_item) {
                    /* if free memory, memcpy. clear prev/next/h_bucket */
                    memcpy(new_it, it, ntotal);
                    new_it->prev = 0;
                    new_it->next = 0;
                    new_it->h_next = 0;
                    /* These are definitely required. else fails assert */
                    new_it->it_flags &= ~ITEM_LINKED;
                    new_it->refcount = 0;
                    do_item_replace(it, new_it, hv);
                    slab_rebal.rescues++;
                } else {
                    do_item_unlink(it, hv);
                }
                item_trylock_unlock(hold_lock);
                pthread_mutex_lock(&slabs_lock);
                /* Always remove the ntotal, as we added it in during
                 * do_slabs_alloc() when copying the item.
                 */
                s_cls->requested -= ntotal;
            case MOVE_FROM_SLAB:
                it->refcount = 0;
                it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
#ifdef DEBUG_SLAB_MOVER
                memcpy(ITEM_key(it), "deadbeef", 8);
#endif
                break;
            case MOVE_BUSY:
            case MOVE_LOCKED:
                slab_rebal.busy_items++;//记录是否有不能马上处理的item  
                was_busy++;
                break;
            case MOVE_PASS:
                break;
        }

        //处理这个页的下一个item  
        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end) //遍历完了这个页 
            break;
    }

    if (slab_rebal.slab_pos >= slab_rebal.slab_end) {
        /* Some items were busy, start again from the top */
        if (slab_rebal.busy_items) {//在处理的时候，跳过了一些item(因为有worker线程在引用)  
            slab_rebal.slab_pos = slab_rebal.slab_start; //此时需要从头再扫描一次这个页  
            STATS_LOCK();
            stats.slab_reassign_busy_items += slab_rebal.busy_items;
            STATS_UNLOCK();
            slab_rebal.busy_items = 0;
        } else {
            slab_rebal.done++;//标志已经处理完这个页的所有item  
        }
    }

    pthread_mutex_unlock(&slabs_lock);

    return was_busy;//返回记录   was_busy就标志了是否有worker线程在引用内存页中的一个item
}

/*
slab_rebalance_move函数的名字取得不好，因为实现的不是移动(迁移)，而是把内存页中的item删除从哈希表和LRU队列中删除。
如果处理完内存页的所有item，那么就会slab_rebal.done++，标志处理完成。在线程函数slab_rebalance_thread中，如
果slab_rebal.done为真就会调用slab_rebalance_finish函数完成真正的内存页迁移操作，把一个内存页从一个slab class 
转移到另外一个slab class中。
*/ // slab_rebalance_move只是把源slab数组的第一个slab页中的所有item进行清除，slab_rebalance_finish把源的slab空间全部迁移给目的slab是在这个函数中完成的
static void slab_rebalance_finish(void) {
    slabclass_t *s_cls;
    slabclass_t *d_cls;
    int x;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &slabclass[slab_rebal.s_clsid];
    d_cls = &slabclass[slab_rebal.d_clsid];

#ifdef DEBUG_SLAB_MOVER
    /* If the algorithm is broken, live items can sneak in. */
    slab_rebal.slab_pos = slab_rebal.slab_start;
    while (1) {
        item *it = slab_rebal.slab_pos;
        assert(it->it_flags == (ITEM_SLABBED|ITEM_FETCHED));
        assert(memcmp(ITEM_key(it), "deadbeef", 8) == 0);
        it->it_flags = ITEM_SLABBED|ITEM_FETCHED;
        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }
#endif

    /* At this point the stolen slab is completely clear.
     * We always kill the "first"/"oldest" slab page in the slab_list, so
     * shuffle the page list backwards and decrement.
     */
    s_cls->slabs--;//源slab class的内存页数减一  
    for (x = 0; x < s_cls->slabs; x++) {
        s_cls->slab_list[x] = s_cls->slab_list[x+1];
    }

    //将slab_rebal.slab_start指向的一个页内存馈赠给目标slab class  
    //slab_rebal.slab_start指向的页是从源slab class中得到的。  
    d_cls->slab_list[d_cls->slabs++] = slab_rebal.slab_start;
    /* Don't need to split the page into chunks if we're just storing it */
    if (slab_rebal.d_clsid > SLAB_GLOBAL_PAGE_POOL) {
        memset(slab_rebal.slab_start, 0, (size_t)settings.item_size_max);
		//按照目标slab class的item尺寸进行划分这个页，并且将这个页的  
    //内存并入到目标slab class的空闲item队列中  
        split_slab_page_into_freelist(slab_rebal.slab_start,
            slab_rebal.d_clsid);
    }

    slab_rebal.done       = 0;
    slab_rebal.s_clsid    = 0;
    slab_rebal.d_clsid    = 0;
    slab_rebal.slab_start = NULL;
    slab_rebal.slab_end   = NULL;
    slab_rebal.slab_pos   = NULL;
    evictions_nomem    = slab_rebal.evictions_nomem;
    inline_reclaim = slab_rebal.inline_reclaim;
    rescues   = slab_rebal.rescues;
    slab_rebal.evictions_nomem    = 0;
    slab_rebal.inline_reclaim = 0;
    slab_rebal.rescues  = 0;

    slab_rebalance_signal = 0; //rebalance线程完成工作后，再次进入休眠状态  

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slab_reassign_running = false;
    stats.slabs_moved++;
    stats.slab_reassign_rescues += rescues;
    stats.slab_reassign_evictions_nomem += evictions_nomem;
    stats.slab_reassign_inline_reclaim += inline_reclaim;
    STATS_UNLOCK();

    if (settings.verbose > 1) {
        fprintf(stderr, "finished a slab move\n");
    }
}

/* Slab mover thread.
 * Sits waiting for a condition to jump off and shovel some memory about
 */ //slab_maintenance_thread线程循环来选举出替换和被替换slabclass的id号，然后发送信号来触发slab_rebalance_thread线程进行真正的替换操作
static void *slab_rebalance_thread(void *arg) { 
    int was_busy = 0;
    /* So we first pass into cond_wait with the mutex held */
    mutex_lock(&slabs_rebalance_lock);

    while (do_run_slab_rebalance_thread) { //实际上默认收到客户端slabs reassign <source class> <dest class>命令的时候，一次命令只前迁移源slab中的一个内存页，也就是默认1M
        if (slab_rebalance_signal == 1) { //do_slabs_reassign选出源和目的后来置1,唤醒
            //标志要移动的内存页的信息，并将slab_rebalance_signal赋值为2  
            //slab_rebal.done赋值为0，表示没有完成  
            if (slab_rebalance_start() < 0) { 
            //这里返回后设置slab_rebalance_signal=2，并设置slab_rebal.slab_start为需要迁移的slab的其实内存处，然后在循环一圈后会执行下面的
            //} else if (slab_rebalance_signal && slab_rebal.slab_start != NULL) {
                /* Handle errors with more specifity as required. */
                slab_rebalance_signal = 0;
            }

            was_busy = 0;
        } else if (slab_rebalance_signal && slab_rebal.slab_start != NULL) {
// slab_rebalance_move只是把源slab数组的第一个slab页中的所有item进行清除，slab_rebalance_finish把源的slab空间全部迁移给目的slab是在这个函数中完成的
            was_busy = slab_rebalance_move(); //进行内存页迁移操作  
//因为前面的slab_rebalance_move在对源slab中的item进行删除的时候，一次默认只删除一个item,所以实际上这里会多次循环执行，知道源slab所有item删除完毕
        }

        if (slab_rebal.done) {
  // slab_rebalance_move只是把源slab数组的第一个slab页中的所有item进行清除，slab_rebalance_finish把源的slab空间全部迁移给目的slab是在这个函数中完成的
            slab_rebalance_finish();//完成内存页重分配操作 
        } else if (was_busy) {//有worker线程在使用内存页上的item  
            /* Stuck waiting for some items to unlock, so slow down a bit
             * to give them a chance to free up */
            usleep(50);//休眠一会儿，等待worker线程放弃使用item，然后再次尝试    
            
        }

        if (slab_rebalance_signal == 0) { //一开始就在这里休眠  
            //等待do_slabs_reassign选出源和目的后来唤醒
            /* always hold this lock while we're running */
            pthread_cond_wait(&slab_rebalance_cond, &slabs_rebalance_lock);
        }
    }
    return NULL;
}

/* Iterate at most once through the slab classes and pick a "random" source.
 * I like this better than calling rand() since rand() is slow enough that we
 * can just check all of the classes once instead.
 *///选出一个内存页数大于1的slab class，并且该slab class不能是dst  
//指定的那个。如果不存在这样的slab class，那么返回-1  
static int slabs_reassign_pick_any(int dst) { //随机从slabclass[]中选出一个slabs数大于1的
    static int cur = POWER_SMALLEST - 1;
    int tries = power_largest - POWER_SMALLEST + 1;
    for (; tries > 0; tries--) {
        cur++;
        if (cur > power_largest)
            cur = POWER_SMALLEST;
        if (cur == dst)
            continue;
        if (slabclass[cur].slabs > 1) {
            return cur;
        }
    }
    return -1;
}

//开启自动automove功能或者接受客户端slabs reassign命令的时候会走到这里
static enum reassign_result_type do_slabs_reassign(int src, int dst) {
    if (slab_rebalance_signal != 0) //正在进行内存页迁移操作
        return REASSIGN_RUNNING;

    if (src == dst) //不能相同  
        return REASSIGN_SRC_DST_SAME;

    /* Special indicator to choose ourselves. */
    if (src == -1) {
        //客户端命令要求随机选出一个源slab class  只有在slabs reassign <source class> <dest class>中指定src为-1的时候才会满足该条件
        //选出一个页数大于1的slab class，并且该slab class不能是dst指定的那个。如果不存在这样的slab class，那么返回-1  
        src = slabs_reassign_pick_any(dst);
        /* TODO: If we end up back at -1, return a new error type */
    }

    if (src < POWER_SMALLEST        || src > power_largest ||
        dst < SLAB_GLOBAL_PAGE_POOL || dst > power_largest)
        return REASSIGN_BADCLASS;

    if (slabclass[src].slabs < 2) //源slab class没有或者只有一个内存页，那么就不能分给别的slab class  
        return REASSIGN_NOSPARE;

    //全局变量slab_rebal  
    slab_rebal.s_clsid = src;//保存源slab class  
    slab_rebal.d_clsid = dst;//保存目标slab class  

    slab_rebalance_signal = 1;
     //唤醒slab_rebalance_thread函数的线程.  
    //在slabs_reassign函数中已经锁上了slabs_rebalance_lock  
    pthread_cond_signal(&slab_rebalance_cond);

    return REASSIGN_OK;
}

//进行slab重分配机制
enum reassign_result_type slabs_reassign(int src, int dst) {
    enum reassign_result_type ret;
    if (pthread_mutex_trylock(&slabs_rebalance_lock) != 0) {
        return REASSIGN_RUNNING;
    }
    ret = do_slabs_reassign(src, dst);
    pthread_mutex_unlock(&slabs_rebalance_lock);
    return ret;
}

/* If we hold this lock, rebalancer can't wake up or move */
void slabs_rebalancer_pause(void) {
    pthread_mutex_lock(&slabs_rebalance_lock);
}

void slabs_rebalancer_resume(void) {
    pthread_mutex_unlock(&slabs_rebalance_lock);
}

static pthread_t rebalance_tid;


/*
    考虑这样的一个情景：在一开始，由于业务原因向memcached存储大量长度为1KB的数据，也就是说memcached服务器进程
里面有很多大小为1KB的item。现在由于业务调整需要存储大量10KB的数据，并且很少使用1KB的那些数据了。由于数据越
来越多，内存开始吃紧。大小为10KB的那些item频繁访问，并且由于内存不够需要使用LRU淘汰一些10KB的item。
对于上面的情景，会不会觉得大量1KB的item实在太浪费了。由于很少访问这些item，所以即使它们超时过期了，还是会
占据着哈希表和LRU队列。LRU队列还好，不同大小的item使用不同的LRU队列。但对于哈希表来说大量的僵尸item会增加
哈希冲突的可能性，并且在迁移哈希表的时候也浪费时间。有没有办法干掉这些item？使用LRU爬虫+lru_crawler命令是
可以强制干掉这些僵尸item。但干掉这些僵尸item后，它们占据的内存是归还到1KB的那些slab分配器中。1KB的slab分
配器不会为10KB的item分配内存。所以还是功亏一篑。

    那有没有别的办法呢？是有的。memcached提供的slab automove 和 rebalance两个东西就是完成这个功能的。在默认
情况下，memcached不启动这个功能，所以要想使用这个功能必须在启动memcached的时候加上参数-o slab_reassign。
之后就可以在客户端发送命令slabs reassign <source class> <dest class>，手动将source class的内存页分给dest 
class。后文会把这个工作称为内存页重分配。而命令slabs automove则是让memcached自动检测是否需要进行内存页重分配，
    如果需要的话就自动去操作，这样一切都不需要人工的干预。
如果在启动memcached的时候使用了参数-o slab_reassign，那么就会把settings.slab_reassign赋值为true(该变量的默认值为false)。
还记得《slab内存分配器》说到的每一个内存页的大小吗？在do_slabs_newslab函数中，一个内存页的大小会根据
settings.slab_reassign是否为true而不同。
 //参考http://blog.csdn.net/luotuo44/article/details/43015129

*/ // main函数会调用start_slab_maintenance_thread函数启动rebalance线程和automove线程。main函数是在settings.slab_reassign为true时才会调用的。
int start_slab_maintenance_thread(void) { //由main函数调用，如果settings.slab_reassign为false将不会调用本函数(默认是false)  
    int ret;
    slab_rebalance_signal = 0;
    slab_rebal.slab_start = NULL;
    char *env = getenv("MEMCACHED_SLAB_BULK_CHECK");
    if (env != NULL) {
        slab_bulk_check = atoi(env);
        if (slab_bulk_check == 0) {
            slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;
        }
    }

    if (pthread_cond_init(&slab_rebalance_cond, NULL) != 0) {
        fprintf(stderr, "Can't intiialize rebalance condition\n");
        return -1;
    }
    pthread_mutex_init(&slabs_rebalance_lock, NULL);

    if ((ret = pthread_create(&rebalance_tid, NULL,
                              slab_rebalance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

/* The maintenance thread is on a sleep/loop cycle, so it should join after a
 * short wait */
void stop_slab_maintenance_thread(void) {
    mutex_lock(&slabs_rebalance_lock);
    do_run_slab_thread = 0;
    do_run_slab_rebalance_thread = 0;
    pthread_cond_signal(&slab_rebalance_cond);
    pthread_mutex_unlock(&slabs_rebalance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(rebalance_tid, NULL);
}
