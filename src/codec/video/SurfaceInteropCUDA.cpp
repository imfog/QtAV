/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2015 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "SurfaceInteropCUDA.h"
#include "QtAV/VideoFrame.h"
#include "utils/Logger.h"
#include "helper_cuda.h"

#define WORKAROUND_UNMAP_CONTEXT_SWITCH 1
#define USE_STREAM 1

namespace QtAV {
namespace cuda {

InteropResource::InteropResource(CUdevice d, CUvideodecoder decoder, CUvideoctxlock declock)
    : cuda_api()
    , dev(d)
    , ctx(NULL)
    , dec(decoder)
    , lock(declock)
{
    memset(res, 0, sizeof(res));
}

InteropResource::~InteropResource()
{
    //CUDA_WARN(cuCtxPushCurrent(ctx)); //error invalid value
    if (res[0].cuRes)
        CUDA_WARN(cuGraphicsUnregisterResource(res[0].cuRes));
    if (res[1].cuRes)
        CUDA_WARN(cuGraphicsUnregisterResource(res[1].cuRes));
    if (res[0].stream)
        CUDA_WARN(cuStreamDestroy(res[0].stream));
    if (res[1].stream)
        CUDA_WARN(cuStreamDestroy(res[1].stream));

    // FIXME: we own the context. But why crash to destroy ctx? CUDA_ERROR_INVALID_VALUE
    //CUDA_ENSURE(cuCtxDestroy(ctx));
}

void* InteropResource::mapToHost(const VideoFormat &format, void *handle, int picIndex, const CUVIDPROCPARAMS &param, int width, int height, int coded_height)
{
    AutoCtxLock locker((cuda_api*)this, lock);
    Q_UNUSED(locker);
    CUdeviceptr devptr;
    unsigned int pitch;

    CUDA_ENSURE(cuvidMapVideoFrame(dec, picIndex, &devptr, &pitch, const_cast<CUVIDPROCPARAMS*>(&param)), NULL);
    CUVIDAutoUnmapper unmapper(this, dec, devptr);
    Q_UNUSED(unmapper);
    uchar* host_data = NULL;
    const size_t host_size = pitch*coded_height*3/2;
    CUDA_ENSURE(cuMemAllocHost((void**)&host_data, host_size), NULL);
    // copy to the memory not allocated by cuda is possible but much slower
    CUDA_ENSURE(cuMemcpyDtoH(host_data, devptr, host_size), NULL);

    VideoFrame frame(width, height, VideoFormat::Format_NV12);
    uchar *planes[] = {
        host_data,
        host_data + pitch * coded_height
    };
    frame.setBits(planes);
    int pitches[] = { (int)pitch, (int)pitch };
    frame.setBytesPerLine(pitches);

    VideoFrame *f = reinterpret_cast<VideoFrame*>(handle);
    frame.setTimestamp(f->timestamp());
    frame.setDisplayAspectRatio(f->displayAspectRatio());
    if (format == frame.format())
        *f = frame.clone();
    else
        *f = frame.to(format);

    cuMemFreeHost(host_data);
    return f;
}

void SurfaceInteropCUDA::setSurface(int picIndex, CUVIDPROCPARAMS param, int width, int height, int coded_height)
{
    m_index = picIndex;
    m_param = param;
    w = width;
    h = height;
    H = coded_height;
}

void* SurfaceInteropCUDA::map(SurfaceType type, const VideoFormat &fmt, void *handle, int plane)
{
    Q_UNUSED(fmt);
    if (m_resource.isNull())
        return NULL;
    if (!handle)
        return NULL;

    if (m_index < 0)
        return 0;
    if (type == GLTextureSurface) {
        // FIXME: to strong ref may delay the delete and cuda resource maybe already destoryed after strong ref is finished
        if (m_resource.toStrongRef()->map(m_index, m_param, *((GLuint*)handle), w, h, H, plane))
            return handle;
    } else if (type == HostMemorySurface) {
        return m_resource.toStrongRef()->mapToHost(fmt, handle, m_index, m_param, w, h, H);
    }
    return NULL;
}

void SurfaceInteropCUDA::unmap(void *handle)
{
    if (m_resource.isNull())
        return;
    // FIXME: to strong ref may delay the delete and cuda resource maybe already destoryed after strong ref is finished
    m_resource.toStrongRef()->unmap(*((GLuint*)handle));
}
} //namespace cuda
} //namespace QtAV

#if QTAV_HAVE(CUDA_EGL)
#define EGL_ENSURE(x, ...) \
    do { \
        if (!(x)) { \
            EGLint err = eglGetError(); \
            qWarning("EGL error@%d<<%s. " #x ": %#x %s", __LINE__, __FILE__, err, eglQueryString(eglGetCurrentDisplay(), err)); \
            return __VA_ARGS__; \
        } \
    } while(0)

#if QTAV_HAVE(GUI_PRIVATE)
#include <qpa/qplatformnativeinterface.h>
#include <QtGui/QGuiApplication>
#endif //QTAV_HAVE(GUI_PRIVATE)
#ifdef QT_OPENGL_ES_2_ANGLE_STATIC
#define CAPI_LINK_EGL
#else
#define EGL_CAPI_NS
#endif //QT_OPENGL_ES_2_ANGLE_STATIC
#include "capi/egl_api.h"
#include <EGL/eglext.h> //include after egl_capi.h to match types
#define DX_LOG_COMPONENT "CUDA.D3D"
#include "utils/DirectXHelper.h"

namespace QtAV {
namespace cuda {
class EGL {
public:
    EGL() : dpy(EGL_NO_DISPLAY), surface(EGL_NO_SURFACE) {}
    EGLDisplay dpy;
    EGLSurface surface; //only support rgb. then we must use CUDA kernel
#ifdef EGL_VERSION_1_5
    // eglCreateImageKHR does not support EGL_NATIVE_PIXMAP_KHR, only 2d, 3d, render buffer
    //EGLImageKHR image[2];
    //EGLImage image[2]; //not implemented yet
#endif //EGL_VERSION_1_5
};

EGLInteropResource::EGLInteropResource(CUdevice d, CUvideodecoder decoder, CUvideoctxlock declock)
    : InteropResource(d, decoder, declock)
    , egl(new EGL())
    , dll9(NULL)
    , d3d9(NULL)
    , device9(NULL)
    , texture9(NULL)
    , surface9(NULL)
    , texture9_nv12(NULL)
    , surface9_nv12(NULL)
    , query9(NULL)
{
}


EGLInteropResource::~EGLInteropResource()
{
    releaseEGL();
    if (egl) {
        delete egl;
        egl = NULL;
    }
    SafeRelease(&query9);
    SafeRelease(&surface9_nv12);
    SafeRelease(&texture9_nv12);
    SafeRelease(&surface9);
    SafeRelease(&texture9);
    SafeRelease(&device9);
    SafeRelease(&d3d9);
    if (dll9)
        FreeLibrary(dll9);
}

bool EGLInteropResource::ensureD3DDevice()
{
    if (device9)
        return true;
    if (!dll9)
        dll9 = LoadLibrary(TEXT("D3D9.DLL"));
    if (!dll9) {
        qWarning("cuda::EGLInteropResource cannot load d3d9.dll");
        return false;
    }
    D3DADAPTER_IDENTIFIER9 ai9;
    ZeroMemory(&ai9, sizeof(ai9));
    device9 = DXHelper::CreateDevice9Ex(dll9, (IDirect3D9Ex**)(&d3d9), &ai9);
    if (!device9) {
        qWarning("Failed to create d3d9 device ex, fallback to d3d9 device");
        device9 = DXHelper::CreateDevice9(dll9, &d3d9, &ai9);
    }
    if (!device9)
        return false;
    qDebug() << QString().sprintf("CUDA.D3D9 (%.*s, vendor %lu, device %lu, revision %lu)",
                                    sizeof(ai9.Description), ai9.Description,
                                    ai9.VendorId, ai9.DeviceId, ai9.Revision);

    // move to ensureResouce
    DX_ENSURE(device9->CreateQuery(D3DQUERYTYPE_EVENT, &query9), false);
    query9->Issue(D3DISSUE_END);
    return !!device9;
}

void EGLInteropResource::releaseEGL() {
    if (egl->surface != EGL_NO_SURFACE) {
        eglReleaseTexImage(egl->dpy, egl->surface, EGL_BACK_BUFFER);
        eglDestroySurface(egl->dpy, egl->surface);
        egl->surface = EGL_NO_SURFACE;
    }
}

bool EGLInteropResource::ensureResource(int w, int h, int ch, GLuint tex)
{
    TexRes &r = res[0];// 1 NV12 texture
    if (ensureD3D9CUDA(w, h, ch) && ensureD3D9EGL(w, h)) {
        r.texture = tex;
        r.w = w;
        r.h = h;
        r.H = ch;
        return true;
    }
    releaseEGL();
    //releaseDX();
    SafeRelease(&query9);
    SafeRelease(&surface9);
    SafeRelease(&texture9);
    SafeRelease(&surface9_nv12);
    SafeRelease(&texture9_nv12);
    return false;
}

bool EGLInteropResource::ensureD3D9CUDA(int w, int h, int ch)
{
    TexRes &r = res[0];// 1 NV12 texture
    if (r.w == w && r.h == h && r.H == ch && r.cuRes)
        return true;
    if (!ctx) {
        // TODO: how to use pop/push decoder's context without the context in opengl context
        if (!ensureD3DDevice())
            return false;
        // CUdevice is different from decoder's
        CUDA_ENSURE(cuD3D9CtxCreate(&ctx, &dev, CU_CTX_SCHED_BLOCKING_SYNC, device9), false);
#if USE_STREAM
        CUDA_WARN(cuStreamCreate(&res[0].stream, CU_STREAM_DEFAULT));
        CUDA_WARN(cuStreamCreate(&res[1].stream, CU_STREAM_DEFAULT));
#endif //USE_STREAM
        qDebug("cuda contex on gl thread: %p", ctx);
        CUDA_ENSURE(cuCtxPopCurrent(&ctx), false); // TODO: why cuMemcpy2D need this
    }
    if (r.cuRes) {
        CUDA_ENSURE(cuGraphicsUnregisterResource(r.cuRes), false);
        r.cuRes = NULL;
    }

    // create d3d resource for interop
    if (!surface9_nv12) {
        // TODO: need pitch from cuvid to ensure cuMemcpy2D can copy the whole pitch
        DX_ENSURE(device9->CreateTexture(w
                                         //, h
                                         , h*3/2
                                         , 1
                                         , 0 // 0 is from NV example. cudaD3D9.h says The primary rendertarget may not be registered with CUDA. So can not be D3DUSAGE_RENDERTARGET?
                                         //, D3DUSAGE_RENDERTARGET
                                         , D3DFMT_L8
                                         //, (D3DFORMAT)MAKEFOURCC('N','V','1','2') // can not create nv12. use 2 textures L8+A8L8?
                                         , D3DPOOL_DEFAULT
                                         , &texture9_nv12
                                         , NULL) // - Resources allocated as shared may not be registered with CUDA.
                  , false);
        DX_ENSURE(texture9_nv12->GetSurfaceLevel(0, &surface9_nv12), false);
    }

    // TODO: cudaD3D9.h says NV12 is not supported
    // CUDA_ERROR_INVALID_HANDLE if register D3D9 surface
    CUDA_ENSURE(cuGraphicsD3D9RegisterResource(&r.cuRes, texture9_nv12, CU_GRAPHICS_REGISTER_FLAGS_NONE), false);
    return true;
}

bool EGLInteropResource::ensureD3D9EGL(int w, int h) {
    if (surface9 && res[0].w == w && res[0].h == h)
        return true;
#if QTAV_HAVE(GUI_PRIVATE)
    QPlatformNativeInterface *nativeInterface = QGuiApplication::platformNativeInterface();
    egl->dpy = static_cast<EGLDisplay>(nativeInterface->nativeResourceForContext("eglDisplay", QOpenGLContext::currentContext()));
    EGLConfig egl_cfg = static_cast<EGLConfig>(nativeInterface->nativeResourceForContext("eglConfig", QOpenGLContext::currentContext()));
#else
#ifdef Q_OS_WIN
#if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
#ifdef _MSC_VER
#pragma message("ANGLE version in Qt<5.5 does not support eglQueryContext. You must upgrade your runtime ANGLE libraries")
#else
#warning "ANGLE version in Qt<5.5 does not support eglQueryContext. You must upgrade your runtime ANGLE libraries"
#endif //_MSC_VER
#endif
#endif //Q_OS_WIN
    // eglQueryContext() added (Feb 2015): https://github.com/google/angle/commit/8310797003c44005da4143774293ea69671b0e2a
    egl->dpy = eglGetCurrentDisplay();
    qDebug("EGL version: %s, client api: %s", eglQueryString(egl->dpy, EGL_VERSION), eglQueryString(egl->dpy, EGL_CLIENT_APIS));
    // TODO: check runtime egl>=1.4 for eglGetCurrentContext()
    EGLint cfg_id = 0;
    EGL_ENSURE(eglQueryContext(egl->dpy, eglGetCurrentContext(), EGL_CONFIG_ID , &cfg_id) == EGL_TRUE, false);
    qDebug("egl config id: %d", cfg_id);
    EGLint nb_cfg = 0;
    EGL_ENSURE(eglGetConfigs(egl->dpy, NULL, 0, &nb_cfg) == EGL_TRUE, false);
    qDebug("eglGetConfigs number: %d", nb_cfg);
    QVector<EGLConfig> cfgs(nb_cfg); //check > 0
    EGL_ENSURE(eglGetConfigs(egl->dpy, cfgs.data(), cfgs.size(), &nb_cfg) == EGL_TRUE, false);
    EGLConfig egl_cfg = NULL;
    for (int i = 0; i < nb_cfg; ++i) {
        EGLint id = 0;
        eglGetConfigAttrib(egl->dpy, cfgs[i], EGL_CONFIG_ID, &id);
        if (id == cfg_id) {
            egl_cfg = cfgs[i];
            break;
        }
    }
#endif
    qDebug("egl display:%p config: %p", egl->dpy, egl_cfg);
    // check extensions
    QList<QByteArray> extensions = QByteArray(eglQueryString(egl->dpy, EGL_EXTENSIONS)).split(' ');
    // ANGLE_d3d_share_handle_client_buffer will be used if possible
    const bool kEGL_ANGLE_d3d_share_handle_client_buffer = extensions.contains("EGL_ANGLE_d3d_share_handle_client_buffer");
    const bool kEGL_ANGLE_query_surface_pointer = extensions.contains("EGL_ANGLE_query_surface_pointer");
    if (!kEGL_ANGLE_d3d_share_handle_client_buffer && !kEGL_ANGLE_query_surface_pointer) {
        qWarning("EGL extension 'kEGL_ANGLE_query_surface_pointer' or 'ANGLE_d3d_share_handle_client_buffer' is required!");
        return false;
    }
    GLint has_alpha = 1; //QOpenGLContext::currentContext()->format().hasAlpha()
    eglGetConfigAttrib(egl->dpy, egl_cfg, EGL_BIND_TO_TEXTURE_RGBA, &has_alpha); //EGL_ALPHA_SIZE
    EGLint attribs[] = {
        EGL_WIDTH, w,
        EGL_HEIGHT, h,
        EGL_TEXTURE_FORMAT, has_alpha ? EGL_TEXTURE_RGBA : EGL_TEXTURE_RGB,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_NONE
    };

    HANDLE share_handle = NULL;
    if (!kEGL_ANGLE_d3d_share_handle_client_buffer && kEGL_ANGLE_query_surface_pointer) {
        EGL_ENSURE((egl->surface = eglCreatePbufferSurface(egl->dpy, egl_cfg, attribs)) != EGL_NO_SURFACE, false);
        qDebug("pbuffer surface: %p", egl->surface);
        PFNEGLQUERYSURFACEPOINTERANGLEPROC eglQuerySurfacePointerANGLE = reinterpret_cast<PFNEGLQUERYSURFACEPOINTERANGLEPROC>(eglGetProcAddress("eglQuerySurfacePointerANGLE"));
        if (!eglQuerySurfacePointerANGLE) {
            qWarning("EGL_ANGLE_query_surface_pointer is not supported");
            return false;
        }
        EGL_ENSURE(eglQuerySurfacePointerANGLE(egl->dpy, egl->surface, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, &share_handle), false);
    }

    SafeRelease(&surface9);
    SafeRelease(&texture9);
    // _A8 for a yuv plane
    /*
     * d3d resource share requires windows >= vista: https://msdn.microsoft.com/en-us/library/windows/desktop/bb219800(v=vs.85).aspx
     * from extension files:
     * d3d9: level must be 1, dimensions must match EGL surface's
     * d3d9ex or d3d10:
     */
    DX_ENSURE(device9->CreateTexture(w, h, 1,
                                        D3DUSAGE_RENDERTARGET,
                                        has_alpha ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8,
                                        D3DPOOL_DEFAULT,
                                        &texture9,
                                        &share_handle) , false);
    DX_ENSURE(texture9->GetSurfaceLevel(0, &surface9), false);

    if (kEGL_ANGLE_d3d_share_handle_client_buffer) {
        // requires extension EGL_ANGLE_d3d_share_handle_client_buffer
        // egl surface size must match d3d texture's
        // d3d9ex or d3d10 is required
        EGL_ENSURE((egl->surface = eglCreatePbufferFromClientBuffer(egl->dpy, EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, share_handle, egl_cfg, attribs)), false);
        qDebug("pbuffer surface from client buffer: %p", egl->surface);
    }
    return true;
}

bool EGLInteropResource::map(int picIndex, const CUVIDPROCPARAMS &param, GLuint tex, int w, int h, int ch, int plane)
{
    // plane is always 0 because frame is rgb
    AutoCtxLock locker((cuda_api*)this, lock);
    Q_UNUSED(locker);
    if (!ensureResource(w, h, ch, tex)) // TODO surface size instead of frame size because we copy the device data
        return false;
    //CUDA_ENSURE(cuCtxPushCurrent(ctx), false);
    CUdeviceptr devptr;
    unsigned int pitch;

    CUDA_ENSURE(cuvidMapVideoFrame(dec, picIndex, &devptr, &pitch, const_cast<CUVIDPROCPARAMS*>(&param)), false);
    CUVIDAutoUnmapper unmapper(this, dec, devptr);
    Q_UNUSED(unmapper);
    // TODO: why can not use res[plane].stream? CUDA_ERROR_INVALID_HANDLE
    CUDA_ENSURE(cuGraphicsMapResources(1, &res[plane].cuRes, 0), false);
    CUarray array;
    CUDA_ENSURE(cuGraphicsSubResourceGetMappedArray(&array, res[plane].cuRes, 0, 0), false);

    CUDA_MEMCPY2D cu2d;
    memset(&cu2d, 0, sizeof(cu2d));
    // Y plane
    cu2d.srcDevice = devptr;
    cu2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cu2d.srcPitch = pitch;
    cu2d.dstArray = array;
    cu2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    cu2d.dstPitch = pitch;
    // the whole size or copy size?
    cu2d.WidthInBytes = w; // the same value as texture9_nv12
    cu2d.Height = h;
#if USE_STREAM
    CUDA_ENSURE(cuMemcpy2DAsync(&cu2d, res[plane].stream), false);
#else
    CUDA_ENSURE(cuMemcpy2D(&cu2d), false);
#endif
    // UV plane
    cu2d.srcXInBytes = 0;// +srcY*srcPitch + srcXInBytes
    cu2d.srcY = ch; // skip the padding height
    cu2d.Height /= 2;
    cu2d.dstXInBytes = 0;//+dstY*dstPitch + dstXInBytes
    cu2d.dstY = h;
#if USE_STREAM
    CUDA_WARN(cuMemcpy2DAsync(&cu2d, res[plane].stream));
#else
    CUDA_ENSURE(cuMemcpy2D(&cu2d), false);
#endif
    //TODO: delay cuCtxSynchronize && unmap. do it in unmap(tex)?
    // map to an already mapped resource will crash. sometimes I can not unmap the resource in unmap(tex) because if context switch error
    // so I simply unmap the resource here
    if (WORKAROUND_UNMAP_CONTEXT_SWITCH) {
#if USE_STREAM
        //CUDA_WARN(cuCtxSynchronize(), false); //wait too long time? use cuStreamQuery?
        CUDA_WARN(cuStreamSynchronize(res[plane].stream)); //slower than CtxSynchronize
#endif
        /*
         * This function provides the synchronization guarantee that any CUDA work issued
         * in \p stream before ::cuGraphicsUnmapResources() will complete before any
         * subsequently issued graphics work begins.
         * The graphics API from which \p resources were registered
         * should not access any resources while they are mapped by CUDA. If an
         * application does so, the results are undefined.
         */
        CUDA_ENSURE(cuGraphicsUnmapResources(1, &res[plane].cuRes, 0), false);
    } else {
        // call it at last. current context will be used by other cuda calls (unmap() for example)
        CUDA_ENSURE(cuCtxPopCurrent(&ctx), false);
    }
    if (!map(surface9_nv12, tex, w, h, ch))
        return false;
    return true;
}

bool EGLInteropResource::map(IDirect3DSurface9* surface, GLuint tex, int w, int h, int ch)
{
    D3DSURFACE_DESC dxvaDesc;
    surface->GetDesc(&dxvaDesc);
    DYGL(glBindTexture(GL_TEXTURE_2D, tex));
    const RECT src = { 0, 0, w, h}; // L8: h*3/2?
    HRESULT ret = device9->StretchRect(surface, &src, surface9, NULL, D3DTEXF_NONE);
    if (SUCCEEDED(ret)) {
        if (query9) {
            // Flush the draw command now. Ideally, this should be done immediately before the draw call that uses the texture. Flush it once here though.
            query9->Issue(D3DISSUE_END);
            // ensure data is copied to egl surface. Solution and comment is from chromium
            // The DXVA decoder has its own device which it uses for decoding. ANGLE has its own device which we don't have access to.
            // The above code attempts to copy the decoded picture into a surface which is owned by ANGLE.
            // As there are multiple devices involved in this, the StretchRect call above is not synchronous.
            // We attempt to flush the batched operations to ensure that the picture is copied to the surface owned by ANGLE.
            // We need to do this in a loop and call flush multiple times.
            // We have seen the GetData call for flushing the command buffer fail to return success occassionally on multi core machines, leading to an infinite loop.
            // Workaround is to have an upper limit of 10 on the number of iterations to wait for the Flush to finish.
            int k = 0;
            // skip at decoder.close()
            while (/*!skip_dx.load() && */(query9->GetData(NULL, 0, D3DGETDATA_FLUSH) == FALSE) && ++k < 10) {
                Sleep(1);
            }
        }
        eglBindTexImage(egl->dpy, egl->surface, EGL_BACK_BUFFER);
    } else {
        qWarning() << "map to egl error: " << ret << " - " << qt_error_string(ret);
    }
    DYGL(glBindTexture(GL_TEXTURE_2D, 0));
    return true;
}

} //namespace cuda
} //namespace QtAV
#endif //QTAV_HAVE(CUDA_EGL)
#if QTAV_HAVE(CUDA_GL)
namespace QtAV {
namespace cuda {
GLInteropResource::GLInteropResource(CUdevice d, CUvideodecoder decoder, CUvideoctxlock lk)
    : InteropResource(d, decoder, lk)
{}

bool GLInteropResource::map(int picIndex, const CUVIDPROCPARAMS &param, GLuint tex, int w, int h, int ch, int plane)
{
    AutoCtxLock locker((cuda_api*)this, lock);
    Q_UNUSED(locker);
    if (!ensureResource(w, h, ch, tex, plane)) // TODO surface size instead of frame size because we copy the device data
        return false;
    //CUDA_ENSURE(cuCtxPushCurrent(ctx), false);
    CUdeviceptr devptr;
    unsigned int pitch;

    CUDA_ENSURE(cuvidMapVideoFrame(dec, picIndex, &devptr, &pitch, const_cast<CUVIDPROCPARAMS*>(&param)), false);
    CUVIDAutoUnmapper unmapper(this, dec, devptr);
    Q_UNUSED(unmapper);
    // TODO: why can not use res[plane].stream? CUDA_ERROR_INVALID_HANDLE
    CUDA_ENSURE(cuGraphicsMapResources(1, &res[plane].cuRes, 0), false);
    CUarray array;
    CUDA_ENSURE(cuGraphicsSubResourceGetMappedArray(&array, res[plane].cuRes, 0, 0), false);

    CUDA_MEMCPY2D cu2d;
    memset(&cu2d, 0, sizeof(cu2d));
    cu2d.srcDevice = devptr;
    cu2d.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cu2d.srcPitch = pitch;
    cu2d.dstArray = array;
    cu2d.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    cu2d.dstPitch = pitch;
    // the whole size or copy size?
    cu2d.WidthInBytes = pitch;
    cu2d.Height = h;
    if (plane == 1) {
        cu2d.srcXInBytes = 0;// TODO: why not pitch*ch?
        cu2d.srcY = ch; // skip the padding height
        cu2d.Height /= 2;
    }
#if USE_STREAM
    CUDA_ENSURE(cuMemcpy2DAsync(&cu2d, res[plane].stream), false);
#else
    CUDA_ENSURE(cuMemcpy2D(&cu2d), false);
#endif
    //TODO: delay cuCtxSynchronize && unmap. do it in unmap(tex)?
    // map to an already mapped resource will crash. sometimes I can not unmap the resource in unmap(tex) because if context switch error
    // so I simply unmap the resource here
    if (WORKAROUND_UNMAP_CONTEXT_SWITCH) {
#if USE_STREAM
        //CUDA_WARN(cuCtxSynchronize(), false); //wait too long time? use cuStreamQuery?
        CUDA_WARN(cuStreamSynchronize(res[plane].stream)); //slower than CtxSynchronize
#endif
        /*
         * This function provides the synchronization guarantee that any CUDA work issued
         * in \p stream before ::cuGraphicsUnmapResources() will complete before any
         * subsequently issued graphics work begins.
         * The graphics API from which \p resources were registered
         * should not access any resources while they are mapped by CUDA. If an
         * application does so, the results are undefined.
         */
        CUDA_ENSURE(cuGraphicsUnmapResources(1, &res[plane].cuRes, 0), false);
    } else {
        // call it at last. current context will be used by other cuda calls (unmap() for example)
        CUDA_ENSURE(cuCtxPopCurrent(&ctx), false);
    }
    return true;
}

bool GLInteropResource::unmap(GLuint tex)
{
    Q_UNUSED(tex);
#if !WORKAROUND_UNMAP_CONTEXT_SWITCH
    int plane = -1;
    if (res[0].texture == tex)
        plane = 0;
    else if (res[1].texture == tex)
        plane = 1;
    else
        return false;
    // FIXME: why cuCtxPushCurrent gives CUDA_ERROR_INVALID_CONTEXT if opengl viewport changed?
    CUDA_WARN(cuCtxPushCurrent(ctx));
    CUDA_WARN(cuStreamSynchronize(res[plane].stream));
    // FIXME: need a correct context. But why we have to push context even though map/unmap are called in the same thread
    // Because the decoder switch the context in another thread so we have to switch the context back?
    // to workaround the context issue, we must pop the context that valid in map() and push it here
    CUDA_ENSURE(cuGraphicsUnmapResources(1, &res[plane].cuRes, 0), false);
    CUDA_ENSURE(cuCtxPopCurrent(&ctx), false);
#endif //WORKAROUND_UNMAP_CONTEXT_SWITCH
    return true;
}

bool GLInteropResource::ensureResource(int w, int h, int ch, GLuint tex, int plane)
{
    Q_ASSERT(plane < 2 && "plane number must be 0 or 1 for NV12");
    TexRes &r = res[plane];
    if (r.texture == tex && r.w == w && r.h == h && r.H == ch && r.cuRes)
        return true;
    if (!ctx) {
        // TODO: how to use pop/push decoder's context without the context in opengl context
        CUDA_ENSURE(cuCtxCreate(&ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev), false);
#if USE_STREAM
        CUDA_WARN(cuStreamCreate(&res[0].stream, CU_STREAM_DEFAULT));
        CUDA_WARN(cuStreamCreate(&res[1].stream, CU_STREAM_DEFAULT));
#endif //USE_STREAM
        qDebug("cuda contex on gl thread: %p", ctx);
        CUDA_ENSURE(cuCtxPopCurrent(&ctx), false); // TODO: why cuMemcpy2D need this
    }
    if (r.cuRes) {
        CUDA_ENSURE(cuGraphicsUnregisterResource(r.cuRes), false);
        r.cuRes = NULL;
    }
    CUDA_ENSURE(cuGraphicsGLRegisterImage(&r.cuRes, tex, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_NONE), false);
    r.texture = tex;
    r.w = w;
    r.h = h;
    r.H = ch;
    return true;
}
} //namespace cuda
} //namespace QtAV
#endif //QTAV_HAVE(CUDA_GL)