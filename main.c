/*  Copyright (C) 2025 Andrea Monaco
 *
 *  This file is part of screenrec, a utility for screen recording on Linux.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <pthread.h>
#include <semaphore.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <x264.h>



enum
pixel_format  /* pixel formats supported by this utility */
  {
    XR24
  };


#define MODIFIER_VENDOR(modifier) (((modifier) >> 56) & 0xff)

#define MODIFIER_VALUE(modifier) ((modifier) & 0x00ffffffffffffffULL)

enum
pixel_order  /* pixel orders supported by this utility */
  {
    LINEAR,
    TILEDX_4KB
  };


enum
action
  {
    DUMP_INFO,
    SCREENSHOT,
    RECORD
  };


struct
cue
{
  long timestamp;
  int cluster_position;
  int relative_position;
};


#define CUE_VECTOR_SIZE 2048

struct
cue_vector
{
  struct cue cues [CUE_VECTOR_SIZE];
  struct cue_vector *next;
};


void *
malloc_and_check (size_t size)
{
  void *mem = malloc (size);

  if (size && !mem)
    {
      fprintf (stderr, "could not allocate %lu bytes.  Exiting...\n", size);
      exit (1);
    }

  return mem;
}


drmDevice **
get_devices (int *num)
{
  drmDevice **ret;

  *num = drmGetDevices2 (0, NULL, 0);

  if (*num < 0)
    {
      fprintf (stderr, "couldn't determine number of devices\n");
      exit (1);
    }

  ret = malloc_and_check (sizeof (drmDevice *) * *num);

  *num = drmGetDevices2 (0, ret, *num);

  if (*num < 0)
    {
      fprintf (stderr, "couldn't enumerate devices\n");
      exit (1);
    }

  return ret;
}


void
dump_drm_info_and_exit (void)
{
  drmDevice **devs;
  drmModeRes *res;
  drmModePlaneRes *planes;
  drmModeCrtc *crtc;
  drmModePlane *plane;
  drmModeFB *fb;
  drmModeFB2 *fb2;
  struct stat statbuf;
  int crtcsnum, ret, i, j, fd, dmabuf_fd;
  char *card;


  if (drmAvailable ())
    printf ("drm is available\n");
  else
    {
      fprintf (stderr, "drm not available\n");
      exit (1);
    }

  devs = get_devices (&ret);

  printf ("there %s %d device%s, selecting only the primary ones...\n",
	  ret == 1 ? "is" : "are", ret, ret == 1 ? "" : "s");

  for (i = 0; i < ret; i++)
    {
      if (!(devs [i]->available_nodes & 1 << DRM_NODE_PRIMARY))
	continue;

      card = devs [i]->nodes [DRM_NODE_PRIMARY];

      printf ("\tdevice %s\n", card);

      fd = open (card, O_RDONLY);

      if (fd < 0)
	{
	  printf ("couldn't open video card %d (%s)\n", i, card);
	  continue;
	}

      res = drmModeGetResources (fd);

      if (!res)
	{
	  fprintf (stderr, "couldn't inspect video card\n");
	  exit (1);
	}

      printf ("\tnum of framebuffers: %d\n\tnum of crtcs: %d\n\tnum of "
	      "connectors: %d\n\tnum of encoders: %d\n",
	      res->count_fbs, res->count_crtcs, res->count_connectors,
	      res->count_encoders);

      crtcsnum = res->count_crtcs;

      for (j = 0; j < crtcsnum; j++)
	{
	  crtc = drmModeGetCrtc (fd, res->crtcs[j]);

	  if (!crtc)
	    {
	      printf ("\tcould not access crtc number %d\n", j);
	      drmModeFreeCrtc (crtc);
	      continue;
	    }

	  printf ("\tcrtc %d:\n\tbuffer_id = %d, x = %d, y = %d, w = %d, "
		  "h = %d\n", j, crtc->buffer_id, crtc->x, crtc->y, crtc->width,
		  crtc->height);

	  if (crtc->mode_valid)
	    printf ("\tvrefresh = %d\n", crtc->mode.vrefresh);
	  else
	    printf ("\tmode is not valid\n");

	  if (crtc->buffer_id)
	    {
	      fb = drmModeGetFB (fd, crtc->buffer_id);
	      fb2 = drmModeGetFB2 (fd, crtc->buffer_id);

	      if (!fb || !fb2)
		{
		  printf ("could not inspect framebuffer\n");
		  drmModeFreeFB (fb);
		  drmModeFreeFB2 (fb2);
		  drmModeFreeCrtc (crtc);
		  continue;
		}

	      printf ("\t\tframebuffer %d:\n\t\twidth = %d, height = %d, "
		      "pitch = %d, bpp = %d, depth = %d, handle = %d\n"
		      "\t\tpixel_format = %d (%c%c%c%c), modifier = %ld "
		      "(vendor = %ld, code = %llu)\n"
		      "\t\tGEM handles = %d %d %d %d\n"
		      "\t\tpitches = %d %d %d %d\n"
		      "\t\toffsets = %d %d %d %d\n",
		      crtc->buffer_id, fb->width, fb->height, fb->pitch, fb->bpp,
		      fb->depth, fb->handle,
		      fb2->pixel_format, fb2->pixel_format & 0xff,
		      (fb2->pixel_format & 0xff00) >> 8,
		      (fb2->pixel_format & 0xff0000) >> 16,
		      (fb2->pixel_format & 0xff000000) >> 24, fb2->modifier,
		      (fb2->modifier >> 56) & 0xff,
		      fb2->modifier & 0x00ffffffffffffffULL,
		      fb2->handles [0], fb2->handles [1], fb2->handles [2],
		      fb2->handles [3], fb2->pitches [0], fb2->pitches [1],
		      fb2->pitches [2], fb2->pitches [3], fb2->offsets [0],
		      fb2->offsets [1], fb2->offsets [2], fb2->offsets [3]);

	      if (drmPrimeHandleToFD (fd, fb->handle, 0, &dmabuf_fd))
		printf ("\t\tcouldn't get file descriptor for this framebuffer, "
			"maybe you lack permissions?\n");
	      else
		{
		  if (fstat (dmabuf_fd, &statbuf) < 0)
		    printf ("\t\tcouldn't stat dmabuf\n");
		  else
		    {
		      printf ("\t\tbuffer size is %ld\n", statbuf.st_size); 
		    }
		}

	      drmModeFreeFB (fb);
	      drmModeFreeFB2 (fb2);
	    }

	  drmModeFreeCrtc (crtc);
	}

      drmModeFreeResources (res);


      planes = drmModeGetPlaneResources (fd);

      if (!planes)
	{
	  printf ("\tcould not inspect planes\n");
	  continue;
	}


      for (j = 0; j < planes->count_planes; j++)
	{
	  plane = drmModeGetPlane (fd, planes->planes [j]);

	  if (!plane)
	    {
	      printf ("\tcould not access plane number %d\n", j);
	      continue;
	    }

	  printf ("\tplane %d:\n\tcrtc_id = %d, fb_id = %d, crtc_x = %d, "
		  "crtc_y = %d, x = %d, y = %d, possible_crtcs = %d, "
		  "gamma_size = %d\n", j, plane->crtc_id, plane->fb_id,
		  plane->crtc_x, plane->crtc_y, plane->x, plane->y,
		  plane->possible_crtcs, plane->gamma_size);

	  drmModeFreePlane (plane);
	}

      drmModeFreePlaneResources (planes);

      close (fd);
    }

  exit (0);
}


void
dump_linear_pixels (char *buf, int w, int h, int p, enum pixel_format pf)
{
  int i, j;
  char *row = buf, *pix;

  for (i = 0; i < h; i++)
    {
      pix = row;

      for (j = 0; j < w; j++)
	{
	  switch (pf)
	    {
	    case XR24:
	      putchar (pix [2]);
	      putchar (pix [1]);
	      putchar (pix [0]);
	      break;
	    }

	  pix += 4;
	}

      row += p;
    }
}


void
convert_tiledx4kb_pixels_to_linear (unsigned char *out, char *in, int x, int y,
				    int w, int h, int p, enum pixel_format pf)
{
  int destind = 0, srcind, i, j;

  for (j = y; j < y+h; j++)
    {
      for (i = x; i < x+w; i++)
	{
	  srcind = j/8*4096*(p/512)+i/128*4096+(j%8)*512+(i%128)*4;

	  out [destind] = in [srcind+2];
	  out [destind+1] = in [srcind+1];
	  out [destind+2] = in [srcind];

	  destind += 3;
	}
    }
}


void
dump_tiledx4kb_pixels_linearly (char *buf, int x, int y, int w, int h, int p,
				enum pixel_format pf)
{
  int i;
  unsigned char *out = malloc_and_check (w*h*3);

  convert_tiledx4kb_pixels_to_linear (out, buf, x, y, w, h, p, pf);

  for (i = 0; i < w*h*3; i++)
    putchar (out [i]);

  free (out);
}


int
open_framebuffer (drmModeFB2 **fb2, int *cardfd, int *native_refresh)
{
  drmDevice **devs;
  drmModeRes *res;
  drmModeCrtc *crtc;
  int devsnum, dmabuf_fd;

  devs = get_devices (&devsnum);

  *cardfd = open (devs [0]->nodes [DRM_NODE_PRIMARY], O_RDONLY);

  if (*cardfd < 0)
    {
      fprintf (stderr, "couldn't open video card %d (%s)\n", 0,
	      devs [0]->nodes [DRM_NODE_PRIMARY]);
      exit (1);
    }

  res = drmModeGetResources (*cardfd);

  if (!res)
    {
      fprintf (stderr, "couldn't inspect video card\n");
      exit (1);
    }

  crtc = drmModeGetCrtc (*cardfd, res->crtcs[0]);

  if (!crtc)
    {
      fprintf (stderr, "could not access crtc number 0\n");
      exit (1);
    }


  if (native_refresh)
    {
      if (crtc->mode_valid)
	*native_refresh = crtc->mode.vrefresh;
      else
	*native_refresh = -1;
    }


  *fb2 = drmModeGetFB2 (*cardfd, crtc->buffer_id);

  if (!*fb2)
    {
      fprintf (stderr, "could not inspect framebuffer\n");
      exit (1);
    }

  if (drmPrimeHandleToFD (*cardfd, (*fb2)->handles [0], 0, &dmabuf_fd))
    {
      fprintf (stderr, "couldn't get file descriptor for framebuffer, "
	       "maybe you lack permissions?\n");
      exit (1);
    }

  drmModeFreeCrtc (crtc);
  drmModeFreeResources (res);

  fprintf (stderr, "selecting first plane of first framebuffer of first crtc of "
	   "first video card...\n");

  return dmabuf_fd;
}


void
take_screenshot_and_exit (int x, int y, int w, int h)
{
  drmModeFB2 *fb2;
  struct stat statbuf;
  enum pixel_format pf;
  enum pixel_order po;
  long mod;
  char *buf;
  int dmabuf_fd, cardfd, pixel_format;


  dmabuf_fd = open_framebuffer (&fb2, &cardfd, NULL);


  w = w < 0 ? fb2->width-x : w;
  h = h < 0 ? fb2->height-y : h;

  if (w <= 0 || h <= 0 || x+w > fb2->width || y+h > fb2->height)
    {
      fprintf (stderr, "out-of-bound geometry in -g option\n");
      exit (1);
    }


  pixel_format = fb2->pixel_format;

  if (!strncmp ((char *)&pixel_format, "XR24", 4))
    pf = XR24;
  else
    {
      fprintf (stderr, "warning: unsupported pixel format, defaulting to XR24...\n");
      pf = XR24;
    }

  mod = fb2->modifier;

  if (!MODIFIER_VENDOR (mod) && !MODIFIER_VALUE (mod))
    po = LINEAR;
  else if (MODIFIER_VENDOR (mod) == 1 && MODIFIER_VALUE (mod) == 1)
    po = TILEDX_4KB;
  else
    {
      fprintf (stderr, "warning: unsupported pixel order, defaulting to linear...\n");
      po = LINEAR;
    }


  if (fstat (dmabuf_fd, &statbuf) < 0)
    {
      fprintf (stderr, "couldn't stat dmabuf of the framebuffer\n");
      exit (1);
    }

  buf = mmap (NULL, statbuf.st_size, PROT_READ, MAP_SHARED, dmabuf_fd,
	      fb2->offsets [0]);

  if (buf == (void *) -1)
    {
      fprintf (stderr, "couldn't mmap dmabuf of the framebuffer\n");
      exit (1);
    }


  printf ("P6\n%d\n%d\n255\n", w, h);

  switch (po)
    {
    case LINEAR:
      dump_linear_pixels (buf, fb2->width, fb2->height, fb2->pitches [0], pf);
      break;
    case TILEDX_4KB:
      dump_tiledx4kb_pixels_linearly (buf, x, y, w, h, fb2->pitches [0], pf);
      break;
    }

  exit (0);
}


void
write_char (int fd, int ch)
{
  unsigned char c = ch & 0xff;

  if (write (fd, &c, 1) != 1)
    {
      fprintf (stderr, "couldn't write to output file: ");
      perror ("");
    }
}


void
write_int32_bigend (int fd, int num)
{
  write_char (fd, (num >> 24) & 0xff);
  write_char (fd, (num >> 16) & 0xff);
  write_char (fd, (num >> 8) & 0xff);
  write_char (fd, num & 0xff);
}


void
write_int64_bigend (int fd, long num)
{
  write_int32_bigend (fd, (num & 0xffffffff00000000) >> 32);
  write_int32_bigend (fd, num);
}


const unsigned char ebml_header [] =
  {0x1a, 0x45, 0xdf, 0xa3, 0xa3,
   0x42, 0x86, 0x81, 0x01,
   0x42, 0xf7, 0x81, 0x01,
   0x42, 0xf2, 0x81, 0x04,
   0x42, 0xf3, 0x81, 0x08,
   0x42, 0x82, 0x88, 'm', 'a', 't', 'r', 'o', 's', 'k', 'a',
   0x42, 0x87, 0x81, 0x04,
   0x42, 0x85, 0x81, 0x02};
unsigned char segment_header [] =
  {0x18, 0x53, 0x80, 0x67, 0x00, 0x00, 0x00, 0x00};

#define SEGMENT_BODY_START (sizeof (ebml_header)+sizeof (segment_header))


void
write_minimal_matroska_header (int outfd, int width, int height,
			       int default_duration, x264_nal_t headers [],
			       int headers_num, off_t *seekhead_offs)
{
  x264_nal_t *sps = NULL, *pps = NULL;
  int i, j, header_sz, avcrec_sz;
  unsigned char tracks_header []
    = {0x16, 0x54, 0xae, 0x6b, 0x00, /* all video tracks */
       0xae, 0x00, /* track entry */
       0xd7, 0x81, 0x1, /* track number */
       0x73, 0xc5, 0x81, 0x1, /* track uid */
       0x83, 0x81, 0x1, /* track type */
       0x23, 0xe3, 0x83, 0x84, 0x00, 0x00, 0x00, 0x00, /* default duration */
       0xe0, 0x88, /* video settings */
       0xb0, 0x82, 0x00, 0x00, 0xba, 0x82, 0x00, 0x00, /* pixel width and height */
       0x86, 0x8f, 'V', '_', 'M', 'P', 'E', 'G', '4', '/' , 'I', 'S', 'O',
       '/', 'A', 'V', 'C', /* codec id */};
  unsigned char codec_private_header []
    = {0x63, 0xa2, 0x00}; /* codec private */
  unsigned char avcrec_header []
    = {0x01, 0x42, 0xc0, 0x1f, 0xff};
  unsigned char other_headers []
    = {0x11, 0x4d, 0x9b, 0x74, 0xad, /* seek head */
       0x4d, 0xbb, 0x8b, /* seek of tracks */
       0x53, 0xab, 0x84, 0x16, 0x54, 0xae, 0x6b, /* seek id of tracks */
       0x53, 0xac, 0x81, 0x00, /* seek position of tracks */
       0x4d, 0xbb, 0x8b, /* seek of info */
       0x53, 0xab, 0x84, 0x15, 0x49, 0xa9, 0x66, /* seek id of info */
       0x53, 0xac, 0x81, 0x00, /* seek position of info */

       0x4d, 0xbb, 0x8e, /* seek of cues */
       0x53, 0xab, 0x84, 0x1c, 0x53, 0xbb, 0x6b, /* seek id of cues */
       0x53, 0xac, 0x84, 0x00, 0x00, 0x00, 0x00, /* seek position of cues */

       0x15, 0x49, 0xa9, 0x66, 0x9f, /* info header */
       0x2a, 0xd7, 0xb1, 0x83, 0x00, 0x00, 0x01, /* timestamp scale */
       0x4d, 0x80, 0x89, 's', 'c', 'r', 'e', 'e', 'n', 'r', 'e', 'c', /* muxing app */
       0x57, 0x41, 0x89, 's', 'c', 'r', 'e', 'e', 'n', 'r', 'e', 'c', /* writing app */
  };
  unsigned char *header;


  for (i = 0; i < headers_num; i++)
    {
      switch (headers [i].i_type)
	{
	case NAL_SPS:
	  sps = &headers [i];
	  break;
	case NAL_PPS:
	  pps = &headers [i];
	  break;
	}
    }

  if (!sps || !pps)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  avcrec_sz = sizeof (avcrec_header)+3+sps->i_payload+3+pps->i_payload;

  header_sz = sizeof (ebml_header)+sizeof (segment_header)+sizeof (tracks_header)
    +sizeof (codec_private_header)+avcrec_sz+sizeof (other_headers);
  header = malloc_and_check (header_sz);

  for (i = 0; i < sizeof (ebml_header); i++)
    {
      header [i] = ebml_header [i];
    }

  for (j = 0; j < sizeof (segment_header); j++)
    {
      header [i++] = segment_header [j];
    }

  for (j = 0; j < sizeof (tracks_header); j++)
    {
      header [i++] = tracks_header [j];
    }

  for (j = 0; j < sizeof (codec_private_header); j++)
    {
      header [i++] = codec_private_header [j];
    }

  for (j = 0; j < sizeof (avcrec_header); j++)
    {
      header [i++] = avcrec_header [j];
    }

  header [i++] = 0xe1;
  header [i++] = (sps->i_payload >> 8) & 0xff;
  header [i++] = sps->i_payload & 0xff;
  memcpy (header+i, sps->p_payload, sps->i_payload);
  i += sps->i_payload;

  header [i++] = 0x01;
  header [i++] = (pps->i_payload >> 8) & 0xff;
  header [i++] = pps->i_payload & 0xff;
  memcpy (header+i, pps->p_payload, pps->i_payload);
  i += pps->i_payload;

  /*fprintf (stderr, "avcrec_sz is %d\n", avcrec_sz);*/

  if (avcrec_sz > 126)
    {
      fprintf (stderr, "avcrec_sz too big\n");
      exit (1);
    }

  header [sizeof (ebml_header)+sizeof (segment_header)+sizeof (tracks_header)+2]
    = 0x80 | avcrec_sz;


  default_duration *= 1;

  header [sizeof (ebml_header)+sizeof (segment_header)+21]
    = (default_duration & 0xff000000) >> 24;
  header [sizeof (ebml_header)+sizeof (segment_header)+22]
    = (default_duration & 0xff0000) >> 16;
  header [sizeof (ebml_header)+sizeof (segment_header)+23]
    = (default_duration & 0xff00) >> 8;
  header [sizeof (ebml_header)+sizeof (segment_header)+24]
    = default_duration & 0xff;


  header [sizeof (ebml_header)+sizeof (segment_header)+29] = (width & 0xff00) >> 8;
  header [sizeof (ebml_header)+sizeof (segment_header)+30] = width & 0xff;
  header [sizeof (ebml_header)+sizeof (segment_header)+33] = (height & 0xff00) >> 8;
  header [sizeof (ebml_header)+sizeof (segment_header)+34] = height & 0xff;

  if (sizeof (tracks_header)-7+sizeof (codec_private_header)+avcrec_sz > 126)
    {
      fprintf (stderr, "track entry too big\n");
      exit (1);
    }

  header [sizeof (ebml_header)+sizeof (segment_header)+6]
    = 0x80 | (sizeof (tracks_header)-7+sizeof (codec_private_header)+avcrec_sz);

  if (sizeof (tracks_header)-5+sizeof (codec_private_header)+avcrec_sz > 126)
    {
      fprintf (stderr, "tracks too big\n");
      exit (1);
    }

  header [sizeof (ebml_header)+sizeof (segment_header)+4]
    = 0x80 | (sizeof (tracks_header)-5+sizeof (codec_private_header)+avcrec_sz);


  *seekhead_offs = i;

  for (j = 0; j < sizeof (other_headers); j++)
    {
      header [i++] = other_headers [j];
    }

  header [*seekhead_offs+32] = *seekhead_offs+50-SEGMENT_BODY_START;

  if (lseek (outfd, 0, SEEK_SET) < 0)
    {
      fprintf (stderr, "couldn't seek in output file\n");
      exit (1);
    }

  for (i = 0; i < header_sz; i++)
    {
      write_char (outfd, header [i]);
    }
}


void
write_cluster_header (int outfd, long timestamp)
{
  int i;
  unsigned char cluster_header [] =
    {0x1f, 0x43, 0xb6, 0x75, 0xff, 0xff, 0xff, 0xff, /* cluster header */
     0xe7, 0x88 /* timestamp */ };

  for (i = 0; i < sizeof (cluster_header); i++)
    {
      write_char (outfd, cluster_header [i]);
    }

  write_int64_bigend (outfd, timestamp);
}


struct
thread_args
{
  int index;
  int total;

  unsigned char *out;
  char *in;
  int x, y, w, h, p;
  enum pixel_format pf;
};

sem_t *may_start;
sem_t has_finished;


void *
rearrange_rows (void *args)
{
  struct thread_args *arg = args;
  int destind, srcind, i, j, striph = ceil ((double)arg->h/arg->total);


  /*fprintf (stderr, "thread %d started, strips are %d high\n", arg->index, striph);*/

  for (;;)
    {
      /*fprintf (stderr, "thread %d waiting for may_start semaphore\n", arg->index);*/
      sem_wait (&may_start [arg->index]);
      /*fprintf (stderr, "thread %d got may_start semaphore\n", arg->index);*/

      destind = arg->index*striph*arg->w*3;

      for (j = arg->y+arg->index*striph; j < arg->y+(arg->index+1)*striph
	     && j < arg->y+arg->h; j++)
	{
	  for (i = arg->x; i < arg->x+arg->w; i++)
	    {
	      srcind = j/8*4096*(arg->p/512)+i/128*4096+(j%8)*512+(i%128)*4;

	      arg->out [destind] = arg->in [srcind+2];
	      arg->out [destind+1] = arg->in [srcind+1];
	      arg->out [destind+2] = arg->in [srcind];

	      destind += 3;
	    }
	}

      /*fprintf (stderr, "thread %d posting has_finished semaphore\n", arg->index);*/
      sem_post (&has_finished);
    }

  fprintf (stderr, "thread %d finished\n", arg->index);

  return NULL;
}


void
record_screen_and_exit (char *output, char *preset, int x, int y, int w, int h,
			int recording_interval)
{
  x264_param_t par;
  x264_picture_t inframe, outframe;
  x264_nal_t *nal, *headers;
  x264_t *enc;
  drmModeFB2 *fb2;
  drmVBlank vbl = {{DRM_VBLANK_RELATIVE, 1}};
  struct thread_args *args;
  pthread_t *threads;
  struct cue_vector cue_vectors = {{{0}}}, *cuevec = &cue_vectors;
  struct stat statbuf;
  struct pollfd pfd = {0, POLLIN};
  off_t off, seekh_off;
  char *buf;
  unsigned char *out;
  long timestamp_of_cluster;
  int i, outfd, dmabuf_fd, cardfd, native_refresh, frame_duration,
    num_frames_within_cluster, outsz, i_nal, headers_num,
    timestamp_within_cluster, cluster_offset_within_segment, cluster_size,
    last_vblank = -1, cueind = 0, cues_size, nthreads;


  dmabuf_fd = open_framebuffer (&fb2, &cardfd, &native_refresh);


  w = w < 0 ? fb2->width-x : w;
  h = h < 0 ? fb2->height-y : h;

  if (w <= 0 || h <= 0 || x+w > fb2->width || y+h > fb2->height)
    {
      fprintf (stderr, "out-of-bound geometry in -g option\n");
      exit (1);
    }


  if (native_refresh < 0)
    {
      fprintf (stderr, "warning: couldn't determine native refresh rate, "
	       "assuming 60 hz\n");
      native_refresh = 60;
    }

  frame_duration = (int) (1000000000.0/native_refresh+0.5);


  if (x264_param_default_preset (&par, preset, NULL) < 0)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  par.i_bitdepth = 8;
  /*par.i_csp = X264_CSP_I420;*/
  par.i_csp = X264_CSP_RGB;
  par.i_width = w;
  par.i_height = h;
  par.b_vfr_input = 0;
  par.b_repeat_headers = 0;
  par.b_annexb = 1;

  if (x264_param_apply_profile (&par, "high444") < 0)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  if (x264_picture_alloc (&inframe, X264_CSP_RGB, w, h) < 0)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  enc = x264_encoder_open (&par);

  if (!enc)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  if (fstat (dmabuf_fd, &statbuf) < 0)
    {
      fprintf (stderr, "couldn't stat dmabuf of the framebuffer\n");
      exit (1);
    }

  buf = mmap (NULL, statbuf.st_size, PROT_READ, MAP_SHARED, dmabuf_fd,
	      fb2->offsets [0]);

  if (buf == (void *) -1)
    {
      fprintf (stderr, "couldn't mmap dmabuf of the framebuffer\n");
      exit (1);
    }

  if (x264_encoder_headers (enc, &headers, &headers_num) < 0)
    {
      fprintf (stderr, "couldn't configure x264 encoder\n");
      exit (1);
    }

  fprintf (stderr, "warning: assuming pixel format XR24...\n");
  fprintf (stderr, "warning: assuming pixel order tiled X by 4 KB...\n\n");

  fprintf (stderr, "press ENTER to stop recording\n\n");

  outfd = open (output, O_RDWR | O_CREAT | O_TRUNC);

  if (outfd < 0)
    {
      fprintf (stderr, "couldn't open %s: ", output);
      perror ("");
    }

  write_minimal_matroska_header (outfd, w, h, frame_duration*recording_interval,
				 headers, headers_num, &seekh_off);

  timestamp_of_cluster = 0;
  cluster_offset_within_segment = lseek (outfd, 0, SEEK_CUR)-SEGMENT_BODY_START;
  write_cluster_header (outfd, timestamp_of_cluster);
  num_frames_within_cluster = 0;
  timestamp_within_cluster = 0;
  cluster_size = 10;

  out = malloc_and_check (w*h*3);
  inframe.img.plane [0] = out;


  nthreads = sysconf (_SC_NPROCESSORS_ONLN);

  args = malloc_and_check (sizeof (*args) * nthreads);
  may_start = malloc_and_check (sizeof (*may_start) * nthreads);
  threads = malloc_and_check (sizeof (*threads) * nthreads);

  for (i = 0; i < nthreads; i++)
    {
      args [i].index = i;
      args [i].total = nthreads;

      args [i].out = out;
      args [i].in = buf;
      args [i].x = x;
      args [i].y = y;
      args [i].w = w;
      args [i].h = h;
      args [i].p = fb2->pitches [0];

      sem_init (&may_start [i], 0, 0);

      if (pthread_create (&threads [i], NULL, rearrange_rows, &args [i]))
	{
	  fprintf (stderr, "couldn't create thread\n");
	  exit (1);
	}
    }

  sem_init (&has_finished, 0, 0);


  for (;;)
    {
      if (drmWaitVBlank (cardfd, &vbl) < 0)
	{
	  fprintf (stderr, "couldn't wait for vblank\n");
	  exit (1);
	}

      if (last_vblank < 0)  /* first iteration */
	{
	  last_vblank = vbl.reply.sequence;

	  vbl.request.type = DRM_VBLANK_ABSOLUTE;
	}
      else
	{
	  if (recording_interval < vbl.reply.sequence - last_vblank)
	    {
	      fprintf (stderr, "warning: at least a frame was skipped\n");
	    }

	  num_frames_within_cluster += vbl.reply.sequence-last_vblank;
	  last_vblank = vbl.reply.sequence;
	}

      vbl.request.sequence = vbl.reply.sequence+recording_interval;


      /*fprintf (stderr, "posting may_start semaphores\n");*/

      for (i = 0; i < nthreads; i++)
	{
	  sem_post (&may_start [i]);
	}

      /*fprintf (stderr, "waiting has_finished semaphore\n");*/

      for (i = 0; i < nthreads; i++)
	{
	  sem_wait (&has_finished);
	}

      /*fprintf (stderr, "got has_finished semaphore\n");*/


      /*convert_tiledx4kb_pixels_to_linear (out, buf, w, h, fb2->pitches [0], 0);*/


      inframe.i_pts = num_frames_within_cluster;

      outsz = x264_encoder_encode (enc, &nal, &i_nal, &inframe, &outframe);

      if (outsz < 0)
	{
	  fprintf (stderr, "couldn't encode framebuffer content\n");
	  exit (1);
	}
      else if (outsz)
	{
	  if (outsz+4 > 268435455)
	    fprintf (stderr, "skipping this frame because size (%d) is too "
		     "big\n", outsz);
	  else
	    {
	      timestamp_within_cluster = num_frames_within_cluster*frame_duration;

	      if (0x7fff < timestamp_within_cluster
		  || nal->i_type == NAL_SLICE_IDR)
		{
		  /*if (nal->i_type != NAL_SLICE_IDR)
		    fprintf (stderr, "warning: closing a cluster before a new IDR "
		    "was reached\n");*/

		  off = lseek (outfd, 0, SEEK_CUR);

		  lseek (outfd, -cluster_size-4, SEEK_CUR);
		  write_int32_bigend (outfd, 0x10000000 | cluster_size);

		  lseek (outfd, off, SEEK_SET);
		  timestamp_of_cluster += timestamp_within_cluster;
		  cluster_offset_within_segment = lseek (outfd, 0, SEEK_CUR)
		    -SEGMENT_BODY_START;
		  write_cluster_header (outfd, timestamp_of_cluster);
		  num_frames_within_cluster = 0;
		  timestamp_within_cluster = 0;
		  cluster_size = 10;
		}

	      /*printf ("nal type is %d\n", nal->i_type);*/

	      if (nal->i_type == NAL_SLICE_IDR)
		{
		  /*fprintf (stderr, "keyframe at %d, offset is %d\n", timestamp_of_cluster
		    +timestamp_within_cluster, cluster_offset_within_segment);*/

		  if (cueind == CUE_VECTOR_SIZE)
		    {
		      cuevec->next = malloc_and_check (sizeof (*cuevec->next));
		      cuevec = cuevec->next;
		      cuevec->next = NULL;
		      cueind = 0;
		    }

		  cuevec->cues [cueind].timestamp = timestamp_of_cluster
		    +timestamp_within_cluster;
		  cuevec->cues [cueind].cluster_position
		    = cluster_offset_within_segment;
		  cuevec->cues [cueind].relative_position = cluster_size;
		  cueind++;
		}

	      write_char (outfd, 0xa3);
	      write_int32_bigend (outfd, 0x10000000 | (outsz+4));

	      /*fprintf (stderr, "timestamp = %ld %ld\n", vbl.reply.tval_sec,
		vbl.reply.tval_usec);*/
	      /*fprintf (stderr, "timestamp = %d\n", timestamp_within_cluster);*/

	      write_char (outfd, 0x81);
	      write_char (outfd, ((timestamp_within_cluster>>8) & 0xff));
	      write_char (outfd, timestamp_within_cluster & 0xff);
	      write_char (outfd, 0);

	      /*if (i_nal > 1)
		{
		  printf ("more than a nal produced\n");

		  for (i = 0; i < i_nal; i++)
		    printf ("nal type is %d\n", nal [i].i_type);
		    }*/

	      if (write (outfd, nal->p_payload, outsz) != outsz)
		{
		  fprintf (stderr, "couldn't encode framebuffer content\n");
		  exit (1);
		}

	      cluster_size += outsz + 9;
	    }
	}

      if (poll (&pfd, 1, 0) < 0)
	{
	  fprintf (stderr, "couldn't poll standard input\n");
	  exit (1);
	}

      if (pfd.revents & POLLIN)
	break;
    }

  fprintf (stderr, "finishing and adding cues...\n");


  off = lseek (outfd, 0, SEEK_CUR);

  lseek (outfd, -cluster_size-4, SEEK_CUR);
  write_int32_bigend (outfd, 0x10000000 | cluster_size);

  lseek (outfd, seekh_off+46, SEEK_SET);
  write_int32_bigend (outfd, off-SEGMENT_BODY_START);

  lseek (outfd, off, SEEK_SET);
  write_int32_bigend (outfd, 0x1c53bb6b);
  off = lseek (outfd, 0, SEEK_CUR);
  write_int32_bigend (outfd, 0x00000000);

  cuevec = &cue_vectors;

  while (cuevec)
    {
      for (i = 0; i < (cuevec->next ? CUE_VECTOR_SIZE : cueind); i++)
	{
	  write_char (outfd, 0xbb); /* cue point */
	  write_char (outfd, 0x9b);

	  write_char (outfd, 0xb3); /* cue time */
	  write_char (outfd, 0x88);
	  write_int64_bigend (outfd, cuevec->cues [i].timestamp);

	  write_char (outfd, 0xb7); /* cue track positions */
	  write_char (outfd, 0x8f);

	  write_char (outfd, 0xf7); /* cue track */
	  write_char (outfd, 0x81);
	  write_char (outfd, 0x01);

	  write_char (outfd, 0xf1); /* cue cluster position */
	  write_char (outfd, 0x84);
	  write_int32_bigend (outfd, cuevec->cues [i].cluster_position);

	  write_char (outfd, 0xf0); /* cue relative position */
	  write_char (outfd, 0x84);
	  write_int32_bigend (outfd, cuevec->cues [i].relative_position);
	}

      cuevec = cuevec->next;
    }

  cues_size = lseek (outfd, 0, SEEK_CUR)-off-4;
  lseek (outfd, off, SEEK_SET);
  write_int32_bigend (outfd, 0x10000000 | cues_size);

  off = lseek (outfd, 0, SEEK_END);
  lseek (outfd, sizeof (ebml_header)+4, SEEK_SET);
  write_int32_bigend (outfd, 0x10000000 | (off-SEGMENT_BODY_START));

  exit (0);
}


void
print_help_and_exit (void)
{
  printf ("options:\n"
	  "\t--record-screen or -r:      record screen and print the binary data "
	  "to stdout in MKV format\n"
	  "\t--preset or -p PRESET:      select a preset when recording screen, "
	  "default is medium\n"
	  "\t--geometry or -g X,Y[,WxH]: select a portion of the screen to record "
	  "or screenshot, starting from (X,Y) and spanning WxH pixels, "
	  "for example 10,20,40x40\n"
	  "\t--record-every-th or -y N   record one frame every N, defaults to one "
	  "for recording at native refresh rate\n"
	  "\t--output or -o FILE:        output file, required for recording\n"
	  "\t--take-screenshot or -s:    take a screenshot and print "
	  "the data to stdout in binary PPM format\n"
	  "\t--dump-info or -d:          dump info about your DRM setup\n"
	  "\t--help or -h:               print this help and exit\n");
  exit (0);
}


int
main (int argc, char *argv [])
{
  enum action act = DUMP_INFO;
  char *preset = "medium", *geometry = NULL, *output = NULL;
  int i, need_arg = 0, record_interv = 1, x = -1, y = -1, w = -1, h = -1;


  for (i = 1; i < argc; i++)
    {
      if (need_arg)
	{
	  switch (need_arg)
	    {
	    case 'p':
	      preset = argv [i];
	      break;
	    case 'g':
	      geometry = argv [i];
	      break;
	    case 'y':
	      if (strlen (argv [i]) != 1 || *argv [i] < '1' || *argv [i] > '9')
		{
		  fprintf (stderr, "option 'y' requires an integer argument "
			   "between 1 and 9\n");
		  print_help_and_exit ();
		}
	      record_interv = *argv [i]-'0';
	      break;
	    case 'o':
	      output = argv [i];
	      break;
	    }

	  need_arg = 0;
	}
      else if (!strcmp (argv [i], "--record-screen")
	       || !strcmp (argv [i], "-r"))
	act = RECORD;
      else if (!strcmp (argv [i], "--preset") || !strcmp (argv [i], "-p"))
	need_arg = 'p';
      else if (!strcmp (argv [i], "--geometry") || !strcmp (argv [i], "-g"))
	need_arg = 'g';
      else if (!strcmp (argv [i], "--record-every-th") || !strcmp (argv [i], "-y"))
	need_arg = 'y';
      else if (!strcmp (argv [i], "--output") || !strcmp (argv [i], "-o"))
	need_arg = 'o';
      else if (!strcmp (argv [i], "--take-screenshot")
	  || !strcmp (argv [i], "-s"))
	act = SCREENSHOT;
      else if (!strcmp (argv [i], "--dump-info")
	       || !strcmp (argv [i], "-d"))
	act = DUMP_INFO;
      else if (!strcmp (argv [i], "--help")
	       || !strcmp (argv [i], "-h"))
	print_help_and_exit ();
      else
	{
	  fprintf (stderr, "option '%s' not recognized\n", argv [i]);
	  print_help_and_exit ();
	}
    }

  if (need_arg)
    {
      fprintf (stderr, "option '%c' requires an argument\n", need_arg);
      print_help_and_exit ();
    }

  if (act == SCREENSHOT || act == RECORD)
    {
      if (geometry)
	{
	  while (*geometry)
	    {
	      if (isdigit (*geometry))
		{
		  if (x == -1)
		    x = *geometry-'0';
		  else if (y == -1)
		    x = x*10+*geometry-'0';
		  else if (w == -1)
		    y = y*10+*geometry-'0';
		  else if (h == -1)
		    w = w*10+*geometry-'0';
		  else
		    h = h*10+*geometry-'0';
		}
	      else if (*geometry == ',')
		{
		  if (x == -1)
		    {
		      fprintf (stderr, "wrong syntax for -g option\n");
		      print_help_and_exit ();
		    }
		  else if (y == -1)
		    y = 0;
		  else if (w == -1)
		    w = 0;
		  else
		    {
		      fprintf (stderr, "wrong syntax for -g option\n");
		      print_help_and_exit ();
		    }
		}
	      else if (*geometry == 'x' || *geometry == 'X')
		{
		  if (w == -1 || h != -1)
		    {
		      fprintf (stderr, "wrong syntax for -g option\n");
		      print_help_and_exit ();
		    }

		  h = 0;
		}
	      else
		{
		  fprintf (stderr, "wrong syntax for -g option\n");
		  print_help_and_exit ();
		}

	      geometry++;
	    }
	}

      x = x < 0 ? 0 : x;
      y = y < 0 ? 0 : y;
    }

  /*fprintf (stderr, "x = %d y = %d w = %d h = %d\n", x, y, w, h);*/

  if (act == DUMP_INFO)
    dump_drm_info_and_exit ();

  if (act == SCREENSHOT)
    take_screenshot_and_exit (x, y, w, h);

  if (act == RECORD)
    {
      if (!output)
	{
	  fprintf (stderr, "for recording, you must provide an output file with "
		   "-o or --output\n");
	  print_help_and_exit ();
	}

      record_screen_and_exit (output, preset, x, y, w, h, record_interv);
    }

  return 0;
}
