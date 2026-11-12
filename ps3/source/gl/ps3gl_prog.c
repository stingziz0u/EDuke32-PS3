/*
 * Copyright (C) 2026 the EDuke32-PS3 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * =======================================================================
 *
 * ps3gl: what EDuke32's Polymost needs on top of OpenGL 1.3.
 *
 *  - GLSL program and shader objects. There is no GLSL compiler on the
 *    PS3: a program whose fragment shader is Polymost's (polymost1Frag,
 *    told apart by its uniforms) runs as the Cg programs written for it,
 *    shaders/polymost_vp.vcg and polymost_fp.fcg, compiled into
 *    ps3gl_polymost_data.h. Uniforms are found by name and kept per
 *    program, like GL does; glUseProgram(0) is the fixed function again.
 *
 *  - Buffer objects, in main memory: every draw copies its vertices into
 *    the vertex ring anyway (ps3gl_draw.c), so a buffer is only a place
 *    the pointers can point into.
 *
 * The fragment program's uniforms go through the vertex program (see
 * polymost_vp.vcg): the RSX keeps fragment program constants inside the
 * program's code, vertex program constants are a few words in the command
 * buffer. Only the constants that changed since the last draw are sent.
 *
 * =======================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ps3gl.h"
#include "ps3gl_polymost_data.h"
#include "../ps3_platform.h"

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_VERTEX_SHADER        0x8B31
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_INFO_LOG_LENGTH      0x8B84
#define GL_SHADER_TYPE          0x8B4F
#endif

/* ================================================================ */
/* Shader and program objects                                         */
/* ================================================================ */

#define MAX_SHADERS     32
#define MAX_PROGRAMS    16

enum { KIND_UNKNOWN, KIND_POLYMOST_VERT, KIND_POLYMOST_FRAG };

/* The uniforms of Polymost's shaders (polymost1Vert/Frag.glsl): their
   index here is their location. */
enum {
	U_S_TEXTURE, U_S_PALSWAP, U_S_PALETTE, U_S_DETAIL, U_S_GLOW,
	U_TEXTUREPOSSIZE, U_HALFTEXELSIZE, U_PALSWAPPOS, U_PALSWAPSIZE, U_CLAMP,
	U_SHADE, U_NUMSHADES, U_VISFACTOR, U_FOGENABLED, U_USECOLORONLY,
	U_USEPALETTE, U_USEDETAILMAPPING, U_USEGLOWMAPPING, U_NPOTEMULATION,
	U_ROTMATRIX, U_SHADEINTERPOLATE, U_COLORCORRECTION,
	U_COUNT
};

static const char *const uniform_names[U_COUNT] = {
	"s_texture", "s_palswap", "s_palette", "s_detail", "s_glow",
	"u_texturePosSize", "u_halfTexelSize", "u_palswapPos", "u_palswapSize", "u_clamp",
	"u_shade", "u_numShades", "u_visFactor", "u_fogEnabled", "u_useColorOnly",
	"u_usePalette", "u_useDetailMapping", "u_useGlowMapping", "u_npotEmulation",
	"u_rotMatrix", "u_shadeInterpolate", "u_colorCorrection",
};

typedef struct {
	int     used;
	GLenum  type;
	int     kind;
} shader_t;

typedef struct {
	int     used;
	int     vert, frag;         /* kinds attached */
	int     polymost;           /* linked as Polymost's program */
	float   u[U_COUNT][16];
} program_t;

static shader_t shaders[MAX_SHADERS];
static program_t programs[MAX_PROGRAMS];

static rsxVertexProgram *pm_vp;
static void *pm_vp_ucode;
static rsxFragmentProgram *pm_fp;
static void *pm_fp_ucode;           /* copy in RSX memory */
static u32 pm_fp_offset;

/* the vertex program's constants m0 .. u7, as polymost_vp.vcg declares
   them: 16 float4 */
#define PM_CONSTS 16
static const char *const pm_const_names[PM_CONSTS] = {
	"m0", "m1", "m2", "m3", "e2", "e3", "t0", "t1", "fog",
	"u1", "u2", "u3", "u4", "u5", "u6", "u7",
};
static rsxProgramConst *pm_const[PM_CONSTS];
static int pm_const_base = -1;      /* contiguous from here: one block */
static float pm_sent[PM_CONSTS][4];
static int pm_sent_valid;

static void
ProgramDefaults(program_t *p)
{
	int i;

	memset(p->u, 0, sizeof(p->u));

	/* what Polymost sets anyway, in case a draw comes first */
	for (i = 0; i < 4; i++)
	{
		p->u[U_ROTMATRIX][i * 5] = 1.0f;
	}

	p->u[U_COLORCORRECTION][0] = p->u[U_COLORCORRECTION][1] = p->u[U_COLORCORRECTION][2] = 1.0f;
	p->u[U_NPOTEMULATION][1] = p->u[U_NPOTEMULATION][3] = 1.0f;
	p->u[U_NUMSHADES][0] = 1.0f;
	p->u[U_NUMSHADES][1] = 1.0f;
	p->u[U_TEXTUREPOSSIZE][2] = p->u[U_TEXTUREPOSSIZE][3] = 1.0f;
}

void
ps3gl_prog_init(void)
{
	void *ucode;
	u32 size;
	int i, contiguous = 1;

	pm_vp = (rsxVertexProgram *)polymost_vp_vpo;
	rsxVertexProgramGetUCode(pm_vp, &pm_vp_ucode, &size);

	for (i = 0; i < PM_CONSTS; i++)
	{
		pm_const[i] = rsxVertexProgramGetConst(pm_vp, pm_const_names[i]);

		if (!pm_const[i] || (int)pm_const[i]->index != (int)pm_const[0]->index + i)
		{
			contiguous = 0;
		}
	}

	/* (the registers are the program's const_start on, as
	   rsxSetVertexProgramParameter counts them) */
	pm_const_base = (contiguous && pm_const[0]) ?
		(int)pm_vp->const_start + (int)pm_const[0]->index : -1;

	pm_fp = (rsxFragmentProgram *)polymost_fp_fpo;
	rsxFragmentProgramGetUCode(pm_fp, &ucode, &size);
	pm_fp_ucode = rsxMemalign(64, size);

	if (pm_fp_ucode)
	{
		memcpy(pm_fp_ucode, ucode, size);
		rsxAddressToOffset(pm_fp_ucode, &pm_fp_offset);
	}

	PS3_Log("[gl] Polymost programs: vertex constants %s (base %d), fragment program %u bytes%s",
			contiguous ? "in one block" : "one by one", pm_const_base, (unsigned)size,
			pm_fp_ucode ? "" : " -- NO RSX MEMORY");
}

GLuint APIENTRY
glCreateShader(GLenum type)
{
	int i;

	for (i = 1; i < MAX_SHADERS; i++)
	{
		if (!shaders[i].used)
		{
			shaders[i].used = 1;
			shaders[i].type = type;
			shaders[i].kind = KIND_UNKNOWN;
			return (GLuint)i;
		}
	}

	PS3_Log("[gl] WARNING: out of shader objects");
	return 0;
}

void APIENTRY
glDeleteShader(GLuint shader)
{
	if (shader > 0 && shader < MAX_SHADERS)
	{
		memset(&shaders[shader], 0, sizeof(shaders[shader]));
	}
}

GLboolean APIENTRY
glIsShader(GLuint shader)
{
	return (shader > 0 && shader < MAX_SHADERS && shaders[shader].used) ? GL_TRUE : GL_FALSE;
}

/* Which shader it is, from its text (the whole of it, as the strings
   given) */
void APIENTRY
glShaderSource(GLuint shader, GLsizei count, const char *const *string, const GLint *length)
{
	shader_t *s;
	int i, has_palswap = 0, has_rot = 0;

	if (shader == 0 || shader >= MAX_SHADERS || !shaders[shader].used || !string)
	{
		return;
	}

	s = &shaders[shader];

	for (i = 0; i < count; i++)
	{
		const char *text = string[i];
		size_t len;
		char *copy;

		if (!text)
		{
			continue;
		}

		len = (length && length[i] >= 0) ? (size_t)length[i] : strlen(text);
		copy = (char *)malloc(len + 1);

		if (!copy)
		{
			continue;
		}

		memcpy(copy, text, len);
		copy[len] = 0;

		if (strstr(copy, "u_palswapPos"))
		{
			has_palswap = 1;
		}

		if (strstr(copy, "u_rotMatrix"))
		{
			has_rot = 1;
		}

		free(copy);
	}

	if (s->type == GL_FRAGMENT_SHADER && has_palswap)
	{
		s->kind = KIND_POLYMOST_FRAG;
	}
	else if (s->type == GL_VERTEX_SHADER && has_rot)
	{
		s->kind = KIND_POLYMOST_VERT;
	}
	else
	{
		s->kind = KIND_UNKNOWN;
		PS3_Log("[gl] a %s shader ps3gl doesn't know: not run",
				s->type == GL_FRAGMENT_SHADER ? "fragment" : "vertex");
	}
}

void APIENTRY
glCompileShader(GLuint shader)
{
	(void)shader;
}

void APIENTRY
glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
	if (!params)
	{
		return;
	}

	switch (pname)
	{
		case GL_COMPILE_STATUS:
			*params = (shader > 0 && shader < MAX_SHADERS &&
				shaders[shader].kind != KIND_UNKNOWN) ? GL_TRUE : GL_FALSE;
			break;
		case GL_SHADER_TYPE:
			*params = (shader > 0 && shader < MAX_SHADERS) ? (GLint)shaders[shader].type : 0;
			break;
		default:
			*params = 0;   /* GL_INFO_LOG_LENGTH: no log */
			break;
	}
}

void APIENTRY
glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, char *infoLog)
{
	(void)shader;

	if (length)
	{
		*length = 0;
	}

	if (infoLog && bufSize > 0)
	{
		infoLog[0] = 0;
	}
}

GLuint APIENTRY
glCreateProgram(void)
{
	int i;

	for (i = 1; i < MAX_PROGRAMS; i++)
	{
		if (!programs[i].used)
		{
			memset(&programs[i], 0, sizeof(programs[i]));
			programs[i].used = 1;
			ProgramDefaults(&programs[i]);
			return (GLuint)i;
		}
	}

	PS3_Log("[gl] WARNING: out of program objects");
	return 0;
}

void APIENTRY
glDeleteProgram(GLuint program)
{
	if (program > 0 && program < MAX_PROGRAMS)
	{
		memset(&programs[program], 0, sizeof(programs[program]));

		if (ps3gl.program == program)
		{
			ps3gl.program = 0;
			ps3gl.program_polymost = 0;
		}
	}
}

GLboolean APIENTRY
glIsProgram(GLuint program)
{
	return (program > 0 && program < MAX_PROGRAMS && programs[program].used) ? GL_TRUE : GL_FALSE;
}

void APIENTRY
glAttachShader(GLuint program, GLuint shader)
{
	if (program == 0 || program >= MAX_PROGRAMS || shader == 0 || shader >= MAX_SHADERS)
	{
		return;
	}

	if (shaders[shader].type == GL_FRAGMENT_SHADER)
	{
		programs[program].frag = shaders[shader].kind;
	}
	else
	{
		programs[program].vert = shaders[shader].kind;
	}
}

void APIENTRY
glDetachShader(GLuint program, GLuint shader)
{
	(void)program;
	(void)shader;   /* the program keeps what it was linked with */
}

void APIENTRY
glLinkProgram(GLuint program)
{
	program_t *p;

	if (program == 0 || program >= MAX_PROGRAMS)
	{
		return;
	}

	p = &programs[program];
	p->polymost = (p->frag == KIND_POLYMOST_FRAG);

	PS3_Log("[gl] program %u linked: %s", (unsigned)program,
			p->polymost ? "Polymost's (Cg on the RSX)" : "unknown, draws with the fixed function");
}

void APIENTRY
glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
	if (!params)
	{
		return;
	}

	*params = (pname == GL_LINK_STATUS && program > 0 && program < MAX_PROGRAMS &&
		programs[program].polymost) ? GL_TRUE : 0;
}

void APIENTRY
glUseProgram(GLuint program)
{
	if (program >= MAX_PROGRAMS || !programs[program].used)
	{
		program = 0;
	}

	ps3gl.program = program;
	ps3gl.program_polymost = program && programs[program].polymost;
}

GLint APIENTRY
glGetUniformLocation(GLuint program, const char *name)
{
	int i;

	(void)program;

	if (!name)
	{
		return -1;
	}

	for (i = 0; i < U_COUNT; i++)
	{
		if (!strcmp(name, uniform_names[i]))
		{
			return i;
		}
	}

	return -1;
}

static float *
Uniform(GLint location)
{
	if (location < 0 || location >= U_COUNT || !ps3gl.program)
	{
		return NULL;
	}

	return programs[ps3gl.program].u[location];
}

void APIENTRY
glUniform1f(GLint location, GLfloat v0)
{
	float *u = Uniform(location);

	if (u)
	{
		u[0] = v0;
	}
}

void APIENTRY
glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
	float *u = Uniform(location);

	if (u)
	{
		u[0] = v0;
		u[1] = v1;
	}
}

void APIENTRY
glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)
{
	float *u = Uniform(location);

	if (u)
	{
		u[0] = v0;
		u[1] = v1;
		u[2] = v2;
		u[3] = v3;
	}
}

void APIENTRY
glUniform2fv(GLint location, GLsizei count, const GLfloat *v)
{
	if (count > 0 && v)
	{
		glUniform2f(location, v[0], v[1]);
	}
}

void APIENTRY
glUniform4fv(GLint location, GLsizei count, const GLfloat *v)
{
	if (count > 0 && v)
	{
		glUniform4f(location, v[0], v[1], v[2], v[3]);
	}
}

/* Samplers: the Cg program has them on fixed units (0, 1, 2), the ones
   Polymost gives them. */
void APIENTRY
glUniform1i(GLint location, GLint v0)
{
	glUniform1f(location, (GLfloat)v0);
}

void APIENTRY
glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	float *u = Uniform(location);
	int r, c;

	if (!u || count <= 0 || !value)
	{
		return;
	}

	if (!transpose)
	{
		memcpy(u, value, 16 * sizeof(float));
		return;
	}

	for (c = 0; c < 4; c++)
	{
		for (r = 0; r < 4; r++)
		{
			u[c * 4 + r] = value[r * 4 + c];
		}
	}
}

/* ---------------------------------------------------------------- */

/* out = a * b, column major */
static void
Mul(float *out, const float *a, const float *b)
{
	int c, r;

	for (c = 0; c < 4; c++)
	{
		for (r = 0; r < 4; r++)
		{
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
				a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
		}
	}
}

static void
Row(float *dst, const float *m, int r)
{
	dst[0] = m[r];
	dst[1] = m[4 + r];
	dst[2] = m[8 + r];
	dst[3] = m[12 + r];
}

static void
Set4(float *dst, float a, float b, float c, float d)
{
	dst[0] = a;
	dst[1] = b;
	dst[2] = c;
	dst[3] = d;
}

/* Polymost's programs on the RSX and their constants for this draw */
void
ps3gl_prog_apply(void)
{
	gcmContextData *ctx = ps3gl.ctx;
	const program_t *p = &programs[ps3gl.program];
	float k[PM_CONSTS][4];
	float mvr[16], pmvr[16];
	float range;
	int first, last, i;

	if (ps3gl.vp_loaded != PS3GL_VP_POLYMOST)
	{
		rsxLoadVertexProgram(ctx, pm_vp, pm_vp_ucode);
		ps3gl.vp_loaded = PS3GL_VP_POLYMOST;
		ps3gl.mvp_uploaded = 0;   /* the fixed one's mvp is overwritten */
		pm_sent_valid = 0;
	}

	if (ps3gl.active_fp != PS3GL_FP_POLYMOST && pm_fp_ucode)
	{
		rsxLoadFragmentProgramLocation(ctx, pm_fp, pm_fp_offset, GCM_LOCATION_RSX);
		ps3gl.active_fp = PS3GL_FP_POLYMOST;
	}

	/* matrices: (projection * modelview * u_rotMatrix), rows */
	Mul(mvr, ps3gl.mv.stack[ps3gl.mv.depth], p->u[U_ROTMATRIX]);
	Mul(pmvr, ps3gl.proj.stack[ps3gl.proj.depth], mvr);

	for (i = 0; i < 4; i++)
	{
		Row(k[i], pmvr, i);
	}

	Row(k[4], mvr, 2);
	Row(k[5], mvr, 3);
	Row(k[6], ps3gl.tex.stack[ps3gl.tex.depth], 0);
	Row(k[7], ps3gl.tex.stack[ps3gl.tex.depth], 1);

	range = ps3gl.fog_end - ps3gl.fog_start;
	Set4(k[8], ps3gl.fog_end, (fabsf(range) > 1e-6f) ? 1.0f / range : 1e6f,
			p->u[U_FOGENABLED][0] != 0.0f ? 1.0f : 0.0f, 0.0f);

	/* the fragment shader's uniforms, as polymost_fp.fcg unpacks them */
	memcpy(k[9], p->u[U_TEXTUREPOSSIZE], 4 * sizeof(float));
	Set4(k[10], p->u[U_HALFTEXELSIZE][0], p->u[U_HALFTEXELSIZE][1],
			p->u[U_CLAMP][0], p->u[U_CLAMP][1]);
	Set4(k[11], p->u[U_PALSWAPPOS][0], p->u[U_PALSWAPPOS][1],
			p->u[U_PALSWAPSIZE][0], p->u[U_PALSWAPSIZE][1]);
	Set4(k[12], p->u[U_SHADE][0], p->u[U_NUMSHADES][0], p->u[U_NUMSHADES][1],
			p->u[U_VISFACTOR][0]);
	Set4(k[13], p->u[U_COLORCORRECTION][0] - 1.0f, p->u[U_USECOLORONLY][0],
			p->u[U_USEPALETTE][0], p->u[U_SHADEINTERPOLATE][0]);
	Set4(k[14], p->u[U_NPOTEMULATION][0],
			p->u[U_NPOTEMULATION][1] != 0.0f ? p->u[U_NPOTEMULATION][1] : 1.0f,
			p->u[U_NPOTEMULATION][2], p->u[U_COLORCORRECTION][1]);
	Set4(k[15], ps3gl.fog_color[0], ps3gl.fog_color[1], ps3gl.fog_color[2],
			p->u[U_COLORCORRECTION][2]);

	/* only what changed since the last draw */
	first = 0;
	last = PM_CONSTS - 1;

	if (pm_sent_valid)
	{
		while (first < PM_CONSTS && !memcmp(k[first], pm_sent[first], sizeof(k[0])))
		{
			first++;
		}

		if (first == PM_CONSTS)
		{
			return;
		}

		while (last > first && !memcmp(k[last], pm_sent[last], sizeof(k[0])))
		{
			last--;
		}
	}

	if (pm_const_base >= 0)
	{
		rsxLoadVertexProgramParameterBlock(ctx, (u32)(pm_const_base + first),
				(u32)(last - first + 1), &k[first][0]);
	}
	else
	{
		for (i = first; i <= last; i++)
		{
			if (pm_const[i])
			{
				rsxSetVertexProgramParameter(ctx, pm_vp, pm_const[i], k[i]);
			}
		}
	}

	memcpy(&pm_sent[first][0], &k[first][0], (size_t)(last - first + 1) * sizeof(k[0]));
	pm_sent_valid = 1;
}

/* ================================================================ */
/* Buffer objects                                                     */
/* ================================================================ */

typedef struct {
	uint8_t *data;
	size_t   size, capacity;
	int      used;
} buffer_t;

static buffer_t *buffers;
static GLuint num_buffers;          /* entries in buffers[] */

static buffer_t *
Buffer(GLuint name)
{
	return (name > 0 && name < num_buffers && buffers[name].used) ? &buffers[name] : NULL;
}

static GLuint *
Binding(GLenum target)
{
	switch (target)
	{
		case GL_ARRAY_BUFFER:         return &ps3gl.array_buffer;
		case GL_ELEMENT_ARRAY_BUFFER: return &ps3gl.element_buffer;
		default:                      return NULL;
	}
}

static buffer_t *
Bound(GLenum target)
{
	GLuint *b = Binding(target);

	return b ? Buffer(*b) : NULL;
}

const void *
ps3gl_buffer_ptr(GLuint vbo, const void *offset)
{
	buffer_t *b = Buffer(vbo);

	if (!b || !b->data || (size_t)(uintptr_t)offset >= b->size)
	{
		return NULL;
	}

	return b->data + (uintptr_t)offset;
}

/* bytes in buffer object vbo, -1 if there is no such buffer */
long
ps3gl_buffer_size(GLuint vbo)
{
	buffer_t *b = Buffer(vbo);

	return (b && b->data) ? (long)b->size : -1;
}

const void *
ps3gl_element_ptr(const void *indices)
{
	if (!ps3gl.element_buffer)
	{
		return indices;
	}

	return ps3gl_buffer_ptr(ps3gl.element_buffer, indices);
}

void APIENTRY
glGenBuffers(GLsizei n, GLuint *names)
{
	GLuint name = 1;
	int i;

	for (i = 0; i < n; i++)
	{
		while (name < num_buffers && buffers[name].used)
		{
			name++;
		}

		if (name >= num_buffers)
		{
			GLuint grow = num_buffers ? num_buffers * 2 : 64;
			buffer_t *nb = (buffer_t *)realloc(buffers, grow * sizeof(buffer_t));

			if (!nb)
			{
				names[i] = 0;
				continue;
			}

			memset(nb + num_buffers, 0, (grow - num_buffers) * sizeof(buffer_t));
			buffers = nb;
			num_buffers = grow;
		}

		buffers[name].used = 1;
		names[i] = name++;
	}
}

void APIENTRY
glDeleteBuffers(GLsizei n, const GLuint *names)
{
	int i;

	for (i = 0; i < n; i++)
	{
		buffer_t *b = Buffer(names[i]);

		if (!b)
		{
			continue;
		}

		free(b->data);
		memset(b, 0, sizeof(*b));

		if (ps3gl.array_buffer == names[i])
		{
			ps3gl.array_buffer = 0;
		}

		if (ps3gl.element_buffer == names[i])
		{
			ps3gl.element_buffer = 0;
		}
	}
}

GLboolean APIENTRY
glIsBuffer(GLuint name)
{
	return Buffer(name) ? GL_TRUE : GL_FALSE;
}

void APIENTRY
glBindBuffer(GLenum target, GLuint name)
{
	GLuint *b = Binding(target);

	if (b)
	{
		*b = Buffer(name) ? name : 0;
	}
}

/* The storage only grows: a new glBufferData with the same size (EDuke32
   "orphans" its stream buffer that way) keeps the memory. Nothing the RSX
   reads is in here, so overwriting is always safe. */
void APIENTRY
glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage)
{
	buffer_t *b = Bound(target);

	(void)usage;

	if (!b || size < 0)
	{
		return;
	}

	if ((size_t)size > b->capacity)
	{
		uint8_t *nd = (uint8_t *)realloc(b->data, (size_t)size);

		if (!nd)
		{
			PS3_Log("[gl] WARNING: no memory for a %ld byte buffer object", (long)size);
			return;
		}

		b->data = nd;
		b->capacity = (size_t)size;
	}

	b->size = (size_t)size;

	/* GL leaves new storage undefined; zeroed here, which also shows at
	   once (crumb) if this memory can't be written */
	if (!data && size)
	{
		PS3_Crumb("bufdata zero", (int)size);
		memset(b->data, 0, (size_t)size);
	}

	{
		static int logged;

		if (logged < 16)
		{
			PS3_Log("[gl] buffer %u (target 0x%x): %ld bytes at %p%s", (unsigned)*Binding(target),
					(unsigned)target, (long)size, (void *)b->data, data ? "" : " (no data)");
			logged++;
		}
	}

	if (data && size)
	{
		memcpy(b->data, data, (size_t)size);
	}
}

void APIENTRY
glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const GLvoid *data)
{
	buffer_t *b = Bound(target);

	if (!b || !data || offset < 0 || size <= 0 || (size_t)(offset + size) > b->size)
	{
		return;
	}

	memcpy(b->data + offset, data, (size_t)size);
}

void * APIENTRY
glMapBuffer(GLenum target, GLenum access)
{
	buffer_t *b = Bound(target);

	(void)access;

	return b ? b->data : NULL;
}

void * APIENTRY
glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
	buffer_t *b = Bound(target);

	(void)length;
	(void)access;

	return (b && b->data && (size_t)offset < b->size) ? b->data + offset : NULL;
}

GLboolean APIENTRY
glUnmapBuffer(GLenum target)
{
	(void)target;
	return GL_TRUE;
}

/* ================================================================ */
/* Queries ps3gl answers with "nothing"                               */
/* ================================================================ */

/* texcache.cpp asks (the texture cache is off: no compression) */
void APIENTRY
glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
	(void)target;
	(void)level;
	(void)pname;

	if (params)
	{
		*params = 0;
	}
}

void APIENTRY
glGenSamplers(GLsizei n, GLuint *samplers)
{
	int i;

	for (i = 0; samplers && i < n; i++)
	{
		samplers[i] = 0;
	}
}
