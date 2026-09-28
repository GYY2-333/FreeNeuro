/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_BANDPOWER.H:  declarations for the Band-Power / Ratio object

  The Band-Power object turns a raw biosignal into the frequency-band
  parameters that are used for ADHD assessment:

      delta, theta, alpha, beta, hbeta, smr, beta2 (calibrated, uV^2)
      delta/theta, theta/beta, hbeta/beta, theta/smr, theta/beta2,
      theta/alpha, alpha/hbeta

  Spectral estimation uses a sliding, Welch-style FFT of the preprocessed
  input (DC-blocker -> notch -> lowpass). The band power is calibrated so
  that a sine with amplitude A yields A^2/2 in its band, which makes the
  values comparable between sessions and devices.

  Output ports (14, the parameter set of the protocol design; the indicator
  panel shows exactly these values in the same order):
      0 delta       1 theta      2 alpha     3 beta      4 hbeta     5 smr
      6 beta2       7 delta/theta            8 theta/beta
      9 hbeta/beta  10 theta/smr             11 theta/beta2
      12 theta/alpha            13 alpha/hbeta
  Input ports:
      0 signal       1 optional quality gate (0 = hold outputs, from the
                        Artifact object)

  The default band edges follow the ADHD protocol design that this object
  was modelled on:
      delta 0.5-4   theta 4-8   alpha 8-12   beta 13-21   hbeta 20-30
      smr 12-15     beta2 16-21
  All edges can be changed in the toolbox; bands are allowed to overlap.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"
#include "ob_fft.h"                 // fft_float(), FFT_BUFFERLEN

// band powers, in the order of the output ports 0 .. 6 (and of the panel cells)
#define BP_BANDS       7            // delta, theta, alpha, beta, hbeta, smr, beta2
#define BP_RATIOS      7            // ratio outputs, ports 7 .. 13
#define BP_PORTS       14           // the 7 band powers + the 7 band ratios
#define BP_PANELCELLS  14           // indicator panel: the 7 band powers + the 7 ratios

// second order IIR section, transposed direct form II
typedef struct BPBIQUAD
{
	double b0,b1,b2,a1,a2;
	double z1,z2;
} BPBIQUAD;


class BANDPOWEROBJ : public BASE_CL
{
  public:

	// ---- configuration -------------------------------------------------
	float	fft_seconds;			// length of one spectrum window (seconds)
	int		window;					// 0 = rectangular, 1 = hanning
	int		hop_ms;					// interval between two spectra (ms)
	int		units;					// 0 = uV^2, 1 = dB, 2 = relative %
	float	smooth;					// smoothing time constant (s)
	float	notchfreq;				// 0 = notch disabled
	float	notchq;
	float	lpcutoff;
	float	band_lo[BP_BANDS];		// band edges (Hz), bands may overlap
	float	band_hi[BP_BANDS];
	int		selftest;				// internal test signal generator
	int		logvalues;				// write the band powers to the logfile

	// ---- runtime state -------------------------------------------------
	int		fftsize;				// samples used for one spectrum
	int		hop;					// samples between two spectra
	int		last_fs;				// sampling rate the filters were built for
	float	ringbuf[FFT_BUFFERLEN];
	int		ringpos;
	int		hopcount;
	float	fftbands[FFT_BUFFERLEN>>1];
	float	input_value;
	float	gate;
	float	rawpow[BP_BANDS];		// unsmoothed band power (uV^2)
	float	avgpow[BP_BANDS];		// smoothed band power (uV^2)
	int		updates;				// number of spectra computed
	double	testphase1;				// phases of the internal test signal
	double	testphase2;
	double	testphase3;

	// ---- indicator panel (own display window) --------------------------
	int		left,top,right,bottom;	// panel window geometry
	char	wndcaption[50];
	int		showpanel;				// show the panel window
	int		columns;				// panel cells per row (1 .. 4)
	int		panelsize;				// font size of the big numbers (points)
	int		panelrows;				// derived from columns
	int		showunits;				// append the unit to the cell caption
	COLORREF panelbk;				// panel background colour
	COLORREF panelcaptcol;			// caption colour
	COLORREF panelvalcol;			// value colour
	HFONT	valfont;
	HFONT	captfont;

	BPBIQUAD dcblock;
	BPBIQUAD notch;
	BPBIQUAD lp1;
	BPBIQUAD lp2;

	BANDPOWEROBJ(int num);
	~BANDPOWEROBJ();

	void	make_dialog(void);
	void	load(HANDLE hFile);
	void	save(HANDLE hFile);
	void	incoming_data(int port, float value);
	void	session_start(void);
	void	session_reset(void);
	void	session_pos(long pos);
	void	work(void);

	void	reset_dsp(void);
	void	update_filters(void);
	void	update_outport_ranges(void);
	float	output_value(int band, float total);
	float	ratio_value(int r);
	void	update_dialog_values(float total);

	void	create_panel_window(void);
	void	update_panel_fonts(void);
	void	draw_panel(void);
	const char *panel_caption(int cell);
	void	panel_value_string(int cell, float total, char * out);

	friend LRESULT CALLBACK BandpowerDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
};

//  from OB_BANDPOWER.CPP :
LRESULT CALLBACK BandPanelWndHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
