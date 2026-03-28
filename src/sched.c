#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "sched.h"
#include "trace.h"
#include "process.h"
#include "cpu.h"

#define END_STEP 29
#define MAX_SIM  500


struct workload_item_t {
	int pid;       //< the event id
    int ppid;      //< the event parent id
	size_t ts;     //< start date
	size_t tf;     //< finish date
	size_t idle;   //< total time the process has been idle;
	char *cmd;     //< the binary name
	int prio;      //< process priority
};

enum  epoch { before, in, after };  //< to compare a date and a timeframe 
typedef enum epoch epoch;


   /**
    *            0,init
    *         /           \
	*      1,bash          2,bash
	*     /   \   \        /      \ 
	* 3,find   \   \      |       |
	*         4,gcc \     |       |
	*            |   |    |       |
	*	       5,ld  |    |       | 
	*                |    6,ssh   |
	*                |    |       |
	*                |    7,crypt |
	*                |           8,snake
    *               9,cat
    */

   /*
	workload_item workload[] = {
	//  pid ppid  ts  tf idle  cmd     prio
	    {0, -1,    0, 18,  0, "init",  10 },
        {1,  0,    1, 16,  0, "bash",   1 },
        {2,  0,    3, 16,  0, "bash",   1 },
        {3,  1,    4,  6,  0, "find",   2 },
        {4,  1,    7,  9,  0, "gcc",    5 },
		{5,  4,    8,  9,  0, "ld",     4 }, 
		{6,  2,   10, 13,  0, "ssh",    3 },
        {7,  6,   11, 13,  0, "crypt",  5 },
        {8,  2,   14, 16,  0, "snake",  4 },
        {9,  1,   14, 15,  0, "cat",    5 },
	};
	*/
void draw_hbar(char c, size_t width) {
	char bar[width+1];
	memset(bar, c, width);
	bar[width]='\0';
	printf("%s",bar);
}

void chronogram(workload_item* workload, size_t nb_processes, size_t timesteps) {
	// drw timeliine
	size_t tick;
	size_t freq=5;
	printf("\t");
	for (tick=0; tick<timesteps; tick++) {
		if (tick%freq==0) printf("|"); else printf(".");
	}
	printf("\n");
	// draw processes lifetime
	for (size_t i=0; i<nb_processes; i++) {
		printf("%s\t", workload[i].cmd);
		draw_hbar(' ',workload[i].ts);
		draw_hbar('X',workload[i].tf-workload[i].ts);
		printf("\t\t\t (tf=%zu,idle=%zu)\n", workload[i].tf, workload[i].idle);
	}
}


/**
 * @brief count lines in file
 * 
 * @param file assumed to be an open file
 * @return the number of lines in files 
 */
size_t count_lines_in_file(FILE *file) {
    int lines = 0;
    char ch;
    // Count the number of newline characters
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            lines++;
        }
    }
    // If the file is not empty and the last line doesn't end with '\n'
    if (ch != '\n' && lines != 0) {
        lines++;
    }
    rewind(file);
    return lines;
}

/**
 * @brief Read the workload data
 * 
 * @param filename the name of file, can be STDIN
 * @return number of data lines read in the file
 */
size_t read_data(size_t workload_size, FILE *file) {
    size_t count = 0;
    char line[256];  // Buffer for each line in the file
    char cmd[50];    // Buffer for command name

    while (fgets(line, sizeof(line), file) && count < workload_size) {
        workload_item item;
        // Parse the line into the workload_item structure
        if (sscanf(line, "%d %d %zu %zu %zu %s %d",
                   &item.pid, &item.ppid, &item.ts, &item.tf,
                   &item.idle, cmd, &item.prio) == 7) {
            item.cmd = strdup(cmd);  // Duplicate the string for command name
            workload[count++] = item;
        } else {
            fprintf(stderr, "Error parsing line: %s\n", line);
			return false;
        }
	}
	return count;
}
/* --- Run queue --- */
static size_t runq[256]; static size_t rq_sz=0; static int cpu_load=0;
 
static int min_runner(void) { /* returns index of lowest prio in runq, or -1 */
    if(!rq_sz) return -1;
    int m=0;
    for(size_t i=1;i<rq_sz;i++) {
        int pi=workload[runq[i]].prio, pm=workload[runq[m]].prio;
        if(pi < pm) m=i;
        else if(pi == pm && runq[i] > runq[m]) m=i; /* tie: evict higher pid */
    }
    return m;
}
static void rem_runner(int idx) {
    cpu_load-=workload[runq[idx]].prio;
    runq[idx]=runq[--rq_sz];
}
static void add_runner(size_t pid) { runq[rq_sz++]=pid; cpu_load+=workload[pid].prio; }
 
static int cmp_pd(const void *a,const void *b) {
    size_t ia=*(size_t*)a,ib=*(size_t*)b;
    int d=workload[ib].prio-workload[ia].prio;
    if(d) return d;
    return (int)ib-(int)ia; /* tie: higher pid first */
}
 
/* --- Pending queue as a set (no duplicates) --- */
static size_t pq[512]; static size_t pq_sz=0;
 
static bool pq_contains(size_t pid) {
    for(size_t i=0;i<pq_sz;i++) if(pq[i]==pid) return true;
    return false;
}
static void pq_add(size_t pid) {
    if(!pq_contains(pid)) pq[pq_sz++]=pid;
}
static void pq_remove_at(size_t idx) { pq[idx]=pq[--pq_sz]; }
 
/*
 * try_schedule: attempt to put pid into run queue.
 * While it doesn't fit, evict the lowest-prio runner (if new prio > runner prio).
 * Evicted pids are added to pq (pending set) directly.
 * Returns true if pid was added to run queue.
 */
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
        pq_add(ep); /* evicted -> pending (will be processed next round) */
    }
    add_runner(pid);
    printf(" added to running queue\n");
    printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
    return true;
}
 
void time_loop(size_t ws, pstate **tl) {
    rq_sz=0; cpu_load=0; pq_sz=0;
 
    for(size_t i=0;i<ws;i++)
        for(size_t t=0;t<END_STEP;t++)
            tl[i][t]=pending;
 
    for(size_t t=0;t<MAX_SIM;t++) {
        /* stop when nothing left */
        int alive=0;
        for(size_t i=0;i<ws;i++) if(t<=workload[i].tf){alive=1;break;}
        if(!alive) break;
 
        printf("[t=%zu]\n",t);
 
        /* 1. Remove finished processes */
        for(int i=(int)rq_sz-1;i>=0;i--) {
            size_t pid=runq[i];
            if(workload[pid].tf < t) {
                printf("> process pid=%zu prio=%d ('%s') finished after time t=%zu\n",
                    pid,workload[pid].prio,workload[pid].cmd,workload[pid].tf);
                rem_runner(i);
                printf("> CPU occupation: CPU[0]=%d \n",cpu_load);
            }
        }
 
        /* 2. Take snapshot of pending queue BEFORE processing new arrivals.
              Step 3 will only retry these pids (not ones evicted during step 2). */
        size_t snap[512]; size_t snap_sz=pq_sz;
        for(size_t k=0;k<pq_sz;k++) snap[k]=pq[k];
        qsort(snap,snap_sz,sizeof(size_t),cmp_pd);
 
        /* Schedule NEW arrivals (ts==t), sorted prio desc.
           Evicted go to pq but are NOT retried this round (not in snapshot). */
        size_t newp[256]; size_t nn=0;
        for(size_t i=0;i<ws;i++) if(workload[i].ts==t) newp[nn++]=i;
        qsort(newp,nn,sizeof(size_t),cmp_pd);
        for(size_t k=0;k<nn;k++) {
            bool ok=try_schedule(newp[k]);
            if(!ok) pq_add(newp[k]);
        }
 
        /* 3. Schedule EXISTING pending (snapshot from before step 2).
              Evicted during this phase also skip this loop. */
 
        for(size_t k=0;k<snap_sz;k++) {
            size_t pid=snap[k];
            /* skip if no longer in pq (could have been evicted out and re-added,
               but with set semantics it's fine - just check it's still current) */
            if(!pq_contains(pid)) continue;
            if(workload[pid].ts>t || workload[pid].tf<t) continue;
            /* remove from pq before trying */
            for(size_t j=0;j<pq_sz;j++) {
                if(pq[j]==pid) { pq_remove_at(j); break; }
            }
            bool ok=try_schedule(pid);
            if(!ok) pq_add(pid); /* back to pq if failed */
            /* if ok: evicted ones were added to pq inside try_schedule */
        }
 
        /* 4. Idle penalty */
        for(size_t k=0;k<pq_sz;k++) {
            size_t pid=pq[k];
            if(workload[pid].ts<=t && t<=workload[pid].tf) {
                workload[pid].idle++; workload[pid].tf++;
            }
        }
 
        /* 5. Print queues */
        size_t disp[256];
        for(size_t i=0;i<rq_sz;i++) disp[i]=runq[i];
        qsort(disp,rq_sz,sizeof(size_t),cmp_pd);
        printf("> running: [");
        for(size_t i=0;i<rq_sz;i++) printf("(%d,%zu) ",workload[disp[i]].prio,disp[i]);
        printf("]\n> pending: [");
        /* sort pq for display */
        size_t pdisp[512];
        for(size_t i=0;i<pq_sz;i++) pdisp[i]=pq[i];
        qsort(pdisp,pq_sz,sizeof(size_t),cmp_pd);
        for(size_t k=0;k<pq_sz;k++) printf("(%d,%zu) ",workload[pdisp[k]].prio,pdisp[k]);
        printf("]\n");
 
        /* 6. Record timeline */
        if(t<END_STEP)
            for(size_t i=0;i<rq_sz;i++) tl[runq[i]][t]=running;
    }
 
    /* post-pass: inactive after final tf */
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