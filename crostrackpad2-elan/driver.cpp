#include "internal.h"
#include "driver.h"
#include "device.h"
#include "ntstrsafe.h"
#include "hiddevice.h"
#include "input.h"

static ULONG ElanPrintDebugLevel = 100;
static ULONG ElanPrintDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

void TrackpadRawInput(PDEVICE_CONTEXT pDevice, struct csgesture_softc *sc, uint8_t report[ETP_MAX_REPORT_LEN], int tickinc);
void ElanTimerFunc(_In_ WDFTIMER hTimer);

#define NT_DEVICE_NAME      L"\\Device\\ELANTP"
#define DOS_DEVICE_NAME     L"\\DosDevices\\ELANTP"

#define MAX_FINGERS 5

//#include "driver.tmh"

NTSTATUS
#pragma prefast(suppress:__WARNING_DRIVER_FUNCTION_TYPE, "thanks, i know this already")
DriverEntry(
_In_ PDRIVER_OBJECT  DriverObject,
_In_ PUNICODE_STRING RegistryPath
)
{
	/*WDF_DRIVER_CONFIG_INIT(&driverConfig, OnDeviceAdd);*/
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      driverConfig;
	WDF_OBJECT_ATTRIBUTES  driverAttributes;
	WDFDRIVER fxDriver;

	WPP_INIT_TRACING(DriverObject, RegistryPath);

	FuncEntry(TRACE_FLAG_WDFLOADING);

	WDF_DRIVER_CONFIG_INIT(&driverConfig, OnDeviceAdd);
	driverConfig.DriverPoolTag = SPBT_POOL_TAG;

	WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
	driverAttributes.EvtCleanupCallback = OnDriverCleanup;

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&driverAttributes,
		&driverConfig,
		&fxDriver
		);

	if (!NT_SUCCESS(status))
	{
		Trace(
			TRACE_LEVEL_ERROR,
			TRACE_FLAG_WDFLOADING,
			"Error creating WDF driver object - %!STATUS!",
			status);
		goto exit;
	}

	Trace(
		TRACE_LEVEL_VERBOSE,
		TRACE_FLAG_WDFLOADING,
		"Created WDF driver object");

exit:

	FuncExit(TRACE_FLAG_WDFLOADING);

	return status;
}

VOID
OnDriverCleanup(
_In_ WDFOBJECT Object
)
{
	FuncEntry(TRACE_FLAG_WDFLOADING);

	UNREFERENCED_PARAMETER(Object);

	WPP_CLEANUP(nullptr);

	FuncExit(TRACE_FLAG_WDFLOADING);
}

NTSTATUS
OnDeviceAdd(
_In_    WDFDRIVER       FxDriver,
_Inout_ PWDFDEVICE_INIT FxDeviceInit
)
/*++

Routine Description:

This routine creates the device object for an SPB
controller and the device's child objects.

Arguments:

FxDriver - the WDF driver object handle
FxDeviceInit - information about the PDO that we are loading on

Return Value:

Status

--*/
{
	FuncEntry(TRACE_FLAG_WDFLOADING);

	PDEVICE_CONTEXT pDevice;
	WDFDEVICE fxDevice;
	WDF_INTERRUPT_CONFIG interruptConfig;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(FxDriver);

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//
	WdfFdoInitSetFilter(FxDeviceInit);

	//
	// Setup PNP/Power callbacks.
	//

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(FxDeviceInit, &pnpCallbacks);
	}

	//
	// Set request attributes.
	//

	{
		WDF_OBJECT_ATTRIBUTES attributes;
		WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
			&attributes,
			REQUEST_CONTEXT);

		WdfDeviceInitSetRequestAttributes(FxDeviceInit, &attributes);
	}

	//
	// Create the device.
	//

	{
		WDF_OBJECT_ATTRIBUTES deviceAttributes;
		WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

		status = WdfDeviceCreate(
			&FxDeviceInit,
			&deviceAttributes,
			&fxDevice);

		if (!NT_SUCCESS(status))
		{
			ElanPrint(
				TRACE_LEVEL_ERROR,
				TRACE_FLAG_WDFLOADING,
				"Error creating WDFDEVICE - %!STATUS!",
				status);

			goto exit;
		}

		pDevice = GetDeviceContext(fxDevice);
		NT_ASSERT(pDevice != nullptr);

		pDevice->FxDevice = fxDevice;
	}

	//
	// Ensure device is disable-able
	//

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(pDevice->FxDevice, &deviceState);
	}

	//
	// Create queues to handle IO
	//

	{
		WDF_IO_QUEUE_CONFIG queueConfig;
		WDFQUEUE queue;

		//
		// Top-level queue
		//

		WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
			&queueConfig,
			WdfIoQueueDispatchParallel);

		queueConfig.EvtIoDefault = OnTopLevelIoDefault;
		queueConfig.PowerManaged = WdfFalse;

		status = WdfIoQueueCreate(
			pDevice->FxDevice,
			&queueConfig,
			WDF_NO_OBJECT_ATTRIBUTES,
			&queue
			);

		if (!NT_SUCCESS(status))
		{
			ElanPrint(
				TRACE_LEVEL_ERROR,
				TRACE_FLAG_WDFLOADING,
				"Error creating top-level IO queue - %!STATUS!",
				status);

			goto exit;
		}

		//
		// Sequential SPB queue
		//

		WDF_IO_QUEUE_CONFIG_INIT(
			&queueConfig,
			WdfIoQueueDispatchSequential);

		queueConfig.EvtIoInternalDeviceControl = OnIoDeviceControl;
		queueConfig.PowerManaged = WdfFalse;

		status = WdfIoQueueCreate(
			fxDevice,
			&queueConfig,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDevice->SpbQueue
			);

		if (!NT_SUCCESS(status))
		{
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
				"WdfIoQueueCreate failed 0x%x\n", status);

			goto exit;
		}
	}

	WDF_IO_QUEUE_CONFIG           queueConfig;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(pDevice->FxDevice,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&pDevice->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "Queue 2!\n");
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		fxDevice,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&pDevice->Interrupt);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		goto exit;
	}

	WDF_TIMER_CONFIG              timerConfig;
	WDFTIMER                      hTimer;
	WDF_OBJECT_ATTRIBUTES         attributes;

	WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, ElanTimerFunc, 10);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = fxDevice;
	status = WdfTimerCreate(&timerConfig, &attributes, &hTimer);
	pDevice->Timer = hTimer;
	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP, "(%!FUNC!) WdfTimerCreate failed status:%!STATUS!\n", status);
		return status;
	}

	ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
		"Success! 0x%x\n", status);

	pDevice->DeviceMode = DEVICE_MODE_MOUSE;

exit:

	FuncExit(TRACE_FLAG_WDFLOADING);

	return status;
}


BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID){
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PDEVICE_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt){
		return true;
	}

	//IRQ's don't work with the elan trackpad so we are resorting to polling instead.
	return true;
}

VOID
ElanReadWriteWorkItem(
IN WDFWORKITEM  WorkItem
)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	PDEVICE_CONTEXT pDevice = GetDeviceContext(Device);
	if (!pDevice->ConnectInterrupt)
		return;

	uint8_t report[ETP_MAX_REPORT_LEN];
	SpbReadDataSynchronously(&pDevice->I2CContext, 0, &report, sizeof(report));

	if (report[0] != 0xff){
		for (int i = 0; i < ETP_MAX_REPORT_LEN; i++)
			pDevice->lastreport[i] = report[i];
	}

	uint8_t *report2 = pDevice->lastreport;

	csgesture_softc sc = pDevice->sc;
	TrackpadRawInput(pDevice, &sc, report2, 1);
	pDevice->sc = sc;
	WdfObjectDelete(WorkItem);
}

void ElanTimerFunc(_In_ WDFTIMER hTimer){
	WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(hTimer);
	PDEVICE_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return;

	PDEVICE_CONTEXT context;
	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_WORKITEM_CONFIG workitemConfig;
	WDFWORKITEM hWorkItem;

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
	attributes.ParentObject = Device;
	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, ElanReadWriteWorkItem);

	WdfWorkItemCreate(&workitemConfig,
		&attributes,
		&hWorkItem);

	WdfWorkItemEnqueue(hWorkItem);

	return;
}

static int distancesq(int delta_x, int delta_y){
	return (delta_x * delta_x) + (delta_y*delta_y);
}

_CYAPA_RELATIVE_MOUSE_REPORT lastreport;

static void update_relative_mouse(PDEVICE_CONTEXT pDevice, BYTE button,
	BYTE x, BYTE y, BYTE wheelPosition, BYTE wheelHPosition){
	_CYAPA_RELATIVE_MOUSE_REPORT report;
	report.ReportID = REPORTID_RELATIVE_MOUSE;
	report.Button = button;
	report.XValue = x;
	report.YValue = y;
	report.WheelPosition = wheelPosition;
	report.HWheelPosition = wheelHPosition;
	if (report.Button == lastreport.Button &&
		report.XValue == lastreport.XValue &&
		report.YValue == lastreport.YValue &&
		report.WheelPosition == lastreport.WheelPosition &&
		report.HWheelPosition == lastreport.HWheelPosition)
		return;
	lastreport = report;

	size_t bytesWritten;
	ElanProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
}

static void update_keyboard(PDEVICE_CONTEXT pDevice, BYTE shiftKeys, BYTE keyCodes[KBD_KEY_CODES]){
	_CYAPA_KEYBOARD_REPORT report;
	report.ReportID = REPORTID_KEYBOARD;
	report.ShiftKeyFlags = shiftKeys;
	for (int i = 0; i < KBD_KEY_CODES; i++){
		report.KeyCodes[i] = keyCodes[i];
	}

	size_t bytesWritten;
	ElanProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
}

bool ProcessMove(csgesture_softc *sc, int abovethreshold, int iToUse[3]) {
	if (abovethreshold == 1 || sc->panningActive) {
		int i = iToUse[0];
		if (!sc->panningActive && sc->tick[i] < 5)
			return false;

		if (sc->panningActive && i == -1)
			i = sc->idForPanning;

		int delta_x = sc->x[i] - sc->lastx[i];
		int delta_y = sc->y[i] - sc->lasty[i];

		if (abs(delta_x) > 75 || abs(delta_y) > 75) {
			delta_x = 0;
			delta_y = 0;
		}

		for (int j = 0;j < MAX_FINGERS;j++) {
			if (j != i) {
				if (sc->blacklistedids[j] != 1) {
					if (sc->y[j] > sc->y[i]) {
						if (sc->truetick[j] > sc->truetick[i] + 15) {
							sc->blacklistedids[j] = 1;
						}
					}
				}
			}
		}

		sc->dx = delta_x;
		sc->dy = delta_y;

		sc->panningActive = true;
		sc->idForPanning = i;
		return true;
	}
	return false;
}

int CalcScrollValue(int rawValue, int ticks) {
	int actionThreshold = 3;
	int invalidThreshold = 120;

	int absValue = abs(rawValue);
	int speed = absValue / max(ticks, 1);
	int step = speed > 11 ? 3 : (speed > 7 ? 4 : (speed > 4 ? 6 : 7));
	if (absValue > invalidThreshold || absValue < actionThreshold) {
		return 0;
	} else {
		return (rawValue / absValue) * ((absValue > actionThreshold && absValue < 2 * step ? 0 : 1) + (absValue - actionThreshold) / step);
	}
}

bool ProcessScroll(csgesture_softc *sc, int abovethreshold, int iToUse[3]) {
	sc->scrollx = 0;
	sc->scrolly = 0;
	if (abovethreshold == 2 || sc->scrollingActive) {
		int i1 = iToUse[0];
		int i2 = iToUse[1];
		if (sc->scrollingActive) {
			if (i1 == -1) {
				if (i2 != sc->idsForScrolling[0])
					i1 = sc->idsForScrolling[0];
				else
					i1 = sc->idsForScrolling[1];
			}
			if (i2 == -1) {
				if (i1 != sc->idsForScrolling[0])
					i2 = sc->idsForScrolling[0];
				else
					i2 = sc->idsForScrolling[1];
			}
		}

		int delta_x1 = sc->x[i1] - sc->lastx[i1];
		int delta_y1 = sc->y[i1] - sc->lasty[i1];

		int delta_x2 = sc->x[i2] - sc->lastx[i2];
		int delta_y2 = sc->y[i2] - sc->lasty[i2];

		if ((abs(delta_y1) + abs(delta_y2)) > (abs(delta_x1) + abs(delta_x2))) {
			int avgy = (delta_y1 + delta_y2) / 2;
			sc->scrolly = -avgy;
		}
		else {
			int avgx = (delta_x1 + delta_x2) / 2;
			sc->scrollx = -avgx;
		}

		int ticks = (sc->tick[i1] + sc->tick[i2]) / 2;
		sc->scrolly = CalcScrollValue(sc->scrolly, ticks);
		sc->scrollx = CalcScrollValue(sc->scrollx, ticks);

		int fngrcount = 0;
		int totfingers = 0;
		for (int i = 0; i < MAX_FINGERS; i++) {
			if (sc->x[i] != -1) {
				totfingers++;
				if (i == i1 || i == i2)
					fngrcount++;
			}
		}

		if (fngrcount == 2)
			sc->ticksSinceScrolling = 0;
		else
			sc->ticksSinceScrolling++;
		if (fngrcount == 2 || sc->ticksSinceScrolling <= 5) {
			sc->scrollingActive = true;
			if (abovethreshold == 2) {
				sc->idsForScrolling[0] = iToUse[0];
				sc->idsForScrolling[1] = iToUse[1];
			}
		}
		else {
			sc->scrollingActive = false;
			sc->idsForScrolling[0] = -1;
			sc->idsForScrolling[1] = -1;
		}
		return true;
	}
	return false;
}

bool ProcessThreeFingerSwipe(PDEVICE_CONTEXT pDevice, csgesture_softc *sc, int abovethreshold, int iToUse[3]) {
	if (abovethreshold == 3 || abovethreshold == 4) {
		int i1 = iToUse[0];
		int delta_x1 = sc->x[i1] - sc->lastx[i1];
		int delta_y1 = sc->y[i1] - sc->lasty[i1];

		int i2 = iToUse[1];
		int delta_x2 = sc->x[i2] - sc->lastx[i2];
		int delta_y2 = sc->y[i2] - sc->lasty[i2];

		int i3 = iToUse[2];
		int delta_x3 = sc->x[i3] - sc->lastx[i3];
		int delta_y3 = sc->y[i3] - sc->lasty[i3];

		int avgx = (delta_x1 + delta_x2 + delta_x3) / 3;
		int avgy = (delta_y1 + delta_y2 + delta_y3) / 3;

		sc->multitaskingx += avgx;
		sc->multitaskingy += avgy;
		sc->multitaskinggesturetick++;

		if (sc->multitaskinggesturetick > 5 && !sc->multitaskingdone) {
			if ((abs(delta_y1) + abs(delta_y2) + abs(delta_y3)) > (abs(delta_x1) + abs(delta_x2) + abs(delta_x3))) {
				if (abs(sc->multitaskingy) > 50) {
					BYTE shiftKeys = KBD_LGUI_BIT;
					BYTE keyCodes[KBD_KEY_CODES] = { 0, 0, 0, 0, 0, 0 };
					if (sc->multitaskingy < 0)
						keyCodes[0] = 0x2B;
					else
						keyCodes[0] = 0x07;
					update_keyboard(pDevice, shiftKeys, keyCodes);
					shiftKeys = 0;
					keyCodes[0] = 0x0;
					update_keyboard(pDevice, shiftKeys, keyCodes);
					sc->multitaskingx = 0;
					sc->multitaskingy = 0;
					sc->multitaskingdone = true;
				}
			}
			else {
				if (abs(sc->multitaskingx) > 50) {
					BYTE shiftKeys = KBD_LGUI_BIT | KBD_LCONTROL_BIT;
					BYTE keyCodes[KBD_KEY_CODES] = { 0, 0, 0, 0, 0, 0 };
					if (sc->multitaskingx > 0)
						keyCodes[0] = 0x50;
					else
						keyCodes[0] = 0x4F;
					update_keyboard(pDevice, shiftKeys, keyCodes);
					shiftKeys = 0;
					keyCodes[0] = 0x0;
					update_keyboard(pDevice, shiftKeys, keyCodes);
					sc->multitaskingx = 0;
					sc->multitaskingy = 0;
					sc->multitaskingdone = true;
				}
			}
		}
		else if (sc->multitaskinggesturetick > 25) {
			sc->multitaskingx = 0;
			sc->multitaskingy = 0;
			sc->multitaskinggesturetick = 0;
			sc->multitaskingdone = false;
		}
		return true;
	}
	else {
		sc->multitaskingx = 0;
		sc->multitaskingy = 0;
		sc->multitaskinggesturetick = 0;
		sc->multitaskingdone = false;
		return false;
	}
}

void TapToClickOrDrag(PDEVICE_CONTEXT pDevice, csgesture_softc *sc, int button) {
	sc->tickssinceclick++;
	if (sc->mouseDownDueToTap && sc->idForMouseDown == -1) {
		if (sc->tickssinceclick > 10) {
			sc->mouseDownDueToTap = false;
			sc->mousedown = false;
			sc->buttonmask = 0;
			//Tap Drag Timed out
		}
		return;
	}
	if (sc->mousedown) {
		sc->tickssinceclick = 0;
		return;
	}
	if (button == 0)
		return;
	int buttonmask = 0;

	switch (button) {
	case 1:
		buttonmask = MOUSE_BUTTON_1;
		break;
	case 2:
		buttonmask = MOUSE_BUTTON_2;
		break;
	case 3:
		buttonmask = MOUSE_BUTTON_3;
		break;
	}
	if (buttonmask != 0 && sc->tickssinceclick > 10 && sc->ticksincelastrelease == 0) {
		sc->idForMouseDown = -1;
		sc->mouseDownDueToTap = true;
		sc->buttonmask = buttonmask;
		sc->mousebutton = button;
		sc->mousedown = true;
		sc->tickssinceclick = 0;
	}
}

void ClearTapDrag(PDEVICE_CONTEXT pDevice, csgesture_softc *sc, int i) {
	if (i == sc->idForMouseDown && sc->mouseDownDueToTap == true) {
		if (sc->tick[i] < 10) {
			//Double Tap
			update_relative_mouse(pDevice, 0, 0, 0, 0, 0);
			update_relative_mouse(pDevice, sc->buttonmask, 0, 0, 0, 0);
		}
		sc->mouseDownDueToTap = false;
		sc->mousedown = false;
		sc->buttonmask = 0;
		sc->idForMouseDown = -1;
		//Clear Tap Drag
	}
}

void ProcessGesture(PDEVICE_CONTEXT pDevice, csgesture_softc *sc) {
#pragma mark reset inputs
	sc->dx = 0;
	sc->dy = 0;

#pragma mark process touch thresholds
	int abovethreshold = 0;
	int recentlyadded = 0;
	int iToUse[3] = { -1,-1,-1 };
	int a = 0;

	int nfingers = 0;
	for (int i = 0;i < MAX_FINGERS;i++) {
		if (sc->x[i] != -1)
			nfingers++;
	}

	int recentTicksThreshold = 20;
	int speedThreshold = 2;

	for (int i = 0;i < MAX_FINGERS;i++) {
		if (sc->truetick[i] < recentTicksThreshold && sc->truetick[i] != 0)
			recentlyadded++;
		if (sc->tick[i] == 0)
			continue;
		if (sc->blacklistedids[i] == 1)
			continue;
		if (distancesq(sc->flextotalx[i], sc->flextotaly[i]) / (sc->tick[i] * sc->tick[i]) > speedThreshold) {
			abovethreshold++;
			iToUse[a] = i;
			a++;
		}
	}

#pragma mark process different gestures
	bool handled = false;
	if (!handled)
		handled = ProcessThreeFingerSwipe(pDevice, sc, abovethreshold, iToUse);
	if (!handled)
		handled = ProcessScroll(sc, abovethreshold, iToUse);
	if (!handled)
		handled = ProcessMove(sc, abovethreshold, iToUse);

#pragma mark process clickpad press state
	int buttonmask = 0;

	sc->mousebutton = recentlyadded;
	if (sc->mousebutton == 0)
		sc->mousebutton = abovethreshold;

	if (sc->mousebutton == 0) {
		if (sc->panningActive)
			sc->mousebutton = 1;
		else
			sc->mousebutton = nfingers;
		if (sc->mousebutton == 0)
			sc->mousebutton = 1;
	}
	if (sc->mousebutton > 3)
		sc->mousebutton = 3;

	if (!sc->mouseDownDueToTap) {
		if (sc->buttondown && !sc->mousedown) {
			sc->mousedown = true;
			sc->tickssinceclick = 0;

			switch (sc->mousebutton) {
			case 1:
				buttonmask = MOUSE_BUTTON_1;
				break;
			case 2:
				buttonmask = MOUSE_BUTTON_2;
				break;
			case 3:
				buttonmask = MOUSE_BUTTON_3;
				break;
			}
			sc->buttonmask = buttonmask;
		}
		else if (sc->mousedown && !sc->buttondown) {
			sc->mousedown = false;
			sc->mousebutton = 0;
			sc->buttonmask = 0;
		}
	}

#pragma mark shift to last
	int releasedfingers = 0;

	for (int i = 0;i < MAX_FINGERS;i++) {
		if (sc->x[i] != -1) {
			if (sc->lastx[i] == -1) {
				if (sc->ticksincelastrelease < 10 && sc->mouseDownDueToTap && sc->idForMouseDown == -1) {
					sc->idForMouseDown = i; //Associate Tap Drag
				}
			}
			sc->truetick[i]++;
			if (sc->tick[i] < 10) {
				if (sc->lastx[i] != -1) {
					sc->totalx[i] += abs(sc->x[i] - sc->lastx[i]);
					sc->totaly[i] += abs(sc->y[i] - sc->lasty[i]);
					sc->totalp[i] += sc->p[i];

					sc->flextotalx[i] = sc->totalx[i];
					sc->flextotaly[i] = sc->totaly[i];

					int j = sc->tick[i];
					sc->xhistory[i][j] = abs(sc->x[i] - sc->lastx[i]);
					sc->yhistory[i][j] = abs(sc->y[i] - sc->lasty[i]);
				}
				sc->tick[i]++;
			}
			else if (sc->lastx[i] != -1) {
				int absx = abs(sc->x[i] - sc->lastx[i]);
				int absy = abs(sc->y[i] - sc->lasty[i]);

				int newtotalx = sc->flextotalx[i] - sc->xhistory[i][0] + absx;
				int newtotaly = sc->flextotaly[i] - sc->yhistory[i][0] + absy;

				sc->totalx[i] += absx;
				sc->totaly[i] += absy;

				sc->flextotalx[i] -= sc->xhistory[i][0];
				sc->flextotaly[i] -= sc->yhistory[i][0];
				for (int j = 1;j < 10;j++) {
					sc->xhistory[i][j - 1] = sc->xhistory[i][j];
					sc->yhistory[i][j - 1] = sc->yhistory[i][j];
				}
				sc->flextotalx[i] += absx;
				sc->flextotaly[i] += absy;

				int j = 9;
				sc->xhistory[i][j] = absx;
				sc->yhistory[i][j] = absy;
			}
		}
		if (sc->x[i] == -1) {
			ClearTapDrag(pDevice, sc, i);
			if (sc->lastx[i] != -1)
				sc->ticksincelastrelease = -1;
			for (int j = 0;j < 10;j++) {
				sc->xhistory[i][j] = 0;
				sc->yhistory[i][j] = 0;
			}
			if (sc->tick[i] < 10 && sc->tick[i] != 0) {
				int avgp = sc->totalp[i] / sc->tick[i];
				if (avgp > 7)
					releasedfingers++;
			}
			sc->totalx[i] = 0;
			sc->totaly[i] = 0;
			sc->totalp[i] = 0;
			sc->tick[i] = 0;
			sc->truetick[i] = 0;

			sc->blacklistedids[i] = 0;

			if (sc->idForPanning == i) {
				sc->panningActive = false;
				sc->idForPanning = -1;
			}
		}
		sc->lastx[i] = sc->x[i];
		sc->lasty[i] = sc->y[i];
		sc->lastp[i] = sc->p[i];
	}
	sc->ticksincelastrelease++;

#pragma mark process tap to click
	TapToClickOrDrag(pDevice, sc, releasedfingers);

#pragma mark send to system
	update_relative_mouse(pDevice, sc->buttonmask, sc->dx, sc->dy, sc->scrolly, sc->scrollx);
}

void TrackpadRawInput(PDEVICE_CONTEXT pDevice, struct csgesture_softc *sc, uint8_t report[ETP_MAX_REPORT_LEN], int tickinc){
	if (report[0] == 0xff){
		return;
	}

	uint8_t *finger_data = &report[ETP_FINGER_DATA_OFFSET];
	int i;
	uint8_t tp_info = report[ETP_TOUCH_INFO_OFFSET];
	uint8_t hover_info = report[ETP_HOVER_INFO_OFFSET];
	bool contact_valid, hover_event;

	int nfingers = 0;

	for (int i = 0;i < 5; i++) {
		sc->x[i] = -1;
		sc->y[i] = -1;
		sc->p[i] = -1;
	}

	hover_event = hover_info & 0x40;
	for (i = 0; i < ETP_MAX_FINGERS; i++) {
		contact_valid = tp_info & (1U << (3 + i));
		unsigned int pos_x, pos_y;
		unsigned int pressure, mk_x, mk_y;
		unsigned int area_x, area_y, major, minor;
		unsigned int scaled_pressure;

		if (contact_valid) {
			pos_x = ((finger_data[0] & 0xf0) << 4) |
				finger_data[1];
			pos_y = ((finger_data[0] & 0x0f) << 8) |
				finger_data[2];

			mk_x = (finger_data[3] & 0x0f);
			mk_y = (finger_data[3] >> 4);
			pressure = finger_data[4];

			//map to cypress coordinates
			//pos_y = 1500 - pos_y;
			pos_y = pDevice->max_y - pos_y;
			pos_x *= 2;
			pos_x /= 7;
			pos_y *= 2;
			pos_y /= 7;


			/*
			* To avoid treating large finger as palm, let's reduce the
			* width x and y per trace.
			*/
			area_x = mk_x;
			area_y = mk_y;

			major = max(area_x, area_y);
			minor = min(area_x, area_y);

			scaled_pressure = pressure;

			if (scaled_pressure > ETP_MAX_PRESSURE)
				scaled_pressure = ETP_MAX_PRESSURE;
			sc->x[i] = pos_x;
			sc->y[i] = pos_y;
			sc->p[i] = scaled_pressure;
		}
		else {
		}

		if (contact_valid){
			finger_data += ETP_FINGER_DATA_LEN;
			nfingers++;
		}
		}
	sc->buttondown = (tp_info & 0x01);

	ProcessGesture(pDevice, sc);
}