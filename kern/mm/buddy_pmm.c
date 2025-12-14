#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <memlayout.h>
#include <buddy_pmm.h>


#define MIN_ORDER 0                 
#define MAX_ORDER 14                
#define ORDER_PAGES(k) ((size_t)1UL << (k))

typedef struct {
    list_entry_t free_list;         
    size_t       nr_free;           
} buddy_area_t;

static buddy_area_t areas[MAX_ORDER + 1];
static size_t total_free_pages;    

extern struct Page *pages;
extern size_t npage;


static inline void area_init(int k) {
    list_init(&areas[k].free_list);
    areas[k].nr_free = 0;
}

static inline void mark_block_head(struct Page *p, size_t sz_pages) {
    p->property = sz_pages;
    SetPageProperty(p);
}

static inline void clear_block_head(struct Page *p) {
    p->property = 0;
    ClearPageProperty(p);
}

static void area_push(int k, struct Page *p) {
    list_entry_t *head = &areas[k].free_list;
    list_entry_t *le = head;
    while ((le = list_next(le)) != head) {
        if (p < le2page(le, page_link)) break;
    }
    list_add_before(le, &(p->page_link));
    areas[k].nr_free++;
    total_free_pages += ORDER_PAGES(k);
}

static struct Page *area_pop(int k) {
    list_entry_t *head = &areas[k].free_list;
    if (list_empty(head)) return NULL;
    list_entry_t *le = list_next(head);
    list_del(le);
    struct Page *p = le2page(le, page_link);
    areas[k].nr_free--;
    total_free_pages -= ORDER_PAGES(k);
    return p;
}

static void area_remove_block(int k, struct Page *p) {
    list_del(&(p->page_link));
    areas[k].nr_free--;
    total_free_pages -= ORDER_PAGES(k);
}


static inline size_t buddy_index(size_t idx, size_t size) {
    return idx ^ size;
}

static int ilog2_floor(size_t x) {
    int k = 0;
    while ((x >> (k + 1)) != 0) k++;
    return k;
}
static int ilog2_ceil(size_t x) {
    int k = ilog2_floor(x);
    return (((size_t)1UL << k) == x) ? k : (k + 1);
}


static void buddy_init(void) {
    for (int k = MIN_ORDER; k <= MAX_ORDER; k++) area_init(k);
    total_free_pages = 0;
}

static void buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    for (size_t i = 0; i < n; i++) {
        struct Page *pp = base + i;
        assert(PageReserved(pp));
        pp->flags = 0;
        set_page_ref(pp, 0);
        pp->property = 0;
    }

    size_t left = n;
    size_t cur_idx = (size_t)(base - pages);
    struct Page *p = base;

    while (left > 0) {
        int k = ilog2_floor(left);
        while (k > MIN_ORDER && ((cur_idx & (ORDER_PAGES(k) - 1)) != 0)) k--;
        size_t sz = ORDER_PAGES(k);

        mark_block_head(p, sz);
        area_push(k, p);

        p      += sz;
        cur_idx += sz;
        left   -= sz;
    }
}

static struct Page *buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > total_free_pages) return NULL;

    int need_k = ilog2_ceil(n);
    int src_k = -1;
    for (int k = need_k; k <= MAX_ORDER; k++) {
        if (!list_empty(&areas[k].free_list)) { src_k = k; break; }
    }
    if (src_k < 0) return NULL;

    struct Page *blk = area_pop(src_k);
    size_t blk_sz = ORDER_PAGES(src_k);

    while (src_k > MIN_ORDER) {
        size_t half = blk_sz >> 1;
        if (half < n) break;
        struct Page *right = blk + half;
        mark_block_head(right, half);
        area_push(src_k - 1, right);
        src_k--;
        blk_sz = half;
    }

    struct Page *ret = blk;
    clear_block_head(blk); 

    size_t remain = blk_sz - n;
    struct Page *cur = blk + n;
    size_t cur_idx  = (size_t)(cur - pages);

    while (remain > 0) {
        int k = ilog2_floor(remain);
        while (k > MIN_ORDER && ((cur_idx & (ORDER_PAGES(k) - 1)) != 0)) k--;
        size_t sz = ORDER_PAGES(k);
        mark_block_head(cur, sz);
        area_push(k, cur);
        cur     += sz;
        cur_idx += sz;
        remain  -= sz;
    }
    return ret;
}

static void buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    struct Page *cur = base;
    size_t cur_idx  = (size_t)(cur - pages);
    size_t left = n;

    while (left > 0) {
        int k = ilog2_floor(left);
        while (k > MIN_ORDER && ((cur_idx & (ORDER_PAGES(k) - 1)) != 0)) k--;
        size_t part = ORDER_PAGES(k);     

        for (size_t i = 0; i < part; i++) {
            struct Page *pp = cur + i;
            assert(!PageReserved(pp));
            pp->flags = 0;
            set_page_ref(pp, 0);
            pp->property = 0;
        }
        mark_block_head(cur, part);

        size_t size = part;
        int    ok   = k;
        while (ok < MAX_ORDER) {
            size_t idx  = (size_t)(cur - pages);
            size_t bidx = buddy_index(idx, size);
            if (bidx >= npage) break;
            struct Page *bd = pages + bidx;
            if (!PageProperty(bd) || bd->property != size) break;

            area_remove_block(ok, bd);      
            if (bd < cur) cur = bd;        
            size <<= 1;
            ok++;
            mark_block_head(cur, size);
        }


        area_push(ok, cur);


        cur     = (base + (n - (left - part)));
        cur_idx = (size_t)(cur - pages);
        left   -= part;
    }
}

static size_t buddy_nr_free_pages(void) {
    return total_free_pages;
}


static void dump_order_stats(void) {
    size_t remain = buddy_nr_free_pages();
    cprintf("\n[概览] 剩余空闲页: %lu\n", (unsigned long)remain);
    cprintf("----[按阶统计]----\n");
    for (int k = MIN_ORDER; k <= MAX_ORDER; k++) {
        size_t cnt = areas[k].nr_free;
        size_t pages_cnt = cnt * ORDER_PAGES(k);
        cprintf("  order=%d  块数=%-4lu  累计页=%lu\n",
                k, (unsigned long)cnt, (unsigned long)pages_cnt);
    }
    cprintf("------------------\n");
}

static void dump_free_lists(void) {
    cprintf("----[free_list 当前状态]----\n");
    cprintf("总空闲页: %lu\n", (unsigned long)buddy_nr_free_pages());
    size_t seq = 1;
    for (int k = MIN_ORDER; k <= MAX_ORDER; k++) {
        list_entry_t *head = &areas[k].free_list;
        list_entry_t *le = head;
        while ((le = list_next(le)) != head) {
            struct Page *p = le2page(le, page_link);
            size_t page_idx = (size_t)(p - pages);
            cprintf("  块 #%lu: 起始页idx=%lu, 大小=%lu页, order=%d, 物理地址=0x%016lx\n",
                    (unsigned long)seq++,
                    (unsigned long)page_idx,
                    (unsigned long)p->property,
                    k,
                    (unsigned long)page2pa(p));
        }
    }
    cprintf("----------------------------\n");
}


static void buddy_check(void) {
    cprintf("[buddy] 基本检查开始...\n");

    struct Page *a = buddy_alloc_pages(1);
    struct Page *b = buddy_alloc_pages(1);
    assert(a && b && a != b);
    buddy_free_pages(a, 1);
    buddy_free_pages(b, 1);

    cprintf("[buddy] 基本功能检测通过，nr_free=%lu\n",
            (unsigned long)buddy_nr_free_pages());

    cprintf("\n>>> 伙伴分配算法开始测试（小块→大块→全部） <<<\n");

    cprintf("\n=== 阶段0：初始化状态 ===\n\n");
    dump_order_stats();
    dump_free_lists();

    struct Page *s1 = buddy_alloc_pages(1);
    struct Page *s2 = buddy_alloc_pages(2);
    struct Page *s3 = buddy_alloc_pages(3);
    cprintf("[阶段1] 分配小块: s1=%p(1) s2=%p(2) s3=%p(3)\n", s1, s2, s3);

    cprintf("\n=== 阶段1：小块分配后 ===\n\n");
    dump_order_stats();
    dump_free_lists();

    struct Page *b1 = buddy_alloc_pages(4096);
    struct Page *b2 = buddy_alloc_pages(8192);
    cprintf("[阶段2] 分配大块: b1=%p(4096) b2=%p(8192)\n", b1, b2);

    cprintf("\n=== 阶段2：大块分配后 ===\n\n");
    dump_order_stats();
    dump_free_lists();


cprintf("\n=== 阶段3：回收之前分配的块（观察是否合并） ===\n");


free_pages(s1, 1);
cprintf("\n[阶段3] 释放 1 页后：\n");
dump_order_stats();     
dump_free_lists();     


free_pages(s2, 2);
free_pages(s3, 3);
cprintf("\n[阶段3] 释放 2和3 页后：\n");
dump_order_stats();     
dump_free_lists();     

free_pages(b1, 4096);
cprintf("\n[阶段3] 释放 4096 页后：\n");
dump_order_stats();
dump_free_lists();


free_pages(b2, 8192);


size_t want_all = nr_free_pages();
struct Page *all = alloc_pages(want_all);
cprintf("[阶段3] 全部分配: 请求=%lu页  结果=%s\n", want_all, all ? "成功" : "失败");
if (all) {
    free_pages(all, want_all);
}


cprintf("\n=== 阶段3：回收后总体状态 ===\n");
dump_order_stats();
dump_free_lists();


    cprintf("\n=== 阶段3：全部分配后 ===\n\n");
    dump_order_stats();
    dump_free_lists();
}



const struct pmm_manager buddy_pmm_manager = {
    .name           = "buddy_pmm_manager",
    .init           = buddy_init,
    .init_memmap    = buddy_init_memmap,
    .alloc_pages    = buddy_alloc_pages,
    .free_pages     = buddy_free_pages,
    .nr_free_pages  = buddy_nr_free_pages,
    .check          = buddy_check,
};
