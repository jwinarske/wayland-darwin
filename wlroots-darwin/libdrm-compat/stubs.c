/*
 * libdrm-compat: behavioral stubs.
 *
 * These are the libdrm symbols wlroots links but never reaches on Darwin
 * (they all require a DRM fd, and no DRM device can be opened on macOS). They
 * return clean failures rather than aborting, so the code paths that probe for
 * DRM devices fall through gracefully.
 *
 * The one load-bearing value is drmGetDevices2() -> 0 (zero devices): wlroots'
 * renderer autocreate uses it to decide whether a render node exists; returning
 * 0 makes it fall cleanly to the pixman renderer with no error logging. A
 * negative return would spam "drmGetDevices2 failed" on every autocreate.
 *
 * Signatures come from the vendored <xf86drm.h>/<xf86drmMode.h>, so the
 * compiler enforces that every definition matches its declaration.
 */
#include <errno.h>
#include <stdlib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* --- device enumeration: report no devices, cleanly --- */
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags; (void)devices; (void)max_devices;
	return 0; /* zero devices found */
}
int drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	(void)devices; (void)max_devices;
	return 0;
}
void drmFreeDevice(drmDevicePtr *device) { (void)device; }
void drmFreeDevices(drmDevicePtr devices[], int count) { (void)devices; (void)count; }

int drmGetDevice(int fd, drmDevicePtr *device)
{ (void)fd; (void)device; return -ENODEV; }
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{ (void)fd; (void)flags; (void)device; return -ENODEV; }
int drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{ (void)dev_id; (void)flags; (void)device; return -ENODEV; }

/* --- capability / master / auth queries: fd-predicated, cannot succeed --- */
int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{ (void)fd; (void)capability; (void)value; return -EINVAL; }
int drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{ (void)fd; (void)capability; (void)value; return -EINVAL; }
int drmIsKMS(int fd) { (void)fd; return 0; }
int drmIsMaster(int fd) { (void)fd; return 0; }
int drmSetMaster(int fd) { (void)fd; return -EINVAL; }
int drmDropMaster(int fd) { (void)fd; return -EINVAL; }
int drmAuthMagic(int fd, drm_magic_t magic) { (void)fd; (void)magic; return -EINVAL; }
int drmGetMagic(int fd, drm_magic_t *magic) { (void)fd; (void)magic; return -EINVAL; }

/* --- node / device name queries: fd-predicated --- */
int drmGetNodeTypeFromFd(int fd) { (void)fd; errno = EINVAL; return -1; }
char *drmGetDeviceNameFromFd2(int fd) { (void)fd; return NULL; }
char *drmGetPrimaryDeviceNameFromFd(int fd) { (void)fd; return NULL; }
char *drmGetRenderDeviceNameFromFd(int fd) { (void)fd; return NULL; }

/* --- version --- */
drmVersionPtr drmGetVersion(int fd) { (void)fd; return NULL; }
void drmFreeVersion(drmVersionPtr v) { (void)v; }

/* --- generic free (real libdrm just wraps free()) --- */
void drmFree(void *p) { free(p); }

/* --- ioctl / prime / event: not implementable without a DRM fd --- */
int drmIoctl(int fd, unsigned long request, void *arg)
{ (void)fd; (void)request; (void)arg; errno = ENOSYS; return -1; }
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{ (void)fd; (void)prime_fd; (void)handle; return -ENOSYS; }
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{ (void)fd; (void)handle; (void)flags; (void)prime_fd; return -ENOSYS; }
int drmCloseBufferHandle(int fd, uint32_t handle)
{ (void)fd; (void)handle; return -ENOSYS; }
int drmHandleEvent(int fd, drmEventContextPtr evctx)
{ (void)fd; (void)evctx; errno = ENOSYS; return -1; }

/* --- dumb buffers / lease: require a DRM master fd --- */
int drmModeCreateDumbBuffer(int fd, uint32_t width, uint32_t height, uint32_t bpp,
			    uint32_t flags, uint32_t *handle, uint32_t *pitch,
			    uint64_t *size)
{ (void)fd; (void)width; (void)height; (void)bpp; (void)flags;
  (void)handle; (void)pitch; (void)size; return -ENOSYS; }
int drmModeDestroyDumbBuffer(int fd, uint32_t handle)
{ (void)fd; (void)handle; return -ENOSYS; }
int drmModeMapDumbBuffer(int fd, uint32_t handle, uint64_t *offset)
{ (void)fd; (void)handle; (void)offset; return -ENOSYS; }
int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects,
		       int flags, uint32_t *lessee_id)
{ (void)fd; (void)objects; (void)num_objects; (void)flags; (void)lessee_id;
  return -ENOSYS; }

/* --- sync objects (explicit fencing): compositor opt-in, never created here --- */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{ (void)fd; (void)flags; (void)handle; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t handle)
{ (void)fd; (void)handle; return -ENOSYS; }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{ (void)fd; (void)handle; (void)obj_fd; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{ (void)fd; (void)obj_fd; (void)handle; return -ENOSYS; }
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; return -ENOSYS; }
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; return -ENOSYS; }
int drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd,
		      uint32_t flags)
{ (void)fd; (void)handle; (void)point; (void)ev_fd; (void)flags; return -ENOSYS; }
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
			     uint64_t *points, uint32_t handle_count)
{ (void)fd; (void)handles; (void)points; (void)handle_count; return -ENOSYS; }
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
			   unsigned num_handles, int64_t timeout_nsec,
			   unsigned flags, uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)points; (void)num_handles; (void)timeout_nsec;
  (void)flags; (void)first_signaled; return -ENOSYS; }
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
		       uint32_t src_handle, uint64_t src_point, uint32_t flags)
{ (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle;
  (void)src_point; (void)flags; return -ENOSYS; }
