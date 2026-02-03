
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>

#include <linux/timer.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "ms912x.h"

static void ms912x_request_timeout(struct timer_list *t)
{
	struct ms912x_usb_request *request = container_of(t, struct ms912x_usb_request, timer);
	usb_sg_cancel(&request->sgr);
}

static void ms912x_request_work(struct work_struct *work)
{
	struct ms912x_usb_request *request =
		container_of(work, struct ms912x_usb_request, work);
	struct ms912x_device *ms912x = request->ms912x;
	struct usb_device *usbdev = interface_to_usbdev(ms912x->intf);
	struct usb_sg_request *sgr = &request->sgr;
	struct sg_table *transfer_sgt = &request->transfer_sgt;

	timer_setup(&request->timer, ms912x_request_timeout, 0);
	usb_sg_init(sgr, usbdev, usb_sndbulkpipe(usbdev, 0x04), 0,
		    transfer_sgt->sgl, transfer_sgt->nents,
		    request->transfer_len, GFP_KERNEL);
	mod_timer(&request->timer, jiffies + msecs_to_jiffies(5000));
	usb_sg_wait(sgr);
	timer_shutdown_sync(&request->timer);
	complete(&request->done);
}

void ms912x_free_request(struct ms912x_usb_request *request)
{
	if (!request->transfer_buffer)
		return;
	sg_free_table(&request->transfer_sgt);
	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
	request->alloc_len = 0;
}

int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len)
{
	int ret, i;
	unsigned int num_pages;
	void *data;
	struct page **pages;
	void *ptr;

	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	for (i = 0, ptr = data; i < num_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);
	ret = sg_alloc_table_from_pages(&request->transfer_sgt, pages,
					num_pages, 0, len, GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto err_vfree;

	request->alloc_len = len;
	request->transfer_buffer = data;
	request->ms912x = ms912x;

	init_completion(&request->done);
	INIT_WORK(&request->work, ms912x_request_work);
	return 0;
err_vfree:
	vfree(data);
	return ret;
}



static const u8 ms912x_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x00 };

static int ms912x_fb_xrgb8888_to_yuv422(void *dst, const struct iosys_map *src,
					struct drm_framebuffer *fb,
					struct drm_rect *rect)
{
	struct ms912x_frame_update_header *header =
		(struct ms912x_frame_update_header *)dst;
	struct iosys_map fb_map;
	int i, x, y1, y2, width;
	void *temp_buffer;

	y1 = rect->y1;
	y2 = min((unsigned int)rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	temp_buffer = kmalloc(width * 4, GFP_KERNEL);
	if (!temp_buffer)
		return -ENOMEM;

	header->header = cpu_to_be16(0xff00);
	header->x = x / 16;
	header->y = cpu_to_be16(y1);
	header->width = width / 16;
	header->height = cpu_to_be16(drm_rect_height(rect));
	dst += sizeof(*header);

	fb_map = IOSYS_MAP_INIT_OFFSET(src, y1 * fb->pitches[0]);

	/* Using FPU in kernel requires protection. 
	 * We process the whole block of lines if possible.
	 */
	kernel_fpu_begin();
	for (i = y1; i < y2; i++) {
		struct iosys_map line_map = fb_map;
		/* 
		 * We need a direct pointer for our SIMD function. 
		 * iosys_map usually points to vmalloc or kmap address.
		 * If it's system memory (is_iomem is false), we can cast.
		 */
		if (!line_map.is_iomem) {
			ms912x_xrgb_to_yuv422_avx2(dst, line_map.vaddr, width);
		} else {
			/* Fallback for IOMEM or if something is weird, 
			 * though for checking simplicity we just use scalar logic here 
			 * if we can't get a direct pointer? 
			 * Actually we can just copy to temp buffer and run check...
			 * But wait, previous code copied to temp_buffer!
			 */
			 
			/* Re-implementing the copy to temp logic for the loop */
			/* Wait, my ms912x_xrgb_to_yuv422_avx2 takes u32*, which is temp_buffer! */
			
			/* Let's be careful. The original code:
			 * ms912x_xrgb_to_yuv422_line(dst, &fb_map, x * 4, width, temp_buffer);
			 * 
			 * Inside that function:
			 * iosys_map_memcpy_from(temp_buffer, xrgb_buffer, offset, width * 4);
			 * ... process temp_buffer ...
			 */
			
			/* So we should do the copy first as before, then process temp_buffer */
		}
		
		/* Correct integration: */
		/* Copy line to temp buffer (sysram) */
		iosys_map_memcpy_from(temp_buffer, &fb_map, x * 4, width * 4);
		
		/* Process with AVX2 */
		ms912x_xrgb_to_yuv422_avx2(dst, temp_buffer, width);

		iosys_map_incr(&fb_map, fb->pitches[0]);
		dst += width * 2;
	}
	kernel_fpu_end();

	kfree(temp_buffer);
	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return 0;
}

int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_rect *rect)
{
	int ret = 0, idx;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_usb_request *prev_request, *current_request;
	int x, width;

	/* Seems like hardware can only update framebuffer 
	 * in multiples of 16 horizontally
	 */
	x = ALIGN_DOWN(rect->x1, 16);
	/* Resolutions that are not a multiple of 16 like 1366*768 
	 * need to be aligned
	 */
	width = min(ALIGN(rect->x2, 16), ALIGN_DOWN((int)fb->width, 16)) - x;
	rect->x1 = x;
	rect->x2 = x + width;
	current_request = &ms912x->requests[ms912x->current_request];
	prev_request = &ms912x->requests[1 - ms912x->current_request];

	drm_dev_enter(drm, &idx);

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto dev_exit;

	ret = ms912x_fb_xrgb8888_to_yuv422(current_request->transfer_buffer,
					   map, fb, rect);
	
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto dev_exit;

	/* Sending frames too fast, drop it.
	 * Do not wait (timeout=0) to avoid blocking the compositor/cursor
	 * if the USB bus is busy. It's better to drop a frame than to lag.
	 */
	if (!wait_for_completion_timeout(&prev_request->done, 0)) {

		ret = -ETIMEDOUT;
		goto dev_exit;
	}

	current_request->transfer_len = width * 2 * drm_rect_height(rect) + 16;
	queue_work(system_long_wq, &current_request->work);
	ms912x->current_request = 1 - ms912x->current_request;
dev_exit:
	drm_dev_exit(idx);
	return ret;
}
