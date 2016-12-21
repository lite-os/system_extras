/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/statfs.h>
#include "ioshark.h"
#define IOSHARK_MAIN
#include "ioshark_bench.h"

#define UNUSED_PARAM(X)		((void)X)

char *progname;

#define MAX_INPUT_FILES		1024
#define MAX_THREADS		1024

struct thread_state_s {
	char *filename;
	FILE *fp;
	int num_files;
	void *db_handle;
};

struct thread_state_s thread_state[MAX_INPUT_FILES];
int num_input_files = 0;
int next_input_file;

pthread_t tid[MAX_THREADS];

/*
 * Global options
 */
int do_delay = 0;

#if 0
static long gettid()
{
        return syscall(__NR_gettid);
}
#endif

void usage()
{
	fprintf(stderr, "%s [-d preserve_delays] [-n num_iterations] [-t num_threads] <list of parsed input files>\n",
		progname);
	exit(EXIT_FAILURE);
}

pthread_mutex_t time_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t work_mutex = PTHREAD_MUTEX_INITIALIZER;
struct timeval aggregate_file_create_time;
struct timeval aggregate_file_remove_time;
struct timeval aggregate_IO_time;
struct timeval aggregate_delay_time;

u_int64_t aggr_op_counts[IOSHARK_MAX_FILE_OP];
struct rw_bytes_s aggr_io_rw_bytes;
struct rw_bytes_s aggr_create_rw_bytes;

static void
update_time(struct timeval *aggr_time,
	    struct timeval *delta_time)
{
	struct timeval tmp;

	pthread_mutex_lock(&time_mutex);
	timeradd(aggr_time, delta_time, &tmp);
	*aggr_time = tmp;
	pthread_mutex_unlock(&time_mutex);
}

static void
update_op_counts(u_int64_t *op_counts)
{
	int i;

	pthread_mutex_lock(&stats_mutex);
	for (i = IOSHARK_LSEEK ; i < IOSHARK_MAX_FILE_OP ; i++)
		aggr_op_counts[i] += op_counts[i];
	pthread_mutex_unlock(&stats_mutex);
}

static void
update_byte_counts(struct rw_bytes_s *dest, struct rw_bytes_s *delta)
{
	pthread_mutex_lock(&stats_mutex);
	dest->bytes_read += delta->bytes_read;
	dest->bytes_written += delta->bytes_written;
	pthread_mutex_unlock(&stats_mutex);
}

static int work_next_file;
static int work_num_files;

void
init_work(int next_file, int num_files)
{
	pthread_mutex_lock(&work_mutex);
	work_next_file = next_file;
	work_num_files = work_next_file + num_files;
	pthread_mutex_unlock(&work_mutex);
}

/* Dole out the next file to work on to the thread */
static struct thread_state_s *
get_work()
{
	struct thread_state_s *work = NULL;

	pthread_mutex_lock(&work_mutex);
	if (work_next_file < work_num_files)
		work = &thread_state[work_next_file++];
	pthread_mutex_unlock(&work_mutex);
	return work;
}

static void
create_files(struct thread_state_s *state)
{
	struct timeval file_create_time;
	int i;
	struct ioshark_file_state file_state;
	char path[512];
	void *db_node;
	struct rw_bytes_s rw_bytes;

	timerclear(&file_create_time);
	memset(&rw_bytes, 0, sizeof(struct rw_bytes_s));
	for (i = 0 ; i < state->num_files ; i++) {
		if (fread(&file_state, sizeof(struct ioshark_file_state),
			  1, state->fp) != 1) {
			fprintf(stderr, "%s read error tracefile\n",
				progname);
			exit(EXIT_FAILURE);
		}
		sprintf(path, "file.%d.%d",
			(int)(state - thread_state),
			file_state.fileno);
		create_file(path, file_state.size,
			    &file_create_time, &rw_bytes);
		db_node = files_db_add_byfileno(state->db_handle,
						file_state.fileno);
		files_db_update_size(db_node, file_state.size);
		files_db_update_filename(db_node, path);
	}
	update_byte_counts(&aggr_create_rw_bytes, &rw_bytes);
	update_time(&aggregate_file_create_time, &file_create_time);
}

static void
do_one_io(void *db_node,
	  struct ioshark_file_operation *file_op,
	  struct timeval *io_time, u_int64_t *op_counts,
	  struct rw_bytes_s *rw_bytes,
	  char **bufp, int *buflen)
{
	struct timeval start;

	assert(file_op->file_op < IOSHARK_MAX_FILE_OP);
	op_counts[file_op->file_op]++;
	switch (file_op->file_op) {
	int ret;
	char *p;
	int fd;

	case IOSHARK_LSEEK:
	case IOSHARK_LLSEEK:
	(void)gettimeofday(&start, (struct timezone *)NULL);
		ret = lseek(files_db_get_fd(db_node),
			    file_op->lseek_offset,
			    file_op->lseek_action);
		update_delta_time(&start, io_time);
		if (ret < 0) {
			fprintf(stderr,
				"%s: lseek(%s %lu %d) returned error %d\n",
				progname, files_db_get_filename(db_node),
				file_op->lseek_offset,
				file_op->lseek_action, errno);
			exit(EXIT_FAILURE);
		}
		break;
	case IOSHARK_PREAD64:
		p = get_buf(bufp, buflen, file_op->prw_len, 0);
		(void)gettimeofday(&start,
				   (struct timezone *)NULL);
		ret = pread(files_db_get_fd(db_node), p,
			    file_op->prw_len, file_op->prw_offset);
		update_delta_time(&start, io_time);
		rw_bytes->bytes_read += file_op->prw_len;
		if (ret < 0) {
			fprintf(stderr,
				"%s: pread(%s %zu %lu) error %d\n",
				progname,
				files_db_get_filename(db_node),
				file_op->prw_len,
				file_op->prw_offset, errno);
			exit(EXIT_FAILURE);
		}
		break;
	case IOSHARK_PWRITE64:
		p = get_buf(bufp, buflen, file_op->prw_len, 1);
		(void)gettimeofday(&start,
				   (struct timezone *)NULL);
		ret = pwrite(files_db_get_fd(db_node), p,
			     file_op->prw_len, file_op->prw_offset);
		update_delta_time(&start, io_time);
		rw_bytes->bytes_written += file_op->prw_len;
		if (ret < 0) {
			fprintf(stderr,
				"%s: pwrite(%s %zu %lu) error %d\n",
				progname,
				files_db_get_filename(db_node),
				file_op->prw_len,
				file_op->prw_offset, errno);
			exit(EXIT_FAILURE);
		}
		break;
	case IOSHARK_READ:
		p = get_buf(bufp, buflen, file_op->rw_len, 0);
		(void)gettimeofday(&start,
				   (struct timezone *)NULL);
		ret = read(files_db_get_fd(db_node), p,
			   file_op->rw_len);
		update_delta_time(&start, io_time);
		rw_bytes->bytes_read += file_op->rw_len;
		if (ret < 0) {
			fprintf(stderr,
				"%s: read(%s %zu) error %d\n",
				progname,
				files_db_get_filename(db_node),
				file_op->rw_len,
				errno);
			exit(EXIT_FAILURE);
		}
		break;
	case IOSHARK_WRITE:
		p = get_buf(bufp, buflen, file_op->rw_len, 1);
		(void)gettimeofday(&start,
				   (struct timezone *)NULL);
		ret = write(files_db_get_fd(db_node), p,
			    file_op->rw_len);
		update_delta_time(&start, io_time);
		rw_bytes->bytes_written += file_op->rw_len;
		if (ret < 0) {
			fprintf(stderr,
				"%s: write(%s %zu) error %d\n",
				progname,
				files_db_get_filename(db_node),
				file_op->rw_len,
				errno);
			exit(EXIT_FAILURE);
		}
		break;
	case IOSHARK_MMAP:
	case IOSHARK_MMAP2:
		ioshark_handle_mmap(db_node, file_op,
				    bufp, buflen, op_counts,
				    rw_bytes, io_time);
		break;
	case IOSHARK_OPEN:
		if (file_op->open_flags & O_CREAT) {
			(void)gettimeofday(&start,
					   (struct timezone *)NULL);
			fd = open(files_db_get_filename(db_node),
				  file_op->open_flags,
				  file_op->open_mode);
			if (fd < 0) {
				/*
				 * EEXIST error acceptable, others are fatal.
				 * Although we failed to O_CREAT the file (O_EXCL)
				 * We will force an open of the file before any
				 * IO.
				 */
				if (errno == EEXIST) {
					update_delta_time(&start, io_time);
					return;
				} else {
					fprintf(stderr,
						"%s: O_CREAT open(%s %x %o) error %d\n",
						progname,
						files_db_get_filename(db_node),
						file_op->open_flags,
						file_op->open_mode, errno);
					exit(EXIT_FAILURE);
				}
			} else
				update_delta_time(&start, io_time);
		} else {
			(void)gettimeofday(&start,
					   (struct timezone *)NULL);
			fd = open(files_db_get_filename(db_node),
				  file_op->open_flags);
			update_delta_time(&start, io_time);
			if (fd < 0) {
				if (file_op->open_flags & O_DIRECTORY) {
					/* O_DIRECTORY open()s should fail */
					return;
				} else {
					fprintf(stderr,
						"%s: open(%s %x) error %d\n",
						progname,
						files_db_get_filename(db_node),
						file_op->open_flags,
						errno);
					exit(EXIT_FAILURE);
				}
			}
		}
		files_db_close_fd(db_node);
		files_db_update_fd(db_node, fd);
		break;
	case IOSHARK_FSYNC:
	case IOSHARK_FDATASYNC:
		if (file_op->file_op == IOSHARK_FSYNC) {
			(void)gettimeofday(&start,
					   (struct timezone *)NULL);
			ret = fsync(files_db_get_fd(db_node));
			update_delta_time(&start, io_time);
			if (ret < 0) {
				fprintf(stderr,
					"%s: fsync(%s) error %d\n",
					progname,
					files_db_get_filename(db_node),
					errno);
				exit(EXIT_FAILURE);
			}
		} else {
			(void)gettimeofday(&start,
					   (struct timezone *)NULL);
			ret = fdatasync(files_db_get_fd(db_node));
			update_delta_time(&start, io_time);
			if (ret < 0) {
				fprintf(stderr,
					"%s: fdatasync(%s) error %d\n",
					progname,
					files_db_get_filename(db_node),
					errno);
				exit(EXIT_FAILURE);
			}
		}
		break;
	case IOSHARK_CLOSE:
		(void)gettimeofday(&start,
				   (struct timezone *)NULL);
		ret = close(files_db_get_fd(db_node));
		update_delta_time(&start, io_time);
		if (ret < 0) {
			fprintf(stderr,
				"%s: close(%s) error %d\n",
				progname,
				files_db_get_filename(db_node), errno);
			exit(EXIT_FAILURE);
		}
		files_db_update_fd(db_node, -1);
		break;
	default:
		fprintf(stderr, "%s: unknown FILE_OP %d\n",
			progname, file_op->file_op);
		exit(EXIT_FAILURE);
		break;
	}
}

static void
do_io(struct thread_state_s *state)
{
	void *db_node;
	struct ioshark_header header;
	struct ioshark_file_operation file_op;
	u_int64_t prev_delay = 0;
	int fd;
	int i;
	char *buf = NULL;
	int buflen = 0;
	struct timeval total_io_time;
	struct timeval total_delay_time;
	struct timeval start;
	u_int64_t op_counts[IOSHARK_MAX_FILE_OP];
	struct rw_bytes_s rw_bytes;

	rewind(state->fp);
	if (fread(&header, sizeof(struct ioshark_header), 1, state->fp) != 1) {
		fprintf(stderr, "%s read error %s\n",
			progname, state->filename);
		exit(EXIT_FAILURE);
	}
	/*
	 * First open and pre-create all the files. Indexed by fileno.
	 */
	timerclear(&total_io_time);
	timerclear(&total_delay_time);
	memset(&rw_bytes, 0, sizeof(struct rw_bytes_s));
	memset(op_counts, 0, sizeof(op_counts));
	fseek(state->fp,
	      sizeof(struct ioshark_header) +
	      header.num_files * sizeof(struct ioshark_file_state),
	      SEEK_SET);
	/*
	 * Loop over all the IOs, and launch each
	 */
	for (i = 0 ; i < header.num_io_operations ; i++) {
		if (fread(&file_op, sizeof(struct ioshark_file_operation),
			  1, state->fp) != 1) {
			fprintf(stderr, "%s read error trace.outfile\n",
				progname);
			exit(EXIT_FAILURE);
		}
		if (do_delay) {
			(void)gettimeofday(&start, (struct timezone *)NULL);
			usleep(file_op.delta_us - prev_delay);
			update_delta_time(&start, &total_delay_time);
			prev_delay = file_op.delta_us;
		}
		db_node = files_db_lookup_byfileno(state->db_handle,
						   file_op.fileno);
		if (db_node == NULL) {
			fprintf(stderr,
				"%s Can't lookup fileno %d, fatal error\n",
				progname, file_op.fileno);
			exit(EXIT_FAILURE);
		}
		if (file_op.file_op != IOSHARK_OPEN &&
		    files_db_get_fd(db_node) == -1) {
			/*
			 * This is a hack to workaround the fact that we did not
			 * see an open() for this file until now. open() the file
			 * O_RDWR, so that we can perform the IO.
			 */
			fd = open(files_db_get_filename(db_node), O_RDWR);
			if (fd < 0) {
				fprintf(stderr, "%s: open(%s O_RDWR) error %d\n",
					progname, files_db_get_filename(db_node),
					errno);
				exit(EXIT_FAILURE);
			}
			files_db_update_fd(db_node, fd);
		}
		do_one_io(db_node, &file_op, &total_io_time,
			  op_counts, &rw_bytes, &buf, &buflen);
	}
	(void)gettimeofday(&start, (struct timezone *)NULL);
	files_db_fsync_discard_files(state->db_handle);
	files_db_close_files(state->db_handle);
	update_delta_time(&start, &total_io_time);
	update_time(&aggregate_IO_time, &total_io_time);
	update_time(&aggregate_delay_time, &total_delay_time);
	update_op_counts(op_counts);
	update_byte_counts(&aggr_io_rw_bytes, &rw_bytes);
}

void *
io_thread(void *unused)
{
	struct thread_state_s *state;

	UNUSED_PARAM(unused);
	srand(gettid());
	while ((state = get_work()))
		do_io(state);
	pthread_exit(NULL);
        return(NULL);
}

static void
do_create(struct thread_state_s *state)
{
	struct ioshark_header header;

	if (fread(&header, sizeof(struct ioshark_header), 1, state->fp) != 1) {
		fprintf(stderr, "%s read error %s\n",
			progname, state->filename);
		exit(EXIT_FAILURE);
	}
	state->num_files = header.num_files;
	state->db_handle = files_db_create_handle();
	create_files(state);
}

void *
create_files_thread(void *unused)
{
	struct thread_state_s *state;

	UNUSED_PARAM(unused);
	while ((state = get_work()))
		do_create(state);
	pthread_exit(NULL);
	return(NULL);
}

int
get_start_end(int *start_ix)
{
	int i, j, ret_numfiles;
	u_int64_t free_fs_bytes;
	char *infile;
	FILE *fp;
	struct ioshark_header header;
	struct ioshark_file_state file_state;
	struct statfs fsstat;
	static int fssize_clamp_next_index = 0;

	if (fssize_clamp_next_index == num_input_files)
		return 0;
	if (statfs("/data/local/tmp", &fsstat) < 0) {
		fprintf(stderr, "%s: Can't statfs /data/local/tmp\n",
			progname);
		exit(EXIT_FAILURE);
	}
	free_fs_bytes = (fsstat.f_bavail * fsstat.f_bsize) * 9 /10;
	for (i = fssize_clamp_next_index; i < num_input_files; i++) {
		infile = thread_state[i].filename;
		fp = fopen(infile, "r");
		if (fp == NULL) {
			fprintf(stderr, "%s: Can't open %s\n",
				progname, infile);
			exit(EXIT_FAILURE);
		}
		if (fread(&header, sizeof(struct ioshark_header),
			  1, fp) != 1) {
			fprintf(stderr, "%s read error %s\n",
				progname, infile);
			exit(EXIT_FAILURE);
		}
		for (j = 0 ; j < header.num_files ; j++) {
			if (fread(&file_state, sizeof(struct ioshark_file_state),
				  1, fp) != 1) {
				fprintf(stderr, "%s read error tracefile\n",
					progname);
				exit(EXIT_FAILURE);
			}
			if (file_state.size > free_fs_bytes) {
				fclose(fp);
				printf("Reducing number of input files from %d down to %d\n",
				       num_input_files,
				       i - fssize_clamp_next_index);
				goto out;
			}
			free_fs_bytes -= file_state.size;
		}
		fclose(fp);
	}
out:
	*start_ix = fssize_clamp_next_index;
	ret_numfiles = i - fssize_clamp_next_index;
	fssize_clamp_next_index = i;
	return ret_numfiles;
}

int
ioshark_pthread_create(pthread_t *tidp, void *(*start_routine)(void *))
{
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
	pthread_attr_setstacksize(&attr, (size_t)(1024*1024));
	return pthread_create(tidp, &attr, start_routine, (void *)NULL);
}

void
wait_for_threads(int num_threads)
{
	int i;

	for (i = 0; i < num_threads; i++) {
		pthread_join(tid[i], NULL);
		tid[i] = 0;
	}
}

int
main(int argc, char **argv)
{
	int i;
	FILE *fp;
	struct stat st;
	char *infile;
	int num_threads = 0;
	int num_iterations = 1;
	int c;
	int num_files, start_file;
	struct thread_state_s *state;

	progname = argv[0];
        while ((c = getopt(argc, argv, "dn:t:")) != EOF) {
                switch (c) {
                case 'd':
			do_delay = 1;
			break;
                case 'n':
			num_iterations = atoi(optarg);
			break;
                case 't':
			num_threads = atoi(optarg);
			break;
 	        default:
                        usage();
		}
	}

	if (num_threads > MAX_THREADS)
		usage();

	if (optind == argc)
                usage();

	for (i = optind; i < argc; i++) {
		infile = argv[i];
		if (stat(infile, &st) < 0) {
			fprintf(stderr, "%s: Can't stat %s\n",
				progname, infile);
			exit(EXIT_FAILURE);
			continue;
		}
		if (st.st_size == 0) {
			fprintf(stderr, "%s: Empty file %s\n",
				progname, infile);
			continue;
		}
		fp = fopen(infile, "r");
		if (fp == NULL) {
			fprintf(stderr, "%s: Can't open %s\n",
				progname, infile);
			continue;
		}
		thread_state[num_input_files].filename = infile;
		thread_state[num_input_files].fp = fp;
		num_input_files++;
	}

	if (num_input_files == 0) {
		exit(EXIT_SUCCESS);
	}
	printf("Total Input Files = %d\n", num_input_files);
	printf("Num Iterations = %d\n", num_iterations);

	timerclear(&aggregate_file_create_time);
	timerclear(&aggregate_file_remove_time);
	timerclear(&aggregate_IO_time);

	/*
	 * We pre-create the files that we need once and then we
	 * loop around N times doing IOs on the pre-created files.
	 *
	 * get_start_end() breaks up the total work here to make sure
	 * that all the files we need to pre-create fit into the
	 * available space in /data/local/tmp (hardcoded for now).
	 *
	 * If it won't fit, then we do several sweeps.
	 */
	while ((num_files = get_start_end(&start_file))) {
		/* Create files once */
		printf("Doing Pre-creation of Files\n");
		if (num_threads == 0 || num_threads > num_files)
			num_threads = num_files;
		(void)system("echo 3 > /proc/sys/vm/drop_caches");
		init_work(start_file, num_files);
		for (i = 0; i < num_threads; i++) {
			if (ioshark_pthread_create(&(tid[i]),
						   create_files_thread)) {
				fprintf(stderr,
					"%s: Can't create creator thread %d\n",
					progname, i);
				exit(EXIT_FAILURE);
			}
		}
		wait_for_threads(num_threads);

		/* Do the IOs N times */
		for (i = 0 ; i < num_iterations ; i++) {
			(void)system("echo 3 > /proc/sys/vm/drop_caches");
			printf("Starting Test. Iteration %d\n", i);
			init_work(start_file, num_files);
			for (c = 0; c < num_threads; c++) {
				if (ioshark_pthread_create(&(tid[c]),
							   io_thread)) {
					fprintf(stderr,
						"%s: Can't create thread %d\n",
						progname, c);
					exit(EXIT_FAILURE);
				}
			}
			wait_for_threads(num_threads);
		}
		/*
		 * We are done with the N iterations of IO.
		 * Destroy the files we pre-created.
		 */
		init_work(start_file, num_files);
		while ((state = get_work())) {
			struct timeval start;

			(void)gettimeofday(&start, (struct timezone *)NULL);
			files_db_unlink_files(state->db_handle);
			update_delta_time(&start, &aggregate_file_remove_time);
			files_db_free_memory(state->db_handle);
		}
	}
	printf("Total Creation time = %llu.%llu (msecs.usecs)\n",
	       (unsigned long long)get_msecs(&aggregate_file_create_time),
	       (unsigned long long)get_usecs(&aggregate_file_create_time));
	printf("Total Remove time = %llu.%llu (msecs.usecs)\n",
	       (unsigned long long)get_msecs(&aggregate_file_remove_time),
	       (unsigned long long)get_usecs(&aggregate_file_remove_time));
	if (do_delay)
		printf("Total delay time = %llu.%llu (msecs.usecs)\n",
		       (unsigned long long)get_msecs(&aggregate_delay_time),
		       (unsigned long long)get_usecs(&aggregate_delay_time));
	printf("Total IO time = %llu.%llu (msecs.usecs)\n",
	       (unsigned long long)get_msecs(&aggregate_IO_time),
	       (unsigned long long)get_usecs(&aggregate_IO_time));
	print_bytes("Upfront File Creation bytes",
		    &aggr_create_rw_bytes);
	print_bytes("IO bytes", &aggr_io_rw_bytes);
	print_op_stats(aggr_op_counts);
}
