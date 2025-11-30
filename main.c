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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>



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
dump_tiledx4kb_pixels (char *buf, int w, int h, int p, enum pixel_format pf)
{
  char *out = malloc_and_check (w*h*3);
  int i, x, y, srcind, destind;

  for (x = 0; x < w; x++)
    {
      for (y = 0; y < h; y++)
	{
	  destind = y*w*3+x*3;
	  srcind = y/8*4096*(p/512)+x/128*4096+(y%8)*512+(x%128)*4;

	  out [destind] = buf [srcind+2];
	  out [destind+1] = buf [srcind+1];
	  out [destind+2] = buf [srcind];
	}
    }

  for (i = 0; i < w*h*3; i++)
      putchar (out [i]);
}


void
print_help_and_exit (void)
{
  printf ("options:\n"
	  "\t--take-screenshot or -s:   take a screenshot and print "
	  "the binary data to stdout in binary PPM format\n"
	  "\t--dump-info or -d:         dump info about your DRM setup\n");
  exit (0);
}


int
main (int argc, char *argv [])
{
  drmDevice **devs;
  drmModeRes *res;
  drmModeCrtc *crtc;
  drmModeFB2 *fb2;
  struct stat statbuf;
  enum pixel_format pf;
  enum pixel_order po;
  int devsnum, fd, dmabuf_fd, i, take_screenshot = 0, pixel_format;
  long mod;
  char *buf;


  for (i = 1; i < argc; i++)
    {
      if (!strcmp (argv [i], "--take-screenshot")
	  || !strcmp (argv [i], "-s"))
	take_screenshot = 1;
      else if (!strcmp (argv [i], "--dump-info")
	       || !strcmp (argv [i], "-d"))
	take_screenshot = 0;
      else
	{
	  fprintf (stderr, "option '%s' not recognized\n", argv [i]);
	  print_help_and_exit ();
	}
    }

  if (!take_screenshot)
    dump_drm_info_and_exit ();

  devs = get_devices (&devsnum);

  fd = open (devs [0]->nodes [DRM_NODE_PRIMARY], O_RDONLY);

  if (fd < 0)
    {
      fprintf (stderr, "couldn't open video card %d (%s)\n", 0,
	      devs [0]->nodes [DRM_NODE_PRIMARY]);
      exit (1);
    }

  res = drmModeGetResources (fd);

  if (!res)
    {
      fprintf (stderr, "couldn't inspect video card\n");
      exit (1);
    }

  crtc = drmModeGetCrtc (fd, res->crtcs[0]);

  if (!crtc)
    {
      fprintf (stderr, "could not access crtc number 0\n");
      exit (1);
    }

  fb2 = drmModeGetFB2 (fd, crtc->buffer_id);

  if (!fb2)
    {
      fprintf (stderr, "could not inspect framebuffer\n");
      exit (1);
    }

  if (drmPrimeHandleToFD (fd, fb2->handles [0], 0, &dmabuf_fd))
    {
      fprintf (stderr, "couldn't get file descriptor for this framebuffer, "
	      "maybe you lack permissions?\n");
      exit (1);
    }

  fprintf (stderr, "selecting first plane of first framebuffer of first crtc of "
	   "first video card...\n");


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
      dump_tiledx4kb_pixels (buf, fb2->width, fb2->height, fb2->pitches [0], pf);
      break;
    }

  return 0;
}
