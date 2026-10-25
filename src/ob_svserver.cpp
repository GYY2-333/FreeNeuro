/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_SVSERVER.CPP:  the "SV-Server" object

  Publishes the values arriving at its input ports as the data channels of a
  "Somatic Vision Data Socket" - the server side of the protocol the Somatic
  Vision games in BioExplorer's Games folder listen for. See sv_proto.h for what
  is proven about the protocol and what still has to be confirmed by a capture.

  Layout of one worker iteration (10 ms):
      check the stop event          (so a stop always takes effect)
      serve the socket set          (accept a game, drop closed clients)
      heartbeat                     (identity, on the group and per client)
      per channel: drain the queue  (one message with up to SV_BATCH samples)
      state change                  (PLAY / STOP)

  The object graph is never touched from here, and nothing in this file sends a
  message to a window: the worker thread may therefore be stopped with a plain
  WaitForSingleObject() and can never block the user interface.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include "brainBay.h"
#include "SDL_net.h"
#include "ob_svserver.h"
#include "sv_proto.h"

// how long one worker iteration waits for socket activity. The stop event is
// polled once per iteration, so this is also the worst case reaction time.
#define SV_TICK_MS          10

// how often a failed bind/listen is retried (the log would flood otherwise)
#define SV_RETRY_MS         2000


// ---------------------------------------------------------------------------
//  packet helpers
// ---------------------------------------------------------------------------

static void sv_put_header(unsigned char *buf, unsigned int size, unsigned int type)
{
	SV_PACKETHEADER *head = (SV_PACKETHEADER *) buf;

	head->size = size;
	head->type = type;
}


// send a finished message to one client; returns TRUE when everything went out
static BOOL sv_send(TCPsocket sock, const void *data, int len)
{
	if (sock == NULL || len <= 0) return FALSE;

	return (SDLNet_TCP_Send(sock, (void *) data, len) == len);
}


// ---------------------------------------------------------------------------
//  object
// ---------------------------------------------------------------------------

SV_SERVEROBJ::SV_SERVEROBJ(int num) : BASE_CL()
{
	int i;

	strcpy(servername, "BrainBay");
	mcast_port   = SV_MCAST_PORT;
	data_port    = SV_DATA_PORT;
	heartbeat_ms = SV_HEARTBEAT_MS;
	enabled      = 1;

	channels = 2;
	for (i = 0; i < SV_MAX_CHANNELS; i++)
	{
		if (i == 0)      strcpy(chname[i], "Reward");
		else if (i == 1) strcpy(chname[i], "Inhibit");
		else             sprintf(chname[i], "Channel %d", i + 1);

		chrate[i]     = 10.0f;
		last_value[i] = 0.0f;
		phase[i]      = 0.0f;
		head[i]       = 0;
		tail[i]       = 0;
		ring[i]       = (float *) calloc(SV_QUEUE_LEN, sizeof(float));
	}

	hThread      = NULL;
	hStopEvent   = NULL;
	active       = 0;
	state        = SV_STATE_STOP;
	last_state   = -1;
	client_count = 0;
	sent_packets = 0;
	sent_samples = 0;
	lost_samples = 0;

	outports = 0;
	width = 90;

	update_channels();

	if (enabled) server_start();
}


SV_SERVEROBJ::~SV_SERVEROBJ(void)
{
	int i;

	server_stop();

	if (hThread != NULL)
	{
		// the worker could not be stopped and still reads the sample queues, so
		// freeing them here would be a use-after-free - leaking is the lesser evil
		write_logfile("SV-Server: worker still running, keeping the sample queues");
		return;
	}

	for (i = 0; i < SV_MAX_CHANNELS; i++)
	{
		if (ring[i]) free(ring[i]);
		ring[i] = NULL;
	}
}


/* -------------------------------------------------------------------------
   configuration
   ------------------------------------------------------------------------- */

void SV_SERVEROBJ::update_channels(void)
{
	int i;

	if (channels < 1) channels = 1;
	if (channels > SV_MAX_CHANNELS) channels = SV_MAX_CHANNELS;

	inports = channels;

	for (i = 0; i < channels; i++)
	{
		strncpy(in_ports[i].in_name, chname[i], 19);
		in_ports[i].in_name[19] = 0;
		strcpy(in_ports[i].in_desc, chname[i]);
		in_ports[i].in_min = 0.0f;
		in_ports[i].in_max = 1.0f;
		strcpy(in_ports[i].in_dim, "ratio");
		in_ports[i].get_range = 0;
	}

	height = CON_START + inports * CON_HEIGHT + 5;
}


void SV_SERVEROBJ::update_captions(void)
{
	update_channels();
}


void SV_SERVEROBJ::make_dialog(void)
{
	char info[512];

	sprintf(info,
	        "SV-Server: publishes %d channel(s) for the Somatic Vision games.\n\n"
	        "Discovery group: %s\n"
	        "Multicast port:  %d %s\n"
	        "Data port:       %d %s\n"
	        "Heartbeat:       %d ms\n"
	        "Channels:        %s (%.1f Hz) ...\n\n"
	        "The ports have to be filled in (sv_proto.h, properties sv_mcastport /\n"
	        "sv_dataport) - they are still unknown and are taken from a packet\n"
	        "capture of BioExplorer + one of the games.",
	        channels, SV_MCAST_GROUP,
	        mcast_port, SV_PROTO_PORT_KNOWN(mcast_port) ? "" : "(unknown)",
	        data_port, SV_PROTO_PORT_KNOWN(data_port) ? "" : "(unknown)",
	        heartbeat_ms, chname[0], chrate[0]);

	write_logfile("SV-Server: channels=%d mcastport=%d dataport=%d", channels, mcast_port, data_port);

	if (!SV_PROTO_PORT_KNOWN(mcast_port) && !SV_PROTO_PORT_KNOWN(data_port))
		write_logfile("SV-Server: multicast/data port unknown - set sv_mcastport and sv_dataport "
		              "(see sv_proto.h), nothing is transmitted yet");

	// NOTE: report() must NOT be used here. It calls close_toolbox(), which sets
	// actobject to NULL - and create_object() dereferences actobject (displayWnd)
	// right after make_dialog() returns, so using it here crashed the application
	// the moment the object was inserted.
	MessageBox(ghWndMain, info, "BrainBay - SV-Server", MB_OK | MB_ICONINFORMATION);
}


/* -------------------------------------------------------------------------
   samples: the timer thread produces, the worker thread consumes
   ------------------------------------------------------------------------- */

void SV_SERVEROBJ::push_sample(int ch, float value)
{
	LONG head = this->head[ch];

	if ((ULONG)(head - tail[ch]) >= SV_QUEUE_LEN)
	{
		// the consumer stopped (no client, session paused) - dropping the sample
		// is the only thing that keeps the queue from growing without bound
		lost_samples++;
		return;
	}

	ring[ch][head & SV_QUEUE_MASK] = value;
	this->head[ch] = head + 1;
}


int SV_SERVEROBJ::pop_samples(int ch, double *dst, int maxsamples)
{
	LONG tail = this->tail[ch];
	LONG head = this->head[ch];
	int  n = 0;

	if (tail == head) return 0;

	n = (int)((ULONG)(head - tail));
	if (n > maxsamples) n = maxsamples;

	for (int i = 0; i < n; i++)
		dst[i] = (double) ring[ch][(tail + i) & SV_QUEUE_MASK];

	this->tail[ch] = tail + n;
	return n;
}


void SV_SERVEROBJ::incoming_data(int port, float value)
{
	if (port < 0 || port >= channels) return;

	last_value[port] = value;
}


void SV_SERVEROBJ::work(void)
{
	int c;

	if (!active || !enabled) return;
	if (GLOBAL.loading) return;

	if (PACKETSPERSECOND < 1) return;

	for (c = 0; c < channels; c++)
	{
		float rate = chrate[c];

		if (rate <= 0.0f) continue;
		if (rate > (float)PACKETSPERSECOND) rate = (float)PACKETSPERSECOND;

		phase[c] += rate / (float)PACKETSPERSECOND;
		while (phase[c] >= 1.0f)
		{
			phase[c] -= 1.0f;
			push_sample(c, last_value[c]);
		}
	}
}


/* -------------------------------------------------------------------------
   session state (the game gets PLAY / STOP with the session)
   ------------------------------------------------------------------------- */

void SV_SERVEROBJ::session_start(void)
{
	int i;

	for (i = 0; i < SV_MAX_CHANNELS; i++)
	{
		head[i] = 0;
		tail[i] = 0;
		phase[i] = 0.0f;
	}

	if (enabled && !active) server_start();

	state = SV_STATE_PLAY;
}


void SV_SERVEROBJ::session_stop(void)
{
	state = SV_STATE_STOP;
}


void SV_SERVEROBJ::session_reset(void)
{
	state = SV_STATE_STOP;
}


/* -------------------------------------------------------------------------
   persistence
   ------------------------------------------------------------------------- */

void SV_SERVEROBJ::load(HANDLE hFile)
{
	int i;

	load_object_basics(this);

	load_property("sv_name",P_STRING,servername);
	load_property("sv_mcastport",P_INT,&mcast_port);
	load_property("sv_dataport",P_INT,&data_port);
	load_property("sv_heartbeat",P_INT,&heartbeat_ms);
	load_property("sv_enabled",P_INT,&enabled);
	load_property("sv_channels",P_INT,&channels);

	for (i = 0; i < SV_MAX_CHANNELS; i++)
	{
		char key[32];

		sprintf(key, "sv_name%d", i + 1);
		load_property(key, P_STRING, chname[i]);

		sprintf(key, "sv_rate%d", i + 1);
		load_property(key, P_FLOAT, &chrate[i]);
	}

	update_channels();

	if (enabled) server_start();
}


void SV_SERVEROBJ::save(HANDLE hFile)
{
	int i;

	save_object_basics(hFile, this);

	save_property(hFile,"sv_name",P_STRING,servername);
	save_property(hFile,"sv_mcastport",P_INT,&mcast_port);
	save_property(hFile,"sv_dataport",P_INT,&data_port);
	save_property(hFile,"sv_heartbeat",P_INT,&heartbeat_ms);
	save_property(hFile,"sv_enabled",P_INT,&enabled);
	save_property(hFile,"sv_channels",P_INT,&channels);

	for (i = 0; i < SV_MAX_CHANNELS; i++)
	{
		char key[32];

		sprintf(key, "sv_name%d", i + 1);
		save_property(hFile, key, P_STRING, chname[i]);

		sprintf(key, "sv_rate%d", i + 1);
		save_property(hFile, key, P_FLOAT, &chrate[i]);
	}
}


/* -------------------------------------------------------------------------
   worker thread
   ------------------------------------------------------------------------- */

static DWORD WINAPI sv_server_thread(LPVOID lpv)
{
	SV_SERVEROBJ *st = (SV_SERVEROBJ *) lpv;

	if (st != NULL) st->worker_loop();
	return 0;
}


void SV_SERVEROBJ::server_start(void)
{
	if (active) return;

	if (hStopEvent == NULL)
	{
		hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (hStopEvent == NULL)
		{
			write_logfile("SV-Server: could not create the stop event");
			return;
		}
	}

	ResetEvent(hStopEvent);
	active = 1;

	hThread = CreateThread(NULL, 0, sv_server_thread, this, 0, NULL);
	if (hThread == NULL)
	{
		active = 0;
		write_logfile("SV-Server: could not create the worker thread");
		return;
	}

	write_logfile("SV-Server: worker thread started");
}


void SV_SERVEROBJ::server_stop(void)
{
	if (hThread == NULL) return;

	if (hStopEvent != NULL) SetEvent(hStopEvent);

	// the worker never touches a window and never runs object code, so waiting
	// for it here cannot deadlock - that is the whole point of this split
	if (WaitForSingleObject(hThread, 2000) != WAIT_OBJECT_0)
	{
		// keep the handle and the thread flag: the worker may still be running,
		// and nothing it can still touch may be released
		write_logfile("SV-Server: worker thread did not stop within 2 s");
		return;
	}

	CloseHandle(hThread);
	hThread = NULL;
	active = 0;
}


void SV_SERVEROBJ::worker_loop(void)
{
	UDPsocket			udp = NULL;
	UDPpacket		   *pkt = NULL;
	TCPsocket			srv = NULL;
	TCPsocket			cli[SV_CLIENTS];
	SDLNet_SocketSet	sockset = NULL;
	IPaddress			addr;
	IPaddress			group;
	unsigned char		msg[SV_PACKET_MAX];
	unsigned char		rx[256];
	double				samples[SV_BATCH];
	int					i, k, n, ready;
	int					udp_bound = 0;
	int					listen_warned = 0;
	int					poll_warned = 0;
	int					nsockets = 0;
	unsigned int		last_beat = 0;
	unsigned int		last_try = 0;

	memset(cli, 0, sizeof(cli));

	write_logfile("SV-Server: worker thread running");

	udp = SDLNet_UDP_Open(0);
	if (udp != NULL) pkt = SDLNet_AllocPacket(SV_PACKET_MAX);

	sockset = SDLNet_AllocSocketSet(SV_CLIENTS + 1);
	if (sockset == NULL)
	{
		write_logfile("SV-Server: could not allocate the socket set");
		if (pkt) SDLNet_FreePacket(pkt);
		if (udp) SDLNet_UDP_Close(udp);
		active = 0;
		return;
	}

	while (WaitForSingleObject(hStopEvent, 0) != WAIT_OBJECT_0)
	{
		int mport = SV_PROTO_PORT_KNOWN(mcast_port) ? mcast_port : SV_MCAST_PORT;
		int dport = SV_PROTO_PORT_KNOWN(data_port)  ? data_port  : SV_DATA_PORT;
		int chn   = channels;
		unsigned int now = GetTickCount();

		if (chn < 1) chn = 1;
		if (chn > SV_MAX_CHANNELS) chn = SV_MAX_CHANNELS;

		// ------------------------------------------------------------------
		//  the protocol constants are not known yet: say so once and idle
		// ------------------------------------------------------------------
		if (!SV_PROTO_PORT_KNOWN(mport) && !SV_PROTO_PORT_KNOWN(dport))
		{
			SDL_Delay(SV_TICK_MS);       // nothing to poll yet, see make_dialog()
			continue;
		}

		// ------------------------------------------------------------------
		//  bind the discovery channel to the multicast group (once)
		// ------------------------------------------------------------------
		if (!udp_bound && udp != NULL && pkt != NULL && mport != 0 && (now - last_try) >= 0)
		{
			if (SDLNet_ResolveHost(&group, SV_MCAST_GROUP, (Uint16) mport) == 0 &&
			    SDLNet_UDP_Bind(udp, 0, &group) >= 0)
			{
				udp_bound = 1;
				write_logfile("SV-Server: announcing on %s:%d", SV_MCAST_GROUP, mport);
			}
			else if ((now - last_try) >= SV_RETRY_MS)
			{
				write_logfile("SV-Server: multicast bind failed (%s)", SDLNet_GetError());
			}
		}

		// ------------------------------------------------------------------
		//  open the data socket (retried, the port may still be busy)
		// ------------------------------------------------------------------
		if (srv == NULL && dport != 0 && (now - last_try) >= SV_RETRY_MS)
		{
			last_try = now;

			addr.host = INADDR_NONE;            // server socket: listen on all interfaces
			addr.port = (Uint16) dport;
			srv = SDLNet_TCP_Open(&addr);

			if (srv != NULL)
			{
				SDLNet_TCP_AddSocket(sockset, srv);
				nsockets++;
				listen_warned = 0;
				write_logfile("SV-Server: listening for the game on port %d", dport);
			}
			else if (!listen_warned)
			{
				listen_warned = 1;
				write_logfile("SV-Server: cannot listen on port %d (%s)", dport, SDLNet_GetError());
			}
		}

		// ------------------------------------------------------------------
		//  wait for socket activity (also the pacing of this loop)
		// ------------------------------------------------------------------
		if (nsockets == 0)
		{
			// nothing to wait for yet - and select() is not specified to be happy
			// about an empty socket set, so do not ask it to be
			SDL_Delay(SV_TICK_MS);
			ready = 0;
		}
		else
		{
			ready = SDLNet_CheckSockets(sockset, SV_TICK_MS);
			if (ready == -1)
			{
				// one failed poll must not kill a server that is otherwise fine
				if (!poll_warned)
				{
					poll_warned = 1;
					write_logfile("SV-Server: SDLNet_CheckSockets failed (%s)", SDLNet_GetError());
				}
				SDL_Delay(SV_TICK_MS);
				ready = 0;
			}
		}

		// --- accept a game -------------------------------------------------
		if (srv != NULL && SDLNet_SocketReady(srv))
		{
			TCPsocket c = SDLNet_TCP_Accept(srv);

			if (c != NULL)
			{
				for (k = 0; k < SV_CLIENTS; k++) if (cli[k] == NULL) break;

				if (k < SV_CLIENTS)
				{
					cli[k] = c;
					SDLNet_TCP_AddSocket(sockset, c);
					nsockets++;
					client_count++;
					write_logfile("SV-Server: client %d connected", k);

					// the client needs the channel list before any sample
					for (i = 0; i < chn; i++)
					{
						SV_CHANNELMSG *cm = (SV_CHANNELMSG *) msg;
						int len = sizeof(SV_CHANNELMSG);
						int nl = (int) strlen(chname[i]) + 1;

						sv_put_header(msg, (unsigned int)(len + nl), SV_MSG_CHANNEL_ADDED);
						cm->id = i;
						cm->rate = (double) chrate[i];
						memcpy(msg + len, chname[i], nl);
						sv_send(c, msg, len + nl);
						sent_packets++;
					}

					last_state = -1;         // send the current state below
				}
				else
				{
					write_logfile("SV-Server: no free client slot");
					SDLNet_TCP_Close(c);
				}
			}
		}

		// --- drop clients that went away (or sent something) ---------------
		for (k = 0; k < SV_CLIENTS; k++)
		{
			if (cli[k] == NULL) continue;

			if (SDLNet_SocketReady(cli[k]))
			{
				if (SDLNet_TCP_Recv(cli[k], rx, sizeof(rx)) <= 0)
				{
					SDLNet_TCP_DelSocket(sockset, cli[k]);
					SDLNet_TCP_Close(cli[k]);
					cli[k] = NULL;
					if (nsockets > 0) nsockets--;
					if (client_count > 0) client_count--;
					write_logfile("SV-Server: client %d disconnected", k);
				}
			}
		}

		// --- heartbeat: the client forgets a server that stays silent ------
		if (udp_bound && (now - last_beat) >= (unsigned int) heartbeat_ms)
		{
			SV_ANNOUNCEMSG *an = (SV_ANNOUNCEMSG *) msg;
			int len = sizeof(SV_ANNOUNCEMSG);
			int nl = (int) strlen(servername) + 1;

			last_beat = now;

			sv_put_header(msg, (unsigned int)(len + nl), SV_MSG_ANNOUNCE);
			an->protocol = 1;
			memcpy(msg + len, servername, nl);

			if (pkt != NULL)
			{
				int total = len + nl;

				if (total > pkt->maxlen) total = pkt->maxlen;
				memcpy(pkt->data, msg, total);
				pkt->len = total;
				pkt->channel = 0;
				SDLNet_UDP_Send(udp, 0, pkt);
			}

			// the discovery datagram is repeated on every data connection as
			// well: the client logs "new server found" for the local socket too
			for (k = 0; k < SV_CLIENTS; k++)
				if (cli[k] != NULL) sv_send(cli[k], msg, len + nl);
		}

		// --- sample blocks --------------------------------------------------
		for (i = 0; i < chn; i++)
		{
			n = pop_samples(i, samples, SV_BATCH);
			if (n <= 0) continue;

			SV_SAMPLESMSG *sm = (SV_SAMPLESMSG *) msg;
			int len = sizeof(SV_SAMPLESMSG);

			sv_put_header(msg, (unsigned int)(len + n * (int)sizeof(double)), SV_MSG_SAMPLES);
			sm->id = i;
			sm->count = n;
			memcpy(msg + len, samples, n * sizeof(double));

			for (k = 0; k < SV_CLIENTS; k++)
				if (cli[k] != NULL && sv_send(cli[k], msg, len + n * (int)sizeof(double)))
				{
					sent_packets++;
					sent_samples += n;
				}
		}

		// --- session state --------------------------------------------------
		if ((int) state != last_state)
		{
			SV_STATEMSG *sm = (SV_STATEMSG *) msg;
			int len = sizeof(SV_STATEMSG);
			unsigned int type = SV_MSG_STOP;

			if (state == SV_STATE_PLAY)       type = SV_MSG_PLAY;
			else if (state == SV_STATE_PAUSE) type = SV_MSG_PAUSE;

			sv_put_header(msg, (unsigned int) len, type);

			for (k = 0; k < SV_CLIENTS; k++)
				if (cli[k] != NULL) sv_send(cli[k], msg, len);

			last_state = (int) state;
			sent_packets++;
		}
	}

	// --- cleanup -------------------------------------------------------------
	for (k = 0; k < SV_CLIENTS; k++)
	{
		if (cli[k] != NULL)
		{
			SDLNet_TCP_DelSocket(sockset, cli[k]);
			SDLNet_TCP_Close(cli[k]);
			cli[k] = NULL;
		}
	}

	if (srv != NULL) SDLNet_TCP_Close(srv);
	if (pkt != NULL) SDLNet_FreePacket(pkt);
	if (udp != NULL) SDLNet_UDP_Close(udp);
	if (sockset != NULL) SDLNet_FreeSocketSet(sockset);

	client_count = 0;
	active = 0;
	write_logfile("SV-Server: worker thread closed");
}
