#include "impl.h"

#include "impl_priv.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <pthread.h>

#include <linux/usb/functionfs.h>

#include "libusbd_priv.h"


#if __BYTE_ORDER == __LITTLE_ENDIAN
#define cpu_to_le16(x)  (x)
#define cpu_to_le32(x)  (x)
#else
#define cpu_to_le16(x)  ((((x) >> 8) & 0xffu) | (((x) & 0xffu) << 8))
#define cpu_to_le32(x)  \
	((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >>  8) | \
	(((x) & 0x0000ff00u) <<  8) | (((x) & 0x000000ffu) << 24))
#endif

#define le32_to_cpu(x)  le32toh(x)
#define le16_to_cpu(x)  le16toh(x)

static const struct {
	struct usb_functionfs_descs_head_v2 header;
	__le32 fs_count;
	__le32 hs_count;
	__le32 ss_count;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio sink;
		struct usb_endpoint_descriptor_no_audio source;
	} __attribute__((packed)) fs_descs, hs_descs;
	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio sink;
		struct usb_ss_ep_comp_descriptor sink_comp;
		struct usb_endpoint_descriptor_no_audio source;
		struct usb_ss_ep_comp_descriptor source_comp;
	} ss_descs;
} __attribute__((packed)) descriptors = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.flags = cpu_to_le32(FUNCTIONFS_HAS_FS_DESC |
				     FUNCTIONFS_HAS_HS_DESC |
				     FUNCTIONFS_HAS_SS_DESC),
		.length = cpu_to_le32(sizeof descriptors),
	},
	.fs_count = cpu_to_le32(3),
	.fs_descs = {
		.intf = {
			.bLength = sizeof descriptors.fs_descs.intf,
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 2,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.sink = {
			.bLength = sizeof descriptors.fs_descs.sink,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			/* .wMaxPacketSize = autoconfiguration (kernel) */
		},
		.source = {
			.bLength = sizeof descriptors.fs_descs.source,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			/* .wMaxPacketSize = autoconfiguration (kernel) */
		},
	},
	.hs_count = cpu_to_le32(3),
	.hs_descs = {
		.intf = {
			.bLength = sizeof descriptors.fs_descs.intf,
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 2,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.sink = {
			.bLength = sizeof descriptors.hs_descs.sink,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = cpu_to_le16(512),
		},
		.source = {
			.bLength = sizeof descriptors.hs_descs.source,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = cpu_to_le16(512),
			.bInterval = 1, /* NAK every 1 uframe */
		},
	},
	.ss_count = cpu_to_le32(5),
	.ss_descs = {
		.intf = {
			.bLength = sizeof descriptors.fs_descs.intf,
			.bDescriptorType = USB_DT_INTERFACE,
			.bNumEndpoints = 2,
			.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
			.iInterface = 1,
		},
		.sink = {
			.bLength = sizeof descriptors.hs_descs.sink,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 1 | USB_DIR_IN,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = cpu_to_le16(1024),
		},
		.sink_comp = {
			.bLength = USB_DT_SS_EP_COMP_SIZE,
			.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
			.bMaxBurst = 0,
			.bmAttributes = 0,
			.wBytesPerInterval = 0,
		},
		.source = {
			.bLength = sizeof descriptors.hs_descs.source,
			.bDescriptorType = USB_DT_ENDPOINT,
			.bEndpointAddress = 2 | USB_DIR_OUT,
			.bmAttributes = USB_ENDPOINT_XFER_BULK,
			.wMaxPacketSize = cpu_to_le16(1024),
			.bInterval = 1, /* NAK every 1 uframe */
		},
		.source_comp = {
			.bLength = USB_DT_SS_EP_COMP_SIZE,
			.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
			.bMaxBurst = 0,
			.bmAttributes = 0,
			.wBytesPerInterval = 0,
		},
	},
};

#define STR_INTERFACE_ "Source/Sink"

static const struct {
	struct usb_functionfs_strings_head header;
	struct {
		__le16 code;
		const char str1[sizeof STR_INTERFACE_];
	} __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
	.header = {
		.magic = cpu_to_le32(FUNCTIONFS_STRINGS_MAGIC),
		.length = cpu_to_le32(sizeof strings),
		.str_count = cpu_to_le32(1),
		.lang_count = cpu_to_le32(1),
	},
	.lang0 = {
		cpu_to_le16(0x0409), /* en-us */
		STR_INTERFACE_,
	},
};

#define STR_INTERFACE strings.lang0.str1

static uint32_t rng_prev = 0;
static uint32_t stupid_random_u32() {
    return rng_prev*1664525U + 1013904223U; // assuming complement-2 integers and non-signaling overflow
}

static int msleep(long msec)
{
    struct timespec ts;
    int res;

    if (msec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    pthread_yield_np();

    return res;
}

static int _usleep(long usec)
{
    struct timespec ts;
    int res;

    if (usec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = usec / 1000;
    ts.tv_nsec = (usec % 1000) * 1000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    pthread_yield_np();

    return res;
}

static void write_str_to_file(const char* fpath, const char* val)
{
    FILE* f = fopen(fpath, "wb+");
    if (f)
    {
        fputs(val, f);
        fflush(f);
        fclose(f);
    }
    else
    {
        printf("Failed to open `%s`!\n", fpath);
    }
}

static void write_hex16_to_file(const char* fpath, uint16_t val)
{
    char tmp[8];
    snprintf(tmp, sizeof(tmp), "0x%x", val);
    write_str_to_file(fpath, tmp);
}

static void write_decimal_to_file(const char* fpath, uint16_t val)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%i", val);
    write_str_to_file(fpath, tmp);
}


#if 0

kern_return_t IOUSBDeviceInterface_WritePipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, const void* data, uint32_t len, uint64_t timeoutMs);
kern_return_t IOUSBDeviceInterface_GetPipeCurrentMaxPacketSize(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, uint64_t* pOut);
kern_return_t IOUSBDeviceInterface_AbortPipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id);
kern_return_t IOUSBDeviceInterface_StallPipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id);

kern_return_t IOUSBDeviceInterface_Open(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num)
{
    uint64_t args[1] = { 0 };

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    return IOConnectCallScalarMethod(pIface->port, 0, args, 1, NULL, NULL);
}

kern_return_t IOUSBDeviceInterface_Close(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    return IOConnectCallScalarMethod(pIface->port, 1, NULL, 0, NULL, NULL);
}

kern_return_t IOUSBDeviceInterface_SetClass(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t val)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {val, pImplCtx->configId};
    return IOConnectCallScalarMethod(pIface->port, 3, args, 2, NULL, NULL);
}

kern_return_t IOUSBDeviceInterface_SetSubClass(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t val)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {val, pImplCtx->configId};
    return IOConnectCallScalarMethod(pIface->port, 4, args, 2, NULL, NULL);
}

kern_return_t IOUSBDeviceInterface_SetProtocol(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t val)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {val, pImplCtx->configId};
    return IOConnectCallScalarMethod(pIface->port, 5, args, 2, NULL, NULL);
}

kern_return_t IOUSBDeviceInterface_CreatePipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t type, uint8_t direction, uint32_t maxPktSize, uint8_t interval, uint64_t unk, uint64_t* pPipeOut)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint32_t outputCount = 1;
    uint64_t output[1] = {0};
    uint64_t args[6] = {type, direction, maxPktSize, interval, unk, pImplCtx->configId};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 10, args, 6, output, &outputCount);

    if (pPipeOut)
        *pPipeOut = output[0];

    return ret;
}

kern_return_t IOUSBDeviceInterface_AppendStandardClassOrVendorDescriptor(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t descType, uint8_t unk, const uint8_t* pDesc, uint64_t descSz)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {descType, unk};

    kern_return_t ret = IOConnectCallMethod(pIface->port, 6, args, 2, pDesc, descSz, NULL, NULL, NULL, NULL);

    return ret;
}

kern_return_t IOUSBDeviceInterface_AppendNonstandardClassOrVendorDescriptor(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint8_t descType, uint8_t unk, const uint8_t* pDesc, uint64_t descSz)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {descType, unk};

    kern_return_t ret = IOConnectCallMethod(pIface->port, 7, args, 2, pDesc, descSz, NULL, NULL, NULL, NULL);

    return ret;
}

kern_return_t IOUSBDeviceInterface_CompleteClassCommandCallback(libusbd_linux_iface_t* pIface, uint8_t iface_num, libusbd_setup_callback_info_t* info, uint64_t* arguments)
{
    uint64_t args[5] = {1, info->out_len, (uint64_t)info->out_data, 0, arguments[4]};
    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 9, args, 5, NULL, NULL);

    //printf("complete %x\n", ret);

    return ret;
}

void IOUSBDeviceInterface_ClassCommandCallback(void* refcon, IOReturn result, uint64_t* arguments)
{
    libusbd_linux_iface_t* pIface = (libusbd_linux_iface_t*)refcon;

    pIface->class_async_done = 1;

    // We get two callbacks, the second can be ignored
    //bool is_done = arguments[0];
    //if (is_done) return;

    pIface->setup_callback_info.bmRequestType = (arguments[1] >> 24) & 0xFF;
    pIface->setup_callback_info.bRequest = (arguments[1] >> 16) & 0xFF;
    pIface->setup_callback_info.wValue = (arguments[1]) & 0xFFFF;
    pIface->setup_callback_info.wIndex = (arguments[2] >> 16) & 0xFFFF;
    pIface->setup_callback_info.wLength = (arguments[2]) & 0xFFFF;

    pIface->setup_callback_info.out_len = 0;
    pIface->setup_callback_info.out_data = pIface->setup_buffer.data;

    if (pIface->setup_callback)
        pIface->setup_callback(&pIface->setup_callback_info);
    
    //printf("AAAAAAA %x %p, %llx %llx %llx %llx %llx\n", result, arguments, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);

    IOUSBDeviceInterface_CompleteClassCommandCallback(pIface, 0, &pIface->setup_callback_info, arguments); // TODO bad
}

kern_return_t IOUSBDeviceInterface_SetClassCommandCallbacks(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, bool a, bool b, bool c)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    uint64_t args[3] = {a,b,c};

    uint64_t asyncRef[8];
    asyncRef[kIOAsyncReservedIndex] = pIface->mnotification_port;
    asyncRef[kIOAsyncCalloutFuncIndex] = (uint64_t)IOUSBDeviceInterface_ClassCommandCallback;
    asyncRef[kIOAsyncCalloutRefconIndex] = (uint64_t)pIface;

    pIface->class_async_done = 0;
    kern_return_t ret = IOConnectCallAsyncScalarMethod(pIface->port, 8, pIface->mnotification_port, asyncRef, kOSAsyncRef64Count, args, 3, NULL, NULL);

    if (ret) { 
        return ret;
    }

    return ret;
}

kern_return_t IOUSBDeviceInterface_CommitConfiguration(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 11, NULL, 0, NULL, NULL);

    return ret;
}

kern_return_t IOUSBDeviceInterface_CreateBuffer(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint32_t bufferSz, libusbd_linux_buffer_t* pOut)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint32_t outputCount = 3;
    uint64_t output[3] = {0};
    uint64_t args[1] = {bufferSz};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 18, args, 1, output, &outputCount);

    if (pOut) {
        pOut->data = (void*)output[0];
        pOut->size = output[1];
        pOut->token = output[2];
    }

    return ret;
}

kern_return_t IOUSBDeviceInterface_ReleaseBuffer(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, libusbd_linux_buffer_t* pBuffer)
{
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[1] = {pBuffer->token};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 19, args, 1, NULL, NULL);

    return ret;
}

void IOUSBDeviceInterface_ReadPipeCallback(void* refcon, IOReturn result, uint64_t* arguments)
{
    libusbd_linux_ep_t* pEp = (libusbd_linux_ep_t*)refcon;
    pEp->last_transferred = (uint64_t)arguments;

    if (pEp->last_transferred || result) {
        pEp->ep_async_done = 1;
    }

    pEp->last_error = result;

    //if (pEp->last_transferred)
    //    printf("libusbd linux: Read %x bytes %x (%x)\n", pEp->last_transferred, result, *(uint32_t*)pEp->buffer.data);
}

void IOUSBDeviceInterface_WritePipeCallback(void* refcon, IOReturn result, uint64_t* arguments)
{
    libusbd_linux_ep_t* pEp = (libusbd_linux_ep_t*)refcon;
    pEp->last_transferred = (uint64_t)arguments;

    if (pEp->last_transferred || result) {
        pEp->ep_async_done = 1;
    }

    pEp->last_error = result;

    //printf("write callback %llx %x\n", pEp->last_transferred, pEp->last_error);

    //if (pEp->last_transferred)
    //    printf("libusbd linux: Read %x bytes %x (%x)\n", pEp->last_transferred, result, *(uint32_t*)pEp->buffer.data);
}

kern_return_t IOUSBDeviceInterface_ReadPipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, void* data, uint32_t len, uint64_t timeoutMs)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[pipe_id];

    libusbd_linux_buffer_t* pBuffer = &pEp->buffer;
    uint64_t args[4] = {pipe_id, pBuffer->token, len, timeoutMs};
    uint32_t outputCount = 1;
    uint64_t output[1] = {0};

    uint64_t asyncRef[8];
    asyncRef[kIOAsyncReservedIndex] = pEp->mnotification_port;
    asyncRef[kIOAsyncCalloutFuncIndex] = (uint64_t)IOUSBDeviceInterface_ReadPipeCallback;
    asyncRef[kIOAsyncCalloutRefconIndex] = (uint64_t)pEp;

    pEp->ep_async_done = 0;
    pEp->last_transferred = 0;
    pEp->last_error = 0;
    kern_return_t ret = 0;

    if (data && pBuffer->data && len) {
        if (len > pBuffer->size) return LIBUSBD_LINUX_FAKERET_BADARGS;
    }

    // The actual read request, size `len`
    ret = IOConnectCallAsyncScalarMethod(pIface->port, 13, pEp->mnotification_port, asyncRef, kOSAsyncRef64Count, args, 4, output, &outputCount);

    // For some reason you have to call it again, size `maxPktSize - len`
    args[2] = pEp->maxPktSize - len;
    if (args[2])
        IOConnectCallAsyncScalarMethod(pIface->port, 13, pEp->mnotification_port, asyncRef, kOSAsyncRef64Count, args, 4, output, &outputCount);
    // TODO ret check

    if (ret) {
        if (ret != LIBUSBD_LINUX_ERR_NOTACTIVATED)
            printf("libusbd linux: Unexpected error during IOUSBDeviceInterface_ReadPipe: %x (output %llx)\n", ret, output[0]);
        return ret;
    }

    msleep(0);

    uint64_t i = 0;
    for (i = 0; i < timeoutMs*1000; i++) 
    {
        if (pEp->ep_async_done) break;
        _usleep(1);
    }

    if (i == timeoutMs && !pEp->ep_async_done) {
        IOUSBDeviceInterface_AbortPipe(pImplCtx, iface_num, pipe_id);
        return 0;
    }

    ret = pEp->last_transferred;

    if (data && pBuffer->data && data != pBuffer->data && len) {
        memcpy(data, pBuffer->data, len);
    }

    return ret;
}

kern_return_t IOUSBDeviceInterface_ReadPipeStart(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, uint32_t len, uint64_t timeoutMs)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[pipe_id];

    libusbd_linux_buffer_t* pBuffer = &pEp->buffer;
    uint64_t args[4] = {pipe_id, pBuffer->token, len, timeoutMs};
    uint32_t outputCount = 1;
    uint64_t output[1];

    uint64_t asyncRef[8];
    asyncRef[kIOAsyncReservedIndex] = pEp->mnotification_port;
    asyncRef[kIOAsyncCalloutFuncIndex] = (uint64_t)IOUSBDeviceInterface_ReadPipeCallback;
    asyncRef[kIOAsyncCalloutRefconIndex] = (uint64_t)pEp;

    pEp->ep_async_done = 0;
    pEp->last_transferred = 0;
    pEp->last_error = 0;

    kern_return_t ret = IOConnectCallAsyncScalarMethod(pIface->port, 13, pEp->mnotification_port, asyncRef, kOSAsyncRef64Count, args, 4, output, &outputCount);

    return ret;
}

kern_return_t IOUSBDeviceInterface_WritePipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, const void* data, uint32_t len, uint64_t timeoutMs)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[pipe_id];

    libusbd_linux_buffer_t* pBuffer = &pEp->buffer;
    uint64_t args[4] = {pipe_id, pBuffer->token, len, timeoutMs};
    uint32_t outputCount = 1;
    uint64_t output[1];

    if (data && pBuffer->data && data != pBuffer->data && len) {
        if (len > pBuffer->size) return LIBUSBD_LINUX_FAKERET_BADARGS;

        memcpy(pBuffer->data, data, len);
    }

    pEp->ep_async_done = 0;
    pEp->last_transferred = 0;
    pEp->last_error = 0;

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 14, args, 4, output, &outputCount);
    if (!ret) {
        ret = output[0];
        pEp->last_transferred = ret;
    }

    return ret;
}

kern_return_t IOUSBDeviceInterface_WritePipeStart(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, const void* data, uint32_t len, uint64_t timeoutMs)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[pipe_id];

    libusbd_linux_buffer_t* pBuffer = &pEp->buffer;
    uint64_t args[4] = {pipe_id, pBuffer->token, len, timeoutMs};
    uint32_t outputCount = 1;
    uint64_t output[1];

    if (data && pBuffer->data && data != pBuffer->data && len) {
        if (len > pBuffer->size) return LIBUSBD_LINUX_FAKERET_BADARGS;

        memcpy(pBuffer->data, data, len);
    }

    uint64_t asyncRef[8];
    asyncRef[kIOAsyncReservedIndex] = pEp->mnotification_port;
    asyncRef[kIOAsyncCalloutFuncIndex] = (uint64_t)IOUSBDeviceInterface_WritePipeCallback;
    asyncRef[kIOAsyncCalloutRefconIndex] = (uint64_t)pEp;

    pEp->ep_async_done = 0;
    pEp->last_transferred = 0;
    pEp->last_error = 0;
    kern_return_t ret = IOConnectCallAsyncScalarMethod(pIface->port, 14, pEp->mnotification_port, asyncRef, kOSAsyncRef64Count, args, 4, output, &outputCount);

    return ret;
}

kern_return_t IOUSBDeviceInterface_StallPipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[1] = {pipe_id};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 15, args, 1, NULL, NULL);

    return ret;
}

kern_return_t IOUSBDeviceInterface_AbortPipe(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[1] = {pipe_id};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 16, args, 1, NULL, NULL);

    return ret;
}

kern_return_t IOUSBDeviceInterface_GetPipeCurrentMaxPacketSize(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, uint64_t* pOut)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[1] = {pipe_id};
    uint32_t outputCount = 1;
    uint64_t output[1];

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 17, args, 1, output, &outputCount);

    if (pOut)
        *pOut = output[0];

    return ret;
}

kern_return_t IOUSBDeviceInterface_SetPipeProperty(libusbd_linux_ctx_t* pImplCtx, uint8_t iface_num, uint64_t pipe_id, uint32_t val)
{
    if (iface_num >= LIBUSBD_MAX_IFACES) return LIBUSBD_LINUX_FAKERET_BADARGS;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    uint64_t args[2] = {pipe_id, val};

    kern_return_t ret = IOConnectCallScalarMethod(pIface->port, 27, args, 2, NULL, NULL);

    return ret;
}
#endif

int libusbd_impl_init(libusbd_ctx_t* pCtx)
{
    if (!pCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    pCtx->pLinuxCtx = malloc(sizeof(libusbd_linux_ctx_t));
    memset(pCtx->pLinuxCtx, 0, sizeof(*pCtx->pLinuxCtx));

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    
    mkdir("/sys/kernel/config/usb_gadget/libusbd", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/functions", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/functions/ffs.usb0", 0777);
    //mkdir("/sys/kernel/config/usb_gadget/libusbd/functions/ncm.usb0", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/strings", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/strings/0x0409", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/configs", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/configs/c.1", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/strings", 0777);
    mkdir("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/strings/0x0409", 0777);
    
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/idVendor", 0x1d6b);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/idProduct", 0x0052);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bcdDevice", 0x0100);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bcdUSB", 0x0200);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceClass", 0x0);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceSubClass", 0x0);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceProtocol", 0x0);
    write_decimal_to_file("/sys/kernel/config/usb_gadget/libusbd/bMaxPacketSize0", 64);
    write_decimal_to_file("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/MaxPower", 50);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/bmAttributes", 0xc0);
    
    write_str_to_file("/sys/kernel/config/usb_gadget/libusbd/UDC", "");
    
    symlink("/sys/kernel/config/usb_gadget/libusbd/functions/ffs.usb0", "/sys/kernel/config/usb_gadget/libusbd/configs/c.1/ffs.usb0");
    
    mkdir("/dev/ffs-usb0", 0777);
    system("mount -t functionfs usb0 /dev/ffs-usb0");
    
    int fd = open("/dev/ffs-usb0/ep0", O_RDWR);
    write(fd, &descriptors, sizeof(descriptors));
    write(fd, &strings, sizeof(strings));
    //fclose(f);

#if 0
    // TODO: A lot more error checking
    io_iterator_t       iter    = 0;
    kern_return_t s_ret;
    IOReturn ret;
    kern_return_t open_ret;

    // Seed the RNG
    rng_prev = ((intptr_t)(pImplCtx))>>16;

    pImplCtx->iface_rand32 = stupid_random_u32();

    LaunchRunLoopThread();

    
    //IOUSBDeviceControllerSendCommand(controller, CFSTR("ShowDeviceDebug"), NULL); 

    for (int i = 0; i < 5; i++)
    {
        CFMutableDictionaryRef match = IOServiceMatching("gay_bowser_usbgadget");
        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        //CFDictionarySetValue(dict, CFSTR("USBDeviceFunction"), CFSTR("MyUSBData"));
        CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), dict);

        CFRetain(match);
        ret = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
        if (ret != KERN_SUCCESS || iter == 0) {
            printf("libusbd linux: Error matching gay_bowser_usbgadget (%x)...\n", ret);
            sleep(1);
            continue;
        }

        // Get the third port
        while (1)
        {
            pImplCtx->service_usbgadget = IOIteratorNext(iter);
            if (!pImplCtx->service_usbgadget) break;

            io_string_t path;
            if (IORegistryEntryGetPath(pImplCtx->service_usbgadget, kIOServicePlane, path) != KERN_SUCCESS) {
                IOObjectRelease(pImplCtx->service_usbgadget);
                continue;
            }

            if (strstr(path, "usb-drd2")) {
                printf("libusbd linux: Connecting to: '%s'\n", path);
                break;
            }

            IOObjectRelease(pImplCtx->service_usbgadget);
        } 

        CFRelease(match);
        IOObjectRelease(iter);

        if (pImplCtx->service_usbgadget) break;
    }

    if (!pImplCtx->service_usbgadget) {
        printf("libusbd linux: Failed to find gay_bowser_usbgadget, aborting...\n");
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    alt_IOUSBDeviceControllerCreateFromService(kCFAllocatorDefault, pImplCtx->service_usbgadget, &pImplCtx->controller);

    pImplCtx->desc = alt_IOUSBDeviceDescriptionCreate(kCFAllocatorDefault);//alt_IOUSBDeviceDescriptionCreateFromController(kCFAllocatorDefault, controller);
    //alt_IOUSBDeviceDescriptionSetSerialString(pImplCtx->desc, serial);

    alt_IOUSBDeviceDescriptionRemoveAllConfigurations(pImplCtx->desc);

    pImplCtx->configId = alt_IOUSBDeviceDescriptionAppendConfiguration(pImplCtx->desc, CFSTR("libusbd"), 0xA0, 255);
    
    // Setup pkt source
    CFRunLoopSourceRef run_loop_source;

    // Create port to listen for kernel notifications on.
    pImplCtx->notification_port = IONotificationPortCreate(kIOMainPortDefault);
    if (!pImplCtx->notification_port) {
        printf("libusbd linux: Error getting notification port.\n");
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    // Get lower level mach port from notification port.
    pImplCtx->mnotification_port = IONotificationPortGetMachPort(pImplCtx->notification_port);
    if (!pImplCtx->mnotification_port) {
        printf("libusbd linux: Error getting mach notification port.\n");
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    // Create a run loop source from our notification port so we can add the port to our run loop.
    run_loop_source = IONotificationPortGetRunLoopSource(pImplCtx->notification_port);
    if (run_loop_source == NULL) {
        printf("libusbd linux: Error getting run loop source.\n");
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    // Add the notification port and timer to the run loop.
    CFRunLoopAddSource(_runLoop, run_loop_source, kCFRunLoopDefaultMode);
#endif
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_free(libusbd_ctx_t* pCtx)
{
    //kern_return_t s_ret;

    if (!pCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

#if 0
    // setDesc
    /*outputCount = 0;
    output[0] = some_str;
    args[0] = some_str; // idk
    s_ret = IOConnectCallMethod(pIface->port, 2, NULL, 0, args, 1, NULL, NULL, NULL, NULL);
    printf("setdesc s_ret %x %x (%x %x)\n", s_ret, outputCount, output[0], output[1]);*/

    for (int j = 0; j < pCtx->bNumInterfaces; j++)
    {
        libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[j];

        free(pIface->pName);
        pIface->pName = NULL;

        if (pIface->is_builtin) {
            continue;
        }

        for (int i = 0; i < pIface->bNumEndpoints; i++)
        {
            libusbd_linux_ep_t* pEp = &pIface->aEndpoints[i];
            s_ret = IOUSBDeviceInterface_ReleaseBuffer(pImplCtx, j, &pEp->buffer); // TODO check
            //printf("release s_ret %x\n", s_ret);

            IONotificationPortDestroy(pEp->notification_port);
        }
        IONotificationPortDestroy(pIface->notification_port);

        s_ret = IOUSBDeviceInterface_Close(pImplCtx, j); // TODO check
        //printf("close s_ret %x\n", s_ret);

        IOServiceClose(pIface->port);
    }

    IONotificationPortDestroy(pImplCtx->notification_port);
#endif
    free(pImplCtx);
    pCtx->pLinuxCtx = NULL;

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_config_finalize(libusbd_ctx_t* pCtx)
{
    //IOReturn ret;
    //kern_return_t s_ret;
    //io_iterator_t iter = 0;
    //kern_return_t open_ret;
    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    
    if (pCtx->vid)
        write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/idVendor", pCtx->vid);
    
    if (pCtx->pid)
        write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/idProduct", pCtx->pid);

    if (pCtx->did)
        write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bcdDevice", pCtx->did);

    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bcdUSB", 0x0200);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceClass", pCtx->bClass);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceSubClass", pCtx->bSubclass);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/bDeviceProtocol", pCtx->bProtocol);
    write_decimal_to_file("/sys/kernel/config/usb_gadget/libusbd/bMaxPacketSize0", 64);
    write_decimal_to_file("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/MaxPower", 50);
    write_hex16_to_file("/sys/kernel/config/usb_gadget/libusbd/configs/c.1/bmAttributes", 0xc0);
    
    if (pCtx->pManufacturerStr) {
        write_str_to_file("/sys/kernel/config/usb_gadget/libusbd/strings/0x0409/manufacturer", pCtx->pManufacturerStr);
    }
    if (pCtx->pProductStr) {
        write_str_to_file("/sys/kernel/config/usb_gadget/libusbd/strings/0x0409/product", pCtx->pProductStr);
    }
    if (pCtx->pSerialStr) {
        write_str_to_file("/sys/kernel/config/usb_gadget/libusbd/strings/0x0409/serialnumber", pCtx->pSerialStr);
    }
    
    // Bind the configuration
    DIR* d;
    struct dirent *dir;
    d = opendir("/sys/class/udc");
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, "..")) continue;

            write_str_to_file("/sys/kernel/config/usb_gadget/libusbd/UDC", dir->d_name);
            printf("Binding to port: %s\n", dir->d_name);
            break;
        }
        closedir(d);
    }

#if 0
    if (alt_IOUSBDeviceControllerSetDescription(pImplCtx->controller, pImplCtx->desc)) {
        return LIBUSBD_NONDESCRIPT_ERROR;
    }


    if (IORegistryEntrySetCFProperty(pImplCtx->service_usbgadget, CFSTR("Poke"), CFSTR("Poke"))) {
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    IOObjectRelease(pImplCtx->service_usbgadget);

    alt_IOUSBDeviceControllerRelease(pImplCtx->controller);
    CFAllocatorDeallocate(kCFAllocatorDefault, pImplCtx->controller);
    pImplCtx->controller = NULL;

    alt_IOUSBDeviceDescriptionRelease(pImplCtx->desc);
    CFAllocatorDeallocate(kCFAllocatorDefault, pImplCtx->desc);
    pImplCtx->desc = NULL;

    for (int j = 0; j < pCtx->bNumInterfaces; j++)
    {
        libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[j];

        // Built-ins don't need us to connect, the kernel driver connects to it.
        if (pIface->is_builtin) {
            continue;
        }

        for (int i = 0; i < 10; i++)
        {
            CFStringRef name = CFStringCreateWithCString(NULL, pIface->pName, 0);
            CFMutableDictionaryRef match = IOServiceMatching("IOUSBDeviceInterface");
            CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(dict, CFSTR("USBDeviceFunction"), name);
            CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), dict);

            CFRetain(match);
            ret = IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter);
            if (ret != KERN_SUCCESS || iter == 0) {
                printf("libusbd linux: Error matching IOUSBDeviceInterface (%x)...\n", ret);
                sleep(1);
                continue;
            }

            // Get the third port
            while (1)
            {
                pIface->service = IOIteratorNext(iter);
                if (!pIface->service) break;

                io_string_t path;
                if (IORegistryEntryGetPath(pIface->service, kIOServicePlane, path) != KERN_SUCCESS) {
                    IOObjectRelease(pIface->service);
                    continue;
                }

                if (strstr(path, "usb-drd2")) {
                    printf("libusbd linux: Connecting to: '%s'\n", path);
                    break;
                }
                IOObjectRelease(pIface->service);
            } 

            CFRelease(match);
            IOObjectRelease(iter);

            if (pIface->service) break;
        }

        if (!pIface->service) {
            printf("libusbd linux: Failed to find IOUSBDeviceInterface, aborting...\n");
            return LIBUSBD_NONDESCRIPT_ERROR;
        }

        open_ret = IOServiceOpen(pIface->service, mach_task_self(), 123, &pIface->port); // TODO check
        //printf("open_ret %x %x\n", open_ret, pIface->port);
        //IOServiceClose(pIface->service);
        IOObjectRelease(pIface->service);

        
        // Open
        s_ret = IOUSBDeviceInterface_Open(pImplCtx, j); // TODO check
        //printf("open s_ret %x\n", s_ret);
    }
#endif
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_finalize(libusbd_ctx_t* pCtx, uint8_t iface_num)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    if (pIface->is_builtin) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

#if 0
    IOUSBDeviceInterface_CreateBuffer(pImplCtx, iface_num, 0x1000, &pIface->setup_buffer); // TODO EP max size, error

    for (int k = 0; k < pIface->bNumEndpoints; k++)
    {
        libusbd_linux_ep_t* pEp = &pIface->aEndpoints[k];
        IOUSBDeviceInterface_CreateBuffer(pImplCtx, iface_num, 0x1000, &pEp->buffer); // TODO EP max size, error

        CFRunLoopSourceRef run_loop_source;

        // Create port to listen for kernel notifications on.
        pEp->notification_port = IONotificationPortCreate(kIOMainPortDefault);
        if (!pEp->notification_port) {
            printf("libusbd linux: Error getting notification port.\n");
            return 0;
        }

        // Get lower level mach port from notification port.
        pEp->mnotification_port = IONotificationPortGetMachPort(pEp->notification_port);
        if (!pEp->mnotification_port) {
            printf("libusbd linux: Error getting mach notification port.\n");
            return 0;
        }

        // Create a run loop source from our notification port so we can add the port to our run loop.
        run_loop_source = IONotificationPortGetRunLoopSource(pEp->notification_port);
        if (run_loop_source == NULL) {
            printf("libusbd linux: Error getting run loop source.\n");
            return 0;
        }

        // Add the notification port and timer to the run loop.
        CFRunLoopAddSource(_runLoop, run_loop_source, kCFRunLoopDefaultMode);
    }

    kern_return_t ret = IOUSBDeviceInterface_CommitConfiguration(pImplCtx, iface_num); // TODO check

    IOUSBDeviceInterface_SetClassCommandCallbacks(pImplCtx, iface_num, true, false, false); // second bool sets whether to report successful sends?
#endif
    pCtx->aInterfaces[iface_num].finalized = true;

    return LIBUSBD_SUCCESS;
}


int libusbd_impl_iface_standard_desc(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t descType, uint8_t unk, const uint8_t* pDesc, uint64_t descSz)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

    //IOUSBDeviceInterface_AppendStandardClassOrVendorDescriptor(pImplCtx, iface_num, descType, unk, pDesc, descSz); // TODO error check
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_nonstandard_desc(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t descType, uint8_t unk, const uint8_t* pDesc, uint64_t descSz)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

    //IOUSBDeviceInterface_AppendNonstandardClassOrVendorDescriptor(pImplCtx, iface_num, descType, unk, pDesc, descSz); // TODO error check
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_add_endpoint(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t type, uint8_t direction, uint32_t maxPktSize, uint8_t interval, uint64_t unk, uint64_t* pEpOut)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }
    if (pIface->bNumEndpoints >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_RESOURCE_LIMIT_REACHED;
    }

    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[pIface->bNumEndpoints];
    pEp->maxPktSize = maxPktSize;

    pIface->bNumEndpoints++;

    //kern_return_t ret = IOUSBDeviceInterface_CreatePipe(pImplCtx, iface_num, type, direction, maxPktSize, interval, unk, pEpOut);

    //printf("create %x %x\n", ret, *pEpOut);
    //IOUSBDeviceInterface_SetPipeProperty(pImplCtx, iface_num, *pEpOut, 0);

    return LIBUSBD_SUCCESS;
}

static int libusbd_impl_iface_alloc_builtin_internal(libusbd_ctx_t* pCtx, const char* name)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    // TODO oob
    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[pCtx->bNumInterfaces];

    pIface->pName = malloc(0x80);
    strncpy(pIface->pName, name, 0x80);
    //CFStringRef nameRef = CFStringCreateWithCString(NULL, pIface->pName, 0);

    pIface->is_builtin = 1;
    pCtx->aInterfaces[pCtx->bNumInterfaces].finalized = 1;

    //alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(pImplCtx->desc, pImplCtx->configId, nameRef);

    pCtx->bNumInterfaces += 1;

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_alloc_builtin(libusbd_ctx_t* pCtx, const char* name)
{
    if (!pCtx || !pCtx->pLinuxCtx || !name) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    int ret = 0;

    if (!strcmp(name, "libusbd_std_ncm")) {
        //if (ret = libusbd_impl_iface_alloc_builtin_internal(pCtx, "AppleUSBNCMControl")) return ret;
    }
    else
    {
        if (ret = libusbd_impl_iface_alloc_builtin_internal(pCtx, name)) return ret;
    }

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_alloc(libusbd_ctx_t* pCtx)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    // TODO oob
    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[pCtx->bNumInterfaces];

    // We have to include a random element, otherwise we might connect to a port which is about to
    // be replaced.
    pIface->pName = malloc(0x80);
    snprintf(pIface->pName, 0x80, "iface-%08x-%u", pImplCtx->iface_rand32, pCtx->bNumInterfaces);
    //CFStringRef name = CFStringCreateWithCString(NULL, pIface->pName, 0);

    //alt_IOUSBDeviceDescriptionAppendInterfaceToConfiguration(pImplCtx->desc, pImplCtx->configId, name);

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_set_class(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t val)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

    /*kern_return_t ret = IOUSBDeviceInterface_SetClass(pImplCtx, iface_num, val);

    if (ret) {
        return LIBUSBD_NONDESCRIPT_ERROR;
    }*/

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_set_subclass(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t val)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

    /*kern_return_t ret = IOUSBDeviceInterface_SetSubClass(pImplCtx, iface_num, val);

    if (ret) {
        return LIBUSBD_NONDESCRIPT_ERROR;
    }*/

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_set_protocol(libusbd_ctx_t* pCtx, uint8_t iface_num, uint8_t val)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    if (pCtx->aInterfaces[iface_num].finalized) {
        return LIBUSBD_ALREADY_FINALIZED;
    }

    /*kern_return_t ret = IOUSBDeviceInterface_SetProtocol(pImplCtx, iface_num, val);

    if (ret) {
        return LIBUSBD_NONDESCRIPT_ERROR;
    }*/

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_iface_set_class_cmd_callback(libusbd_ctx_t* pCtx, uint8_t iface_num, libusbd_setup_callback_t func)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];

    // TODO check finalized?

    pIface->setup_callback = func;

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_read(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep, void* data, uint32_t len, uint64_t timeoutMs)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    /*kern_return_t ret = IOUSBDeviceInterface_ReadPipe(pImplCtx, iface_num, ep, data, len, timeoutMs);
    if (ret == LIBUSBD_LINUX_ERR_NOTACTIVATED)
    {
        return LIBUSBD_NOT_ENUMERATED;
    }
    else if (ret == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }
    else if (ret == LIBUSBD_LINUX_FAKERET_BADARGS)
    {
        return LIBUSBD_INVALID_ARGUMENT;
    }
    else if (ret & 0xFFF00000)
    {
        printf("libusbd linux: Unknown error from IOUSBDeviceInterface_ReadPipe: %08x\n", ret);
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    return ret;*/
    return LIBUSBD_SUCCESS;
}


int libusbd_impl_ep_write(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep, const void* data, uint32_t len, uint64_t timeoutMs)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    /*kern_return_t ret = IOUSBDeviceInterface_WritePipe(pImplCtx, iface_num, ep, data, len, timeoutMs);
    if (ret == LIBUSBD_LINUX_ERR_NOTACTIVATED)
    {
        return LIBUSBD_NOT_ENUMERATED;
    }
    else if (ret == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }
    else if (ret == LIBUSBD_LINUX_FAKERET_BADARGS)
    {
        return LIBUSBD_INVALID_ARGUMENT;
    }
    else if (ret & 0xFFF00000)
    {
        printf("libusbd linux: Unknown error from IOUSBDeviceInterface_WritePipe: %08x\n", ret);
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    return ret;*/
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_stall(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    //IOUSBDeviceInterface_StallPipe(pImplCtx, iface_num, ep);

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_abort(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    //IOUSBDeviceInterface_AbortPipe(pImplCtx, iface_num, ep);

    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_get_buffer(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep, void** pOut)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[ep]; // TODO oob

    *pOut = pEp->buffer.data;

    return (pEp->buffer.size & 0x7FFFFFFF);
}

int libusbd_impl_ep_read_start(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep, uint32_t len, uint64_t timeout_ms)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    /*kern_return_t ret = IOUSBDeviceInterface_ReadPipeStart(pImplCtx, iface_num, ep, len, timeout_ms);
    if (ret == LIBUSBD_LINUX_ERR_NOTACTIVATED)
    {
        return LIBUSBD_NOT_ENUMERATED;
    }
    else if (ret == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }
    else if (ret == LIBUSBD_LINUX_FAKERET_BADARGS)
    {
        return LIBUSBD_INVALID_ARGUMENT;
    }
    else if (ret & 0xFFF00000)
    {
        printf("libusbd linux: Unknown error from IOUSBDeviceInterface_ReadPipeStart: %08x\n", ret);
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    return ret;*/
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_write_start(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep, const void* data, uint32_t len, uint64_t timeout_ms)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;

    /*kern_return_t ret = IOUSBDeviceInterface_WritePipeStart(pImplCtx, iface_num, ep, data, len, timeout_ms);
    if (ret == LIBUSBD_LINUX_ERR_NOTACTIVATED)
    {
        return LIBUSBD_NOT_ENUMERATED;
    }
    else if (ret == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }
    else if (ret == LIBUSBD_LINUX_FAKERET_BADARGS)
    {
        return LIBUSBD_INVALID_ARGUMENT;
    }
    else if (ret & 0xFFF00000)
    {
        printf("libusbd linux: Unknown error from IOUSBDeviceInterface_WritePipeStart: %08x\n", ret);
        return LIBUSBD_NONDESCRIPT_ERROR;
    }

    return ret;*/
    return LIBUSBD_SUCCESS;
}

int libusbd_impl_ep_transfer_done(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[ep];

    if (pEp->last_error == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }

    return pEp->ep_async_done;
}

int libusbd_impl_ep_transferred_bytes(libusbd_ctx_t* pCtx, uint8_t iface_num, uint64_t ep)
{
    if (!pCtx || !pCtx->pLinuxCtx) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (iface_num >= LIBUSBD_MAX_IFACES) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    if (ep >= LIBUSBD_MAX_IFACE_EPS) {
        return LIBUSBD_INVALID_ARGUMENT;
    }

    libusbd_linux_ctx_t* pImplCtx = pCtx->pLinuxCtx;
    libusbd_linux_iface_t* pIface = &pImplCtx->aInterfaces[iface_num];
    libusbd_linux_ep_t* pEp = &pIface->aEndpoints[ep];

    if (pEp->last_error == LIBUSBD_LINUX_ERR_TIMEOUT)
    {
        return LIBUSBD_TIMEOUT;
    }

    return pEp->last_transferred;
}
