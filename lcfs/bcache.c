#include "includes.h"

/* Return the hash number for the block number provided */
static inline int
lc_pageBlockHash(uint64_t block) {
    assert(block);
    assert(block != LC_INVALID_BLOCK);
    return block % LC_PCACHE_SIZE;
}

/* Allocate a new page */
static struct page *
lc_newPage(struct gfs *gfs) {
    struct page *page = malloc(sizeof(struct page));

    page->p_data = NULL;
    page->p_block = LC_INVALID_BLOCK;
    page->p_refCount = 1;
    page->p_hitCount = 0;
    page->p_cnext = NULL;
    page->p_dnext = NULL;
    page->p_dvalid = 0;
    pthread_mutex_init(&page->p_dlock, NULL);
    __sync_add_and_fetch(&gfs->gfs_pcount, 1);
    return page;
}

/* Free a page */
static void
lc_freePage(struct gfs *gfs, struct page *page) {
    assert(page->p_refCount == 0);
    assert(page->p_block == LC_INVALID_BLOCK);
    assert(page->p_cnext == NULL);
    assert(page->p_dnext == NULL);

    if (page->p_data) {
        free(page->p_data);
    }
    pthread_mutex_destroy(&page->p_dlock);
    free(page);
    __sync_sub_and_fetch(&gfs->gfs_pcount, 1);
}

/* Allocate and initialize page block hash table */
struct pcache *
lc_pcache_init() {
    struct pcache *pcache = malloc(sizeof(struct pcache) * LC_PCACHE_SIZE);
    int i;

    for (i = 0; i < LC_PCACHE_SIZE; i++) {
        pthread_mutex_init(&pcache[i].pc_lock, NULL);
        pcache[i].pc_head = NULL;
        pcache[i].pc_pcount = 0;
    }
    return pcache;
}

/* Remove pages from page cache and free the hash table */
void
lc_destroy_pages(struct gfs *gfs, struct pcache *pcache, bool remove) {
    uint64_t i, count = 0, pcount;
    struct page *page;

    for (i = 0; i < LC_PCACHE_SIZE; i++) {
        pcount = 0;
        pthread_mutex_lock(&pcache[i].pc_lock);
        while ((page = pcache[i].pc_head)) {
            pcache[i].pc_head = page->p_cnext;
            page->p_block = LC_INVALID_BLOCK;
            page->p_cnext = NULL;
            page->p_dvalid = 0;
            lc_freePage(gfs, page);
            pcount++;
        }
        assert(pcount == pcache[i].pc_pcount);
        assert(pcache[i].pc_head == NULL);
        pthread_mutex_unlock(&pcache[i].pc_lock);
        pthread_mutex_destroy(&pcache[i].pc_lock);
        count += pcount;
    }
    free(pcache);
    if (count && remove) {
        __sync_add_and_fetch(&gfs->gfs_preused, count);
    }
}

/* Release a page */
void
lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page, bool read) {
    struct page *cpage, *prev = NULL, *fpage = NULL, *fprev = NULL;
    struct pcache *pcache = fs->fs_pcache;
    uint64_t hash, hit;

    hash = lc_pageBlockHash(page->p_block);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    assert(page->p_refCount > 0);
    page->p_refCount--;

    /* If page was read, increment hit count */
    if (read) {
        page->p_hitCount++;
    }

    /* Free a page if page cache is above limit */
    if (pcache[hash].pc_pcount > (LC_PAGE_MAX / LC_PCACHE_SIZE)) {
        cpage = pcache[hash].pc_head;
        hit = page->p_hitCount;

        /* Look for a page with lowest hit count */
        while (cpage) {
            if ((cpage->p_refCount == 0) && (cpage->p_hitCount <= hit)) {
                fprev = prev;
                fpage = cpage;
                hit = cpage->p_hitCount;
            }
            prev = cpage;
            cpage = cpage->p_cnext;
        }

        /* If a page is picked for freeing, take off page from block cache */
        if (fpage) {
            if (fprev == NULL) {
                pcache[hash].pc_head = fpage->p_cnext;
            } else {
                fprev->p_cnext = fpage->p_cnext;
            }
            fpage->p_block = LC_INVALID_BLOCK;
            fpage->p_cnext = NULL;
            pcache[hash].pc_pcount--;
        }
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);
    if (fpage) {
        lc_freePage(gfs, fpage);
        __sync_add_and_fetch(&gfs->gfs_precycle, 1);
    }
}

/* Release a linked list of pages */
void
lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head) {
    struct page *page = head, *next;

    while (page) {
        next = page->p_dnext;
        page->p_dnext = NULL;

        /* If block is not in the cache, free it */
        if (page->p_block == LC_INVALID_BLOCK) {
            assert(page->p_refCount == 1);
            page->p_refCount = 0;
            lc_freePage(gfs, page);
        } else {
            lc_releasePage(gfs, fs, page, false);
        }
        page = next;
    }
}

/* Release pages */
void
lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                    struct page **pages, uint64_t pcount) {
    uint64_t i;

    for (i = 0; i < pcount; i++) {
        lc_releasePage(gfs, fs, pages[i], true);
    }
}

/* Add a page to page block hash list */
void
lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                    struct page *page, uint64_t block) {
    struct pcache *pcache = fs->fs_pcache;
    int hash = lc_pageBlockHash(block);
    struct page *cpage;

    assert(page->p_block == LC_INVALID_BLOCK);
    page->p_block = block;
    pthread_mutex_lock(&pcache[hash].pc_lock);
    cpage = pcache[hash].pc_head;

    /* Invalidate previous instance of this block if there is one */
    while (cpage) {
        if (cpage->p_block == block) {
            assert(cpage->p_refCount == 0);
            cpage->p_block = LC_INVALID_BLOCK;
            break;
        }
        cpage = cpage->p_cnext;
    }
    page->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = page;
    pcache[hash].pc_pcount++;
    pthread_mutex_unlock(&pcache[hash].pc_lock);
}

/* Lookup a page in the block hash */
struct page *
lc_getPage(struct fs *fs, uint64_t block, bool read) {
    int hash = lc_pageBlockHash(block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    bool hit = false;

    assert(block);
    assert(block != LC_PAGE_HOLE);

retry:
    pthread_mutex_lock(&pcache[hash].pc_lock);
    page = pcache[hash].pc_head;
    while (page) {

        /* If a page is found, increment reference count */
        if (page->p_block == block) {
            page->p_refCount++;
            hit = true;
            break;
        }
        page = page->p_cnext;
    }

    /* If page is not found, instantiate one */
    if ((page == NULL) && new) {
        page = new;
        new = NULL;
        page->p_block = block;
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
        pcache[hash].pc_pcount++;
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);

    /* If no page is found, allocate one and retry */
    if (page == NULL) {
        new = lc_newPage(gfs);
        assert(!new->p_dvalid);
        goto retry;
    }

    /* If raced with another thread, free the unused page */
    if (new) {
        new->p_refCount = 0;
        lc_freePage(gfs, new);
    }

    /* If page is missing data, read from disk */
    if (read && !page->p_dvalid) {

        /* XXX Use a shared lock instead of a per page one */
        pthread_mutex_lock(&page->p_dlock);
        if (!page->p_dvalid) {
            page->p_data = lc_readBlock(gfs, fs, block, page->p_data);
            page->p_dvalid = 1;
        }
        pthread_mutex_unlock(&page->p_dlock);
    }
    assert(page->p_refCount > 0);
    assert(!read || page->p_data);
    assert(!read || page->p_dvalid);
    assert(page->p_block == block);
    if (hit) {
        __sync_add_and_fetch(&gfs->gfs_phit, 1);
    } else if (read) {
        __sync_add_and_fetch(&gfs->gfs_pmissed, 1);
    }
    return page;
}

/* Link new data to the page of the file */
struct page *
lc_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = lc_getPage(fs, block, false);

    assert(page->p_refCount == 1);
    if (page->p_data) {
        free(page->p_data);
    }
    page->p_data = data;
    page->p_dvalid = 1;
    page->p_hitCount = 0;
    return page;
}

/* Get a page with no block associated with.  Blocks will be allocated later
 * and the page will be added to hash then.  This interface for allocating
 * space contiguously for a set of pages.
 */
struct page *
lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                  struct page *prev) {
    struct page *page = lc_newPage(gfs);

    page->p_data = data;
    page->p_dvalid = 1;
    page->p_dnext = prev;
    return page;
}

/* Get a page for the block without reading it from disk, but making sure a
 * data buffer exists for copying in data.  New data will be copied to the
 * page by the caller.
 */
struct page *
lc_getPageNewData(struct fs *fs, uint64_t block) {
    struct page *page;

    page = lc_getPage(fs, block, false);
    if (page->p_data == NULL) {
        malloc_aligned((void **)&page->p_data);
    }
    page->p_hitCount = 0;
    return page;
}

/* Flush a cluster of pages */
void
lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                    struct page *head, uint64_t count) {
    uint64_t i, j, bcount = 0;
    struct page *page = head;
    struct iovec *iovec;
    uint64_t block = 0;

    if (count == 1) {
        block = page->p_block;
        lc_writeBlock(gfs, fs, page->p_data, block);
    } else {
        iovec = alloca(count * sizeof(struct iovec));

        /* Issue the I/O in block order */
        for (i = 0, j = count - 1; i < count; i++, j--) {

            /* Flush current set of dirty pages if the new page is not adjacent
             * to those.
             * XXX This could happen when metadata and userdata are flushed
             * concurrently OR files flushed concurrently.
             */
            if (i && ((page->p_block + 1) != block)) {
                lc_printf("Not contigous, block %ld previous block %ld i %ld count %ld\n", block, page->p_block, i, count);
                lc_writeBlocks(gfs, fs, &iovec[j + 1], bcount, block);
                bcount = 0;
            }
            iovec[j].iov_base = page->p_data;
            iovec[j].iov_len = LC_BLOCK_SIZE;
            block = page->p_block;
            bcount++;
            page = page->p_dnext;
        }
        assert(page == NULL);
        assert(block != 0);
        lc_writeBlocks(gfs, fs, iovec, bcount, block);
    }

    /* Release the pages after writing */
    lc_releasePages(gfs, fs, head);
}

/* Add a page to the file system dirty list for writeback */
void
lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                       struct page *tail, uint64_t pcount) {
    struct page *page = NULL;
    uint64_t count = 0;

    assert(count < LC_CLUSTER_SIZE);
    pthread_mutex_lock(&fs->fs_plock);
    tail->p_dnext = fs->fs_dpages;
    fs->fs_dpages = head;
    fs->fs_dpcount += pcount;

    /* Issue write when a certain number of dirty pages accumulated */
    if (fs->fs_dpcount >= LC_CLUSTER_SIZE) {
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
    }
    pthread_mutex_unlock(&fs->fs_plock);
    if (count) {
        lc_flushPageCluster(gfs, fs, page, count);
    }
}

/* Flush dirty pages of a file system before unmounting it */
void
lc_flushDirtyPages(struct gfs *gfs, struct fs *fs) {
    struct page *page;
    uint64_t count;

    if (fs->fs_dpcount && !fs->fs_removed) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        if (count) {
            lc_flushPageCluster(gfs, fs, page, count);
        }
    }
}

/* Invalidate dirty pages */
void
lc_invalidateDirtyPages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_dpcount) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        lc_releasePages(gfs, fs, page);
    }
}

