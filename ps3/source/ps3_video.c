/*
 * Copyright (C) 2026 the ROTT-PS3 and EDuke32-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * PS3 display: everything goes through ps3_gpu.c, the RSX layer of the
 * 8 bit ports (CrispyCell, ECellWolf, ROTT-PS3), proven on hardware.
 *
 *  - The game: ps3layer.cpp hands over the Build engine's 8 bit screen
 *    (its internal resolution) and the faded palette; the RSX draws it
 *    through the palette, scaled to the 720p output inside the screen fit
 *    area, at the aspect of the video mode (square pixels).
 *  - Text screens (fatal errors, missing data): drawn by the CPU into a
 *    32 bit image.
 *
 * =======================================================================
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps3_platform.h"
#include "ps3_gpu.h"
#include "ps3_font.h"

static int fit_percent = 90;
static int filter_linear = 1;
static int perf_log;

/* the frame being built */
static int frame_open;

/* profiling, reset every 10 seconds */
static long long prof_copy_us;

/* ps3_gpu.c's log */
void
PS3_GPU_Log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	PS3_Log("%s", buf);
}

int
PS3_Video_Init(void)
{
	if (GPU_Ready())
	{
		return 0;
	}

	if (GPU_Init() != 0)
	{
		PS3_Log("[video] FATAL: the RSX did not start");
		return -1;
	}

	GPU_SetFit(fit_percent);
	GPU_SetFilter2D(filter_linear);
	GPU_SetPerfLog(perf_log);
	PS3_Log("[video] ready: %dx%d", GPU_Width(), GPU_Height());

	return 0;
}

void
PS3_Video_Shutdown(void)
{
	if (!GPU_Ready())
	{
		return;
	}

	PS3_Log("[video] shutdown");
	GPU_Shutdown();
	frame_open = 0;
}

int
PS3_Video_Ready(void)
{
	return GPU_Ready();
}

void
PS3_Video_SetFit(int percent)
{
	if (percent < 50)
	{
		percent = 50;
	}

	if (percent > 100)
	{
		percent = 100;
	}

	fit_percent = percent;
	GPU_SetFit(percent);
}

void
PS3_Video_SetFilter(int linear)
{
	filter_linear = linear ? 1 : 0;
	GPU_SetFilter2D(filter_linear);
}

void
PS3_Video_SetPerfLog(int on)
{
	perf_log = on ? 1 : 0;
	GPU_SetPerfLog(perf_log);
}

static void
CloseFrame(void)
{
	static int frames;
	static unsigned long long window_start;
	static int window_frames;
	unsigned long long now;

	GPU_EndFrame();
	frame_open = 0;

	if (frames < 3)
	{
		PS3_Log("[video] frame %d presented", frames);
	}

	frames++;

	/* frame rate in the log every 10 seconds (not more often: every write
	   to the HDD flashes the system's activity icon) */
	now = PS3_Micros();

	if (window_start == 0)
	{
		window_start = now;
	}

	window_frames++;

	if (now - window_start >= 10000000ull)
	{
		if (perf_log)
		{
			double ms = (double)(now - window_start) / 1000.0 / window_frames;

			PS3_Log("[video] %.1f fps: frame %.1f ms, copy %.1f ms",
					1000.0 / ms, ms, prof_copy_us / 1000.0 / window_frames);
		}

		window_start = now;
		window_frames = 0;
		prof_copy_us = 0;
	}
}

void
PS3_Video_SetPalette(const unsigned char *rgb)
{
	GPU_SetPalette(rgb);
}

static int
Gcd(int a, int b)
{
	while (b)
	{
		int t = a % b;

		a = b;
		b = t;
	}

	return a;
}

void
PS3_Video_Present8(const unsigned char *pixels, int w, int h, int aw, int ah)
{
	unsigned long long t0;
	int g;

	if (!GPU_Ready() || !pixels || w <= 0 || h <= 0)
	{
		return;
	}

	if (aw <= 0 || ah <= 0)
	{
		aw = w;
		ah = h;
	}

	g = Gcd(aw, ah);

	if (frame_open)
	{
		CloseFrame();
	}

	GPU_SetScreen(w, h, aw / g, ah / g);
	GPU_BeginFrame();
	frame_open = 1;

	t0 = PS3_Micros();
	GPU_DrawScreen(pixels, -1, 0, 0, 0, 0);
	prof_copy_us += (long long)(PS3_Micros() - t0);

	CloseFrame();
}

/* ================================================================ */
/* Text screen, drawn by the CPU into a 1280x720 image                */
/* ================================================================ */

#define TXT_W 1280
#define TXT_H 720
#define TXT_SCALE 2

static uint32_t *txt;

static void
TxtRect(int x, int y, int w, int h, uint32_t c)
{
	int i, j;

	if (x < 0)
	{
		w += x;
		x = 0;
	}

	if (y < 0)
	{
		h += y;
		y = 0;
	}

	if (x + w > TXT_W)
	{
		w = TXT_W - x;
	}

	if (y + h > TXT_H)
	{
		h = TXT_H - y;
	}

	for (j = 0; j < h; j++)
	{
		uint32_t *row = txt + (y + j) * TXT_W + x;

		for (i = 0; i < w; i++)
		{
			row[i] = c;
		}
	}
}

static void
TxtString(int x, int y, const char *str, uint32_t c)
{
	for (; *str; str++, x += 8 * TXT_SCALE)
	{
		unsigned ch = (unsigned char)*str;
		int gy, gx;

		if (ch < 32 || ch > 126)
		{
			ch = '?';
		}

		for (gy = 0; gy < 16; gy++)
		{
			unsigned bits = ps3_font8x16[ch - 32][gy];

			if (!bits)
			{
				continue;
			}

			for (gx = 0; gx < 8; gx++)
			{
				if (bits & (0x80u >> gx))
				{
					TxtRect(x + gx * TXT_SCALE, y + gy * TXT_SCALE, TXT_SCALE, TXT_SCALE, c);
				}
			}
		}
	}
}

void
PS3_Video_TextScreen(const char *title, const char *const *lines, int nlines)
{
	int i, y;

	if (!GPU_Ready())
	{
		return;
	}

	if (!txt)
	{
		txt = (uint32_t *)malloc(TXT_W * TXT_H * 4);

		if (!txt)
		{
			return;
		}
	}

	if (frame_open)
	{
		/* a frame half built when the error came: it goes as it is */
		CloseFrame();
	}

	TxtRect(0, 0, TXT_W, TXT_H, 0xff000000u);
	y = TXT_H / 5;

	if (title)
	{
		int tw = (int)strlen(title) * 8 * TXT_SCALE;

		TxtString((TXT_W - tw) / 2, y, title, 0xffd02020u);
		y += 16 * TXT_SCALE * 2;
	}

	for (i = 0; i < nlines; i++)
	{
		if (lines[i])
		{
			TxtString(TXT_W / 10, y, lines[i], 0xffe0e0e0u);
		}

		y += 16 * TXT_SCALE + 8;
	}

	GPU_SetClearColor(0xff000000u);
	GPU_SetScreen(TXT_W, TXT_H, 16, 9);
	GPU_BeginFrame();
	frame_open = 1;
	GPU_DrawScreen32(txt);
	CloseFrame();
	GPU_Finish();

	PS3_Log("[video] text screen: %s", title ? title : "");
}
