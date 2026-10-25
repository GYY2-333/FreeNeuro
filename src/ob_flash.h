/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_FLASH.H:  declarations for the "Flash / SWF" object

  The Flash object plays a Flash-movie (.swf) inside a child window of the main
  window and writes the values of its input ports into ActionScript variables of
  that movie. This is exactly the interface the BioExplorer game models use:
  the movie is loaded through "simpleAdapterLoader.swf" (which loads the real
  game named in simpleadapterloader.ini) and the game is steered by the
  variables in1, in2, in3 - one per threshold.

  Signal flow:
      input port 0..n   one value per variable, expected 0.0 - 1.0
      SWF variables     varnames[0..n-1]  (default in1, in2, in3)

  Threading (same rule as process_biosemi() / process_physioamp()):
      the ActiveX control may only be touched by the thread that created it,
      i.e. the UI thread that owns displayWnd. incoming_data() therefore only
      stores the value and posts a window message - all OLE calls happen in
      FlashWndHandler(). Never call IDispatch from the timer thread.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"

#define FLASH_MAXVAR     8          // input ports / SWF variables
#define FLASH_VARLEN     32         // max variable name length
#define FLASH_PATHLEN    512        // max swf path length

#define FLASH_QUALITY_LOW        0
#define FLASH_QUALITY_AUTOLOW    1
#define FLASH_QUALITY_AUTOHIGH   2
#define FLASH_QUALITY_MEDIUM     3
#define FLASH_QUALITY_HIGH       4
#define FLASH_QUALITY_BEST       5

#define FLASH_SCALE_SHOWALL      0
#define FLASH_SCALE_NOBORDER     1
#define FLASH_SCALE_EXACTFIT     2
#define FLASH_SCALE_NOSCALE      3

// messages sent to the flash window, all ActiveX work is done there
#define WM_FLASH_CMD      (WM_APP + 70)
#define FLASH_CMD_RELOAD  1
#define FLASH_CMD_PLAY    2
#define FLASH_CMD_STOP    3
#define FLASH_CMD_UPDATE  4

LRESULT CALLBACK FlashWndHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK FlashDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

class FLASHOBJ : public BASE_CL
{
  public:

	// ---- configuration ---------------------------------------------------
	char	swfpath[FLASH_PATHLEN];			// movie to load, empty = nothing
	int		channels;						// number of variables / input ports
	char	varnames[FLASH_MAXVAR][FLASH_VARLEN];
	char	wndcaption[50];
	int		autoplay;						// start the movie on session start
	int		loop_movie;
	int		showmenu;
	int		quality;						// FLASH_QUALITY_*
	int		scalemode;						// FLASH_SCALE_*
	int		top, left, right, bottom;		// window geometry

	// ---- NeuroFlasher game drive ------------------------------------------
	//  The bxd models of BioExplorer (e.g. 注意力拼图) do NOT feed the three
	//  thresholds into the loader. They feed
	//      input 1 = a score that grows while the training criteria are met
	//      input 2 = a free running "keep alive" clock
	//  Because In1In2InputModel only steps the movie when a value CHANGES, a
	//  constant threshold value freezes the game. In drive mode the object
	//  produces those two signals itself:
	//      In1  += 1  every 'scorems' while the gate port is open
	//      In2  += 1  every 'clockms' (always)
	//  All other input ports are passed through unchanged.
	int		drive;						// 1 = score + clock, 0 = raw values
	int		gateport;					// input port that opens the score gate
	int		scorems;					// milliseconds per score point
	int		clockms;					// milliseconds per keep alive tick
	char	scorevar[FLASH_VARLEN];		// SWF variable that receives the score
	char	clockvar[FLASH_VARLEN];		// SWF variable that receives the clock
	double	score;						// accumulated score      -> In1
	double	heartbeat;					// keep alive counter     -> In2
	DWORD	scoretick;					// GetTickCount() of the last point
	DWORD	clocktick;					// GetTickCount() of the last tick

	// ---- runtime state ---------------------------------------------------
	void		  *ax_host;						// HWND of the control container
	void		  *ax_container;				// host object (client site), must stay alive
	void		  *ax_ctrl;						// IDispatch* of the flash control
	volatile LONG	pending;				// an update message is in the queue
	volatile LONG	active;					// session is running
	volatile LONG	reload_req;				// movie has to be (re)loaded
	int				var_ok;					// SetVariable() worked at least once
	unsigned int	sent_count;				// number of pushed updates
	float			last_value[FLASH_MAXVAR]; // last value at the input port

	FLASHOBJ(int num);
	~FLASHOBJ();

	void	make_dialog(void);
	void	load(HANDLE hFile);
	void	save(HANDLE hFile);
	void	incoming_data(int port, float value);
	void	work(void);
	void	session_start(void);
	void	session_stop(void);
	void	session_reset(void);
	void	update_inports(void);			// rebuild ports from the config

	void	request_reload(void);			// thread safe: reload the movie
	void	request_play(void);
	void	request_stop(void);
};
