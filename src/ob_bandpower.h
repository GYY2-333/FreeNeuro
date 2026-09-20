/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_BANDPOWER.H:  declarations for the Band-Power / Ratio object

  The Band-Power object turns a raw biosignal into the frequency-band
  parameters that are used for ADHD assessment and neurofeedback:

      delta, theta, alpha, beta, gamma power   (calibrated, uV^2)
      theta/beta (TBR), theta/alpha, (delta+theta)/(alpha+beta)
      individual alpha peak frequency (IAF)

  Spectral estimation uses a sliding, Welch-style FFT of the preprocessed
  input (DC-blocker -> notch -> lowpass). The band power is calibrated so
  that a sine with amplitude A yields A^2/2 in its band, which makes the
  values comparable between sessions and devices.

  Output ports:
      0 delta        1 theta        2 alpha       3 beta       4 gamma
      5 theta/beta   6 theta/alpha  7 (delta+theta)/(alpha+beta)
      8 alpha peak frequency (IAF)
  Input ports:
      0 signal       1 optional quality gate (0 = hold outputs, from the
                        Artifact object)

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"
#include "ob_fft.h"                 // fft_float(), FFT_BUFFERLEN

#define BP_BANDS       5            // delta, theta, alpha, beta, gamma

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
	float	band_lo[BP_BANDS];
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
	float	iaf;					// smoothed alpha peak frequency (Hz)
	int		updates;				// number of spectra computed
	double	testphase1;				// phases of the internal test signal
	double	testphase2;

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
	void	update_dialog_values(float total);

	friend LRESULT CALLBACK BandpowerDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
};
