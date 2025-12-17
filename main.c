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
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/mman.h>

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
convert_tiledx4kb_pixels_to_linear (unsigned char *out, char *in, int w, int h,
				    int p, enum pixel_format pf)
{
  int destind = 0, srcind, x, y;

  for (y = 0; y < h; y++)
    {
      for (x = 0; x < w; x++)
	{
	  srcind = y/8*4096*(p/512)+x/128*4096+(y%8)*512+(x%128)*4;

	  out [destind] = in [srcind+2];
	  out [destind+1] = in [srcind+1];
	  out [destind+2] = in [srcind];

	  destind += 3;
	}
    }
}


void
dump_tiledx4kb_pixels_linearly (char *buf, int w, int h, int p, enum pixel_format pf)
{
  int i;
  unsigned char *out = malloc_and_check (w*h*3);

  convert_tiledx4kb_pixels_to_linear (out, buf, w, h, p, pf);

  for (i = 0; i < w*h*3; i++)
    putchar (out [i]);

  free (out);
}


int
open_framebuffer (drmModeFB2 **fb2, int *cardfd)
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
take_screenshot_and_exit (void)
{
  drmModeFB2 *fb2;
  struct stat statbuf;
  enum pixel_format pf;
  enum pixel_order po;
  long mod;
  char *buf;
  int dmabuf_fd, cardfd, pixel_format;


  dmabuf_fd = open_framebuffer (&fb2, &cardfd);


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


  printf ("P6\n%d\n%d\n255\n", fb2->width, fb2->height);

  switch (po)
    {
    case LINEAR:
      dump_linear_pixels (buf, fb2->width, fb2->height, fb2->pitches [0], pf);
      break;
    case TILEDX_4KB:
      dump_tiledx4kb_pixels_linearly (buf, fb2->width, fb2->height,
				      fb2->pitches [0], pf);
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
write_minimal_matroska_header (int outfd, int width, int height,
			       x264_nal_t headers [], int headers_num)
{
  x264_nal_t *sps = NULL, *pps = NULL;
  int i, j, header_sz, avcrec_sz;
  unsigned char header_start []
    = {0x1a, 0x45, 0xdf, 0xa3, 0xa3, /* ebml header */
       0x42, 0x86, 0x81, 0x01,
       0x42, 0xf7, 0x81, 0x01,
       0x42, 0xf2, 0x81, 0x04,
       0x42, 0xf3, 0x81, 0x08,
       0x42, 0x82, 0x88, 'm', 'a', 't', 'r', 'o', 's', 'k', 'a',
       0x42, 0x87, 0x81, 0x04,
       0x42, 0x85, 0x81, 0x02,

       0x18, 0x53, 0x80, 0x67, 0xff, /* segment */

       0x16, 0x54, 0xae, 0x6b, 0x00, /* all video tracks */
       0xae, 0x00, /* track entry */
       0xd7, 0x81, 0x1, /* track number */
       0x73, 0xc5, 0x81, 0x1, /* track uid */
       0x83, 0x81, 0x1, /* track type */
       0xe0, 0x88, /* video settings */
       0xb0, 0x82, 0x00, 0x00, 0xba, 0x82, 0x00, 0x00, /* pixel width and height */
       0x86, 0x8f, 'V', '_', 'M', 'P', 'E', 'G', '4', '/' , 'I', 'S', 'O',
       '/', 'A', 'V', 'C', /* codec id */
       0x63, 0xa2, 0x00, /* codec private */
       /* beginning of AVCDecoderConfigurationRecord */
       0x01, 0x42, 0xc0, 0x1f,
       0xff};
  unsigned char other_headers []
    = {0x15, 0x49, 0xa9, 0x66, 0x9f, /* info header */
       0x2a, 0xd7, 0xb1, 0x83, 0x0f, 0x42, 0x40, /* timestamp scale */
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

  header_sz = sizeof (header_start) / sizeof (header_start [0])
    +3+sps->i_payload+3+pps->i_payload+sizeof (other_headers)
    / sizeof (other_headers [0]);
  header = malloc_and_check (header_sz);

  for (i = 0; i < sizeof (header_start) / sizeof (header_start [0]); i++)
    {
      header [i] = header_start [i];
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

  avcrec_sz = 8+sps->i_payload+3+pps->i_payload;

  if (avcrec_sz > 126)
    {
      fprintf (stderr, "avcrec_sz too big\n");
      exit (1);
    }

  header [91] = 0x80 | avcrec_sz;

  header [66] = (width & 0xff00) >> 8;
  header [67] = width & 0xff;
  header [70] = (height & 0xff00) >> 8;
  header [71] = height & 0xff;

  if (avcrec_sz+40 > 126)
    {
      fprintf (stderr, "track entry too big\n");
      exit (1);
    }

  header [51] = 0x80 | (avcrec_sz+40);

  if (avcrec_sz+42 > 126)
    {
      fprintf (stderr, "tracks too big\n");
      exit (1);
    }

  header [49] = 0x80 | (avcrec_sz+42);

  for (j = 0; j < sizeof (other_headers) / sizeof (other_headers [0]); j++)
    {
      header [i++] = other_headers [j];
    }

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
write_cluster_header (int outfd, int timestamp)
{
  int i;
  unsigned char cluster_header [] =
    {0x1f, 0x43, 0xb6, 0x75, 0xff, 0xff, 0xff, 0xff, /* cluster header */
     0xe7, 0x84 /* timestamp */ };

  for (i = 0; i < sizeof (cluster_header); i++)
    {
      write_char (outfd, cluster_header [i]);
    }

  write_int32_bigend (outfd, timestamp);
}


void
record_screen_and_exit (char *output, char *preset, int recording_interval)
{
  x264_param_t par;
  x264_picture_t inframe, outframe;
  x264_nal_t *nal, *headers;
  x264_t *enc;
  drmModeFB2 *fb2;
  drmVBlank vbl = {{DRM_VBLANK_RELATIVE, 1}};
  struct stat statbuf;
  struct pollfd pfd = {0, POLLIN};
  off_t off;
  char *buf;
  unsigned char *out;
  int w, h, outfd, dmabuf_fd, cardfd, num_frames_within_cluster, outsz, i_nal,
    headers_num, timestamp_of_cluster, timestamp_within_cluster, cluster_size,
    last_vblank = -1;

  dmabuf_fd = open_framebuffer (&fb2, &cardfd);

  w = fb2->width;
  h = fb2->height;

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
  /*par.b_repeat_headers = 1;*/
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

  write_minimal_matroska_header (outfd, w, h, headers, headers_num);

  timestamp_of_cluster = 0;
  write_cluster_header (outfd, timestamp_of_cluster);
  num_frames_within_cluster = 0;
  timestamp_within_cluster = 0;
  cluster_size = 6;

  out = malloc_and_check (w*h*3);

  /*if (clock_gettime (CLOCK_REALTIME, &latest_ts) < 0)
    {
      fprintf (stderr, "couldn't read system clock\n");
      exit (1);
      }*/

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

      convert_tiledx4kb_pixels_to_linear (out, buf, w, h, fb2->pitches [0], 0);

      inframe.img.plane [0] = out;
      inframe.i_pts = num_frames_within_cluster;

      outsz = x264_encoder_encode (enc, &nal, &i_nal, &inframe, &outframe);

      if (outsz < 0)
	{
	  fprintf (stderr, "couldn't encode framebuffer content\n");
	  exit (1);
	}
      else if (outsz)
	{
	  if (outsz+4 > 1048575)
	    fprintf (stderr, "skipping this frame because size (%d) is too "
		     "big\n", outsz);
	  else
	    {
	      timestamp_within_cluster = num_frames_within_cluster*17;

	      if (0x7fff < timestamp_within_cluster)
		{
		  off = lseek (outfd, 0, SEEK_CUR);

		  lseek (outfd, -cluster_size-4, SEEK_CUR);
		  write_int32_bigend (outfd, 0x10000000 | cluster_size);

		  lseek (outfd, off, SEEK_SET);
		  timestamp_of_cluster += timestamp_within_cluster;
		  write_cluster_header (outfd, timestamp_of_cluster);
		  num_frames_within_cluster = 0;
		  timestamp_within_cluster = 0;
		  cluster_size = 6;
		}

	      write_char (outfd, 0xa3);
	      write_char (outfd, 0x20 | (((outsz+4) >> 16) & 0xff));
	      write_char (outfd, ((outsz+4) >> 8) & 0xff);
	      write_char (outfd, (outsz+4) & 0xff);

	      /*fprintf (stderr, "timestamp = %d\n", timestamp);*/

	      write_char (outfd, 0x81);
	      write_char (outfd, ((timestamp_within_cluster>>8) & 0xff));
	      write_char (outfd, timestamp_within_cluster & 0xff);
	      write_char (outfd, 0);

	      if (write (outfd, nal->p_payload, outsz) != outsz)
		{
		  fprintf (stderr, "couldn't encode framebuffer content\n");
		  exit (1);
		}

	      cluster_size += outsz + 8;
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

  fprintf (stderr, "finishing...\n");

  lseek (outfd, -cluster_size-4, SEEK_CUR);
  write_int32_bigend (outfd, 0x10000000 | cluster_size);

  exit (0);
}


void
print_help_and_exit (void)
{
  printf ("options:\n"
	  "\t--record-screen or -r:     record screen and print the binary data "
	  "to stdout in MKV format\n"
	  "\t--preset or -p PRESET:     select a preset when recording screen, "
	  "default is medium\n"
	  "\t--record-every-th or -y N  record one frame every N, defaults to one "
	  "for recording at native refresh rate\n"
	  "\t--output or -o FILE:       output file, required for recording\n"
	  "\t--take-screenshot or -s:   take a screenshot and print "
	  "the data to stdout in binary PPM format\n"
	  "\t--dump-info or -d:         dump info about your DRM setup\n"
	  "\t--help or -h:              print this help and exit\n");
  exit (0);
}


int
main (int argc, char *argv [])
{
  enum action act = DUMP_INFO;
  char *preset = "medium", *output = NULL;
  int i, need_arg = 0, record_interv = 1;


  for (i = 1; i < argc; i++)
    {
      if (need_arg)
	{
	  switch (need_arg)
	    {
	    case 'p':
	      preset = argv [i];
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

  if (act == DUMP_INFO)
    dump_drm_info_and_exit ();

  if (act == SCREENSHOT)
    take_screenshot_and_exit ();

  if (act == RECORD)
    {
      if (!output)
	{
	  fprintf (stderr, "for recording, you must provide an output file with "
		   "-o or --output\n");
	  print_help_and_exit ();
	}

      record_screen_and_exit (output, preset, record_interv);
    }

  return 0;
}
