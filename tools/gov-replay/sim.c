/* Replay harness: the governor's real source, a model of the M3s and the A55s, a virtual clock. */
#include <setjmp.h>
#include <stdlib.h>
#include <math.h>
#include "simstub.h"
static uint64_t vnow = 1000000;            /* the clock: 24 MHz ticks as on the phone (1 tick = 41.67 ns) */
#define TICKS_MS 24000ull
uint64_t mach_absolute_time(void) { return vnow; }
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *a) { *a = ns * 24 / 1000; }
void absolutetime_to_nanoseconds(uint64_t a, uint64_t *ns) { *ns = a * 1000 / 24; }
/* ---- model state */
static double rate_khz[2];                 /* the clusters' rates now, as asked */
static uint32_t rec_mask=~0u; static int m3s_on(void){ return __builtin_popcount(rec_mask>>4&0xf); }
static double req_count;
static double core_cycles[8];              /* cycles still to do per core */
static uint64_t idle_ticks[8];
#define LOG_LEVELS 32
/* ---- the workload, from the log's seconds */
typedef struct { int t; int ba, la, bf, lf, fr, touch; } sec_t;
static sec_t *secs; static int nsec;
/* per-frame stats */
static double frame_left = 0; static uint64_t frame_began = 0; static int frame_core = 4;
static uint64_t fr_total, fr_missed, fr_sum_us, fr_touch_total, fr_touch_missed, fr_touch_sum_us; static uint64_t fr_lat[2000000]; static uint64_t fr_n;
static uint64_t touchfr_lat[2000000]; static uint64_t touchfr_n; static uint64_t first_lat[200000]; static uint64_t first_n,first_late;
static double energy_big, energy_lit, time_big[4]; static double busy_cyc[2];
/* the APM's table as logged: mV at each rate. Under 1586 MHz (M3s) the log's line is cut: extrapolated at the slope above, not measured */
static double big_mv(double mhz){ if(mhz>=1794) return 818.75; if(mhz>=1690) return 787.5; if(mhz>=1586) return 762.5; double v=762.5-(1586-mhz)*0.27; return v<600?600:v; }
static double lit_mv(double mhz){ const double t[][2]={{1794,1006.25},{1690,962.5},{1456,881.25},{1248,812.5},{1053,762.5},{949,731.25},{832,700},{715,668.75},{598,637.5},{455,593.75}}; for(int i=0;i<10;i++) if(mhz>=t[i][0]) return t[i][1]; return 581.25; }
static void cluster_of(int core, int *cl){ *cl = core>=4; }
/* advance virtual time by dt ticks; rates fixed meanwhile */
static void run_for(uint64_t dt){
  double sec=dt/24e6;
  for(int cl=0;cl<2;cl++){ double mhz=rate_khz[cl]/1000; double mv=cl?big_mv(mhz):lit_mv(mhz);
    int busy=0; for(int c=cl*4;c<cl*4+4;c++){ double can=rate_khz[cl]*1000*sec; double d=core_cycles[c]<can?core_cycles[c]:can; core_cycles[c]-=d; if(can>0){ double fr=d/can; idle_ticks[c]+=(uint64_t)((1-fr)*dt); double e=(mv*mv/1e6)*(d/1e9)*(cl?1.0:0.25); if(cl) energy_big+=e; else energy_lit+=e; if(fr>0) busy=1; } }
    (void)busy; }
  vnow+=dt;
}
/* event list: frames and touches, in time order, built from the seconds */
typedef struct { uint64_t at; int kind; double cycles; int core; int touched; } ev_t; /* kind 0 frame 1 finger event 2 finger down 3 finger up 4 work */
static ev_t *evs; static int nev, ev_next;
static int cmpev(const void*a,const void*b){ uint64_t x=((ev_t*)a)->at,y=((ev_t*)b)->at; return x<y?-1:x>y; }
static jmp_buf done; static uint64_t end_at;
extern void hakios_exynos_cpufreq_touch(void); extern void hakios_exynos_cpufreq_frame(void); extern void hakios_exynos_cpufreq_fingers(uint32_t);
/* frame jobs: a queue per frame on the render core, measured to their finish */
typedef struct { uint64_t began; double cycles; int touched; } fj_t;
static fj_t fq[64]; static int fq_h, fq_t; static double fq_left;
static void frame_step(uint64_t dt){ /* the render core: frames in order, at the M3s' rate */
  double can=rate_khz[1]*1000*dt/24e6;
  while(can>0 && fq_h!=fq_t){ double d=fq_left<can?fq_left:can; fq_left-=d; can-=d;
    if(fq_left<=1e-3){ uint64_t fin=vnow+dt-(uint64_t)(can/(rate_khz[1]*1000)*24e6); fj_t*f=&fq[fq_h]; double us=(fin-f->began)/24.0; fr_total++; fr_sum_us+=(uint64_t)us; if(fr_n<2000000) fr_lat[fr_n++]=(uint64_t)us; if(us>16667) fr_missed++; if(f->touched==2){ if(first_n<200000) first_lat[first_n++]=(uint64_t)us; if(us>16667) first_late++; } if(f->touched){ fr_touch_total++; fr_touch_sum_us+=(uint64_t)us; if(us>16667) fr_touch_missed++; if(touchfr_n<2000000) touchfr_lat[touchfr_n++]=(uint64_t)us; } fq_h=(fq_h+1)%64; if(fq_h!=fq_t) fq_left=fq[fq_h].cycles; } }
}
static void watch(void);
static void advance_to(uint64_t t){
  while(vnow<t){ uint64_t dt=t-vnow; if(dt>2400) dt=2400; /* 100 us steps */
    watch();
    double before=core_cycles[4]; (void)before; frame_step(dt); run_for(dt);
    double mhz=rate_khz[1]/1000; int b=mhz>=1794?3:mhz>=1469?2:mhz>=1261?1:0; time_big[b]+=dt/24e6;
    if(vnow>=end_at) longjmp(done,1); }
}
static void fire(ev_t*e){
  switch(e->kind){
   case 0: { fq[fq_t]=(fj_t){vnow,e->cycles,e->touched}; if(fq_h==fq_t) fq_left=e->cycles; fq_t=(fq_t+1)%64; core_cycles[4]+=e->cycles; hakios_exynos_cpufreq_frame(); break; }
   case 1: hakios_exynos_cpufreq_touch(); break;
   case 2: hakios_exynos_cpufreq_touch(); hakios_exynos_cpufreq_fingers(1); break;
   case 3: hakios_exynos_cpufreq_touch(); hakios_exynos_cpufreq_fingers(0); break;
   case 4: core_cycles[e->core]+=e->cycles; break;
  }
}
/* ---- what the governor calls */
static int woken;
wait_result_t assert_wait_deadline(event_t e, wait_interrupt_t i, uint64_t dl); static uint64_t wait_deadline;
wait_result_t assert_wait_deadline(event_t e, wait_interrupt_t i, uint64_t dl){ wait_deadline=dl; woken=0; return 0; }
kern_return_t thread_wakeup_prim(event_t e, boolean_t one, wait_result_t r){ woken=1; return 0; }
int thread_block(void *cont){ if(!wait_deadline) return 0;
  uint64_t dl=wait_deadline; wait_deadline=0;
  for(;;){ if(woken) return 0; uint64_t next=ev_next<nev?evs[ev_next].at:~0ull; if(next>=dl){ advance_to(dl); return 0; } advance_to(next); fire(&evs[ev_next++]); }
}
void *thread_bind(void *p){ return 0; } void *cpu_to_processor(int c){ return 0; }
bool ml_cpu_idle_time(int c, uint64_t *i){ *i=idle_ticks[c]+1; return true; }
static ml_topology_info_t topo={8,{{0,0},{1,1},{2,2},{3,3},{0x100,4},{0x101,5},{0x102,6},{0x103,7}}};
const ml_topology_info_t *ml_get_topology_info(void){ return &topo; }
boolean_t ml_set_interrupts_enabled(boolean_t e){ return e; }
static int sim_policy=1;
int PE_parse_boot_argn(const char *n, void *v, int s){ if(!strcmp(n,"cpufreq_gov")){*(uint32_t*)v=1;return 1;} if(!strcmp(n,"gov_policy")){*(uint32_t*)v=sim_policy;return 1;} extern int sim_arg(const char*,uint32_t*); return sim_arg(n,v); }
static void limits(void);
int hakios_acpm_request(uint32_t id, uint32_t c[4]){ req_count++; rate_khz[c[0]==3]=c[1]; limits(); return 0; }
int hakios_acpm_dvfs_get(uint32_t i, uint32_t *k){ *k=0; return 0; }
uint32_t hakios_acpm_dvfs_table(uint32_t i, uint32_t *k, uint32_t *u, uint32_t most, int (*say)(const char *, ...)){ return 0; }
bool hakios_acpm_init(int (*say)(const char *, ...)){ return true; }
static struct { unsigned sat,satl,win,rev,revl,raise,top,missed,acpm; double raise_sum,raise_max,top_sum,top_max,acpm_sum; } acc;
void loader_log_line(const char *l){ const char*p=strstr(l,"| sat big "); if(!p) return; unsigned a,b,c,d,e,rn,tn,tm,an; unsigned long long ra,rm,ta,tx,aa,ax;
  if(sscanf(p,"| sat big %u lit %u of %u | rev big %u lit %u",&a,&b,&c,&d,&e)==5){acc.sat+=a;acc.satl+=b;acc.win+=c;acc.rev+=d;acc.revl+=e;}
  p=strstr(l,"| raise "); if(p&&sscanf(p,"| raise %u avg %llu us max %llu | top %u avg %llu ms max %llu, %u missed | acpm %u avg %llu us max %llu",&rn,&ra,&rm,&tn,&ta,&tx,&tm,&an,&aa,&ax)==10){ acc.raise+=rn;acc.raise_sum+=rn*(double)ra; if(rm>acc.raise_max)acc.raise_max=rm; acc.top+=tn;acc.top_sum+=tn*(double)ta; if(tx>acc.top_max)acc.top_max=tx; acc.missed+=tm; acc.acpm+=an; acc.acpm_sum+=an*(double)aa; } }
int kernel_sysctlbyname(const char *n, void *o, size_t *os, void *np, size_t nl){ return 1; }
/* The temperature: one RC per cluster, stepped once a second as the TMU reads it. The constants are chosen so 4 M3s at 1794 MHz with the log's
 * load peak near the log's 78 C, not measured: only the phone can say how fast 2314 MHz heats. */
static double heat[2]={45,40}, heat_w[2]; static uint64_t tmu_at=1000000, tmu_next; static int tmu_frozen, tmu_garbage; static double tmu_forced;
int32_t hakios_exynos_tmu_zone_mc(unsigned z){ if(tmu_garbage) return -273000; return (int32_t)(1000*(tmu_forced>0?tmu_forced:heat[z==1])); }
int32_t hakios_exynos_tmu_zone_now_mc(unsigned z){ return hakios_exynos_tmu_zone_mc(z); }
uint64_t hakios_exynos_tmu_age_ms(void){ return (vnow-tmu_at)/TICKS_MS; }
static void limits(void);
void sched_perfcontrol_update_recommended_cores(uint32_t m){ rec_mask=m; limits(); }
static int test_fail; static const char *test_name; static uint64_t test_at;
void thread_deallocate(thread_t t){}
int sysctl_io_number(struct sysctl_req *r, int a, int s, int *p, void *c){ return 0; }
kern_return_t kernel_thread_start(thread_continue_t f, void *a, thread_t *t){ return KERN_SUCCESS; } /* run by main below */
#define printf(...) 0
#define vsnprintf vsnprintf
#include "hakios.exynos_cpufreq.c"
#include "hakios.scion_gov.c"
#undef printf
/* whenever the rate or the cores move: never more M3s than stock allows at the rate */
static void limits(void){
  if((m3s_on()>2 && rate_khz[1]>1794000) || rate_khz[1]>2314000){ printf("FAIL: %d M3s at %.0f kHz, above stock's limit for that many\n",m3s_on(),rate_khz[1]); test_fail=1; longjmp(done,1); }
}
/* every 100 us: the TMU, the tests' conditions */
static void watch(void){
  if(vnow>=tmu_next){ double dt=1; tmu_next=vnow+24000000ull;
    for(int c=0;c<2;c++){ double w=(energy_big*(c==1)+energy_lit*(c==0)-heat_w[c]); heat_w[c]=energy_big*(c==1)+energy_lit*(c==0); heat[c]+=dt*(35+w*(c?6.0:12.0)-heat[c])/25; }
    if(!tmu_frozen) tmu_at=vnow; }
  if(test_name && vnow>=test_at){ const char*n=test_name; test_name=0;
    if(!strcmp(n,"stale")) tmu_frozen=1; else if(!strcmp(n,"garbage")) tmu_garbage=1; else if(!strcmp(n,"burst")) tmu_forced=80; else tmu_forced=97; test_at=vnow; }
  if(test_at && !test_name){ uint64_t by=test_at+(tmu_garbage?100:tmu_frozen?3100:tmu_forced==80?100:1100)*TICKS_MS;
    if(vnow>=by){ double most=tmu_forced==80?1794000:tmu_forced>0?650000:1261000;
      if(rate_khz[1]>most || m3s_on()!=4){ printf("FAIL: %.0f ms on: %.0f kHz, %d M3s (wanted at most %.0f, all 4)\n",(vnow-test_at)/24.0/1000,rate_khz[1],m3s_on(),most); test_fail=1; }
      else printf("PASS: %.0f kHz and all 4 M3s %.0f ms on\n",rate_khz[1],(vnow-test_at)/24.0/1000);
      longjmp(done,1); } }
}
/* ---- args */
struct {const char*n; uint32_t v;} sargs[32]; int nsargs;
int sim_arg(const char*n,uint32_t*v){ for(int i=0;i<nsargs;i++) if(!strcmp(n,sargs[i].n)){*v=sargs[i].v;return 1;} return 0; }
static void push(uint64_t at,int kind,double cyc,int core,int touched){ if(nev%100000==0) evs=realloc(evs,sizeof(ev_t)*(nev+100000)); evs[nev++]=(ev_t){at,kind,cyc,core,touched}; }
static double drand(void){ return rand()/(double)RAND_MAX; }
/* The log's seconds into events: the busiest core's cycles of that second (its avg load x its rate) split into its frames,
 * with the remainder as background bursts; finger events evenly through the second, a finger coming down at a touch's start. */
static void build(void){
  uint64_t t0=vnow; int was=0;
  for(int i=0;i<nsec;i++){ sec_t*s=&secs[i]; uint64_t at=t0+(uint64_t)i*24000000ull;
    double cyc=s->ba/100.0*s->bf*1e6; double lcyc=s->la/100.0*s->lf*1e6;
    int fr=s->fr; double framecyc=0;
    if(fr>0){ framecyc=cyc*0.9/fr; for(int f=0;f<fr;f++){ uint64_t a=at+(uint64_t)((f+drand()*0.2)*24000000.0/fr); double j=framecyc*(0.5+drand()); double cap=30e-3*1794e6; if(j>cap){ double r=j-cap; j=cap; for(int q=0;q<4;q++) push(a+(uint64_t)(q*6000000.0/fr),4,r/4,4,0);} push(a,0,j,4,s->touch>0); } }
    double rest= fr>0? cyc*0.1 : cyc; int bursts=25; for(int b=0;b<bursts;b++) if(rest>0) push(at+(uint64_t)((b+drand())*24000000.0/bursts),4,rest/bursts,fr>0?5:4,0);
    if(lcyc>0) for(int b=0;b<25;b++) push(at+(uint64_t)((b+drand())*24000000.0/25),4,lcyc/25,1,0);
    if(s->touch>0){ int nx=i+1<nsec?secs[i+1].touch:0; if(!was) push(at+(uint64_t)(drand()*12000000),2,0,0,0); for(int k=0;k<s->touch;k++) push(at+(uint64_t)((k+0.5)*24000000.0/s->touch),1,0,0,0); if(!nx) push(at+22000000ull,3,0,0,0); was=1; } else was=0;
  }
  qsort(evs,nev,sizeof(ev_t),cmpev);
  end_at=t0+(uint64_t)nsec*24000000ull;
}
static void scen(const char*name){ uint64_t t=vnow+24000000ull; uint64_t MS=24000; 
  for(int i=0;i<40;i++){ uint64_t gap=2000+(uint64_t)(drand()*1000); t+=gap*MS; /* idle before it: a little background at the floor */
    for(uint64_t b=0;b<gap;b+=40) push(t-gap*MS+b*MS+(uint64_t)(drand()*40*MS),4,0.08*40e-3*650e6,4,0);
    if(!strcmp(name,"scroll")){ push(t,2,0,0,0); for(int k=1;k<180;k++) push(t+(uint64_t)(k*8.33*MS),1,0,0,0); int fc=0; for(uint64_t a=8;a<1500;a+=17){ double j=(12e-3*1794e6)*(0.8+0.4*drand()); push(t+a*MS,0,j,4,fc<5?2:1); fc++; } push(t+1500*MS,3,0,0,0); for(uint64_t a=1500;a<2500;a+=17) push(t+a*MS,0,(4e-3*1261e6)*(0.8+0.4*drand()),4,1); t+=2500*MS; }
    else if(!strcmp(name,"tap")){ push(t,2,0,0,0); push(t+80*MS,3,0,0,0); int fc=0; for(uint64_t a=90;a<500;a+=17){ double j=(10e-3*1794e6)*(0.8+0.4*drand()); push(t+a*MS,0,j,4,fc<5?2:1); fc++; } t+=500*MS; }
    else if(!strcmp(name,"hold")){ push(t,2,0,0,0); int fc=0; for(uint64_t a=1000;a<1300;a+=17){ double j=(10e-3*1794e6)*(0.8+0.4*drand()); push(t+a*MS,0,j,4,fc<5?2:1); fc++; } push(t+1500*MS,3,0,0,0); t+=1500*MS; }
    else if(!strcmp(name,"idle")){ t+=500*MS; }
    else if(!strcmp(name,"two")){ /* a render thread and a helper, and nothing else: the shape parking is for */
      push(t,2,0,0,0); for(int k=1;k<360;k++) push(t+(uint64_t)(k*8.33*MS),1,0,0,0); int fc=0; for(uint64_t a=8;a<3000;a+=17){ double j=(15e-3*1794e6)*(0.8+0.4*drand()); push(t+a*MS,0,j,4,fc<5?2:1); push(t+a*MS,4,0.3*j,5,0); fc++; } push(t+3000*MS,3,0,0,0); t+=3000*MS; }
    else if(!strcmp(name,"load")){ /* the same for the tests: frames without end */
      push(t,2,0,0,0); for(uint64_t a=8;a<20000;a+=17){ double j=(15e-3*1794e6); push(t+a*MS,0,j,4,1); push(t+a*MS,4,0.3*j,5,0); } t+=20000*MS; break; }
  }
  qsort(evs,nev,sizeof(ev_t),cmpev); end_at=t+2000*MS; nsec=(end_at-vnow)/24000000; }
static int cmpu(const void*a,const void*b){ uint64_t x=*(uint64_t*)a,y=*(uint64_t*)b; return x<y?-1:x>y; }
static uint64_t pctl(uint64_t*a,uint64_t n,double p){ return n?a[(uint64_t)(n*p)<n?(uint64_t)(n*p):n-1]:0; }
int main(int ac,char**av){
  if(ac<3){ fprintf(stderr,"sim policy trace [arg=val...]\n"); return 2; }
  sim_policy=atoi(av[1]); FILE*f=fopen(av[2],"r"); if(!f && strncmp(av[2],"scen:",5) && strncmp(av[2],"test:",5)) return 2; secs=malloc(sizeof(sec_t)*200000);
  if(f) while(fscanf(f,"%d %d %d %d %d %d %d",&secs[nsec].t,&secs[nsec].ba,&secs[nsec].la,&secs[nsec].bf,&secs[nsec].lf,&secs[nsec].fr,&secs[nsec].touch)==7) nsec++;
  for(int i=3;i<ac;i++){ char*e=strchr(av[i],'='); *e=0; sargs[nsargs].n=av[i]; sargs[nsargs++].v=atoi(e+1); }
  if(!strncmp(av[2],"test:",5)){ test_name=av[2]+5; test_at=vnow+8000*24000ull; scen("load"); }
  else { srand(12345); if(f) build(); else scen(av[2]+5); }
  hakios_exynos_cpufreq_gov_start();
  gov_running=true;
  if(setjmp(done)==0) gov_thread(NULL,0);
  if(!strncmp(av[2],"test:",5)) return test_fail;
  if(test_fail) return 1;
  qsort(fr_lat,fr_n,8,cmpu); qsort(touchfr_lat,touchfr_n,8,cmpu);
  printf("policy %d: %d s; frames %llu (late >16.7ms: %.1f%%, p50 %.1f p90 %.1f p99 %.1f ms); frames during touch %llu (late %.1f%%, p50 %.1f p90 %.1f p99 %.1f ms)\n",sim_policy,nsec,fr_total,100.0*fr_missed/(fr_total?fr_total:1),pctl(fr_lat,fr_n,.5)/1e3,pctl(fr_lat,fr_n,.9)/1e3,pctl(fr_lat,fr_n,.99)/1e3,fr_touch_total,100.0*fr_touch_missed/(fr_touch_total?fr_touch_total:1),pctl(touchfr_lat,touchfr_n,.5)/1e3,pctl(touchfr_lat,touchfr_n,.9)/1e3,pctl(touchfr_lat,touchfr_n,.99)/1e3);
  double tt=time_big[0]+time_big[1]+time_big[2]+time_big[3];
  printf(" big time: <1261 %.1f%% <1469 %.1f%% <1794 %.1f%% top %.1f%%; ACPM requests %.0f (%.2f/s, of which changes %u/%u); energy proxy big %.1f lit %.1f (V^2 x Gcycles; lit x0.25)\n",100*time_big[0]/tt,100*time_big[1]/tt,100*time_big[2]/tt,100*time_big[3]/tt,req_count,req_count/nsec,clusters[1].changes,clusters[0].changes,energy_big,energy_lit);
  printf(" stat: sat big %u lit %u of %u windows; rev big %u lit %u; raise n %u avg %.0f us max %.0f; top n %u avg %.1f ms max %.0f, missed %u\n",acc.sat,acc.satl,acc.win,acc.rev,acc.revl,acc.raise,acc.raise?acc.raise_sum/acc.raise:0,acc.raise_max,acc.top,acc.top?acc.top_sum/acc.top:0,acc.top_max,acc.missed);
  if(first_n){ qsort(first_lat,first_n,8,cmpu); printf(" first 5 frames of a gesture: n %llu p50 %.1f p90 %.1f max %.1f ms, late %.1f%%\n",(unsigned long long)first_n,pctl(first_lat,first_n,.5)/1e3,pctl(first_lat,first_n,.9)/1e3,first_lat[first_n-1]/1e3,100.0*first_late/first_n); }
  return 0;
}
