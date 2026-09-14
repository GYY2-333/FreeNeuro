/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: HID_PHYSIOAMP.CPP:  PhysioAmp GP-8 USB HID interface (signal mode)

  Provides interfacing to the PhysioAmp GP-8 custom USB HID device
  (VID 0x0EA9 / PID 0x0FA8). The device is enumerated via the Win32 SetupAPI,
  opened with CreateFile and read/written through the HID class driver.

  Because the device requires the host to send Output Reports for the handshake
  (0x80 / 0x8F / 0xF8), it cannot be driven through the Raw Input API (as the NIA
  is).

  The device is opened for overlapped I/O and every transfer is waited for with a
  timeout, so neither the handshake nor the shutdown can block the caller (the UI
  thread) forever, and a pending read can always be cancelled cleanly. The reader
  thread waits on its read completion event and on a stop event at the same time,
  which is what lets DisconnectPhysioAmp() guarantee that the thread has really
  finished before the device handle is closed. Closing that handle under a live
  thread is what previously let a stale reader keep running and keep feeding the
  object graph after a reconnect.

  Each 64 byte data frame carries 30 little-endian ADC codes plus a four byte
  header (frame number, type id, tick counter). In signal mode the six protocol
  channels are just consecutive slices of one single ADC time series, so the 30
  samples are re-ordered into a single channel time series: for every sample we
  store it in PACKET.buffer[0] and call process_packets(), which drives the EEG
  object's work() function - identical in spirit to how the NIA / Ganglion sources
  feed the signal processing chain.

  The device powers up in signal mode; 0xF8 toggles between signal and impedance
  mode and is answered by nothing at all. ConnectPhysioAmp() therefore asks the
  device for its mode (the handshake reply carries a mode stamp in bytes 4..5) and
  only toggles when the device is not already in the requested mode. Impedance
  frames carry one signal at six phase delays rather than a time series, so they
  are refused instead of being streamed into the EEG channel.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include <windows.h>
#include <setupapi.h>
#include <string.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include "brainBay.h"
#include "hid_physioamp.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

// how many reports to skip while waiting for a handshake reply: a device left
// streaming by a previous session may still have data frames in flight
#define PA_HANDSHAKE_DISCARDS   8

// total time budget for reading the handshake reply. It is shared between the
// discards above, so a silent device costs the caller this much and no more
#define PA_HANDSHAKE_TOTAL_MS   3000

// how long to wait for the reader thread to finish when disconnecting
#define PA_THREAD_EXIT_MS       5000

// --- module state ------------------------------------------------------------
static HANDLE  hPhysioAmp        = INVALID_HANDLE_VALUE;
static HANDLE  hReaderThread     = NULL;

static HANDLE  hStopEvent        = NULL;   // manual reset: asks the reader to stop
static HANDLE  hReadEvent        = NULL;   // completion event of the pending read
static HANDLE  hWriteEvent       = NULL;   // completion event of a pending write
static OVERLAPPED readOverlapped;
static OVERLAPPED writeOverlapped;

// report sizes as announced by the device, and how many leading bytes of a
// transfer buffer precede the payload (0 without Report IDs, 1 with)
static USHORT  inputReportLength  = PA_REPORT_LEN;
static USHORT  outputReportLength = PA_REPORT_LEN;
static int     reportIdBytes      = 0;

static int  requestedMode = PA_MODE_SIGNAL;  // mode the next connect asks for
static int  actualMode    = -1;              // mode negotiated, -1 while closed
static int  streamingMode = PA_MODE_SIGNAL;  // mode the reader decodes

static unsigned int oldTick = PA_FRAME_TICK_NONE;   // lost frame detection


/* The handshake request is a full 64 byte Output Report rather than a bare
   command byte: byte 0 carries 0x80, the remaining bytes are the parameter block
   the device is started with. The firmware only inspects byte 0, but sending the
   block unchanged avoids depending on that. */
static const unsigned char paHandshakeRequest[PA_REPORT_LEN] = {
	0x80, 0x20, 0x10, 0x01, 0x01, 0x00, 0x00, 0x66,
	0x0F, 0x67, 0x0F, 0x00, 0x10, 0xFA, 0x00, 0x01,
	0x6E, 0x36, 0x80, 0x07, 0xC6, 0xA6, 0x01, 0x14,
	0x08, 0x1A, 0x11, 0x80, 0x20, 0xA0, 0x01, 0x60,
	0x20, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};


/*----------------------------------------------------------------------------
  report_payload_len:  how many payload bytes fit into a report of the given
  size. This is below PA_REPORT_LEN only for a device whose report is shorter
  than the 64 bytes this protocol uses.
----------------------------------------------------------------------------*/
static unsigned int report_payload_len(USHORT reportLength)
{
	if (reportLength <= (USHORT) reportIdBytes) return 0;

	unsigned int len = (unsigned int)(reportLength - reportIdBytes);
	return (len > PA_REPORT_LEN) ? PA_REPORT_LEN : len;
}


/*----------------------------------------------------------------------------
  wait_for_io:  wait for an overlapped transfer to complete.

  Returns 1 when it completed, 0 when the stop event was signalled (only waited
  for when watchStop is set), -1 on timeout or device error. On timeout the
  transfer is cancelled and reaped, so the OVERLAPPED is free for reuse and the
  caller's buffer is no longer owned by the driver.
----------------------------------------------------------------------------*/
static int wait_for_io(OVERLAPPED *ol, HANDLE completion, DWORD timeoutMs, DWORD *transferred, BOOL watchStop)
{
	HANDLE waits[2];
	DWORD  count = 1;

	waits[0] = completion;
	if (watchStop && hStopEvent != NULL)
	{
		waits[1] = hStopEvent;
		count = 2;
	}

	DWORD w = WaitForMultipleObjects(count, waits, FALSE, timeoutMs);

	if (w == WAIT_OBJECT_0)
	{
		if (!GetOverlappedResult(hPhysioAmp, ol, transferred, FALSE)) return -1;
		return 1;
	}
	if (w == WAIT_OBJECT_0 + 1) return 0;          // stop requested

	// timeout: cancel the transfer, then wait for the cancellation to complete
	CancelIoEx(hPhysioAmp, ol);
	GetOverlappedResult(hPhysioAmp, ol, transferred, TRUE);
	return -1;
}


/*----------------------------------------------------------------------------
  close_device:  stop the reader thread and close the device handle.

  Returns FALSE when the reader thread is still alive after the timeout. In that
  case both handles are deliberately kept: releasing them while the thread still
  runs is exactly what would let a stale reader touch the object graph later.
----------------------------------------------------------------------------*/
static BOOL close_device(void)
{
	if (hReaderThread != NULL)
	{
		if (hStopEvent != NULL) SetEvent(hStopEvent);
		if (hPhysioAmp != INVALID_HANDLE_VALUE) CancelIoEx(hPhysioAmp, NULL);

		if (WaitForSingleObject(hReaderThread, PA_THREAD_EXIT_MS) != WAIT_OBJECT_0)
		{
			write_logfile("PhysioAmp: reader thread still running after %d ms, keeping handles",
			              PA_THREAD_EXIT_MS);
			return FALSE;
		}

		CloseHandle(hReaderThread);
		hReaderThread = NULL;
	}

	if (hPhysioAmp != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hPhysioAmp);
		hPhysioAmp = INVALID_HANDLE_VALUE;
	}

	actualMode = -1;
	oldTick    = PA_FRAME_TICK_NONE;
	return TRUE;
}


/*----------------------------------------------------------------------------
  find_and_open_physioamp:  enumerate all HID devices, open the one whose
  attributes match PHYSIOAMP_VID / PHYSIOAMP_PID.  Returns TRUE on success and
  fills hPhysioAmp plus the report lengths.
----------------------------------------------------------------------------*/
static BOOL find_and_open_physioamp(void)
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	// start from the documented layout (no Report ID) so the values are sane even
	// if the caps query below fails
	inputReportLength  = PA_REPORT_LEN;
	outputReportLength = PA_REPORT_LEN;
	reportIdBytes      = 0;

	HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, NULL, NULL,
	                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devInfo == INVALID_HANDLE_VALUE)
	{
		write_logfile("PhysioAmp: SetupDiGetClassDevs failed");
		return FALSE;
	}

	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	BOOL opened = FALSE;
	for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, idx, &ifData); idx++)
	{
		DWORD needed = 0;
		SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, NULL, 0, &needed, NULL);
		if (needed == 0) continue;

		PSP_DEVICE_INTERFACE_DETAIL_DATA detail =
		    (PSP_DEVICE_INTERFACE_DETAIL_DATA) malloc(needed);
		if (detail == NULL) continue;
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

		if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, needed, NULL, NULL))
		{
			// overlapped so that transfers can be waited for with a timeout and
			// cancelled - neither is possible on a synchronous handle
			HANDLE h = CreateFile(detail->DevicePath,
			                      GENERIC_READ | GENERIC_WRITE,
			                      FILE_SHARE_READ | FILE_SHARE_WRITE,
			                      NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (h != INVALID_HANDLE_VALUE)
			{
				HIDD_ATTRIBUTES attr;
				attr.Size = sizeof(HIDD_ATTRIBUTES);
				if (HidD_GetAttributes(h, &attr) &&
				    attr.VendorID  == PHYSIOAMP_VID &&
				    attr.ProductID == PHYSIOAMP_PID)
				{
					// query the actual report lengths from the caps
					PHIDP_PREPARSED_DATA preparsed = NULL;
					if (HidD_GetPreparsedData(h, &preparsed))
					{
						HIDP_CAPS caps;
						if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
						{
							if (caps.InputReportByteLength  > 0)
								inputReportLength  = caps.InputReportByteLength;
							if (caps.OutputReportByteLength > 0)
								outputReportLength = caps.OutputReportByteLength;

							// the report descriptor decides whether the payload is
							// preceded by a report id byte
							reportIdBytes = (inputReportLength > PA_REPORT_LEN) ? 1 : 0;
							write_logfile("PhysioAmp: report length in=%d out=%d, leading id bytes=%d",
							              inputReportLength, outputReportLength, reportIdBytes);
						}
						HidD_FreePreparsedData(preparsed);
					}
					hPhysioAmp = h;
					opened = TRUE;
					free(detail);
					break;
				}
				CloseHandle(h);
			}
		}
		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);
	return opened;
}


/*----------------------------------------------------------------------------
  send_report:  send one payload as an Output Report.

  The transfer buffer is exactly outputReportLength bytes, as the class driver
  expects, and the payload is copied behind the leading report id byte - limited
  to what actually fits, so a device whose output report is shorter than its input
  report cannot make this write past the end of the buffer.
----------------------------------------------------------------------------*/
static BOOL send_report(const unsigned char *payload)
{
	if (hPhysioAmp == INVALID_HANDLE_VALUE) return FALSE;

	unsigned int payloadLen = report_payload_len(outputReportLength);
	if (payloadLen == 0) return FALSE;

	unsigned char *buf = (unsigned char*) calloc(outputReportLength, 1);
	if (buf == NULL) return FALSE;

	memcpy(buf + reportIdBytes, payload, payloadLen);

	ZeroMemory(&writeOverlapped, sizeof(writeOverlapped));
	writeOverlapped.hEvent = hWriteEvent;
	ResetEvent(hWriteEvent);

	DWORD written = 0;
	BOOL  ok = FALSE;

	if (WriteFile(hPhysioAmp, buf, outputReportLength, &written, &writeOverlapped))
		ok = (written == outputReportLength);
	else if (GetLastError() == ERROR_IO_PENDING)
		ok = (wait_for_io(&writeOverlapped, hWriteEvent, PA_HANDSHAKE_TOTAL_MS, &written, FALSE) == 1)
		     && (written == outputReportLength);

	if (!ok)
		write_logfile("PhysioAmp: could not write output report");

	free(buf);
	return ok;
}


/*----------------------------------------------------------------------------
  send_command:  send a single byte command as an Output Report.
----------------------------------------------------------------------------*/
static BOOL send_command(unsigned char cmd)
{
	unsigned char payload[PA_REPORT_LEN];
	memset(payload, 0, sizeof(payload));
	payload[0] = cmd;
	return send_report(payload);
}


/*----------------------------------------------------------------------------
  read_response:  perform the handshake read and verify the reply signature.

  Reports without the signature are discarded, because a device left streaming by
  a previous session can still have a data frame in flight when the host asks for
  a new handshake. The whole search is bounded by PA_HANDSHAKE_TOTAL_MS, so a
  silent device cannot block the caller for longer than that.

  On success the payload is copied to respPayload (when not NULL) and the mode the
  device reports is returned through *mode: PA_MODE_SIGNAL, PA_MODE_IMPEDANCE, or
  -1 when the mode stamp matches neither.
----------------------------------------------------------------------------*/
static BOOL read_response(unsigned char *respPayload, int *mode)
{
	if (hPhysioAmp == INVALID_HANDLE_VALUE) return FALSE;

	unsigned int payloadLen = report_payload_len(inputReportLength);
	if (payloadLen < 6) return FALSE;              // needs the signature and the stamp

	unsigned char *buf = (unsigned char*) calloc(inputReportLength, 1);
	if (buf == NULL) return FALSE;

	DWORD deadline = GetTickCount() + PA_HANDSHAKE_TOTAL_MS;
	BOOL  valid = FALSE;

	for (int attempt = 0; attempt < PA_HANDSHAKE_DISCARDS && !valid; attempt++)
	{
		DWORD now = GetTickCount();
		if ((int)(deadline - now) <= 0)
		{
			write_logfile("PhysioAmp: no valid handshake reply within %d ms", PA_HANDSHAKE_TOTAL_MS);
			break;
		}

		ZeroMemory(&readOverlapped, sizeof(readOverlapped));
		readOverlapped.hEvent = hReadEvent;
		ResetEvent(hReadEvent);

		DWORD read  = 0;
		int   state = -1;

		if (ReadFile(hPhysioAmp, buf, inputReportLength, &read, &readOverlapped))
			state = 1;
		else if (GetLastError() == ERROR_IO_PENDING)
			state = wait_for_io(&readOverlapped, hReadEvent, deadline - now, &read, FALSE);
		else
			write_logfile("PhysioAmp: ReadFile failed during handshake");

		if (state != 1) break;                     // timeout, stop or device error
		if (read < (DWORD)(reportIdBytes + payloadLen)) continue;

		// the payload starts behind the leading report id byte, if there is one
		const unsigned char *payload = buf + reportIdBytes;

		if (payload[0] != PA_RESP_SIG0 || payload[1] != PA_RESP_SIG1 ||
		    payload[2] != PA_RESP_SIG2 || payload[3] != PA_RESP_SIG3)
			continue;

		valid = TRUE;
		if (respPayload != NULL) memcpy(respPayload, payload, payloadLen);
		if (mode != NULL)
		{
			if (payload[4] == PA_STAMP0_SIGNAL && payload[5] == PA_STAMP1_SIGNAL)
				*mode = PA_MODE_SIGNAL;
			else if (payload[4] == PA_STAMP0_IMPEDANCE && payload[5] == PA_STAMP1_IMPEDANCE)
				*mode = PA_MODE_IMPEDANCE;
			else
				*mode = -1;
		}
	}

	free(buf);
	return valid;
}


/*----------------------------------------------------------------------------
  do_handshake:  send the handshake request and evaluate the reply.

  Returns TRUE when the device answered with a valid signature; *mode then holds
  the mode the device is in and echo holds the reply, whose bytes 1..7 the start
  command has to echo back.
----------------------------------------------------------------------------*/
static BOOL do_handshake(int *mode, unsigned char *echo)
{
	if (!send_report(paHandshakeRequest)) return FALSE;
	if (!read_response(echo, mode))       return FALSE;
	return TRUE;
}


/*----------------------------------------------------------------------------
  parse_frame:  decode one data frame (payload, without any report id).

  Signal mode: 30 little-endian ADC codes, filled in time order. The six protocol
  channels are consecutive slices of that one ADC series, so the frame becomes a
  consecutive block of the single channel time series.
----------------------------------------------------------------------------*/
static void parse_frame(const unsigned char *payload)
{
	static BOOL typeIdWarned = FALSE;

	// bytes 0..3 are the frame header: frame number, type id, tick counter.
	// The tick counter advances by a fixed amount per frame, so a gap in it
	// means frames were lost - it covers 2050 (signal) / 2070 (impedance) ticks
	// where the 8 bit frame number only covers 256.
	int frameNumber = payload[0];
	int typeId      = payload[1];
	unsigned int tick = (unsigned int) payload[2] | ((unsigned int) payload[3] << 8);
	int tickStep = (streamingMode == PA_MODE_IMPEDANCE) ? PA_TICK_STEP_IMPEDANCE
	                                                    : PA_TICK_STEP_SIGNAL;

	if (typeId < 0x80 || typeId > 0x8F)
	{
		if (!typeIdWarned)
		{
			typeIdWarned = TRUE;
			write_logfile("PhysioAmp: unexpected frame type 0x%02X in frame %d", typeId, frameNumber);
		}
	}

	if (oldTick != PA_FRAME_TICK_NONE)
	{
		long delta = (long) tick - (long) oldTick;
		if (delta > 0)
		{
			if ((delta % tickStep) != 0)
				GLOBAL.syncloss++;                       // tick left the expected grid
			else if (delta > tickStep)
				GLOBAL.syncloss += (int)(delta / tickStep) - 1;
		}
		// delta <= 0: the counter wrapped (or stalled), no loss to account for
	}
	oldTick = tick;

	if (TTY.read_pause || GLOBAL.loading) return;

	for (int s = 0; s < PA_SAMPLES_PER_FRAME; s++)
	{
		int off = PA_DATA_OFFSET + s * 2;
		unsigned int sample = (unsigned int) payload[off] |
		                      ((unsigned int) payload[off + 1] << 8);

		PACKET.buffer[0] = sample;
		process_packets();          // drives the EEG object's work()
	}
}


/*----------------------------------------------------------------------------
  PhysioAmpReadThread:  reader loop.

  The read is waited for together with the stop event, so DisconnectPhysioAmp()
  can always bring this loop to an end - even while a read is pending.
----------------------------------------------------------------------------*/
static DWORD WINAPI PhysioAmpReadThread(LPVOID lpv)
{
	write_logfile("PhysioAmp reader thread running");

	unsigned char *buf = (unsigned char*) calloc(inputReportLength, 1);
	if (buf == NULL)
	{
		write_logfile("PhysioAmp: reader buffer allocation failed");
		return 0;
	}

	const DWORD frameLen = (DWORD)(reportIdBytes + PA_DATA_OFFSET + PA_SAMPLES_PER_FRAME * 2);

	for (;;)
	{
		ZeroMemory(&readOverlapped, sizeof(readOverlapped));
		readOverlapped.hEvent = hReadEvent;
		ResetEvent(hReadEvent);

		DWORD read  = 0;
		int   state;

		if (ReadFile(hPhysioAmp, buf, inputReportLength, &read, &readOverlapped))
			state = 1;
		else if (GetLastError() == ERROR_IO_PENDING)
			state = wait_for_io(&readOverlapped, hReadEvent, INFINITE, &read, TRUE);
		else
			state = -1;

		if (state == 0) break;                          // stop requested
		if (state < 0)
		{
			write_logfile("PhysioAmp: ReadFile failed, closing reader");
			break;
		}

		if (read >= frameLen) parse_frame(buf + reportIdBytes);
	}

	free(buf);
	write_logfile("PhysioAmp reader thread closed");
	return 0;
}


/*----------------------------------------------------------------------------
  ConnectPhysioAmp:  open the device, negotiate the requested mode, perform the
  handshake and start the reader thread.
----------------------------------------------------------------------------*/
BOOL ConnectPhysioAmp(HWND hDlg)
{
	int sav_pause;
	int mode = -1;
	unsigned char echo[PA_REPORT_LEN];

	write_logfile("Connecting PhysioAmp");

	(void)hDlg;   // handshake needs no dialog interaction for this device

	if (hPhysioAmp != INVALID_HANDLE_VALUE)
	{
		write_logfile("PhysioAmp already connected!");
		return TRUE;
	}

	// a reader that never finished would keep touching the object graph, so make
	// sure it is really gone before the device is opened again
	if (hReaderThread != NULL && !close_device())
	{
		write_logfile("PhysioAmp: previous reader thread still running, not connecting");
		return FALSE;
	}

	if (hStopEvent == NULL)  hStopEvent  = CreateEvent(NULL, TRUE,  FALSE, NULL);
	if (hReadEvent == NULL)  hReadEvent  = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hWriteEvent == NULL) hWriteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (hStopEvent == NULL || hReadEvent == NULL || hWriteEvent == NULL)
	{
		write_logfile("PhysioAmp: could not create the I/O events");
		return FALSE;
	}
	ResetEvent(hStopEvent);

	sav_pause = TTY.read_pause;
	TTY.read_pause = 1;

	if (!find_and_open_physioamp())
	{
		write_logfile("No PhysioAmp found");
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	// ask the device which mode it is in; 0xF8 toggles, so it must only be sent
	// when the device is not already in the requested mode
	memset(echo, 0, sizeof(echo));

	if (!do_handshake(&mode, echo))
	{
		write_logfile("PhysioAmp: handshake failed");
		close_device();
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	if (mode < 0)
	{
		// the reply carried no mode stamp we know, so toggling would be a guess
		write_logfile("PhysioAmp: handshake reply has no known mode stamp, refusing to switch");
		close_device();
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	if (mode != requestedMode)
	{
		write_logfile("PhysioAmp: device is in mode %d, switching to %d", mode, requestedMode);

		if (!send_command(PA_CMD_SWITCHMODE))
		{
			write_logfile("PhysioAmp: could not send mode switch command");
			close_device();
			TTY.read_pause = sav_pause;
			return FALSE;
		}

		if (!do_handshake(&mode, echo))
		{
			write_logfile("PhysioAmp: handshake after mode switch failed");
			close_device();
			TTY.read_pause = sav_pause;
			return FALSE;
		}

		if (mode != requestedMode)
		{
			write_logfile("PhysioAmp: mode switch failed, device reports mode %d", mode);
			close_device();
			TTY.read_pause = sav_pause;
			return FALSE;
		}
	}

	if (mode == PA_MODE_IMPEDANCE)
	{
		// an impedance frame is one signal at six phase delays, not a time series,
		// so feeding it into the EEG channel would be meaningless until the phase
		// analysis exists
		write_logfile("PhysioAmp: impedance mode is not supported yet, disconnecting");
		close_device();
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	// the start command is one report: byte 0 carries 0x8F, bytes 1..7 repeat the
	// handshake reply
	unsigned char startRequest[PA_REPORT_LEN];
	memset(startRequest, 0, sizeof(startRequest));
	startRequest[0] = PA_CMD_START;
	memcpy(startRequest + 1, echo + 1, 7);

	if (!send_report(startRequest))
	{
		write_logfile("PhysioAmp: could not send start command");
		close_device();
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	// launch the reader thread
	actualMode    = mode;
	streamingMode = mode;
	oldTick       = PA_FRAME_TICK_NONE;
	hReaderThread = CreateThread(NULL, 0, PhysioAmpReadThread, NULL, 0, NULL);
	if (hReaderThread == NULL)
	{
		write_logfile("PhysioAmp: could not create reader thread");
		close_device();
		TTY.read_pause = sav_pause;
		return FALSE;
	}

	TTY.read_pause = sav_pause;
	write_logfile("PhysioAmp connected (mode %d, %d samples/sec)",
	              mode, PhysioAmpGetSamplingRate());
	return TRUE;
}


/*----------------------------------------------------------------------------
  DisconnectPhysioAmp:  stop the reader thread and close the device.

  Returns FALSE when the reader could not be stopped; the caller then keeps the
  "connected" state instead of pretending the device is free.
----------------------------------------------------------------------------*/
BOOL DisconnectPhysioAmp(void)
{
	write_logfile("Disconnecting PhysioAmp");

	if (!close_device())
	{
		write_logfile("PhysioAmp: could not stop the reader thread");
		return FALSE;
	}

	return TRUE;
}


/*----------------------------------------------------------------------------
  mode accessors
----------------------------------------------------------------------------*/
BOOL PhysioAmpSetMode(int mode)
{
	if (mode != PA_MODE_SIGNAL && mode != PA_MODE_IMPEDANCE)
	{
		write_logfile("PhysioAmp: unknown mode %d", mode);
		return FALSE;
	}

	if (hPhysioAmp != INVALID_HANDLE_VALUE)
	{
		write_logfile("PhysioAmp: cannot change mode while connected");
		return FALSE;
	}

	requestedMode = mode;
	return TRUE;
}


int PhysioAmpGetMode(void)
{
	return actualMode;
}


int PhysioAmpGetSamplingRate(void)
{
	return (requestedMode == PA_MODE_IMPEDANCE) ? PA_SAMPLINGRATE_IMPEDANCE
	                                           : PA_SAMPLINGRATE_SIGNAL;
}


int PhysioAmpGetResolution(void)
{
	return (requestedMode == PA_MODE_IMPEDANCE) ? PA_RESOLUTION_IMPEDANCE
	                                           : PA_RESOLUTION_SIGNAL;
}
