/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2005 Red Hat, Inc.
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is Red Hat, Inc.
 *
 * Contributor(s):
 *	Owen Taylor <otaylor@redhat.com>
 *	Stuart Parmenter <stuart@mozilla.com>
 *	Vladimir Vukicevic <vladimir@pobox.com>
 */

#define WIN32_LEAN_AND_MEAN

/* We require Windows 2000 features such as ETO_PDY */
#if !defined(WINVER) || (WINVER < 0x0500)
# define WINVER 0x0500
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0500)
# define _WIN32_WINNT 0x0500
#endif

#include "cairoint.h"

#include "cairo-atomic-private.h"
#include "cairo-device-private.h"
#include "cairo-win32-private.h"

#include <wchar.h>
#include <windows.h>

#ifdef CAIRO_WIN32_DIRECT2D
# include <d3d11.h>
# include <d2d1_1.h>
# include <dxgi.h>
#endif

#ifdef CAIRO_WIN32_DIRECT2D
void Cairo_d2d_init (cairo_win32_device_t *device)
{
  HRESULT hr;
  D2D1_FACTORY_OPTIONS d2d_opt = {D2D1_DEBUG_LEVEL_NONE};

  CoInitializeEx (NULL, COINIT_MULTITHREADED);

  /* We are going to depend on Windows 7 with the platform update and later here... */
  hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_MULTI_THREADED, /* We want multithreaded support */
                          &IID_ID2D1Factory1, /* We want a D2D 1.1 factory */
                          &d2d_opt,
                          &device->d2d_factory);

  if (SUCCEEDED (hr))
    {
      /* First, get the D3D11 device for the default display adapter */
      UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

      D3D_FEATURE_LEVEL featureLevels[] =
        {
          D3D_FEATURE_LEVEL_11_1,
          D3D_FEATURE_LEVEL_11_0,
          D3D_FEATURE_LEVEL_10_1,
          D3D_FEATURE_LEVEL_10_0,
          D3D_FEATURE_LEVEL_9_3,
          D3D_FEATURE_LEVEL_9_2,
          D3D_FEATURE_LEVEL_9_1
        };

      hr = D3D11CreateDevice (NULL, /* default display adapter */
                              D3D_DRIVER_TYPE_HARDWARE, /* hardware! */
                              NULL, /* hardware! */
                              creationFlags, /* We need to be D2D compatible */
                              featureLevels, /* list of feature levels this app can support */
                              7, /* number of possible feature levels */
                              D3D11_SDK_VERSION, /* We must specify this here */
                              &device->d3d_device, /* returns the Direct3D device created */
                              NULL, /* returns feature level of device created (not for now) */
                              &device->d3d_device_ctx /* returns the device immediate context */);
    }
  else
    device->d2d_factory = NULL;

  if (SUCCEEDED (hr))
    /* Next, get the DXGI device that is used by the D3D11 Device */
    hr = ID3D11Device_QueryInterface (device->d3d_device,
                                      &IID_IDXGIDevice,
                                      &device->dxgi_device);
  else
    {
      device->d3d_device = NULL;
      device->d3d_device_ctx = NULL;
    }

  if (SUCCEEDED (hr))
    /* Now create the D2D device from the DXGI device we have */
	hr = ID2D1Factory1_CreateDevice (device->d2d_factory,
                                     device->dxgi_device,
                                     &device->d2d_device);
  else
    device->dxgi_device = NULL;

  if (SUCCEEDED (hr))
    hr = CoCreateInstance (&CLSID_WICImagingFactory,
                           NULL,
                           CLSCTX_INPROC_SERVER,
                           &IID_IWICImagingFactory,
                           &device->wic_factory);
  else
    device->d2d_device = NULL;

  if (SUCCEEDED (hr))
    /* Create the D2D device context from the D2D device, for our DXGI swapchain */
    hr = ID2D1Device_CreateDeviceContext (device->d2d_device,
                                          D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,
                                          &device->d2d_device_ctx);
  else
    device->wic_factory = NULL;

  if (SUCCEEDED (hr))
    device->fallback_gdi = FALSE;
  else
    {
      device->d2d_device_ctx = NULL;

      /* on failure, destroy all D3D/DXGI/D2D/WIC items we might have created */
      if (device->wic_factory)
        {
          IWICImagingFactory_Release (device->wic_factory);
          device->wic_factory = NULL;
        }
      if (device->d2d_device)
        {
          ID2D1Device_Release (device->d2d_device);
          device->d2d_device = NULL;
        }
      if (device->d2d_device)
        {
          IDXGIDevice_Release (device->dxgi_device);
          device->dxgi_device = NULL;
        }
      if (device->d3d_device_ctx)
        {
          ID3D11DeviceContext_Release (device->d3d_device_ctx);
          device->d3d_device_ctx = NULL;
        }
      if (device->d3d_device)
        {
          ID3D11Device_Release (device->d3d_device);
          device->d3d_device = NULL;
        }
      if (device->d2d_factory)
        {
          ID2D1Factory1_Release (device->d2d_factory);
          device->d2d_factory = NULL;
        }

      device->fallback_gdi = TRUE;
    }
}
#else
/* do nothing when Direct2D is not enabled besides enforcing GDI! */
void Cairo_d2d_init (cairo_win32_device_t *device)
{
  device->fallback_gdi = TRUE;
}
#endif

static cairo_device_t *__cairo_win32_device;

static cairo_status_t
_cairo_win32_device_flush (void *device)
{
    GdiFlush ();
    return CAIRO_STATUS_SUCCESS;
}

static void
_cairo_win32_device_finish (void *device)
{
	printf ("finish device!\n");
}

static void
_cairo_win32_device_destroy (void *device)
{
	printf ("destroy device!\n");
    free (device);
}

static const cairo_device_backend_t _cairo_win32_device_backend = {
    CAIRO_DEVICE_TYPE_WIN32,

    NULL, NULL, /* lock, unlock */

    _cairo_win32_device_flush,
    _cairo_win32_device_finish,
    _cairo_win32_device_destroy,
};

#if 0
D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
								   D2D1::PixelFormat(
										     DXGI_FORMAT_B8G8R8A8_UNORM,
										     D2D1_ALPHA_MODE_IGNORE),
								   0,
								   0,
								   D2D1_RENDER_TARGET_USAGE_NONE,
								   D2D1_FEATURE_LEVEL_DEFAULT
								  );

hr = m_pD2DFactory->CreateDCRenderTarget(&props, &device->d2d);
#endif

static cairo_bool_t is_win98 (void)
{
    OSVERSIONINFO os;

    os.dwOSVersionInfoSize = sizeof (os);
    GetVersionEx (&os);

    return (VER_PLATFORM_WIN32_WINDOWS == os.dwPlatformId &&
	    os.dwMajorVersion == 4 &&
	    os.dwMinorVersion == 10);
}

static void *
_cairo_win32_device_get_alpha_blend (cairo_win32_device_t *device)
{
    void *func = NULL;

    if (is_win98 ())
	return NULL;

    device->msimg32_dll = LoadLibraryW (L"msimg32");
    if (device->msimg32_dll)
	func = GetProcAddress (device->msimg32_dll, "AlphaBlend");

    return func;
}

cairo_device_t *
_cairo_win32_device_get (void)
{
    cairo_win32_device_t *device;

    CAIRO_MUTEX_INITIALIZE ();

    if (__cairo_win32_device)
	return cairo_device_reference (__cairo_win32_device);

    device = _cairo_malloc (sizeof (*device));

    _cairo_device_init (&device->base, &_cairo_win32_device_backend);

    device->compositor = _cairo_win32_gdi_compositor_get ();

    device->msimg32_dll = NULL;
    device->alpha_blend = _cairo_win32_device_get_alpha_blend (device);

    /* Establish the ID2D1Factory for our device, if Direct2D enabled */
    Cairo_d2d_init (device);

    if (_cairo_atomic_ptr_cmpxchg ((void **)&__cairo_win32_device, NULL, device))
	return cairo_device_reference(&device->base);

    _cairo_win32_device_destroy (device);
    return cairo_device_reference (__cairo_win32_device);
}

unsigned
_cairo_win32_flags_for_dc (HDC dc, cairo_format_t format)
{
    uint32_t flags = 0;
    cairo_bool_t is_display = GetDeviceCaps(dc, TECHNOLOGY) == DT_RASDISPLAY;

    if (format == CAIRO_FORMAT_RGB24 || format == CAIRO_FORMAT_ARGB32)
    {
	int cap = GetDeviceCaps(dc, RASTERCAPS);
	if (cap & RC_BITBLT)
	    flags |= CAIRO_WIN32_SURFACE_CAN_BITBLT;
	if (!is_display && GetDeviceCaps(dc, SHADEBLENDCAPS) != SB_NONE)
	    flags |= CAIRO_WIN32_SURFACE_CAN_ALPHABLEND;

	/* ARGB32 available operations are a strict subset of RGB24
	 * available operations. This is because the same GDI functions
	 * can be used but most of them always reset alpha channel to 0
	 * which is bad for ARGB32.
	 */
	if (format == CAIRO_FORMAT_RGB24)
	{
	    flags |= CAIRO_WIN32_SURFACE_CAN_RGB_BRUSH;
	    if (cap & RC_STRETCHBLT)
		flags |= CAIRO_WIN32_SURFACE_CAN_STRETCHBLT;
	    if (cap & RC_STRETCHDIB)
		flags |= CAIRO_WIN32_SURFACE_CAN_STRETCHDIB;
	}
    }

    if (is_display) {
	flags |= CAIRO_WIN32_SURFACE_IS_DISPLAY;

	/* These will always be possible, but the actual GetDeviceCaps
	 * calls will return whether they're accelerated or not.
	 * We may want to use our own (pixman) routines sometimes
	 * if they're eventually faster, but for now have GDI do
	 * everything.
	 */
#if 0
	flags |= CAIRO_WIN32_SURFACE_CAN_BITBLT;
	flags |= CAIRO_WIN32_SURFACE_CAN_ALPHABLEND;
	flags |= CAIRO_WIN32_SURFACE_CAN_STRETCHBLT;
	flags |= CAIRO_WIN32_SURFACE_CAN_STRETCHDIB;
#endif
    }

    return flags;
}
