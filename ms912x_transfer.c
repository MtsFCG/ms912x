
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
	kfree(request->conversion_buffer);
	request->conversion_buffer = NULL;
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

	request->conversion_buffer = kmalloc(2048 * 4, GFP_KERNEL);
	if (!request->conversion_buffer) {
		vfree(data);
		return -ENOMEM;
	}

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
	kfree(request->conversion_buffer);
	vfree(data);
	return ret;
}



static const u8 ms912x_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x00 };

static int ms912x_fb_xrgb8888_to_yuv422(struct ms912x_usb_request *request,
					const struct iosys_map *src,
					struct drm_framebuffer *fb,
					struct drm_rect *rect)
{
	void *dst = request->transfer_buffer;
	struct ms912x_frame_update_header *header =
		(struct ms912x_frame_update_header *)dst;
	struct iosys_map fb_map;
	int i, x, y1, y2, width;
	void *temp_buffer = request->conversion_buffer;

	y1 = rect->y1;
	y2 = min((unsigned int)rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	/* Zero-Allocation: temp_buffer is pre-allocated in request->conversion_buffer */

	header->header = cpu_to_be16(0xff00);
	header->x = x / 16;
	header->y = cpu_to_be16(y1);
	header->width = width / 16;
	header->height = cpu_to_be16(drm_rect_height(rect));
	dst += sizeof(*header);

	fb_map = IOSYS_MAP_INIT_OFFSET(src, y1 * fb->pitches[0]);

	kernel_fpu_begin();
	for (i = y1; i < y2; i++) {
		/* Copiar línea al buffer temporal (sysram) para procesamiento seguro */
		iosys_map_memcpy_from(temp_buffer, &fb_map, x * 4, width * 4);
		
		/* Procesar con AVX2 */
		ms912x_xrgb_to_yuv422_avx2(dst, temp_buffer, width);

		iosys_map_incr(&fb_map, fb->pitches[0]);
		dst += width * 2;
	}
	kernel_fpu_end();

	/* Fin del buffer */
	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return 0;
}

static int ms912x_fb_uyvy_to_uyvy(struct ms912x_usb_request *request,
				  const struct iosys_map *src,
				  struct drm_framebuffer *fb,
				  struct drm_rect *rect)
{
	void *dst = request->transfer_buffer;
	struct ms912x_frame_update_header *header =
		(struct ms912x_frame_update_header *)dst;
	struct iosys_map fb_map;
	int i, x, y1, y2, width;

	y1 = rect->y1;
	y2 = min((unsigned int)rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	header->header = cpu_to_be16(0xff00);
	header->x = x / 16;
	header->y = cpu_to_be16(y1);
	header->width = width / 16;
	header->height = cpu_to_be16(drm_rect_height(rect));
	dst += sizeof(*header);

	/* UYVY is 2 bytes per pixel */
	fb_map = IOSYS_MAP_INIT_OFFSET(src, y1 * fb->pitches[0]);

	for (i = y1; i < y2; i++) {
		iosys_map_memcpy_from(dst, &fb_map, x * 2, width * 2);

		iosys_map_incr(&fb_map, fb->pitches[0]);
		dst += width * 2;
	}

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

    /* DEBUG: Identify format */
    {
        static u32 last_fmt = 0;
        if (fb->format->format != last_fmt) {
            printk(KERN_INFO "ms912x: format change detected: %p4cc\n", &fb->format->format);
            last_fmt = fb->format->format;
        }
    }

	if (fb->format->format == DRM_FORMAT_UYVY)
		ret = ms912x_fb_uyvy_to_uyvy(current_request, map, fb, rect);
	else
		ret = ms912x_fb_xrgb8888_to_yuv422(current_request, map, fb, rect);
	
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
