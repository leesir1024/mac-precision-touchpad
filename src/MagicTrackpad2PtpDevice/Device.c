// Device.c: Device handling events for driver.

#include "driver.h"
#include "device.tmh"

static const struct BCM5974_CONFIG*
MagicTrackpad2GetConfig(
	_In_ USB_DEVICE_DESCRIPTOR deviceInfo
)
{
	u16 id = deviceInfo.idProduct;
	const struct BCM5974_CONFIG *cfg;

	for (cfg = Bcm5974ConfigTable; cfg->ansi; ++cfg)
	{
		if (cfg->ansi == id || cfg->iso == id || cfg->jis == id)
		{
			return cfg;
		}
	}

	return NULL;
}

NTSTATUS
MagicTrackpad2PtpDeviceCreateDevice(
	_In_    WDFDRIVER       Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
{
	WDF_PNPPOWER_EVENT_CALLBACKS		pnpPowerCallbacks;
	WDF_DEVICE_PNP_CAPABILITIES         pnpCaps;
	WDF_OBJECT_ATTRIBUTES				deviceAttributes;
	PDEVICE_CONTEXT						deviceContext;
	WDFDEVICE							device;
	NTSTATUS							status;

	UNREFERENCED_PARAMETER(Driver);
	PAGED_CODE();

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	// Initialize Power Callback
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

	// Initialize PNP power event callbacks
	pnpPowerCallbacks.EvtDevicePrepareHardware = MagicTrackpad2PtpDeviceEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry = MagicTrackpad2PtpDeviceEvtDeviceD0Entry;
	pnpPowerCallbacks.EvtDeviceD0Exit = MagicTrackpad2PtpDeviceEvtDeviceD0Exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	// Create WDF device object
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"WdfDeviceCreate failed with Status code %!STATUS!\n", status);
		return status;
	}

	//
	// Get a pointer to the device context structure that we just associated
	// with the device object. We define this structure in the device.h
	// header file. DeviceGetContext is an inline function generated by
	// using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
	// This function will do the type checking and return the device context.
	// If you pass a wrong object handle it will return NULL and assert if
	// run under framework verifier mode.
	//
	deviceContext = DeviceGetContext(device);

	//
	// Tell the framework to set the SurpriseRemovalOK in the DeviceCaps so
	// that you don't get the popup in usermode 
	// when you surprise remove the device.
	//
	WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
	pnpCaps.SurpriseRemovalOK = WdfTrue;
	WdfDeviceSetPnpCapabilities(device, &pnpCaps);

	//
	// Create a device interface so that applications can find and talk
	// to us.
	//
	status = WdfDeviceCreateDeviceInterface(
		device,
		&GUID_DEVINTERFACE_MagicTrackpad2PtpDevice,
		NULL // ReferenceString
	);

	if (NT_SUCCESS(status)) {
		//
		// Initialize the I/O Package and any Queues
		//
		status = MagicTrackpad2PtpDeviceQueueInitialize(device);
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

	return status;
}

NTSTATUS
MagicTrackpad2PtpDeviceEvtDevicePrepareHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourceList,
	_In_ WDFCMRESLIST ResourceListTranslated
)
{
	NTSTATUS								status;
	PDEVICE_CONTEXT							pDeviceContext;
	ULONG									waitWakeEnable;
	WDF_USB_DEVICE_INFORMATION				deviceInfo;

	waitWakeEnable = FALSE;

	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);
	PAGED_CODE();

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

	status = STATUS_SUCCESS;
	pDeviceContext = DeviceGetContext(Device);

	if (pDeviceContext->UsbDevice == NULL) {
		status = WdfUsbTargetDeviceCreate(Device,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDeviceContext->UsbDevice);
		if (!NT_SUCCESS(status)) {
			TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
				"WdfUsbTargetDeviceCreate failed with Status code %!STATUS!\n", status);
			return status;
		}
	}

	// Retrieve device information
	WdfUsbTargetDeviceGetDeviceDescriptor(pDeviceContext->UsbDevice, &pDeviceContext->DeviceDescriptor);
	if (NT_SUCCESS(status)) {
		// Get correct configuration from conf store
		pDeviceContext->DeviceInfo = MagicTrackpad2GetConfig(pDeviceContext->DeviceDescriptor);
		if (pDeviceContext->DeviceInfo == NULL) {
			TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "MagicTrackpad2GetConfig failed 0x%x", status);
			return status;
		}
	}

	//
	// Retrieve USBD version information, port driver capabilites and device
	// capabilites such as speed, power, etc.
	//
	WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
	status = WdfUsbTargetDeviceRetrieveInformation(pDeviceContext->UsbDevice, &deviceInfo);

	if (NT_SUCCESS(status))
	{
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "IsDeviceHighSpeed: %s\n",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"IsDeviceSelfPowered: %s\n",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

		waitWakeEnable = deviceInfo.Traits &
			WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"IsDeviceRemoteWakeable: %s\n",
			waitWakeEnable ? "TRUE" : "FALSE");

		//
		// Save these for use later.
		//
		pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
	}
	else
	{
		pDeviceContext->UsbDeviceTraits = 0;
	}

	// Select interface to use
	status = SelectInterruptInterface(Device);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "SelectInterruptInterface failed 0x%x\n", status);
		return status;
	}

	//
	// Enable wait-wake and idle timeout if the device supports it
	//
	if (waitWakeEnable) {
		status = MagicTrackpad2PtpDeviceSetPowerPolicy(Device);
		if (!NT_SUCCESS(status)) {
			TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "MagicTrackpad2PtpDeviceSetPowerPolicy failed  %!STATUS!\n", status);
			return status;
		}
	}

	// Set up interrupt
	status = MagicTrackpad2PtpDeviceConfigContReaderForInterruptEndPoint(pDeviceContext);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "MagicTrackpad2PtpDeviceConfigContReaderForInterruptEndPoint failed 0x%x\n", status);
		return status;
	}

	// Set up wellspring mode
	// status = MagicTrackpad2PtpDeviceSetWellspringMode(pDeviceContext, TRUE);
	// if (!NT_SUCCESS(status)) {
	// 	TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "MagicTrackpad2PtpDeviceSetWellspringMode failed 0x%x\n", status);
	//	return status;
	// }

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MagicTrackpad2PtpDeviceSetWellspringMode(
	_In_ PDEVICE_CONTEXT DeviceContext,
	_In_ BOOL IsWellspringModeOn
)
{

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC!: entry");

	NTSTATUS						status;
	WDF_USB_CONTROL_SETUP_PACKET	setupPacket;

	WDF_MEMORY_DESCRIPTOR			memoryDescriptor;
	ULONG							cbTransferred;

	WDFMEMORY						bufHandle = NULL;
	unsigned char*					buffer;

	status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, PagedPool, 0, DeviceContext->DeviceInfo->um_size, &bufHandle, &buffer);
	if (!NT_SUCCESS(status)) {
		goto cleanup;
	}

	RtlZeroMemory(buffer, DeviceContext->DeviceInfo->um_size);

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memoryDescriptor, buffer, sizeof(DeviceContext->DeviceInfo->um_size));

	WDF_USB_CONTROL_SETUP_PACKET_INIT(&setupPacket, BmRequestDeviceToHost, BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
		(USHORT)DeviceContext->DeviceInfo->um_req_val, (USHORT)DeviceContext->DeviceInfo->um_req_idx);

	// Set stuffs right
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL, &setupPacket, &memoryDescriptor, &cbTransferred);
	if (!NT_SUCCESS(status) || cbTransferred != (ULONG)DeviceContext->DeviceInfo->um_size) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfUsbTargetDeviceSendControlTransferSynchronously (Read) failed 0x%x\n", status);
		goto cleanup;
	}

	// Apply the mode switch
	buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn ?
		(unsigned char)DeviceContext->DeviceInfo->um_switch_on : (unsigned char)DeviceContext->DeviceInfo->um_switch_off;

	// Write configuration
	WDF_USB_CONTROL_SETUP_PACKET_INIT(&setupPacket, BmRequestHostToDevice, BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
		(USHORT)DeviceContext->DeviceInfo->um_req_val, (USHORT)DeviceContext->DeviceInfo->um_req_idx);

	// Set stuffs right
	setupPacket.Packet.bm.Request.Type = BmRequestClass;
	status = WdfUsbTargetDeviceSendControlTransferSynchronously(DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL, &setupPacket, &memoryDescriptor, &cbTransferred);
	if (!NT_SUCCESS(status) /* || cbTransferred != (ULONG) DeviceContext->DeviceInfo->um_size */) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfUsbTargetDeviceSendControlTransferSynchronously (Write) failed 0x%x\n", status);
		goto cleanup;
	}

	// Set status
	DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC!: exit");

cleanup:
	bufHandle = NULL;
	return status;
}

// D0 Entry & Exit
NTSTATUS
MagicTrackpad2PtpDeviceEvtDeviceD0Entry(
	WDFDEVICE Device,
	WDF_POWER_DEVICE_STATE PreviousState
)
{
	PDEVICE_CONTEXT         pDeviceContext;
	NTSTATUS                status;
	BOOLEAN                 isTargetStarted;

	pDeviceContext = DeviceGetContext(Device);
	isTargetStarted = FALSE;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "-->MagicTrackpad2PtpDeviceEvtDeviceD0Entry - coming from %s\n",
		DbgDevicePowerString(PreviousState));

	//
	// Since continuous reader is configured for this interrupt-pipe, we must explicitly start
	// the I/O target to get the framework to post read requests.
	//
	status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Failed to start interrupt pipe %!STATUS!\n", status);
		goto End;
	}

	isTargetStarted = TRUE;

End:

	if (!NT_SUCCESS(status)) {
		//
		// Failure in D0Entry will lead to device being removed. So let us stop the continuous
		// reader in preparation for the ensuing remove.
		//
		if (isTargetStarted) {
			WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe), WdfIoTargetCancelSentIo);
		}
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "<--MagicTrackpad2PtpDeviceEvtDeviceD0Entry\n");

	return status;
}

NTSTATUS
MagicTrackpad2PtpDeviceEvtDeviceD0Exit(
	WDFDEVICE Device,
	WDF_POWER_DEVICE_STATE TargetState
)
{
	PDEVICE_CONTEXT         pDeviceContext;
	PAGED_CODE();

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
		"-->MagicTrackpad2PtpDeviceEvtDeviceD0Exit - moving to %s\n", DbgDevicePowerString(TargetState));

	pDeviceContext = DeviceGetContext(Device);
	WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe), WdfIoTargetCancelSentIo);
	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "<--MagicTrackpad2PtpDeviceEvtDeviceD0Exit\n");

	return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
MagicTrackpad2PtpDeviceSetPowerPolicy(
	_In_ WDFDEVICE Device
)
{
	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idleSettings;
	WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS wakeSettings;
	NTSTATUS    status = STATUS_SUCCESS;

	PAGED_CODE();

	//
	// Init the idle policy structure.
	//
	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idleSettings, IdleUsbSelectiveSuspend);
	idleSettings.IdleTimeout = 10000; // 10-sec

	status = WdfDeviceAssignS0IdleSettings(Device, &idleSettings);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceSetPowerPolicyS0IdlePolicy failed %x\n", status);
		return status;
	}

	//
	// Init wait-wake policy structure.
	//
	WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&wakeSettings);

	status = WdfDeviceAssignSxWakeSettings(Device, &wakeSettings);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "WdfDeviceAssignSxWakeSettings failed %x\n", status);
		return status;
	}

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
	_In_ WDFDEVICE Device
)
{
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
	NTSTATUS                            status = STATUS_SUCCESS;
	PDEVICE_CONTEXT                     pDeviceContext;
	WDFUSBPIPE                          pipe;
	WDF_USB_PIPE_INFORMATION            pipeInfo;
	UCHAR                               index;
	UCHAR                               numberConfiguredPipes;
	WDFUSBINTERFACE                     usbInterface;

	PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);

	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

	usbInterface = WdfUsbTargetDeviceGetInterface(pDeviceContext->UsbDevice, 0);

	if (NULL == usbInterface) {
		status = STATUS_UNSUCCESSFUL;
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"WdfUsbTargetDeviceGetInterface 0 failed %!STATUS! \n",
			status);
		return status;
	}

	configParams.Types.SingleInterface.ConfiguredUsbInterface = usbInterface;
	configParams.Types.SingleInterface.NumberConfiguredPipes = WdfUsbInterfaceGetNumConfiguredPipes(usbInterface);

	pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;

	numberConfiguredPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;

	//
	// Get pipe handles
	//
	for (index = 0; index < numberConfiguredPipes; index++) {

		WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

		pipe = WdfUsbInterfaceGetConfiguredPipe(
			pDeviceContext->UsbInterface,
			index, //PipeIndex,
			&pipeInfo
		);

		//
		// Tell the framework that it's okay to read less than
		// MaximumPacketSize
		//
		WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

		if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
			TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Interrupt Pipe is 0x%p\n", pipe);
			pDeviceContext->InterruptPipe = pipe;
			// One interrupt is enough
			break;
		}

	}

	//
	// If we didn't find interrupt pipe, fail the start.
	//
	if (!pDeviceContext->InterruptPipe) {
		status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Device is not configured properly %!STATUS!\n", status);

		return status;
	}

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
PCHAR
DbgDevicePowerString(
	_In_ WDF_POWER_DEVICE_STATE Type
)
{
	switch (Type)
	{
	case WdfPowerDeviceInvalid:
		return "WdfPowerDeviceInvalid";
	case WdfPowerDeviceD0:
		return "WdfPowerDeviceD0";
	case WdfPowerDeviceD1:
		return "WdfPowerDeviceD1";
	case WdfPowerDeviceD2:
		return "WdfPowerDeviceD2";
	case WdfPowerDeviceD3:
		return "WdfPowerDeviceD3";
	case WdfPowerDeviceD3Final:
		return "WdfPowerDeviceD3Final";
	case WdfPowerDevicePrepareForHibernation:
		return "WdfPowerDevicePrepareForHibernation";
	case WdfPowerDeviceMaximum:
		return "WdfPowerDeviceMaximum";
	default:
		return "UnKnown Device Power State";
	}
}