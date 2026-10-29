/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org
  
  OB_THRESHOLD.H:  contains the THRESHOLD-Object
  the object's propertries are declared and the
  constructor-, make_dialog-, load-, save-, incoming_data-, work-, and destructor-
  methods are implemented here
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.
-----------------------------------------------------------------------------*/


#include "brainBay.h"

// moving-average window of the threshold object, in samples. 9000 covers 3 s at
// the 3000 Hz of the PhysioAmp, which is what the BioExplorer designs use for
// their 'AveragePeriod' (1 s for attention/distraction, 2 s for tension).
// (named THRESHOLD_ACCULEN because ob_sessionmanager.h already uses ACCULEN)
#define THRESHOLD_ACCULEN 9000

#define THRESHOLD_ADAPTMODE_NONE 0
#define THRESHOLD_ADAPTMODE_RANGE 1
#define THRESHOLD_ADAPTMODE_QUANTILE 2
#define THRESHOLD_ADAPTMODE_AVERAGE 3


class THRESHOLDOBJ : public BASE_CL
{
  protected: 
	DWORD dwRead,dwWritten;

  public: 
	float input;

	float current_value;
	float last_risingTest;
	int  last_value;
	float accu[THRESHOLD_ACCULEN];
	int  accupos;
	int buckets[1025];
    int adapt_num;
	int  play_interval;
	int  interval_len;
	int  signal_gain;
	float lower_limit;
	float upper_limit;
	float threshold_avg_sum;
	float interval_sum;
	float range_min,range_max;
	float peak;					// peak hold level of the bar
	DWORD peaktick;				// GetTickCount() when the peak was last updated
	int   decayms;				// ms the peak needs to fall over the full range
	int  op;
	int  showmeter;
	int  rising,falling;
	int  baseline;
	int  firstadapt;
	int  adapt_lower_limit,adapt_upper_limit;
	int  adapt_lower_mode,adapt_upper_mode;
	int  adapt_interval;
	int old_y1,old_y2;
	int barsize,fontsize;
	int redraw;
	DWORD last_data_tick;			// GetTickCount() of the last sample that arrived
	COLORREF color,bkcolor, fontcolor, fontbkcolor;
	int  top,left,right,bottom;
	char wndcaption[50];
	HFONT font;
	HFONT valfont;				// big bold font for the current value in the header
	float numericTrueValue, numericFalseValue;
	int trueMode;
	int falseMode;
	int dragging;          // the level line is being dragged in the meter window
	int drag_upper;        // 0 = lower_limit, 1 = upper_limit


    THRESHOLDOBJ(int num);
    float get_quantile(int number_of_values); 
  	void empty_buckets(void);
    void clear_averagers();

	void session_start(void);
	void session_reset(void);
	void session_pos(long pos);

	void make_dialog(void);
	void load(HANDLE hFile);
	void save(HANDLE hFile);
	void incoming_data(int port, float value);
	void update_inports(void);
	void work(void);
    ~THRESHOLDOBJ();

};
