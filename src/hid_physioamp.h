/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: HID_PHYSIOAMP.H:  declarations for the PhysioAmp GP-8 USB HID interface

  The PhysioAmp GP-8 is a custom USB HID device (PIC18F4458 based) that delivers
  EEG signal data. Unlike the NIA (which is read via Raw Input), this device
  requires the host to actively send Output Reports for the handshake, so it is
  accessed through the Win32 SetupAPI + HidD/HidP interface.

  Handshake, signal mode (the device powers up in this mode):
     host --> 64 byte handshake request (byte 0 = 0x80)
     dev  --> 64 byte reply (77 62 11 00 4B D4 ...)
     host --> 64 byte start request (byte 0 = 0x8F, bytes 1..7 echoed from the reply)
     dev  ==> 64 byte data frames, one every 10ms

  Handshake, impedance mode:
     host --> 0xF8 (toggle mode; no reply, no state change on the device)
     host --> 64 byte handshake request (byte 0 = 0x80)
     dev  --> 64 byte reply (77 62 11 00 57 D3 ...)
     host --> 64 byte start request
     dev  ==> 64 byte data frames, one every 30ms

  Note that 0xF8 *toggles* the mode instead of setting it, so the host has to
  read the mode stamp (reply bytes 4..5) back from the device rather than assume
  that sending 0xF8 once selects impedance mode.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#ifndef HID_PHYSIOAMP_H
#define HID_PHYSIOAMP_H

#include <windows.h>

// --- USB identification ------------------------------------------------------
#define PHYSIOAMP_VID          0x0EA9   // idVendor  (PhysioData)
#define PHYSIOAMP_PID          0x0FA8   // idProduct (PhysioAmp GP-8)

// --- HID report / frame layout ----------------------------------------------
// The report descriptor declares no Report ID, so the three reports are plain
// 64 byte payloads: InputReportByteLength and OutputReportByteLength are both 64
// and byte 0 of the transfer buffer is byte 0 of the payload. A device that does
// declare Report IDs would report 65 byte transfers with an extra leading byte.
// hid_physioamp.cpp derives the leading byte count from the HID caps instead of
// assuming either layout.
#define PA_REPORT_LEN          64       // HID report payload length (bytes)
#define PA_DATA_OFFSET         4        // first sample byte within a data frame
#define PA_SAMPLES_PER_FRAME   30       // number of 16-bit samples per frame
#define PA_CHANNELS            6        // protocol channels a frame is split into
#define PA_SAMPLES_PER_CHANNEL (PA_SAMPLES_PER_FRAME / PA_CHANNELS)  // 5

// --- data frame header (bytes 0..3) -----------------------------------------
//   0      frame number 0..255; wraps, the wrap carries into the type id
//   1      type id 0x80..0x8F; advances only when byte 0 wraps
//   2..3   tick counter, little endian; steps by PA_TICK_STEP_* per frame and
//          restarts at 0 once the high byte passes 0x07
#define PA_FRAME_TICK_NONE     0xFFFF   // marker: no tick seen yet

// --- handshake commands (host -> device, byte 0 of an Output Report) ---------
#define PA_CMD_HANDSHAKE       0x80     // request handshake, device replies
#define PA_CMD_START           0x8F     // start streaming
#define PA_CMD_SWITCHMODE      0xF8     // toggle signal<->impedance mode

// --- handshake reply signature (reply bytes 0..3) ---------------------------
// Both modes answer with the same four bytes, so this only proves that the
// device is alive - the mode stamp below is what tells the modes apart.
#define PA_RESP_SIG0           0x77
#define PA_RESP_SIG1           0x62
#define PA_RESP_SIG2           0x11
#define PA_RESP_SIG3           0x00

// --- handshake reply mode stamp (reply bytes 4..5) --------------------------
#define PA_STAMP0_SIGNAL       0x4B     // reply[4] in signal mode
#define PA_STAMP1_SIGNAL       0xD4     // reply[5] in signal mode
#define PA_STAMP0_IMPEDANCE    0x57     // reply[4] in impedance mode
#define PA_STAMP1_IMPEDANCE    0xD3     // reply[5] in impedance mode

// --- working modes -----------------------------------------------------------
#define PA_MODE_IMPEDANCE      0        // MODE_IMPEDANCE in the device firmware
#define PA_MODE_SIGNAL         1        // MODE_SIGNAL, the power-up default

// --- per mode data characteristics ------------------------------------------
// signal:    one frame every 10ms, 30 samples per frame, 3000 samples/sec
// impedance: one frame every 30ms. The six protocol channels are one and the
//            same signal read through six different phase delays, so an
//            impedance frame is not a plain time series and cannot be fed into
//            the EEG channel unchanged.
//
// The samples are full width ADC codes, not a 12 bit range: the reference host
// reads them as 16 bit values (ADC_MAX_VALUE 65535) and measured codes from both
// the original unit and the clone run into the tens of thousands, well above the
// 4095 a 12 bit range would allow. The EEG object divides the codes by
// 1 << PA_RESOLUTION_*, so this has to match that code width. With a narrower one
// every sample is scaled past the top of the port range, and the oscilloscope
// (which clamps to the port range and does not clamp its drawing) shows a flat
// line against the edge of its band instead of a signal.
#define PA_RESOLUTION_SIGNAL       16
#define PA_RESOLUTION_IMPEDANCE    16
#define PA_SAMPLINGRATE_SIGNAL     3000
#define PA_SAMPLINGRATE_IMPEDANCE  1000     // ADC rate; per protocol channel 1000/6
#define PA_TICK_STEP_SIGNAL        10
#define PA_TICK_STEP_IMPEDANCE     30

// --- public interface --------------------------------------------------------
BOOL ConnectPhysioAmp(HWND hDlg);
BOOL DisconnectPhysioAmp(void);

// The reader thread only queues the decoded samples; they are fed into the object
// graph by process_physioamp(), which timer.cpp calls while GLOBAL.physioamp_available
// is set - one sample per packet time, exactly like process_biosemi() and
// process_brainflow(). Keeping process_packets() off the reader thread is what
// makes the disconnect work while a session is running.
void process_physioamp(void);

// Working mode the next ConnectPhysioAmp() asks the device for. The device is
// read in signal mode only for now, impedance needs the phase analysis first.
BOOL PhysioAmpSetMode(int mode);        // PA_MODE_SIGNAL / PA_MODE_IMPEDANCE
int  PhysioAmpGetMode(void);            // negotiated mode, -1 while disconnected
int  PhysioAmpGetSamplingRate(void);    // for the requested mode
int  PhysioAmpGetResolution(void);      // for the requested mode

#endif // HID_PHYSIOAMP_H
