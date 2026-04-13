#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include "sched.h"
#include "trace.h"
#include "process.h"
#include "cpu.h"

#define END_STEP 29
#define MAX_SIM  500

struct workload_item_t {
    int pid, ppid;
    size_t ts, tf, idle;
    char *cmd;
    int prio;
};

enum epoch { before, in, after };
typedef enum epoch epoch;

void draw_hbar(char c, size_t width) {
    char bar[width+1]; memset(bar,c,width); bar[width]='\0'; printf("%s",bar);
}

void chronogram(workload_item *w, size_t nb, size_t steps) {
    printf("\t");
    for(size_t t=0;t<steps;t++) printf(t%5==0?"|":".");
    printf("\n");
    for(size_t i=0;i<nb;i++) {
        printf("%s\t",w[i].cmd);
        draw_hbar(' ',w[i].ts);
        draw_hbar('X',w[i].tf-w[i].ts);
        printf("\t\t\t (tf=%zu,idle=%zu)\n",w[i].tf,w[i].idle);
    }
}

size_t count_lines_in_file(FILE *file) {
    int lines=0; char ch;
    while((ch=fgetc(file))!=EOF) if(ch=='\n') lines++;
    if(ch!='\n'&&lines!=0) lines++;
    rewind(file); return lines;
}

size_t read_data(size_t workload_size, FILE *file) {
    size_t count=0; char line[256], cmd[50];
    while(fgets(line,sizeof(line),file)&&count<workload_size) {
        workload_item item;
        if(sscanf(line,"%d %d %zu %zu %zu %s %d",
            &item.pid,&item.ppid,&item.ts,&item.tf,&item.idle,cmd,&item.prio)==7) {
            item.cmd=strdup(cmd); workload[count++]=item;
        } else { fprintf(stderr,"Error parsing line: %s\n",line); return 0; }
    }
    return count;
}

/* ================================================================
 * Run queue
 * ================================================================ */
static size_t runq[256]; static size_t rq_sz=0; static int cpu_load=0;

/* Evict lowest-prio runner; tie: higher pid first */
static int min_runner(void) {
    if(!rq_sz) return -1;
    int m=0;
    for(size_t i=1;i<rq_sz;i++) {
        int pi=workload[runq[i]].prio, pm=workload[runq[m]].prio;
        if(pi<pm) m=i;
        else if(pi==pm && runq[i]>runq[m]) m=i;
    }
    return m;
}
static void rem_runner(int idx) {
    cpu_load -= workload[runq[idx]].prio;
    runq[idx] = runq[--rq_sz];
}
static void add_runner(size_t pid) {
    runq[rq_sz++]=pid; cpu_load+=workload[pid].prio;
}

/* ================================================================
 * Pending queue — insertion-ordered set (no duplicates)
 * ================================================================ */
static size_t pq[512]; static size_t pq_sz=0;

static bool pq_contains(size_t pid) {
    for(size_t i=0;i<pq_sz;i++) if(pq[i]==pid) return true;
    return false;
}
static void pq_add(size_t pid) {
    if(!pq_contains(pid)) pq[pq_sz++]=pid;
}
static void pq_remove_at(size_t idx) { pq[idx]=pq[--pq_sz]; }

/* ================================================================
 * Stable insertion sort by prio DESC.
 * Equal-priority elements preserve their original relative order.
 * ================================================================ */
static void stable_sort_prio_desc(size_t *arr, size_t n) {
    for(size_t i=1;i<n;i++) {
        size_t key=arr[i]; int j=(int)i-1;
        while(j>=0 && workload[arr[j]].prio < workload[key].prio) {
            arr[j+1]=arr[j]; j--;
        }
        arr[j+1]=key;
    }
}

/* ================================================================
 * Try to schedule one process.
 * Evicted go to pq (appended at end).
 * ================================================================ */
static bool try_schedule(size_t pid) {
    int p=workload[pid].prio;
    printf("> schedule pid=%zu prio=%d ('%s') ",pid,p,workload[pid].cmd);
    while(cpu_load+p > CPU_CAPABILITY) {
        int m=min_runner();
        if(m<0 || workload[runq[m]].prio >= p) {
            if(m>=0)
                printf("...can't fit. Pick process to put asleep: None, as min prio: pid=%zu prio=%d ('%s') has greater or equal priority\n",
                    runq[m],workload[runq[m]].prio,workload[runq[m]].cmd);
            printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
            return false;
        }
        size_t ep=runq[m];
        printf("...can't fit. Pick process to put asleep: pid=%zu prio=%d ('%s')\n",
            ep,workload[ep].prio,workload[ep].cmd);
        rem_runner(m);
        printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
        pq_add(ep);
    }
    add_runner(pid);
    printf(" added to running queue\n");
    printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
    return true;
}

/* ================================================================
 * Main loop
 *
 * Each round:
 *  1. Remove finished processes
 *  2. Build candidate list: existing pending (in insertion order)
 *     + new arrivals (appended after pending, sorted by prio DESC
 *     with higher-pid tiebreak among themselves)
 *  3. Stable-sort entire candidate list by prio DESC
 *     (pending keeps relative order; equal-prio new arrivals go
 *      after equal-prio pending since appended after)
 *  4. Process candidates one by one: try_schedule each
 *     (evicted processes go to pq but are NOT in this round's list)
 *  5. Idle penalty
 * ================================================================ */
void time_loop(size_t ws, pstate **tl) {
    rq_sz=0; cpu_load=0; pq_sz=0;

    for(size_t i=0;i<ws;i++)
        for(size_t t=0;t<END_STEP;t++)
            tl[i][t]=pending;

    for(size_t t=0; t<MAX_SIM; t++) {
        int alive=0;
        for(size_t i=0;i<ws;i++) if(t<=workload[i].tf){alive=1;break;}
        if(!alive) break;

        printf("[t=%zu]\n",t);

        /* 1. Remove finished */
        for(int i=(int)rq_sz-1;i>=0;i--) {
            size_t pid=runq[i];
            if(workload[pid].tf < t) {
                printf("> process pid=%zu prio=%d ('%s') finished after time t=%zu\n",
                    pid,workload[pid].prio,workload[pid].cmd,workload[pid].tf);
                rem_runner(i);
                printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
            }
        }

        /* 2. Build candidate list:
              First: existing pending in insertion order (snapshot)
              Then:  new arrivals sorted prio DESC / higher-pid first */
        size_t cands[512]; size_t nc=0;

        /* Snapshot of existing pending */
        for(size_t k=0;k<pq_sz;k++) cands[nc++]=pq[k];
        size_t n_existing = nc;

        /* New arrivals sorted among themselves: prio DESC, higher pid first */
        size_t newp[256]; size_t nn=0;
        for(size_t i=0;i<ws;i++) if(workload[i].ts==t) newp[nn++]=i;
        /* sort new arrivals: prio desc, higher pid first */
        for(size_t i=1;i<nn;i++) {
            size_t key=newp[i]; int j=(int)i-1;
            while(j>=0) {
                int pa=workload[newp[j]].prio, pb=workload[key].prio;
                if(pa > pb) break;
                if(pa == pb && newp[j] > key) break; /* higher pid first */
                newp[j+1]=newp[j]; j--;
            }
            newp[j+1]=key;
        }
        for(size_t k=0;k<nn;k++) cands[nc++]=newp[k];

        /* 3. Stable-sort entire candidate list by prio DESC.
              Existing pending keeps relative order; new arrivals
              (appended after) stay after equal-prio pending. */
        stable_sort_prio_desc(cands, nc);

        /* 4. Process candidates: remove from pq if present, try schedule */
        for(size_t k=0;k<nc;k++) {
            size_t pid=cands[k];
            /* skip if not current (not yet started or already done) */
            if(workload[pid].ts > t || workload[pid].tf < t) continue;
            /* if in pq, remove it before trying */
            bool was_in_pq=false;
            for(size_t j=0;j<pq_sz;j++) {
                if(pq[j]==pid) { pq_remove_at(j); was_in_pq=true; break; }
            }
            bool ok=try_schedule(pid);
            if(!ok) pq_add(pid);
            /* evicted processes were added to pq inside try_schedule */
        }

        /* 5. Idle penalty */
        for(size_t k=0;k<pq_sz;k++) {
            size_t pid=pq[k];
            if(workload[pid].ts<=t && t<=workload[pid].tf) {
                workload[pid].idle++;
                workload[pid].tf++;
            }
        }

        /* 6. Print queues */
        size_t disp[256];
        for(size_t i=0;i<rq_sz;i++) disp[i]=runq[i];
        /* sort runq for display: prio desc, higher pid first */
        for(size_t i=1;i<rq_sz;i++) {
            size_t key=disp[i]; int j=(int)i-1;
            while(j>=0) {
                int pa=workload[disp[j]].prio,pb=workload[key].prio;
                if(pa>pb) break;
                if(pa==pb && disp[j]>key) break;
                disp[j+1]=disp[j]; j--;
            }
            disp[j+1]=key;
        }
        printf("> running: [");
        for(size_t i=0;i<rq_sz;i++) printf("(%d,%zu) ",workload[disp[i]].prio,disp[i]);
        printf("]\n> pending: [");
        for(size_t k=0;k<pq_sz;k++) printf("(%d,%zu) ",workload[pq[k]].prio,pq[k]);
        printf("]\n");

        /* 7. Record timeline */
        if(t<END_STEP)
            for(size_t i=0;i<rq_sz;i++) tl[runq[i]][t]=running;
    }

    for(size_t i=0;i<ws;i++) {
        size_t end=workload[i].tf+1;
        if(end>END_STEP) end=END_STEP;
        for(size_t t=end;t<END_STEP;t++) tl[i][t]=inactive;
    }
}

int main(int argc, char **argv) {
    FILE *input;
    if(argc>1) {
        if((input=fopen(argv[1],"r"))==NULL){perror("Error:");exit(EXIT_FAILURE);}
        printf("* Read from %s ...",argv[1]);
    } else { printf("* Read from stdin ..."); input=stdin; }
    fflush(stdout);
    size_t nr=count_lines_in_file(input);
    printf(" %zu lines in data.\n",nr);
    workload=malloc(sizeof(workload_item)*nr);
    size_t ws=read_data(nr,input);
    if(input!=stdin) fclose(input);
    printf("* Loaded %zu lines of data.\n",ws);
    printf("* starting scheduling on 1 CPUs\n");
    pstate **tl=alloc_timeline(END_STEP,ws);
    if(ws>0) time_loop(ws,tl);
    else { free(workload); return EXIT_FAILURE; }
    printf("* Chronogram === \n");
    chronogram(workload,ws,END_STEP);
    print_timeline(END_STEP,ws,tl);
    free_timeline(ws,tl);
    for(size_t i=0;i<ws;i++) free(workload[i].cmd);
    free(workload);
    return 0;
}