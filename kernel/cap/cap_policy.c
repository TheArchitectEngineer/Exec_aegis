/* cap_policy.c — file-based capability policy engine.
 *
 * Loads policy files from /etc/aegis/caps.d/ at boot via VFS.
 * Each file is named after a binary basename (e.g. "httpd") and contains
 * lines of the form:
 *     service NET_SOCKET
 *     admin AUTH SETUID
 *
 * "service" caps are granted unconditionally at exec.
 * "admin" caps are granted only if proc->authenticated == 1.
 *
 * cap_policy_lookup(exe_path) extracts the basename from the path
 * and returns the matching entry, or NULL.
 */
#include "vfs.h"
#include "cap.h"
#include "printk.h"
#include <stdint.h>
#include <stddef.h>

/* Static policy table */
static cap_policy_entry_t s_entries[CAP_POLICY_MAX_ENTRIES];
static uint32_t s_entry_count;

/* Map a capability name string to its CAP_KIND_* value.
 * Returns 0 (CAP_KIND_NULL) if not recognized. */
static uint32_t
cap_name_to_kind(const char *name)
{
    /* Compare with known cap kind names.
     * Using manual comparison to avoid libc dependency. */
    struct { const char *str; uint32_t kind; } map[] = {
        { "VFS_OPEN",       CAP_KIND_VFS_OPEN },
        { "VFS_WRITE",      CAP_KIND_VFS_WRITE },
        { "VFS_READ",       CAP_KIND_VFS_READ },
        { "AUTH",           CAP_KIND_AUTH },
        { "CAP_GRANT",      CAP_KIND_CAP_GRANT },
        { "SETUID",         CAP_KIND_SETUID },
        { "NET_SOCKET",     CAP_KIND_NET_SOCKET },
        { "NET_ADMIN",      CAP_KIND_NET_ADMIN },
        { "THREAD_CREATE",  CAP_KIND_THREAD_CREATE },
        { "PROC_READ",      CAP_KIND_PROC_READ },
        { "DISK_ADMIN",     CAP_KIND_DISK_ADMIN },
        { "FB",             CAP_KIND_FB },
        { "CAP_DELEGATE",   CAP_KIND_CAP_DELEGATE },
        { "CAP_QUERY",      CAP_KIND_CAP_QUERY },
        { "IPC",            CAP_KIND_IPC },
        { "POWER",          CAP_KIND_POWER },
    };
    uint32_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const char *a = map[i].str;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return map[i].kind;
    }
    return CAP_KIND_NULL;
}

/* String comparison without libc */
static int
streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* Parse one policy file's contents into an entry.
 * Format: one line per cap, each line is "service CAP_NAME" or "admin CAP_NAME".
 * Multiple cap names per line separated by spaces.
 * Returns 0 on success, -1 on error. */
static int
parse_policy(const char *data, uint64_t len, cap_policy_entry_t *entry)
{
    uint64_t pos = 0;
    entry->count = 0;

    while (pos < len && entry->count < CAP_POLICY_MAX_CAPS) {
        /* Skip whitespace and newlines */
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t' ||
                             data[pos] == '\n' || data[pos] == '\r'))
            pos++;
        if (pos >= len) break;

        /* Skip comment lines */
        if (data[pos] == '#') {
            while (pos < len && data[pos] != '\n') pos++;
            continue;
        }

        /* Read tier word */
        char tier_buf[16];
        uint32_t ti = 0;
        while (pos < len && data[pos] != ' ' && data[pos] != '\t' &&
               data[pos] != '\n' && data[pos] != '\r' && ti < sizeof(tier_buf) - 1) {
            tier_buf[ti++] = data[pos++];
        }
        tier_buf[ti] = '\0';

        uint32_t tier;
        if (streq(tier_buf, "service"))
            tier = CAP_TIER_SERVICE;
        else if (streq(tier_buf, "admin"))
            tier = CAP_TIER_ADMIN;
        else
            continue;  /* unknown tier — skip line */

        /* Read cap names on the rest of this line */
        while (pos < len && data[pos] != '\n' && data[pos] != '\r') {
            /* Skip whitespace */
            while (pos < len && (data[pos] == ' ' || data[pos] == '\t'))
                pos++;
            if (pos >= len || data[pos] == '\n' || data[pos] == '\r')
                break;

            /* Read cap name */
            char cap_buf[32];
            uint32_t ci = 0;
            while (pos < len && data[pos] != ' ' && data[pos] != '\t' &&
                   data[pos] != '\n' && data[pos] != '\r' && ci < sizeof(cap_buf) - 1) {
                cap_buf[ci++] = data[pos++];
            }
            cap_buf[ci] = '\0';

            uint32_t kind = cap_name_to_kind(cap_buf);
            if (kind == CAP_KIND_NULL) {
                printk("[CAP_POLICY] WARN: unknown cap '%s'\n", cap_buf);
                continue;
            }

            if (entry->count < CAP_POLICY_MAX_CAPS) {
                entry->caps[entry->count].kind   = kind;
                entry->caps[entry->count].rights = CAP_RIGHTS_READ | CAP_RIGHTS_WRITE | CAP_RIGHTS_EXEC;
                entry->caps[entry->count].tier   = tier;
                entry->count++;
            }
        }
    }

    return 0;
}

void
cap_policy_load(void)
{
    s_entry_count = 0;

    /* Read the directory listing from /etc/aegis/caps.d/ */
    vfs_file_t dir;
    int r = vfs_open("/etc/aegis/caps.d", 0, &dir);
    if (r != 0) {
        printk("[CAP_POLICY] OK: no /etc/aegis/caps.d — 0 policies loaded\n");
        return;
    }

    /* Iterate directory entries */
    char name[256];
    uint8_t dtype;
    uint64_t idx = 0;

    while (dir.ops->readdir &&
           dir.ops->readdir(dir.priv, idx, name, &dtype) == 0) {
        idx++;
        if (dtype != 8)  /* DT_REG = 8, skip non-regular files */
            continue;
        if (s_entry_count >= CAP_POLICY_MAX_ENTRIES)
            break;

        /* Build full path */
        char path[256];
        uint32_t pi = 0;
        const char *prefix = "/etc/aegis/caps.d/";
        while (*prefix && pi < sizeof(path) - 1)
            path[pi++] = *prefix++;
        const char *np = name;
        while (*np && pi < sizeof(path) - 1)
            path[pi++] = *np++;
        path[pi] = '\0';

        /* Open and read the policy file */
        vfs_file_t f;
        if (vfs_open(path, 0, &f) != 0)
            continue;

        /* Read file contents into a stack buffer (policy files are small) */
        char buf[512];
        uint64_t fsize = f.size;
        if (fsize > sizeof(buf) - 1)
            fsize = sizeof(buf) - 1;
        if (f.ops->read) {
            int bytes = f.ops->read(f.priv, buf, 0, fsize);
            if (bytes > 0) {
                buf[bytes] = '\0';
                cap_policy_entry_t *entry = &s_entries[s_entry_count];
                /* Copy basename as the entry name */
                uint32_t ni = 0;
                const char *src = name;
                while (*src && ni < sizeof(entry->name) - 1)
                    entry->name[ni++] = *src++;
                entry->name[ni] = '\0';

                if (parse_policy(buf, (uint64_t)bytes, entry) == 0 &&
                    entry->count > 0) {
                    s_entry_count++;
                }
            }
        }
        if (f.ops->close)
            f.ops->close(f.priv);
    }
    if (dir.ops->close)
        dir.ops->close(dir.priv);

    printk("[CAP_POLICY] OK: %u policies loaded\n", s_entry_count);
}

const cap_policy_entry_t *
cap_policy_lookup(const char *exe_path)
{
    if (!exe_path || !exe_path[0])
        return (const cap_policy_entry_t *)0;

    /* Extract basename: everything after the last '/' */
    const char *basename = exe_path;
    const char *p = exe_path;
    while (*p) {
        if (*p == '/')
            basename = p + 1;
        p++;
    }

    if (!basename[0])
        return (const cap_policy_entry_t *)0;

    /* Search the policy table */
    uint32_t i;
    for (i = 0; i < s_entry_count; i++) {
        if (streq(s_entries[i].name, basename))
            return &s_entries[i];
    }
    return (const cap_policy_entry_t *)0;
}
