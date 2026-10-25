/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_SVSERVER.H:  declarations for the "SV-Server" object

  The SV-Server object publishes values from its input ports as the named data
  channels of a "Somatic Vision Data Socket", i.e. as the server side of the
  protocol the Somatic Vision games (Games\Dual Drive Xtreme BE, Inner Tube BE,
  Particle Editor BE) are looking for. See sv_proto.h for the protocol and for
  the parts of it that still have to be confirmed by a packet capture.

  Signal flow:
      input port 0..n   one value per channel, expected 0.0 - 1.0
      send rate         one sample per channel every 1/chrate seconds

  Threading (intentionally the same rule as process_biosemi()/process_physioamp()):
      the worker thread only moves bytes. It never calls process_packets(), never
      touches the object graph and never sends a message to a window - that is
      what made the PhysioAmp reader un-stoppable before. incoming_data() and
      work() run on the timer thread and are the only producers of the sample
      queues the worker consumes.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "sv_proto.h"

#define SV_QUEUE_LEN     2048       // per channel ring, power of two
#define SV_QUEUE_MASK    (SV_QUEUE_LEN-1)
#define SV_BATCH         32         // samples per message
#define SV_CLIENTS       4          // clients served at the same time

class SV_SERVEROBJ : public BASE_CL
{
  public:

	// ---- configuration ---------------------------------------------------
	char	servername[SV_NAME_LEN*2];			// identity sent in the announcement
	int		mcast_port;							// 0 = use SV_MCAST_PORT
	int		data_port;							// 0 = use SV_DATA_PORT
	int		heartbeat_ms;
	int		enabled;							// 0 = object stays silent
	int		channels;							// number of published channels
	char	chname[SV_MAX_CHANNELS][SV_NAME_LEN];
	float	chrate[SV_MAX_CHANNELS];			// samples/sec per channel

	// ---- runtime state ---------------------------------------------------
	HANDLE			hThread;
	HANDLE			hStopEvent;
	volatile LONG	active;						// worker thread alive
	volatile LONG	state;						// SV_STATE_*
	volatile LONG	client_count;
	volatile LONG	sent_packets;
	volatile LONG	sent_samples;
	volatile LONG	lost_samples;

	float		  *ring[SV_MAX_CHANNELS];		// SPSC ring per channel
	volatile LONG	head[SV_MAX_CHANNELS];		// written by the timer thread
	volatile LONG	tail[SV_MAX_CHANNELS];		// written by the worker thread
	float			last_value[SV_MAX_CHANNELS];// last value at the input port
	float			phase[SV_MAX_CHANNELS];		// rate decimation accumulator
	int				last_state;					// last state put on the wire

	SV_SERVEROBJ(int num);
	~SV_SERVEROBJ();

	void	make_dialog(void);
	void	load(HANDLE hFile);
	void	save(HANDLE hFile);
	void	incoming_data(int port, float value);
	void	session_start(void);
	void	session_stop(void);
	void	session_reset(void);
	void	work(void);

	void	update_channels(void);				// rebuild the ports from the config
	void	update_captions(void);				// port labels from the channel names
	void	push_sample(int ch, float value);
	int		pop_samples(int ch, double *dst, int maxsamples);

	void	server_start(void);
	void	server_stop(void);
	void	worker_loop(void);					// runs on the worker thread
};
