#ifndef QEMU_CPUS_H
#define QEMU_CPUS_H

#include "qemu/thread.h"

/* register accel-specific operations */
void cpus_register_accel(const AccelOpsClass *i);

/* return registers ops */
const AccelOpsClass *cpus_get_accel(void);

/* interface available for cpus accelerator threads */

/* For temporary buffers for forming a name */
#define VCPU_THREAD_NAME_SIZE 16

void cpus_kick_thread(CPUState *cpu);
bool cpu_work_list_empty(CPUState *cpu);
bool cpu_thread_is_idle(CPUState *cpu);
bool all_cpu_threads_idle(void);
bool cpu_can_run(CPUState *cpu);
void qemu_process_cpu_events_common(CPUState *cpu);
void cpu_thread_signal_created(CPUState *cpu);
void cpu_thread_signal_destroyed(CPUState *cpu);
void cpu_handle_guest_debug(CPUState *cpu);

/*
 * Debug probe (XEMU_TBRATE=1): the vCPU thread's waits, for the big lock
 * held by another thread and halted until an interrupt (lock retake
 * included). Read and reset by the report in accel/tcg/cpu-exec.c. One
 * vCPU, so plain counters.
 */
typedef struct XemuVcpuWaits {
    int64_t bql_ns;
    uint64_t bql_waits;
    int64_t halt_ns;
    uint32_t irqs[256];     /* hardware interrupts taken, by vector */
    int64_t gpu_ns;         /* waiting for a lock the GPU thread holds */
    uint64_t gpu_waits;
} XemuVcpuWaits;
extern XemuVcpuWaits xemu_vcpu_waits;
extern int xemu_tbrate_state;   /* -1 until the environment is read */
bool xemu_tbrate_init(void);
/* The GPU thread's own account, added atomically, read by the report. */
extern int64_t xemu_tbrate_flipsvc_ns, xemu_tbrate_fence_ns;
void xemu_tbrate_lock_slow(QemuMutex *m);

static inline bool xemu_tbrate_enabled(void)
{
    return likely(xemu_tbrate_state >= 0) ? xemu_tbrate_state
                                          : xemu_tbrate_init();
}

/* A GPU lock taken on the guest's behalf; with the probe on, a wait for a
 * lock the GPU thread holds is timed. */
static inline void xemu_tbrate_lock(QemuMutex *m)
{
    if (likely(!xemu_tbrate_enabled())) {
        qemu_mutex_lock(m);
    } else {
        xemu_tbrate_lock_slow(m);
    }
}

/* end interface for cpus accelerator threads */

bool qemu_in_vcpu_thread(void);
void qemu_init_cpu_loop(void);
void resume_all_vcpus(void);
void pause_all_vcpus(void);
void cpu_stop_current(void);

/* Unblock cpu */
void qemu_cpu_kick_self(void);

bool cpus_are_resettable(void);

void cpu_synchronize_all_states(void);
void cpu_synchronize_all_post_reset(void);
void cpu_synchronize_all_post_init(void);
void cpu_synchronize_all_pre_loadvm(void);

#endif
