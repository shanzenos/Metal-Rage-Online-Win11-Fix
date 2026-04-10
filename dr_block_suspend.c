/* dr_block_suspend.c — DynamoRIO client to block NtSuspendThread from y0da
 *
 * Simplified version: uses ONLY core DR API (no drmgr extension) to avoid
 * import resolution issues with TCC-compiled DLLs.
 */

#include "dr_api.h"

static int suspend_sysnum = -1;
static app_pc jeus_start = NULL;
static app_pc jeus_end = NULL;
static int blocked_count = 0;

static int
get_suspend_sysnum(void)
{
    byte *entry;
    module_data_t *data = dr_lookup_module_by_name("ntdll.dll");
    int num;
    if (data == NULL)
        return -1;
    entry = (byte *)dr_get_proc_address(data->handle, "NtSuspendThread");
    if (entry == NULL) {
        dr_free_module_data(data);
        return -1;
    }
    /* Decode the syscall number from the ntdll stub:
     * mov eax, <sysnum>  =>  B8 XX XX XX XX */
    if (entry[0] == 0xB8) {
        num = *(int *)(entry + 1);
    } else {
        /* Fallback to use drmgr decode if available, or just read the mov */
        num = -1;
    }
    dr_free_module_data(data);
    return num;
}

static void
find_jeus_section(void)
{
    module_data_t *mod = dr_lookup_module_by_name("MetalRage.exe");
    if (mod == NULL) {
        dr_fprintf(STDERR, "[block_suspend] MetalRage.exe not found!\n");
        return;
    }
    /* .Jeus is at RVA 0x51000, size ~0x1B000 */
    jeus_start = mod->start + 0x51000;
    jeus_end = mod->start + 0x6C000;
    dr_fprintf(STDERR, "[block_suspend] MetalRage base: " PFX "\n", mod->start);
    dr_fprintf(STDERR, "[block_suspend] .Jeus range: " PFX "-" PFX "\n",
               jeus_start, jeus_end);
    dr_free_module_data(mod);
}

static void
event_exit(void)
{
    dr_fprintf(STDERR, "[block_suspend] Exit. Blocked %d NtSuspendThread calls.\n",
               blocked_count);
}

static bool
event_filter_syscall(void *drcontext, int sysnum)
{
    return (sysnum == suspend_sysnum);
}

static bool
event_pre_syscall(void *drcontext, int sysnum)
{
    if (sysnum == suspend_sysnum) {
        dr_mcontext_t mc;
        void *retaddr = NULL;

        mc.size = sizeof(mc);
        mc.flags = DR_MC_CONTROL;
        dr_get_mcontext(drcontext, &mc);

        /* Check return address on stack */
        dr_safe_read((void *)mc.xsp, sizeof(retaddr), &retaddr, NULL);

        if ((retaddr >= (void *)jeus_start && retaddr < (void *)jeus_end) ||
            ((app_pc)mc.pc >= jeus_start && (app_pc)mc.pc < jeus_end)) {
            /* Block: y0da trying to suspend a thread */
            dr_syscall_set_result(drcontext, 0); /* STATUS_SUCCESS */
            blocked_count++;
            return false; /* skip syscall */
        }
    }
    return true;
}

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    dr_set_client_name("Metal Rage y0da Blocker", "");
    dr_enable_console_printing();

    suspend_sysnum = get_suspend_sysnum();
    dr_fprintf(STDERR, "[block_suspend] NtSuspendThread sysnum = %d (0x%x)\n",
               suspend_sysnum, suspend_sysnum);

    find_jeus_section();

    dr_register_filter_syscall_event(event_filter_syscall);
    dr_register_pre_syscall_event(event_pre_syscall);
    dr_register_exit_event(event_exit);

    dr_fprintf(STDERR, "[block_suspend] Ready. Blocking NtSuspendThread from .Jeus.\n");
}
