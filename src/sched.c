#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "sched.h"
#include "trace.h"
#include "process.h"
#include "cpu.h"

#define END_STEP 30



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

/* comparator: sort indices by priority descending */
static workload_item *g_workload;
int cmp_prio_desc(const void *a, const void *b) {
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;
    return g_workload[ib].prio - g_workload[ia].prio;
}

/**
 * @brief main loop for simulation: describe actions taken at each
 * time step from time ts to tf. 
 */
void time_loop(size_t workload_size, size_t ts, size_t tf,
               size_t ncpus, pstate **timeline) {
 
    g_workload = workload;
 
    size_t *current   = malloc(workload_size * sizeof(size_t));
    size_t *runq_idx  = malloc(workload_size * sizeof(size_t));
    size_t *pendq_idx = malloc(workload_size * sizeof(size_t));
    process *run_procs  = malloc(workload_size * sizeof(process));
    process *pend_procs = malloc(workload_size * sizeof(process));
 
    /* 
     * Step 0: pre-mark every process as 'pending' for all timesteps
     * from 0 to END_STEP-1. The post-pass below will fix up 'inactive'
     * for timesteps after a process truly finishes.
     */
    for (size_t i = 0; i < workload_size; i++)
        for (size_t t = 0; t < (size_t)END_STEP; t++)
            timeline[i][t] = pending;
 
    for (size_t t = ts; t <= tf; t++) {
 
        size_t n_current = 0, n_run = 0, n_pend = 0;
 
        /* 1. Collect current processes: ts_i <= t <= tf_i */
        for (size_t i = 0; i < workload_size; i++) {
            if (workload[i].ts <= t && t <= workload[i].tf)
                current[n_current++] = i;
        }
 
        /* 2. Sort by priority descending */
        qsort(current, n_current, sizeof(size_t), cmp_prio_desc);
 
        /* 3. Greedy fill run queue up to CPU_CAPABILITY */
        int cpu_load = 0;
        for (size_t k = 0; k < n_current; k++) {
            size_t idx = current[k];
            int p = workload[idx].prio;
            if (cpu_load + p <= CPU_CAPABILITY) {
                runq_idx[n_run++] = idx;
                cpu_load += p;
            } else {
                pendq_idx[n_pend++] = idx;
            }
        }
 
        /* 4. Penalise de-scheduled processes: idle++ and tf++ */
        for (size_t k = 0; k < n_pend; k++) {
            size_t idx = pendq_idx[k];
            workload[idx].idle++;
            workload[idx].tf++;
        }
 
        /* 5. Build process arrays for record_timeline */
        for (size_t k = 0; k < n_run; k++) {
            run_procs[k].pid  = (int)runq_idx[k];
            run_procs[k].prio = workload[runq_idx[k]].prio;
        }
        for (size_t k = 0; k < n_pend; k++) {
            pend_procs[k].pid  = (int)pendq_idx[k];
            pend_procs[k].prio = workload[pendq_idx[k]].prio;
        }
 
        /* 6. Log this timestep */
        printf("t=%zu | run:", t);
        for (size_t k = 0; k < n_run; k++)
            printf(" %zu(p=%d)", runq_idx[k], workload[runq_idx[k]].prio);
        printf(" | pend:");
        for (size_t k = 0; k < n_pend; k++)
            printf(" %zu(p=%d)", pendq_idx[k], workload[pendq_idx[k]].prio);
        printf(" | cpu_load=%d\n", cpu_load);
 
        /* 7. Record running state in timeline (pending already set) */
        for (size_t k = 0; k < n_run; k++)
            timeline[runq_idx[k]][t] = running;
    }
 
    /*
     * Step 8: post-pass — mark timesteps AFTER a process's final tf as inactive.
     * The final tf is now stored in workload[i].tf after all the idle increments.
     */
    for (size_t i = 0; i < workload_size; i++) {
        for (size_t t = workload[i].tf + 1; t < (size_t)END_STEP; t++)
            timeline[i][t] = inactive;
    }
 
    free(current); free(runq_idx); free(pendq_idx);
    free(run_procs); free(pend_procs);
}
 

/**
 * main
 */
int main(int argc, char** argv) {
	FILE *input;
	if (argc > 1) { // if one arg, use it to read in data 
		if ((input = fopen(argv[1],"r")) == NULL) {
			perror("Error reading file:");
			exit(EXIT_FAILURE);
		}
		else
			printf("* Read from %s ...", argv[1]);
	}
	else { // no arg provided, read from stdin
			printf("* Read from stdin ...");
			input = stdin;
	}
	// read from standard input
	fflush(stdout);
	size_t nr = count_lines_in_file(input);

	printf(" %zu lines in data.\n", nr);

	workload = malloc(sizeof(workload_item) * nr);
	size_t workload_size = read_data(nr, input);
	printf("* Loaded %zu lines of data.\n", nr);
	pstate **timeline = alloc_timeline(END_STEP, workload_size);

	if (nr > 0) 
		time_loop(workload_size, 0, END_STEP-1, MAX_CPU, timeline);
	else
		return EXIT_FAILURE;


	printf("* Chronogram === \n");
	chronogram(workload, workload_size, END_STEP-1);
	print_timeline(END_STEP-1, workload_size, timeline);
	free(workload);
	return 0;
}
