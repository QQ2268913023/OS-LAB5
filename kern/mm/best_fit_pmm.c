#include <pmm.h>
#include <list.h>
#include <string.h>
#include <best_fit_pmm.h>
#include <stdio.h>
#include <assert.h>


static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

static void
best_fit_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

static void
best_fit_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    list_entry_t* le;
    
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        /*LAB2 EXERCISE 2: YOUR CODE*/ 
        // 清空当前页框的标志和属性信息，并将页框的引用计数设置为0
        p->flags = 0;
        set_page_ref(p, 0);
    }
    
    // 设置第一个页框的属性
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 插入到空闲链表，保持地址有序
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            /*LAB2 EXERCISE 2: YOUR CODE*/ 
            // 1、当base < page时，找到第一个大于base的页，将base插入到它前面，并退出循环
            // 2、当list_next(le) == &free_list时，若已经到达链表结尾，将base插入到链表尾部
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
                break;
            }
        }
    }
}

static struct Page *
best_fit_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }

    // 引入两个新变量：用于记录找到的最佳匹配块及其大小
    struct Page *best_fit_page = NULL;
    // 使用 (size_t)-1 来表示最大值
    size_t min_property = (size_t)-1; 

    list_entry_t *le = &free_list;
    // 1. 遍历整个空闲列表，寻找 Best-Fit 块 (>= n 且最小)
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        
        // 检查当前空闲块 p 是否最合适
        if (p->property >= n && p->property < min_property) {
            min_property = p->property;
            best_fit_page = p;
        }
    }

    // 2. 如果找到了 Best-Fit 块
    if (best_fit_page != NULL) {
        struct Page *page = best_fit_page;
        
        // 3. 将 Best-Fit 块从链表中移除
        list_entry_t* prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));

        // 4. 分裂：如果 Best-Fit 块有剩余空间，将分裂出的碎片插回原位
        if (page->property > n) {
            struct Page *p_new_free = page + n;
            p_new_free->property = page->property - n;
            SetPageProperty(p_new_free);
            
            // 将碎片插入到原块的前一个元素 prev 之后
            list_add(prev, &(p_new_free->page_link));
        }
        
        // 5. 更新统计数据和页属性
        nr_free -= n;
        ClearPageProperty(page); // 清除属性标记，表示已分配
        
        return page;
    }
    
    return NULL; // 未找到合适的块
}

static void
best_fit_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    
    // 修复编译错误: 将所有局部变量声明移到函数开始处
    list_entry_t *le;
    list_entry_t *le_prev;
    list_entry_t *le_next;
    struct Page *p = base;

    for (; p != base + n; p ++) {
        // 检查页是否未被保留且未被标记为 Property Page
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    
    /*LAB2 EXERCISE 2: YOUR CODE (A)*/ 
    // 设置当前页块的属性、标记并更新 nr_free
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    
    // 1. 将页块插入到空闲链表的正确位置 (按地址从小到大排序)
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                goto merge_check; // 插入完成，跳转到合并检查
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
                goto merge_check; // 插入到链表尾部
            }
        }
    }

merge_check: // 合并检查点

    // 2. 检查与低地址空闲块的合并 (向后合并)
    le_prev = list_prev(&(base->page_link));
    if (le_prev != &free_list) {
        p = le2page(le_prev, page_link);
        /*LAB2 EXERCISE 2: YOUR CODE (B)*/ 
        // 检查前面的空闲页块是否与当前页块连续并进行合并
        if (p + p->property == base) { 
            p->property += base->property;   // 2. 更新前一个空闲页块的大小
            ClearPageProperty(base);         // 3. 清除当前页块的属性标记
            list_del(&(base->page_link));    // 4. 从链表中删除当前页块
            base = p;                        // 5. 将起始块指针指向合并后的块 p
        }
    }

    // 3. 检查与高地址空闲块的合并 (向前合并)
    // 注意：如果上一步发生了合并，base 已经是 p (低地址块)
    le_next = list_next(&(base->page_link));
    if (le_next != &free_list) {
        p = le2page(le_next, page_link);
        if (base + base->property == p) { // 检查是否连续
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}

static size_t
best_fit_nr_free_pages(void) {
    return nr_free;
}

// ----------------------------------------------------------------------
// 以下是 ucore 的检查函数和 pmm_manager 结构体定义，保持不变
// ----------------------------------------------------------------------

static void
basic_check(void) {
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;
    
}

static void
best_fit_check(void) {
    int score = 0 ,sumscore = 6;
    int count = 0, total = 0;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        assert(PageProperty(p));
        count ++, total += p->property;
    }
    assert(total == nr_free_pages());


    #ifdef ucore_test
    score += 1;
    #endif
    

}

const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",
    .init = best_fit_init,
    .init_memmap = best_fit_init_memmap,
    .alloc_pages = best_fit_alloc_pages,
    .free_pages = best_fit_free_pages,
    .nr_free_pages = best_fit_nr_free_pages,
    .check = best_fit_check,
};