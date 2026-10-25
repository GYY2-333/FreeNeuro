/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: SV_PROTO.H:  wire format of the "Somatic Vision Data Socket"

  The Somatic Vision games shipped in BioExplorer's Games folder (Dual Drive
  Xtreme, Inner Tube, Particle Editor) are separate Torque based programs. They
  read the feedback values from a BioExplorer "Server" object through the COM
  object CBEClient (BEClient.dll): the client joins a multicast group to find the
  server, connects back to it over the loopback interface and then receives a
  stream of named channels with double samples.

  What is provable from the shipped files (no guessing):
    - BEClient.dll .data contains the strings
          "239.192.100.200"   (multicast group used for server discovery)
          "127.0.0.1"         (the address Connect() is called with)
      and the debug strings that name the messages the client dispatches:
          SS_MSG_CHANNEL_ADDED / _REMOVED / _NAME_CHANGED / _SAMPLE_RATE_CHANGED,
          SS_MSG_PLAY / SS_MSG_PAUSE / SS_MSG_STOP
    - the client reports FD_CONNECT / FD_CLOSE on the local socket, i.e. the data
      connection is connection oriented (TCP), while discovery is datagram based
    - the API (Code\include\BEClientInterface.h) hands the samples over as
          OnChannelSamples(id, double *pSamples, count)
      with OnChannelAdded(id, name, sampleRate) announcing the channels first
    - the official designs range their values to 0.0 - 1.0 before they are given
      to the Server object, which is what the games expect

  What is NOT known yet (only a packet capture can tell):
    - the UDP port of the multicast announcements and the TCP port of the data
      connection (both are numeric constants inside the .text section)
    - the numeric message ids and the exact field order of every packet

  Everything below is therefore the *assumed* layout of this implementation, kept
  in this one header on purpose: when the capture is available, only this file has
  to be corrected - ob_svserver.cpp builds every packet through these structures.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#ifndef SV_PROTO_H
#define SV_PROTO_H

// --- discovery ---------------------------------------------------------------

// group the client joins while looking for a server (string verified in
// BEClient.dll, file offset 0x5034)
#define SV_MCAST_GROUP      "239.192.100.200"

// port of the announcements and port the client connects to for data.
// 0 means "not filled in yet": the object then only logs the fact instead of
// sending traffic to a port that would be wrong anyway.
#define SV_MCAST_PORT       0
#define SV_DATA_PORT        0

// the client keeps a table of servers and drops the ones it has not heard from
// for a while ("Timer: don't see %s, removing..." in BEClient.dll), which is why
// the identity has to be repeated periodically - and not only on the multicast
// group but on the connected socket as well ("OnLocalRead: new server found").
#define SV_HEARTBEAT_MS     1000

// --- message ids (names verified, numeric values unknown) --------------------
enum
{
	SV_MSG_ANNOUNCE = 0,            // server identity / heartbeat
	SV_MSG_CHANNEL_ADDED,           // SS_MSG_CHANNEL_ADDED
	SV_MSG_CHANNEL_REMOVED,         // SS_MSG_CHANNEL_REMOVED
	SV_MSG_CHANNEL_NAME_CHANGED,    // SS_MSG_CHANNEL_NAME_CHANGED
	SV_MSG_CHANNEL_RATE_CHANGED,    // SS_MSG_CHANNEL_SAMPLE_RATE_CHANGED
	SV_MSG_SAMPLES,                 // sample block (no debug string in the client)
	SV_MSG_PLAY,                    // SS_MSG_PLAY
	SV_MSG_PAUSE,                   // SS_MSG_PAUSE
	SV_MSG_STOP                     // SS_MSG_STOP
};

// --- session state as seen by the game --------------------------------------
#define SV_STATE_STOP       0
#define SV_STATE_PLAY       1
#define SV_STATE_PAUSE      2

// --- packet layout (ASSUMED) -------------------------------------------------

#define SV_PACKET_MAX       1024        // largest datagram / message we build
#define SV_MAX_CHANNELS     16          // channels one object can publish
#define SV_NAME_LEN         32          // channel name buffer (including NUL)

#pragma pack(push,1)

// every message starts with its own size and the message id; the exact base
// layout still has to be confirmed by a capture
typedef struct SV_PACKETHEADER
{
	unsigned int	size;               // total message size in bytes
	unsigned int	type;               // SV_MSG_*
} SV_PACKETHEADER;

typedef struct SV_ANNOUNCEMSG
{
	SV_PACKETHEADER	head;
	unsigned int	protocol;           // version stamp, meaning unknown
	/* server name follows, zero terminated */
} SV_ANNOUNCEMSG;

typedef struct SV_CHANNELMSG
{
	SV_PACKETHEADER	head;
	int				id;                 // channel id the samples are tagged with
	double			rate;               // samples per second
	/* channel name follows, zero terminated */
} SV_CHANNELMSG;

typedef struct SV_SAMPLESMSG
{
	SV_PACKETHEADER	head;
	int				id;                 // channel the block belongs to
	int				count;              // number of doubles that follow
	/* count doubles, little endian IEEE-754 */
} SV_SAMPLESMSG;

typedef struct SV_STATEMSG
{
	SV_PACKETHEADER	head;
} SV_STATEMSG;

#pragma pack(pop)

// true when the two ports are known, i.e. when the object may talk to the network
#define SV_PROTO_PORT_KNOWN(p)  ((p) > 0 && (p) < 65536)

#endif // SV_PROTO_H
