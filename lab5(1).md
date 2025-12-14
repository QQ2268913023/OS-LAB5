# Lab5 实验报告:用户程序

## 练习1: 加载应用程序并执行

### 实现过程：
在`load_icode`函数中，我们需要构建用户态进程的中断帧，一遍内核在执行完系统调用返回时能够正确的切换到用户模式并执行新的应用程序。
1. 设置用户栈指针(`tf->gpr.sp`):
   - 将栈指针设置为`USTACKTOP `。此前已经通过`mm_map`和 `pgdir_alloc_page`为用户栈分配了虚拟内存空间，`USTACKTOP`是该空间的高地址。
2. 设置入口地址(`tf->epc`):
   - 将异常程序计数器（EPC）设置为`ELF`可执行文件头中记录的入口地址 (elf->e_entry)。当内核执行`sret`指令返回时，硬件会将`PC`指针跳转到该地址，从而开始执行用户程序的第一条指令。
3. 设置状态寄存器(`tf->status`):
   - **清零`SPP`位** (SSTATUS_SPP)：将`sstatus`的`SPP`位清零。`SPP`记录了进入中断前的特权级，将其置`0`意味着执行 `sret`后，CPU 将切换回 User Mode（用户态）。
   - **置位 SPIE 位** (SSTATUS_SPIE)：将`sstatus`的 `SPIE`位置 1。这意味着当进程回到用户态后，中断将被开启（允许响应时钟中断等），保证操作系统的抢占式调度正常运行。
### 具体代码
```C
    // 1. 设置用户栈指针
    // 将栈指针(sp)设置为用户栈的顶部地址(USTACKTOP)
    // 这样用户程序就有了一个可用的栈空间
    tf->gpr.sp = USTACKTOP;

    // 2. 设置异常程序计数器 (EPC)
    // 将 epc 设置为 ELF 文件头中指定的入口地址 (e_entry)
    // 当内核执行 sret 指令返回时，CPU 的 PC 指针会跳转到这个地址，
    // 从而开始执行用户程序的第一行代码
    tf->epc = elf->e_entry;

    // 3. 设置状态寄存器 (SSTATUS)
    // 基于当前的 sstatus 进行修改（变量 sstatus 保存了 memset 清零前的旧值）
    // (1) 清除 SPP (Supervisor Previous Privilege) 位：
    //     设置为 0，意味着 sret 返回后，CPU 的特权级将切换为 User Mode (用户模式)
    // (2) 设置 SPIE (Supervisor Previous Interrupt Enable) 位：
    //     设置为 1，意味着 sret 返回用户模式后，全局中断是开启的 (允许响应时钟中断进行调度)
    tf->status = (read_csr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE;
```
### 整体流程

整体功能：**将一个 ELF 格式的二进制可执行文件加载到内存中，并为该进程建立好新的内存空间和执行上下文，使其准备好在用户态运行。**

#### **第 (1) 步：创建内存管理结构**
*   **操作**：调用 `mm_create()` 函数。
*   **分析**：
    *   `exec` 系统调用需要丢弃原有的内存空间，因此首先需要申请一个新的内存管理结构体 `mm_struct`。
    *   这个结构体将用来管理新程序所有的虚拟内存区域（VMA，即 Virtual Memory Area）。

#### **第 (2) 步：创建页目录表 (PDT)**
*   **操作**：调用 `setup_pgdir(mm)` 函数。
*   **分析**：
    *   **分配物理页**：申请一页物理内存作为新的页目录表（Page Directory Table）。
    *   **内核映射**：将内核空间的页表项（通常是高地址部分）复制到这个新表中。这确保了即便是用户进程，在陷入内核（如中断、系统调用）时，也能正确访问内核代码和数据。
    *   **关联**：将 `mm->pgdir` 指向这个页表的内核虚拟地址，方便内核后续操作。

#### **第 (3) 步：加载 ELF 段 (代码/数据/BSS)**
这是最复杂的一步，代码中又细分了几个小步骤：

*   **(3.1) 读取文件头**：将二进制数据的开头强转为 `elfhdr` 结构，获取 ELF Header 信息。
*   **(3.2) 获取程序头表**：根据 Header 中的偏移量找到 Program Header Table，这是一个数组，描述了文件中各个段的信息。
*   **(3.3) 校验合法性**：检查 `e_magic` 是否等于 `ELF_MAGIC`，确保这是个合法的 ELF 文件。
*   **(3.4) 遍历与筛选**：
    *   遍历所有的程序头。
    *   只处理 `p_type` 为 `ELF_PT_LOAD` 的段（即需要加载到内存的可加载段）。
*   **(3.5) 建立虚拟内存映射 (VMA)**：
    *   根据段的属性（读/写/执行），设置对应的权限标志 `vm_flags`。
    *   调用 `mm_map()`，在 `mm` 结构中登记这段虚拟地址范围（从 `p_va` 开始，长度 `p_memsz`）。此时并未分配物理内存。
*   **(3.6) 物理内存分配与内容拷贝**：
    *   **(3.6.1) 拷贝 TEXT/DATA**：对于文件中有实际内容的部分（长度为 `p_filesz`），调用 `pgdir_alloc_page` 分配物理页，并调用 `memcpy` 将二进制数据从 ELF 文件复制到物理内存中。
    *   **(3.6.2) 建立 BSS 段**：对于内存大小大于文件大小的部分（`p_memsz > p_filesz`，通常是未初始化的全局变量），调用 `memset` 将其对应的内存空间清零。

#### **第 (4) 步：建立用户栈**
*   **操作**：
    *   调用 `mm_map` 建立用户栈的 VMA。范围是 `[USTACKTOP - USTACKSIZE, USTACKTOP)`。
    *   连续调用 4 次 `pgdir_alloc_page`。
*   **分析**：
    *   为用户程序分配栈空间。
    *   这里不仅仅是建立映射，还**立即分配了物理页**（通常分配 4 页）。这是为了保证用户程序在刚开始运行时（如参数传递、函数调用）直接有栈可用，而不会立刻触发缺页异常。

#### **第 (5) 步：切换内存空间**
*   **操作**：
    *   `mm_count_inc(mm)`：增加引用计数。
    *   `current->mm = mm`：将当前进程的内存管理器替换为新的。
    *   `current->pgdir = PADDR(mm->pgdir)`：记录页表物理地址。
    *   **`lsatp(PADDR(mm->pgdir))`**：这是一条汇编指令封装。
*   **分析**：
    *   这是**生效**的关键点。
    *   通过 `lsatp` 修改 CPU 的 SATP 寄存器，CPU 的内存管理单元（MMU）立即开始使用新的页表。
    *   从此之后，该进程看到的虚拟地址空间就是新程序的空间了。

#### **第 (6) 步：设置中断帧 (Trapframe)**
*   **操作**：修改 `current->tf` 结构体的内容。
*   **分析**：
    *   这是为了让内核在执行完 `exec` 系统调用并返回（执行 `sret` 指令）时，能够跳转到用户程序的入口，并切换到用户态。
    *   **设置 `sp`**：`tf->gpr.sp = USTACKTOP`。让用户程序使用刚刚建立好的用户栈。
    *   **设置 `epc`**：`tf->epc = elf->e_entry`。让 PC 指针跳转到 ELF 头中定义的程序入口地址。
    *   **设置 `status`**：`tf->status = ...`。清除 `SPP` 位（设为 User Mode），置位 `SPIE` 位（开启中断）。

### 总结：

**`load_icode` 函数的主要功能是：重置当前进程的内存空间，加载新的 ELF 可执行文件，并设置好进程调度和执行所需的硬件上下文。**


1.  **初始化内存管理结构**
    *   为当前进程分配一个新的 `mm_struct` 结构体，用于管理新进程的虚拟内存空间（VMA）和页表。

2.  **初始化页目录表**
    *   分配一个新的物理页作为页目录表（PDT）。
    *   将内核空间的页表映射复制到该页表中，确保进程在用户态陷入内核时（如系统调用）能够正确访问内核代码和数据。

3.  **解析 ELF 并建立内存映射**
    *   解析 ELF 文件头和程序头表，校验文件合法性。
    *   根据 ELF 中的段信息（TEXT/DATA/BSS），调用 `mm_map` 建立虚拟地址空间映射（VMA）。
    *   分配物理内存，将 ELF 文件中的代码和数据段复制到内存中；对 BSS 段（未初始化数据）所占的内存进行清零操作。

4.  **分配用户栈空间**
    *   在虚拟地址空间的顶部（`USTACKTOP`）建立用户栈的 VMA。
    *   立即分配物理内存页作为栈空间，防止程序一开始运行就触发缺页异常。

5.  **切换页表（激活新地址空间）**
    *   将当前进程的 `mm` 指针指向新创建的 `mm_struct`。
    *   将 CPU 的页表基址寄存器（SATP/CR3）设置为新页目录表的物理地址。此时，进程的虚拟地址空间正式切换为新程序的空间。

6.  **构造中断帧（设置执行上下文）**
    *   修改当前进程的中断帧（`tf`），设定内核返回用户态后的硬件状态：
        *   **SP**：指向用户栈顶，确保用户程序有栈可用。
        *   **EPC**：指向 ELF 入口地址，确保程序从正确位置开始执行。
        *   **Status**：配置状态寄存器，确保 CPU 切换至用户特权级（User Mode）并开启中断。
   


## 重要知识点

### load_icode调用的相关函数：

#### 虚拟内存管理 (VMM) 相关函数

这一层主要负责管理抽象的内存区域（VMA），定义了进程“逻辑上”拥有的内存布局。

##### 1. `mm_create`
**文件位置**: `kern/mm/vmm.c`
**源码**:
```c
struct mm_struct *
mm_create(void)
{
    struct mm_struct *mm = kmalloc(sizeof(struct mm_struct));

    if (mm != NULL)
    {
        list_init(&(mm->mmap_list)); // 初始化VMA链表
        mm->mmap_cache = NULL;       // 初始化局部性缓存
        mm->pgdir = NULL;            // 页目录表暂为空
        mm->map_count = 0;

        mm->sm_priv = NULL;

        set_mm_count(mm, 0);         // 引用计数置0
        lock_init(&(mm->mm_lock));
    }
    return mm;
}
```
**具体实现与功能**:
*   **实现**: 使用 `kmalloc` 分配一个 `mm_struct` 结构体，并对其成员进行初始化。特别重要的是初始化了 `mmap_list` 双向链表，用于后续挂载 `vma_struct`。
*   **load_icode中的作用**: **(Step 1)** 为当前进程创建一个全新的内存管理器实例，用于替换旧进程（如 `init` 或父进程）的内存空间。

##### 2. `mm_map`
**文件位置**: `kern/mm/vmm.c`
**源码**:
```c
int mm_map(struct mm_struct *mm, uintptr_t addr, size_t len, uint32_t vm_flags,
           struct vma_struct **vma_store)
{
    uintptr_t start = ROUNDDOWN(addr, PGSIZE), end = ROUNDUP(addr + len, PGSIZE);
    // 1. 检查地址是否处于用户空间范围
    if (!USER_ACCESS(start, end))
    {
        return -E_INVAL;
    }

    assert(mm != NULL);
    int ret = -E_INVAL;
    struct vma_struct *vma;
    // 2. 查找是否有重叠区域
    if ((vma = find_vma(mm, start)) != NULL && end > vma->vm_start)
    {
        goto out;
    }
    ret = -E_NO_MEM;
    // 3. 创建一个新的 vma 结构体
    if ((vma = vma_create(start, end, vm_flags)) == NULL)
    {
        goto out;
    }
    // 4. 将 vma 插入到 mm 的链表中
    insert_vma_struct(mm, vma);
    if (vma_store != NULL)
    {
        *vma_store = vma;
    }
    ret = 0;
out:
    return ret;
}
```
**具体实现与功能**:
*   **实现**: 
    1.  对请求的虚拟地址进行页对齐。
    2.  `USER_ACCESS` 宏检查地址是否合法（不能越界访问内核空间）。
    3.  `find_vma` 检查目标地址范围内是否已经存在其他 VMA，防止重叠。
    4.  `vma_create` 分配结构体内存。
    5.  `insert_vma_struct` 将新 VMA 按地址顺序插入链表，并更新 `mm->map_count`。
*   **load_icode中的作用**: **(Step 3.5 & 4)** 根据 ELF 文件头信息，在内核数据结构中“登记”代码段、数据段、BSS段和用户栈的虚拟地址范围。此时**尚未分配物理内存**。

---

#### 物理内存管理 (PMM) 与页表相关函数

这一层主要负责实际物理页的分配，以及虚拟地址到物理地址映射关系（页表）的建立。

##### 3. `setup_pgdir`
**文件位置**: `kern/process/proc.c`
**源码**:
```c
static int
setup_pgdir(struct mm_struct *mm)
{
    struct Page *page;
    // 1. 分配一个物理页作为页目录表(PDT)
    if ((page = alloc_page()) == NULL)
    {
        return -E_NO_MEM;
    }
    pde_t *pgdir = page2kva(page);
    // 2. 拷贝内核页表项 (关键步骤)
    memcpy(pgdir, boot_pgdir_va, PGSIZE);

    mm->pgdir = pgdir;
    return 0;
}
```
**具体实现与功能**:
*   **实现**: 调用 `alloc_page` 获取一页物理内存，通过 `page2kva` 拿到其内核虚拟地址。然后使用 `memcpy` 将系统启动时建立的 `boot_pgdir_va` 的内容复制过去。
*   **load_icode中的作用**: **(Step 2)** 建立进程的一级页表。复制 `boot_pgdir_va` 是为了确保用户进程陷入内核（系统调用/中断）时，内核代码和数据的映射依然存在且正确。

##### 4. `pgdir_alloc_page`
**文件位置**: `kern/mm/pmm.c`
**源码**:
```c
struct Page *pgdir_alloc_page(pde_t *pgdir, uintptr_t la, uint32_t perm)
{
    // 1. 分配物理页
    struct Page *page = alloc_page();
    if (page != NULL)
    {
        // 2. 在页表中建立映射
        if (page_insert(pgdir, page, la, perm) != 0)
        {
            free_page(page);
            return NULL;
        }
        page->pra_vaddr = la; // 记录反向映射，用于页面置换
        assert(page_ref(page) == 1);
    }
    return page;
}
```
**具体实现与功能**:
*   **实现**: 这是一个组合函数，先调 `alloc_page` 拿物理内存，再调 `page_insert` 改写页表。如果映射失败，会回滚释放刚才申请的页。
*   **load_icode中的作用**: **(Step 3.6.1 & 4)** 这是**真正分配内存**的时刻。它将物理内存与 `mm_map` 规划的虚拟地址联系起来。

##### 5. `alloc_pages`
**文件位置**: `kern/mm/pmm.c`
**源码**:
```c
struct Page *alloc_pages(size_t n)
{
    struct Page *page = NULL;
    bool intr_flag;
    // 关中断，保证分配过程原子性
    local_intr_save(intr_flag);
    {
        // 调用底层分配算法 (如 First Fit)
        page = pmm_manager->alloc_pages(n);
    }
    local_intr_restore(intr_flag);
    return page;
}
```
**具体实现与功能**:
*   **实现**: 封装了底层的物理内存分配器 (`pmm_manager`)，并在调用前后开关中断，防止分配过程中被中断打断导致数据结构不一致。
*   **load_icode中的作用**: 为代码、数据、栈以及页表本身提供物理存储空间。

##### 6. `page_insert`
**文件位置**: `kern/mm/pmm.c`
**源码**:
```c
int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm)
{
    // 1. 获取(或创建)页表项 PTE
    pte_t *ptep = get_pte(pgdir, la, 1);
    if (ptep == NULL)
    {
        return -E_NO_MEM;
    }
    page_ref_inc(page); // 增加物理页引用计数
    // 2. 如果该地址原本已存在映射，处理旧页
    if (*ptep & PTE_V)
    {
        struct Page *p = pte2page(*ptep);
        if (p == page)
        {
            page_ref_dec(page); // 重新映射同一页，引用计数复原
        }
        else
        {
            page_remove_pte(pgdir, la, ptep); // 移除旧映射
        }
    }
    // 3. 设置新 PTE：写入物理页号(PPN)和权限位
    *ptep = pte_create(page2ppn(page), PTE_V | perm);
    // 4. 刷新 TLB
    tlb_invalidate(pgdir, la);
    return 0;
}
```
**具体实现与功能**:
*   **实现**: 
    1.  调用 `get_pte` 找到虚拟地址对应的页表项地址。
    2.  维护 `Page` 结构的引用计数 (`page_ref`)。
    3.  构造 PTE 值（物理页号 + 有效位 + 权限位）并写入内存。
    4.  调用 `tlb_invalidate` 确保 CPU 缓存的旧映射失效。
*   **load_icode中的作用**: 完成虚拟地址到物理地址的最终绑定。

##### 7. `get_pte`
**文件位置**: `kern/mm/pmm.c`
**源码**:
```c
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create)
{
    // 1. 查找一级页目录项
    pde_t *pdep1 = &pgdir[PDX1(la)];
    if (!(*pdep1 & PTE_V))
    {
        struct Page *page;
        // 如果不存在且不创建，返回NULL；否则分配页表页
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE); // 清空新分配的页表
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }

    // 2. 查找二级页目录项 (SV39三级结构中的中间层)
    pde_t *pdep0 = &((pde_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)];
    if (!(*pdep0 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep0 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    
    // 3. 返回三级(叶子)页表项的内核虚拟地址
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}
```
**具体实现与功能**:
*   **实现**: 模拟硬件 MMU 的页表遍历过程（SV39 模式）。如果中间某一级页表不存在且 `create=true`，会自动分配一个物理页作为新的中间页表，并建立上一级对它的映射。
*   **load_icode中的作用**: 支持 `page_insert`，负责按需创建多级页表结构。

---

#### 总结：函数调用流

在 `load_icode` 的执行过程中，这些函数是这样协作的：

1.  **`mm_create()`**: 造一个空的内存管理器。
2.  **`setup_pgdir()`**: 造一个空的页表（带内核映射）。
3.  **循环 ELF 段**:
    *   **`mm_map()`**: 在管理器里划地盘（虚拟地址）。
    *   **`pgdir_alloc_page()`**:
        *   调用 **`alloc_pages()`**: 拿物理内存。
        *   调用 **`page_insert()`**:
            *   调用 **`get_pte()`**: 找页表项（找不到就自动造页表）。
            *   写入映射。
    *   **`page2kva()`** (宏): 拿到物理内存的内核地址，方便 `memcpy` 往里填代码。
4.  最后切换 `satp`，一切生效。

### 复习RISCV特权态
RISC-V 架构的设计哲学非常简洁且模块化，其特权级机制是保障系统安全性、稳定性和隔离性的基石。

---

### 1. 特权级总览图


| 等级 | 编码 | 名称 (缩写) | 角色类比 | 主要职责 | 典型运行软件 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Level 3** | 11 | **Machine (M)** | **公司创始人/大老板** | 掌控一切硬件，不可被剥夺的最高权限 | OpenSBI, BIOS, Firmware |
| **Level 1** | 01 | **Supervisor (S)** | **部门经理** | 管理员工，分配资源，处理突发状况 | Linux Kernel, uCore OS |
| **Level 0** | 00 | **User (U)** | **普通员工** | 专心干活，无权直接动用公司资产 | Shell, GCC, Python, ls |

---

### 2. 深度解析各特权级

#### (1) M 态：Machine Mode（机器模式）
这是 RISC-V 系统复位（Reset）后进入的**第一个模式**，也是**唯一必须实现**的模式。

*   **特点**：
    *   **物理地址访问**：M 态通常直接操作物理内存，不经过 MMU（除非配置了特定的 M 态页表，但很少见）。
    *   **掌控硬件**：可以访问所有的控制状态寄存器（CSR）。
    *   **拦截中断**：默认情况下，所有的硬件中断（时钟、外部设备）都会先发给 M 态。M 态可以选择自己处理，也可以通过“中断委托”（Delegation）转发给 S 态处理。
*   **PMP (Physical Memory Protection)**：
    *   M 态拥有PMP。它可以划定物理内存的范围，规定哪些物理地址 S 态和 U 态可以访问，哪些不行。这是 OpenSBI 保护自己不被 OS 内核（S 态）破坏的机制。
*   **在 uCore 中的表现**：
    *   当你启动 qemu 时，最先运行的是 OpenSBI（M 态）。它完成硬件初始化后，会跳转到 `0x80200000`（或其他约定地址），并将特权级切换到 S 态，把控制权交给 uCore。

#### (2) S 态：Supervisor Mode（监管模式）
这是现代操作系统内核运行的模式。

*   **特点**：
    *   **虚拟内存管理 (MMU)**：S 态最重要的能力是控制 **SATP** 寄存器（Supervisor Address Translation and Protection）。通过它，内核开启分页机制，建立页表，将虚拟地址映射到物理地址。
    *   **管理 U 态**：S 态负责切换 U 态程序的上下文，处理 U 态发出的系统调用（Syscall）。
    *   **受限的硬件访问**：S 态不能直接执行某些 M 态指令。比如，S 态想复位时钟或者重启系统，通常不能直接写硬件寄存器，而是要通过 `ecall` 指令发起 **SBI 调用**，请求 M 态帮忙
*   **异常处理**：
    *   当 U 态程序发生除零错误、缺页异常或执行 `ecall` 时，CPU 会自动跳转到 S 态的中断入口（`stvec` 指向的地址）进行处理。

#### (3) U 态：User Mode（用户模式）
这是应用程序运行的地方。

*   **特点**：
    *   **极度受限**：不能访问任何特权 CSR 寄存器（如 `sstatus`, `satp`）。
    *   **不可见物理地址**：U 态程序只能看到虚拟地址。如果它尝试访问页表中未映射的地址，或者没有权限的地址（比如内核空间），CPU 会立即抛出异常（Page Fault），控制权交回 S 态。
    *   **被动**：U 态程序想要读写硬盘、在屏幕打印字符，必须通过 `ecall` 指令发起系统调用，让内核代劳。

---

### 3. 特权级切换机制（核心知识点）

在 uCore 实验中，最关键的就是 S 态和 U 态之间的反复横跳。

#### **向上切换（Trap / Exception）：U -> S**
当 CPU 在 U 态执行时，遇到以下情况会强制切换到 S 态：
1.  **系统调用**：用户程序执行 `ecall` 指令。
2.  **异常**：指令非法、除零、缺页。
3.  **中断**：时钟中断（时间片用完了）、磁盘中断。

**硬件自动完成的动作**（以 U -> S 为例）：
1.  保存当前 PC 到 **`sepc`** (Supervisor Exception Program Counter)。
2.  保存当前 U 态的 Cause 到 **`scause`**。
3.  将当前状态（开/关中断等）保存到 **`sstatus`** 的 `SPIE` (Previous Interrupt Enable) 和 `SPP` (Previous Privilege) 位。
4.  **将 `sstatus` 中的 `SPP` 设为 0 (代表来自 User 态)**。
5.  **关闭中断** (`sstatus.SIE` 置 0)。
6.  跳转到 **`stvec`** 寄存器指向的内核中断处理代码。

#### **向下切换（Return）：S -> U**
当内核处理完系统调用或中断后，需要返回用户程序继续执行。这正是 `load_icode` 第 6 步要准备的事情。

**软件（内核）需要做的准备**：
1.  构造好 **`sstatus`**：将 `SPP` 位清零（表示目标是 U 态），将 `SPIE` 置 1（表示回 U 态后开启中断）。
2.  构造好 **`sepc`**：填入用户程序继续执行的地址（`elf->e_entry`）。

**指令执行**：
*   内核执行 **`sret`** (Supervisor Return) 指令。

**硬件自动完成的动作**：
1.  从 `sepc` 读取地址，赋值给 PC。
2.  从 `sstatus` 读取 `SPP`，切换特权级（变为 U 态）。
3.  从 `sstatus` 读取 `SPIE`，恢复中断使能状态。

---

### 核心态向用户态转变的难点

#### 1. 核心矛盾：特权隔离与服务需求的冲突
*   用户程序运行在受限的 U 态，无法直接操作硬件（如内存分配、IO输出），但又必须依赖这些资源才能运行。
*   **特权级分析**：
    *   **隔离性 (Isolation)**：这是 RISC-V 架构设计的初衷。**S 态**（内核）拥有管理 `satp`（页表基址）和 `sstatus`（中断状态）的权力，而 **U 态**（用户）被剥夺了这些权力。
    *   **安全性**：如果允许用户程序直接访问硬件，任何一个程序的崩溃都可能导致整个系统瘫痪。因此，硬件上必须强制实施这种“阶级壁垒”。

#### 2. 解决方案：系统调用（System Call）作为跨特权级桥梁
*   系统调用是连接 U 态和 S 态的标准化接口（如 `write`）。
*   **特权级分析（动态交互流）**：
    *   **上升通道 (`ecall`)**：当用户程序执行 `ecall` 指令时，CPU 会**自动**将特权级从 **U 态提升到 S 态**。
        *   此时，硬件会保存 `sepc`（记录 U 态刚才执行到哪）和 `scause`（记录是因为 `ecall` 导致的中断）。
        *   控制权移交给 `stvec` 指向的内核中断处理程序（Trap Handler）。
    *   **下降通道 (`sret`)**：内核处理完请求后，执行 `sret` 指令，CPU 会**自动**将特权级从 **S 态降低回 U 态**，并跳转回 `sepc` 继续执行。

#### 3. 实现难点：第一个进程的“无中生有”机制（鸡生蛋问题）
这是 Lab 5 最核心的逻辑，也是 `load_icode` 函数存在的意义。

*   **问题描述**：
    *   正常流程是“用户态主动陷入 -> 内核处理 -> 返回用户态”。
    *   但系统启动时，CPU **一直处于 S 态**（内核态），不存在一个“之前的用户态”可以返回。
    *   因此，必须在内核态“手动触发”一次返回流程，来启动第一个用户进程。

*   **特权级分析（静态构造流）**：
    *   **伪造现场**：这就是我们在 `load_icode` 第 6 步做的事情。我们手动填充了一个 `trapframe`（中断帧）。
        *   我们假装 `sstatus` 中的 `SPP` 是 0（假装之前的特权级是 U 态）。
        *   我们假装 `epc` 是用户程序的入口地址。
    *   **欺骗硬件**：当内核调用 `forkrets` -> `__trapret` 并最终执行 `sret` 时：
        *   硬件读取我们伪造的 `SPP=0`，于是**乖乖地将 CPU 降级为 U 态**。
        *   硬件读取我们伪造的 `epc`，于是**跳转到用户程序第一行代码**。

#### 总结
Lab 5 的本质任务：
**利用 S 态的最高权限，精心编造一个“虚假的”中断历史（Trapframe），通过执行 `sret` 指令，“欺骗” CPU 以为自己是从用户态陷入进来的，从而顺理成章地“返回”到我们想要启动的第一个用户进程中去。**

### 用户态进程的创建逻辑

这段文本详细描述了在 uCore (Lab 5) 环境下，系统是如何打破“始终在内核态”的局面，创建并运行**第一个用户进程**的。

围绕“第一个用户进程的创建”，核心知识点可以总结为以下 4 个阶段：

#### 1. 孕育阶段：从内核线程开始
*   **发起者**：`initproc`（PID=1，内核初始化进程）。
*   **动作**：在 `init_main` 函数中，调用 `kernel_thread(user_main, ...)`。
*   **结果**：创建了一个名为 `user_main` 的**内核线程**（通常 PID=2）。
*   **状态**：此时，`user_main` 仍然运行在 **S 态（内核态）**，共享内核内存空间，还不是真正的用户进程。父进程 `init_main` 会调用 `do_wait` 等待它结束。

### 2. 准备阶段：获取用户程序二进制代码
*   **问题**：文件系统尚未实现（Lab 8 才会有），用户程序的代码在哪里？
*   **解决方案**：**静态链接嵌入**。
    *   用户程序（如 `user/exit.c`）在编译时被编译成二进制文件。
    *   通过 `Makefile` 和链接器脚本，这些二进制数据被直接“打入”内核的镜像文件中。
    *   **获取方式**：代码通过特定的链接符号（如 `_binary_obj___user_exit_out_start`）来定位这些二进制数据在内存中的起始位置和大小。

### 3. 蜕变阶段：从内核态到用户态
*   **关键函数**：`user_main` 线程内部调用了 `kernel_execve`。
*   **执行逻辑**：
    1.  `kernel_execve` 内部调用核心函数 `do_execve` -> `load_icode`。
    2.  **清洗**：清空当前进程（`user_main`）原有的内存空间。
    3.  **加载**：将第 2 步中获取的用户程序二进制代码拷贝到新分配的内存页中。
    4.  **伪造现场**：设置 `Trapframe`，将 SP 指向用户栈，EPC 指向用户程序入口，特权级设置为 U 态（这是我们之前分析的 `load_icode` 第 6 步）。
    5.  **切换**：函数返回，执行 `sret`。
*   **结果**：CPU 特权级从 S 态降为 U 态，PC 跳转到用户程序入口。`user_main` 从一个内核线程“变身”为第一个真正的用户进程。

### 4. 运行阶段：用户库与系统调用
*   **环境限制**：用户进程运行在 U 态，不能直接访问硬件（如不能直接调用 OpenSBI 打印字符）。
*   **适配层（User Library）**：
    *   用户程序调用的是 `user/libs/ulib.c` 或 `stdio.c` 中的封装函数（如 `fork`, `exit`, `cprintf`）。
    *   **差异点**：
        *   内核态的 `cprintf` 直接调用底层硬件接口。
        *   用户态的 `cprintf` 调用 `sys_putc`，这是一个系统调用。
*   **系统调用机制**：这些库函数最终执行 `ecall` 指令，再次陷入内核，请求操作系统代为完成任务。

---

**总结：**
第一个用户进程并非直接由用户创建，而是由内核线程 `user_main` 通过“**自我重塑**”（执行 `exec` 加载内嵌二进制代码）的方式，利用中断返回机制（`sret`）主动降级特权级而诞生的。

### 系统调用相关知识
基于提供的文本，以下是对系统调用（System Call）核心知识点的总结，以及对应的系统功能列表。

### 一、 系统调用核心知识点总结

系统调用是 uCore 操作系统中用户态与内核态交互的桥梁，其实现涉及硬件指令、寄存器约定、中断处理和函数分发四个维度：

1.  **触发机制 (`ecall`)**
    *   **动作**：用户程序执行 RISC-V 的 `ecall` 指令。
    *   **效果**：触发同步异常（Exception），CPU 特权级从 **U 态（User Mode）** 提升至 **S 态（Supervisor Mode）**，程序计数器（PC）跳转至内核的中断处理入口。

2.  **参数传递约定 (ABI)**
    *   **a0 寄存器**：具有双重作用。
        *   **输入时**：传递**系统调用编号**（如 `SYS_exit` 是 1）。
        *   **返回时**：存放系统调用的**返回值**。
    *   **a1 ~ a5 寄存器**：用于传递系统调用的具体参数（最多支持 5 个参数）。

3.  **内核中断处理 (Trap Handler)**
    *   **识别**：在 `trap.c` 中，通过检查 `scause` 寄存器的值是否为 `CAUSE_USER_ECALL` 来识别系统调用。
    *   **关键修正 (`epc += 4`)**：`ecall` 指令长度为 4 字节。内核处理完系统调用后，必须将记录异常地址的 `epc` 寄存器加 4，否则返回用户态后会再次执行 `ecall` 指令，导致死循环。

4.  **分发机制 (Dispatcher)**
    *   **查表法**：内核维护了一个函数指针数组 `syscalls[]`。
    *   **流程**：`syscall()` 函数从 Trapframe 中读取 `a0` 获取编号 -> 检查编号合法性 -> 以编号为下标从 `syscalls[]` 找到对应的内核函数（如 `sys_fork`）-> 执行并获取结果 -> 将结果写回 Trapframe 的 `a0`。

5.  **实现分层**
    *   **用户层**：`user/libs/syscall.c` 封装内联汇编，对外提供 C 语言接口。
    *   **接口层**：`kern/syscall/syscall.c` 负责参数提取和分发。
    *   **实现层**：`kern/process/proc.c` 等文件包含真正的逻辑实现（如 `do_fork`, `do_exit`）。

---

### 二、 系统调用功能列表

| 宏定义名称 (Macro) | 编号 (ID) | 功能描述 (Description) | 对应内核实现函数 |
| :--- | :---: | :--- | :--- |
| **SYS_exit** | 1 | **进程终止**。终止当前进程，释放资源，返回值返给父进程。 | `do_exit` |
| **SYS_fork** | 2 | **进程创建**。复制当前进程，创建一个子进程。 | `do_fork` |
| **SYS_wait** | 3 | **进程等待**。挂起当前进程，直到子进程退出。 | `do_wait` |
| **SYS_exec** | 4 | **程序加载**。加载并执行一个新的程序，替换当前进程内存。 | `do_execve` |
| **SYS_clone** | 5 | **线程创建**。创建轻量级进程（线程），通常共享内存。 | `do_fork` |
| **SYS_yield** | 10 | **让出 CPU**。主动放弃当前时间片，触发调度。 | `do_yield` |
| **SYS_sleep** | 11 | **进程休眠**。使进程进入睡眠状态一段时间。 | `do_sleep` (推测) |
| **SYS_kill** | 12 | **杀死进程**。向指定 PID 的进程发送终止信号。 | `do_kill` |
| **SYS_gettime** | 17 | **获取时间**。获取系统当前时间。 | - |
| **SYS_getpid** | 18 | **获取 PID**。获取当前进程的进程 ID。 | `current->pid` |
| **SYS_brk** | 19 | **堆管理**。改变数据段的大小（用于动态内存分配）。 | - |
| **SYS_mmap** | 20 | **内存映射**。将文件或设备映射到内存。 | - |
| **SYS_munmap** | 21 | **取消映射**。取消之前的内存映射。 | - |
| **SYS_shmem** | 22 | **共享内存**。创建或访问共享内存段。 | - |
| **SYS_putc** | 30 | **字符输出**。向控制台输出一个字符（主要用于 print）。 | `cputchar` |
| **SYS_pgdir** | 31 | **页目录操作**。可能涉及页表相关信息的获取或检查。 | - |

#### 系统调用大致流程

基于 uCore 的实现和 RISC-V 架构，系统调用的处理流程是一个严密的闭环，涉及从**用户态准备**到**内核态处理**再到**返回用户态**的完整过程。

我们可以将其分为四个阶段来总结：

##### 第一阶段：用户态准备与触发 (Preparation & Trigger)

1.  **API 调用**：用户程序调用库函数（如 `fork()` 或 `printf()`）。
2.  **封装层 (`user/libs/syscall.c`)**：库函数内部调用通用的 `syscall(num, ...)` 函数。
3.  **寄存器传参**：
    *   将 **系统调用编号** 放入 `a0` 寄存器。
    *   将 **参数** 依次放入 `a1` ~ `a5` 寄存器。
4.  **指令触发**：执行 **`ecall`** 指令。此时 CPU 产生同步异常，硬件自动保存现场，将特权级从 U 态（User）提升至 S 态（Supervisor），并跳转到内核中断入口。

##### 第二阶段：内核中断分发 (Trap Handling)

1.  **中断入口**：进入 `trapentry.S`（汇编），保存当前所有寄存器到 **Trapframe**（中断帧）中。
2.  **异常识别 (`kern/trap/trap.c`)**：调用 `exception_handler(tf)`。
    *   检查 `tf->cause` 寄存器，确认异常原因为 `CAUSE_USER_ECALL`。
3.  **指针修正 (关键)**：执行 `tf->epc += 4`。
    *   **原因**：`epc` 记录的是触发异常的那条指令（即 `ecall`）的地址。如果不加 4，处理完返回后 CPU 会再次执行 `ecall`，导致死循环。加 4 是为了让返回后的 PC 指向 `ecall` 的下一条指令。
4.  **进入分发器**：调用内核层的 `syscall()` 函数。

##### 第三阶段：系统调用分发与执行 (Dispatch & Execution)

1.  **参数提取 (`kern/syscall/syscall.c`)**：
    *   从 `current->tf`（当前进程的中断帧）中读取 `a0` 获取系统调用编号。
    *   从 `tf->gpr.a1` ~ `a5` 中读取参数。
2.  **查表分发**：
    *   检查编号合法性。
    *   使用编号作为下标，在函数指针数组 `syscalls[]` 中找到对应的内核处理函数（如 `sys_fork`, `sys_exit`）。
3.  **具体执行**：
    *   执行对应的内核函数（如 `sys_fork` 最终调用 `do_fork`）。
    *   函数执行完毕后，返回一个整数结果。
4.  **保存返回值**：
    *   将内核函数的返回值写入中断帧的 `a0` 寄存器：`tf->gpr.a0 = ret`。
    *   **注意**：这就通过修改内存中的备份数据，改变了未来返回用户态时 CPU 真实 `a0` 寄存器的值。

##### 第四阶段：恢复现场与返回 (Restoration & Return)

1.  **退出中断**：系统调用函数返回，逐层退回到汇编层 `__trapret`。
2.  **恢复寄存器**：根据修改后的 Trapframe，将所有通用寄存器恢复到 CPU 中（此时 CPU 的 `a0` 变成了刚才的返回值）。
3.  **降级返回**：执行 **`sret`** 指令。
    *   CPU 特权级从 S 态降回 U 态。
    *   PC 跳转到 `sepc` 指向的地址（即 `ecall` 的下一条指令）。
4.  **继续运行**：用户程序从 `syscall()` 函数返回，检查 `a0` 寄存器拿到返回值，继续执行后续代码。

---
