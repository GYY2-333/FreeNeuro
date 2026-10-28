/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_BANDPOWER.CPP:  functions for the Band-Power / Ratio object

  see ob_bandpower.h for a description of the object and its output ports.

  BandpowerDlgHandler: processes the events of the settings window.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"
#include "ob_bandpower.h"

// names of the frequency bands, in the order of the output ports 0 .. 6
static const char *bp_bandname[BP_BANDS] =
{ "delta", "theta", "alpha", "beta", "hbeta", "smr", "beta2" };

// every ratio is one band divided by another band: {numerator, denominator}
// the order matches the output ports 7 .. 13 and the panel cells 7 .. 13
static const int bp_ratioidx[BP_RATIOS][2] =
{
	{ 0, 1 },   // delta / theta
	{ 1, 3 },   // theta / beta
	{ 4, 3 },   // hbeta / beta
	{ 1, 5 },   // theta / smr
	{ 1, 6 },   // theta / beta2
	{ 1, 2 },   // theta / alpha
	{ 2, 4 }    // alpha / hbeta
};

#define BP_PI 3.14159265358979323846

// a ratio is meaningless when the divisor is below the noise floor - report 0
// instead of a huge number (an idle band, e.g. alpha with eyes open).
// the result is also clamped to the range that the ratio ports advertise,
// so that a noise band can never produce an unreadable value.
#define BP_RATIO_EPS 0.05f
#define BP_RATIO_MAX 10.0f

static float bp_ratio(float num, float den)
{
	float r;

	if (den<BP_RATIO_EPS) return(0.0f);
	r=num/den;
	if (r<0.0f) r=0.0f;
	if (r>BP_RATIO_MAX) r=BP_RATIO_MAX;
	return(r);
}


// long names of the bands, used for the port descriptions.  "Q-beta" is the
// 13-21 Hz band and "beta" the 16-21 Hz band, exactly as printed in the design.
static const char *bp_descname[BP_BANDS] =
{ "Delta", "Theta", "Alpha", "Q-beta", "High-beta", "SMR", "Beta" };

// range/dimension of a band-power port, depending on the output unit
static void bp_unit_range(OUTPORTStruct *p, int units)
{
	if (units==1)                                       // dB
	{
		strcpy(p->out_dim,"dB");
		p->out_min=-20.0f;
		p->out_max=60.0f;
	}
	else if (units==2)                                  // relative power
	{
		strcpy(p->out_dim,"%");
		p->out_min=0.0f;
		p->out_max=100.0f;
	}
	else if (units==3)                                  // RMS amplitude
	{
		strcpy(p->out_dim,"uV");
		p->out_min=0.0f;
		p->out_max=100.0f;
	}
	else                                                // uV^2
	{
		strcpy(p->out_dim,"uV^2");
		p->out_min=0.0f;
		p->out_max=1000.0f;
	}
}

static void bp_ratio_range(OUTPORTStruct *p)
{
	strcpy(p->out_dim,"ratio");
	p->out_min=0.0f;
	p->out_max=BP_RATIO_MAX;
}


/* -------------------------------------------------------------------------
   second order IIR helpers
   ------------------------------------------------------------------------- */

static void bq_reset(BPBIQUAD *f)
{
	f->z1=0.0; f->z2=0.0;
}

static double bq_process(BPBIQUAD *f, double x)
{
	double y = f->b0*x + f->z1;
	f->z1 = f->b1*x - f->a1*y + f->z2;
	f->z2 = f->b2*x - f->a2*y;
	return(y);
}

// one pole DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]
static void bq_dcblocker(BPBIQUAD *f, double cutoff, double fs)
{
	double r = exp(-2.0*BP_PI*cutoff/fs);

	f->b0=1.0; f->b1=-1.0; f->b2=0.0;
	f->a1=-r;  f->a2=0.0;
	bq_reset(f);
}

// 2nd order IIR notch (RBJ cookbook)
static void bq_notch(BPBIQUAD *f, double freq, double q, double fs)
{
	double w0 = 2.0*BP_PI*freq/fs;
	double alpha = sin(w0)/(2.0*q);
	double a0 = 1.0 + alpha;
	double cw = cos(w0);

	f->b0 =  1.0/a0;
	f->b1 = -2.0*cw/a0;
	f->b2 =  1.0/a0;
	f->a1 = -2.0*cw/a0;
	f->a2 = (1.0-alpha)/a0;
	bq_reset(f);
}

// lowpass section (RBJ cookbook) with a given Q - two of them give a
// 4th order Butterworth lowpass
static void bq_lowpass(BPBIQUAD *f, double freq, double q, double fs)
{
	double w0 = 2.0*BP_PI*freq/fs;
	double cw = cos(w0);
	double sw = sin(w0);
	double alpha = sw/(2.0*q);
	double a0 = 1.0 + alpha;
	double b0 = (1.0-cw)/2.0;

	f->b0 = b0/a0;
	f->b1 = (1.0-cw)/a0;
	f->b2 = b0/a0;
	f->a1 = -2.0*cw/a0;
	f->a2 = (1.0-alpha)/a0;
	bq_reset(f);
}

// highpass section (RBJ cookbook)
static void bq_highpass(BPBIQUAD *f, double freq, double q, double fs)
{
	double w0 = 2.0*BP_PI*freq/fs;
	double cw = cos(w0);
	double sw = sin(w0);
	double alpha = sw/(2.0*q);
	double a0 = 1.0 + alpha;

	f->b0 = (1.0+cw)/2.0/a0;
	f->b1 = -(1.0+cw)/a0;
	f->b2 = (1.0+cw)/2.0/a0;
	f->a1 = -2.0*cw/a0;
	f->a2 = (1.0-alpha)/a0;
	bq_reset(f);
}


/* -------------------------------------------------------------------------
   object
   ------------------------------------------------------------------------- */

BANDPOWEROBJ::BANDPOWEROBJ(int num) : BASE_CL()
{
	int i;

	width=90;

	// ---- ports
	outports=BP_PORTS;
	inports=2;

	// 0 .. 6 : the band powers (the descriptions are filled in by
	// update_outport_ranges(), which knows the current band edges)
	// the names follow the design: Q-beta = 13-21 Hz, beta = 16-21 Hz
	strcpy(out_ports[0].out_name,"delta");
	strcpy(out_ports[1].out_name,"theta");
	strcpy(out_ports[2].out_name,"alpha");
	strcpy(out_ports[3].out_name,"Qbeta");
	strcpy(out_ports[4].out_name,"hbeta");
	strcpy(out_ports[5].out_name,"smr");
	strcpy(out_ports[6].out_name,"beta");
	// 7 .. 13 : the band ratios
	strcpy(out_ports[7].out_name,"delta/theta");
	strcpy(out_ports[8].out_name,"theta/Qbeta");
	strcpy(out_ports[9].out_name,"hbeta/Qbeta");
	strcpy(out_ports[10].out_name,"theta/smr");
	strcpy(out_ports[11].out_name,"theta/beta");
	strcpy(out_ports[12].out_name,"theta/alpha");
	strcpy(out_ports[13].out_name,"alpha/hbeta");

	for (i=0;i<outports;i++)
	{
		strcpy(out_ports[i].out_dim,"uV^2");
		out_ports[i].get_range=-1;          // the object defines its own range
		out_ports[i].out_min=0.0f;
		out_ports[i].out_max=1000.0f;
	}

	strcpy(in_ports[0].in_name,"signal");
	strcpy(in_ports[0].in_desc,"raw biosignal");
	strcpy(in_ports[0].in_dim,"uV");
	in_ports[0].get_range=1;                // take range from the source
	in_ports[0].in_min=-500.0f;
	in_ports[0].in_max=500.0f;

	strcpy(in_ports[1].in_name,"gate");
	strcpy(in_ports[1].in_desc,"quality gate (holds outputs when 0)");
	strcpy(in_ports[1].in_dim,"none");
	in_ports[1].get_range=0;
	in_ports[1].in_min=0.0f;
	in_ports[1].in_max=1.0f;

	// ---- defaults
	fft_seconds=2.0f;                       // ~2 seconds per spectrum
	window=1;                               // Hanning
	hop_ms=100;                             // 10 spectra per second
	units=0;                                // uV^2
	smooth=1.0f;
	notchfreq=50.0f;
	notchq=20.0f;
	lpcutoff=45.0f;
	selftest=0;
	logvalues=0;

	// ---- indicator panel defaults (dark background, big blue digits)
	left=360; top=70; right=620; bottom=690;
	strcpy(wndcaption,"Band-Power");
	showpanel=1;
	columns=2;                              // 7 rows x 2 columns, like the protocol
	panelfirst=0;							// this panel shows all cells by default
	panelcells=BP_PANELCELLS;
	highlight=0;							// no cell marked as "in use" by default
	panelsize=24;
	panelrows=BP_PANELCELLS/2;
	showunits=1;
	panelbk=RGB(42,12,42);
	panelcaptcol=RGB(168,168,182);
	panelvalcol=RGB( 90,200,255);
	valfont=NULL;
	captfont=NULL;

	// ---- band edges of the ADHD protocol design (Hz), bands may overlap
	band_lo[0]=0.5f;  band_hi[0]=4.0f;      // delta
	band_lo[1]=4.0f;  band_hi[1]=8.0f;      // theta
	band_lo[2]=8.0f;  band_hi[2]=12.0f;     // alpha
	band_lo[3]=13.0f; band_hi[3]=21.0f;     // beta
	band_lo[4]=20.0f; band_hi[4]=30.0f;     // hbeta
	band_lo[5]=12.0f; band_hi[5]=15.0f;     // smr
	band_lo[6]=16.0f; band_hi[6]=21.0f;     // beta2

	fftsize=(int)((double)fft_seconds*(double)PACKETSPERSECOND);
	hop=0;
	last_fs=0;
	updates=0;
	gate=1.0f;

	update_outport_ranges();
	reset_dsp();
	update_filters();

	update_panel_fonts();
	create_panel_window();
}


BANDPOWEROBJ::~BANDPOWEROBJ()
{
	if (valfont) { DeleteObject(valfont); valfont=NULL; }
	if (captfont) { DeleteObject(captfont); captfont=NULL; }
}


void BANDPOWEROBJ::reset_dsp(void)
{
	int i;

	for (i=0;i<FFT_BUFFERLEN;i++) ringbuf[i]=0.0f;
	for (i=0;i<(FFT_BUFFERLEN>>1);i++) fftbands[i]=0.0f;

	ringpos=0;
	hopcount=0;
	updates=0;
	input_value=0.0f;
	gate=1.0f;
	testphase1=0.0;
	testphase2=0.0;
	testphase3=0.0;

	for (i=0;i<BP_BANDS;i++) { rawpow[i]=0.0f; avgpow[i]=0.0f; }

	bq_reset(&dcblock);
	bq_reset(&notch);
	bq_reset(&lp1);
	bq_reset(&lp2);
}


void BANDPOWEROBJ::update_filters(void)
{
	double fs = (double) PACKETSPERSECOND;
	double nyq = fs/2.0;
	double lp = (double) lpcutoff;

	if (fs<2.0) fs=2.0;
	if (lp>nyq*0.95) lp=nyq*0.95;
	if (lp<1.0) lp=1.0;

	bq_dcblocker(&dcblock, 0.5, fs);
	if ((notchfreq>0.5f)&&((double)notchfreq<nyq*0.95))
		bq_notch(&notch, (double)notchfreq, (double)notchq, fs);

	// 4th order Butterworth lowpass as two cascaded sections
	bq_lowpass(&lp1, lp, 0.54119610, fs);
	bq_lowpass(&lp2, lp, 1.30656296, fs);

	// spectral settings that depend on the sampling rate: the window length
	// is given in seconds, fft_float() needs a power of two, so the closest
	// one is used (256 ... FFT_BUFFERLEN samples)
	{
		double want=(double)fft_seconds*fs;
		int n;

		if (want<1.0) want=1.0;
		n=(int)pow(2.0,floor(log(want)/log(2.0)+0.5));
		if (n<256) n=256;
		if (n>FFT_BUFFERLEN) n=FFT_BUFFERLEN;
		fftsize=n;
	}

	hop=(int)((double)hop_ms*fs/1000.0);
	if (hop<1) hop=1;
	if (hop>fftsize) hop=fftsize;

	last_fs=PACKETSPERSECOND;
}


void BANDPOWEROBJ::update_outport_ranges(void)
{
	int i;

	// 0 .. 6 : the band powers (the descriptions follow the current band edges)
	for (i=0;i<=6;i++)
	{
		bp_unit_range(&out_ports[i],units);
		sprintf(out_ports[i].out_desc,"%s power (%.1f - %.1f Hz)",
			bp_descname[i],band_lo[i],band_hi[i]);
	}

	// 7 .. 13 : the band ratios
	for (i=0;i<BP_RATIOS;i++)
	{
		bp_ratio_range(&out_ports[7+i]);
		sprintf(out_ports[7+i].out_desc,"%s / %s ratio",
			bp_descname[bp_ratioidx[i][0]],bp_descname[bp_ratioidx[i][1]]);
	}
}


float BANDPOWEROBJ::output_value(int band, float total)
{
	if (band<0) band=0;
	if (band>=BP_BANDS) band=BP_BANDS-1;

	if (units==1)                                      // dB (10*log10)
	{
		if (avgpow[band]<1.0e-4f) return(-40.0f);
		return((float)(10.0*log10((double)avgpow[band])));
	}
	if (units==2)                                      // relative power
	{
		if (total<1.0e-6f) return(0.0f);
		return((float)(100.0*(double)avgpow[band]/(double)total));
	}
	if (units==3)                                      // RMS amplitude in uV
	{
		if (avgpow[band]<=0.0f) return(0.0f);
		return((float)sqrt((double)avgpow[band]));
	}
	return(avgpow[band]);
}


// one of the band/band ratios (index 0 .. BP_RATIOS-1, port 7 .. 13)
float BANDPOWEROBJ::ratio_value(int r)
{
	if (r<0) r=0;
	if (r>=BP_RATIOS) r=BP_RATIOS-1;
	return(bp_ratio(avgpow[bp_ratioidx[r][0]],avgpow[bp_ratioidx[r][1]]));
}


void BANDPOWEROBJ::incoming_data(int port, float value)
{
	if (port==1) gate=value;
	else input_value=value;
}


void BANDPOWEROBJ::make_dialog(void)
{
	display_toolbox(hDlg=CreateDialog(hInst, (LPCTSTR)IDD_BANDPOWERBOX, ghWndStatusbox, (DLGPROC)BandpowerDlgHandler));
}


void BANDPOWEROBJ::session_start(void) { reset_dsp(); update_filters(); }
void BANDPOWEROBJ::session_reset(void) { reset_dsp(); }
void BANDPOWEROBJ::session_pos(long pos) { reset_dsp(); }


void BANDPOWEROBJ::load(HANDLE hFile)
{
	int i;
	char pname[30];

	load_object_basics(this);

	load_property("bp_fftsecs",P_FLOAT,&fft_seconds);
	load_property("bp_window",P_INT,&window);
	load_property("bp_hopms",P_INT,&hop_ms);
	load_property("bp_units",P_INT,&units);
	load_property("bp_smooth",P_FLOAT,&smooth);
	load_property("bp_notchfreq",P_FLOAT,&notchfreq);
	load_property("bp_notchq",P_FLOAT,&notchq);
	load_property("bp_lpcutoff",P_FLOAT,&lpcutoff);
	load_property("bp_selftest",P_INT,&selftest);
	load_property("bp_logvalues",P_INT,&logvalues);

	load_property("bp_panelcaption",P_STRING,wndcaption);
	load_property("bp_showpanel",P_INT,&showpanel);
	load_property("bp_columns",P_INT,&columns);
	load_property("bp_first",P_INT,&panelfirst);
	load_property("bp_cells",P_INT,&panelcells);
	load_property("bp_hl",P_INT,&highlight);
	load_property("bp_panelsize",P_INT,&panelsize);
	load_property("bp_showunits",P_INT,&showunits);
	load_property("bp_valcol",P_INT,&panelvalcol);
	load_property("bp_bkcol",P_INT,&panelbk);
	load_property("bp_captcol",P_INT,&panelcaptcol);
	load_property("bp_paneltop",P_INT,&top);
	load_property("bp_panelleft",P_INT,&left);
	load_property("bp_panelright",P_INT,&right);
	load_property("bp_panelbottom",P_INT,&bottom);

	for (i=0;i<BP_BANDS;i++)
	{
		sprintf(pname,"bp_%slo",bp_bandname[i]);
		load_property(pname,P_FLOAT,&band_lo[i]);
		sprintf(pname,"bp_%shi",bp_bandname[i]);
		load_property(pname,P_FLOAT,&band_hi[i]);
	}

	update_outport_ranges();
	reset_dsp();
	last_fs=0;                     // force the filters to be rebuilt
	update_filters();

	// apply the panel settings to the display window
	update_panel_fonts();
	if (displayWnd!=NULL)
	{
		SetWindowText(displayWnd,wndcaption);
		MoveWindow(displayWnd,left,top,right-left,bottom-top,TRUE);
		ShowWindow(displayWnd, showpanel ? SW_SHOW : SW_HIDE);
		InvalidateRect(displayWnd,NULL,TRUE);
	}
}


void BANDPOWEROBJ::save(HANDLE hFile)
{
	int i;
	char pname[30];

	save_object_basics(hFile, this);

	save_property(hFile,"bp_fftsecs",P_FLOAT,&fft_seconds);
	save_property(hFile,"bp_window",P_INT,&window);
	save_property(hFile,"bp_hopms",P_INT,&hop_ms);
	save_property(hFile,"bp_units",P_INT,&units);
	save_property(hFile,"bp_smooth",P_FLOAT,&smooth);
	save_property(hFile,"bp_notchfreq",P_FLOAT,&notchfreq);
	save_property(hFile,"bp_notchq",P_FLOAT,&notchq);
	save_property(hFile,"bp_lpcutoff",P_FLOAT,&lpcutoff);
	save_property(hFile,"bp_selftest",P_INT,&selftest);
	save_property(hFile,"bp_logvalues",P_INT,&logvalues);

	save_property(hFile,"bp_panelcaption",P_STRING,wndcaption);
	save_property(hFile,"bp_showpanel",P_INT,&showpanel);
	save_property(hFile,"bp_columns",P_INT,&columns);
	save_property(hFile,"bp_first",P_INT,&panelfirst);
	save_property(hFile,"bp_cells",P_INT,&panelcells);
	save_property(hFile,"bp_hl",P_INT,&highlight);
	save_property(hFile,"bp_panelsize",P_INT,&panelsize);
	save_property(hFile,"bp_showunits",P_INT,&showunits);
	save_property(hFile,"bp_valcol",P_INT,&panelvalcol);
	save_property(hFile,"bp_bkcol",P_INT,&panelbk);
	save_property(hFile,"bp_captcol",P_INT,&panelcaptcol);
	save_property(hFile,"bp_paneltop",P_INT,&top);
	save_property(hFile,"bp_panelleft",P_INT,&left);
	save_property(hFile,"bp_panelright",P_INT,&right);
	save_property(hFile,"bp_panelbottom",P_INT,&bottom);

	for (i=0;i<BP_BANDS;i++)
	{
		sprintf(pname,"bp_%slo",bp_bandname[i]);
		save_property(hFile,pname,P_FLOAT,&band_lo[i]);
		sprintf(pname,"bp_%shi",bp_bandname[i]);
		save_property(hFile,pname,P_FLOAT,&band_hi[i]);
	}
}


void BANDPOWEROBJ::work(void)
{
	double x;
	double sum,wfac,alpha_s,dt;
	int b,k,k0,k1;
	float total;

	if (PACKETSPERSECOND!=last_fs) update_filters();

	if (selftest)
	{
		// internal verification signal: 6 Hz / 20 uV (theta) + 13.5 Hz / 10 uV
		// (smr, beta) + 25 Hz / 10 uV (hbeta)
		x = 20.0*sin(testphase1) + 10.0*sin(testphase2) + 10.0*sin(testphase3);
		testphase1 += 2.0*BP_PI*6.0/(double)PACKETSPERSECOND;
		testphase2 += 2.0*BP_PI*13.5/(double)PACKETSPERSECOND;
		testphase3 += 2.0*BP_PI*25.0/(double)PACKETSPERSECOND;
		if (testphase1>=2.0*BP_PI) testphase1-=2.0*BP_PI;
		if (testphase2>=2.0*BP_PI) testphase2-=2.0*BP_PI;
		if (testphase3>=2.0*BP_PI) testphase3-=2.0*BP_PI;
	}
	else
	{
		if (input_value==(float)INVALID_VALUE) return;   // no valid data
		x = (double) input_value;
	}

	// ---- preprocessing
	x = bq_process(&dcblock, x);
	if (notchfreq>0.5f) x = bq_process(&notch, x);
	x = bq_process(&lp1, x);
	x = bq_process(&lp2, x);

	// ---- ring buffer
	ringbuf[ringpos]=(float)x;
	ringpos++;
	if (ringpos>=FFT_BUFFERLEN) ringpos=0;

	hopcount++;

	if (hopcount>=hop)
	{
		hopcount=0;

		fft_float(ringpos, ringbuf, window, fftbands, fftsize>>1);

		// a sine with amplitude A gives A^2/2 in its band
		wfac = (window==1) ? 0.375 : 1.0;

		for (b=0;b<BP_BANDS;b++)
		{
			k0=(int)ceil((double)band_lo[b]*(double)fftsize/(double)PACKETSPERSECOND);
			k1=(int)floor((double)band_hi[b]*(double)fftsize/(double)PACKETSPERSECOND);
			if (k0<1) k0=1;
			if (k1>(fftsize>>1)-1) k1=(fftsize>>1)-1;
			if (k1<k0) k1=k0;

			sum=0.0;
			for (k=k0;k<=k1;k++) sum += (double)fftbands[k]*(double)fftbands[k];

			rawpow[b]=(float)(sum/(2.0*wfac));
			if (rawpow[b]<0.0f) rawpow[b]=0.0f;
		}

		// ---- smoothing
		dt=(double)hop/(double)PACKETSPERSECOND;
		alpha_s=(smooth>0.01f) ? (1.0-exp(-dt/(double)smooth)) : 1.0;
		if (alpha_s>1.0) alpha_s=1.0;
		if (alpha_s<0.0) alpha_s=0.0;

		if (updates<1)
		{
			for (b=0;b<BP_BANDS;b++) avgpow[b]=rawpow[b];
		}
		else if (gate>0.5f)
		{
			for (b=0;b<BP_BANDS;b++)
				avgpow[b]+=(float)(alpha_s*((double)rawpow[b]-(double)avgpow[b]));
		}

		updates++;
		if (updates>2000000000) updates=2000000000;

		if (((selftest)||(logvalues))&&((updates%10)==0))
			write_logfile("bandpower: delta=%.1f theta=%.1f alpha=%.1f beta=%.1f hbeta=%.1f smr=%.1f beta2=%.1f | d/t=%.3f t/b=%.3f hb/b=%.3f t/smr=%.3f t/b2=%.3f t/a=%.3f a/hb=%.3f",
				avgpow[0], avgpow[1], avgpow[2], avgpow[3], avgpow[4], avgpow[5], avgpow[6],
				ratio_value(0), ratio_value(1), ratio_value(2), ratio_value(3),
				ratio_value(4), ratio_value(5), ratio_value(6));
	}

	// ---- outputs: the 7 band powers and the 7 band ratios
	total=0.0f;
	for (b=0;b<BP_BANDS;b++) total+=avgpow[b];

	for (b=0;b<=6;b++) pass_values(b, output_value(b,total));   // band powers
	for (b=0;b<BP_RATIOS;b++) pass_values(7+b, ratio_value(b)); // band ratios

	// ---- indicator panel
	if ((displayWnd!=NULL)&&(!TIMING.draw_update)&&(!GLOBAL.fly))
		InvalidateRect(displayWnd,NULL,FALSE);

	if ((hDlg==ghWndToolbox)&&(!TIMING.dialog_update)) update_dialog_values(total);
}


/* -------------------------------------------------------------------------
   indicator panel (own display window)
   ------------------------------------------------------------------------- */

// captions of the panel cells: cells 0 .. 6 are the band powers, cells 7 .. 13
// the ratios - exactly the parameter set of the protocol design.  The greek
// letters are written with \u escapes in a wide literal, so that the source
// file encoding does not matter; bp_ansi() converts them to the code page of
// the window at draw time.
static const wchar_t *bp_cellcaption[BP_PANELCELLS] =
{
	L"\u03B4",              // delta
	L"\u03B8",              // theta
	L"\u03B1",              // alpha
	L"Q\u03B2",             // beta (13 - 21 Hz), as printed in the protocol design
	L"H\u03B2",             // high beta
	L"SMR",                 // sensorimotor rhythm
	L"\u03B2",              // beta2 (16 - 21 Hz)
	L"\u03B4/\u03B8",       // delta / theta
	L"\u03B8/Q\u03B2",      // theta / beta
	L"H\u03B2/Q\u03B2",     // hbeta / beta
	L"\u03B8/SMR",          // theta / smr
	L"\u03B8/\u03B2",       // theta / beta2
	L"\u03B8/\u03B1",       // theta / alpha
	L"\u03B1/H\u03B2"       // alpha / hbeta
};

static void bp_ansi(const wchar_t *w, char *out, int size)
{
	out[0]=0;
	WideCharToMultiByte(CP_ACP,0,w,-1,out,size,NULL,NULL);
	out[size-1]=0;
}


void BANDPOWEROBJ::create_panel_window(void)
{
	if(!(displayWnd=CreateWindow("BandPanel_Class", wndcaption,
		WS_CLIPSIBLINGS | WS_CHILD | WS_CAPTION | WS_THICKFRAME,
		left, top, right-left, bottom-top, ghWndMain, NULL, hInst, NULL)))
	{
		report_error("can't create Band-Power panel window");
		return;
	}

	SetWindowLong(displayWnd, GWL_STYLE, GetWindowLong(displayWnd, GWL_STYLE) | WS_SIZEBOX);
	ShowWindow(displayWnd, showpanel ? SW_SHOW : SW_HIDE);
	UpdateWindow(displayWnd);
}


void BANDPOWEROBJ::update_panel_fonts(void)
{
	int dpi=GetDeviceCaps(GetDC(NULL), LOGPIXELSY);
	int captsize;

	if (valfont)  { DeleteObject(valfont);  valfont=NULL; }
	if (captfont) { DeleteObject(captfont); captfont=NULL; }

	if (panelsize<8) panelsize=8;
	if (panelsize>200) panelsize=200;
	captsize=panelsize/3;
	if (captsize<9) captsize=9;

	valfont=CreateFont(-MulDiv(panelsize,dpi,72),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
		OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Arial");
	captfont=CreateFont(-MulDiv(captsize,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
		OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Arial");
}


//  caption of one panel cell, e.g. "theta  [uV^2]" / "theta/beta  [ratio]"
const char *BANDPOWEROBJ::panel_caption(int cell)
{
	static char cap[80];
	char sym[40];
	const char *unit;

	if (cell<0) cell=0;
	if (cell>=BP_PANELCELLS) cell=BP_PANELCELLS-1;

	if (cell<7)                                         // band powers
		unit=(units==1) ? "dB" : ((units==2) ? "%" : ((units==3) ? "uV" : "uV^2"));
	else                                                // band ratios
		unit="ratio";

	bp_ansi(bp_cellcaption[cell],sym,40);

	if (showunits) sprintf(cap,"%s  [%s]",sym,unit);
	else strcpy(cap,sym);

	return(cap);
}


void BANDPOWEROBJ::panel_value_string(int cell, float total, char * out)
{
	float v;
	int digits;

	if (cell<0) cell=0;
	if (cell>=BP_PANELCELLS) cell=BP_PANELCELLS-1;

	if (cell<7)
	{
		v=output_value(cell,total);                     // band power, unit aware
		digits=1;
	}
	else
	{
		v=ratio_value(cell-7);                          // band ratio
		digits=3;
	}

	if (v>=1000.0f) digits=0;
	else if ((v>=100.0f)&&(digits>1)) digits=1;

	sprintf(out,"%.*f",digits,v);
}


void BANDPOWEROBJ::draw_panel(void)
{
	PAINTSTRUCT ps;
	HDC hdc,memdc;
	HBITMAP membmp,oldbmp;
	RECT rc,cell,cap,vr;
	HBRUSH bkbrush;
	HPEN pen,oldpen;
	char buf[40];
	int i,col,row,cw,ch,saved;
	int first,ncells,k;
	float total,bandmax;

	if (displayWnd==NULL) return;

	GetClientRect(displayWnd,&rc);
	if ((rc.right<1)||(rc.bottom<1)) return;

	hdc=BeginPaint(displayWnd,&ps);

	memdc=CreateCompatibleDC(hdc);
	membmp=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
	oldbmp=(HBITMAP)SelectObject(memdc,membmp);

	bkbrush=CreateSolidBrush(panelbk);
	FillRect(memdc,&rc,bkbrush);
	DeleteObject(bkbrush);

	// (the frame around the panel is drawn by the window itself - see
	//  PaintDisplayFrame() in darkmode.cpp - a second one inside the client
	//  area only produced a doubled border)

	if (columns<1) columns=1;
	if (columns>BP_PANELCELLS) columns=BP_PANELCELLS;

	// only draw the cells of this panel (allows splitting the 14 cells into two
	// windows, e.g. the 7 bands on the left and the 7 ratios on the right)
	first=panelfirst; ncells=panelcells;
	if ((first<0) || (first>BP_PANELCELLS-1)) first=0;
	if ((ncells<1) || (first+ncells>BP_PANELCELLS)) { first=0; ncells=BP_PANELCELLS; }

	panelrows=(ncells+columns-1)/columns;
	if (panelrows<1) panelrows=1;

	cw=rc.right/columns;   if (cw<1) cw=1;
	ch=rc.bottom/panelrows; if (ch<1) ch=1;

	total=avgpow[0]+avgpow[1]+avgpow[2]+avgpow[3]+avgpow[4];

	SetBkMode(memdc,TRANSPARENT);

	// largest band of this frame: the level bars are scaled to it so that even
	// small amplitudes stay visible
	bandmax=0.0f;
	for (i=0;i<BP_BANDS;i++)
	{
		float v=output_value(i,total);
		if (v>bandmax) bandmax=v;
	}

	pen=CreatePen(PS_SOLID,1,RGB(96,54,100));
	oldpen=(HPEN)SelectObject(memdc,pen);

	for (k=0;k<ncells;k++)
	{
		i=first+k;
		col=k%columns;
		row=k/columns;

		cell.left=rc.left+col*cw;
		cell.top=rc.top+row*ch;
		cell.right=cell.left+cw;
		cell.bottom=cell.top+ch;

		MoveToEx(memdc,cell.left,cell.bottom-1,NULL);
		LineTo(memdc,cell.right-1,cell.bottom-1);
		MoveToEx(memdc,cell.right-1,cell.top,NULL);
		LineTo(memdc,cell.right-1,cell.bottom-1);

		// ---- level bar at the bottom of the cell ----------------------
		{
			int   bh, by, bwmax, bw;
			float fv, fmax, frac;
			HBRUSH bb;
			RECT brc;

			bh = ch/6; if (bh<4) bh=4; if (bh>10) bh=10;
			by = cell.bottom-3-bh;
			bwmax = cell.right-cell.left-10;
			if (bwmax<4) bwmax=4;

			if (i<BP_BANDS) { fv=output_value(i,total); fmax=bandmax; }
			else            { fv=ratio_value(i-BP_BANDS); fmax=BP_RATIO_MAX; }

			frac = (fmax>1.0e-6f) ? (fv/fmax) : 0.0f;
			if (frac<0.0f) frac=0.0f;
			if (frac>1.0f) frac=1.0f;
			bw = (int)((float)bwmax*frac);
			if (bw<1) bw=1;

			brc.left   = cell.left+5;
			brc.right  = brc.left+bwmax;
			brc.top    = by;
			brc.bottom = by+bh;

			bb=CreateSolidBrush(RGB(28,6,28));
			FillRect(memdc,&brc,bb);
			DeleteObject(bb);

			brc.right = brc.left+bw;
			bb=CreateSolidBrush((highlight & (1<<i)) ? RGB(120,220,255)
			                                         : panelvalcol);
			FillRect(memdc,&brc,bb);
			DeleteObject(bb);
		}

		// ---- mark the cells that drive the training criteria ----------
		if (highlight & (1<<i))
		{
			HPEN hp=CreatePen(PS_SOLID,1,RGB(80,190,245));
			HPEN op=(HPEN)SelectObject(memdc,hp);
			HBRUSH nb=(HBRUSH)GetStockObject(NULL_BRUSH);
			HBRUSH ob=(HBRUSH)SelectObject(memdc,nb);

			Rectangle(memdc,cell.left+1,cell.top+1,cell.right-2,cell.bottom-2);

			SelectObject(memdc,ob);
			SelectObject(memdc,op);
			DeleteObject(hp);
		}

		cap=cell;
		cap.left+=6;
		cap.right-=4;
		cap.bottom=cell.top+(ch*28)/100;

		// clip to the cell so a large font cannot bleed into the neighbour cells
		saved=SaveDC(memdc);
		IntersectClipRect(memdc,cell.left,cell.top,cell.right,cell.bottom);

		SelectObject(memdc,captfont);
		SetTextColor(memdc,panelcaptcol);
		DrawText(memdc,panel_caption(i),-1,&cap,DT_SINGLELINE|DT_LEFT|DT_VCENTER|DT_END_ELLIPSIS);

		vr=cell;
		vr.top=cap.bottom;
		SelectObject(memdc,valfont);
		SetTextColor(memdc,(gate>0.5f) ? panelvalcol : RGB(255,90,90));
		panel_value_string(i,total,buf);
		DrawText(memdc,buf,-1,&vr,DT_SINGLELINE|DT_CENTER|DT_VCENTER);

		RestoreDC(memdc,saved);
	}

	SelectObject(memdc,oldpen);
	DeleteObject(pen);

	BitBlt(hdc,0,0,rc.right,rc.bottom,memdc,0,0,SRCCOPY);

	SelectObject(memdc,oldbmp);
	DeleteObject(membmp);
	DeleteDC(memdc);

	EndPaint(displayWnd,&ps);
}


LRESULT CALLBACK BandPanelWndHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int t;
	BANDPOWEROBJ * st;

	st=NULL;
	for (t=0;(t<GLOBAL.objects)&&(st==NULL);t++)
		if (objects[t]->type==OB_BANDPOWER)
		{
			st=(BANDPOWEROBJ *)objects[t];
			if (st->displayWnd!=hWnd) st=NULL;
		}

	if (st==NULL) return DefWindowProc(hWnd,message,wParam,lParam);

	switch( message )
	{
		case WM_KEYDOWN:
			SendMessage(ghWndMain,message,wParam,lParam);
			break;

		case WM_MOUSEACTIVATE:
			close_toolbox();
			actobject=st;
			SetWindowPos(hWnd,HWND_TOP,0,0,0,0,SWP_DRAWFRAME|SWP_NOMOVE|SWP_NOSIZE);
			InvalidateRect(ghWndDesign,NULL,TRUE);
			break;

		case WM_SIZE:
		case WM_MOVE:
			{
				WINDOWPLACEMENT wndpl;
				GetWindowPlacement(st->displayWnd,&wndpl);

				if (GLOBAL.locksession)
				{
					wndpl.rcNormalPosition.top=st->top;
					wndpl.rcNormalPosition.left=st->left;
					wndpl.rcNormalPosition.right=st->right;
					wndpl.rcNormalPosition.bottom=st->bottom;
					SetWindowPlacement(st->displayWnd,&wndpl);
					SetWindowLong(st->displayWnd, GWL_STYLE, GetWindowLong(st->displayWnd, GWL_STYLE)&~WS_SIZEBOX);
				}
				else
				{
					st->top=wndpl.rcNormalPosition.top;
					st->left=wndpl.rcNormalPosition.left;
					st->right=wndpl.rcNormalPosition.right;
					st->bottom=wndpl.rcNormalPosition.bottom;
					SetWindowLong(st->displayWnd, GWL_STYLE, GetWindowLong(st->displayWnd, GWL_STYLE) | WS_SIZEBOX);
				}
				InvalidateRect(hWnd,NULL,TRUE);
			}
			break;

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
			st->draw_panel();
			break;

		default:
			return DefWindowProc(hWnd,message,wParam,lParam);
	}
	return 0;
}


/* -------------------------------------------------------------------------
   toolbox window
   ------------------------------------------------------------------------- */

static void bp_setf(HWND hDlg, int idc, float value)
{
	char tmp[40];

	sprintf(tmp,"%.2f",value);
	SetDlgItemText(hDlg,idc,tmp);
}

static void bp_seti(HWND hDlg, int idc, int value)
{
	char tmp[40];

	sprintf(tmp,"%d",value);
	SetDlgItemText(hDlg,idc,tmp);
}

static float bp_getf(HWND hDlg, int idc, float fallback)
{
	char tmp[40];
	float v;

	GetDlgItemText(hDlg,idc,tmp,40);
	if (sscanf(tmp,"%f",&v)!=1) return(fallback);
	return(v);
}

static int bp_geti(HWND hDlg, int idc, int fallback)
{
	char tmp[40];
	int v;

	GetDlgItemText(hDlg,idc,tmp,40);
	if (sscanf(tmp,"%d",&v)!=1) return(fallback);
	return(v);
}

// show which power of two resulted from the requested window length
static void bp_update_fftinfo(HWND hDlg, BANDPOWEROBJ * st)
{
	char tmp[60];
	double df=(st->fftsize>0) ? (double)PACKETSPERSECOND/(double)st->fftsize : 0.0;

	sprintf(tmp,"%d samples, %.3f Hz",st->fftsize,df);
	SetDlgItemText(hDlg,IDC_BP_FFTINFO,tmp);
}


void BANDPOWEROBJ::update_dialog_values(float total)
{
	char tmp[40];

	if (hDlg==NULL) return;

	// ---- the band powers
	sprintf(tmp,"%.1f",output_value(0,total)); SetDlgItemText(hDlg,IDC_BP_V_DELTA,tmp);
	sprintf(tmp,"%.1f",output_value(1,total)); SetDlgItemText(hDlg,IDC_BP_V_THETA,tmp);
	sprintf(tmp,"%.1f",output_value(2,total)); SetDlgItemText(hDlg,IDC_BP_V_ALPHA,tmp);
	sprintf(tmp,"%.1f",output_value(3,total)); SetDlgItemText(hDlg,IDC_BP_V_BETA,tmp);
	sprintf(tmp,"%.1f",output_value(4,total)); SetDlgItemText(hDlg,IDC_BP_V_HBETA,tmp);
	sprintf(tmp,"%.1f",output_value(5,total)); SetDlgItemText(hDlg,IDC_BP_V_SMR,tmp);
	sprintf(tmp,"%.1f",output_value(6,total)); SetDlgItemText(hDlg,IDC_BP_V_BETA2,tmp);

	// ---- the band ratios
	sprintf(tmp,"%.3f",ratio_value(0)); SetDlgItemText(hDlg,IDC_BP_V_DT,tmp);
	sprintf(tmp,"%.3f",ratio_value(1)); SetDlgItemText(hDlg,IDC_BP_V_TBR,tmp);
	sprintf(tmp,"%.3f",ratio_value(2)); SetDlgItemText(hDlg,IDC_BP_V_HBB,tmp);
	sprintf(tmp,"%.3f",ratio_value(3)); SetDlgItemText(hDlg,IDC_BP_V_TSMR,tmp);
	sprintf(tmp,"%.3f",ratio_value(4)); SetDlgItemText(hDlg,IDC_BP_V_TB2,tmp);
	sprintf(tmp,"%.3f",ratio_value(5)); SetDlgItemText(hDlg,IDC_BP_V_TAR,tmp);
	sprintf(tmp,"%.3f",ratio_value(6)); SetDlgItemText(hDlg,IDC_BP_V_AHB,tmp);
}


LRESULT CALLBACK BandpowerDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	BANDPOWEROBJ * st;
	float total;
	int t;
	char tmp[40];

	st = (BANDPOWEROBJ *) actobject;
	if ((st==NULL)||(st->type!=OB_BANDPOWER)) return(FALSE);

	switch( message )
	{
		case WM_INITDIALOG:
			bp_setf(hDlg,IDC_BP_FFTSECS,st->fft_seconds);
			bp_update_fftinfo(hDlg,st);

			SendMessage(GetDlgItem(hDlg,IDC_BP_WINDOW),CB_ADDSTRING,0,(LPARAM)(LPSTR)"rectangular");
			SendMessage(GetDlgItem(hDlg,IDC_BP_WINDOW),CB_ADDSTRING,0,(LPARAM)(LPSTR)"hanning");
			SendMessage(GetDlgItem(hDlg,IDC_BP_WINDOW),CB_SETCURSEL,st->window,0);

			SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_ADDSTRING,0,(LPARAM)(LPSTR)"uV^2");
			SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_ADDSTRING,0,(LPARAM)(LPSTR)"dB");
			SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_ADDSTRING,0,(LPARAM)(LPSTR)"relative %");
			SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_ADDSTRING,0,(LPARAM)(LPSTR)"uV (amplitude)");
			SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_SETCURSEL,st->units,0);

			bp_seti(hDlg,IDC_BP_HOPMS,st->hop_ms);
			bp_setf(hDlg,IDC_BP_SMOOTH,st->smooth);
			bp_setf(hDlg,IDC_BP_NOTCHF,st->notchfreq);
			bp_setf(hDlg,IDC_BP_NOTCHQ,st->notchq);
			bp_setf(hDlg,IDC_BP_LP,st->lpcutoff);

			bp_setf(hDlg,IDC_BP_DELTA_LO,st->band_lo[0]); bp_setf(hDlg,IDC_BP_DELTA_HI,st->band_hi[0]);
			bp_setf(hDlg,IDC_BP_THETA_LO,st->band_lo[1]); bp_setf(hDlg,IDC_BP_THETA_HI,st->band_hi[1]);
			bp_setf(hDlg,IDC_BP_ALPHA_LO,st->band_lo[2]); bp_setf(hDlg,IDC_BP_ALPHA_HI,st->band_hi[2]);
			bp_setf(hDlg,IDC_BP_BETA_LO, st->band_lo[3]); bp_setf(hDlg,IDC_BP_BETA_HI, st->band_hi[3]);
			bp_setf(hDlg,IDC_BP_HBETA_LO,st->band_lo[4]); bp_setf(hDlg,IDC_BP_HBETA_HI,st->band_hi[4]);
			bp_setf(hDlg,IDC_BP_SMR_LO,  st->band_lo[5]); bp_setf(hDlg,IDC_BP_SMR_HI,  st->band_hi[5]);
			bp_setf(hDlg,IDC_BP_BETA2_LO,st->band_lo[6]); bp_setf(hDlg,IDC_BP_BETA2_HI,st->band_hi[6]);

			// the labels carry the greek symbols of the design; the .rc only
			// holds ASCII placeholders, because it is compiled in the code page
			// of the build machine
			for (t=0;t<BP_BANDS;t++)
			{
				bp_ansi(bp_cellcaption[t],tmp,40);
				SetDlgItemText(hDlg,IDC_BP_LB_B0+t,tmp);
				SetDlgItemText(hDlg,IDC_BP_LB_V0+t,tmp);
			}
			for (t=0;t<BP_RATIOS;t++)
			{
				bp_ansi(bp_cellcaption[BP_BANDS+t],tmp,40);
				SetDlgItemText(hDlg,IDC_BP_LB_V7+t,tmp);
			}

			CheckDlgButton(hDlg,IDC_BP_SELFTEST,st->selftest ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(hDlg,IDC_BP_LOGVALUES,st->logvalues ? BST_CHECKED : BST_UNCHECKED);

			CheckDlgButton(hDlg,IDC_BP_SHOWPANEL,st->showpanel ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(hDlg,IDC_BP_SHOWUNITS,st->showunits ? BST_CHECKED : BST_UNCHECKED);
			bp_seti(hDlg,IDC_BP_COLUMNS,st->columns);
			bp_seti(hDlg,IDC_BP_PANELSIZE,st->panelsize);
			color_button(GetDlgItem(hDlg,IDC_BP_VALCOL),st->panelvalcol);
			color_button(GetDlgItem(hDlg,IDC_BP_BKCOL),st->panelbk);
			color_button(GetDlgItem(hDlg,IDC_BP_CAPTCOL),st->panelcaptcol);

			total=0.0f;
			for (t=0;t<BP_BANDS;t++) total+=st->avgpow[t];
			st->update_dialog_values(total);
			return TRUE;

		case WM_CLOSE:
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_BP_FFTSECS:
					st->fft_seconds=bp_getf(hDlg,IDC_BP_FFTSECS,st->fft_seconds);
					if (st->fft_seconds<0.25f) st->fft_seconds=0.25f;
					if (st->fft_seconds>20.0f) st->fft_seconds=20.0f;
					st->last_fs=0;		// rebuild spectrum settings
					st->update_filters();
					st->reset_dsp();
					bp_update_fftinfo(hDlg,st);
					break;
				case IDC_BP_WINDOW:
					if (HIWORD(wParam)==CBN_SELCHANGE)
					{
						st->window=(int)SendMessage(GetDlgItem(hDlg,IDC_BP_WINDOW),CB_GETCURSEL,0,0);
						st->reset_dsp();
					}
					break;
				case IDC_BP_UNITS:
					if (HIWORD(wParam)==CBN_SELCHANGE)
					{
						st->units=(int)SendMessage(GetDlgItem(hDlg,IDC_BP_UNITS),CB_GETCURSEL,0,0);
						st->update_outport_ranges();
						InvalidateRect(ghWndDesign,NULL,TRUE);
					}
					break;
				case IDC_BP_HOPMS:
					st->hop_ms=bp_geti(hDlg,IDC_BP_HOPMS,st->hop_ms);
					if (st->hop_ms<10) st->hop_ms=10;
					if (st->hop_ms>5000) st->hop_ms=5000;
					st->last_fs=0;
					st->update_filters();
					break;
				case IDC_BP_SMOOTH:
					st->smooth=bp_getf(hDlg,IDC_BP_SMOOTH,st->smooth);
					if (st->smooth<0.0f) st->smooth=0.0f;
					if (st->smooth>60.0f) st->smooth=60.0f;
					break;
				case IDC_BP_NOTCHF:
					st->notchfreq=bp_getf(hDlg,IDC_BP_NOTCHF,st->notchfreq);
					if (st->notchfreq<0.0f) st->notchfreq=0.0f;
					st->update_filters();
					break;
				case IDC_BP_NOTCHQ:
					st->notchq=bp_getf(hDlg,IDC_BP_NOTCHQ,st->notchq);
					if (st->notchq<0.5f) st->notchq=0.5f;
					if (st->notchq>200.0f) st->notchq=200.0f;
					st->update_filters();
					break;
				case IDC_BP_LP:
					st->lpcutoff=bp_getf(hDlg,IDC_BP_LP,st->lpcutoff);
					st->last_fs=0;
					st->update_filters();
					break;
				case IDC_BP_SELFTEST:
					st->selftest=IsDlgButtonChecked(hDlg,IDC_BP_SELFTEST);
					st->reset_dsp();
					break;
				case IDC_BP_LOGVALUES:
					st->logvalues=IsDlgButtonChecked(hDlg,IDC_BP_LOGVALUES);
					break;

				case IDC_BP_SHOWPANEL:
					st->showpanel=IsDlgButtonChecked(hDlg,IDC_BP_SHOWPANEL);
					if (st->displayWnd!=NULL)
						ShowWindow(st->displayWnd, st->showpanel ? SW_SHOW : SW_HIDE);
					break;
				case IDC_BP_COLUMNS:
					st->columns=bp_geti(hDlg,IDC_BP_COLUMNS,st->columns);
					if (st->columns<1) st->columns=1;
					if (st->columns>BP_PANELCELLS) st->columns=BP_PANELCELLS;
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;
				case IDC_BP_PANELSIZE:
					st->panelsize=bp_geti(hDlg,IDC_BP_PANELSIZE,st->panelsize);
					if (st->panelsize<8) st->panelsize=8;
					if (st->panelsize>200) st->panelsize=200;
					st->update_panel_fonts();
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;
				case IDC_BP_SHOWUNITS:
					st->showunits=IsDlgButtonChecked(hDlg,IDC_BP_SHOWUNITS);
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;
				case IDC_BP_VALCOL:
					st->panelvalcol=select_color(hDlg,st->panelvalcol);
					color_button(GetDlgItem(hDlg,IDC_BP_VALCOL),st->panelvalcol);
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;
				case IDC_BP_BKCOL:
					st->panelbk=select_color(hDlg,st->panelbk);
					color_button(GetDlgItem(hDlg,IDC_BP_BKCOL),st->panelbk);
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;
				case IDC_BP_CAPTCOL:
					st->panelcaptcol=select_color(hDlg,st->panelcaptcol);
					color_button(GetDlgItem(hDlg,IDC_BP_CAPTCOL),st->panelcaptcol);
					if (st->displayWnd!=NULL) InvalidateRect(st->displayWnd,NULL,TRUE);
					break;

				case IDC_BP_DELTA_LO: st->band_lo[0]=bp_getf(hDlg,IDC_BP_DELTA_LO,st->band_lo[0]); break;
				case IDC_BP_DELTA_HI: st->band_hi[0]=bp_getf(hDlg,IDC_BP_DELTA_HI,st->band_hi[0]); break;
				case IDC_BP_THETA_LO: st->band_lo[1]=bp_getf(hDlg,IDC_BP_THETA_LO,st->band_lo[1]); break;
				case IDC_BP_THETA_HI: st->band_hi[1]=bp_getf(hDlg,IDC_BP_THETA_HI,st->band_hi[1]); break;
				case IDC_BP_ALPHA_LO: st->band_lo[2]=bp_getf(hDlg,IDC_BP_ALPHA_LO,st->band_lo[2]); break;
				case IDC_BP_ALPHA_HI: st->band_hi[2]=bp_getf(hDlg,IDC_BP_ALPHA_HI,st->band_hi[2]); break;
				case IDC_BP_BETA_LO:  st->band_lo[3]=bp_getf(hDlg,IDC_BP_BETA_LO, st->band_lo[3]); break;
				case IDC_BP_BETA_HI:  st->band_hi[3]=bp_getf(hDlg,IDC_BP_BETA_HI, st->band_hi[3]); break;
				case IDC_BP_HBETA_LO: st->band_lo[4]=bp_getf(hDlg,IDC_BP_HBETA_LO,st->band_lo[4]); break;
				case IDC_BP_HBETA_HI: st->band_hi[4]=bp_getf(hDlg,IDC_BP_HBETA_HI,st->band_hi[4]); break;
				case IDC_BP_SMR_LO:   st->band_lo[5]=bp_getf(hDlg,IDC_BP_SMR_LO,  st->band_lo[5]); break;
				case IDC_BP_SMR_HI:   st->band_hi[5]=bp_getf(hDlg,IDC_BP_SMR_HI,  st->band_hi[5]); break;
				case IDC_BP_BETA2_LO: st->band_lo[6]=bp_getf(hDlg,IDC_BP_BETA2_LO,st->band_lo[6]); break;
				case IDC_BP_BETA2_HI: st->band_hi[6]=bp_getf(hDlg,IDC_BP_BETA2_HI,st->band_hi[6]); break;
			}
			st->update_outport_ranges();
			InvalidateRect(ghWndDesign,NULL,TRUE);
			return TRUE;

		case WM_MOVE:
		case WM_SIZE:
			update_toolbox_position(hDlg);
			break;

		case WM_PAINT:
			color_button(GetDlgItem(hDlg,IDC_BP_VALCOL),st->panelvalcol);
			color_button(GetDlgItem(hDlg,IDC_BP_BKCOL),st->panelbk);
			color_button(GetDlgItem(hDlg,IDC_BP_CAPTCOL),st->panelcaptcol);
			break;
	}
	return FALSE;
}
