/*
 * mlt_glsl.c
 * Copyright (C) 2011 Christophe Thommeret
 * Author: Christophe Thommeret <hftom@free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "mlt_glsl.h"

#include <framework/mlt_properties.h>
#include <framework/mlt_factory.h>
#include <framework/mlt_filter.h>
#include <framework/mlt_log.h>

#include <string.h>
#include <math.h>

// XXX: GL_RGBA32F is a part of <OpenGL/gl3.h> on OS X, but we are not supposed to include it - only gl.h.
#ifndef GL_RGBA32F
#define GL_RGBA32F (0x8814)
#endif

////// unsorted list //////////////////////////////////
static int list_add( glsl_list list, void *p )
{
	int good = 1;
	if ( list->count < MAXLISTCOUNT )
		list->items[list->count++] = p;
	else
		good = 0;

	return good;
}

static void* list_at( glsl_list list, int index )
{
	return (index < list->count) ? list->items[index] : NULL;
}

static void* list_take_at( glsl_list list, int index )
{
	void* ret = NULL;
	int j;
	if ( (ret = list_at( list, index )) ) {
		for ( j=index; j<list->count-1; ++j )
			list->items[j] = list->items[j+1];
		--list->count;
	}
	return ret;
}

static void* list_take( glsl_list list, void *p )
{
	void* ret = NULL;
	int i;
	for ( i=0; i<list->count; ++i ) {
		if ( list->items[i] == p )
			return list_take_at( list, i );
	}
	return ret;
}

static glsl_list glsl_list_create()
{
	glsl_list list = calloc( sizeof( struct glsl_list_s ), 1 );
	if ( list ) {
		list->count = 0;
		list->add = list_add;
		list->at = list_at;
		list->take_at = list_take_at;
		list->take = list_take;
	}
	return list;
}
////// unsorted list //////////////////////////////////



static const char *filter_bicubic_pass1_frag=
"#extension GL_ARB_texture_rectangle : enable\n"
"uniform sampler2DRect tex, lut;\n"
"uniform float spline;\n"
"void main() {\n"
"    vec2 coord = gl_TexCoord[0].xy;\n"
"    vec2 TexCoord = vec2( floor( coord.x - 0.5 ) + 0.5, coord.y );\n"
"    vec4 sum = vec4( 0.0 );\n"
"    float coefsum = 0.0;\n"
"    mat4 wlut;\n"
"    wlut[0] = texture2DRect( lut, vec2( abs( coord.x - TexCoord.x ) * 1000.0, spline ) );\n"
"    for( int x = -1; x <= 2; x++ ) {\n"
"        vec4 col = texture2DRect( tex, TexCoord + vec2( float( x ), 0.0) );\n"
"        sum += col * wlut[0][x+1];\n"
"        coefsum += wlut[0][x+1];\n"
"    }\n"
"    gl_FragColor = sum / coefsum;\n"
"}\n";


static const char *filter_bicubic_pass2_frag=
"#extension GL_ARB_texture_rectangle : enable\n"
"uniform sampler2DRect tex, lut;\n"
"uniform float spline;\n"
"void main() {\n"
"    vec2 coord = gl_TexCoord[0].xy;\n"
"    vec2 TexCoord = vec2( coord.x, floor( coord.y - 0.5 ) + 0.5 );\n"
"    vec4 sum = vec4( 0.0 );\n"
"    float coefsum = 0.0;\n"
"    mat4 wlut;\n"
"    wlut[0] = texture2DRect( lut, vec2( abs( coord.y - TexCoord.y ) * 1000.0, spline ) );\n"
"    for( int y = -1; y <= 2; y++ ) {\n"
"        vec4 col = texture2DRect( tex, TexCoord + vec2( 0.0, float( y ) ) );\n"
"        sum += col * wlut[0][y+1];\n"
"        coefsum += wlut[0][y+1];\n"
"    }\n"
"    gl_FragColor = sum / coefsum;\n"
"}\n";



#define PI 3.14159265359
#define LUTWIDTH 1000

static float compute_cos_spline( float x )
{
	if ( x < 0.0 )
		x = -x;
	return 0.5 * cos( PI * x / 2.0 ) + 0.5;
}



static float compute_catmullrom_spline( float x )
{
	if ( x < 0.0 )
		x = -x;
	if ( x < 1.0 )
		return ((9.0 * (x * x * x)) - (15.0 * (x * x)) + 6.0) / 6.0;
	if ( x <= 2.0 )
		return ((-3.0 * (x * x * x)) + (15.0 * (x * x)) - (24.0 * x) + 12.0) / 6.0;
	return 0.0;
}



static void create_lut_texture( glsl_env g )
{
	int i = 0;
	float *lut = calloc( sizeof(float) * LUTWIDTH * 4 * N_SPLINES, 1 );
	float t;
	while ( i < LUTWIDTH ) {
		t = (float)i / (float)LUTWIDTH;

		lut[i * 4] = compute_catmullrom_spline( t + 1.0 );
		lut[(i * 4) + 1] = compute_catmullrom_spline( t );
		lut[(i * 4) + 2] = compute_catmullrom_spline( t - 1.0 );
		lut[(i * 4) + 3] = compute_catmullrom_spline( t - 2.0 );

		lut[(i * 4) + (LUTWIDTH * 4)] = compute_cos_spline( t + 1.0 );
		lut[(i * 4) + (LUTWIDTH * 4) + 1] = compute_cos_spline( t );
		lut[(i * 4) + (LUTWIDTH * 4) + 2] = compute_cos_spline( t - 1.0 );
		lut[(i * 4) + (LUTWIDTH * 4) + 3] = compute_cos_spline( t - 2.0 );

		++i;
	}

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	if ( !tex )
		return;

	glsl_texture gtex = calloc( sizeof( struct glsl_texture_s ), 1 );
	if ( !gtex ) {
		glDeleteTextures( 1, &tex );
		return;
	}

	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, tex );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA32F, LUTWIDTH, N_SPLINES, 0, GL_RGBA, GL_FLOAT, lut );
	free( lut );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, 0 );

	gtex->parent = g;
	gtex->texture = tex;
	gtex->width = LUTWIDTH;
	gtex->height = N_SPLINES;
	gtex->internal_format = GL_RGBA32F;
	gtex->used = 1;
	g->texture_list->add( g->texture_list, (void*)gtex );
	g->bicubic_lut = gtex;
}



void glsl_set_ortho_view( int width, int height )
{
	glViewport( 0, 0, width, height );
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0.0, width, 0.0, height, -1.0, 1.0 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
}



void glsl_draw_quad( float x1, float y1, float x2, float y2 )
{
	glBegin( GL_QUADS );
		glTexCoord2f( x1, y1 );		glVertex3f( x1, y1, 0.);
		glTexCoord2f( x1, y2 );		glVertex3f( x1, y2, 0.);
		glTexCoord2f( x2, y2 );		glVertex3f( x2, y2, 0.);
		glTexCoord2f( x2, y1 );		glVertex3f( x2, y1, 0.);
	glEnd();
}



glsl_texture glsl_rescale_bicubic( glsl_env g, glsl_texture source_tex, int iwidth, int iheight, int owidth, int oheight, int spline )
{
	if ( !g->bicubic_lut ) {
		create_lut_texture( g );
		if ( !g->bicubic_lut )
			return NULL;
	}
	
	glsl_fbo fbo_pass1 = g->get_fbo( g, owidth, iheight );
	if ( !fbo_pass1 )
		return NULL;

	glsl_fbo fbo_pass2 = g->get_fbo( g, owidth, oheight );
	if ( !fbo_pass2 ) {
		g->release_fbo( fbo_pass1 );
		return NULL;
	}

	glsl_texture dest_firstpass = g->get_texture( g, owidth, iheight, GL_RGBA );
	if ( !dest_firstpass ) {
		g->release_fbo( fbo_pass1 );
		g->release_fbo( fbo_pass2 );
		return NULL;
	}

	glsl_texture dest = g->get_texture( g, owidth, oheight, GL_RGBA );
	if ( !dest ) {
		g->release_fbo( fbo_pass1 );
		g->release_fbo( fbo_pass2 );
		g->release_texture( dest_firstpass );
		return NULL;
	}

	glsl_shader shader_pass1 = g->get_shader( g, "filter_bicubic_pass1_frag", &filter_bicubic_pass1_frag );
	if ( !shader_pass1 ) {
		g->release_fbo( fbo_pass1 );
		g->release_fbo( fbo_pass2 );
		g->release_texture( dest_firstpass );
		g->release_texture( dest );
		return NULL;
	}

	glsl_shader shader_pass2 = g->get_shader( g, "filter_bicubic_pass2_frag", &filter_bicubic_pass2_frag );
	if ( !shader_pass2 ) {
		g->release_fbo( fbo_pass1 );
		g->release_fbo( fbo_pass2 );
		g->release_texture( dest_firstpass );
		g->release_texture( dest );
		return NULL;
	}

	// first pass
	glBindFramebuffer( GL_FRAMEBUFFER, fbo_pass1->fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB, dest_firstpass->texture, 0 );

	glsl_set_ortho_view( owidth, iheight );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, source_tex->texture );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, g->bicubic_lut->texture );
	glUseProgram( shader_pass1->program );
	glUniform1i( glGetUniformLocationARB( shader_pass1->program, "tex" ), 0 );
	glUniform1i( glGetUniformLocationARB( shader_pass1->program, "lut" ), 1 );
	glUniform1f( glGetUniformLocationARB( shader_pass1->program, "spline" ), spline );

	glBegin( GL_QUADS );
		glTexCoord2f( 0.0, 0.0 );			glVertex3f( 0.0, 0.0, 0.);
		glTexCoord2f( 0.0, iheight );		glVertex3f( 0.0, iheight, 0.);
		glTexCoord2f( iwidth, iheight );	glVertex3f( owidth, iheight, 0.);
		glTexCoord2f( iwidth, 0.0 );		glVertex3f( owidth, 0.0, 0.);
	glEnd();

	// second pass
	glBindFramebuffer( GL_FRAMEBUFFER, fbo_pass2->fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB, dest->texture, 0 );

	glsl_set_ortho_view( owidth, oheight );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, dest_firstpass->texture );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, g->bicubic_lut->texture );
	glUseProgram( shader_pass2->program );
	glUniform1i( glGetUniformLocationARB( shader_pass2->program, "tex" ), 0 );
	glUniform1i( glGetUniformLocationARB( shader_pass2->program, "lut" ), 1 );
	glUniform1f( glGetUniformLocationARB( shader_pass2->program, "spline" ), spline );

	glBegin( GL_QUADS );
		glTexCoord2f( 0.0, 0.0 );			glVertex3f( 0.0, 0.0, 0.);
		glTexCoord2f( 0.0, iheight );		glVertex3f( 0.0, oheight, 0.);
		glTexCoord2f( owidth, iheight );	glVertex3f( owidth, oheight, 0.);
		glTexCoord2f( owidth, 0.0 );		glVertex3f( owidth, 0.0, 0.);
	glEnd();

	glUseProgram( 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	g->release_fbo( fbo_pass1 );
	g->release_fbo( fbo_pass2 );
	g->release_texture( dest_firstpass );

	return dest;
}



glsl_texture glsl_rescale_bilinear( glsl_env g, glsl_texture source_tex, int iwidth, int iheight, int owidth, int oheight )
{
	glsl_fbo fbo = g->get_fbo( g, owidth, oheight );
	if ( !fbo )
		return NULL;

	glsl_texture dest = g->get_texture( g, owidth, oheight, GL_RGBA );
	if ( !dest ) {
		g->release_fbo( fbo );
		return NULL;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, fbo->fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ARB, dest->texture, 0 );

	glsl_set_ortho_view( owidth, oheight );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, source_tex->texture );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	glBegin( GL_QUADS );
		glTexCoord2f( 0.0, 0.0 );			glVertex3f( 0.0, 0.0, 0.);
		glTexCoord2f( 0.0, iheight );		glVertex3f( 0.0, oheight, 0.);
		glTexCoord2f( iwidth, iheight );	glVertex3f( owidth, oheight, 0.);
		glTexCoord2f( iwidth, 0.0 );		glVertex3f( owidth, 0.0, 0.);
	glEnd();

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

	g->release_fbo( fbo );

	return dest;
}



static glsl_fbo glsl_get_fbo( glsl_env g, int width, int height )
{
	fprintf( stderr, "glsl_get_fbo, %d\n", g->fbo_list->count);
	
	int i;

	for ( i=0; i<g->fbo_list->count; ++i ) {
		glsl_fbo fbo = (glsl_fbo)g->fbo_list->at( g->fbo_list, i );
		if ( !fbo->used && (fbo->width == width) && (fbo->height == height) ) {
			fbo->used = 1;
			return fbo;
		}
	}

	GLuint fb=0;
	glGenFramebuffers( 1, &fb );
	if ( !fb )
		return NULL;

	glsl_fbo fbo = calloc( sizeof( struct glsl_fbo_s ), 1 );
	if ( !fbo ) {
		glDeleteFramebuffers( 1, &fb );
		return NULL;
	}
	
	fbo->fbo = fb;
	fbo->width = width;
	fbo->height = height;
	fbo->used = 1;
	g->fbo_list->add( g->fbo_list, (void*)fbo );
	return fbo;
}



static void glsl_release_fbo( glsl_fbo fbo )
{
	fbo->used = 0;
}



static glsl_pbo glsl_get_pbo( glsl_env g, int size )
{
	if ( !g->pbo ) {
		GLuint pb = 0;
		glGenBuffers( 1, &pb );
		if ( !pb )
			return NULL;

		g->pbo = calloc( sizeof( struct glsl_pbo_s ), 1 );
		if ( !g->pbo ) {
			glDeleteBuffers( 1, &pb );
			return NULL;
		}
		g->pbo->pbo = pb;
	}

	if ( size > g->pbo->size ) {
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER_ARB, g->pbo->pbo );
		glBufferData( GL_PIXEL_UNPACK_BUFFER_ARB, size, NULL, GL_STREAM_DRAW );
		glBindBuffer( GL_PIXEL_UNPACK_BUFFER_ARB, 0 );
		g->pbo->size = size;
	}

	return g->pbo;
}



static glsl_texture glsl_get_texture( glsl_env g, int width, int height, GLint internal_format )
{
	fprintf( stderr, "glsl_get_texture, %d\n", g->texture_list->count);

	int i;

	for ( i=0; i<g->texture_list->count; ++i ) {
		glsl_texture tex = (glsl_texture)g->texture_list->at( g->texture_list, i );
		if ( !tex->used && (tex->width == width) && (tex->height == height) && (tex->internal_format == internal_format) ) {
			glBindTexture( GL_TEXTURE_RECTANGLE_ARB, tex->texture );
			glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			glBindTexture( GL_TEXTURE_RECTANGLE_ARB, 0 );
			tex->used = 1;
			return tex;
		}
	}

	GLuint tex=0;
	glGenTextures( 1, &tex );
	if ( !tex )
		return NULL;

	glsl_texture gtex = calloc( sizeof( struct glsl_texture_s ), 1 );
	if ( !gtex ) {
		glDeleteTextures( 1, &tex );
		return NULL;
	}

	glBindTexture( GL_TEXTURE_RECTANGLE_ARB, tex );
	glTexImage2D( GL_TEXTURE_RECTANGLE_ARB, 0, internal_format, width, height, 0, internal_format, GL_UNSIGNED_BYTE, NULL );
    glTexParameterf( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameterf( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glBindTexture( GL_TEXTURE_RECTANGLE_ARB, 0 );

	gtex->parent = g;
	gtex->texture = tex;
	gtex->width = width;
	gtex->height = height;
	gtex->internal_format = internal_format;
	gtex->used = 1;
	g->texture_list->add( g->texture_list, (void*)gtex );
	return gtex;
}



static void glsl_release_texture( glsl_texture tex )
{
	tex->used = 0;
}



static void glsl_texture_destructor( void *p )
{
	glsl_texture tex = (glsl_texture)p;
	fprintf(stderr,"(((((((((((((((((((((((((((...glsl_texture_destructor, %u\n", (unsigned int)tex);
	tex->used = 0;
}



static glsl_shader glsl_get_shader( glsl_env g, const char *name, const char **source )
{
	fprintf( stderr, "glsl_get_shader, %d\n", g->shader_list->count);

	int i;

	for ( i=0; i<g->shader_list->count; ++i ) {
		glsl_shader shader = (glsl_shader)g->shader_list->at( g->shader_list, i );
		fprintf(stderr, "shader name : %s\n", shader->name);
		if ( !strcmp( shader->name, name ) ) {
			shader->used = 1;
			return shader;
		}
	}
	fprintf(stderr, "No shader found : %s\n", name);

	GLuint shader = glCreateShader( GL_FRAGMENT_SHADER );
	glShaderSource( shader, 1, source, NULL );
	glCompileShader( shader );

	GLint length;
	GLchar *log;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	log = (GLchar*)malloc( length );
	if ( !log ) {
		fprintf( stderr, "Error at %s:%d\n", __FILE__, __LINE__ );
		return 0;
	}
	glGetShaderInfoLog( shader, length, &length, log );
	if ( length ) {
		fprintf( stderr, "Fragment Shader Compilation Log:\n" );
		fprintf( stderr, "%s", log );
	}
	free( log );

	GLuint program = glCreateProgram();
	glAttachShader( program, shader );
	glLinkProgram( program );
	glGetProgramiv( program, GL_INFO_LOG_LENGTH, &length );
	log = (GLchar*)malloc( length );
	if ( !log ) {
		fprintf( stderr, "Error at %s:%d\n", __FILE__, __LINE__ );
		return 0;
	}
	glGetProgramInfoLog( program, length, &length, log );
	if ( length ) {
		fprintf( stderr, "Linking Log:\n" );
		fprintf( stderr, "%s", log );
	}
	free( log );

	glsl_shader pshader = calloc( sizeof( struct glsl_shader_s ), 1 );
	pshader->program = program;
	pshader->used = 1;
	strcpy( pshader->name, name );
	g->shader_list->add( g->shader_list, (void*)pshader );
	return pshader;
}

void mlt_glsl_start( glsl_env g )
{
	if ( g && !g->is_started )
	{
		g->is_started = 1;
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClearDepth( 1.0f );
		glDepthFunc( GL_LEQUAL );
		glEnable( GL_DEPTH_TEST );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glDisable( GL_BLEND );
		glShadeModel( GL_SMOOTH );
		glEnable( GL_TEXTURE_RECTANGLE_ARB );
		glHint( GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST );
	}
}



static glsl_env glsl_env_create()
{
	glsl_env g = calloc( 1, sizeof( struct glsl_env_s ) );
	if ( g ) {
		g->texture_list = glsl_list_create();
		g->fbo_list = glsl_list_create();
		g->shader_list = glsl_list_create();
		g->get_fbo = glsl_get_fbo;
		g->release_fbo = glsl_release_fbo;
		g->get_pbo = glsl_get_pbo;
		g->get_texture = glsl_get_texture;
		g->release_texture = glsl_release_texture;
		g->texture_destructor = glsl_texture_destructor;
		g->get_shader = glsl_get_shader;
		g->image_format = mlt_image_none;
	}
	return g;
}



int mlt_glsl_supported()
{
	const char *extensions = glGetString( GL_EXTENSIONS );

	if ( !extensions )
		return 0;
	if ( !strstr( extensions, "ARB_texture_rectangle" ) )
		return 0;
	if ( !strstr( extensions, "ARB_texture_non_power_of_two" ) )
		return 0;
	if ( !strstr( extensions, "ARB_pixel_buffer_object" ) )
		return 0;
	if ( !strstr( extensions, "ARB_framebuffer_object" ) )
		return 0;
	if ( !strstr( extensions, "ARB_fragment_shader" ) )
		return 0;
	if ( !strstr( extensions, "ARB_vertex_shader" ) )
		return 0;

	fprintf(stderr, "mlt_glsl is supported.\n");

	return 1;
}

glsl_env mlt_glsl_init( mlt_profile profile )
{
	char prop_name[24];
	mlt_properties prop = mlt_global_properties();
	snprintf( prop_name, sizeof(prop_name), "glsl-%p", profile );
	glsl_env g = mlt_properties_get_data( prop, prop_name, NULL );

	if ( !g && ( g = glsl_env_create() ) )
	{
		mlt_properties_set_data( prop, prop_name, g, 0, (mlt_destructor) mlt_glsl_close, NULL );
		fprintf(stderr, "mlt_glsl init done.\n");
	}

	return g;
}

glsl_env mlt_glsl_get( mlt_profile profile )
{
	char prop_name[24];
	mlt_properties prop = mlt_global_properties();
	snprintf( prop_name, sizeof(prop_name), "glsl-%p", profile );
	return mlt_properties_get_data( prop, prop_name, NULL );
}
