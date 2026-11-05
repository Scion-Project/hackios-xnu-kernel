#pragma once
#define EINVAL 22
#define ENXIO 6
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define __unused __attribute__((unused))
#define __printflike(a,b) __attribute__((format(printf,a,b)))
#define MAX_CPUS 8
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_USEC 1000ull
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TRUE 1
#define FALSE 0
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define KERN_SUCCESS 0
#define HAKIOS_ACPM_CH_DVFS 0
typedef int boolean_t; typedef int wait_result_t; typedef int wait_interrupt_t; typedef void *event_t; typedef int kern_return_t; typedef void *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
typedef struct { unsigned phys_id; int cpu_id; } ml_cpu_t;
typedef struct { unsigned num_cpus; ml_cpu_t cpus[8]; } ml_topology_info_t;
const ml_topology_info_t *ml_get_topology_info(void);
uint64_t mach_absolute_time(void);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *a);
void absolutetime_to_nanoseconds(uint64_t a, uint64_t *ns);
boolean_t ml_set_interrupts_enabled(boolean_t e);
int PE_parse_boot_argn(const char *n, void *v, int s);
int hakios_acpm_request(uint32_t id, uint32_t cmd[4]);
int hakios_acpm_dvfs_get(uint32_t i, uint32_t *k);
uint32_t hakios_acpm_dvfs_table(uint32_t i, uint32_t *k, uint32_t *u, uint32_t most, int (*say)(const char *, ...));
bool hakios_acpm_init(int (*say)(const char *, ...));
kern_return_t kernel_thread_start(thread_continue_t f, void *a, thread_t *t);
void thread_deallocate(thread_t t);
struct sysctl_oid; struct sysctl_req { void *newptr; };
int sysctl_io_number(struct sysctl_req *r, int a, int s, int *p, void *c);
#define SYSCTL_DECL(x)
#define SYSCTL_PROC(...)
