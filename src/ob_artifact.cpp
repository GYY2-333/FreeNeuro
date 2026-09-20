/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_ARTIFACT.CPP:  functions for the Artifact / Signal-Quality object

  see ob_artifact.h for a description of the object and its output ports.

  ArtifactDlgHandler: processes the events of the settings window.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"
#include "ob_artifact.h"

#define AR_PI 3.14159265358979323846

// smoothing time constant of the artifact percentage (seconds)
#define AR_PCT_TAU   10.0
// smoothing time constant of the EMG power (seconds)
#define AR_EMG_TAU   0.1
// after this time without incoming samples the data is called invalid
#define AR_TIMEOUT_MS 200


static void ar_bq_reset(ARBIQUAD *f)
{
	f->z1=0.0; f->z2=0.0;
}

static double ar_bq_process(ARBIQUAD *f, double x)
{
	double y = f->b0*x + f->z1;
	f->z1 = f->b1*x - f->a1*y + f->z2;
	f->z2 = f->b2*x - f->a2*y;
	return(y);
}

// 2nd order Butterworth highpass (RBJ cookbook)
static void ar_bq_highpass(ARBIQUAD *f, double freq, double fs)
{
	double q = 0.70710678;
	double w0 = 2.0*AR_PI*freq/fs;
	double cw = cos(w0);
	double sw = sin(w0);
	double alpha = sw/(2.0*q);
	double a0 = 1.0 + alpha;

	f->b0 =  (1.0+cw)/2.0/a0;
	f->b1 = -(1.0+cw)/a0;
	f->b2 =  (1.0+cw)/2.0/a0;
	f->a1 = -2.0*cw/a0;
	f->a2 = (1.0-alpha)/a0;
	ar_bq_reset(f);
}


ARTIFACTOBJ::ARTIFACTOBJ(int num) : BASE_CL()
{
	width=90;

	outports=4;
	inports=1;

	strcpy(out_ports[0].out_name,"quality");
	strcpy(out_ports[0].out_desc,"1 = usable data, 0 = artifact");
	strcpy(out_ports[0].out_dim,"none");
	out_ports[0].get_range=-1;
	out_ports[0].out_min=0.0f;
	out_ports[0].out_max=1.0f;

	strcpy(out_ports[1].out_name,"artifact%");
	strcpy(out_ports[1].out_desc,"percentage of rejected data");
	strcpy(out_ports[1].out_dim,"%");
	out_ports[1].get_range=-1;
	out_ports[1].out_min=0.0f;
	out_ports[1].out_max=100.0f;

	strcpy(out_ports[2].out_name,"p-pamp");
	strcpy(out_ports[2].out_desc,"peak-to-peak amplitude");
	strcpy(out_ports[2].out_dim,"uV");
	out_ports[2].get_range=-1;
	out_ports[2].out_min=0.0f;
	out_ports[2].out_max=500.0f;

	strcpy(out_ports[3].out_name,"emg");
	strcpy(out_ports[3].out_desc,"high frequency (EMG) rms");
	strcpy(out_ports[3].out_dim,"uV");
	out_ports[3].get_range=-1;
	out_ports[3].out_min=0.0f;
	out_ports[3].out_max=100.0f;

	strcpy(in_ports[0].in_name,"signal");
	strcpy(in_ports[0].in_desc,"raw biosignal");
	strcpy(in_ports[0].in_dim,"uV");
	in_ports[0].get_range=1;
	in_ports[0].in_min=-500.0f;
	in_ports[0].in_max=500.0f;

	// defaults
	pp_threshold=150.0f;      // uV peak-to-peak
	block_ms=500;
	emg_enable=1;
	emg_threshold=12.0f;      // uV rms
	emg_cutoff=45.0f;
	clip_level=490.0f;        // ADC full scale of the PhysioAmp (+-500 uV)
	selftest=0;

	last_fs=0;
	update_filters();
	reset_dsp();
}


ARTIFACTOBJ::~ARTIFACTOBJ()
{
}


void ARTIFACTOBJ::reset_dsp(void)
{
	block_pos=0;
	block_min=0.0f;
	block_max=0.0f;
	block_clipped=0;
	samples_since_input=0;
	input_value=0.0f;
	quality=1.0f;
	artifact_pct=0.0f;
	pp_amp=0.0f;
	emg_rms=0.0f;
	emg_ema=0.0;
	testphase=0.0;
	blocks=0;

	ar_bq_reset(&emg_hp);
}


void ARTIFACTOBJ::update_filters(void)
{
	double fs=(double)PACKETSPERSECOND;
	double nyq=fs/2.0;
	double cut=(double)emg_cutoff;

	if (fs<2.0) fs=2.0;
	if (cut>nyq*0.9) cut=nyq*0.9;
	if (cut<1.0) cut=1.0;

	ar_bq_highpass(&emg_hp, cut, fs);

	block_len=(int)((double)block_ms*fs/1000.0);
	if (block_len<8) block_len=8;
	if (block_len>(int)fs*5) block_len=(int)fs*5;

	last_fs=PACKETSPERSECOND;
}


void ARTIFACTOBJ::incoming_data(int port, float value)
{
	input_value=value;
	samples_since_input=0;
}


void ARTIFACTOBJ::make_dialog(void)
{
	display_toolbox(hDlg=CreateDialog(hInst, (LPCTSTR)IDD_ARTIFACTBOX, ghWndStatusbox, (DLGPROC)ArtifactDlgHandler));
}


void ARTIFACTOBJ::session_start(void) { reset_dsp(); update_filters(); }
void ARTIFACTOBJ::session_reset(void) { reset_dsp(); }
void ARTIFACTOBJ::session_pos(long pos) { reset_dsp(); }


void ARTIFACTOBJ::load(HANDLE hFile)
{
	load_object_basics(this);

	load_property("ar_ppthreshold",P_FLOAT,&pp_threshold);
	load_property("ar_blockms",P_INT,&block_ms);
	load_property("ar_emgenable",P_INT,&emg_enable);
	load_property("ar_emgthreshold",P_FLOAT,&emg_threshold);
	load_property("ar_emgcutoff",P_FLOAT,&emg_cutoff);
	load_property("ar_cliplevel",P_FLOAT,&clip_level);
	load_property("ar_selftest",P_INT,&selftest);

	last_fs=0;
	update_filters();
	reset_dsp();
}


void ARTIFACTOBJ::save(HANDLE hFile)
{
	save_object_basics(hFile, this);

	save_property(hFile,"ar_ppthreshold",P_FLOAT,&pp_threshold);
	save_property(hFile,"ar_blockms",P_INT,&block_ms);
	save_property(hFile,"ar_emgenable",P_INT,&emg_enable);
	save_property(hFile,"ar_emgthreshold",P_FLOAT,&emg_threshold);
	save_property(hFile,"ar_emgcutoff",P_FLOAT,&emg_cutoff);
	save_property(hFile,"ar_cliplevel",P_FLOAT,&clip_level);
	save_property(hFile,"ar_selftest",P_INT,&selftest);
}


void ARTIFACTOBJ::work(void)
{
	double x, sq, alpha_s, dt;
	int artifact, timeout;

	if (PACKETSPERSECOND!=last_fs) update_filters();

	if (selftest)
	{
		// internal verification signal:
		//   10 Hz / 30 uV background, a 300 ms burst of 300 uV p-p every 3 s
		//   and a 60 Hz / 6 uV muscle component
		double t=(double)blocks;
		double tt=fmod((double)(blocks*block_len+block_pos)/(double)PACKETSPERSECOND,3.0);

		x = 30.0*sin(testphase) + 6.0*sin(testphase*6.0);
		if (tt<0.3) x += 150.0*sin(testphase*1.7);
		testphase += 2.0*AR_PI*10.0/(double)PACKETSPERSECOND;
		if (testphase>=2.0*AR_PI) testphase-=2.0*AR_PI;
		(void)t;
	}
	else
	{
		if (input_value==(float)INVALID_VALUE)
		{
			samples_since_input++;
		}
		else
		{
			x=(double)input_value;
			samples_since_input++;
		}
	}

	// ---- clipping
	if (fabs(x)>=fabs((double)clip_level)) block_clipped=1;

	// ---- peak-to-peak of the running block
	if (block_pos==0) { block_min=(float)x; block_max=(float)x; }
	else
	{
		if (x<(double)block_min) block_min=(float)x;
		if (x>(double)block_max) block_max=(float)x;
	}

	// ---- high frequency (EMG) power
	sq=ar_bq_process(&emg_hp,x);
	sq*=sq;
	dt=1.0/(double)PACKETSPERSECOND;
	alpha_s=1.0-exp(-dt/AR_EMG_TAU);
	if (alpha_s>1.0) alpha_s=1.0;
	emg_ema+=alpha_s*(sq-emg_ema);

	block_pos++;

	// ---- evaluate one block
	if (block_pos>=block_len)
	{
		block_pos=0;
		pp_amp=block_max-block_min;
		if (pp_amp<0.0f) pp_amp=0.0f;
		emg_rms=(float)sqrt(emg_ema>0.0 ? emg_ema : 0.0);

		timeout=0;
		if ((!selftest)&&(samples_since_input>((int)PACKETSPERSECOND*AR_TIMEOUT_MS/1000)))
			timeout=1;

		artifact=0;
		if (pp_amp>pp_threshold) artifact=1;
		if ((emg_enable)&&(emg_rms>emg_threshold)) artifact=1;
		if (block_clipped) artifact=1;
		if (timeout) artifact=1;

		quality=artifact ? 0.0f : 1.0f;

		dt=(double)block_len/(double)PACKETSPERSECOND;
		alpha_s=1.0-exp(-dt/AR_PCT_TAU);
		if (alpha_s>1.0) alpha_s=1.0;
		artifact_pct+=(float)(alpha_s*((artifact ? 100.0 : 0.0)-(double)artifact_pct));

		block_min=block_max=(float)x;
		block_clipped=0;
		blocks++;

		if (selftest)
			write_logfile("artifact selftest: pp=%.1f emg=%.2f quality=%.0f artifact=%.1f%%",
				pp_amp, emg_rms, quality, artifact_pct);
	}

	// ---- outputs
	pass_values(0,quality);
	pass_values(1,artifact_pct);
	pass_values(2,pp_amp);
	pass_values(3,emg_rms);

	if ((hDlg==ghWndToolbox)&&(!TIMING.dialog_update)) update_dialog_values();
}


/* -------------------------------------------------------------------------
   toolbox window
   ------------------------------------------------------------------------- */

static void ar_setf(HWND hDlg, int idc, float value)
{
	char tmp[40];

	sprintf(tmp,"%.2f",value);
	SetDlgItemText(hDlg,idc,tmp);
}

static void ar_seti(HWND hDlg, int idc, int value)
{
	char tmp[40];

	sprintf(tmp,"%d",value);
	SetDlgItemText(hDlg,idc,tmp);
}

static float ar_getf(HWND hDlg, int idc, float fallback)
{
	char tmp[40];
	float v;

	GetDlgItemText(hDlg,idc,tmp,40);
	if (sscanf(tmp,"%f",&v)!=1) return(fallback);
	return(v);
}

static int ar_geti(HWND hDlg, int idc, int fallback)
{
	char tmp[40];
	int v;

	GetDlgItemText(hDlg,idc,tmp,40);
	if (sscanf(tmp,"%d",&v)!=1) return(fallback);
	return(v);
}


void ARTIFACTOBJ::update_dialog_values(void)
{
	char tmp[40];

	if (hDlg==NULL) return;

	sprintf(tmp,"%.0f",quality);     SetDlgItemText(hDlg,IDC_AR_V_QUALITY,tmp);
	sprintf(tmp,"%.1f",artifact_pct);SetDlgItemText(hDlg,IDC_AR_V_ARTIFACT,tmp);
	sprintf(tmp,"%.1f",pp_amp);      SetDlgItemText(hDlg,IDC_AR_V_PPAMP,tmp);
	sprintf(tmp,"%.2f",emg_rms);     SetDlgItemText(hDlg,IDC_AR_V_EMG,tmp);
}


LRESULT CALLBACK ArtifactDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	ARTIFACTOBJ * st;

	st = (ARTIFACTOBJ *) actobject;
	if ((st==NULL)||(st->type!=OB_ARTIFACT)) return(FALSE);

	switch( message )
	{
		case WM_INITDIALOG:
			ar_setf(hDlg,IDC_AR_PPTHRESHOLD,st->pp_threshold);
			ar_seti(hDlg,IDC_AR_BLOCKMS,st->block_ms);
			ar_setf(hDlg,IDC_AR_EMGTHRESHOLD,st->emg_threshold);
			ar_setf(hDlg,IDC_AR_EMGCUTOFF,st->emg_cutoff);
			ar_setf(hDlg,IDC_AR_CLIPLEVEL,st->clip_level);
			CheckDlgButton(hDlg,IDC_AR_EMGENABLE,st->emg_enable ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(hDlg,IDC_AR_SELFTEST,st->selftest ? BST_CHECKED : BST_UNCHECKED);
			st->update_dialog_values();
			return TRUE;

		case WM_CLOSE:
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_AR_PPTHRESHOLD:
					st->pp_threshold=ar_getf(hDlg,IDC_AR_PPTHRESHOLD,st->pp_threshold);
					if (st->pp_threshold<1.0f) st->pp_threshold=1.0f;
					if (st->pp_threshold>5000.0f) st->pp_threshold=5000.0f;
					break;
				case IDC_AR_BLOCKMS:
					st->block_ms=ar_geti(hDlg,IDC_AR_BLOCKMS,st->block_ms);
					if (st->block_ms<50) st->block_ms=50;
					if (st->block_ms>5000) st->block_ms=5000;
					st->last_fs=0;
					st->update_filters();
					break;
				case IDC_AR_EMGTHRESHOLD:
					st->emg_threshold=ar_getf(hDlg,IDC_AR_EMGTHRESHOLD,st->emg_threshold);
					if (st->emg_threshold<0.0f) st->emg_threshold=0.0f;
					break;
				case IDC_AR_EMGCUTOFF:
					st->emg_cutoff=ar_getf(hDlg,IDC_AR_EMGCUTOFF,st->emg_cutoff);
					st->last_fs=0;
					st->update_filters();
					break;
				case IDC_AR_CLIPLEVEL:
					st->clip_level=ar_getf(hDlg,IDC_AR_CLIPLEVEL,st->clip_level);
					if (st->clip_level<1.0f) st->clip_level=1.0f;
					break;
				case IDC_AR_EMGENABLE:
					st->emg_enable=IsDlgButtonChecked(hDlg,IDC_AR_EMGENABLE);
					break;
				case IDC_AR_SELFTEST:
					st->selftest=IsDlgButtonChecked(hDlg,IDC_AR_SELFTEST);
					st->reset_dsp();
					break;
			}
			return TRUE;

		case WM_MOVE:
		case WM_SIZE:
			update_toolbox_position(hDlg);
			break;
	}
	return FALSE;
}
