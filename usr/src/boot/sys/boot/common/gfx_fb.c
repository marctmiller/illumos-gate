/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Toomas Soome <tsoome@me.com>
 */

/*
 * Common functions to implement graphical framebuffer support for console.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <stand.h>
#if defined(EFI)
#include <efi.h>
#include <efilib.h>
#else
#include <btxv86.h>
#endif
#include <sys/tem_impl.h>
#include <sys/consplat.h>
#include <sys/visual_io.h>
#include <sys/multiboot2.h>
#include <sys/font.h>
#include <sys/endian.h>
#include <gfx_fb.h>
#include <pnglite.h>
#include <bootstrap.h>

/*
 * Global framebuffer struct, to be updated with mode changes.
 */
multiboot_tag_framebuffer_t gfx_fb;

/* To support setenv, keep track of inverses and colors. */
static int gfx_inverse = 0;
static int gfx_inverse_screen = 0;
static uint8_t gfx_fg = DEFAULT_ANSI_FOREGROUND;
static uint8_t gfx_bg = DEFAULT_ANSI_BACKGROUND;

static int gfx_fb_cons_clear(struct vis_consclear *);
static void gfx_fb_cons_copy(struct vis_conscopy *);
static void gfx_fb_cons_display(struct vis_consdisplay *);

#define	abs(x)		((x) < 0? -(x):(x))
#define max(x, y)	((x) >= (y) ? (x) : (y))
/*
 * Translate platform specific FB address.
 */
static uint8_t *
gfx_get_fb_address(void)
{
#if defined(EFI)
	return ((uint8_t *)(uintptr_t)
	    gfx_fb.framebuffer_common.framebuffer_addr);
#else
	return ((uint8_t *)PTOV((uint32_t)
	    gfx_fb.framebuffer_common.framebuffer_addr & 0xffffffff));
#endif
}

/*
 * Generic platform callbacks for tem.
 */
void
plat_tem_get_prom_font_size(int *charheight, int *windowtop)
{
	*charheight = 0;
	*windowtop = 0;
}

void
plat_tem_get_colors(uint8_t *fg, uint8_t *bg)
{
	*fg = gfx_fg;
	*bg = gfx_bg;
}

void
plat_tem_get_inverses(int *inverse, int *inverse_screen)
{
	*inverse = gfx_inverse;
	*inverse_screen = gfx_inverse_screen;
}

/*
 * Support for color mapping.
 */
uint32_t
gfx_fb_color_map(uint8_t index)
{
	uint8_t c;
	int pos, size;
	uint32_t color;

	if (gfx_fb.framebuffer_common.framebuffer_type !=
	    MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
		return (index);
	}

	c = cmap4_to_24.red[index];
	pos = gfx_fb.u.fb2.framebuffer_red_field_position;
	size = gfx_fb.u.fb2.framebuffer_red_mask_size;
	color = ((c >> (8 - size)) & ((1 << size) - 1)) << pos;

	c = cmap4_to_24.green[index];
	pos = gfx_fb.u.fb2.framebuffer_green_field_position;
	size = gfx_fb.u.fb2.framebuffer_green_mask_size;
	color |= ((c >> (8 - size)) & ((1 << size) - 1)) << pos;

	c = cmap4_to_24.blue[index];
	pos = gfx_fb.u.fb2.framebuffer_blue_field_position;
	size = gfx_fb.u.fb2.framebuffer_blue_mask_size;
	color |= ((c >> (8 - size)) & ((1 << size) - 1)) << pos;

	return (color);
}

static int
color_name_to_ansi(const char *name, int *val)
{
	if (strcasecmp(name, "black") == 0) {
		*val = 0;
		return (0);
	}
	if (strcasecmp(name, "red") == 0) {
		*val = 1;
		return (0);
	}
	if (strcasecmp(name, "green") == 0) {
		*val = 2;
		return (0);
	}
	if (strcasecmp(name, "yellow") == 0) {
		*val = 3;
		return (0);
	}
	if (strcasecmp(name, "blue") == 0) {
		*val = 4;
		return (0);
	}
	if (strcasecmp(name, "magenta") == 0) {
		*val = 5;
		return (0);
	}
	if (strcasecmp(name, "cyan") == 0) {
		*val = 6;
		return (0);
	}
	if (strcasecmp(name, "white") == 0) {
		*val = 7;
		return (0);
	}
	return (1);
}

/* Callback to check and set colors */
static int
gfx_set_colors(struct env_var *ev, int flags, const void *value)
{
	int val = 0;
	char buf[2];
	const void *evalue;

	if (value == NULL)
		return (CMD_OK);

	if (color_name_to_ansi(value, &val)) {
		val = (int) strtol(value, NULL, 0);
		evalue = value;
	} else {
		snprintf(buf, sizeof (buf), "%d", val);
		evalue = buf;
	}

	/* invalid value? */
	if (val < 0 || val > 7)
		return (CMD_OK);

	if (strcmp(ev->ev_name, "tem.fg_color") == 0) {
		/* is it already set? */
		if (gfx_fg == val)
			return (CMD_OK);
		gfx_fg = val;
	}
	if (strcmp(ev->ev_name, "tem.bg_color") == 0) {
		/* is it already set? */
		if (gfx_bg == val)
			return (CMD_OK);
		gfx_bg = val;
	}
	env_setenv(ev->ev_name, flags | EV_NOHOOK, evalue, NULL, NULL);
	plat_cons_update_mode(-1);
	return (CMD_OK);
}

/* Callback to check and set inverses */
static int
gfx_set_inverses(struct env_var *ev, int flags, const void *value)
{
	int t, f;

	if (value == NULL)
		return (CMD_OK);

	t = strcmp(value, "true");
	f = strcmp(value, "false");

	/* invalid value? */
	if (t != 0 && f != 0)
		return (CMD_OK);

	if (strcmp(ev->ev_name, "tem.inverse") == 0) {
		/* is it already set? */
		if (gfx_inverse == (t == 0))
			return (CMD_OK);
		gfx_inverse = (t == 0);
	}
	if (strcmp(ev->ev_name, "tem.inverse-screen") == 0) {
		/* is it already set? */
		if (gfx_inverse_screen == (t == 0))
			return (CMD_OK);
		gfx_inverse_screen = (t == 0);
	}
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);
	plat_cons_update_mode(-1);
	return (CMD_OK);
}

/*
 * Initialize gfx framework.
 */
void
gfx_framework_init(struct visual_ops *fb_ops)
{
	int rc;
	char *env, buf[2];

	/* Add visual io callbacks */
	fb_ops->cons_clear = gfx_fb_cons_clear;
	fb_ops->cons_copy = gfx_fb_cons_copy;
	fb_ops->cons_display = gfx_fb_cons_display;

	/* set up tem inverse controls */
	env = getenv("tem.inverse");
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			gfx_inverse = 1;
		unsetenv("tem.inverse");
	}

	env = getenv("tem.inverse-screen");
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			gfx_inverse_screen = 1;
		unsetenv("tem.inverse-screen");
	}

	if (gfx_inverse)
		env = "true";
	else
		env = "false";
	env_setenv("tem.inverse", EV_VOLATILE, env, gfx_set_inverses,
            env_nounset);

	if (gfx_inverse_screen)
		env = "true";
	else
		env = "false";

	env_setenv("tem.inverse-screen", EV_VOLATILE, env, gfx_set_inverses,
            env_nounset);

	/* set up tem color controls */
	env = getenv("tem.fg_color");
	if (env != NULL) {
		rc = (int) strtol(env, NULL, 0);
		if (rc >= 0 && rc <= 7)
			gfx_fg = rc;
		unsetenv("tem.fg_color");
	}

	env = getenv("tem.bg_color");
	if (env != NULL) {
		rc = (int) strtol(env, NULL, 0);
		if (rc >= 0 && rc <= 7)
			gfx_bg = rc;
		unsetenv("tem.bg_color");
	}

	snprintf(buf, sizeof (buf), "%d", gfx_fg);
	env_setenv("tem.fg_color", EV_VOLATILE, buf, gfx_set_colors,
	    env_nounset);
	snprintf(buf, sizeof (buf), "%d", gfx_bg);
	env_setenv("tem.bg_color", EV_VOLATILE, buf, gfx_set_colors,
	    env_nounset);
}

/*
 * visual io callbacks.
 */

static int
gfx_fb_cons_clear(struct vis_consclear *ca)
{
	uint8_t *fb, *fb8;
	uint32_t data, *fb32;
	uint16_t *fb16;
	int i, j, width, height, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	pitch = gfx_fb.framebuffer_common.framebuffer_pitch;
	width = gfx_fb.framebuffer_common.framebuffer_width;
	height = gfx_fb.framebuffer_common.framebuffer_height;

	data = gfx_fb_color_map(ca->bg_color);
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	switch (gfx_fb.framebuffer_common.framebuffer_bpp) {
	case 8:		/* 8 bit */
		for (i = 0; i < height; i++) {
			(void) memset(fb + i * pitch, ca->bg_color, pitch);
		}
		break;
	case 15:
	case 16:		/* 16 bit */
		for (i = 0; i < height; i++) {
			fb16 = (uint16_t *)(fb + i * pitch);
			for (j = 0; j < width; j++)
				fb16[j] = (uint16_t)(data & 0xffff);
		}
		break;
	case 24:		/* 24 bit */
		for (i = 0; i < height; i++) {
			fb8 = fb + i * pitch;
			for (j = 0; j < pitch; j += 3) {
				fb8[j] = (data >> 16) & 0xff;
				fb8[j+1] = (data >> 8) & 0xff;
				fb8[j+2] = data & 0xff;
			}
		}
		break;
	case 32:		/* 32 bit */
		for (i = 0; i < height; i++) {
			fb32 = (uint32_t *)(fb + i * pitch);
			for (j = 0; j < width; j++)
				fb32[j] = data;
		}
		break;
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif

	return (0);
}

static void
gfx_fb_cons_copy(struct vis_conscopy *ma)
{
	uint32_t soffset, toffset;
	uint32_t width, height;
	uint8_t *src, *dst, *fb;
	int i, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	bpp = roundup2(gfx_fb.framebuffer_common.framebuffer_bpp, 8) >> 3;
	pitch = gfx_fb.framebuffer_common.framebuffer_pitch;

	soffset = ma->s_col * bpp + ma->s_row * pitch;
	toffset = ma->t_col * bpp + ma->t_row * pitch;
	src = fb + soffset;
	dst = fb + toffset;
	width = (ma->e_col - ma->s_col + 1) * bpp;
	height = ma->e_row - ma->s_row + 1;

#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	if (toffset <= soffset) {
		for (i = 0; i < height; i++) {
			uint32_t increment = i * pitch;
			(void)memmove(dst + increment, src + increment, width);
		}
	} else {
		for (i = height - 1; i >= 0; i--) {
			uint32_t increment = i * pitch;
			(void)memmove(dst + increment, src + increment, width);
		}
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

/*
 * Implements alpha blending for RGBA data, could use pixels for arguments,
 * but byte stream seems more generic.
 * The generic alpha blending is:
 * blend = alpha * fg + (1.0 - alpha) * bg.
 * Since our alpha is not from range [0..1], we scale appropriately.
 */
static uint8_t
alpha_blend(uint8_t fg, uint8_t bg, uint8_t alpha)
{
	uint16_t blend, h, l;

	/* trivial corner cases */
	if (alpha == 0)
		return (bg);
	if (alpha == 0xFF)
		return (fg);
	blend = (alpha * fg + (0xFF - alpha) * bg);
	/* Division by 0xFF */
	h = blend >> 8;
	l = blend & 0xFF;
	if (h + l >= 0xFF)
		h++;
	return (h);
}

/* Copy memory to framebuffer or to memory. */
static void
bitmap_cpy(uint8_t *dst, uint8_t *src, uint32_t len, int bpp)
{
	uint32_t i;
	uint8_t a;

	switch (bpp) {
	case 4:
		for (i = 0; i < len; i += bpp) {
			a = src[i+3];
			dst[i] = alpha_blend(src[i], dst[i], a);
			dst[i+1] = alpha_blend(src[i+1], dst[i+1], a);
			dst[i+2] = alpha_blend(src[i+2], dst[i+2], a);
			dst[i+3] = a;
		}
		break;
	default:
		(void) memcpy(dst, src, len);
		break;
	}
}

static void
gfx_fb_cons_display(struct vis_consdisplay *da)
{
	uint32_t size;		/* write size per scanline */
	uint8_t *fbp;		/* fb + calculated offset */
	int i, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	/* make sure we will not write past FB */
	if (da->col >= gfx_fb.framebuffer_common.framebuffer_width ||
	    da->row >= gfx_fb.framebuffer_common.framebuffer_height ||
	    da->col + da->width > gfx_fb.framebuffer_common.framebuffer_width ||
	    da->row + da->height > gfx_fb.framebuffer_common.framebuffer_height)
		return;

	bpp = roundup2(gfx_fb.framebuffer_common.framebuffer_bpp, 8) >> 3;
	pitch = gfx_fb.framebuffer_common.framebuffer_pitch;

	size = da->width * bpp;
	fbp = gfx_get_fb_address();
	fbp += da->col * bpp + da->row * pitch;

	/* write all scanlines in rectangle */
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	for (i = 0; i < da->height; i++) {
		uint8_t *dest = fbp + i * pitch;
		uint8_t *src = da->data + i * size;
		bitmap_cpy(dest, src, size, bpp);
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

void
gfx_fb_display_cursor(struct vis_conscursor *ca)
{
	uint32_t fg, bg;
	uint32_t offset, size, *fb32;
	uint16_t *fb16;
	uint8_t *fb8, *fb;
	int i, j, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	bpp = roundup2(gfx_fb.framebuffer_common.framebuffer_bpp, 8) >> 3;
	pitch = gfx_fb.framebuffer_common.framebuffer_pitch;

	size = ca->width * bpp;

	/*
	 * Build cursor image. We are building mirror image of data on
	 * frame buffer by (D xor FG) xor BG.
	 */
	offset = ca->col * bpp + ca->row * pitch;
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	switch (gfx_fb.framebuffer_common.framebuffer_bpp) {
	case 8:		/* 8 bit */
		fg = ca->fg_color.mono;
		bg = ca->bg_color.mono;
		for (i = 0; i < ca->height; i++) {
			fb8 = fb + offset + i * pitch;
			for (j = 0; j < size; j += 1) {
				fb8[j] = (fb8[j] ^ (fg & 0xff)) ^ (bg & 0xff);
			}
		}
		break;
	case 15:
	case 16:	/* 16 bit */
		fg = ca->fg_color.sixteen[0] << 8;
		fg |= ca->fg_color.sixteen[1];
		bg = ca->bg_color.sixteen[0] << 8;
		bg |= ca->bg_color.sixteen[1];
		for (i = 0; i < ca->height; i++) {
			fb16 = (uint16_t *)(fb + offset + i * pitch);
			for (j = 0; j < ca->width; j++) {
				fb16[j] = (fb16[j] ^ (fg & 0xffff)) ^
				    (bg & 0xffff);
			}
		}
		break;
	case 24:	/* 24 bit */
		fg = ca->fg_color.twentyfour[0] << 16;
		fg |= ca->fg_color.twentyfour[1] << 8;
		fg |= ca->fg_color.twentyfour[2];
		bg = ca->bg_color.twentyfour[0] << 16;
		bg |= ca->bg_color.twentyfour[1] << 8;
		bg |= ca->bg_color.twentyfour[2];

		for (i = 0; i < ca->height; i++) {
			fb8 = fb + offset + i * pitch;
			for (j = 0; j < size; j += 3) {
				fb8[j] = (fb8[j] ^ ((fg >> 16) & 0xff)) ^
				    ((bg >> 16) & 0xff);
				fb8[j+1] = (fb8[j+1] ^ ((fg >> 8) & 0xff)) ^
				    ((bg >> 8) & 0xff);
				fb8[j+2] = (fb8[j+2] ^ (fg & 0xff)) ^
				    (bg & 0xff);
			}
		}
		break;
	case 32:	/* 32 bit */
		fg = ca->fg_color.twentyfour[0] << 16;
		fg |= ca->fg_color.twentyfour[1] << 8;
		fg |= ca->fg_color.twentyfour[2];
		bg = ca->bg_color.twentyfour[0] << 16;
		bg |= ca->bg_color.twentyfour[1] << 8;
		bg |= ca->bg_color.twentyfour[2];
		for (i = 0; i < ca->height; i++) {
			fb32 = (uint32_t *)(fb + offset + i * pitch);
			for (j = 0; j < ca->width; j++)
				fb32[j] = (fb32[j] ^ fg) ^ bg;
		}
		break;
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

/*
 * Public graphics primitives.
 */

static int isqrt(int num) {
	int res = 0;
	int bit = 1 << 30;

	/* "bit" starts at the highest power of four <= the argument. */
	while (bit > num)
		bit >>= 2;

	while (bit != 0) {
		if (num >= res + bit) {
			num -= res + bit;
			res = (res >> 1) + bit;
		} else
			res >>= 1;
		bit >>= 2;
	}
	return (res);
}

/* set pixel in framebuffer using gfx coordinates */
void
gfx_fb_setpixel(int x, int y)
{
	uint32_t c, offset, pitch, bpp;
	uint8_t *fb;
	text_color_t fg, bg;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	tem_get_colors((tem_vt_state_t)tems.ts_active, &fg, &bg);
	c = gfx_fb_color_map(fg);

	if (x < 0 || y < 0)
		return;

	if (x >= gfx_fb.framebuffer_common.framebuffer_width ||
	    y >= gfx_fb.framebuffer_common.framebuffer_height)
		return;

	fb = gfx_get_fb_address();
	pitch = gfx_fb.framebuffer_common.framebuffer_pitch;
	bpp = roundup2(gfx_fb.framebuffer_common.framebuffer_bpp, 8) >> 3;

	offset = y * pitch + x * bpp;
	switch (gfx_fb.framebuffer_common.framebuffer_bpp) {
	case 8:
		fb[offset] = c & 0xff;
		break;
	case 15:
	case 16:
		*(uint16_t *)(fb + offset) = c & 0xffff;
		break;
	case 24:
		fb[offset] = (c >> 16) & 0xff;
		fb[offset + 1] = (c >> 8) & 0xff;
		fb[offset + 2] = c & 0xff;
		break;
	case 32:
		*(uint32_t *)(fb + offset) = c;
		break;
	}
}

/*
 * draw rectangle in framebuffer using gfx coordinates.
 * The function is borrowed from fbsd vt_fb.c
 */
void
gfx_fb_drawrect(int x1, int y1, int x2, int y2, int fill)
{
	int x, y;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	for (y = y1; y <= y2; y++) {
		if (fill || (y == y1) || (y == y2)) {
			for (x = x1; x <= x2; x++)
				gfx_fb_setpixel(x, y);
		} else {
			gfx_fb_setpixel(x1, y);
			gfx_fb_setpixel(x2, y);
		}
	}
}

void
gfx_fb_line(int x0, int y0, int x1, int y1, int width)
{
	int dx, sx, dy, sy;
	int err, e2, x2, y2, ed;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	sx = x0 < x1? 1:-1;
	sy = y0 < y1? 1:-1;
	dx = abs(x1 - x0);
	dy = abs(y1 - y0);
	err = dx + dy;
	ed = dx + dy == 0 ? 1: isqrt(dx * dx + dy * dy);

	for (;;) {
		gfx_fb_setpixel(x0, y0);
		e2 = err;
		x2 = x0;
		if ((e2 << 1) >= -dx) {		/* x step */
			e2 += dy;
			y2 = y0;
			while (e2 < ed * width && (y1 != y2 || dx > dy)) {
				y2 += sy;
				gfx_fb_setpixel(x0, y2);
				e2 += dx;
			}
			if (x0 == x1)
				break;
			e2 = err;
			err -= dy;
			x0 += sx;
		}
		if ((e2 << 1) <= dy) {		/* y step */
			e2 = dx-e2;
			while (e2 < ed * width && (x1 != x2 || dx < dy)) {
				x2 += sx;
				gfx_fb_setpixel(x2, y0);
				e2 += dy;
			}
			if (y0 == y1)
				break;
			err += dx;
			y0 += sy;
		}
	}
}

/*
 * quadratic Bézier curve limited to gradients without sign change.
 */
void
gfx_fb_bezier(int x0, int y0, int x1, int y1, int x2, int y2, int width)
{
	int sx, sy, xx, yy, xy;
	int dx, dy, err, curvature;
	int i;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	sx = x2 - x1;
	sy = y2 - y1;
	xx = x0 - x1;
	yy = y0 - y1;
	curvature = xx*sy - yy*sx;

	if (sx*sx + sy*sy > xx*xx+yy*yy) {
		x2 = x0;
		x0 = sx + x1;
		y2 = y0;
		y0 = sy + y1;
		curvature = -curvature;
	}
	if (curvature != 0) {
		xx += sx;
		sx = x0 < x2? 1:-1;
		xx *= sx;
		yy += sy;
		sy = y0 < y2? 1:-1;
		yy *= sy;
		xy = (xx*yy) << 1;
		xx *= xx;
		yy *= yy;
		if (curvature * sx * sy < 0) {
			xx = -xx;
			yy = -yy;
			xy = -xy;
			curvature = -curvature;
		}
		dx = 4 * sy * curvature * (x1 - x0) + xx - xy;
		dy = 4 * sx * curvature * (y0 - y1) + yy - xy;
		xx += xx;
		yy += yy;
		err = dx + dy + xy;
		do {
			for (i = 0; i <= width; i++)
				gfx_fb_setpixel(x0 + i, y0);
			if (x0 == x2 && y0 == y2)
				return;  /* last pixel -> curve finished */
			y1 = 2 * err < dx;
			if (2 * err > dy) {
				x0 += sx;
				dx -= xy;
				dy += yy;
				err += dy;
			}
			if (y1 != 0) {
				y0 += sy;
				dy -= xy;
				dx += xx;
				err += dx;
			}
		} while (dy < dx ); /* gradient negates -> algorithm fails */
	}
	gfx_fb_line(x0, y0, x2, y2, width);
}

/*
 * draw rectangle using terminal coordinates and current foreground color.
 */
void
gfx_term_drawrect(int row1, int col1, int row2, int col2)
{
	int x1, y1, x2, y2;
	int xshift, yshift;
	int width, i;
	uint32_t vf_width, vf_height;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	vf_width = tems.ts_font.vf_width;
	vf_height = tems.ts_font.vf_height;
	width = vf_width / 4;			/* line width */
	xshift = (vf_width - width) / 2;
	yshift = (vf_height - width) / 2;
	/* Terminal coordinates start from (1,1) */
	row1--;
	col1--;
	row2--;
	col2--;

	/*
	 * Draw horizontal lines width points thick, shifted from outer edge.
	 */
	x1 = row1 * vf_width + tems.ts_p_offset.x;
	x1 += vf_width;
	y1 = col1 * vf_height + tems.ts_p_offset.y + yshift;
	x2 = row2 * vf_width + tems.ts_p_offset.x;
	gfx_fb_drawrect(x1, y1, x2, y1 + width, 1);
	y2 = col2 * vf_height + tems.ts_p_offset.y;
	y2 += vf_height - yshift - width;
	gfx_fb_drawrect(x1, y2, x2, y2 + width, 1);

	/*
	 * Draw vertical lines width points thick, shifted from outer edge.
	 */
	x1 = row1 * vf_width + tems.ts_p_offset.x + xshift;
	y1 = col1 * vf_height + tems.ts_p_offset.y;
	y1 += vf_height;
	y2 = col2 * vf_height + tems.ts_p_offset.y;
	gfx_fb_drawrect(x1, y1, x1 + width, y2, 1);
	x1 = row2 * vf_width + tems.ts_p_offset.x;
	x1 += vf_width - xshift - width;
	gfx_fb_drawrect(x1, y1, x1 + width, y2, 1);

	/* Draw upper left corner. */
	x1 = row1 * vf_width + tems.ts_p_offset.x + xshift;
	y1 = col1 * vf_height + tems.ts_p_offset.y;
	y1 += vf_height;

	x2 = row1 * vf_width + tems.ts_p_offset.x;
	x2 += vf_width;
	y2 = col1 * vf_height + tems.ts_p_offset.y + yshift;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1 + i, y1, x1 + i, y2 + i, x2, y2 + i, width-i);

	/* Draw lower left corner. */
	x1 = row1 * vf_width + tems.ts_p_offset.x;
	x1 += vf_width;
	y1 = col2 * vf_height + tems.ts_p_offset.y;
	y1 += vf_height - yshift;
	x2 = row1 * vf_width + tems.ts_p_offset.x + xshift;
	y2 = col2 * vf_height + tems.ts_p_offset.y;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 - i, x2 + i, y1 - i, x2 + i, y2, width-i);

	/* Draw upper right corner. */
	x1 = row2 * vf_width + tems.ts_p_offset.x;
	y1 = col1 * vf_height + tems.ts_p_offset.y + yshift;
	x2 = row2 * vf_width + tems.ts_p_offset.x;
	x2 += vf_width - xshift - width;
	y2 = col1 * vf_height + tems.ts_p_offset.y;
	y2 += vf_height;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 + i, x2 + i, y1 + i, x2 + i, y2, width-i);

	/* Draw lower right corner. */
	x1 = row2 * vf_width + tems.ts_p_offset.x;
	y1 = col2 * vf_height + tems.ts_p_offset.y;
	y1 += vf_height - yshift;
	x2 = row2 * vf_width + tems.ts_p_offset.x;
	x2 += vf_width - xshift - width;
	y2 = col2 * vf_height + tems.ts_p_offset.y;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 - i, x2 + i, y1 - i, x2 + i, y2, width-i);
}

int
gfx_fb_putimage(png_t *png)
{
	struct vis_consdisplay da;
	uint32_t i, j, height, width, color;
	int bpp;
	uint8_t r, g, b, a, *p;

	if (plat_stdout_is_framebuffer() == 0 ||
	    png->color_type != PNG_TRUECOLOR_ALPHA) {
		return (1);
	}

	bpp = roundup2(gfx_fb.framebuffer_common.framebuffer_bpp, 8) >> 3;
	width = png->width;
	height = png->height;
	da.width = png->width;
	da.height = png->height;
	da.col = gfx_fb.framebuffer_common.framebuffer_width -
	    tems.ts_p_offset.x;
	da.col -= da.width;
	da.row = gfx_fb.framebuffer_common.framebuffer_height -
	    tems.ts_p_offset.y;
	da.row -= da.height;

	da.data = malloc(width * height * bpp);
	if (da.data == NULL)
		return (1);

	/*
	 * Build image for our framebuffer.
	 */
	for (i = 0; i < height * width * png->bpp; i += png->bpp) {
		r = png->image[i];
		g = png->image[i+1];
		b = png->image[i+2];
		a = png->image[i+3];

		j = i / png->bpp * bpp;
		color = r >> (8 - gfx_fb.u.fb2.framebuffer_red_mask_size)
		    << gfx_fb.u.fb2.framebuffer_red_field_position;
		color |= g >> (8 - gfx_fb.u.fb2.framebuffer_green_mask_size)
		    << gfx_fb.u.fb2.framebuffer_green_field_position;
		color |= b >> (8 - gfx_fb.u.fb2.framebuffer_blue_mask_size)
		    << gfx_fb.u.fb2.framebuffer_blue_field_position;

		switch (gfx_fb.framebuffer_common.framebuffer_bpp) {
		case 8: {
			uint32_t best, dist, k;
			int diff;

			color = 0;
			best = 256 * 256 * 256;
			for (k = 0; k < 16; k++) {
				diff = r - cmap4_to_24.red[k];
				dist = diff * diff;
				diff = g - cmap4_to_24.green[k];
				dist += diff * diff;
				diff = b - cmap4_to_24.blue[k];
				dist += diff * diff;

				if (dist < best) {
					color = k;
					best = dist;
					if (dist == 0)
						break;
				}
			}
			da.data[j] = color;
			break;
		}
		case 15:
		case 16:
			*(uint16_t *)(da.data+j) = color;
			break;
		case 24:
			p = (uint8_t *)&color;
			da.data[j] = p[0];
			da.data[j+1] = p[1];
			da.data[j+2] = p[2];
			break;
		case 32:
			color |= a << 24;
			*(uint32_t *)(da.data+j) = color;
			break;
		}
	}

	gfx_fb_cons_display(&da);
	free(da.data);
	return (0);
}

static int
load_mapping(int fd, struct font *fp, int n)
{
	int i;
	size_t size;
	struct font_map *mp;

	if (fp->vf_map_count[n] == 0)
		return (0);

	size = fp->vf_map_count[n] * sizeof (*mp);
	mp = malloc(size);
	if (mp == NULL)
		return (ENOMEM);
	fp->vf_map[n] = mp;

	if ((i = read(fd, mp, size)) != size) {
		return (EIO);
	}

	for (i = 0; i < fp->vf_map_count[n]; i++) {
		mp[i].font_src = be32toh(mp[i].font_src);
		mp[i].font_dst = be16toh(mp[i].font_dst);
		mp[i].font_len = be16toh(mp[i].font_len);
	}
	return (0);
}

/* Load font from file. */
static bitmap_data_t *
load_font(char *path)
{
	int fd, rc, i;
	uint32_t glyphs;
	struct font_header fh;
	struct fontlist *fl;
	bitmap_data_t *bp = NULL;
	struct font *fp;
	size_t size;

	/* Get our entry from the font list. */
	STAILQ_FOREACH(fl, &fonts, font_next) {
		if (strcmp(fl->font_name, path) == 0)
			break;
	}
	if (fl == NULL)
		return (NULL);	/* Should not happen. */
	bp = fl->font_data;
	if (bp->font != NULL)
		return (bp);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return (NULL);
	}

	if ((rc = read(fd, &fh, sizeof (fh))) != sizeof (fh))
		goto done;
	if (memcmp(fh.fh_magic, FONT_HEADER_MAGIC, sizeof (fh.fh_magic)) != 0)
		goto done;
	if ((fp = calloc(sizeof (struct font), 1)) == NULL) {
		bp = NULL;
		goto done;
	}
	for (i = 0; i < VFNT_MAPS; i++)
		fp->vf_map_count[i] = be32toh(fh.fh_map_count[i]);

	glyphs = be32toh(fh.fh_glyph_count);
	fp->vf_width = fh.fh_width;
	fp->vf_height = fh.fh_height;

	bp->uncompressed_size = howmany(bp->width, 8) * bp->height * glyphs;
	size = bp->uncompressed_size;
	if ((fp->vf_bytes = malloc(size)) == NULL)
		goto free_done;

	if ((i = read(fd, fp->vf_bytes, size)) != size)
		goto free_done;
	for (i = 0; i < VFNT_MAPS; i++) {
		if (load_mapping(fd, fp, i) != 0)
			goto free_done;
	}
	bp->font = fp;

	/*
	 * Release previously loaded entry. We can do this now, as
	 * the new font is loaded. Note, there can be no console
	 * output till the new font is in place and tem is notified.
	 * We do need to keep fl->font_data for glyph dimensions.
	 */
	STAILQ_FOREACH(fl, &fonts, font_next) {
		if (fl->font_data->width == bp->width &&
		    fl->font_data->height == bp->height)
			continue;

		if (fl->font_data->font != NULL) {
			/* The tem is releasing font bytes. */
			for (i = 0; i < VFNT_MAPS; i++)
				free(fl->font_data->font->vf_map[i]);
			free(fl->font_data->font);
			fl->font_data->font = NULL;
			fl->font_data->uncompressed_size = 0;
			fl->font_flags = FONT_AUTO;
		}
	}

done:
	close(fd);
	return (bp);

free_done:
	for (i = 0; i < VFNT_MAPS; i++)
		free(fp->vf_map[i]);
	free(fp->vf_bytes);
	free(fp);
	bp = NULL;
	goto done;
}


struct name_entry {
	char			*n_name;
	SLIST_ENTRY(name_entry)	n_entry;
};

SLIST_HEAD(name_list, name_entry);

/* Read font names from index file. */
static struct name_list *
read_list(char *fonts)
{
	struct name_list *nl;
	struct name_entry *np;
	char buf[PATH_MAX];
	int fd, len;

	fd = open(fonts, O_RDONLY);
	if (fd < 0)
		return (NULL);

	nl = malloc(sizeof (*nl));
	if (nl == NULL) {
		close(fd);
		return (nl);
	}

	SLIST_INIT(nl);
	while ((len = fgetstr(buf, sizeof (buf), fd)) > 0) {
		np = malloc(sizeof (*np));
		if (np == NULL) {
			close(fd);
			return (nl);    /* return what we have */
		}
		np->n_name = strdup(buf);
		if (np->n_name == NULL) {
			free(np);
			close(fd);
			return (nl);    /* return what we have */
		}
		SLIST_INSERT_HEAD(nl, np, n_entry);
	}
	close(fd);
	return (nl);
}

/*
 * Read the font properties and insert new entry into the list.
 * The font list is built in descending order.
 */
static bool
insert_font(char *name)
{
	struct font_header fh;
	struct fontlist *fp, *previous, *entry, *next;
	int fd, size;

	fd = open(name, O_RDONLY);
	if (fd < 0)
		return (false);
	size = read(fd, &fh, sizeof (fh));
	close(fd);
	if (size != sizeof (fh))
		return (false);

	if (memcmp(fh.fh_magic, FONT_HEADER_MAGIC, sizeof (fh.fh_magic)) != 0)
		return (false);

	/*
	 * If we have an entry with the same glyph dimensions, just replace
	 * the file name.
	 */
	STAILQ_FOREACH(entry, &fonts, font_next) {
		if (fh.fh_width == entry->font_data->width &&
		    fh.fh_height == entry->font_data->height) {
			free(entry->font_name);
			entry->font_name = name;
			return (true);
		}
	}

	fp = calloc(sizeof (*fp), 1);
	if (fp == NULL)
		return (false);
	fp->font_data = calloc(sizeof (*fp->font_data), 1);
	if (fp->font_data == NULL) {
		free(fp);
		return (false);
	}
	fp->font_name = name;
	fp->font_flags = FONT_AUTO;
	fp->font_load = load_font;
	fp->font_data->width = fh.fh_width;
	fp->font_data->height = fh.fh_height;

	if (STAILQ_EMPTY(&fonts)) {
		STAILQ_INSERT_HEAD(&fonts, fp, font_next);
		return (true);
	}

	previous = NULL;
	size = fp->font_data->width * fp->font_data->height;

	STAILQ_FOREACH(entry, &fonts, font_next) {
		/* Should fp be inserted before the entry? */
		if (size >
		    entry->font_data->width * entry->font_data->height) {
			if (previous == NULL) {
				STAILQ_INSERT_HEAD(&fonts, fp, font_next);
			} else {
				STAILQ_INSERT_AFTER(&fonts, previous, fp,
				    font_next);
			}
			return (true);
		}
		next = STAILQ_NEXT(entry, font_next);
		if (next == NULL ||
		    size > next->font_data->width * next->font_data->height) {
			STAILQ_INSERT_AFTER(&fonts, entry, fp, font_next);
			return (true);
		}
		previous = entry;
	}
	return (true);
}

static int
font_set(struct env_var *ev, int flags, const void *value)
{
	struct fontlist *fl, *tmp;
	char *eptr;
	unsigned long x = 0, y = 0;

	/*
	 * Attempt to extract values from "XxY" string. In case of error,
	 * we have unmaching glyph dimensions and will just output the
	 * available values.
	 */
	if (value != NULL) {
		x = strtoul(value, &eptr, 10);
		if (*eptr == 'x')
			y = strtoul(eptr + 1, &eptr, 10);
	}
	STAILQ_FOREACH(fl, &fonts, font_next) {
		if (fl->font_data->width == x && fl->font_data->height == y)
			break;
	}
	if (fl != NULL) {
		/* Reset any FONT_MANUAL flag. */
		STAILQ_FOREACH(tmp, &fonts, font_next)
			tmp->font_flags = FONT_AUTO;

		fl->font_flags = FONT_MANUAL;
		/* Trigger tem update. */
		tems.update_font = true;
		plat_cons_update_mode(-1);
		return (CMD_OK);
	}

	printf("Available fonts:\n");
	STAILQ_FOREACH(fl, &fonts, font_next) {
		printf("    %dx%d\n", fl->font_data->width,
		    fl->font_data->height);
	}
	return (CMD_OK);
}

void
autoload_font(void)
{
	struct name_list *nl;
	struct name_entry *np;

	nl = read_list("/boot/fonts/fonts.dir");
	if (nl == NULL)
		return;

	while (!SLIST_EMPTY(nl)) {
		np = SLIST_FIRST(nl);
		SLIST_REMOVE_HEAD(nl, n_entry);
		if (insert_font(np->n_name) == false)
			free(np->n_name);
		free(np);
	}

	unsetenv("screen-font");
	env_setenv("screen-font", EV_VOLATILE, NULL, font_set, env_nounset);
	/* Trigger tem update. */
	tems.update_font = true;
	plat_cons_update_mode(-1);
}

COMMAND_SET(load_font, "loadfont", "load console font from file", command_font);

static int
command_font(int argc, char *argv[])
{
	int i, rc = CMD_OK;
	struct fontlist *fl;
	bitmap_data_t *bd;

	if (argc > 2) {
		printf("Usage: loadfont [file.fnt]\n");
		return (CMD_ERROR);
	}

	if (argc == 2) {
		char *name = strdup(argv[1]);

		if (name == NULL) {
			printf("loadfont error: out of memory\n");
			return (CMD_ERROR);
		}

		if (insert_font(name) == false) {
			free(name);
			printf("loadfont error: loading failed\n");
			return (CMD_ERROR);
		}

		bd = load_font(argv[1]);
		if (bd == NULL) {
			printf("loadfont error: loading failed\n");
			return (CMD_ERROR);
		}

		/* Get the font list entry and mark it manually loaded. */
		STAILQ_FOREACH(fl, &fonts, font_next) {
			if (strcmp(fl->font_name, argv[1]) == 0)
				fl->font_flags = FONT_MANUAL;
		}
		tems.update_font = true;
		plat_cons_update_mode(-1);
		return (CMD_OK);
	}

	if (argc == 1) {
		/*
		 * Walk entire font list, release any loaded font, and set
		 * autoload flag. If the font list is empty, the tem will
		 * get the builtin default.
		 */
		STAILQ_FOREACH(fl, &fonts, font_next) {
			if (fl->font_data->font != NULL) {
				/* Note the tem is releasing font bytes */
				for (i = 0; i < VFNT_MAPS; i++)
					free(fl->font_data->font->vf_map[i]);
				free(fl->font_data->font);
				fl->font_data->font = NULL;
				fl->font_data->uncompressed_size = 0;
				fl->font_flags = FONT_AUTO;
			}
		}
		tems.update_font = true;
		plat_cons_update_mode(-1);
	}
	return (rc);
}
