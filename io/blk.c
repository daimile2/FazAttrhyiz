#include "capmix/io.h"
#include "capmix/disk_layout.h"
#include "capmix/boot.h"
#include "capmix/ahci.h"
#include "capmix/virtio_blk.h"
#include "capmix/mm.h"
#include "capmix/klog.h"
#include "capmix/obs.h"

#include <string.h>
#ifdef CAPMIX_HOSTED
#include <stdlib.h>
#endif

/* 8 MiB ramdisk. Bare metal takes it from the buddy (one order-11 block)
 * so it lives in HHDM-mapped RAM like every other kernel allocation; hosted
 * mallocs it so it does not eat a third of the 16 MiB simulated arena the
 * pressure selftest is trying to fill. */
static uint8_t *g_disk;
static cm_bio_t g_q[CM_BLK_QUEUES][CM_BLK_QD];
static int      g_qh[CM_BLK_QUEUES], g_qt[CM_BLK_QUEUES], g_qn[CM_BLK_QUEUES];
static cm_bio_t g_done[CM_BLK_QD];
static int      g_dh, g_dt, g_dn;
static uint64_t g_done_total;
static uint64_t g_tag_next = 1;
_Static_assert(CM_INSTALL_ROOT_SECT==CM_BLK_DISK_SECT,"installed volume preserves raw/swap/COW layout");
static uint64_t g_root_base[3];
static int g_layout_bad[3];
static int      g_backend, g_offline[3]; /* independently offline-able devices */

static void probe_layout(int backend)
{
    static uint8_t sector[512];uint64_t base=0;
    cm_error_t e=backend==CM_BLK_AHCI?cm_ahci_rw(CM_OP_READ,0,1,sector):cm_virtio_blk_rw(CM_OP_READ,0,1,sector);
    if(!e)e=cm_disk_layout_parse(sector,backend==CM_BLK_AHCI?cm_ahci_capacity():cm_virtio_blk_capacity(),&base);
    if(e==CM_ERR_NOENT){kinfo("disk install: %s legacy raw root (no install marker)",backend==CM_BLK_AHCI?"ahci":"virtio-blk");return;}
    if(e){g_layout_bad[backend]=1;kerr("disk install: layout rejected backend=%d error=%d root I/O disabled; no format",backend,e);return;}
    g_root_base[backend]=base;
    kinfo("disk install: layout verified backend=%d root_base_lba=%llu ESP separate, root window=8MiB",backend,(unsigned long)base);
}
uint64_t cm_blk_root_base(void){return g_root_base[g_backend];}
int cm_blk_installed(void){return g_backend!=CM_BLK_RAM && !g_layout_bad[g_backend] && g_root_base[g_backend]!=0;}

cm_error_t cm_blk_init(void)
{
    if (!g_disk) {
#ifdef CAPMIX_HOSTED
        g_disk = (uint8_t *)malloc((size_t)CM_BLK_DISK_SECT * CM_BLK_SECTOR);
#else
        g_disk = (uint8_t *)cm_page_alloc(11);   /* 2048 pages = 8 MiB */
#endif
    }
    if (!g_disk) {
        kerr("blk: cannot allocate %u-sector ramdisk", CM_BLK_DISK_SECT);
        return CM_ERR_NOMEM;
    }
    memset(g_disk, 0, (size_t)CM_BLK_DISK_SECT * CM_BLK_SECTOR);
    memset(g_q, 0, sizeof(g_q));
    memset(g_done, 0, sizeof(g_done));
    memset(g_qh, 0, sizeof(g_qh));
    memset(g_qt, 0, sizeof(g_qt));
    memset(g_qn, 0, sizeof(g_qn));
    g_dh = g_dt = g_dn = 0;
    g_done_total = 0;
    memset(g_offline,0,sizeof(g_offline));
    memset(g_root_base,0,sizeof(g_root_base));memset(g_layout_bad,0,sizeof(g_layout_bad));
    int virtio = cm_virtio_blk_init() == CM_OK;
    int ahci = cm_ahci_init() == CM_OK;
    if(virtio)probe_layout(CM_BLK_VIRTIO);
    if(ahci)probe_layout(CM_BLK_AHCI);
    g_backend = virtio?CM_BLK_VIRTIO:ahci?CM_BLK_AHCI:CM_BLK_RAM;
    if(!virtio && !ahci)kinfo("disk install skipped: no supported disk; RAM root fallback unchanged");
    if(cm_boot_has_option("capmix.installed=1") && !cm_blk_installed()) {
        g_layout_bad[g_backend]=1;
        kerr("disk install: installed boot refused; selected backend lacks verified partition; no RAM/raw root substitute");
    }
    kinfo("blk root backend: %s", cm_blk_backend_name());
    kinfo("blk: mq %d queues x qd %d, ramdisk %u sectors "
          "(raw %u / swap %u / fs %u)",
          CM_BLK_QUEUES, CM_BLK_QD, CM_BLK_DISK_SECT,
          CM_BLK_RAW_SECT, CM_BLK_SWAP_SECT, CM_BLK_FS_SECT);
    return CM_OK;
}

void cm_blk_set_offline(int off) { g_offline[g_backend] = off; }
int  cm_blk_online(void)         { return !g_layout_bad[g_backend] && !g_offline[g_backend] && (g_backend==CM_BLK_VIRTIO ? cm_virtio_blk_available() : g_backend==CM_BLK_AHCI ? cm_ahci_available() : g_disk != NULL); }

uint8_t *cm_blk_raw_sector(uint64_t lba)
{
    if (g_backend || !g_disk || lba >= CM_BLK_DISK_SECT) return NULL;
    return g_disk + lba * CM_BLK_SECTOR;
}

static void complete_one(cm_bio_t *b)
{
    if (g_dn >= CM_BLK_QD) return;
    g_done[g_dt] = *b;
    g_dt = (g_dt + 1) % CM_BLK_QD;
    g_dn++;
    g_done_total++;
}

static void service(cm_bio_t *b)
{
    if (!cm_blk_online()) {
        b->status = CM_ERR_DEAD;
        complete_one(b);
        return;
    }
    if (!b->buf || !b->nsect || b->lba >= CM_BLK_DISK_SECT || b->nsect > CM_BLK_DISK_SECT-b->lba ||
        (b->op != CM_OP_READ && b->op != CM_OP_WRITE)) {
        b->status = CM_ERR_INVAL;
        complete_one(b);
        return;
    }
    if (g_backend) {
        cm_error_t err=g_backend==CM_BLK_AHCI ? cm_ahci_rw(b->op,b->lba+g_root_base[g_backend],b->nsect,b->buf) : cm_virtio_blk_rw(b->op,b->lba+g_root_base[g_backend],b->nsect,b->buf);
        b->status=err ? err : (int32_t)(b->nsect*CM_BLK_SECTOR);
        complete_one(b); return;
    }
    uint8_t *sec = g_disk + b->lba * CM_BLK_SECTOR;
    size_t   n   = (size_t)b->nsect * CM_BLK_SECTOR;
    if (b->op == CM_OP_WRITE && b->buf)
        memcpy(sec, b->buf, n);
    else if (b->op == CM_OP_READ && b->buf)
        memcpy(b->buf, sec, n);
    b->status = (int32_t)n;
    complete_one(b);
}

cm_error_t cm_blk_submit(const cm_bio_t *bio)
{
    if (!bio) return CM_ERR_INVAL;
    int q = bio->qid % CM_BLK_QUEUES;
    if (q < 0) q = 0;
    if (g_qn[q] >= CM_BLK_QD) return CM_ERR_AGAIN;
    g_q[q][g_qt[q]] = *bio;
    g_qt[q] = (g_qt[q] + 1) % CM_BLK_QD;
    g_qn[q]++;
    /* Hosted: service inline after queueing so blk-mq shape stays,
     * completion still goes through the CQ. */
    cm_bio_t *b = &g_q[q][g_qh[q]];
    g_qh[q] = (g_qh[q] + 1) % CM_BLK_QD;
    g_qn[q]--;
    service(b);
    return CM_OK;
}

cm_error_t cm_blk_poll(cm_bio_t *out)
{
    if (!out || !g_dn) return CM_ERR_AGAIN;
    *out = g_done[g_dh];
    g_dh = (g_dh + 1) % CM_BLK_QD;
    g_dn--;
    return CM_OK;
}

uint64_t cm_blk_completed(void) { return g_done_total; }

/* Reap the completion carrying `tag` and leave every other entry in order.
 * cm_blk_poll() hands back the OLDEST completion, which may belong to the
 * uring path; a synchronous caller that took whatever came first would
 * steal someone else's result. */
static int reap_tag(uint64_t tag, cm_bio_t *out)
{
    for (int k = 0; k < g_dn; k++) {
        int i = (g_dh + k) % CM_BLK_QD;
        if (g_done[i].tag != tag) continue;
        *out = g_done[i];
        for (int m = k; m + 1 < g_dn; m++)
            g_done[(g_dh + m) % CM_BLK_QD] = g_done[(g_dh + m + 1) % CM_BLK_QD];
        g_dt = (g_dt + CM_BLK_QD - 1) % CM_BLK_QD;
        g_dn--;
        return 1;
    }
    return 0;
}

cm_error_t cm_blk_rw(uint32_t op, uint64_t lba, uint32_t nsect, void *buf)
{
    if (!buf || nsect == 0) return CM_ERR_INVAL;
    cm_bio_t bio;
    memset(&bio, 0, sizeof(bio));
    bio.op = op;
    bio.lba = lba;
    bio.nsect = nsect;
    bio.buf = buf;
    bio.qid = (int)(lba & (CM_BLK_QUEUES - 1));
    bio.tag = (1ull << 63) | g_tag_next++;
    cm_error_t e = cm_blk_submit(&bio);
    if (e != CM_OK) return e;
    cm_bio_t done;
    if (!reap_tag(bio.tag, &done)) return CM_ERR_AGAIN;
    return done.status < 0 ? (cm_error_t)done.status : CM_OK;
}

/* Selection is boot/test-controller only, with VFS unmounted and no I/O pending. */
int cm_blk_is_virtio(void) { return g_backend==CM_BLK_VIRTIO; }
int cm_blk_is_disk(void) { return g_backend!=CM_BLK_RAM; }
int cm_blk_backend(void) { return g_backend; }
const char *cm_blk_backend_name(void)
{ return g_backend==CM_BLK_VIRTIO?"virtio-blk":g_backend==CM_BLK_AHCI?"ahci":"ramdisk"; }
cm_error_t cm_blk_select(int virtio)
{ return cm_blk_select_backend(virtio?CM_BLK_VIRTIO:CM_BLK_RAM); }
cm_error_t cm_blk_select_backend(int backend)
{
    if(backend<CM_BLK_RAM || backend>CM_BLK_AHCI)return CM_ERR_INVAL;
    if (g_dn) return CM_ERR_BUSY;
    for (unsigned q=0;q<CM_BLK_QUEUES;q++) if(g_qn[q])return CM_ERR_BUSY;
    if (backend==CM_BLK_VIRTIO && !cm_virtio_blk_available())return CM_ERR_DEAD;
    if (backend==CM_BLK_AHCI && !cm_ahci_available())return CM_ERR_DEAD;
    g_backend=backend;
    kinfo("blk backend: %s offline=%d",cm_blk_backend_name(),g_offline[g_backend]);
    return CM_OK;
}
cm_error_t cm_blk_flush(void)
{
    if(!cm_blk_online())return CM_ERR_DEAD;
    return g_backend==CM_BLK_AHCI?cm_ahci_flush():g_backend==CM_BLK_VIRTIO?cm_virtio_blk_flush():CM_OK;
}
cm_error_t cm_blk_volume_blank(int *blank)
{
    if(!blank)return CM_ERR_INVAL;
    *blank=0;
    /* Refuse auto-format if any byte of the reserved 8 MiB volume is nonzero. */
    static uint8_t sector[4096];
    for(uint32_t lba=0;lba<CM_BLK_DISK_SECT;lba+=8) {
        cm_error_t err=cm_blk_rw(CM_OP_READ,lba,8,sector);
        if(err)return err;
        for(unsigned i=0;i<sizeof(sector);i++)if(sector[i])return CM_OK;
    }
    *blank=1;return CM_OK;
}
