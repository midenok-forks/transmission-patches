/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2 (b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: peer-mgr.h 13625 2012-12-05 17:29:46Z jordan $
 */

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#ifndef TR_PEER_MGR_H
#define TR_PEER_MGR_H

#include <inttypes.h> /* uint16_t */

#ifdef WIN32
 #include <winsock2.h> /* struct in_addr */
#endif

#include "bitfield.h"
#include "history.h"
#include "net.h" /* tr_address */
#include "peer-common.h" /* struct peer_request */

/**
 * @addtogroup peers Peers
 * @{
 */

struct UTPSocket;
struct tr_peer_stat;
struct tr_torrent;
typedef struct tr_peerMgr tr_peerMgr;

/* added_f's bitwise-or'ed flags */
enum
{
    /* true if the peer supports encryption */
    ADDED_F_ENCRYPTION_FLAG = 1,

    /* true if the peer is a seed or partial seed */
    ADDED_F_SEED_FLAG = 2,

    /* true if the peer supports uTP */
    ADDED_F_UTP_FLAGS = 4,

    /* true if the peer has holepunch support */
    ADDED_F_HOLEPUNCH = 8,

    /* true if the peer telling us about this peer
     * initiated the connection (implying that it is connectible) */
    ADDED_F_CONNECTABLE = 16
};

typedef struct tr_pex
{
    tr_address addr;
    tr_port    port; /* this field is in network byte order */
    uint8_t    flags;
}
tr_pex;


struct tr_peerIo;
struct tr_peermsgs;

enum
{
    ENCRYPTION_PREFERENCE_UNKNOWN,
    ENCRYPTION_PREFERENCE_YES,
    ENCRYPTION_PREFERENCE_NO
};

/* opaque forward declaration */
struct peer_atom;

/**
 * State information about a connected peer.
 *
 * @see struct peer_atom
 * @see tr_peermsgs
 */
typedef struct tr_peer
{
    bool                     peerIsChoked;
    bool                     peerIsInterested;
    bool                     clientIsChoked;
    bool                     clientIsInterested;
    bool                     doPurge;

    /* number of bad pieces they've contributed to */
    uint8_t                  strikes;

    uint8_t                  encryption_preference;
    tr_port                  dht_port;

    /* how many requests the peer has made that we haven't responded to yet */
    int                      pendingReqsToClient;

    /* how many requests we've made and are currently awaiting a response for */
    int                      pendingReqsToPeer;

    struct tr_peerIo       * io;
    struct peer_atom       * atom;

    struct tr_bitfield       blame;
    struct tr_bitfield       have;

    /** how complete the peer's copy of the torrent is. [0.0...1.0] */
    float                    progress;

    /* the client name from the `v' string in LTEP's handshake dictionary */
    char                   * client;

    time_t                   chokeChangedAt;

    tr_recentHistory         blocksSentToClient;
    tr_recentHistory         blocksSentToPeer;

    tr_recentHistory         cancelsSentToClient;
    tr_recentHistory         cancelsSentToPeer;

    struct tr_peermsgs     * msgs;
}
tr_peer;

void tr_peerConstruct (struct tr_peer * peer);

void tr_peerDestruct (tr_torrent * tor, struct tr_peer * peer);


static inline bool
tr_isPex (const tr_pex * pex)
{
    return pex && tr_address_is_valid (&pex->addr);
}

const tr_address * tr_peerAddress (const tr_peer *);

int tr_pexCompare (const void * a, const void * b);

tr_peerMgr* tr_peerMgrNew (tr_session *);

void tr_peerMgrFree (tr_peerMgr * manager);

bool tr_peerMgrPeerIsSeed (const tr_torrent * tor,
                              const tr_address * addr);

void tr_peerMgrSetUtpSupported (tr_torrent       * tor,
                                const tr_address * addr);

void tr_peerMgrSetUtpFailed (tr_torrent *tor,
                             const tr_address *addr,
                             bool failed);

void tr_peerMgrGetNextRequests (tr_torrent          * torrent,
                                tr_peer             * peer,
                                int                   numwant,
                                tr_block_index_t    * setme,
                                int                 * numgot,
                                bool                  get_intervals);

bool tr_peerMgrDidPeerRequest (const tr_torrent  * torrent,
                               const tr_peer     * peer,
                               tr_block_index_t    block);

void tr_peerMgrRebuildRequests (tr_torrent * torrent);

void tr_peerMgrAddIncoming (tr_peerMgr  * manager,
                            tr_address  * addr,
                            tr_port       port,
                            int           socket,
                            struct UTPSocket *utp_socket);

tr_pex * tr_peerMgrCompactToPex (const void    * compact,
                                 size_t          compactLen,
                                 const uint8_t * added_f,
                                 size_t          added_f_len,
                                 size_t        * setme_pex_count);

tr_pex * tr_peerMgrCompact6ToPex (const void    * compact,
                                  size_t          compactLen,
                                  const uint8_t * added_f,
                                  size_t          added_f_len,
                                  size_t        * pexCount);

tr_pex * tr_peerMgrArrayToPex (const void * array,
                               size_t       arrayLen,
                               size_t      * setme_pex_count);

/**
 * @param seedProbability [0..100] for likelihood that the peer is a seed; -1 for unknown
 */
void tr_peerMgrAddPex (tr_torrent     * tor,
                       uint8_t          from,
                       const tr_pex   * pex,
                       int8_t           seedProbability);

void tr_peerMgrMarkAllAsSeeds (tr_torrent * tor);

enum
{
    TR_PEERS_CONNECTED,
    TR_PEERS_INTERESTING
};

int  tr_peerMgrGetPeers (tr_torrent      * tor,
                         tr_pex         ** setme_pex,
                         uint8_t           address_type,
                         uint8_t           peer_list_mode,
                         int               max_peer_count);

void tr_peerMgrStartTorrent (tr_torrent * tor);

void tr_peerMgrStopTorrent (tr_torrent * tor);

void tr_peerMgrAddTorrent (tr_peerMgr         * manager,
                           struct tr_torrent  * tor);

void tr_peerMgrRemoveTorrent (tr_torrent * tor);

void tr_peerMgrTorrentAvailability (const tr_torrent * tor,
                                    int8_t           * tab,
                                    unsigned int       tabCount);

uint64_t tr_peerMgrGetDesiredAvailable (const tr_torrent * tor);

void tr_peerMgrOnTorrentGotMetainfo (tr_torrent * tor);

void tr_peerMgrOnBlocklistChanged (tr_peerMgr * manager);

void tr_peerMgrTorrentStats (tr_torrent * tor,
                             int * setmePeersConnected,
                             int * setmeWebseedsSendingToUs,
                             int * setmePeersSendingToUs,
                             int * setmePeersGettingFromUs,
                             int * setmePeersFrom); /* TR_PEER_FROM__MAX */

struct tr_peer_stat* tr_peerMgrPeerStats (const tr_torrent * tor,
                                          int              * setmeCount);

double* tr_peerMgrWebSpeeds_KBps (const tr_torrent * tor);


unsigned int tr_peerGetPieceSpeed_Bps (const tr_peer    * peer,
                                       uint64_t           now,
                                       tr_direction       direction);

void tr_peerMgrClearInterest (tr_torrent * tor);

/* @} */

#endif
