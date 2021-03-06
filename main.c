// Gateless Gate, a Zcash miner
// Copyright 2016 zawawa @ bitcointalk.org
//
// The initial version of this software was based on:
// SILENTARMY v5
// The MIT License (MIT) Copyright (c) 2016 Marc Bevand, Genoil
//
// This program is free software : you can redistribute it and / or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.If not, see <http://www.gnu.org/licenses/>.

#ifdef WIN32
#pragma comment(lib, "winmm.lib")
#define _CRT_RAND_S 
#endif

#define _GNU_SOURCE	1/* memrchr */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>


#include <errno.h>
#include <CL/cl.h>
#include "blake.h"
#include "sha256.h"

#ifdef WIN32

#undef _UNICODE // @mrb quick patch to make win getopt work

#include <Winsock2.h>
#include <io.h>
#include <BaseTsd.h>
#include "windows/gettimeofday.h"
#include "windows/getopt.h"
#include "windows/memrchr.h"

typedef SSIZE_T ssize_t;

#define open _open
#define read _read
#define write _write
#define close _close
#define snprintf _snprintf

#else

#include <sys/time.h>
#include <unistd.h>
#include <getopt.h>
#include "_kernel.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#endif

typedef uint8_t		uchar;
typedef uint32_t	uint;
#ifdef NVIDIA
#include "param-nvidia.h"
#else
#include "param.h"
#endif

#define MIN(A, B)	(((A) < (B)) ? (A) : (B))
#define MAX(A, B)	(((A) > (B)) ? (A) : (B))

int             verbose = 0;
uint32_t	show_encoded = 0;
uint64_t	nr_nonces = 1;
uint32_t	do_list_devices = 0;
uint32_t	gpu_to_use = 0;
uint32_t	mining = 0;
struct timeval kern_avg_run_time;
int amd_flag = 0;
const char *source = NULL;
size_t source_len;
const char *binary = NULL;
size_t binary_len;

typedef struct  debug_s
{
	uint32_t    dropped_coll;
	uint32_t    dropped_stor;
}               debug_t;

void debug(const char *fmt, ...)
{
	va_list     ap;
	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void warn(const char *fmt, ...)
{
	va_list     ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void fatal(const char *fmt, ...)
{
	va_list     ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

uint64_t parse_num(char *str)
{
	char	*endptr;
	uint64_t	n;
	n = strtoul(str, &endptr, 0);
	if (endptr == str || *endptr)
		fatal("'%s' is not a valid number\n", str);
	return n;
}

uint64_t now(void)
{
	struct timeval	tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 * 1000 + tv.tv_usec;
}

void show_time(uint64_t t0)
{
	uint64_t            t1;
	t1 = now();
	fprintf(stderr, "Elapsed time: %.1f msec\n", (t1 - t0) / 1e3);
}

#ifndef WIN32
void set_blocking_mode(int fd, int block)
{

	int	f;
	if (-1 == (f = fcntl(fd, F_GETFL)))
		fatal("fcntl F_GETFL: %s\n", strerror(errno));
	if (-1 == fcntl(fd, F_SETFL, block ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK)))
		fatal("fcntl F_SETFL: %s\n", strerror(errno));
}
#endif

void randomize(void *p, ssize_t l)
{
#ifndef WIN32
	const char	*fname = "/dev/urandom";
	int		fd;
	ssize_t	ret;
	if (-1 == (fd = open(fname, O_RDONLY)))
		fatal("open %s: %s\n", fname, strerror(errno));
	if (-1 == (ret = read(fd, p, l)))
		fatal("read %s: %s\n", fname, strerror(errno));
	if (ret != l)
		fatal("%s: short read %d bytes out of %d\n", fname, ret, l);
	if (-1 == close(fd))
		fatal("close %s: %s\n", fname, strerror(errno));
#else
	for (int i = 0; i < l; i++) {
		unsigned int ui;
		rand_s(&ui);
		((uint8_t *)p)[i] = ui & 0xff;
	}
#endif
}

struct timeval time_diff(struct timeval start, struct timeval end)
{
	struct timeval temp;
	if ((end.tv_usec - start.tv_usec)<0) {
		temp.tv_sec = end.tv_sec - start.tv_sec - 1;
		temp.tv_usec = 1000000 + end.tv_usec - start.tv_usec;
	}
	else {
		temp.tv_sec = end.tv_sec - start.tv_sec;
		temp.tv_usec = end.tv_usec - start.tv_usec;
	}
	return temp;
}

cl_mem check_clCreateBuffer(cl_context ctx, cl_mem_flags flags, size_t size,
	void *host_ptr)
{
	cl_int	status;
	cl_mem	ret;
	ret = clCreateBuffer(ctx, flags, size, host_ptr, &status);
	if (status != CL_SUCCESS || !ret)
		fatal("clCreateBuffer (%d)\n", status);
	return ret;
}

void check_clSetKernelArg(cl_kernel k, cl_uint a_pos, cl_mem *a)
{
	cl_int	status;
	status = clSetKernelArg(k, a_pos, sizeof(*a), a);
	if (status != CL_SUCCESS)
		fatal("clSetKernelArg (%d)\n", status);
}

void check_clEnqueueNDRangeKernel(cl_command_queue queue, cl_kernel k, cl_uint
	work_dim, const size_t *global_work_offset, const size_t
	*global_work_size, const size_t *local_work_size, cl_uint
	num_events_in_wait_list, const cl_event *event_wait_list, cl_event
	*event)
{
	cl_uint	status;
	status = clEnqueueNDRangeKernel(queue, k, work_dim, global_work_offset,
		global_work_size, local_work_size, num_events_in_wait_list,
		event_wait_list, event);
	if (status != CL_SUCCESS)
		fatal("clEnqueueNDRangeKernel (%d)\n", status);
}

void check_clEnqueueReadBuffer(cl_command_queue queue, cl_mem buffer, cl_bool
	blocking_read, size_t offset, size_t size, void *ptr, cl_uint
	num_events_in_wait_list, const cl_event *event_wait_list, cl_event
	*event)
{
	cl_int	status;
	status = clEnqueueReadBuffer(queue, buffer, blocking_read, offset,
		size, ptr, num_events_in_wait_list, event_wait_list, event);
	if (status != CL_SUCCESS)
		fatal("clEnqueueReadBuffer (%d)\n", status);
}

void hexdump(uint8_t *a, uint32_t a_len)
{
	for (uint32_t i = 0; i < a_len; i++)
		fprintf(stderr, "%02x", a[i]);
}

char *s_hexdump(const void *_a, uint32_t a_len)
{
	const uint8_t	*a = _a;
	static char		buf[4096];
	uint32_t		i;
	for (i = 0; i < a_len && i + 2 < sizeof(buf); i++)
		sprintf(buf + i * 2, "%02x", a[i]);
	buf[i * 2] = 0;
	return buf;
}

uint8_t hex2val(const char *base, size_t off)
{
	const char          c = base[off];
	if (c >= '0' && c <= '9')           return c - '0';
	else if (c >= 'a' && c <= 'f')      return 10 + c - 'a';
	else if (c >= 'A' && c <= 'F')      return 10 + c - 'A';
	fatal("Invalid hex char at offset %d: ...%d...\n", off, c);
	return 0;
}

unsigned nr_compute_units(const char *gpu)
{
	if (!strcmp(gpu, "rx480")) return 36;
	fprintf(stderr, "Unknown GPU: %s\n", gpu);
	return 0;
}

void load_file(const char *fname, char **dat, size_t *dat_len, int ignore_error)
{
	struct stat	st;
	int		fd;
	ssize_t	ret;
	if (-1 == (fd = open(fname, O_RDONLY | O_BINARY))) {
		if (ignore_error)
			return;
		fatal("%s: %s\n", fname, strerror(errno));
	}
	if (fstat(fd, &st))
		fatal("fstat: %s: %s\n", fname, strerror(errno));
	*dat_len = st.st_size;
	if (!(*dat = (char *)malloc(*dat_len + 1)))
		fatal("malloc: %s\n", strerror(errno));
	ret = read(fd, *dat, *dat_len);
	if (ret < 0)
		fatal("read: %s: %s\n", fname, strerror(errno));
	if ((size_t)ret != *dat_len)
		fatal("%s: partial read\n", fname);
	if (close(fd))
		fatal("close: %s: %s\n", fname, strerror(errno));
	(*dat)[*dat_len] = 0;
}

void get_program_build_log(cl_program program, cl_device_id device)
{
	cl_int		status;
	char	        val[2 * 1024 * 1024];
	size_t		ret = 0;
	status = clGetProgramBuildInfo(program, device,
		CL_PROGRAM_BUILD_LOG,
		sizeof(val),	// size_t param_value_size
		&val,		// void *param_value
		&ret);		// size_t *param_value_size_ret
	if (status != CL_SUCCESS)
		fatal("clGetProgramBuildInfo (%d)\n", status);
	fprintf(stderr, "%s\n", val);
}

void dump(const char *fname, void *data, size_t len)
{
	int			fd;
	ssize_t		ret;
	if (-1 == (fd = open(fname, O_BINARY | O_WRONLY | O_CREAT | O_TRUNC, 0666)))
		fatal("%s: %s\n", fname, strerror(errno));
	ret = write(fd, data, len);
	if (ret == -1)
		fatal("write: %s: %s\n", fname, strerror(errno));
	if ((size_t)ret != len)
		fatal("%s: partial write\n", fname);
	if (-1 == close(fd))
		fatal("close: %s: %s\n", fname, strerror(errno));
}

void get_program_bins(cl_program program)
{
	cl_int		status;
	size_t		sizes;
	unsigned char	*p;
	size_t		ret = 0;
	status = clGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES,
		sizeof(sizes),	// size_t param_value_size
		&sizes,		// void *param_value
		&ret);		// size_t *param_value_size_ret
	if (status != CL_SUCCESS)
		fatal("clGetProgramInfo(sizes) (%d)\n", status);
	if (ret != sizeof(sizes))
		fatal("clGetProgramInfo(sizes) did not fill sizes (%d)\n", status);
	debug("Program binary size is %zd bytes\n", sizes);
	p = (unsigned char *)malloc(sizes);
	status = clGetProgramInfo(program, CL_PROGRAM_BINARIES,
		sizeof(p),	// size_t param_value_size
		&p,		// void *param_value
		&ret);	// size_t *param_value_size_ret
	if (status != CL_SUCCESS)
		fatal("clGetProgramInfo (%d)\n", status);
	dump("dump.co", p, sizes);
	debug("program: %02x%02x%02x%02x...\n", p[0], p[1], p[2], p[3]);
}

void print_platform_info(cl_platform_id plat)
{
	char	name[1024];
	size_t	len = 0;
	int		status;
	status = clGetPlatformInfo(plat, CL_PLATFORM_NAME, sizeof(name), &name,
		&len);
	if (status != CL_SUCCESS)
		fatal("clGetPlatformInfo (%d)\n", status);
	printf("Devices on platform \"%s\":\n", name);
	fflush(stdout);
}

int is_platform_amd(cl_platform_id plat)
{
	char	name[1024];
	size_t	len = 0;
	int		status;
	status = clGetPlatformInfo(plat, CL_PLATFORM_NAME, sizeof(name), &name,
		&len);
	if (status != CL_SUCCESS)
		fatal("clGetPlatformInfo (%d)\n", status);
	return strncmp(name, "AMD Accelerated Parallel Processing", len) == 0;
}

void print_device_info(unsigned i, cl_device_id d)
{
	char	name[1024];
	size_t	len = 0;
	int		status;
	status = clGetDeviceInfo(d, CL_DEVICE_NAME, sizeof(name), &name, &len);
	if (status != CL_SUCCESS)
		fatal("clGetDeviceInfo (%d)\n", status);
	printf("  ID %d: %s\n", i, name);
	fflush(stdout);
}

#ifdef ENABLE_DEBUG
uint32_t has_i(uint32_t round, uint8_t *ht, uint32_t row, uint32_t i,
	uint32_t mask, uint32_t *res)
{
	uint32_t	slot;
	uint8_t	*p = (uint8_t *)(ht + row * NR_SLOTS * SLOT_LEN);
	uint32_t	cnt = *(uint32_t *)p;
	cnt = MIN(cnt, NR_SLOTS);
	for (slot = 0; slot < cnt; slot++, p += SLOT_LEN)
	{
		if ((*(uint32_t *)(p + xi_offset_for_round(round) - 4) & mask) ==
			(i & mask))
		{
			if (res)
				*res = slot;
			return 1;
		}
	}
	return 0;
}

uint32_t has_xi(uint32_t round, uint8_t *ht, uint32_t row, uint32_t xi,
	uint32_t *res)
{
	uint32_t	slot;
	uint8_t	*p = (uint8_t *)(ht + row * NR_SLOTS * SLOT_LEN);
	uint32_t	cnt = *(uint32_t *)p;
	cnt = MIN(cnt, NR_SLOTS);
	for (slot = 0; slot < cnt; slot++, p += SLOT_LEN)
	{
		if ((*(uint32_t *)(p + xi_offset_for_round(round))) == (xi))
		{
			if (res)
				*res = slot;
			return 1;
		}
	}
	return 0;
}

void examine_ht(unsigned round, cl_command_queue queue, cl_mem *hash_table_buffers, cl_mem row_counters_buffer)
{
	uint32_t     *hash_tables[PARAM_K];
	uint32_t     *row_counters;

	if (verbose < 3)
		return;
	if (NR_ROWS_LOG >= 15) {
		for (uint i = 0; i < PARAM_K; ++i) {
			hash_tables[i] = (uint8_t *)malloc(HT_SIZE);
			if (!hash_tables[i])
				fatal("malloc: %s\n", strerror(errno));
			check_clEnqueueReadBuffer(queue,
									  hash_table_buffers[i],
									  CL_TRUE,        // cl_bool	blocking_read
									  0,		      // size_t	offset
									  HT_SIZE,        // size_t	size
									  hash_tables[i], // void		*ptr
									  0,		// cl_uint	num_events_in_wait_list
									  NULL,	// cl_event	*event_wait_list
									  NULL);	// cl_event	*event
		}
	}
	row_counters = (uint32_t *)malloc(RC_SIZE);
	if (!row_counters)
		fatal("malloc: %s\n", strerror(errno));
	check_clEnqueueReadBuffer(queue,
							  row_counters_buffer,
							  CL_TRUE,        // cl_bool	blocking_read
							  0,		      // size_t	offset
							  RC_SIZE,        // size_t	size
							  row_counters, // void		*ptr
							  0,		// cl_uint	num_events_in_wait_list
							  NULL,	// cl_event	*event_wait_list
							  NULL);	// cl_event	*event
	uint slot_count = 0;
	uint overflow_count = 0;
	uint empty_count = 0;
	for (unsigned row = 0; row < NR_ROWS; row++) {
		uint32_t  rowIdx, rowOffset;
		rowIdx = row / ROWS_PER_UINT;
		rowOffset = BITS_PER_ROW * (row % ROWS_PER_UINT);
		uint32_t cnt = (row_counters[rowIdx] >> rowOffset) & ROW_MASK;
		cnt = min(cnt, (uint32_t)NR_SLOTS);
		if (cnt >= NR_SLOTS)
			++overflow_count;
		if (cnt == 0)
			++empty_count;
		slot_count += cnt;

		if (NR_ROWS_LOG >= 15) {
			for (uint32_t slot_index = 0; slot_index < NR_SLOTS; ++slot_index) {
				uint32_t *slot = hash_tables[round] + (row * NR_SLOTS + slot_index) * (ADJUSTED_SLOT_LEN(round) / 4);
				uint32_t i = slot[0];
				if (round) {
					uint32_t prev_row = DECODE_ROW(i);
					uint32_t prev_slot0_index = DECODE_SLOT0(i);
					uint32_t prev_slot1_index = DECODE_SLOT1(i);
					if (prev_row >= NR_ROWS || prev_slot0_index >= NR_SLOTS || prev_slot1_index >= NR_SLOTS) {
						printf("Invalid reference (i: 0x%08x).\n", i);
					}
					else {
						uint32_t *prev_slot0 = hash_tables[round - 1] + (prev_row * NR_SLOTS + prev_slot0_index) * (ADJUSTED_SLOT_LEN(round - 1) / 4);
						uint32_t *prev_slot1 = hash_tables[round - 1] + (prev_row * NR_SLOTS + prev_slot1_index) * (ADJUSTED_SLOT_LEN(round - 1) / 4);
						if (0 && (prev_slot0[1] & 0xff) != (prev_slot1[1] & 0xff))
							printf("Invalid reference (row: %u, i: 0x%08x, slot0: 0x%08x 0x%08x, slot1: 0x%08x 0x%08x).\n",
							(unsigned int)row,
								(unsigned int)i,
								prev_slot0[0],
								prev_slot0[1],
								prev_slot1[0],
								prev_slot1[1]);
					}
				}
			}
		}
	}
	printf("%d slots were generated in %d rows (capacity: %d, average: %.1f, full: %.1f%%, empty: %.1f%%).\n",
		(int)slot_count,
		   (int)NR_ROWS,
		   (int)NR_SLOTS,
		   (float)slot_count / NR_ROWS,
		   (float)overflow_count / NR_ROWS * 100,
		   (float)empty_count / NR_ROWS * 100);
	if (NR_ROWS_LOG >= 15) {
		for (uint i = 0; i < PARAM_K; ++i)
			free(hash_tables[i]);
	}
	free(row_counters);
}
#else
void examine_ht(unsigned round, cl_command_queue queue, cl_mem *hash_table_buffers, cl_mem row_counters_buffer)
{
}
#endif

void examine_dbg(cl_command_queue queue, cl_mem buf_dbg, size_t dbg_size)
{
	debug_t     *dbg;
	size_t      dropped_coll_total, dropped_stor_total;
	if (verbose < 2)
		return;
	dbg = (debug_t *)malloc(dbg_size);
	if (!dbg)
		fatal("malloc: %s\n", strerror(errno));
	check_clEnqueueReadBuffer(queue, buf_dbg,
		CL_TRUE,	// cl_bool	blocking_read
		0,		// size_t	offset
		dbg_size,   // size_t	size
		dbg,	// void		*ptr
		0,		// cl_uint	num_events_in_wait_list
		NULL,	// cl_event	*event_wait_list
		NULL);	// cl_event	*event
	dropped_coll_total = dropped_stor_total = 0;
	for (unsigned tid = 0; tid < dbg_size / sizeof(*dbg); tid++)
	{
		dropped_coll_total += dbg[tid].dropped_coll;
		dropped_stor_total += dbg[tid].dropped_stor;
		if (0 && (dbg[tid].dropped_coll || dbg[tid].dropped_stor))
			debug("thread %6d: dropped_coll %zd dropped_stor %zd\n", tid,
				dbg[tid].dropped_coll, dbg[tid].dropped_stor);
	}
	debug("Dropped: %zd (coll) %zd (stor)\n",
		dropped_coll_total, dropped_stor_total);
	free(dbg);
}

size_t select_work_size_blake(void)
{
	size_t              work_size =
		64 * /* thread per wavefront */
		BLAKE_WPS * /* wavefront per simd */
		4 * /* simd per compute unit */
		nr_compute_units("rx480");
	// Make the work group size a multiple of the nr of wavefronts, while
	// dividing the number of inputs. This results in the worksize being a
	// power of 2.
	while (NR_INPUTS % work_size)
		work_size += 64;
	//debug("Blake: work size %zd\n", work_size);
	return work_size;
}

void init_ht(cl_command_queue queue, cl_kernel k_init_ht, cl_mem buf_ht,
	cl_mem rowCounters)
{
	size_t      global_ws = RC_SIZE / sizeof(cl_uint);
	size_t      local_ws = 256;
	cl_int      status;
#if 0
	uint32_t    pat = -1;
	status = clEnqueueFillBuffer(queue, buf_ht, &pat, sizeof(pat), 0,
		NR_ROWS * NR_SLOTS * SLOT_LEN,
		0,		// cl_uint	num_events_in_wait_list
		NULL,	// cl_event	*event_wait_list
		NULL);	// cl_event	*event
	if (status != CL_SUCCESS)
		fatal("clEnqueueFillBuffer (%d)\n", status);
#endif
	status = clSetKernelArg(k_init_ht, 0, sizeof(buf_ht), &buf_ht);
	clSetKernelArg(k_init_ht, 1, sizeof(rowCounters), &rowCounters);
	if (status != CL_SUCCESS)
		fatal("clSetKernelArg (%d)\n", status);
	check_clEnqueueNDRangeKernel(queue, k_init_ht,
		1,		// cl_uint	work_dim
		NULL,	// size_t	*global_work_offset
		&global_ws,	// size_t	*global_work_size
		&local_ws,	// size_t	*local_work_size
		0,		// cl_uint	num_events_in_wait_list
		NULL,	// cl_event	*event_wait_list
		NULL);	// cl_event	*event
}

/*
** Write ZCASH_SOL_LEN bytes representing the encoded solution as per the
** Zcash protocol specs (512 x 21-bit inputs).
**
** out		ZCASH_SOL_LEN-byte buffer where the solution will be stored
** inputs	array of 32-bit inputs
** n		number of elements in array
*/
void store_encoded_sol(uint8_t *out, uint32_t *inputs, uint32_t n)
{
	uint32_t byte_pos = 0;
	int32_t bits_left = PREFIX + 1;
	uint8_t x = 0;
	uint8_t x_bits_used = 0;
	while (byte_pos < n)
	{
		if (bits_left >= 8 - x_bits_used)
		{
			x |= inputs[byte_pos] >> (bits_left - 8 + x_bits_used);
			bits_left -= 8 - x_bits_used;
			x_bits_used = 8;
		}
		else if (bits_left > 0)
		{
			uint32_t mask = ~(-1 << (8 - x_bits_used));
			mask = ((~mask) >> bits_left) & mask;
			x |= (inputs[byte_pos] << (8 - x_bits_used - bits_left)) & mask;
			x_bits_used += bits_left;
			bits_left = 0;
		}
		else if (bits_left <= 0)
		{
			assert(!bits_left);
			byte_pos++;
			bits_left = PREFIX + 1;
		}
		if (x_bits_used == 8)
		{
			*out++ = x;
			x = x_bits_used = 0;
		}
	}
}

/*
** Print on stdout a hex representation of the encoded solution as per the
** zcash protocol specs (512 x 21-bit inputs).
**
** inputs	array of 32-bit inputs
** n		number of elements in array
*/
void print_encoded_sol(uint32_t *inputs, uint32_t n)
{
	uint8_t	sol[ZCASH_SOL_LEN];
	uint32_t	i;
	store_encoded_sol(sol, inputs, n);
	for (i = 0; i < sizeof(sol); i++)
		printf("%02x", sol[i]);
	printf("\n");
	fflush(stdout);
}

void print_sol(uint32_t *values, uint64_t *nonce)
{
	uint32_t	show_n_sols;
	show_n_sols = (1 << PARAM_K);
	if (verbose < 2)
		show_n_sols = MIN(10, show_n_sols);
	fprintf(stderr, "Soln:");
	// for brievity, only print "small" nonces
	if (*nonce < (1ULL << 32))
		fprintf(stderr, " 0x%" PRIx64 ":", *nonce);
	for (unsigned i = 0; i < show_n_sols; i++)
		fprintf(stderr, " %x", values[i]);
	fprintf(stderr, "%s\n", (show_n_sols != (1 << PARAM_K) ? "..." : ""));
}

/*
** Compare two 256-bit values interpreted as little-endian 256-bit integers.
*/
int32_t cmp_target_256(void *_a, void *_b)
{
	uint8_t	*a = _a;
	uint8_t	*b = _b;
	int32_t	i;
	for (i = SHA256_TARGET_LEN - 1; i >= 0; i--)
		if (a[i] != b[i])
			return (int32_t)a[i] - b[i];
	return 0;
}

/*
** Verify if the solution's block hash is under the target, and if yes print
** it formatted as:
** "sol: <job_id> <ntime> <nonce_rightpart> <solSize+sol>"
**
** Return 1 iff the block hash is under the target.
*/
uint32_t print_solver_line(uint32_t *values, uint8_t *header,
	size_t fixed_nonce_bytes, uint8_t *target, char *job_id)
{
	uint8_t	buffer[ZCASH_BLOCK_HEADER_LEN + ZCASH_SOLSIZE_LEN +
		ZCASH_SOL_LEN];
	uint8_t	hash0[SHA256_DIGEST_SIZE];
	uint8_t	hash1[SHA256_DIGEST_SIZE];
	uint8_t	*p;
	p = buffer;
	memcpy(p, header, ZCASH_BLOCK_HEADER_LEN);
	p += ZCASH_BLOCK_HEADER_LEN;
	memcpy(p, "\xfd\x40\x05", ZCASH_SOLSIZE_LEN);
	p += ZCASH_SOLSIZE_LEN;
	store_encoded_sol(p, values, 1 << PARAM_K);
	Sha256_Onestep(buffer, sizeof(buffer), hash0);
	Sha256_Onestep(hash0, sizeof(hash0), hash1);
	// compare the double SHA256 hash with the target
	if (cmp_target_256(target, hash1) < 0)
	{
		debug("Hash is above target\n");
		return 0;
	}
	debug("Hash is under target\n");
	printf("sol: %s ", job_id);
	p = header + ZCASH_BLOCK_OFFSET_NTIME;
	printf("%02x%02x%02x%02x ", p[0], p[1], p[2], p[3]);
	printf("%s ", s_hexdump(header + ZCASH_BLOCK_HEADER_LEN - ZCASH_NONCE_LEN +
		fixed_nonce_bytes, ZCASH_NONCE_LEN - fixed_nonce_bytes));
	printf("%s%s\n", ZCASH_SOLSIZE_HEX,
		s_hexdump(buffer + ZCASH_BLOCK_HEADER_LEN + ZCASH_SOLSIZE_LEN,
			ZCASH_SOL_LEN));
	fflush(stdout);
	return 1;
}

int sol_cmp(const void *_a, const void *_b)
{
	const uint32_t	*a = _a;
	const uint32_t	*b = _b;
	for (uint32_t i = 0; i < (1 << PARAM_K); i++)
	{
		if (*a != *b)
			return *a - *b;
		a++;
		b++;
	}
	return 0;
}

/*
** Print all solutions.
**
** In mining mode, return the number of shares, that is the number of solutions
** that were under the target.
*/
uint32_t print_sols(sols_t *all_sols, uint64_t *nonce, uint32_t nr_valid_sols,
	uint8_t *header, size_t fixed_nonce_bytes, uint8_t *target,
	char *job_id)
{
	uint8_t		*valid_sols;
	uint32_t		counted;
	uint32_t		shares = 0;
	valid_sols = malloc(nr_valid_sols * SOL_SIZE);
	if (!valid_sols)
		fatal("malloc: %s\n", strerror(errno));
	counted = 0;
	for (uint32_t i = 0; i < all_sols->nr; i++)
		if (all_sols->valid[i])
		{
			if (counted >= nr_valid_sols)
				fatal("Bug: more than %d solutions\n", nr_valid_sols);
			memcpy(valid_sols + counted * SOL_SIZE, all_sols->values[i],
				SOL_SIZE);
			counted++;
		}
	assert(counted == nr_valid_sols);
	// sort the solutions amongst each other, to make the solver's output
	// deterministic and testable
	qsort(valid_sols, nr_valid_sols, SOL_SIZE, sol_cmp);
	for (uint32_t i = 0; i < nr_valid_sols; i++)
	{
		uint32_t	*inputs = (uint32_t *)(valid_sols + i * SOL_SIZE);
		if (!mining && show_encoded)
			print_encoded_sol(inputs, 1 << PARAM_K);
		if (verbose)
			print_sol(inputs, nonce);
		if (mining)
			shares += print_solver_line(inputs, header, fixed_nonce_bytes,
				target, job_id);
	}
	free(valid_sols);
	return shares;
}

/*
** Sort a pair of binary blobs (a, b) which are consecutive in memory and
** occupy a total of 2*len 32-bit words.
**
** a            points to the pair
** len          number of 32-bit words in each pair
*/
void sort_pair(uint32_t *a, uint32_t len)
{
	uint32_t    *b = a + len;
	uint32_t     tmp, need_sorting = 0;
	for (uint32_t i = 0; i < len; i++)
		if (need_sorting || a[i] > b[i])
		{
			need_sorting = 1;
			tmp = a[i];
			a[i] = b[i];
			b[i] = tmp;
		}
		else if (a[i] < b[i])
			return;
}

/*
** If solution is invalid return 0. If solution is valid, sort the inputs
** and return 1.
*/

#define SEEN_LEN (1 << (PREFIX + 1)) / 8

uint32_t verify_sol(sols_t *sols, unsigned sol_i)
{
	uint32_t	*inputs = sols->values[sol_i];
	//uint32_t	seen_len = (1 << (PREFIX + 1)) / 8; 
	//uint8_t	seen[seen_len]; // @mrb MSVC didn't like this.
	uint8_t	seen[SEEN_LEN];
	uint32_t	i;
	uint8_t	tmp;
	// look for duplicate inputs
	memset(seen, 0, SEEN_LEN);
	for (i = 0; i < (1 << PARAM_K); i++)
	{
		if (inputs[i] / 8 >= SEEN_LEN)
		{
			warn("Invalid input retrieved from device: %d\n", inputs[i]);
			sols->valid[sol_i] = 0;
			return 0;
		}
		tmp = seen[inputs[i] / 8];
		seen[inputs[i] / 8] |= 1 << (inputs[i] & 7);
		if (tmp == seen[inputs[i] / 8])
		{
			// at least one input value is a duplicate
			sols->valid[sol_i] = 0;
			return 0;
		}
	}
	// the valid flag is already set by the GPU, but set it again because
	// I plan to change the GPU code to not set it
	sols->valid[sol_i] = 1;
	// sort the pairs in place
	for (uint32_t level = 0; level < PARAM_K; level++)
		for (i = 0; i < (1 << PARAM_K); i += (2 << level))
			sort_pair(&inputs[i], 1 << level);
	return 1;
}

/*
** Return the number of valid solutions.
*/
uint32_t verify_sols(cl_command_queue queue, cl_mem buf_sols, uint64_t *nonce,
	uint8_t *header, size_t fixed_nonce_bytes, uint8_t *target,
	char *job_id, uint32_t *shares, struct timeval *start_time)
{
	sols_t	*sols;
	uint32_t	nr_valid_sols;
	sols = (sols_t *)malloc(sizeof(*sols));
	if (!sols)
		fatal("malloc: %s\n", strerror(errno));
#ifdef WIN32
	timeBeginPeriod(1);
	DWORD duration = (DWORD)kern_avg_run_time.tv_sec * 1000 + (DWORD)kern_avg_run_time.tv_usec / 1000;
	if (!amd_flag && duration < 1000)
		Sleep(duration);
#endif
	check_clEnqueueReadBuffer(queue, buf_sols,
		CL_TRUE,	// cl_bool	blocking_read
		0,		// size_t	offset
		sizeof(*sols),	// size_t	size
		sols,	// void		*ptr
		0,		// cl_uint	num_events_in_wait_list
		NULL,	// cl_event	*event_wait_list
		NULL);	// cl_event	*event
	struct timeval curr_time;
	gettimeofday(&curr_time, NULL);

	struct timeval t_diff = time_diff(*start_time, curr_time);

	double a_diff = t_diff.tv_sec * 1e6 + t_diff.tv_usec;
	double kern_avg = kern_avg_run_time.tv_sec * 1e6 + kern_avg_run_time.tv_usec;
	if (kern_avg == 0)
		kern_avg = a_diff;
	else
		kern_avg = kern_avg * 70 / 100 + a_diff * 28 / 100; // it is 2% less than average
															// thus allowing time to reduce

	kern_avg_run_time.tv_sec = (time_t)(kern_avg / 1e6);
	kern_avg_run_time.tv_usec = ((long)kern_avg) % 1000000;

	if (sols->nr > MAX_SOLS)
	{
		fprintf(stderr, "%d (probably invalid) solutions were dropped!\n",
			sols->nr - MAX_SOLS);
		sols->nr = MAX_SOLS;
	}
	debug("Retrieved %d potential solutions\n", sols->nr);
	nr_valid_sols = 0;
	for (unsigned sol_i = 0; sol_i < sols->nr; sol_i++)
		nr_valid_sols += verify_sol(sols, sol_i);
	uint32_t sh = print_sols(sols, nonce, nr_valid_sols, header,
		fixed_nonce_bytes, target, job_id);
	if (shares)
		*shares = sh;
	if (!mining || verbose)
		fprintf(stderr, "Nonce %s: %d sol%s\n",
			s_hexdump(nonce, ZCASH_NONCE_LEN), nr_valid_sols,
			nr_valid_sols == 1 ? "" : "s");
	debug("Stats: %d likely invalids\n", sols->likely_invalids);
	free(sols);
	return nr_valid_sols;
}

unsigned get_value(unsigned *data, unsigned row)
{
	return data[row];
}

/*
** Attempt to find Equihash solutions for the given Zcash block header and
** nonce. The 'header' passed in argument is a 140-byte header specifying
** the nonce, which this function may auto-increment if 'do_increment'. This
** allows repeatedly calling this fuction to solve different Equihash problems.
**
** header	must be a buffer allocated with ZCASH_BLOCK_HEADER_LEN bytes
** header_len	number of bytes initialized in header (either 140 or 108)
** shares	if not NULL, in mining mode the number of shares (ie. number
**		of solutions that were under the target) are stored here
**
** Return the number of solutions found.
*/
uint32_t solve_equihash(cl_device_id dev_id, cl_context ctx, cl_command_queue queue,
	cl_kernel k_init_ht, cl_kernel *k_rounds, cl_kernel k_sols,
	cl_mem *buf_ht, cl_mem buf_sols, cl_mem buf_dbg, size_t dbg_size,
	uint8_t *header, size_t header_len, char do_increment,
	size_t fixed_nonce_bytes, uint8_t *target, char *job_id,
	uint32_t *shares, cl_mem *rowCounters, cl_mem buf_blake_st)
{
	blake2b_state_t     blake;
	size_t		global_ws;
	size_t              local_work_size = LOCAL_WORK_SIZE;
	uint32_t		sol_found = 0;
	uint64_t		*nonce_ptr;
	int              status;
	cl_uint          nr_compute_units;
	status = clGetDeviceInfo(dev_id, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(nr_compute_units), &nr_compute_units, NULL);
	if (status != CL_SUCCESS)
		fatal("clGetDeviceInfo (%d)\n", status);

	assert(header_len == ZCASH_BLOCK_HEADER_LEN);
	if (mining)
		assert(target && job_id);
	nonce_ptr = (uint64_t *)(header + ZCASH_BLOCK_HEADER_LEN - ZCASH_NONCE_LEN);
	if (do_increment)
	{
		// Increment the nonce
		if (mining)
		{
			// increment bytes 17-19
			(*(uint32_t *)((uint8_t *)nonce_ptr + 17))++;
			// byte 20 and above must be zero
			*(uint32_t *)((uint8_t *)nonce_ptr + 20) = 0;
		}
		else
			// increment bytes 0-7
			(*nonce_ptr)++;
	}
	debug("\nSolving nonce %s\n", s_hexdump(nonce_ptr, ZCASH_NONCE_LEN));
	// Process first BLAKE2b-400 block
	zcash_blake2b_init(&blake, ZCASH_HASH_LEN, PARAM_N, PARAM_K);
	zcash_blake2b_update(&blake, header, 128, 0);
	clEnqueueWriteBuffer(queue, buf_blake_st, CL_TRUE, 0, 64, &(blake.h), 0, NULL, NULL);
	for (unsigned round = 0; round < PARAM_K; round++)
	{
		if (verbose > 1)
			debug("Round %d\n", round);
		// Now on every round!!!!
		init_ht(queue, k_init_ht, buf_ht[round], rowCounters[round % 2]);
		if (!round)
		{
			check_clSetKernelArg(k_rounds[round], 0, &buf_blake_st);
			check_clSetKernelArg(k_rounds[round], 1, &buf_ht[round]);
			check_clSetKernelArg(k_rounds[round], 2, &rowCounters[round % 2]);
			global_ws = select_work_size_blake();
		}
		else
		{
			check_clSetKernelArg(k_rounds[round], 0, &buf_ht[round - 1]);
			check_clSetKernelArg(k_rounds[round], 1, &buf_ht[round]);
			check_clSetKernelArg(k_rounds[round], 2, &rowCounters[(round - 1) % 2]);
			check_clSetKernelArg(k_rounds[round], 3, &rowCounters[round % 2]);
			global_ws = GLOBAL_WORK_SIZE_RATIO * nr_compute_units * LOCAL_WORK_SIZE;
			if (global_ws > NR_ROWS * THREADS_PER_ROW)
				global_ws = NR_ROWS * THREADS_PER_ROW;
		}
		check_clSetKernelArg(k_rounds[round], round == 0 ? 3 : 4, &buf_dbg);
		check_clEnqueueNDRangeKernel(queue, k_rounds[round], 1, NULL,
			&global_ws, &local_work_size, 0, NULL, NULL);
		examine_ht(round, queue, buf_ht, rowCounters[round % 2]);
		examine_dbg(queue, buf_dbg, dbg_size);
	}
	check_clSetKernelArg(k_sols, 0, &buf_ht[0]);
	check_clSetKernelArg(k_sols, 1, &buf_ht[1]);
	check_clSetKernelArg(k_sols, 2, &buf_ht[2]);
	check_clSetKernelArg(k_sols, 3, &buf_ht[3]);
	check_clSetKernelArg(k_sols, 4, &buf_ht[4]);
	check_clSetKernelArg(k_sols, 5, &buf_ht[5]);
	check_clSetKernelArg(k_sols, 6, &buf_ht[6]);
	check_clSetKernelArg(k_sols, 7, &buf_ht[7]);
	check_clSetKernelArg(k_sols, 8, &buf_ht[8]);
	check_clSetKernelArg(k_sols, 9, &buf_sols);
	check_clSetKernelArg(k_sols, 10, &rowCounters[0]);
	global_ws = GLOBAL_WORK_SIZE_RATIO * nr_compute_units * LOCAL_WORK_SIZE_SOLS;
	if (global_ws > NR_ROWS * THREADS_PER_ROW_SOLS)
		global_ws = NR_ROWS * THREADS_PER_ROW_SOLS;
	local_work_size = LOCAL_WORK_SIZE_SOLS;
	struct timeval start_time;
	gettimeofday(&start_time, NULL);
	check_clEnqueueNDRangeKernel(queue, k_sols, 1, NULL,
		&global_ws, &local_work_size, 0, NULL, NULL);
	clFlush(queue);
	sol_found = verify_sols(queue, buf_sols, nonce_ptr, header,
		fixed_nonce_bytes, target, job_id, shares, &start_time);
	return sol_found;
}

/*
** Read a complete line from stdin. If 2 or more lines are available, store
** only the last one in the buffer.
**
** buf		buffer to store the line
** len		length of the buffer
** block	blocking mode: do not return until a line was read
**
** Return 1 iff a line was read.
*/
int read_last_line(char *buf, size_t len, int block)
{
	char	*start;
	size_t	pos = 0;
	ssize_t	n;
#ifndef WIN32
	set_blocking_mode(0, block);
#endif
	while (42)
	{
#ifndef WIN32
		n = read(0, buf + pos, len - pos);
		if (n == -1 && errno == EINTR)
			continue;
		else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			if (!pos)
				return 0;
			warn("strange: a partial line was read\n");
			// a partial line was read, continue reading it in blocking mode
			// to be sure to read it completely
			set_blocking_mode(0, 1);
			continue;
		}
		else if (n == -1)
			fatal("read stdin: %s\n", strerror(errno));
		else if (!n)
			fatal("EOF on stdin\n");
		pos += n;

		if (buf[pos - 1] == '\n')
			// 1 (or more) complete lines were read
			break;
#else
		DWORD bytesAvailable = 0;
		HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
		PeekNamedPipe(stdinHandle, NULL, 0, NULL, &bytesAvailable, NULL);
		if (bytesAvailable > 0) {

			if (!ReadFile(stdinHandle, buf, bytesAvailable, &bytesAvailable, NULL)) {
				fatal("ReadFile: %d", GetLastError());
			}
			pos += bytesAvailable;
		}
		else {
			return 0;
		}
		if (buf[pos - 1] == '\n')
			// 1 (or more) complete lines were read
			break;
#endif
	}
	start = memrchr(buf, '\n', pos - 1);
	if (start)
	{
		warn("strange: more than 1 line was read\n");
		// more than 1 line; copy the last line to the beginning of the buffer
		pos -= (start + 1 - buf);
		memmove(buf, start + 1, pos);
	}
	// overwrite '\n' with NUL

	buf[pos - 1] = 0;
	return 1;
}

/*
** Parse a string:
**   "<target> <job_id> <header> <nonce_leftpart>"
** (all the parts are in hex, except job_id which is a non-whitespace string),
** decode the hex values and store them in the relevant buffers.
**
** The remaining part of <header> that is not set by
** <header><nonce_leftpart> will be randomized so that the miner
** solves a unique Equihash PoW.
**
** str		string to parse
** target	buffer where the <target> will be stored
** target_len	size of target buffer
** job_id	buffer where the <job_id> will be stored
** job_id_len	size of job_id buffer
** header	buffer where the <header><nonce_leftpart> will be
** 		concatenated and stored
** header_len	size of the header_buffer
** fixed_nonce_bytes
** 		nr of bytes represented by <nonce_leftpart> will be stored here;
** 		this is the number of nonce bytes fixed by the stratum server
*/
void mining_parse_job(char *str, uint8_t *target, size_t target_len,
	char *job_id, size_t job_id_len, uint8_t *header, size_t header_len,
	size_t *fixed_nonce_bytes)
{
	uint32_t		str_i, i;
	// parse target
	str_i = 0;
	for (i = 0; i < target_len; i++, str_i += 2)
		target[i] = hex2val(str, str_i) * 16 + hex2val(str, str_i + 1);
	assert(str[str_i] == ' ');
	str_i++;
	// parse job_id
	for (i = 0; i < job_id_len && str[str_i] != ' '; i++, str_i++)
		job_id[i] = str[str_i];
	assert(str[str_i] == ' ');
	assert(i < job_id_len);
	job_id[i] = 0;
	str_i++;
	// parse header and nonce_leftpart
	for (i = 0; i < header_len && str[str_i] != ' '; i++, str_i += 2)
		header[i] = hex2val(str, str_i) * 16 + hex2val(str, str_i + 1);
	assert(str[str_i] == ' ');
	str_i++;
	*fixed_nonce_bytes = 0;
	while (i < header_len && str[str_i] && str[str_i] != '\n')
	{
		header[i] = hex2val(str, str_i) * 16 + hex2val(str, str_i + 1);
		i++;
		str_i += 2;
		(*fixed_nonce_bytes)++;
	}
	assert(!str[str_i]);
	// Randomize rest of the bytes except N_ZERO_BYTES bytes which must be zero
	debug("Randomizing %d bytes in nonce\n", header_len - N_ZERO_BYTES - i);
	randomize(header + i, header_len - N_ZERO_BYTES - i);
	memset(header + header_len - N_ZERO_BYTES, 0, N_ZERO_BYTES);
}

/*
** Run in mining mode.
*/
#ifdef WIN32

#ifndef DEFAULT_NUM_MINING_MODE_THREADS
#define DEFAULT_NUM_MINING_MODE_THREADS 1
#define MAX_NUM_MINING_MODE_THREADS 16
#endif
uint32_t num_mining_mode_threads = DEFAULT_NUM_MINING_MODE_THREADS;
CRITICAL_SECTION cs;

struct mining_mode_thread_args {
	cl_device_id dev_id;
	cl_context ctx;
	cl_command_queue queue;
	size_t dbg_size;
	//
	uint8_t     header[ZCASH_BLOCK_HEADER_LEN];
	uint8_t		target[SHA256_DIGEST_SIZE];
	char		job_id[256];
	size_t		fixed_nonce_bytes;
	uint64_t		*total;
	uint64_t		*total_shares;
};

#define ARGS ((struct mining_mode_thread_args *)args)
DWORD mining_mode_thread(LPVOID *args)
{
	uint8_t     header[ZCASH_BLOCK_HEADER_LEN];
	uint8_t		target[SHA256_DIGEST_SIZE];
	char		job_id[256] = { '\0' };
	size_t		fixed_nonce_bytes;
	cl_int      status;
	cl_program program;
	cl_kernel k_init_ht, k_rounds[PARAM_K], k_sols;
	cl_mem              buf_ht[9], buf_sols, buf_dbg, rowCounters[2], buf_blake_st;
	void                *dbg = NULL;

	if (binary) {
		program = clCreateProgramWithBinary(ARGS->ctx, 1, &(ARGS->dev_id),
			&binary_len, (const char **)&binary, NULL, &status);
		if (status != CL_SUCCESS || !program)
			fatal("clCreateProgramWithBinary (%d)\n", status);
	}
	else {
		program = clCreateProgramWithSource(ARGS->ctx, 1, (const char **)&source,
			&source_len, &status);
		if (status != CL_SUCCESS || !program)
			fatal("clCreateProgramWithSource (%d)\n", status);
	}

	/* Build program. */
	if (!mining || verbose)
		fprintf(stderr, "Building program\n");
	status = clBuildProgram(program, 1, &(ARGS->dev_id),
		(binary) ? "" : (amd_flag) ? (OPENCL_BUILD_OPTIONS_AMD) : (OPENCL_BUILD_OPTIONS), // compile options
		NULL, NULL);
	if (status != CL_SUCCESS)
	{
		warn("OpenCL build failed (%d). Build log follows:\n", status);
		get_program_build_log(program, ARGS->dev_id);
		exit(1);
	}

	// Create kernel objects
	k_init_ht = clCreateKernel(program, "kernel_init_ht", &status);
	if (status != CL_SUCCESS || !k_init_ht)
		fatal("clCreateKernel (%d)\n", status);
	for (unsigned round = 0; round < PARAM_K; round++)
	{
		char	name[128];
		snprintf(name, sizeof(name), "kernel_round%d", round);
		k_rounds[round] = clCreateKernel(program, name, &status);
		if (status != CL_SUCCESS || !k_rounds[round])
			fatal("clCreateKernel (%d)\n", status);
	}
	k_sols = clCreateKernel(program, "kernel_sols", &status);
	if (status != CL_SUCCESS || !k_sols)
		fatal("clCreateKernel (%d)\n", status);

	// Set up buffers for the host and memory objects for the kernel
	if (!(dbg = calloc(ARGS->dbg_size, 1)))
		fatal("malloc: %s\n", strerror(errno));
	buf_dbg = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE |
		CL_MEM_COPY_HOST_PTR, ARGS->dbg_size, dbg);
	buf_ht[0] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[1] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[2] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[3] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[4] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[5] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[6] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[7] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_ht[8] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
	buf_sols = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, sizeof(sols_t), NULL);
	rowCounters[0] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, RC_SIZE, NULL);
	rowCounters[1] = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_WRITE, RC_SIZE, NULL);
	buf_blake_st = check_clCreateBuffer(ARGS->ctx, CL_MEM_READ_ONLY, 64, NULL);

	while (1) {
		EnterCriticalSection(&cs);
		if (memcmp(ARGS->job_id, job_id, sizeof(job_id))) {
			memcpy(target, ARGS->target, sizeof(target));
			memcpy(job_id, ARGS->job_id, sizeof(job_id));
			memcpy(header, ARGS->header, ZCASH_BLOCK_HEADER_LEN);
			fixed_nonce_bytes = ARGS->fixed_nonce_bytes;
		}
		LeaveCriticalSection(&cs);

		uint32_t shares;
		uint32_t num_sols = solve_equihash(ARGS->dev_id, ARGS->ctx, ARGS->queue, k_init_ht, k_rounds, k_sols, buf_ht,
			buf_sols, buf_dbg, ARGS->dbg_size, header, ZCASH_BLOCK_HEADER_LEN, 1,
			fixed_nonce_bytes, target, job_id, &shares, rowCounters, buf_blake_st);

		EnterCriticalSection(&cs);
		*(ARGS->total) += num_sols;
		*(ARGS->total_shares) += shares;
		LeaveCriticalSection(&cs);
	}
}

void mining_mode(cl_device_id dev_id, cl_program program, cl_context ctx, cl_command_queue queue,
	cl_kernel k_init_ht, cl_kernel *k_rounds, cl_kernel k_sols,
	cl_mem *buf_ht, cl_mem buf_sols, cl_mem buf_dbg, size_t dbg_size,
	uint8_t *header, cl_mem *rowCounters, cl_mem buf_blake_st)
{
	char		line[4096];
	uint8_t		target[SHA256_DIGEST_SIZE];
	char		job_id[256];
	size_t		fixed_nonce_bytes = 0;
	uint64_t		i;
	uint64_t		total = 0;
	uint32_t		shares;
	uint64_t		total_shares = 0;
	uint64_t		t0 = 0, t1;
	uint64_t		status_period = 500e3; // time (usec) between statuses
	cl_int          status;
	struct mining_mode_thread_args args[MAX_NUM_MINING_MODE_THREADS];

	InitializeCriticalSection(&cs);

	puts("Gateless Gate mining mode ready");
	fflush(stdout);
	SetConsoleOutputCP(65001);
	for (i = 0; ; i++)
	{
		// iteration #0 always reads a job or else there is nothing to do

		if (read_last_line(line, sizeof(line), !i)) {
			EnterCriticalSection(&cs);
			for (int thread_index = 0; thread_index < num_mining_mode_threads; ++thread_index) {
				mining_parse_job(line,
					target, sizeof(target),
					job_id, sizeof(job_id),
					header, ZCASH_BLOCK_HEADER_LEN,
					&fixed_nonce_bytes);
				if (num_mining_mode_threads > 1) {
					memcpy(args[thread_index].target, target, sizeof(target));
					memcpy(args[thread_index].job_id, job_id, sizeof(job_id));
					memcpy(args[thread_index].header, header, ZCASH_BLOCK_HEADER_LEN);
					args[thread_index].fixed_nonce_bytes = fixed_nonce_bytes;
					if (!i) {
						args[thread_index].dev_id = dev_id;
						args[thread_index].ctx = ctx;
						args[thread_index].queue = queue;
						args[thread_index].dbg_size = dbg_size;
						args[thread_index].total = &total;
						args[thread_index].total_shares = &total_shares;
						CreateThread(
							NULL,                   // default security attributes
							0,                      // use default stack size  
							mining_mode_thread,     // thread function name
							&args[thread_index],    // argument to thread function 
							0,                      // use default creation flags 
							NULL);                  // returns the thread identifier 
					}
				}
			}
			LeaveCriticalSection(&cs);
		}

		if (num_mining_mode_threads <= 1) {
			uint32_t shares;
			uint32_t num_sols = solve_equihash(dev_id, ctx, queue, k_init_ht, k_rounds, k_sols, buf_ht,
				buf_sols, buf_dbg, dbg_size, header, ZCASH_BLOCK_HEADER_LEN, 1,
				fixed_nonce_bytes, target, job_id, &shares, rowCounters, buf_blake_st);
			total += num_sols;
			total_shares += shares;
		}
		else {
			Sleep(status_period / 1000);
		}
		if ((t1 = now()) > t0 + status_period)
		{
			EnterCriticalSection(&cs);
			t0 = t1;
			printf("status: %" PRId64 " %" PRId64 "\n", total, total_shares);
			fflush(stdout);
			LeaveCriticalSection(&cs);
		}
	}
}
#else
void mining_mode(cl_device_id dev_id, cl_program program, cl_context ctx, cl_command_queue queue,
	cl_kernel k_init_ht, cl_kernel *k_rounds, cl_kernel k_sols,
	cl_mem *buf_ht, cl_mem buf_sols, cl_mem buf_dbg, size_t dbg_size,
	uint8_t *header, cl_mem *rowCounters, cl_mem buf_blake_st)
{
	char		line[4096];
	uint8_t		target[SHA256_DIGEST_SIZE];
	char		job_id[256];
	size_t		fixed_nonce_bytes = 0;
	uint64_t		i;
	uint64_t		total = 0;
	uint32_t		shares;
	uint64_t		total_shares = 0;
	uint64_t		t0 = 0, t1;
	uint64_t		status_period = 500e3; // time (usec) between statuses
	cl_int          status;

	puts("Gateless Gate mining mode ready");
	fflush(stdout);
#ifdef WIN32
	SetConsoleOutputCP(65001);
#endif
	for (i = 0; ; i++)
	{
		// iteration #0 always reads a job or else there is nothing to do

		if (read_last_line(line, sizeof(line), !i)) {
			mining_parse_job(line,
				target, sizeof(target),
				job_id, sizeof(job_id),
				header, ZCASH_BLOCK_HEADER_LEN,
				&fixed_nonce_bytes);
		}
		total += solve_equihash(dev_id, ctx, queue, k_init_ht, k_rounds, k_sols, buf_ht,
			buf_sols, buf_dbg, dbg_size, header, ZCASH_BLOCK_HEADER_LEN, 1,
			fixed_nonce_bytes, target, job_id, &shares, rowCounters, buf_blake_st);
		total_shares += shares;
		if ((t1 = now()) > t0 + status_period)
		{
			t0 = t1;
			printf("status: %" PRId64 " %" PRId64 "\n", total, total_shares);
			fflush(stdout);
		}
	}
}
#endif

void run_opencl(uint8_t *header, size_t header_len, cl_device_id *dev_id, cl_context ctx,
	cl_command_queue queue, cl_program program, cl_kernel k_init_ht, cl_kernel *k_rounds,
	cl_kernel k_sols)
{
	cl_mem              buf_ht[9], buf_sols, buf_dbg, rowCounters[2], buf_blake_st;
	void                *dbg = NULL;
#ifdef ENABLE_DEBUG
	size_t              dbg_size = NR_ROWS * THREADS_PER_ROW * sizeof(debug_t);
#else
	size_t              dbg_size = 1 * sizeof(debug_t);
#endif
	uint64_t		nonce;
	uint64_t		total;
	if (!mining || verbose)
		fprintf(stderr, "Hash tables will use %.1f MB\n", 9.0 * HT_SIZE / 1e6);
#ifdef WIN32
	if (!mining || num_mining_mode_threads <= 1) {
#endif
		// Set up buffers for the host and memory objects for the kernel
		if (!(dbg = calloc(dbg_size, 1)))
			fatal("malloc: %s\n", strerror(errno));
		buf_dbg = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE |
			CL_MEM_COPY_HOST_PTR, dbg_size, dbg);
		buf_ht[0] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[1] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[2] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[3] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[4] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[5] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[6] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[7] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_ht[8] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, HT_SIZE, NULL);
		buf_sols = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(sols_t), NULL);
		rowCounters[0] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, RC_SIZE, NULL);
		rowCounters[1] = check_clCreateBuffer(ctx, CL_MEM_READ_WRITE, RC_SIZE, NULL);
		buf_blake_st = check_clCreateBuffer(ctx, CL_MEM_READ_ONLY, 64, NULL);
#ifdef WIN32
	}
#endif
	if (mining)
		mining_mode(*dev_id, program, ctx, queue, k_init_ht, k_rounds, k_sols, buf_ht,
			buf_sols, buf_dbg, dbg_size, header, rowCounters, buf_blake_st);
	fprintf(stderr, "Running...\n");
	total = 0;
	uint64_t t0 = now();
	// Solve Equihash for a few nonces
	for (nonce = 0; nonce < nr_nonces; nonce++)
		total += solve_equihash(*dev_id, ctx, queue, k_init_ht, k_rounds, k_sols, buf_ht,
			buf_sols, buf_dbg, dbg_size, header, header_len, !!nonce,
			0, NULL, NULL, NULL, rowCounters, buf_blake_st);
	uint64_t t1 = now();
	fprintf(stderr, "Total %" PRId64 " solutions in %.1f ms (%.1f Sol/s)\n",
		total, (t1 - t0) / 1e3, total / ((t1 - t0) / 1e6));
	// Clean up
	if (dbg)
		free(dbg);
	clReleaseMemObject(buf_dbg);
	clReleaseMemObject(buf_sols);
	clReleaseMemObject(buf_ht[0]);
	clReleaseMemObject(buf_ht[1]);
	clReleaseMemObject(buf_blake_st);
}

/*
** Scan the devices available on this platform. Try to find the device
** selected by the "--use <id>" option and, if found, store the platform and
** device in plat_id and dev_id.
**
** plat			platform being scanned
** nr_devs_total	total number of devices detected so far, will be
** 			incremented by the number of devices available on this
** 			platform
** plat_id		where to store the platform id
** dev_id		where to store the device id
**
** Return 1 iff the selected device was found.
*/
unsigned scan_platform(cl_platform_id plat, cl_uint *nr_devs_total,
	cl_platform_id *plat_id, cl_device_id *dev_id)
{
	cl_device_type	typ = CL_DEVICE_TYPE_ALL;
	cl_uint		nr_devs = 0;
	cl_device_id	*devices;
	cl_int		status;
	unsigned		found = 0;
	unsigned		i;
	if (do_list_devices)
		print_platform_info(plat);
	status = clGetDeviceIDs(plat, typ, 0, NULL, &nr_devs);
	if (status != CL_SUCCESS)
		fatal("clGetDeviceIDs (%d)\n", status);
	if (nr_devs == 0)
		return 0;
	devices = (cl_device_id *)malloc(nr_devs * sizeof(*devices));
	status = clGetDeviceIDs(plat, typ, nr_devs, devices, NULL);
	if (status != CL_SUCCESS)
		fatal("clGetDeviceIDs (%d)\n", status);
	i = 0;
	while (i < nr_devs)
	{
		if (do_list_devices)
			print_device_info(*nr_devs_total, devices[i]);
		else if (*nr_devs_total == gpu_to_use)
		{
			found = 1;
			*plat_id = plat;
			*dev_id = devices[i];
			break;
		}
		(*nr_devs_total)++;
		i++;
	}
	free(devices);
	return found;
}

/*
** Stores the platform id and device id that was selected by the "--use <id>"
** option.
**
** plat_id		where to store the platform id
** dev_id		where to store the device id
*/
void scan_platforms(cl_platform_id *plat_id, cl_device_id *dev_id)
{
	cl_uint		nr_platforms;
	cl_platform_id	*platforms;
	cl_uint		i, nr_devs_total;
	cl_int		status;
	status = clGetPlatformIDs(0, NULL, &nr_platforms);
	if (status != CL_SUCCESS)
		fatal("Cannot get OpenCL platforms (%d)\n", status);
	if (!nr_platforms || verbose)
		fprintf(stderr, "Found %d OpenCL platform(s)\n", nr_platforms);
	if (!nr_platforms)
		exit(1);
	platforms = (cl_platform_id *)malloc(nr_platforms * sizeof(*platforms));
	if (!platforms)
		fatal("malloc: %s\n", strerror(errno));
	status = clGetPlatformIDs(nr_platforms, platforms, NULL);
	if (status != CL_SUCCESS)
		fatal("clGetPlatformIDs (%d)\n", status);
	i = nr_devs_total = 0;
	while (i < nr_platforms)
	{
		if (scan_platform(platforms[i], &nr_devs_total, plat_id, dev_id))
			break;
		i++;
	}
	if (do_list_devices)
		exit(0);
	debug("Using GPU device ID %d\n", gpu_to_use);
	amd_flag = is_platform_amd(*plat_id);
	free(platforms);
}

void init_and_run_opencl(uint8_t *header, size_t header_len)
{
	cl_platform_id	plat_id = 0;
	cl_device_id	dev_id = 0;
	cl_kernel		k_rounds[PARAM_K];
	cl_int		status;
	scan_platforms(&plat_id, &dev_id);
	if (!plat_id || !dev_id)
		fatal("Selected device (ID %d) not found; see --list\n", gpu_to_use);
	/* Create context.*/
	cl_context context = clCreateContext(NULL, 1, &dev_id,
		NULL, NULL, &status);
	if (status != CL_SUCCESS || !context)
		fatal("clCreateContext (%d)\n", status);
	/* Creating command queue associate with the context.*/
	cl_command_queue queue = clCreateCommandQueue(context, dev_id,
		0, &status);
	if (status != CL_SUCCESS || !queue)
		fatal("clCreateCommandQueue (%d)\n", status);
	/* Create program object */
#ifdef WIN32
	load_file("input.cl", &source, &source_len, 0);
	load_file("input.bin", &binary, &binary_len, 1);
#else
	source = ocl_code;
#endif
	source_len = strlen(source);
	cl_program program;
	cl_kernel k_init_ht;
	cl_kernel k_sols;
#ifdef WIN32
	if (!mining || num_mining_mode_threads <= 1) {
#endif
		program = clCreateProgramWithSource(context, 1, (const char **)&source,
			&source_len, &status);
		if (status != CL_SUCCESS || !program)
			fatal("clCreateProgramWithSource (%d)\n", status);
		/* Build program. */
		if (!mining || verbose)
			fprintf(stderr, "Building program\n");
		status = clBuildProgram(program, 1, &dev_id,
			(amd_flag) ? (OPENCL_BUILD_OPTIONS_AMD) : (OPENCL_BUILD_OPTIONS), // compile options
			NULL, NULL);
		if (status != CL_SUCCESS)
		{
			warn("OpenCL build failed (%d). Build log follows:\n", status);
			get_program_build_log(program, dev_id);
			exit(1);
		}
		get_program_bins(program);
		// Create kernel objects
		k_init_ht = clCreateKernel(program, "kernel_init_ht", &status);
		if (status != CL_SUCCESS || !k_init_ht)
			fatal("clCreateKernel (%d)\n", status);
		for (unsigned round = 0; round < PARAM_K; round++)
		{
			char	name[128];
			snprintf(name, sizeof(name), "kernel_round%d", round);
			k_rounds[round] = clCreateKernel(program, name, &status);
			if (status != CL_SUCCESS || !k_rounds[round])
				fatal("clCreateKernel (%d)\n", status);
		}
		k_sols = clCreateKernel(program, "kernel_sols", &status);
		if (status != CL_SUCCESS || !k_sols)
			fatal("clCreateKernel (%d)\n", status);
#ifdef WIN32
	}
#endif
	// Run
	run_opencl(header, header_len, &dev_id, context, queue, program, k_init_ht, k_rounds, k_sols);
	// Release resources
	assert(CL_SUCCESS == 0);
	status = CL_SUCCESS;
	status |= clReleaseKernel(k_init_ht);
	for (unsigned round = 0; round < PARAM_K; round++)
		status |= clReleaseKernel(k_rounds[round]);
	status |= clReleaseKernel(k_sols);
	status |= clReleaseProgram(program);
	status |= clReleaseCommandQueue(queue);
	status |= clReleaseContext(context);
	if (status)
		fprintf(stderr, "Cleaning resources failed\n");
}

uint32_t parse_header(uint8_t *h, size_t h_len, const char *hex)
{
	size_t      hex_len;
	size_t      bin_len;
	size_t	opt0 = ZCASH_BLOCK_HEADER_LEN;
	size_t      i;
	if (!hex)
	{
		if (!do_list_devices && !mining)
			fprintf(stderr, "Solving default all-zero %zd-byte header\n", opt0);
		return opt0;
	}
	hex_len = strlen(hex);
	bin_len = hex_len / 2;
	if (hex_len % 2)
		fatal("Error: input header must be an even number of hex digits\n");
	if (bin_len != opt0)
		fatal("Error: input header must be a %zd-byte full header\n", opt0);
	assert(bin_len <= h_len);
	for (i = 0; i < bin_len; i++)
		h[i] = hex2val(hex, i * 2) * 16 + hex2val(hex, i * 2 + 1);
	while (--i >= bin_len - N_ZERO_BYTES)
		if (h[i])
			fatal("Error: last %d bytes of full header (ie. last %d "
				"bytes of 32-byte nonce) must be zero due to an "
				"optimization in my BLAKE2b implementation\n",
				N_ZERO_BYTES, N_ZERO_BYTES);
	return bin_len;
}

enum
{
	OPT_HELP,
	OPT_VERBOSE,
	OPT_INPUTHEADER,
	OPT_NONCES,
	OPT_THREADS,
	OPT_N,
	OPT_K,
	OPT_LIST,
	OPT_USE,
	OPT_MINING,
};

static struct option    optlong[] =
{
	{ "help",		no_argument,		0,	OPT_HELP },
	{ "h",		no_argument,		0,	OPT_HELP },
	{ "verbose",	no_argument,		0,	OPT_VERBOSE },
	{ "v",		no_argument,		0,	OPT_VERBOSE },
	{ "i",		required_argument,	0,	OPT_INPUTHEADER },
	{ "nonces",	required_argument,	0,	OPT_NONCES },
	{ "t",		required_argument,	0,	OPT_THREADS },
	{ "n",		required_argument,	0,	OPT_N },
	{ "k",		required_argument,	0,	OPT_K },
	{ "list",		no_argument,		0,	OPT_LIST },
	{ "use",		required_argument,	0,	OPT_USE },
	{ "mining",	no_argument,		0,	OPT_MINING },
	{ 0,		0,			0,	0 },
};

void usage(const char *progname)
{
	printf("Usage: %s [options]\n"
		"A standalone GPU Zcash Equihash solver.\n"
		"\n"
		"Options are:\n"
		"  -h, --help     display this help and exit\n"
		"  -v, --verbose  print verbose messages\n"
		"  -i <input>     140-byte hex block header to solve "
		"(default: all-zero header)\n"
		"  --nonces <nr>  number of nonces to try (default: 1)\n"
		"  -n <n>         equihash n param (only supported value is 200)\n"
		"  -k <k>         equihash k param (only supported value is 9)\n"
		"  --list         list available OpenCL devices by ID (GPUs...)\n"
		"  --use <id>     use GPU <id> (default: 0)\n"
		"  --mining       enable mining mode (solver controlled via "
		"stdin/stdout)\n"
		, progname);
}

void tests(void)
{
	// if NR_ROWS_LOG is smaller, there is not enough space to store all bits
	// of Xi in a 32-byte slot
	assert(NR_ROWS_LOG >= 12);
}

int main(int argc, char **argv)
{
	uint8_t             header[ZCASH_BLOCK_HEADER_LEN] = { 0, };
	uint32_t            header_len;
	char		*hex_header = NULL;
	int32_t             i;
	while (-1 != (i = getopt_long_only(argc, argv, "", optlong, 0)))
		switch (i)
		{
		case OPT_HELP:
			usage(argv[0]), exit(0);
			break;
		case OPT_VERBOSE:
			verbose += 1;
			break;
		case OPT_INPUTHEADER:
			hex_header = optarg;
			show_encoded = 1;
			break;
		case OPT_NONCES:
			nr_nonces = parse_num(optarg);
			break;
		case OPT_THREADS:
#ifdef WIN32
			num_mining_mode_threads = parse_num(optarg);
			if (num_mining_mode_threads < 1)
				num_mining_mode_threads = 1;
			else if (num_mining_mode_threads > MAX_NUM_MINING_MODE_THREADS)
				num_mining_mode_threads = MAX_NUM_MINING_MODE_THREADS;
#endif
			break;
		case OPT_N:
			if (PARAM_N != parse_num(optarg))
				fatal("Unsupported n (must be %d)\n", PARAM_N);
			break;
		case OPT_K:
			if (PARAM_K != parse_num(optarg))
				fatal("Unsupported k (must be %d)\n", PARAM_K);
			break;
		case OPT_LIST:
			do_list_devices = 1;
			break;
		case OPT_USE:
			gpu_to_use = parse_num(optarg);
			break;
		case OPT_MINING:
			mining = 1;
			break;
		default:
			fatal("Try '%s --help'\n", argv[0]);
			break;
		}
	tests();
	if (mining)
		puts("Gateless Gate mining mode ready"), fflush(stdout);
	if (!mining) {
		verbose = 3;
		// fprintf(stderr, "Gateless Gate, a Zcash miner\n");
		// fprintf(stderr, "Copyright 2016 zawawa @ bitcointalk.org\n");
	}
	header_len = parse_header(header, sizeof(header), hex_header);
	init_and_run_opencl(header, header_len);
	if (!mining)
		getchar();
	return 0;
}
