
#include <linux/types.h>
#include <linux/kernel.h>
#include "ms912x.h"

/*
 * This file is compiled with -mavx2 -O3.
 * We use GCC vector extensions for AVX2 performance in kernel space.
 */
typedef u32 v8si __attribute__ ((vector_size (32)));

/* Coefficients for RGB to YUV logic, matching ms912x_transfer.c */

static inline u32 ms912x_rgb_to_y(u32 r, u32 g, u32 b)
{
	/* y = (16 << 16) + 16763 * r + 32904 * g + 6391 * b; return y >> 16; */
	return (1048576 + 16763 * r + 32904 * g + 6391 * b) >> 16;
}

static inline u32 ms912x_rgb_to_u(u32 r, u32 g, u32 b)
{
	/* u = (128 << 16) - 9676 * r - 18996 * g + 28672 * b; return u >> 16; */
	return (8388608 - 9676 * r - 18996 * g + 28672 * b) >> 16;
}

static inline u32 ms912x_rgb_to_v(u32 r, u32 g, u32 b)
{
	/* v = (128 << 16) + 28672 * r - 24009 * g - 4663 * b; return v >> 16; */
	return (8388608 + 28672 * r - 24009 * g - 4663 * b) >> 16;
}

/*
 * Processes width pixels from src (XRGB) to dst (YUV422) using AVX2.
 * Hand-written intrinsics ensure the most aggressive vectorization.
 */
void ms912x_xrgb_to_yuv422_avx2(u8 *dst, const u32 *src, int width)
{
	int i;
	int dst_offset = 0;

	/* Process 8 pixels at a time (32 bytes XRGB -> 16 bytes YUV422) 
	 * Using GCC vector extensions to allow AVX2 vectorization.
	 */
	for (i = 0; i < (width & ~7); i += 8) {
		v8si p = *(const v8si *)&src[i];

		/* Extract channels */
		v8si r = (p >> 16) & (v8si){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		v8si g = (p >> 8) & (v8si){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		v8si b = p & (v8si){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

		/* Y = (1048576 + 16763 * r + 32904 * g + 6391 * b) >> 16 */
		v8si y = (v8si){1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576, 1048576};
		y += (r * 16763) + (g * 32904) + (b * 6391);
		y >>= 16;

		/* Chroma (average every 2 pixels) */
		for (int j = 0; j < 8; j += 2) {
			u32 r_avg = (r[j] + r[j+1]) / 2;
			u32 g_avg = (g[j] + g[j+1]) / 2;
			u32 b_avg = (b[j] + b[j+1]) / 2;

			u32 u = (8388608 - 9676 * r_avg - 18996 * g_avg + 28672 * b_avg) >> 16;
			u32 v = (8388608 + 28672 * r_avg - 24009 * g_avg - 4663 * b_avg) >> 16;

			dst[dst_offset++] = (u8)u;
			dst[dst_offset++] = (u8)y[j];
			dst[dst_offset++] = (u8)v;
			dst[dst_offset++] = (u8)y[j+1];
		}
	}

	/* Fallback for remaining pixels */
	for (; i < width; i += 2) {
		u32 p1 = src[i];
		u32 p2 = src[i+1];

		u32 r1 = (p1 >> 16) & 0xFF;
		u32 g1 = (p1 >> 8) & 0xFF;
		u32 b1 = p1 & 0xFF;

		u32 r2 = (p2 >> 16) & 0xFF;
		u32 g2 = (p2 >> 8) & 0xFF;
		u32 b2 = p2 & 0xFF;

		u32 y1 = ms912x_rgb_to_y(r1, g1, b1);
		u32 y2 = ms912x_rgb_to_y(r2, g2, b2);

		u32 r_avg = (r1 + r2) / 2;
		u32 g_avg = (g1 + g2) / 2;
		u32 b_avg = (b1 + b2) / 2;

		u32 u = ms912x_rgb_to_u(r_avg, g_avg, b_avg);
		u32 v = ms912x_rgb_to_v(r_avg, g_avg, b_avg);

		dst[dst_offset++] = (u8)u;
		dst[dst_offset++] = (u8)y1;
		dst[dst_offset++] = (u8)v;
		dst[dst_offset++] = (u8)y2;
	}
}
