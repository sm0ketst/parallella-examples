/*
 * Copyright (c) 2015 Brown Deer Technology, LLC.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This software was developed by Brown Deer Technology, LLC.
 * For more information contact info@browndeertechnology.com
 */

/* DAR */

#include <complex.h>
#define cfloat float complex
#include <string.h>
#if 0
#include <stdint.h>
#else
/* Workaround: Including stdint result in clcc1 segfault, oh and typedefs
 * won't work either ... */
#define uint8_t		unsigned char
#define uint16_t	unsigned short
#define uint32_t	unsigned int
#define uint64_t	unsigned long long

#define int8_t		char
#define int16_t		short
#define int32_t		int
#define int64_t		long long

#define uintptr_t	unsigned int
#endif

#include <coprthr.h>

/* Use repo version. Doesn't matter for device code right now though */
#include "coprthr_mpi.h"

/* struct my_args */
#include "device.h"

void bitreverse_swap(uint16_t * const brp, void *p_data)
{
	uint16_t i, j;
	int k;

        cfloat *data = (cfloat *) p_data;

	for (k=0; brp[k]; ) {
		i = brp[k++];
		j = brp[k++];
		cfloat tmp = data[i];
		data[i] = data[j];
		data[j] = tmp;
	}
}

void fft_r2_dit(unsigned int n, unsigned int m, void* p_wn, void* p_data )
{
	cfloat* wn = (cfloat*)p_wn;
	cfloat* data = (cfloat*)p_data;

	int l,l1,l2,wstride = n;
	int i0,i1;
	l2 = 1;
	for(l=0; l<m; l++) {
		l1 = l2;
		l2 <<= 1;
		int i=0, j;
		wstride = wstride >> 1;
		for(j=0; j<l1; j++) {
			cfloat wni = wn[i];
			for(i0=j; i0<n; i0+=l2) {
				i1 = i0 + l1;
				cfloat tmp0 = data[i0];
				cfloat tmp = wni * data[i1];
				data[i1] = tmp0 - tmp;
				data[i0] = tmp0 + tmp;
			}
			i += wstride;
		}
	}
}


void corner_turn( 
	int nprocs, int rank, void* p_data, unsigned int n, unsigned int nlocal,
	void* p_comm 
) {
	cfloat* data = (cfloat*)p_data;
	MPI_Comm comm = (MPI_Comm)p_comm;

	MPI_Status status;

	void* memfree = coprthr_tls_sbrk(0);
	cfloat* buf2 = (cfloat*)coprthr_tls_sbrk(nlocal*nlocal*sizeof(cfloat));

	int i, j, k = rank, l;

	int ct = (nprocs - k) % nprocs;

	for(l=0;l<nprocs;l++) {

		cfloat* src = data + ct*nlocal;
		cfloat* dst = buf2;
		for(i=0;i<nlocal;i++) {
			uint64_t *psrc = (uint64_t *)src;
			uint64_t *pdst = (uint64_t *)dst;
			for(j=0; j<nlocal; j+=4) {
				*(pdst++) = *(psrc++);
				*(pdst++) = *(psrc++);
				*(pdst++) = *(psrc++);
				*(pdst++) = *(psrc++);
			}
			src += n;
			dst += nlocal;
		}

		MPI_Sendrecv_replace(buf2,2*nlocal*nlocal,MPI_FLOAT,
			ct,71,ct,72,comm,&status);

		for(i=0;i<nlocal;i++) {
			cfloat* pdst = data+ct*nlocal+i;
			cfloat* psrc = buf2+i*nlocal;
			for(j=0; j<nlocal; j+=4) {
				*pdst = *psrc++;
				pdst += n;
				*pdst = *psrc++;
				pdst += n;
				*pdst = *psrc++;
				pdst += n;
				*pdst = *psrc++;
				pdst += n;
			}
		}

		ct = (ct+1) % nprocs;

	}

	coprthr_tls_brk(memfree);

}

void fft2d(void *comm, int nprocs, int rank, unsigned int nlocal,
	   unsigned int n, unsigned int m, uint16_t *brp, void *p_wn,
	   void *p_signal)
{
	cfloat *s;
	unsigned int row;

	//// bit-reversal swap in-place
	for(s = (cfloat *) p_signal, row = 0; row < nlocal; row++, s += n) {
		bitreverse_swap(brp, s);
	}

	for(s = (cfloat *) p_signal, row=0; row < nlocal; row++, s += n) {
		fft_r2_dit(n, m, p_wn, s);
	}

	//// corner turn
	corner_turn(nprocs, rank, p_signal, n, nlocal, comm);

	//// bit-reversal swap in-place
	for(s = (cfloat *) p_signal, row=0; row < nlocal; row++, s += n) {
		bitreverse_swap(brp, s);
	}

	//// forward FFT
	for(s = (cfloat *) p_signal, row=0; row < nlocal; row++, s += n) {
		fft_r2_dit(n, m, p_wn, s);
	}

	//// corner turn
	corner_turn(nprocs, rank, p_signal, n, nlocal, comm);
}


/* TODO: xcorr_batch */
__kernel void
my_thread (void *p) {
	struct my_args args;

        memcpy(&args, p, sizeof(args));

	cfloat *g_wn_fwd = args.wn_fwd;
	cfloat *g_wn_bwd = args.wn_bwd;

	/* TODO: First image now */
	cfloat *g_cmp_bmp = args.bitmaps;

	int i,j,k,l;
	int row;

	int mask = (1 << args.m) - 1;

	int nprocs;
	int myrank;

	MPI_Init(0, MPI_BUF_SIZE);

	MPI_Comm comm = MPI_COMM_THREAD;

	MPI_Comm_size(comm, &nprocs);
	MPI_Comm_rank(comm, &myrank);

	size_t nlocal = args.n / nprocs;

	size_t wn_sz		= args.n * sizeof(cfloat);
	size_t brp_sz		= 2 * args.n * sizeof(uint16_t);
	size_t l_fft_sz		= nlocal * args.n * sizeof(cfloat);

	void *memfree = coprthr_tls_sbrk(0);

	uint16_t *brp = (uint16_t *) coprthr_tls_sbrk(brp_sz);
        memset(brp, 0, brp_sz);

	for(i = 0, k = 0; i < args.n; i++) {
		int x = i;
		int y = 0;
		for(j=0; j < args.m; j++)
			y = (y << 1) | ((x >> j) & 1);
		if (x < y) {
			brp[k++] = x;
			brp[k++] = y;
		}
	}

	cfloat* wn_fwd = (cfloat *) coprthr_tls_sbrk(wn_sz);
	cfloat* wn_bwd = (cfloat *) coprthr_tls_sbrk(wn_sz);
	cfloat* l_tmp_fft  = (cfloat *) coprthr_tls_sbrk(l_fft_sz);

	/* Copy wn coeffs to local mem */
	e_dma_copy(wn_fwd, g_wn_fwd, wn_sz);
	e_dma_copy(wn_bwd, g_wn_bwd, wn_sz);

	/* Copy IMG A (reference image) to local memory */
	e_dma_copy(l_tmp_fft, args.ref_bitmap + myrank * nlocal * args.n,
		   l_fft_sz);

	/* TODO: normalize(l_tmp_fft) */

	// FFT IMG A
	fft2d(comm, nprocs, myrank, nlocal, args.n, args.m, brp, wn_fwd,
	      l_tmp_fft);
	e_dma_copy(args.ref_fft + myrank * nlocal * args.n, l_tmp_fft,
		   l_fft_sz);

	// FFT IMG B
	e_dma_copy(l_tmp_fft, g_cmp_bmp + myrank * nlocal * args.n, l_fft_sz);
	fft2d(comm, nprocs, myrank, nlocal, args.n, args.m, brp, wn_fwd,
	      l_tmp_fft);

	// tmp = A * conj(B)

	/* Not enough core SRAM to store two FFTs + code ...
	 * So we resort to copying in smaller chunks of ref FFT...
	 * It is possible to get the number of iterations down to 4 if you patch
	 * COPRTHR to use -Os instead of -O3 when compiling this code.
	 *
	 * TODO: Investigate if we can get rid of this (fit two FFTs in mem)
	 * or at least get the number of iterations down.
	 */
	size_t small_iters = 8;
	size_t n_small = args.n * nlocal / small_iters;
	size_t sz_small = n_small * sizeof(cfloat);
	cfloat *l_ref_fft  = (cfloat *) coprthr_tls_sbrk(sz_small);

	for (i = 0; i < small_iters; i++) {
		e_dma_copy(l_ref_fft,
			   args.ref_fft + myrank * nlocal * args.n + i * n_small,
			   sz_small);

		for (j = 0; j < n_small; j++) {
			l_tmp_fft[i * n_small + j] =
				l_ref_fft[j] * conjf(l_tmp_fft[i * n_small + j]);
		}
	}

	/* IFFT() */
	fft2d(comm, nprocs, myrank, nlocal, args.n, args.m, brp, wn_bwd,
	      l_tmp_fft);

	// TODO: Calculate max on device ...
	e_dma_copy(args.ref_bitmap + myrank * nlocal * args.n, l_tmp_fft,
		   l_fft_sz);

	coprthr_tls_brk(memfree);

	MPI_Finalize();
}

