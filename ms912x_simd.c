
#include <linux/types.h>
#include <linux/kernel.h>

/*
 * This file is compiled with -mavx2 -O3.
 * We avoid including complex kernel headers that might conflict with FPU flags,
 * but linux/types.h is usually safe and necessary.
 */

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
 * Processes width pixels from src (XRGB) to dst (YUV422).
 * width must be a multiple of 8 for this AVX2 path to be most effective,
 * but the loop handles any even width.
 * With -mavx2 -O3, GCC should vectorize this loop primarily.
 * We process 2 pixels per iteration producing 4 bytes (U Y V Y).
 *
 * This function handles the WHOLE line, logic-wise identical to the optimized scalar version.
 */
void ms912x_xrgb_to_yuv422_avx2(u8 *dst, const u32 *src, int width)
{
	int i;
	int dst_offset = 0;

	/* 
	 * We rely on the compiler to vectorize this loop with AVX2 instructions.
	 * We unroll slightly to encourage it.
	 */
	for (i = 0; i < width; i += 2) {
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

		/* Average for Chroma */
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
