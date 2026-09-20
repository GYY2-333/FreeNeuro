/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_ARTIFACT.H:  declarations for the Artifact / Signal-Quality object

  The Artifact object judges the quality of an incoming biosignal in real
  time. It is meant to sit between the amplifier and the analysing objects
  (Band-Power, Threshold, ...) so that eye blinks, muscle activity, movement
  and electrode problems cannot be mistaken for "good" training data.

  Criteria (all evaluated over a sliding analysis block):

      peak-to-peak amplitude above a limit        (movement / electrode pops)
      high frequency (EMG) rms above a limit      (muscle tension, jaw clench)
      absolute level at the ADC full scale        (clipping)
      missing samples                            (cable / driver problems)

  Output ports:
      0 quality      1.0 = block is usable, 0.0 = artifact
      1 artifact %   percentage of rejected data (sliding average)
      2 peak-to-peak amplitude of the last block (uV)
      3 high frequency rms of the last block (uV)

  Input ports:
      0 signal (raw biosignal)

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"

// second order IIR section, transposed direct form II
typedef struct ARBIQUAD
{
	double b0,b1,b2,a1,a2;
	double z1,z2;
} ARBIQUAD;


class ARTIFACTOBJ : public BASE_CL
{
  public:

	// ---- configuration -------------------------------------------------
	float	pp_threshold;			// peak-to-peak limit (uV)
	int		block_ms;				// length of one analysis block (ms)
	int		emg_enable;				// use the high frequency criterion
	float	emg_threshold;			// high frequency rms limit (uV)
	float	emg_cutoff;				// high pass cutoff of the EMG band (Hz)
	float	clip_level;				// level counted as clipping (uV)
	int		selftest;				// internal test signal generator

	// ---- runtime state -------------------------------------------------
	int		block_len;				// samples per block
	int		block_pos;
	float	block_min,block_max;
	int		block_clipped;
	int		last_fs;
	int		samples_since_input;
	float	input_value;
	float	quality;				// 1 = usable, 0 = artifact
	float	artifact_pct;			// sliding percentage of rejected blocks
	float	pp_amp;					// peak-to-peak of the last block (uV)
	float	emg_rms;				// high frequency rms (uV)
	double	emg_ema;				// smoothed square of the high pass signal
	double	testphase;
	int		blocks;					// number of evaluated blocks

	ARBIQUAD emg_hp;

	ARTIFACTOBJ(int num);
	~ARTIFACTOBJ();

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
	void	update_dialog_values(void);

	friend LRESULT CALLBACK ArtifactDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
};
