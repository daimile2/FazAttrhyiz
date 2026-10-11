#ifndef CAPMIX_SECURITY_H
#define CAPMIX_SECURITY_H
#include "capmix/sha256.h"

#include "types.h"
#include "cap.h"

/* Layer 6: capability deny-by-default + POSIX DAC/ACL compat
 * + Seatbelt per-operation profiles + SIP-style system integrity
 * + single-operation authorization (not UAC whole-process tokens).
 */

#define CM_OP_FILE_READ     (1u << 0)
#define CM_OP_FILE_WRITE    (1u << 1)
#define CM_OP_FILE_EXEC     (1u << 2)
#define CM_OP_NET_BIND      (1u << 3)
#define CM_OP_NET_CONNECT   (1u << 4)
#define CM_OP_SYSCTL        (1u << 5)
#define CM_OP_TASK_FOR_PID  (1u << 6)
#define CM_OP_KEXT_LOAD     (1u << 7)
#define CM_OP_TRACE         (1u << 8)
#define CM_OP_MOUNT         (1u << 9)

#define CM_SIP_KERN_TEXT    (1u << 0)
#define CM_SIP_SSV          (1u << 1)
#define CM_SIP_UNTRUSTED_KEXT (1u << 2)
#define CM_SIP_NVRAM        (1u << 3)
#define CM_SIP_TASK_FOR_PID (1u << 4)
#define CM_SIP_DEFAULT      (CM_SIP_KERN_TEXT | CM_SIP_SSV | CM_SIP_UNTRUSTED_KEXT | CM_SIP_NVRAM)

#define CM_SIGN_NONE        0
#define CM_SIGN_SIGNED      (1u << 0)
#define CM_SIGN_NOTARIZED   (1u << 1)
#define CM_SIGN_MODULE      (1u << 2)

#define CM_KASLR_SLIDE      0x0000000210000000ull
#define CM_KTEXT_BASE       0xffffffff80000000ull

typedef struct {
    uint32_t allow_mask;     /* per-operation, not per-process */
    uint32_t deny_mask;
    const char *label;
} cm_seatbelt_t;

typedef struct {
    uint32_t uid, gid, mode;
    uint32_t acl_allow;
    uint32_t acl_deny;
} cm_dac_t;

typedef struct {
    /* Expected digest must come from trusted policy, not the untrusted image.
     * Caller keeps both buffers immutable throughout verification. */
    const void *content;
    size_t content_size;
    const uint8_t *expected_digest;
    size_t digest_size;
    uint32_t flags;
    const char *id;
} cm_sign_t;

typedef int (*cm_lsm_hook_t)(uint32_t op, cm_id_t subject, uint64_t obj);

cm_error_t cm_security_init(void);

/* Capability check: missing bit = deny. */
cm_error_t cm_cap_check(cm_task_t *task, int hid, cm_rights_t need);

/* POSIX overlay — never replaces caps. */
cm_error_t cm_dac_check(const cm_dac_t *dac, uint32_t uid, uint32_t need_op);

cm_error_t cm_seatbelt_apply(cm_id_t pid, const cm_seatbelt_t *p);
cm_error_t cm_seatbelt_check(cm_id_t pid, uint32_t op);

uint32_t   cm_sip_mask(void);
bool       cm_sip_protects(cm_vaddr_t addr);
cm_error_t cm_sip_write_check(cm_vaddr_t addr); /* even root */
int        cm_sip_allows_kext(const cm_sign_t *s);

/* One-shot authorization. Token dies after the matching op. */
int        cm_authz_grant(cm_id_t pid, uint32_t op);
cm_error_t cm_authz_consume(cm_id_t pid, uint32_t op);

cm_error_t cm_sign_verify(const cm_sign_t *s, int want_module);
void       cm_lsm_register(cm_lsm_hook_t h);
cm_error_t cm_lsm_check(uint32_t op, cm_id_t subject, uint64_t obj);

uint64_t   cm_kaslr_slide(void);
int        cm_mitigations(void); /* Actual CR4 bits: 0x02 SMEP, 0x04 SMAP; no KASLR claim. */

/* Combined gate used by syscall-shaped helpers. */
cm_error_t cm_sec_gate(cm_task_t *task, int hid, cm_rights_t cap_need,
                       cm_id_t pid, uint32_t op);

#endif
