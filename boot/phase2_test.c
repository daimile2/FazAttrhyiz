#include "capmix/kernel.h"
#include "capmix/elf.h"
#include "capmix/fs.h"
#include <string.h>
#include "../build/hello_elf.inc"

#define CHECK(expr) do { if (!(expr)) { \
    kerr("phase2: line %d: %s", __LINE__, #expr); failed++; goto cleanup; \
} } while (0)
#define TEST_CPL_VA 0x401040ull

static cm_error_t put_elf(const char *path, const void *bytes, uint64_t length)
{
    cm_file_t *f = NULL;
    cm_error_t err = cm_vfs_open(path, CM_O_WRONLY | CM_O_CREAT | CM_O_TRUNC, &f);
    if (err) return err;
    int64_t n = cm_vfs_write(f, bytes, length);
    err = n == (int64_t)length ? cm_vfs_fsync(f) : CM_ERR_IO;
    cm_vfs_close(f);
    return err;
}
static int malformed_tests(void)
{
    int failed = 0;
    static unsigned char bad[sizeof(cm_test_elf)];
    cm_elf_image_t *im = NULL;
    for (unsigned test = 0; test < 11; test++) {
        memcpy(bad, cm_test_elf, sizeof(bad));
        cm_elf64_ehdr_t eh;
        memcpy(&eh, bad, sizeof(eh));
        cm_elf64_phdr_t ph[2];
        memcpy(ph, bad + eh.phoff, sizeof(ph));
        cm_error_t expected = CM_ERR_INVAL;
        switch (test) {
        case 0: eh.ident[0] = 0; break;
        case 1: eh.phoff = UINT64_MAX; break;
        case 2: eh.type = 3; expected = CM_ERR_NOTSUP; break;
        case 3: ph[1].type = 3; expected = CM_ERR_NOTSUP; break;
        case 4: ph[0].filesz = ph[0].memsz + 1; break;
        case 5: ph[0].vaddr = 0xffffffff80000000ull; break;
        case 6: ph[0].memsz = UINT64_MAX; break;
        case 7: eh.entry = TEST_CPL_VA; break;
        case 8: ph[1].vaddr = ph[0].vaddr; break;
        case 9: ph[0].align = 3; break;
        case 10: ph[1].flags = 7; break;
        }
        /* Original header offset is valid, even when this case corrupts phoff. */
        cm_elf64_ehdr_t original;
        memcpy(&original, cm_test_elf, sizeof(original));
        memcpy(bad + original.phoff, ph, sizeof(ph));
        memcpy(bad, &eh, sizeof(eh));
        CHECK(put_elf("/bin/bad.elf", bad, sizeof(bad)) == CM_OK);
        CHECK(cm_elf_load("/bin/bad.elf", &im) == expected && !im);
    }
    CHECK(put_elf("/bin/bad.elf", cm_test_elf, sizeof(cm_elf64_ehdr_t)) == CM_OK);
    CHECK(cm_elf_load("/bin/bad.elf", &im) == CM_ERR_INVAL && !im);
    cm_spawn_attr_t missing = { .path = "/bin/missing.elf", .qos = CM_QOS_DEFAULT };
    cm_id_t pid = 99;
    CHECK(cm_posix_spawn(&missing, &pid) == CM_ERR_NOENT && pid == 99);
    kinfo("elf reject ok: malformed/truncated/overflow/overlap/entry/ET_DYN/PT_INTERP/W+X/missing");
cleanup:
    cm_elf_destroy(im);
    return failed;
}
#ifndef CAPMIX_HOSTED
static unsigned competitor_runs;
static void elf_competitor(void *arg)
{
    cm_proc_t *p = arg;
    while (!p->exited) {
        if (p->elf_started && p->elf_preempts > 0) competitor_runs++;
        cm_sched_yield();
    }
    cm_sched_current()->state = CM_TH_DEAD;
}
#endif
int cm_phase2_selftest(void)
{
    int failed = 0;
    cm_elf_image_t *im = NULL;
    cm_id_t pid = 0;
    cm_thread_t *competitor = NULL;
    kinfo("--- selftest: phase2 ELF loader + exec spawn ---");
    CHECK(cm_vfs_mkdir("/bin") == CM_OK || cm_vfs_stat("/bin", &(cm_stat_t){0}) == CM_OK);
    CHECK(put_elf("/bin/hello.elf", cm_test_elf, sizeof(cm_test_elf)) == CM_OK);
    kinfo("elf fixture: tests/hello_elf.S -> build/hello.elf -> COWFS /bin/hello.elf (%llu bytes)",
          (unsigned long)sizeof(cm_test_elf));
    failed += malformed_tests();
    CHECK(cm_elf_load("/bin/hello.elf", &im) == CM_OK);
    CHECK(im->entry == CM_ELF_USER_MIN && im->npages == 6);
    CHECK((cm_space_lookup(im->root, im->entry) & 7) == 5); /* RX: supervisor writes denied */
    CHECK((cm_space_lookup(im->root, TEST_CPL_VA) & 7) == 7);
    CHECK(!cm_space_lookup(im->root, 0x402000));
    for (unsigned i = 0; i < 16; i++) CHECK(((uint8_t *)cm_elf_pointer(im, TEST_CPL_VA))[i] == 0);
    CHECK(cm_elf_user_range(im, 0x401000, 15, 4));
    CHECK(!cm_elf_user_range(im, UINT64_MAX - 7, 16, 4));
    CHECK(!cm_elf_user_range(im, 0xffffffff80000000ull, 1, 4));
    CHECK(!cm_elf_user_range(im, 0x401fff, 2, 4));
#if CM_KPTI
    CHECK(!cm_space_lookup(im->shadow, (uint64_t)(uintptr_t)cm_phase2_selftest));
#endif
    cm_elf_destroy(im); im = NULL;
    extern void cm_commit_stats(uint64_t *, uint64_t *);
    uint64_t before, limit;
    cm_commit_stats(&before, &limit);
    uint64_t free_before = cm_mm_free_pages();
    uint64_t reserve = limit - before;
    cm_error_t charge = cm_commit_charge(reserve);
    CHECK(charge == CM_OK);
    cm_error_t denied = cm_elf_load("/bin/hello.elf", &im);
    cm_commit_uncharge(reserve);
    CHECK(denied == CM_ERR_OVERCOMMIT && !im && free_before == cm_mm_free_pages());
    kinfo("elf rollback ok: rejected image and commit-denied leave no image allocations");
    cm_spawn_attr_t attr = { .path = "/bin/hello.elf", .qos = CM_QOS_USER_INTERACTIVE };
    CHECK(cm_posix_spawn(&attr, &pid) == CM_OK && pid);
    cm_id_t another = 0;
    CHECK(cm_posix_spawn(&attr, &another) == CM_ERR_BUSY && !another);
    cm_proc_t *p = cm_proc_lookup(pid);
    CHECK(p && p->elf && !p->exited);
    CHECK(cm_proc_task_cpu_allowed(p->task, 0) && !cm_proc_task_cpu_allowed(p->task, 1));
    CHECK(cm_proc_reap_elf(pid) == CM_ERR_BUSY);
#ifndef CAPMIX_HOSTED
    cm_task_t *competition = cm_task_create("elf-preempt-competitor", CM_QOS_USER_INTERACTIVE);
    CHECK(competition);
    competitor_runs = 0;
    competitor = cm_thread_create(competition, elf_competitor, p, CM_QOS_USER_INTERACTIVE);
    CHECK(competitor);
#endif
    for (unsigned step = 0; step < 512 &&
         (!p->exited || (p->main && p->main->id) || (competitor && competitor->id)); step++)
        (void)cm_sched_run_until_idle(1);
    CHECK(p->exited);
#ifndef CAPMIX_HOSTED
    CHECK(p->elf_started && p->exit_code == 0);
    CHECK(*(uint8_t *)cm_elf_pointer(p->elf, TEST_CPL_VA) == 3);
    p->elf_cpl = 3;
    CHECK(*(uint64_t *)cm_elf_pointer(p->elf, TEST_CPL_VA + 16) & 0x200);
    CHECK(p->elf_preempts > 0 && p->elf_run_ticks > 1 && competitor_runs > 0);
    CHECK(!competitor->id);
    kinfo("elf user preempt verified: CPL=3 IF=1 preempts=%llu run_ticks=%llu competitor_runs=%u registers/stack ok exit=0",
          (unsigned long)p->elf_preempts, (unsigned long)p->elf_run_ticks, competitor_runs);
    CHECK(p->elf_write_calls == 1 && p->elf_written_bytes == 15);
#if CM_KPTI
    CHECK(p->elf_timer_entries > 0);
#endif
    kinfo("elf exec ok: hello from elf, CPL=3, BSS/invalid-buffer/fork checks, exit 0");
#else
    CHECK(!p->elf_started && p->exit_code == CM_ERR_NOTSUP);
    kinfo("elf hosted: loader/BSS/permissions/lifetime/CPU affinity verified; ring3 preempt execution skipped");
#endif
    CHECK(cm_proc_reap_elf(pid) == CM_OK);
    uint64_t after;
    cm_commit_stats(&after, &limit);
    CHECK(before == after && free_before == cm_mm_free_pages());
    kinfo("elf lifecycle ok: image pages/page tables/commit reclaimed");
cleanup:
    if (competitor && competitor->id) {
        competitor->state = CM_TH_DEAD;
        cm_thread_reap(competitor);
    }
    cm_elf_destroy(im);
    if (pid) {
        cm_proc_t *unfinished = cm_proc_lookup(pid);
        if (unfinished && unfinished->main && unfinished->main->id) {
            unfinished->exited = 1; unfinished->exit_code = CM_ERR_AGAIN;
            unfinished->main->state = CM_TH_DEAD;
            cm_thread_reap(unfinished->main);
        }
    }
    if (pid) (void)cm_proc_reap_elf(pid);
    kinfo("phase2 selftest: %s (%d failures)", failed ? "FAIL" : "PASS", failed);
    return failed;
}
