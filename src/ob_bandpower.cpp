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

// names of the frequency bands, in the order of the output ports
static const char *bp_bandname[BP_BANDS] = { "delta", "theta", "alpha", "beta", "gamma" };

#define BP_PI 3.14159265358979323846


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
	outports=9;
	inports=2;

	strcpy(out_ports[0].out_name,"delta");
	strcpy(out_ports[0].out_desc,"Delta power (1 - 4 Hz)");
	strcpy(out_ports[1].out_name,"theta");
	strcpy(out_ports[1].out_desc,"Theta power (4 - 8 Hz)");
	strcpy(out_ports[2].out_name,"alpha");
	strcpy(out_ports[2].out_desc,"Alpha power (8 - 13 Hz)");
	strcpy(out_ports[3].out_name,"beta");
	strcpy(out_ports[3].out_desc,"Beta power (13 - 30 Hz)");
	strcpy(out_ports[4].out_name,"gamma");
	strcpy(out_ports[4].out_desc,"Gamma power (30 - 45 Hz)");
	strcpy(out_ports[5].out_name,"th/beta");
	strcpy(out_ports[5].out_desc,"Theta/Beta ratio (TBR)");
	strcpy(out_ports[6].out_name,"th/alpha");
	strcpy(out_ports[6].out_desc,"Theta/Alpha ratio");
	strcpy(out_ports[7].out_name,"slow/fast");
	strcpy(out_ports[7].out_desc,"(delta+theta)/(alpha+beta)");
	strcpy(out_ports[8].out_name,"IAF");
	strcpy(out_ports[8].out_desc,"Individual alpha peak frequency");

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

	band_lo[0]=1.0f;   band_hi[0]=4.0f;     // delta
	band_lo[1]=4.0f;   band_hi[1]=8.0f;     // theta
	band_lo[2]=8.0f;   band_hi[2]=13.0f;    // alpha
	band_lo[3]=13.0f;  band_hi[3]=30.0f;    // beta
	band_lo[4]=30.0f;  band_hi[4]=45.0f;    // gamma

	fftsize=(int)((double)fft_seconds*(double)PACKETSPERSECOND);
	hop=0;
	last_fs=0;
	updates=0;
	gate=1.0f;
	iaf=10.0f;

	update_outport_ranges();
	reset_dsp();
	update_filters();
}


BANDPOWEROBJ::~BANDPOWEROBJ()
{
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

	for (i=0;i<BP_BANDS;i++)
	{
		if (units==1)                                 // dB
		{
			strcpy(out_ports[i].out_dim,"dB");
			out_ports[i].out_min=-20.0f;
			out_ports[i].out_max=60.0f;
		}
		else if (units==2)                            // relative power
		{
			strcpy(out_ports[i].out_dim,"%");
			out_ports[i].out_min=0.0f;
			out_ports[i].out_max=100.0f;
		}
		else                                          // uV^2
		{
			strcpy(out_ports[i].out_dim,"uV^2");
			out_ports[i].out_min=0.0f;
			out_ports[i].out_max=1000.0f;
		}
	}

	for (i=BP_BANDS;i<8;i++)
	{
		strcpy(out_ports[i].out_dim,"ratio");
		out_ports[i].out_min=0.0f;
		out_ports[i].out_max=10.0f;
	}

	strcpy(out_ports[8].out_dim,"Hz");
	out_ports[8].out_min=4.0f;
	out_ports[8].out_max=20.0f;
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
	return(avgpow[band]);
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
	double sum,wfac,peak,pw,alpha_s,dt;
	int b,k,k0,k1,kmax;
	float total;

	if (PACKETSPERSECOND!=last_fs) update_filters();

	if (selftest)
	{
		// internal verification signal: 6 Hz / 20 uV (theta) + 20 Hz / 10 uV (beta)
		x = 20.0*sin(testphase1) + 10.0*sin(testphase2);
		testphase1 += 2.0*BP_PI*6.0/(double)PACKETSPERSECOND;
		testphase2 += 2.0*BP_PI*20.0/(double)PACKETSPERSECOND;
		if (testphase1>=2.0*BP_PI) testphase1-=2.0*BP_PI;
		if (testphase2>=2.0*BP_PI) testphase2-=2.0*BP_PI;
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

		// individual alpha peak frequency (parabolic interpolation)
		k0=(int)ceil((double)band_lo[2]*(double)fftsize/(double)PACKETSPERSECOND);
		k1=(int)floor((double)band_hi[2]*(double)fftsize/(double)PACKETSPERSECOND);
		if (k0<1) k0=1;
		if (k1>(fftsize>>1)-2) k1=(fftsize>>1)-2;
		if (k1<k0) k1=k0;
		kmax=k0; peak=-1.0;
		for (k=k0;k<=k1;k++)
			if ((double)fftbands[k]>peak) { peak=(double)fftbands[k]; kmax=k; }

		pw=(double)kmax*(double)PACKETSPERSECOND/(double)fftsize;
		if ((kmax>k0)&&(kmax<k1)&&(peak>0.0))
		{
			double pa=(double)fftbands[kmax-1];
			double pb=(double)fftbands[kmax];
			double pc=(double)fftbands[kmax+1];
			double den=pa-2.0*pb+pc;
			if (den<0.0)
				pw += 0.5*(pa-pc)/den*(double)PACKETSPERSECOND/(double)fftsize;
		}

		// ---- smoothing
		dt=(double)hop/(double)PACKETSPERSECOND;
		alpha_s=(smooth>0.01f) ? (1.0-exp(-dt/(double)smooth)) : 1.0;
		if (alpha_s>1.0) alpha_s=1.0;
		if (alpha_s<0.0) alpha_s=0.0;

		if (updates<1)
		{
			for (b=0;b<BP_BANDS;b++) avgpow[b]=rawpow[b];
			iaf=(float)pw;
		}
		else if (gate>0.5f)
		{
			for (b=0;b<BP_BANDS;b++)
				avgpow[b]+=(float)(alpha_s*((double)rawpow[b]-(double)avgpow[b]));
			iaf+=(float)(alpha_s*(pw-(double)iaf));
		}

		updates++;
		if (updates>2000000000) updates=2000000000;

		if (((selftest)||(logvalues))&&((updates%10)==0))
			write_logfile("bandpower: delta=%.1f theta=%.1f alpha=%.1f beta=%.1f gamma=%.1f theta/beta=%.3f iaf=%.2f",
				avgpow[0], avgpow[1], avgpow[2], avgpow[3], avgpow[4],
				(avgpow[3]>0.0f) ? (avgpow[1]/avgpow[3]) : 0.0f, iaf);
	}

	// ---- outputs
	total=avgpow[0]+avgpow[1]+avgpow[2]+avgpow[3]+avgpow[4];

	for (b=0;b<BP_BANDS;b++) pass_values(b, output_value(b,total));

	pass_values(5,(avgpow[3]>0.0f) ? (avgpow[1]/avgpow[3]) : 0.0f);                  // theta/beta
	pass_values(6,(avgpow[2]>0.0f) ? (avgpow[1]/avgpow[2]) : 0.0f);                  // theta/alpha
	pass_values(7,((avgpow[2]+avgpow[3])>0.0f)
		? ((avgpow[0]+avgpow[1])/(avgpow[2]+avgpow[3])) : 0.0f);                     // slow/fast
	pass_values(8,iaf);                                                              // IAF

	if ((hDlg==ghWndToolbox)&&(!TIMING.dialog_update)) update_dialog_values(total);
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

	sprintf(tmp,"%.1f",output_value(0,total)); SetDlgItemText(hDlg,IDC_BP_V_DELTA,tmp);
	sprintf(tmp,"%.1f",output_value(1,total)); SetDlgItemText(hDlg,IDC_BP_V_THETA,tmp);
	sprintf(tmp,"%.1f",output_value(2,total)); SetDlgItemText(hDlg,IDC_BP_V_ALPHA,tmp);
	sprintf(tmp,"%.1f",output_value(3,total)); SetDlgItemText(hDlg,IDC_BP_V_BETA,tmp);
	sprintf(tmp,"%.1f",output_value(4,total)); SetDlgItemText(hDlg,IDC_BP_V_GAMMA,tmp);
	sprintf(tmp,"%.3f",(avgpow[3]>0.0f) ? (avgpow[1]/avgpow[3]) : 0.0f);
	SetDlgItemText(hDlg,IDC_BP_V_TBR,tmp);
	sprintf(tmp,"%.3f",(avgpow[2]>0.0f) ? (avgpow[1]/avgpow[2]) : 0.0f);
	SetDlgItemText(hDlg,IDC_BP_V_TAR,tmp);
	sprintf(tmp,"%.3f",((avgpow[2]+avgpow[3])>0.0f)
		? ((avgpow[0]+avgpow[1])/(avgpow[2]+avgpow[3])) : 0.0f);
	SetDlgItemText(hDlg,IDC_BP_V_SF,tmp);
	sprintf(tmp,"%.2f",iaf); SetDlgItemText(hDlg,IDC_BP_V_IAF,tmp);
}


LRESULT CALLBACK BandpowerDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	BANDPOWEROBJ * st;
	float total;

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
			bp_setf(hDlg,IDC_BP_GAMMA_LO,st->band_lo[4]); bp_setf(hDlg,IDC_BP_GAMMA_HI,st->band_hi[4]);

			CheckDlgButton(hDlg,IDC_BP_SELFTEST,st->selftest ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(hDlg,IDC_BP_LOGVALUES,st->logvalues ? BST_CHECKED : BST_UNCHECKED);

			total=st->avgpow[0]+st->avgpow[1]+st->avgpow[2]+st->avgpow[3]+st->avgpow[4];
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

				case IDC_BP_DELTA_LO: st->band_lo[0]=bp_getf(hDlg,IDC_BP_DELTA_LO,st->band_lo[0]); break;
				case IDC_BP_DELTA_HI: st->band_hi[0]=bp_getf(hDlg,IDC_BP_DELTA_HI,st->band_hi[0]); break;
				case IDC_BP_THETA_LO: st->band_lo[1]=bp_getf(hDlg,IDC_BP_THETA_LO,st->band_lo[1]); break;
				case IDC_BP_THETA_HI: st->band_hi[1]=bp_getf(hDlg,IDC_BP_THETA_HI,st->band_hi[1]); break;
				case IDC_BP_ALPHA_LO: st->band_lo[2]=bp_getf(hDlg,IDC_BP_ALPHA_LO,st->band_lo[2]); break;
				case IDC_BP_ALPHA_HI: st->band_hi[2]=bp_getf(hDlg,IDC_BP_ALPHA_HI,st->band_hi[2]); break;
				case IDC_BP_BETA_LO:  st->band_lo[3]=bp_getf(hDlg,IDC_BP_BETA_LO, st->band_lo[3]); break;
				case IDC_BP_BETA_HI:  st->band_hi[3]=bp_getf(hDlg,IDC_BP_BETA_HI, st->band_hi[3]); break;
				case IDC_BP_GAMMA_LO: st->band_lo[4]=bp_getf(hDlg,IDC_BP_GAMMA_LO,st->band_lo[4]); break;
				case IDC_BP_GAMMA_HI: st->band_hi[4]=bp_getf(hDlg,IDC_BP_GAMMA_HI,st->band_hi[4]); break;
			}
			return TRUE;

		case WM_MOVE:
		case WM_SIZE:
			update_toolbox_position(hDlg);
			break;
	}
	return FALSE;
}
