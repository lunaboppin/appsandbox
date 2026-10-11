/* AppSandboxVHID.c - KMDF Virtual HID Framework relative mouse.
 *
 * A user-mode helper writes relative mouse reports through a small buffered
 * IOCTL.  VHF exposes those reports to Windows as a genuine HID mouse, which
 * lets DirectInput/Raw Input games consume them without cursor warping.
 */
#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include "appsandbox_vhid_ioctl.h"

#pragma pack(push, 1)
typedef struct _ASB_MOUSE_REPORT {
    UCHAR reportId;
    UCHAR buttons;
    SHORT x;
    SHORT y;
    CHAR  wheel;
} ASB_MOUSE_REPORT;
#pragma pack(pop)

typedef struct _DEVICE_CONTEXT {
    VHFHANDLE vhf;
    WDFWAITLOCK submitLock;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AsbVhidEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AsbVhidEvtDeviceCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AsbVhidEvtIoDeviceControl;

static const UCHAR g_mouseDescriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /*   Report ID (1) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Button) */
    0x19, 0x01,       /*     Usage Minimum (1) */
    0x29, 0x05,       /*     Usage Maximum (5) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x75, 0x01,       /*     Report Size (1) */
    0x95, 0x05,       /*     Report Count (5) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
    0x75, 0x03,       /*     Report Size (3) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x03,       /*     Input (Constant) */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x16, 0x01, 0x80, /*     Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767) */
    0x75, 0x10,       /*     Report Size (16) */
    0x95, 0x02,       /*     Report Count (2) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0x09, 0x38,       /*     Usage (Wheel) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0xC0,             /*   End Collection */
    0xC0              /* End Collection */
};

VOID AsbVhidEvtIoDeviceControl(WDFQUEUE queue, WDFREQUEST request,
                               size_t outputLength, size_t inputLength,
                               ULONG ioControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(queue));
    AsbVhidMouseInput *input = NULL;
    ASB_MOUSE_REPORT report;
    HID_XFER_PACKET packet;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(outputLength);
    UNREFERENCED_PARAMETER(inputLength);

    if (ioControlCode != IOCTL_ASB_VHID_MOUSE) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(request, sizeof(*input),
                                           (PVOID *)&input, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    if (input->dx < -32767 || input->dx > 32767 ||
        input->dy < -32767 || input->dy > 32767 ||
        input->wheel < -127 || input->wheel > 127 || input->buttons > 0x1F) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
        return;
    }
    report.reportId = 1;
    report.buttons = (UCHAR)input->buttons;
    report.x = (SHORT)input->dx;
    report.y = (SHORT)input->dy;
    report.wheel = (CHAR)input->wheel;
    packet.reportBuffer = (PUCHAR)&report;
    packet.reportBufferLen = sizeof(report);
    packet.reportId = report.reportId;
    /* No ReadyForNextRead callback: VHF copies/buffers each report before
       returning. The sequential queue preserves down/up edges, even when a
       click arrives before the HID consumer issues its next read. */
    WdfWaitLockAcquire(ctx->submitLock, NULL);
    status = ctx->vhf ? VhfReadReportSubmit(ctx->vhf, &packet) : STATUS_DEVICE_NOT_READY;
    WdfWaitLockRelease(ctx->submitLock);
    WdfRequestComplete(request, status);
}

VOID AsbVhidEvtDeviceCleanup(WDFOBJECT object)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext((WDFDEVICE)object);
    if (!ctx->submitLock) return;
    /* Serialize deletion against the synchronous IOCTL submit path. */
    WdfWaitLockAcquire(ctx->submitLock, NULL);
    if (ctx->vhf) {
        VhfDelete(ctx->vhf, TRUE);
        ctx->vhf = NULL;
    }
    WdfWaitLockRelease(ctx->submitLock);
}

NTSTATUS AsbVhidEvtDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    DECLARE_CONST_UNICODE_STRING(deviceName, L"\\Device\\AppSandboxVHID");
    DECLARE_CONST_UNICODE_STRING(symbolicName, L"\\DosDevices\\AppSandboxVHID");
    DECLARE_CONST_UNICODE_STRING(sddl,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    VHF_CONFIG vhfConfig;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(driver);

    WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);
    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(deviceInit, &sddl);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = AsbVhidEvtDeviceCleanup;
    /* VhfCreate and the synchronous form of VhfDelete require PASSIVE_LEVEL. */
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &symbolicName);
    if (!NT_SUCCESS(status)) return status;

    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->submitLock);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = AsbVhidEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    VHF_CONFIG_INIT(&vhfConfig, WdfDeviceWdmGetDeviceObject(device),
                    (USHORT)sizeof(g_mouseDescriptor),
                    (PUCHAR)g_mouseDescriptor);
    vhfConfig.VhfClientContext = ctx;
    vhfConfig.VendorID = 0xF055;
    vhfConfig.ProductID = 0xA5B0;
    vhfConfig.VersionNumber = 0x0001;
    status = VhfCreate(&vhfConfig, &ctx->vhf);
    if (!NT_SUCCESS(status)) return status;
    status = VhfStart(ctx->vhf);
    if (!NT_SUCCESS(status)) {
        VhfDelete(ctx->vhf, TRUE);
        ctx->vhf = NULL;
    }
    return status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AsbVhidEvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}
