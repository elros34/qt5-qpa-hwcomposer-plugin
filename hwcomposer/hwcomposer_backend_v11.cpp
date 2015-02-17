/****************************************************************************
**
** Copyright (C) 2013, 2015 Jolla Ltd.
** Contact: Thomas Perl <thomas.perl@jolla.com>
** Contact: Gunnar Sletta <gunnar.sletta@jollamobile.com>
**
** This file is part of the hwcomposer plugin.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <android-version.h>

#include "hwcomposer_backend_v11.h"
#include <stdio.h>

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QEvent>
#include <QCoreApplication>
#include <QSize>
#include <QRect>

#ifdef HWC_PLUGIN_HAVE_HWCOMPOSER1_API

class HWC11Thread;

class HWC11WindowSurface : public HWComposerNativeWindow
{
protected:
    void present(HWComposerNativeWindowBuffer *buffer) { backend->present(buffer); }
    // int dequeueBuffer(BaseNativeWindowBuffer** buffer, int* fenceFd) {
    //     int result = HWComposerNativeWindow::dequeueBuffer(buffer, fenceFd);
    //     qDebug() << " --- dequeueBuffer" << *buffer << *fenceFd << (*buffer)->handle << QThread::currentThread();
    //     return result;
    // }



public:
    unsigned int width() const { return HWComposerNativeWindow::width(); }
    unsigned int height() const { return HWComposerNativeWindow::height(); }
    HWC11WindowSurface(HwComposerBackend_v11 *backend, unsigned int width, unsigned int height, unsigned int format);

private:
    HwComposerBackend_v11 *backend;
};


class HWC11Thread : public QThread
{
public:
    enum Action {
        InitializeAction = QEvent::User,
        CleanupAction,
        DisplaySleepAction,
        DisplayWakeAction,
        CheckLayerListAction,
        EglSurfaceCompositionAction,
        LayerListCompositionAction,
    };

    HWC11Thread(HwComposerBackend_v11 *backend, hwc_composer_device_1_t *d);

    void composeEglSurface();
    void composeAcceptedLayerList();
    void doComposition(hwc_display_contents_1_t *dc);
    void initialize();
    void cleanup();
    void checkLayerList();
    void syncAndCloseOldFences();

    void post(Action a) { QCoreApplication::postEvent(this, new QEvent((QEvent::Type) a)); }
    bool event(QEvent *e);

    inline void lock() { mutex.lock(); }
    inline void unlock() { mutex.unlock(); }
    inline void wait() { condition.wait(&mutex); }
    inline void wake() { condition.wakeOne(); }

    void stopGracefully() {
        post(CleanupAction);
        lock();
        size = QSize();
        unlock();
        quit();
    }

    HwComposerBackend_v11 *backend;
    hwc_composer_device_1_t *hwcDevice;
    hwc_display_contents_1_t *hwcEglSurfaceList;
    hwc_display_contents_1_t *hwcLayerList;
    buffer_handle_t lastEglSurfaceBuffer;

    struct BufferAndFd {
        buffer_handle_t buffer;
        int fd;
    };
    QVarLengthArray<BufferAndFd, 8> m_releaseFences;

    QSize size;

    QMutex layerListMutex;
    HwcInterface::LayerList *acceptedLayerList;

    // The following values is the state of the upcoming composition.
    HWComposerNativeWindowBuffer *eglSurfaceBuffer;
    bool useLayerList;

    // Mutex/wait condition to be used when updating the upcoming composition state
    QMutex mutex;
    QWaitCondition condition;
};

void hwc11_copy_layer_list(QVarLengthArray<void *, 8> *dst, HwcInterface::LayerList *src)
{
    dst->resize(src->layerCount);
    for (int i=0; i<src->layerCount; ++i)
        (*dst)[i] = src->layers[i].handle;
}


HWC11WindowSurface::HWC11WindowSurface(HwComposerBackend_v11 *b, unsigned int width, unsigned int height, unsigned int format)
    : HWComposerNativeWindow(width, height, format)
    , backend(b)
{
}


HwComposerBackend_v11::HwComposerBackend_v11(hw_module_t *hwc_module, hw_device_t *hw_device, int num_displays)
    : HwComposerBackend(hwc_module)
    , m_scheduledLayerList(0)
    , m_releaseLayerListCallback(0)
    , m_bufferAvailableCallback(0)
    , m_bufferAvailableCallbackData(0)
    , m_eglSurfaceBuffer(0)
    , m_eglWithLayerList(false)
{
    Q_UNUSED(num_displays);
    m_thread = new HWC11Thread(this, (hwc_composer_device_1_t *) hw_device);
    m_thread->moveToThread(m_thread);
}

HwComposerBackend_v11::~HwComposerBackend_v11()
{
    // Stop the compositor thread

    if (m_thread->isRunning())
        m_thread->stopGracefully();
    m_thread->QThread::wait();
    delete m_thread;
}

EGLNativeDisplayType
HwComposerBackend_v11::display()
{
    return EGL_DEFAULT_DISPLAY;
}

EGLNativeWindowType
HwComposerBackend_v11::createWindow(int width, int height)
{
    qCDebug(QPA_LOG_HWC, "createWindow: %d x %d", width, height);
    // We only support a single window
    HWC11WindowSurface *window = new HWC11WindowSurface(this, width, height, HAL_PIXEL_FORMAT_RGBA_8888);
    Q_ASSERT(!m_thread->isRunning());
    m_thread->size = QSize(width, height);
    m_thread->post(HWC11Thread::InitializeAction);
    m_thread->start();
    return (EGLNativeWindowType) static_cast<ANativeWindow *>(window);
}

void
HwComposerBackend_v11::destroyWindow(EGLNativeWindowType window)
{
    Q_UNUSED(window);
    qCDebug(QPA_LOG_HWC, "destroyWindow");
    Q_ASSERT(m_thread->isRunning());
    // Stop rendering...
    m_thread->stopGracefully();

    // No need to delete the window, refcounting in libhybris will handle that
    // as a result of this call stemming from where the platfrom plugin calls
    // eglDestroyWindowSurface.
}

/* Sets the buffer as the current front buffer to be displayed through the
   HWC. The HWC will pick up the buffer and set it to 0.

   If there already is a buffer pending for display, this function will block
   until the current buffer has been picked up. As HwcWindowSurfaceNativeWindow
   has two buffers by default, this allows us to queue up one buffer before
   rendering is blocked on the EGL render thread.
 */
void HwComposerBackend_v11::present(HWComposerNativeWindowBuffer *b)
{
    qCDebug(QPA_LOG_HWC, "present: %p (%p), current=%p, layerList=%d, thread=%p", b, b->handle, m_eglSurfaceBuffer, m_layerListBuffers.size(), QThread::currentThread());
    m_thread->lock();
    if (waitForComposer()) {
        qCDebug(QPA_LOG_HWC, " - need to wait for composer... %p", QThread::currentThread());
        m_thread->wait();
    }
    Q_ASSERT(m_eglSurfaceBuffer == 0);
    Q_ASSERT(m_layerListBuffers.size() == 0);
    m_eglSurfaceBuffer = b;
    if (m_eglWithLayerList) {
        // present is called directly from eglSwapBuffers, so the acceptedLayerList will be
        // the same as the input to swapLayerList, so we pick the buffer values from here.
        hwc11_copy_layer_list(&m_layerListBuffers, m_thread->acceptedLayerList);
        m_thread->post(HWC11Thread::LayerListCompositionAction);
    } else {
        m_thread->post(HWC11Thread::EglSurfaceCompositionAction);
    }
    m_thread->unlock();
}

void
HwComposerBackend_v11::swap(EGLNativeDisplayType display, EGLSurface surface)
{
    qCDebug(QPA_LOG_HWC, "eglSwapBuffers");
    m_eglWithLayerList = false;
    eglSwapBuffers(display, surface);
}

void
HwComposerBackend_v11::sleepDisplay(bool sleep)
{
    qCDebug(QPA_LOG_HWC, "sleep: %d", sleep);
    m_thread->post(sleep ? HWC11Thread::DisplaySleepAction : HWC11Thread::DisplayWakeAction);
}

float
HwComposerBackend_v11::refreshRate()
{
    // TODO: Implement new hwc 1.1 querying of vsync period per-display
    //
    // from HwcWindowSurface_defs.h:
    // "This query is not used for HWC_DEVICE_API_VERSION_1_1 and later.
    //  Instead, the per-display attribute HWC_DISPLAY_VSYNC_PERIOD is used."
    return 60.0;
}

void HwComposerBackend_v11::scheduleLayerList(HwcInterface::LayerList *list)
{
    qCDebug(QPA_LOG_HWC, "scheduleLayerList");

    if (!m_releaseLayerListCallback)
        qFatal("ReleaseLayerListCallback has not been installed");
    if (!m_bufferAvailableCallback)
        qFatal("BufferAvailableCallback has not been installed");

    m_thread->layerListMutex.lock();
    for (int i=0; i<list->layerCount; ++i) {
        if (!list->layers[i].handle)
            qFatal("missing buffer handle for layer %d", i);
    }

    if (m_scheduledLayerList)
        m_releaseLayerListCallback(m_scheduledLayerList);
    m_scheduledLayerList = list;
    m_thread->post(HWC11Thread::CheckLayerListAction);
    m_thread->layerListMutex.unlock();
}

const HwcInterface::LayerList *HwComposerBackend_v11::acceptedLayerList() const
{
    m_thread->layerListMutex.lock();
    HwcInterface::LayerList *list = m_thread->acceptedLayerList;
    m_thread->layerListMutex.unlock();
    return list;
}

void HwComposerBackend_v11::swapLayerList(HwcInterface::LayerList *list)
{
    qCDebug(QPA_LOG_HWC, "swapLayerList, thread=%p", QThread::currentThread());

    if (list != acceptedLayerList())
        qFatal("submitted list is not accepted list");
    if (m_scheduledLayerList)
        qFatal("submitted layerlist while there is a pending 'scheduledLayerList'");

    if (list->eglRenderingEnabled) {
        m_eglWithLayerList = true; // will be picked up in present() which is called from eglSwapBuffers()
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
        qCDebug(QPA_LOG_HWC, " - with eglSwapBuffers, display=%p, surface=%p", display, surface);
        eglSwapBuffers(display, surface);

    } else {
        qCDebug(QPA_LOG_HWC, " - swapping layers directly: m_eglSurfaceBuffer=%p, m_layerListBuffers.size()=%d", m_eglSurfaceBuffer, m_layerListBuffers.size());
        m_thread->lock();
        if (waitForComposer()) {
            qCDebug(QPA_LOG_HWC, " - wait for composer");;
            m_thread->wait();
        }

        Q_ASSERT(m_eglSurfaceBuffer == 0);
        Q_ASSERT(m_layerListBuffers.size() == 0);
        hwc11_copy_layer_list(&m_layerListBuffers, list);
        m_thread->post(HWC11Thread::LayerListCompositionAction);
        m_thread->unlock();
    }
}

static void hwc11_dump_display_contents(hwc_display_contents_1_t *dc)
{
    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - displayContents, retireFence=%d, outbuf=%p, outAcqFence=%d, flags=%x, numLayers=%d",
            dc->retireFenceFd,
            dc->outbuf,
            dc->outbufAcquireFenceFd,
            (int) dc->flags,
            (int) dc->numHwLayers);
    for (unsigned int i=0; i<dc->numHwLayers; ++i) {
        const hwc_layer_1_t &l = dc->hwLayers[i];
        qCDebug(QPA_LOG_HWC, "                                (HWCT)    - layer comp=%x, hints=%x, flags=%x, handle=%p, transform=%x, blending=%x, "
                "src=(%d %d - %dx%d), dst=(%d %d - %dx%d), afd=%d, rfd=%d, a=%d, "
                "region=(%d %d - %dx%d)",
                l.compositionType, l.hints, l.flags, l.handle, l.transform, l.blending,
                (int) l.sourceCropf.left, (int) l.sourceCropf.top, (int) l.sourceCropf.right, (int) l.sourceCropf.bottom,
                l.displayFrame.left, l.displayFrame.top, l.displayFrame.right, l.displayFrame.bottom,
                l.acquireFenceFd, l.releaseFenceFd, l.planeAlpha,
                l.visibleRegionScreen.rects[0].left,
                l.visibleRegionScreen.rects[0].top,
                l.visibleRegionScreen.rects[0].right,
                l.visibleRegionScreen.rects[0].bottom);
    }
}

static void hwc11_callback_vsync(const struct hwc_procs *, int, int64_t)
{
    qCDebug(QPA_LOG_HWC, " ********** callback_vsync **********");
}

static void hwc11_callback_invalidate(const struct hwc_procs *)
{
    qCDebug(QPA_LOG_HWC, "callback_invalidate");
}

static void hwc11_callback_hotplug(const struct hwc_procs *, int, int)
{
    qCDebug(QPA_LOG_HWC, "callback_hotplug");
}

HWC11Thread::HWC11Thread(HwComposerBackend_v11 *b, hwc_composer_device_1_t *d)
    : backend(b)
    , hwcDevice(d)
    , hwcEglSurfaceList(0)
    , hwcLayerList(0)
    , lastEglSurfaceBuffer(0)
    , acceptedLayerList(0)
    , eglSurfaceBuffer(0)
    , useLayerList(false)
{
    setObjectName("QPA/HWC Thread");
}

static void hwc11_populate_layer(hwc_layer_1_t *layer, const QRect &tr, const QRect &sr, buffer_handle_t handle, int32_t type)
{
    layer->handle = handle;
    layer->hints = HWC_GEOMETRY_CHANGED;
    layer->flags = 0;
    layer->compositionType = type;
    layer->blending = HWC_BLENDING_PREMULT;
    layer->transform = 0;
    layer->acquireFenceFd = -1;
    layer->releaseFenceFd = -1;
#ifdef HWC_DEVICE_API_VERSION_1_2
    layer->planeAlpha = 0xff;
#endif
#ifdef HWC_DEVICE_API_VERSION_1_3
    layer->sourceCropf.left = sr.x();
    layer->sourceCropf.top = sr.y();
    layer->sourceCropf.right = sr.width() + sr.x();
    layer->sourceCropf.bottom = sr.height() + sr.y();
#else
    layer->sourceCrop.left = sr.x();
    layer->sourceCrop.top = sr.y();
    layer->sourceCrop.right = sr.width() + sr.x();
    layer->sourceCrop.bottom = sr.height() + sr.y();
#endif
    layer->displayFrame.left = tr.x();
    layer->displayFrame.top = tr.y();
    layer->displayFrame.right = tr.width() + tr.x();
    layer->displayFrame.bottom = tr.height() + tr.y();
    layer->visibleRegionScreen.numRects = 1;
    layer->visibleRegionScreen.rects = &layer->displayFrame;
}

static void hwc11_update_layer(hwc_layer_1_t *layer, int acqFd, buffer_handle_t handle)
{
    layer->handle = handle;
    layer->acquireFenceFd = acqFd;
    layer->releaseFenceFd = -1;
    layer->hints = HWC_GEOMETRY_CHANGED;
}

void HWC11Thread::initialize()
{
    qCDebug(QPA_LOG_HWC, "                                (HWCT) initialize");
    Q_ASSERT(size.width() > 1 && size.height() > 1);

    hwc_procs *procs = new hwc_procs();
    procs->invalidate = hwc11_callback_invalidate;
    procs->hotplug = hwc11_callback_hotplug;
    procs->vsync = hwc11_callback_vsync;
    hwcDevice->registerProcs(hwcDevice, procs);
    hwcDevice->eventControl(hwcDevice, 0, HWC_EVENT_VSYNC, 1);

    int hwcEglSurfaceListSize = sizeof(hwc_display_contents_1_t) + sizeof(hwc_layer_1_t);
    hwcEglSurfaceList = (hwc_display_contents_1_t *) malloc(hwcEglSurfaceListSize);
    memset(hwcEglSurfaceList, 0, hwcEglSurfaceListSize);
    hwcEglSurfaceList->retireFenceFd = -1;
    hwcEglSurfaceList->outbuf = 0;
    hwcEglSurfaceList->outbufAcquireFenceFd = -1;
    hwcEglSurfaceList->flags = HWC_GEOMETRY_CHANGED;
    hwcEglSurfaceList->numHwLayers = 1;
    QRect fs(0, 0, size.width(), size.height());
    hwc11_populate_layer(&hwcEglSurfaceList->hwLayers[0], fs, fs, 0, HWC_FRAMEBUFFER_TARGET);
}

void HWC11Thread::cleanup()
{
    free(hwcEglSurfaceList);
    hwcEglSurfaceList = 0;
    free(hwcLayerList);
    hwcLayerList = 0;
    acceptedLayerList = 0;
     HWC_PLUGIN_EXPECT_ZERO(hwc_close_1(hwcDevice));
    hwcDevice = 0;
}

struct _BufferFenceAccessor : public HWComposerNativeWindowBuffer {
    int get() { return fenceFd; }
    void set(int fd) { fenceFd = fd; };
};
static inline int hwc11_getBufferFenceFd(const HWComposerNativeWindowBuffer *b) { return ((_BufferFenceAccessor *) b)->get(); }
static inline void hwc11_setBufferFenceFd(const HWComposerNativeWindowBuffer *b, int fd) { ((_BufferFenceAccessor *) b)->set(fd); }

void HWC11Thread::composeEglSurface()
{
    qCDebug(QPA_LOG_HWC, "                                (HWCT) composeEglSurface");
    lock();

    if (size.isNull()) {
        // unlikely bug might happen after destroyWindow
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - no window surface, aborting");
        unlock();
        return;
    }

    // Grab the current egl surface buffer
    eglSurfaceBuffer = backend->m_eglSurfaceBuffer;
    hwc11_update_layer(hwcEglSurfaceList->hwLayers, hwc11_getBufferFenceFd(eglSurfaceBuffer), eglSurfaceBuffer->handle);
    backend->m_eglSurfaceBuffer = 0;

    // HWC requires retireFenceFd to be unspecified on 'set'
    hwcEglSurfaceList->retireFenceFd = -1;

    doComposition(hwcEglSurfaceList);

    hwc11_setBufferFenceFd(eglSurfaceBuffer, hwcEglSurfaceList->hwLayers[0].releaseFenceFd);
    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - egl buffer=%p has release fd=%d (%d)",
            eglSurfaceBuffer->handle, hwcEglSurfaceList->hwLayers[0].releaseFenceFd,
            hwc11_getBufferFenceFd(eglSurfaceBuffer));

    // We need "some" fullscreen buffer to use in checkLayerList's prepare. It
    // doesn't really matter where it comes from, so just use the last frame
    // we swapped.
    lastEglSurfaceBuffer = eglSurfaceBuffer->handle;

    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - composition done, waking up render thread");
    wake();
    unlock();
}

void HWC11Thread::composeAcceptedLayerList()
{
    qCDebug(QPA_LOG_HWC, "                                (HWCT) composeAcceptedLayerList");

    lock();

    if (size.isNull()) {
        // unlikely bug might happen after destroyWindow
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - no window surface, aborting");
        unlock();
        return;
    }

    Q_ASSERT(acceptedLayerList);

    // Required by 'set'
    hwcLayerList->retireFenceFd = -1;
    hwcLayerList->flags = HWC_GEOMETRY_CHANGED;

    int actualLayers = 0;
    Q_ASSERT(acceptedLayerList->layerCount);
    while (actualLayers < acceptedLayerList->layerCount && acceptedLayerList->layers[actualLayers].accepted)
        actualLayers++;

    if (acceptedLayerList->eglRenderingEnabled) {
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - egl surface as layer %d", actualLayers);
        eglSurfaceBuffer = backend->m_eglSurfaceBuffer;
        hwc11_update_layer(&hwcLayerList->hwLayers[actualLayers], hwc11_getBufferFenceFd(eglSurfaceBuffer), eglSurfaceBuffer->handle);
        hwcLayerList->hwLayers[actualLayers].compositionType = HWC_FRAMEBUFFER;
        backend->m_eglSurfaceBuffer = 0;
    }

    // copy the pending layers into our own list
    for (int i=0; i<actualLayers; ++i) {
        // If we're posting the same buffer again, we need to close its
        // release fd and mark it as -1 so we don't send release event back
        // to app after composition...
        buffer_handle_t buffer = (buffer_handle_t) backend->m_layerListBuffers.at(i);
        if (i < m_releaseFences.size() && m_releaseFences.at(i).buffer == buffer) {
            int fd = m_releaseFences.at(i).fd;
            if (fd != -1) {
                qCDebug(QPA_LOG_HWC, "                                (HWCT)  - posting buffer=%p again, closing fd=%d", buffer, fd);
                close(fd);
                m_releaseFences[i].fd = -1;
            }
        }
        hwc11_update_layer(&hwcLayerList->hwLayers[i], -1, buffer);
        hwcLayerList->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
    }
    backend->m_layerListBuffers.clear();

    doComposition(hwcLayerList);

    // deal with release fences once composition is over..
    if (acceptedLayerList->eglRenderingEnabled) {
        hwc11_setBufferFenceFd(eglSurfaceBuffer, hwcLayerList->hwLayers[actualLayers].releaseFenceFd);
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - egl buffer=%p has release fd=%d (%d)",
                eglSurfaceBuffer->handle, hwcLayerList->hwLayers[actualLayers].releaseFenceFd,
                hwc11_getBufferFenceFd(eglSurfaceBuffer));
        lastEglSurfaceBuffer = eglSurfaceBuffer->handle;
    }

    m_releaseFences.resize(actualLayers);
    for (int i=0; i<actualLayers; ++i) {
        const hwc_layer_1_t &l = hwcLayerList->hwLayers[i];
        BufferAndFd entry = { l.handle, l.releaseFenceFd };
        m_releaseFences[i] = entry;
        if (l.releaseFenceFd == -1) {
            qCDebug(QPA_LOG_HWC, "                                (HWCT)  - buffer %p does not have release fence, available right away", l.handle);
            backend->m_bufferAvailableCallback((void *) entry.buffer, backend->m_bufferAvailableCallbackData);
        } else {
            qCDebug(QPA_LOG_HWC, "                                (HWCT)  - buffer %p (fd=%d) stored for later...", l.handle, l.releaseFenceFd);
        }
    }

    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - composition done, waking up render thread");
    wake();
    unlock();
}

void HWC11Thread::doComposition(hwc_display_contents_1_t *dc)
{
    if (QPA_LOG_HWC().isDebugEnabled())
        hwc11_dump_display_contents(dc);

    HWC_PLUGIN_EXPECT_ZERO(hwcDevice->prepare(hwcDevice, 1, &dc));

    if (QPA_LOG_HWC().isDebugEnabled()) {
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - after preprare:");
        for (unsigned int i = 0; i<dc->numHwLayers; ++i) {
            qCDebug(QPA_LOG_HWC, "                                (HWCT)    - layer has composition type=%x", dc->hwLayers[i].compositionType);
        }
    }

    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - calling set");
    HWC_PLUGIN_EXPECT_ZERO(hwcDevice->set(hwcDevice, 1, &dc));
    qCDebug(QPA_LOG_HWC, "                                (HWCT)  - set completed..");

    if (QPA_LOG_HWC().isDebugEnabled())
        hwc11_dump_display_contents(dc);

    if (dc->retireFenceFd != -1)
        close(dc->retireFenceFd);
}

void HWC11Thread::checkLayerList()
{
    layerListMutex.lock();

    Q_ASSERT(backend->m_scheduledLayerList);
    if (acceptedLayerList)
        backend->m_releaseLayerListCallback(acceptedLayerList);
    HwcInterface::LayerList *layerList = backend->m_scheduledLayerList;
    backend->m_scheduledLayerList = 0;

    int actualLayerCount = 1 + layerList->layerCount + (layerList->eglRenderingEnabled ? 1 : 0);
    int dcSize = sizeof(hwc_display_contents_1_t) + actualLayerCount * sizeof(hwc_layer_1_t);
    hwc_display_contents_1_t *dc = (hwc_display_contents_1_t *) malloc(dcSize);
    memset(dc, 0, dcSize);

    dc->retireFenceFd = -1;
    dc->outbuf = 0;
    dc->outbufAcquireFenceFd = -1;
    dc->flags = HWC_GEOMETRY_CHANGED;
    dc->numHwLayers = actualLayerCount;
    QRect fs(0, 0, size.width(), size.height());

    bool accept = false;

    int layerCount = layerList->layerCount;

    qCDebug(QPA_LOG_HWC, "                                (HWCT) checkLayerList, %d layers, %d%s + HWC_FRAMEBUFFER_TARGET",
            actualLayerCount,
            layerList->layerCount,
            layerList->eglRenderingEnabled ? " + EGL Surface" : "");

    while (!accept && layerCount > 0) {

        for (int i=0; i<layerCount; ++i) {
            const HwcInterface::Layer &l = layerList->layers[i];
            QRect tr(l.tx, l.ty, l.tw, l.th);
            QRect sr(l.sx, l.sy, l.sw, l.sh);
            hwc11_populate_layer(&dc->hwLayers[i], tr, sr, (buffer_handle_t) l.handle, HWC_FRAMEBUFFER);
        }

        dc->numHwLayers = layerCount;

        if (layerList->eglRenderingEnabled) {
            // ### Can lastEglSurfaceBuffer be 0 here?
            hwc11_populate_layer(&dc->hwLayers[layerCount], fs, fs, lastEglSurfaceBuffer, HWC_FRAMEBUFFER);
            ++dc->numHwLayers;
        }

        // Add the dummy fallback HWC_FRAMEBUFFER_TARGET layer. This one has
        // buffer handle 0 as we intend to never render to it and that means
        // 'set' is supposed to ignore it.
        hwc11_populate_layer(&dc->hwLayers[dc->numHwLayers], fs, fs, lastEglSurfaceBuffer, HWC_FRAMEBUFFER_TARGET);
        ++dc->numHwLayers;

        if (QPA_LOG_HWC().isDebugEnabled())
            hwc11_dump_display_contents(dc);

        if (hwcDevice->prepare(hwcDevice, 1, &dc) == 0) {

            // Iterate over all the layers (excluding the dummy hwc_fb_target)
            // and check that we got HWC_OVERLAY, meaning that composition was
            // supported for that layer. If not, we need to flag it as not
            // possible, remove the last one and try again...
            accept = true;
            for (uint i=0; i<dc->numHwLayers-1; ++i) {
                if (dc->hwLayers[i].compositionType != HWC_OVERLAY) {
                    qCDebug(QPA_LOG_HWC, "                                (HWCT)    - layer %d failed", i);
                    accept = false;
                    break;
                }
            }

            if (!accept) {
                // Not ok, remove one layer and try again. However, this does
                // mean that we need to do egl rendering in addition to our
                // own rendering, so we enable that flag regardless of its own
                // state. This adds another layer, but we also reduce the
                // total count by one so we're still good with the memory we
                // allocated for 'dc'.
                --layerCount;
                layerList->eglRenderingEnabled = true;
                layerList->layers[layerCount].accepted = false;
            }

        } else {
            qCDebug(QPA_LOG_HWC, "                                (HWCT)    - prepare call failed, layerCount=%d, original=%d, egl=%d",
                    layerCount,
                    layerList->layerCount,
                    layerList->eglRenderingEnabled);
            break;
        }
    }



    if (accept) {

        // Flag the accepted ones as such
        for (int i=0; i<layerCount; ++i)
            layerList->layers[i].accepted = true;
        acceptedLayerList = layerList;
        hwcLayerList = dc;

        if (QPA_LOG_HWC().isDebugEnabled()) {
            qCDebug(QPA_LOG_HWC, "                                (HWCT)  - layer list was accepted, %d out of %d, egl=%d",
                    layerCount,
                    layerList->layerCount,
                    layerList->eglRenderingEnabled);
            for (int i=0; i<layerList->layerCount; ++i) {
                const HwcInterface::Layer &l = layerList->layers[i];
                qCDebug(QPA_LOG_HWC, "                                (HWCT)    - %d: %p, t=(%d,%d %dx%d), s=(%d,%d %dx%d) %s", i, l.handle,
                        l.tx, l.ty, l.tw, l.th,
                        l.sx, l.sy, l.sw, l.sh,
                        l.accepted ? "accepted" : "rejected");
            }
        }

    } else {
        qCDebug(QPA_LOG_HWC, "                                (HWCT)  - layer list was not accepted");
        free(dc);
        backend->m_releaseLayerListCallback(layerList);
        Q_ASSERT(acceptedLayerList == 0);
    }


    layerListMutex.unlock();
}

void HWC11Thread::syncAndCloseOldFences()
{
    for (int i=0; i<m_releaseFences.size(); ++i) {
        const BufferAndFd &entry = m_releaseFences.at(i);
        if (entry.fd != -1) {
            sync_wait(entry.fd, -1);
            close(entry.fd);
            qCDebug(QPA_LOG_HWC, "                                (HWCT)  - old buffer %p (fd=%d) is released from hwc", entry.buffer, entry.fd);
            backend->m_bufferAvailableCallback((void *) entry.buffer, backend->m_bufferAvailableCallbackData);
        }
    }
    m_releaseFences.clear();
}

bool HWC11Thread::event(QEvent *e)
{
    switch ((int) e->type()) {
    case InitializeAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: initialize");
        initialize();
        break;
    case CleanupAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: cleanup");
        cleanup();
        break;
    case EglSurfaceCompositionAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: egl surface composition");
        composeEglSurface();
        break;
    case DisplayWakeAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: display wake");
        HWC_PLUGIN_EXPECT_ZERO(hwcDevice->blank(hwcDevice, 0, 0));
        break;
    case DisplaySleepAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: display sleep");
        HWC_PLUGIN_EXPECT_ZERO(hwcDevice->blank(hwcDevice, 0, 1));
        break;
    case CheckLayerListAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: check layer list");
        checkLayerList();
        break;
    case LayerListCompositionAction:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) action: layer list composition");
        composeAcceptedLayerList();
        break;
    default:
        qCDebug(QPA_LOG_HWC, "                                (HWCT) unknown action: %d", e->type());
        break;
    }
    return QThread::event(e);
}

#endif
