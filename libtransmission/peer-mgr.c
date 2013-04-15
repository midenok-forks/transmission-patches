/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2 (b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: peer-mgr.c 13631 2012-12-07 01:53:31Z jordan $
 */

#include <assert.h>
#include <errno.h> /* error codes ERANGE, ... */
#include <limits.h> /* INT_MAX */
#include <string.h> /* memcpy, memcmp, strstr */
#include <stdlib.h> /* qsort */

#include <event2/event.h>

#include <libutp/utp.h>

#include "transmission.h"
#include "announcer.h"
#include "bandwidth.h"
#include "blocklist.h"
#include "cache.h"
#include "clients.h"
#include "completion.h"
#include "crypto.h"
#include "handshake.h"
#include "net.h"
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-msgs.h"
#include "ptrarray.h"
#include "session.h"
#include "stats.h" /* tr_statsAddUploaded, tr_statsAddDownloaded */
#include "torrent.h"
#include "tr-utp.h"
#include "utils.h"
#include "webseed.h"

enum
{
    /* how frequently to cull old atoms */
    ATOM_PERIOD_MSEC = (60 * 1000),

    /* how frequently to change which peers are choked */
    RECHOKE_PERIOD_MSEC = (10 * 1000),

    /* an optimistically unchoked peer is immune from rechoking
       for this many calls to rechokeUploads (). */
    OPTIMISTIC_UNCHOKE_MULTIPLIER = 4,

    /* how frequently to reallocate bandwidth */
    BANDWIDTH_PERIOD_MSEC = 500,

    /* how frequently to age out old piece request lists */
    REFILL_UPKEEP_PERIOD_MSEC = (10 * 1000),

    /* how frequently to decide which peers live and die */
    RECONNECT_PERIOD_MSEC = 500,

    /* when many peers are available, keep idle ones this long */
    MIN_UPLOAD_IDLE_SECS = (60),

    /* when few peers are available, keep idle ones this long */
    MAX_UPLOAD_IDLE_SECS = (60 * 5),

    /* max number of peers to ask for per second overall.
     * this throttle is to avoid overloading the router */
    MAX_CONNECTIONS_PER_SECOND = 12,

    MAX_CONNECTIONS_PER_PULSE = (int)(MAX_CONNECTIONS_PER_SECOND * (RECONNECT_PERIOD_MSEC/1000.0)),

    /* number of bad pieces a peer is allowed to send before we ban them */
    MAX_BAD_PIECES_PER_PEER = 5,

    /* amount of time to keep a list of request pieces lying around
       before it's considered too old and needs to be rebuilt */
    PIECE_LIST_SHELF_LIFE_SECS = 60,

    /* use for bitwise operations w/peer_atom.flags2 */
    MYFLAG_BANNED = 1,

    /* use for bitwise operations w/peer_atom.flags2 */
    /* unreachable for now... but not banned.
     * if they try to connect to us it's okay */
    MYFLAG_UNREACHABLE = 2,

    /* the minimum we'll wait before attempting to reconnect to a peer */
    MINIMUM_RECONNECT_INTERVAL_SECS = 5,

    /** how long we'll let requests we've made linger before we cancel them */
    REQUEST_TTL_SECS = 120,

    NO_BLOCKS_CANCEL_HISTORY = 120,

    CANCEL_HISTORY_SEC = 60
};

const tr_peer_event TR_PEER_EVENT_INIT = { 0, 0, NULL, 0, 0, 0, false, 0 };

/**
***
**/

/**
 * Peer information that should be kept even before we've connected and
 * after we've disconnected. These are kept in a pool of peer_atoms to decide
 * which ones would make good candidates for connecting to, and to watch out
 * for banned peers.
 *
 * @see tr_peer
 * @see tr_peermsgs
 */
struct peer_atom
{
    uint8_t     fromFirst;          /* where the peer was first found */
    uint8_t     fromBest;           /* the "best" value of where the peer has been found */
    uint8_t     flags;              /* these match the added_f flags */
    uint8_t     flags2;             /* flags that aren't defined in added_f */
    int8_t      seedProbability;    /* how likely is this to be a seed... [0..100] or -1 for unknown */
    int8_t      blocklisted;        /* -1 for unknown, true for blocklisted, false for not blocklisted */

    tr_port     port;
    bool        utp_failed;         /* We recently failed to connect over uTP */
    uint16_t    numFails;
    time_t      time;               /* when the peer's connection status last changed */
    time_t      piece_data_time;

    time_t      lastConnectionAttemptAt;
    time_t      lastConnectionAt;

    /* similar to a TTL field, but less rigid --
     * if the swarm is small, the atom will be kept past this date. */
    time_t      shelf_date;
    tr_peer   * peer;               /* will be NULL if not connected */
    tr_address  addr;
};

#ifdef NDEBUG
#define tr_isAtom(a) (TRUE)
#else
static bool
tr_isAtom (const struct peer_atom * atom)
{
    return (atom != NULL)
        && (atom->fromFirst < TR_PEER_FROM__MAX)
        && (atom->fromBest < TR_PEER_FROM__MAX)
        && (tr_address_is_valid (&atom->addr));
}
#endif

static const char*
tr_atomAddrStr (const struct peer_atom * atom)
{
    return atom ? tr_peerIoAddrStr (&atom->addr, atom->port) : "[no atom]";
}

struct block_request
{
    tr_block_index_t block;
    tr_peer * peer;
    time_t sentAt;
};

struct weighted_piece
{
    tr_piece_index_t index;
    int16_t salt;
    int16_t requestCount;
};

enum piece_sort_state
{
    PIECES_UNSORTED,
    PIECES_SORTED_BY_INDEX,
    PIECES_SORTED_BY_WEIGHT
};

/** @brief Opaque, per-torrent data structure for peer connection information */
typedef struct tr_torrent_peers
{
    tr_ptrArray                outgoingHandshakes; /* tr_handshake */
    tr_ptrArray                pool; /* struct peer_atom */
    tr_ptrArray                peers; /* tr_peer */
    tr_ptrArray                webseeds; /* tr_webseed */

    tr_torrent               * tor;
    struct tr_peerMgr        * manager;

    tr_peer                  * optimistic; /* the optimistic peer, or NULL if none */
    int                        optimisticUnchokeTimeScaler;

    bool                       isRunning;
    bool                       needsCompletenessCheck;

    struct block_request     * requests;
    int                        requestCount;
    int                        requestAlloc;

    struct weighted_piece    * pieces;
    int                        pieceCount;
    enum piece_sort_state      pieceSortState;

    /* An array of pieceCount items stating how many peers have each piece.
       This is used to help us for downloading pieces "rarest first."
       This may be NULL if we don't have metainfo yet, or if we're not
       downloading and don't care about rarity */
    uint16_t                 * pieceReplication;
    size_t                     pieceReplicationSize;

    int                        interestedCount;
    int                        maxPeers;
    time_t                     lastCancel;

    /* Before the endgame this should be 0. In endgame, is contains the average
     * number of pending requests per peer. Only peers which have more pending
     * requests are considered 'fast' are allowed to request a block that's
     * already been requested from another (slower?) peer. */
    int                        endgame;
}
Torrent;

struct tr_peerMgr
{
    tr_session    * session;
    tr_ptrArray     incomingHandshakes; /* tr_handshake */
    struct event  * bandwidthTimer;
    struct event  * rechokeTimer;
    struct event  * refillUpkeepTimer;
    struct event  * atomTimer;
};

#define tordbg(t, ...) \
  do \
    { \
      if (tr_deepLoggingIsActive ()) \
        tr_deepLog (__FILE__, __LINE__, tr_torrentName (t->tor), __VA_ARGS__); \
    } \
  while (0)

#define dbgmsg(...) \
  do \
    { \
      if (tr_deepLoggingIsActive ()) \
        tr_deepLog (__FILE__, __LINE__, NULL, __VA_ARGS__); \
    } \
  while (0)

/**
***
**/

static inline void
managerLock (const struct tr_peerMgr * manager)
{
    tr_sessionLock (manager->session);
}

static inline void
managerUnlock (const struct tr_peerMgr * manager)
{
    tr_sessionUnlock (manager->session);
}

static inline void
torrentLock (Torrent * torrent)
{
    managerLock (torrent->manager);
}

static inline void
torrentUnlock (Torrent * torrent)
{
    managerUnlock (torrent->manager);
}

static inline int
torrentIsLocked (const Torrent * t)
{
    return tr_sessionIsLocked (t->manager->session);
}

/**
***
**/

static int
handshakeCompareToAddr (const void * va, const void * vb)
{
    const tr_handshake * a = va;

    return tr_address_compare (tr_handshakeGetAddr (a, NULL), vb);
}

static int
handshakeCompare (const void * a, const void * b)
{
    return handshakeCompareToAddr (a, tr_handshakeGetAddr (b, NULL));
}

static inline tr_handshake*
getExistingHandshake (tr_ptrArray * handshakes, const tr_address * addr)
{
    if (tr_ptrArrayEmpty (handshakes))
        return NULL;

    return tr_ptrArrayFindSorted (handshakes, addr, handshakeCompareToAddr);
}

static int
comparePeerAtomToAddress (const void * va, const void * vb)
{
    const struct peer_atom * a = va;

    return tr_address_compare (&a->addr, vb);
}

static int
compareAtomsByAddress (const void * va, const void * vb)
{
    const struct peer_atom * b = vb;

    assert (tr_isAtom (b));

    return comparePeerAtomToAddress (va, &b->addr);
}

/**
***
**/

const tr_address *
tr_peerAddress (const tr_peer * peer)
{
    return &peer->atom->addr;
}

static Torrent*
getExistingTorrent (tr_peerMgr *    manager,
                    const uint8_t * hash)
{
    tr_torrent * tor = tr_torrentFindFromHash (manager->session, hash);

    return tor == NULL ? NULL : tor->torrentPeers;
}

static int
peerCompare (const void * a, const void * b)
{
    return tr_address_compare (tr_peerAddress (a), tr_peerAddress (b));
}

static struct peer_atom*
getExistingAtom (const Torrent    * t,
                 const tr_address * addr)
{
    Torrent * tt = (Torrent*)t;
    assert (torrentIsLocked (t));
    return tr_ptrArrayFindSorted (&tt->pool, addr, comparePeerAtomToAddress);
}

static bool
peerIsInUse (const Torrent * ct, const struct peer_atom * atom)
{
    Torrent * t = (Torrent*) ct;

    assert (torrentIsLocked (t));

    return (atom->peer != NULL)
        || getExistingHandshake (&t->outgoingHandshakes, &atom->addr)
        || getExistingHandshake (&t->manager->incomingHandshakes, &atom->addr);
}

void
tr_peerConstruct (tr_peer * peer)
{
    memset (peer, 0, sizeof (tr_peer));

    peer->have = TR_BITFIELD_INIT;
}

static tr_peer*
peerNew (struct peer_atom * atom)
{
    tr_peer * peer = tr_new (tr_peer, 1);
    tr_peerConstruct (peer);

    peer->atom = atom;
    atom->peer = peer;

    return peer;
}

static tr_peer*
getPeer (Torrent * torrent, struct peer_atom * atom)
{
    tr_peer * peer;

    assert (torrentIsLocked (torrent));

    peer = atom->peer;

    if (peer == NULL)
    {
        peer = peerNew (atom);
        tr_bitfieldConstruct (&peer->have, torrent->tor->info.pieceCount);
        tr_bitfieldConstruct (&peer->blame, torrent->tor->blockCount);
        tr_ptrArrayInsertSorted (&torrent->peers, peer, peerCompare);
    }

    return peer;
}

static void peerDeclinedAllRequests (Torrent *, const tr_peer *);

void
tr_peerDestruct (tr_torrent * tor, tr_peer * peer)
{
    assert (peer != NULL);

    peerDeclinedAllRequests (tor->torrentPeers, peer);

    if (peer->msgs != NULL)
        tr_peerMsgsFree (peer->msgs);

    if (peer->io) {
        tr_peerIoClear (peer->io);
        tr_peerIoUnref (peer->io); /* balanced by the ref in handshakeDoneCB () */
    }

    tr_bitfieldDestruct (&peer->have);
    tr_bitfieldDestruct (&peer->blame);
    tr_free (peer->client);

    if (peer->atom)
        peer->atom->peer = NULL;
}

static void
peerDelete (Torrent * t, tr_peer * peer)
{
    tr_peerDestruct (t->tor, peer);
    tr_free (peer);
}

static bool
replicationExists (const Torrent * t)
{
    return t->pieceReplication != NULL;
}

static void
replicationFree (Torrent * t)
{
    tr_free (t->pieceReplication);
    t->pieceReplication = NULL;
    t->pieceReplicationSize = 0;
}

static void
replicationNew (Torrent * t)
{
    tr_piece_index_t piece_i;
    const tr_piece_index_t piece_count = t->tor->info.pieceCount;
    tr_peer ** peers = (tr_peer**) tr_ptrArrayBase (&t->peers);
    const int peer_count = tr_ptrArraySize (&t->peers);

    assert (!replicationExists (t));

    t->pieceReplicationSize = piece_count;
    t->pieceReplication = tr_new0 (uint16_t, piece_count);

    for (piece_i=0; piece_i<piece_count; ++piece_i)
    {
        int peer_i;
        uint16_t r = 0;

        for (peer_i=0; peer_i<peer_count; ++peer_i)
            if (tr_bitfieldHas (&peers[peer_i]->have, piece_i))
                ++r;

        t->pieceReplication[piece_i] = r;
    }
}

static void
torrentFree (void * vt)
{
    Torrent * t = vt;

    assert (t);
    assert (!t->isRunning);
    assert (torrentIsLocked (t));
    assert (tr_ptrArrayEmpty (&t->outgoingHandshakes));
    assert (tr_ptrArrayEmpty (&t->peers));

    tr_ptrArrayDestruct (&t->webseeds, (PtrArrayForeachFunc)tr_webseedFree);
    tr_ptrArrayDestruct (&t->pool, (PtrArrayForeachFunc)tr_free);
    tr_ptrArrayDestruct (&t->outgoingHandshakes, NULL);
    tr_ptrArrayDestruct (&t->peers, NULL);

    replicationFree (t);

    tr_free (t->requests);
    tr_free (t->pieces);
    tr_free (t);
}

static void peerCallbackFunc (tr_peer *, const tr_peer_event *, void *);

static void
rebuildWebseedArray (Torrent * t, tr_torrent * tor)
{
    int i;
    const tr_info * inf = &tor->info;

    /* clear the array */
    tr_ptrArrayDestruct (&t->webseeds, (PtrArrayForeachFunc)tr_webseedFree);
    t->webseeds = TR_PTR_ARRAY_INIT;

    /* repopulate it */
    for (i = 0; i < inf->webseedCount; ++i)
    {
        tr_webseed * w = tr_webseedNew (tor, inf->webseeds[i], peerCallbackFunc, t);
        tr_ptrArrayAppend (&t->webseeds, w);
    }
}

static Torrent*
torrentNew (tr_peerMgr * manager, tr_torrent * tor)
{
    Torrent * t;

    t = tr_new0 (Torrent, 1);
    t->manager = manager;
    t->tor = tor;
    t->pool = TR_PTR_ARRAY_INIT;
    t->peers = TR_PTR_ARRAY_INIT;
    t->webseeds = TR_PTR_ARRAY_INIT;
    t->outgoingHandshakes = TR_PTR_ARRAY_INIT;

    rebuildWebseedArray (t, tor);

    return t;
}

static void ensureMgrTimersExist (struct tr_peerMgr * m);

tr_peerMgr*
tr_peerMgrNew (tr_session * session)
{
    tr_peerMgr * m = tr_new0 (tr_peerMgr, 1);
    m->session = session;
    m->incomingHandshakes = TR_PTR_ARRAY_INIT;
    ensureMgrTimersExist (m);
    return m;
}

static void
deleteTimer (struct event ** t)
{
    if (*t != NULL)
    {
        event_free (*t);
        *t = NULL;
    }
}

static void
deleteTimers (struct tr_peerMgr * m)
{
    deleteTimer (&m->atomTimer);
    deleteTimer (&m->bandwidthTimer);
    deleteTimer (&m->rechokeTimer);
    deleteTimer (&m->refillUpkeepTimer);
}

void
tr_peerMgrFree (tr_peerMgr * manager)
{
    managerLock (manager);

    deleteTimers (manager);

    /* free the handshakes. Abort invokes handshakeDoneCB (), which removes
     * the item from manager->handshakes, so this is a little roundabout... */
    while (!tr_ptrArrayEmpty (&manager->incomingHandshakes))
        tr_handshakeAbort (tr_ptrArrayNth (&manager->incomingHandshakes, 0));

    tr_ptrArrayDestruct (&manager->incomingHandshakes, NULL);

    managerUnlock (manager);
    tr_free (manager);
}

static int
clientIsDownloadingFrom (const tr_torrent * tor, const tr_peer * peer)
{
    if (!tr_torrentHasMetadata (tor))
        return true;

    return peer->clientIsInterested && !peer->clientIsChoked;
}

static int
clientIsUploadingTo (const tr_peer * peer)
{
    return peer->peerIsInterested && !peer->peerIsChoked;
}

/***
****
***/

void
tr_peerMgrOnBlocklistChanged (tr_peerMgr * mgr)
{
    tr_torrent * tor = NULL;
    tr_session * session = mgr->session;

    /* we cache whether or not a peer is blocklisted...
       since the blocklist has changed, erase that cached value */
    while ((tor = tr_torrentNext (session, tor)))
    {
        int i;
        Torrent * t = tor->torrentPeers;
        const int n = tr_ptrArraySize (&t->pool);
        for (i=0; i<n; ++i) {
            struct peer_atom * atom = tr_ptrArrayNth (&t->pool, i);
            atom->blocklisted = -1;
        }
    }
}

static bool
isAtomBlocklisted (tr_session * session, struct peer_atom * atom)
{
    if (atom->blocklisted < 0)
        atom->blocklisted = tr_sessionIsAddressBlocked (session, &atom->addr);

    assert (tr_isBool (atom->blocklisted));
    return atom->blocklisted;
}


/***
****
***/

static void
atomSetSeedProbability (struct peer_atom * atom, int seedProbability)
{
    assert (atom != NULL);
    assert (-1<=seedProbability && seedProbability<=100);

    atom->seedProbability = seedProbability;

    if (seedProbability == 100)
        atom->flags |= ADDED_F_SEED_FLAG;
    else if (seedProbability != -1)
        atom->flags &= ~ADDED_F_SEED_FLAG;
}

static inline bool
atomIsSeed (const struct peer_atom * atom)
{
    return atom->seedProbability == 100;
}

static void
atomSetSeed (const Torrent * t, struct peer_atom * atom)
{
    if (!atomIsSeed (atom))
    {
        tordbg (t, "marking peer %s as a seed", tr_atomAddrStr (atom));

        atomSetSeedProbability (atom, 100);
    }
}


bool
tr_peerMgrPeerIsSeed (const tr_torrent  * tor,
                      const tr_address  * addr)
{
    bool isSeed = false;
    const Torrent * t = tor->torrentPeers;
    const struct peer_atom * atom = getExistingAtom (t, addr);

    if (atom)
        isSeed = atomIsSeed (atom);

    return isSeed;
}

void
tr_peerMgrSetUtpSupported (tr_torrent * tor, const tr_address * addr)
{
    struct peer_atom * atom = getExistingAtom (tor->torrentPeers, addr);

    if (atom)
        atom->flags |= ADDED_F_UTP_FLAGS;
}

void
tr_peerMgrSetUtpFailed (tr_torrent *tor, const tr_address *addr, bool failed)
{
    struct peer_atom * atom = getExistingAtom (tor->torrentPeers, addr);

    if (atom)
        atom->utp_failed = failed;
}


/**
***  REQUESTS
***
*** There are two data structures associated with managing block requests:
***
*** 1. Torrent::requests, an array of "struct block_request" which keeps
***    track of which blocks have been requested, and when, and by which peers.
***    This is list is used for (a) cancelling requests that have been pending
***    for too long and (b) avoiding duplicate requests before endgame.
***
*** 2. Torrent::pieces, an array of "struct weighted_piece" which lists the
***    pieces that we want to request. It's used to decide which blocks to
***    return next when tr_peerMgrGetBlockRequests () is called.
**/

/**
*** struct block_request
**/

static int
compareReqByBlock (const void * va, const void * vb)
{
    const struct block_request * a = va;
    const struct block_request * b = vb;

    /* primary key: block */
    if (a->block < b->block) return -1;
    if (a->block > b->block) return 1;

    /* secondary key: peer */
    if (a->peer < b->peer) return -1;
    if (a->peer > b->peer) return 1;

    return 0;
}

static void
requestListAdd (Torrent * t, tr_block_index_t block, tr_peer * peer)
{
    struct block_request key;

    /* ensure enough room is available... */
    if (t->requestCount + 1 >= t->requestAlloc)
    {
        const int CHUNK_SIZE = 128;
        t->requestAlloc += CHUNK_SIZE;
        t->requests = tr_renew (struct block_request,
                                t->requests, t->requestAlloc);
    }

    /* populate the record we're inserting */
    key.block = block;
    key.peer = peer;
    key.sentAt = tr_time ();

    /* insert the request to our array... */
    {
        bool exact;
        const int pos = tr_lowerBound (&key, t->requests, t->requestCount,
                                       sizeof (struct block_request),
                                       compareReqByBlock, &exact);
        assert (!exact);
        memmove (t->requests + pos + 1,
                 t->requests + pos,
                 sizeof (struct block_request) * (t->requestCount++ - pos));
        t->requests[pos] = key;
    }

    if (peer != NULL)
    {
        ++peer->pendingReqsToPeer;
        assert (peer->pendingReqsToPeer >= 0);
    }

    /*fprintf (stderr, "added request of block %lu from peer %s... "
                       "there are now %d block\n",
                     (unsigned long)block, tr_atomAddrStr (peer->atom), t->requestCount);*/
}

static struct block_request *
requestListLookup (Torrent * t, tr_block_index_t block, const tr_peer * peer)
{
    struct block_request key;
    key.block = block;
    key.peer = (tr_peer*) peer;

    return bsearch (&key, t->requests, t->requestCount,
                    sizeof (struct block_request),
                    compareReqByBlock);
}

/**
 * Find the peers are we currently requesting the block
 * with index @a block from and append them to @a peerArr.
 */
static void
getBlockRequestPeers (Torrent * t, tr_block_index_t block,
                      tr_ptrArray * peerArr)
{
    bool exact;
    int i, pos;
    struct block_request key;

    key.block = block;
    key.peer = NULL;
    pos = tr_lowerBound (&key, t->requests, t->requestCount,
                         sizeof (struct block_request),
                         compareReqByBlock, &exact);

    assert (!exact); /* shouldn't have a request with .peer == NULL */

    for (i = pos; i < t->requestCount; ++i)
    {
        if (t->requests[i].block != block)
            break;
        tr_ptrArrayAppend (peerArr, t->requests[i].peer);
    }
}

static void
decrementPendingReqCount (const struct block_request * b)
{
    if (b->peer != NULL)
        if (b->peer->pendingReqsToPeer > 0)
            --b->peer->pendingReqsToPeer;
}

static void
requestListRemove (Torrent * t, tr_block_index_t block, const tr_peer * peer)
{
    const struct block_request * b = requestListLookup (t, block, peer);
    if (b != NULL)
    {
        const int pos = b - t->requests;
        assert (pos < t->requestCount);

        decrementPendingReqCount (b);

        tr_removeElementFromArray (t->requests,
                                   pos,
                                   sizeof (struct block_request),
                                   t->requestCount--);

        /*fprintf (stderr, "removing request of block %lu from peer %s... "
                           "there are now %d block requests left\n",
                         (unsigned long)block, tr_atomAddrStr (peer->atom), t->requestCount);*/
    }
}

static int
countActiveWebseeds (const Torrent * t)
{
    int activeCount = 0;
    const tr_webseed ** w = (const tr_webseed **) tr_ptrArrayBase (&t->webseeds);
    const tr_webseed ** const wend = w + tr_ptrArraySize (&t->webseeds);

    for (; w!=wend; ++w)
        if (tr_webseedIsActive (*w))
            ++activeCount;

    return activeCount;
}

static bool
testForEndgame (const Torrent * t)
{
    /* we consider ourselves to be in endgame if the number of bytes
       we've got requested is >= the number of bytes left to download */
    return (t->requestCount * t->tor->blockSize)
               >= tr_cpLeftUntilDone (&t->tor->completion);
}

static void
updateEndgame (Torrent * t)
{
    assert (t->requestCount >= 0);

    if (!testForEndgame (t))
    {
        /* not in endgame */
        t->endgame = 0;
    }
    else if (!t->endgame) /* only recalculate when endgame first begins */
    {
        int numDownloading = 0;
        const tr_peer ** p = (const tr_peer **) tr_ptrArrayBase (&t->peers);
        const tr_peer ** const pend = p + tr_ptrArraySize (&t->peers);

        /* add the active bittorrent peers... */
        for (; p!=pend; ++p)
            if ((*p)->pendingReqsToPeer > 0)
                ++numDownloading;

        /* add the active webseeds... */
        numDownloading += countActiveWebseeds (t);

        /* average number of pending requests per downloading peer */
        t->endgame = t->requestCount / MAX (numDownloading, 1);
    }
}


/****
*****
*****  Piece List Manipulation / Accessors
*****
****/

static inline void
invalidatePieceSorting (Torrent * t)
{
    t->pieceSortState = PIECES_UNSORTED;
}

static const tr_torrent * weightTorrent;

static const uint16_t * weightReplication;

static void
setComparePieceByWeightTorrent (Torrent * t)
{
    if (!replicationExists (t))
        replicationNew (t);

    weightTorrent = t->tor;
    weightReplication = t->pieceReplication;
}

/* we try to create a "weight" s.t. high-priority pieces come before others,
 * and that partially-complete pieces come before empty ones. */
static int
comparePieceByWeight (const void * va, const void * vb)
{
    const struct weighted_piece * a = va;
    const struct weighted_piece * b = vb;
    int ia, ib, missing, pending;
    const tr_torrent * tor = weightTorrent;
    const uint16_t * rep = weightReplication;

    /* primary key: weight */
    missing = tr_cpMissingBlocksInPiece (&tor->completion, a->index);
    pending = a->requestCount;
    ia = missing > pending ? missing - pending : (tor->blockCountInPiece + pending);
    missing = tr_cpMissingBlocksInPiece (&tor->completion, b->index);
    pending = b->requestCount;
    ib = missing > pending ? missing - pending : (tor->blockCountInPiece + pending);
    if (ia < ib) return -1;
    if (ia > ib) return 1;

    /* secondary key: higher priorities go first */
    ia = tor->info.pieces[a->index].priority;
    ib = tor->info.pieces[b->index].priority;
    if (ia > ib) return -1;
    if (ia < ib) return 1;

    /* tertiary key: rarest first. */
    ia = rep[a->index];
    ib = rep[b->index];
    if (ia < ib) return -1;
    if (ia > ib) return 1;

    /* quaternary key: random */
    if (a->salt < b->salt) return -1;
    if (a->salt > b->salt) return 1;

    /* okay, they're equal */
    return 0;
}

static int
comparePieceByIndex (const void * va, const void * vb)
{
    const struct weighted_piece * a = va;
    const struct weighted_piece * b = vb;
    if (a->index < b->index) return -1;
    if (a->index > b->index) return 1;
    return 0;
}

static void
pieceListSort (Torrent * t, enum piece_sort_state state)
{
    assert (state==PIECES_SORTED_BY_INDEX
         || state==PIECES_SORTED_BY_WEIGHT);


    if (state == PIECES_SORTED_BY_WEIGHT)
    {
        setComparePieceByWeightTorrent (t);
        qsort (t->pieces, t->pieceCount, sizeof (struct weighted_piece), comparePieceByWeight);
    }
    else
        qsort (t->pieces, t->pieceCount, sizeof (struct weighted_piece), comparePieceByIndex);

    t->pieceSortState = state;
}

/**
 * These functions are useful for testing, but too expensive for nightly builds.
 * let's leave it disabled but add an easy hook to compile it back in
 */
#if 1
#define assertWeightedPiecesAreSorted(t)
#define assertReplicationCountIsExact(t)
#else
static void
assertWeightedPiecesAreSorted (Torrent * t)
{
    if (!t->endgame)
    {
        int i;
        setComparePieceByWeightTorrent (t);
        for (i=0; i<t->pieceCount-1; ++i)
            assert (comparePieceByWeight (&t->pieces[i], &t->pieces[i+1]) <= 0);
    }
}
static void
assertReplicationCountIsExact (Torrent * t)
{
    /* This assert might fail due to errors of implementations in other
     * clients. It happens when receiving duplicate bitfields/HaveAll/HaveNone
     * from a client. If a such a behavior is noticed,
     * a bug report should be filled to the faulty client. */

    size_t piece_i;
    const uint16_t * rep = t->pieceReplication;
    const size_t piece_count = t->pieceReplicationSize;
    const tr_peer ** peers = (const tr_peer**) tr_ptrArrayBase (&t->peers);
    const int peer_count = tr_ptrArraySize (&t->peers);

    assert (piece_count == t->tor->info.pieceCount);

    for (piece_i=0; piece_i<piece_count; ++piece_i)
    {
        int peer_i;
        uint16_t r = 0;

        for (peer_i=0; peer_i<peer_count; ++peer_i)
            if (tr_bitsetHas (&peers[peer_i]->have, piece_i))
                ++r;

        assert (rep[piece_i] == r);
    }
}
#endif

static struct weighted_piece *
pieceListLookup (Torrent * t, tr_piece_index_t index)
{
    int i;

    for (i=0; i<t->pieceCount; ++i)
        if (t->pieces[i].index == index)
            return &t->pieces[i];

    return NULL;
}

static void
pieceListRebuild (Torrent * t)
{

    if (!tr_torrentIsSeed (t->tor))
    {
        tr_piece_index_t i;
        tr_piece_index_t * pool;
        tr_piece_index_t poolCount = 0;
        const tr_torrent * tor = t->tor;
        const tr_info * inf = tr_torrentInfo (tor);
        struct weighted_piece * pieces;
        int pieceCount;

        /* build the new list */
        pool = tr_new (tr_piece_index_t, inf->pieceCount);
        for (i=0; i<inf->pieceCount; ++i)
            if (!inf->pieces[i].dnd)
                if (!tr_cpPieceIsComplete (&tor->completion, i))
                    pool[poolCount++] = i;
        pieceCount = poolCount;
        pieces = tr_new0 (struct weighted_piece, pieceCount);
        for (i=0; i<poolCount; ++i) {
            struct weighted_piece * piece = pieces + i;
            piece->index = pool[i];
            piece->requestCount = 0;
            piece->salt = tr_cryptoWeakRandInt (4096);
        }

        /* if we already had a list of pieces, merge it into
         * the new list so we don't lose its requestCounts */
        if (t->pieces != NULL)
        {
            struct weighted_piece * o = t->pieces;
            struct weighted_piece * oend = o + t->pieceCount;
            struct weighted_piece * n = pieces;
            struct weighted_piece * nend = n + pieceCount;

            pieceListSort (t, PIECES_SORTED_BY_INDEX);

            while (o!=oend && n!=nend) {
                if (o->index < n->index)
                    ++o;
                else if (o->index > n->index)
                    ++n;
                else
                    *n++ = *o++;
            }

            tr_free (t->pieces);
        }

        t->pieces = pieces;
        t->pieceCount = pieceCount;

        pieceListSort (t, PIECES_SORTED_BY_WEIGHT);

        /* cleanup */
        tr_free (pool);
    }
}

static void
pieceListRemovePiece (Torrent * t, tr_piece_index_t piece)
{
    struct weighted_piece * p;

    if ((p = pieceListLookup (t, piece)))
    {
        const int pos = p - t->pieces;

        tr_removeElementFromArray (t->pieces,
                                   pos,
                                   sizeof (struct weighted_piece),
                                   t->pieceCount--);

        if (t->pieceCount == 0)
        {
            tr_free (t->pieces);
            t->pieces = NULL;
        }
    }
}

static void
pieceListResortPiece (Torrent * t, struct weighted_piece * p)
{
    int pos;
    bool isSorted = true;

    if (p == NULL)
        return;

    /* is the torrent already sorted? */
    pos = p - t->pieces;
    setComparePieceByWeightTorrent (t);
    if (isSorted && (pos > 0) && (comparePieceByWeight (p-1, p) > 0))
        isSorted = false;
    if (isSorted && (pos < t->pieceCount - 1) && (comparePieceByWeight (p, p+1) > 0))
        isSorted = false;

    if (t->pieceSortState != PIECES_SORTED_BY_WEIGHT)
    {
       pieceListSort (t, PIECES_SORTED_BY_WEIGHT);
       isSorted = true;
    }

    /* if it's not sorted, move it around */
    if (!isSorted)
    {
        bool exact;
        const struct weighted_piece tmp = *p;

        tr_removeElementFromArray (t->pieces,
                                   pos,
                                   sizeof (struct weighted_piece),
                                   t->pieceCount--);

        pos = tr_lowerBound (&tmp, t->pieces, t->pieceCount,
                             sizeof (struct weighted_piece),
                             comparePieceByWeight, &exact);

        memmove (&t->pieces[pos + 1],
                 &t->pieces[pos],
                 sizeof (struct weighted_piece) * (t->pieceCount++ - pos));

        t->pieces[pos] = tmp;
    }

    assertWeightedPiecesAreSorted (t);
}

static void
pieceListRemoveRequest (Torrent * t, tr_block_index_t block)
{
    struct weighted_piece * p;
    const tr_piece_index_t index = tr_torBlockPiece (t->tor, block);

    if (((p = pieceListLookup (t, index))) && (p->requestCount > 0))
    {
        --p->requestCount;
        pieceListResortPiece (t, p);
    }
}


/****
*****
*****  Replication count (for rarest first policy)
*****
****/

/**
 * Increase the replication count of this piece and sort it if the
 * piece list is already sorted
 */
static void
tr_incrReplicationOfPiece (Torrent * t, const size_t index)
{
    assert (replicationExists (t));
    assert (t->pieceReplicationSize == t->tor->info.pieceCount);

    /* One more replication of this piece is present in the swarm */
    ++t->pieceReplication[index];

    /* we only resort the piece if the list is already sorted */
    if (t->pieceSortState == PIECES_SORTED_BY_WEIGHT)
        pieceListResortPiece (t, pieceListLookup (t, index));
}

/**
 * Increases the replication count of pieces present in the bitfield
 */
static void
tr_incrReplicationFromBitfield (Torrent * t, const tr_bitfield * b)
{
    size_t i;
    uint16_t * rep = t->pieceReplication;
    const size_t n = t->tor->info.pieceCount;

    assert (replicationExists (t));

    for (i=0; i<n; ++i)
        if (tr_bitfieldHas (b, i))
            ++rep[i];

    if (t->pieceSortState == PIECES_SORTED_BY_WEIGHT)
        invalidatePieceSorting (t);
}

/**
 * Increase the replication count of every piece
 */
static void
tr_incrReplication (Torrent * t)
{
    int i;
    const int n = t->pieceReplicationSize;

    assert (replicationExists (t));
    assert (t->pieceReplicationSize == t->tor->info.pieceCount);

    for (i=0; i<n; ++i)
        ++t->pieceReplication[i];
}

/**
 * Decrease the replication count of pieces present in the bitset.
 */
static void
tr_decrReplicationFromBitfield (Torrent * t, const tr_bitfield * b)
{
    int i;
    const int n = t->pieceReplicationSize;

    assert (replicationExists (t));
    assert (t->pieceReplicationSize == t->tor->info.pieceCount);

    if (tr_bitfieldHasAll (b))
    {
        for (i=0; i<n; ++i)
            --t->pieceReplication[i];
    }
    else if (!tr_bitfieldHasNone (b))
    {
        for (i=0; i<n; ++i)
            if (tr_bitfieldHas (b, i))
                --t->pieceReplication[i];

        if (t->pieceSortState == PIECES_SORTED_BY_WEIGHT)
            invalidatePieceSorting (t);
    }
}

/**
***
**/

void
tr_peerMgrRebuildRequests (tr_torrent * tor)
{
    assert (tr_isTorrent (tor));

    pieceListRebuild (tor->torrentPeers);
}

void
tr_peerMgrGetNextRequests (tr_torrent           * tor,
                           tr_peer              * peer,
                           int                    numwant,
                           tr_block_index_t     * setme,
                           int                  * numgot,
                           bool                   get_intervals)
{
    int i;
    int got;
    Torrent * t;
    struct weighted_piece * pieces;
    const tr_bitfield * const have = &peer->have;

    /* sanity clause */
    assert (tr_isTorrent (tor));
    assert (peer->clientIsInterested);
    assert (!peer->clientIsChoked);
    assert (numwant > 0);

    /* walk through the pieces and find blocks that should be requested */
    got = 0;
    t = tor->torrentPeers;

    /* prep the pieces list */
    if (t->pieces == NULL)
        pieceListRebuild (t);

    if (t->pieceSortState != PIECES_SORTED_BY_WEIGHT)
        pieceListSort (t, PIECES_SORTED_BY_WEIGHT);

    assertReplicationCountIsExact (t);
    assertWeightedPiecesAreSorted (t);

    updateEndgame (t);
    pieces = t->pieces;
    for (i=0; i<t->pieceCount && got<numwant; ++i)
    {
        struct weighted_piece * p = pieces + i;

        /* if the peer has this piece that we want... */
        if (tr_bitfieldHas (have, p->index))
        {
            tr_block_index_t b;
            tr_block_index_t first;
            tr_block_index_t last;
            tr_ptrArray peerArr = TR_PTR_ARRAY_INIT;

            tr_torGetPieceBlockRange (tor, p->index, &first, &last);

            for (b=first; b<=last && (got<numwant || (get_intervals && setme[2*got-1] == b-1)); ++b)
            {
                int peerCount;
                tr_peer ** peers;

                /* don't request blocks we've already got */
                if (tr_cpBlockIsComplete (&tor->completion, b))
                    continue;

                /* always add peer if this block has no peers yet */
                tr_ptrArrayClear (&peerArr);
                getBlockRequestPeers (t, b, &peerArr);
                peers = (tr_peer **) tr_ptrArrayPeek (&peerArr, &peerCount);
                if (peerCount != 0)
                {
                    /* don't make a second block request until the endgame */
                    if (!t->endgame)
                        continue;

                    /* don't have more than two peers requesting this block */
                    if (peerCount > 1)
                        continue;

                    /* don't send the same request to the same peer twice */
                    if (peer == peers[0])
                        continue;

                    /* in the endgame allow an additional peer to download a
                       block but only if the peer seems to be handling requests
                       relatively fast */
                    if (peer->pendingReqsToPeer + numwant - got < t->endgame)
                        continue;
                }

                /* update the caller's table */
                if (!get_intervals) {
                    setme[got++] = b;
                }
                /* if intervals are requested two array entries are necessarry:
                   one for the interval's starting block and one for its end block */
                else if (got && setme[2 * got - 1] == b - 1 && b != first) {
                    /* expand the last interval */
                    ++setme[2 * got - 1];
                }
                else {
                    /* begin a new interval */
                    setme[2 * got] = setme[2 * got + 1] = b;
                    ++got;
                }

                /* update our own tables */
                requestListAdd (t, b, peer);
                ++p->requestCount;
            }

            tr_ptrArrayDestruct (&peerArr, NULL);
        }
    }

    /* In most cases we've just changed the weights of a small number of pieces.
     * So rather than qsort ()ing the entire array, it's faster to apply an
     * adaptive insertion sort algorithm. */
    if (got > 0)
    {
        /* not enough requests || last piece modified */
        if (i == t->pieceCount) --i;

        setComparePieceByWeightTorrent (t);
        while (--i >= 0)
        {
            bool exact;

            /* relative position! */
            const int newpos = tr_lowerBound (&t->pieces[i], &t->pieces[i + 1],
                                              t->pieceCount - (i + 1),
                                              sizeof (struct weighted_piece),
                                              comparePieceByWeight, &exact);
            if (newpos > 0)
            {
                const struct weighted_piece piece = t->pieces[i];
                memmove (&t->pieces[i],
                         &t->pieces[i + 1],
                         sizeof (struct weighted_piece) * (newpos));
                t->pieces[i + newpos] = piece;
            }
        }
    }

    assertWeightedPiecesAreSorted (t);
    *numgot = got;
}

bool
tr_peerMgrDidPeerRequest (const tr_torrent  * tor,
                          const tr_peer     * peer,
                          tr_block_index_t    block)
{
    const Torrent * t = tor->torrentPeers;
    return requestListLookup ((Torrent*)t, block, peer) != NULL;
}

/* cancel requests that are too old */
static void
refillUpkeep (int foo UNUSED, short bar UNUSED, void * vmgr)
{
    time_t now;
    time_t too_old;
    tr_torrent * tor;
    int cancel_buflen = 0;
    struct block_request * cancel = NULL;
    tr_peerMgr * mgr = vmgr;
    managerLock (mgr);

    now = tr_time ();
    too_old = now - REQUEST_TTL_SECS;

    /* alloc the temporary "cancel" buffer */
    tor = NULL;
    while ((tor = tr_torrentNext (mgr->session, tor)))
        cancel_buflen = MAX (cancel_buflen, tor->torrentPeers->requestCount);
    if (cancel_buflen > 0)
        cancel = tr_new (struct block_request, cancel_buflen);

    /* prune requests that are too old */
    tor = NULL;
    while ((tor = tr_torrentNext (mgr->session, tor)))
    {
        Torrent * t = tor->torrentPeers;
        const int n = t->requestCount;
        if (n > 0)
        {
            int keepCount = 0;
            int cancelCount = 0;
            const struct block_request * it;
            const struct block_request * end;

            for (it=t->requests, end=it+n; it!=end; ++it)
            {
                if ((it->sentAt <= too_old) && it->peer->msgs && !tr_peerMsgsIsReadingBlock (it->peer->msgs, it->block))
                    cancel[cancelCount++] = *it;
                else
                {
                    if (it != &t->requests[keepCount])
                        t->requests[keepCount] = *it;
                    keepCount++;
                }
            }

            /* prune out the ones we aren't keeping */
            t->requestCount = keepCount;

            /* send cancel messages for all the "cancel" ones */
            for (it=cancel, end=it+cancelCount; it!=end; ++it) {
                if ((it->peer != NULL) && (it->peer->msgs != NULL)) {
                    tr_historyAdd (&it->peer->cancelsSentToPeer, now, 1);
                    tr_peerMsgsCancel (it->peer->msgs, it->block);
                    decrementPendingReqCount (it);
                }
            }

            /* decrement the pending request counts for the timed-out blocks */
            for (it=cancel, end=it+cancelCount; it!=end; ++it)
                pieceListRemoveRequest (t, it->block);
        }
    }

    tr_free (cancel);
    tr_timerAddMsec (mgr->refillUpkeepTimer, REFILL_UPKEEP_PERIOD_MSEC);
    managerUnlock (mgr);
}

static void
addStrike (Torrent * t, tr_peer * peer)
{
    tordbg (t, "increasing peer %s strike count to %d",
            tr_atomAddrStr (peer->atom), peer->strikes + 1);

    if (++peer->strikes >= MAX_BAD_PIECES_PER_PEER)
    {
        struct peer_atom * atom = peer->atom;
        atom->flags2 |= MYFLAG_BANNED;
        peer->doPurge = 1;
        tordbg (t, "banning peer %s", tr_atomAddrStr (atom));
    }
}

static void
gotBadPiece (Torrent * t, tr_piece_index_t pieceIndex)
{
    tr_torrent *   tor = t->tor;
    const uint32_t byteCount = tr_torPieceCountBytes (tor, pieceIndex);

    tor->corruptCur += byteCount;
    tor->downloadedCur -= MIN (tor->downloadedCur, byteCount);

    tr_announcerAddBytes (tor, TR_ANN_CORRUPT, byteCount);
}

static void
peerSuggestedPiece (Torrent            * t UNUSED,
                    tr_peer            * peer UNUSED,
                    tr_piece_index_t     pieceIndex UNUSED,
                    int                  isFastAllowed UNUSED)
{
#if 0
    assert (t);
    assert (peer);
    assert (peer->msgs);

    /* is this a valid piece? */
    if (pieceIndex >= t->tor->info.pieceCount)
        return;

    /* don't ask for it if we've already got it */
    if (tr_cpPieceIsComplete (t->tor->completion, pieceIndex))
        return;

    /* don't ask for it if they don't have it */
    if (!tr_bitfieldHas (peer->have, pieceIndex))
        return;

    /* don't ask for it if we're choked and it's not fast */
    if (!isFastAllowed && peer->clientIsChoked)
        return;

    /* request the blocks that we don't have in this piece */
    {
        tr_block_index_t b;
        tr_block_index_t first;
        tr_block_index_t last;
        const tr_torrent * tor = t->tor;

        tr_torGetPieceBlockRange (t->tor, pieceIndex, &first, &last);

        for (b=first; b<=last; ++b)
        {
            if (!tr_cpBlockIsComplete (tor->completion, b))
            {
                const uint32_t offset = getBlockOffsetInPiece (tor, b);
                const uint32_t length = tr_torBlockCountBytes (tor, b);
                tr_peerMsgsAddRequest (peer->msgs, pieceIndex, offset, length);
                incrementPieceRequests (t, pieceIndex);
            }
        }
    }
#endif
}

static void
removeRequestFromTables (Torrent * t, tr_block_index_t block, const tr_peer * peer)
{
    requestListRemove (t, block, peer);
    pieceListRemoveRequest (t, block);
}

/* peer choked us, or maybe it disconnected.
   either way we need to remove all its requests */
static void
peerDeclinedAllRequests (Torrent * t, const tr_peer * peer)
{
    int i, n;
    tr_block_index_t * blocks = tr_new (tr_block_index_t, t->requestCount);

    for (i=n=0; i<t->requestCount; ++i)
        if (peer == t->requests[i].peer)
            blocks[n++] = t->requests[i].block;

    for (i=0; i<n; ++i)
        removeRequestFromTables (t, blocks[i], peer);

    tr_free (blocks);
}

static void tr_peerMgrSetBlame (tr_torrent *, tr_piece_index_t, int);

static void
peerCallbackFunc (tr_peer * peer, const tr_peer_event * e, void * vt)
{
    Torrent * t = vt;

    torrentLock (t);

    assert (peer != NULL);

    switch (e->eventType)
    {
        case TR_PEER_PEER_GOT_DATA:
        {
            const time_t now = tr_time ();
            tr_torrent * tor = t->tor;

            if (e->wasPieceData)
            {
                tor->uploadedCur += e->length;
                tr_announcerAddBytes (tor, TR_ANN_UP, e->length);
                tr_torrentSetActivityDate (tor, now);
                tr_torrentSetDirty (tor);
            }

            /* update the stats */
            if (e->wasPieceData)
                tr_statsAddUploaded (tor->session, e->length);

            /* update our atom */
            if (peer->atom && e->wasPieceData)
                peer->atom->piece_data_time = now;

            break;
        }

        case TR_PEER_CLIENT_GOT_HAVE:
            if (replicationExists (t)) {
                tr_incrReplicationOfPiece (t, e->pieceIndex);
                assertReplicationCountIsExact (t);
            }
            break;

        case TR_PEER_CLIENT_GOT_HAVE_ALL:
            if (replicationExists (t)) {
                tr_incrReplication (t);
                assertReplicationCountIsExact (t);
            }
            break;

        case TR_PEER_CLIENT_GOT_HAVE_NONE:
            /* noop */
            break;

        case TR_PEER_CLIENT_GOT_BITFIELD:
            assert (e->bitfield != NULL);
            if (replicationExists (t)) {
                tr_incrReplicationFromBitfield (t, e->bitfield);
                assertReplicationCountIsExact (t);
            }
            break;

        case TR_PEER_CLIENT_GOT_REJ: {
            tr_block_index_t b = _tr_block (t->tor, e->pieceIndex, e->offset);
            if (b < t->tor->blockCount)
                removeRequestFromTables (t, b, peer);
            else
                tordbg (t, "Peer %s sent an out-of-range reject message",
                           tr_atomAddrStr (peer->atom));
            break;
        }

        case TR_PEER_CLIENT_GOT_CHOKE:
            peerDeclinedAllRequests (t, peer);
            break;

        case TR_PEER_CLIENT_GOT_PORT:
            if (peer->atom)
                peer->atom->port = e->port;
            break;

        case TR_PEER_CLIENT_GOT_SUGGEST:
            peerSuggestedPiece (t, peer, e->pieceIndex, false);
            break;

        case TR_PEER_CLIENT_GOT_ALLOWED_FAST:
            peerSuggestedPiece (t, peer, e->pieceIndex, true);
            break;

        case TR_PEER_CLIENT_GOT_DATA:
        {
            const time_t now = tr_time ();
            tr_torrent * tor = t->tor;

            if (e->wasPieceData)
            {
                tor->downloadedCur += e->length;
                tr_torrentSetActivityDate (tor, now);
                tr_torrentSetDirty (tor);
            }

            /* update the stats */
            if (e->wasPieceData)
                tr_statsAddDownloaded (tor->session, e->length);

            /* update our atom */
            if (peer->atom && e->wasPieceData)
                peer->atom->piece_data_time = now;

            break;
        }

        case TR_PEER_CLIENT_GOT_BLOCK:
        {
            tr_torrent * tor = t->tor;
            tr_block_index_t block = _tr_block (tor, e->pieceIndex, e->offset);
            int i, peerCount;
            tr_peer ** peers;
            tr_ptrArray peerArr = TR_PTR_ARRAY_INIT;

            removeRequestFromTables (t, block, peer);
            getBlockRequestPeers (t, block, &peerArr);
            peers = (tr_peer **) tr_ptrArrayPeek (&peerArr, &peerCount);

            /* remove additional block requests and send cancel to peers */
            for (i=0; i<peerCount; i++) {
                tr_peer * p = peers[i];
                assert (p != peer);
                if (p->msgs) {
                    tr_historyAdd (&p->cancelsSentToPeer, tr_time (), 1);
                    tr_peerMsgsCancel (p->msgs, block);
                }
                removeRequestFromTables (t, block, p);
            }

            tr_ptrArrayDestruct (&peerArr, false);

            tr_historyAdd (&peer->blocksSentToClient, tr_time (), 1);

            if (tr_cpBlockIsComplete (&tor->completion, block))
            {
                /* we already have this block... */
                const uint32_t n = tr_torBlockCountBytes (tor, block);
                tor->downloadedCur -= MIN (tor->downloadedCur, n);
                tordbg (t, "we have this block already...");
            }
            else
            {
                tr_cpBlockAdd (&tor->completion, block);
                pieceListResortPiece (t, pieceListLookup (t, e->pieceIndex));
                tr_torrentSetDirty (tor);

                if (tr_cpPieceIsComplete (&tor->completion, e->pieceIndex))
                {
                    const tr_piece_index_t p = e->pieceIndex;
                    const bool ok = tr_torrentCheckPiece (tor, p);

                    tordbg (t, "[LAZY] checked just-completed piece %zu", (size_t)p);

                    if (!ok)
                    {
                        tr_torerr (tor, _("Piece %lu, which was just downloaded, failed its checksum test"),
                                 (unsigned long)p);
                    }

                    tr_peerMgrSetBlame (tor, p, ok);

                    if (!ok)
                    {
                        gotBadPiece (t, p);
                    }
                    else
                    {
                        int i;
                        int peerCount;
                        tr_peer ** peers;
                        tr_file_index_t fileIndex;

                        /* only add this to downloadedCur if we got it from a peer --
                         * webseeds shouldn't count against our ratio. As one tracker
                         * admin put it, "Those pieces are downloaded directly from the
                         * content distributor, not the peers, it is the tracker's job
                         * to manage the swarms, not the web server and does not fit
                         * into the jurisdiction of the tracker." */
                        if (peer->msgs != NULL) {
                            const uint32_t n = tr_torPieceCountBytes (tor, p);
                            tr_announcerAddBytes (tor, TR_ANN_DOWN, n);
                        }

                        peerCount = tr_ptrArraySize (&t->peers);
                        peers = (tr_peer**) tr_ptrArrayBase (&t->peers);
                        for (i=0; i<peerCount; ++i)
                            tr_peerMsgsHave (peers[i]->msgs, p);

                        for (fileIndex=0; fileIndex<tor->info.fileCount; ++fileIndex) {
                            const tr_file * file = &tor->info.files[fileIndex];
                            if ((file->firstPiece <= p) && (p <= file->lastPiece)) {
                                if (tr_cpFileIsComplete (&tor->completion, fileIndex)) {
                                    tr_cacheFlushFile (tor->session->cache, tor, fileIndex);
                                    tr_torrentFileCompleted (tor, fileIndex);
                                }
                            }
                        }

                        pieceListRemovePiece (t, p);
                    }
                }

                t->needsCompletenessCheck = true;
            }
            break;
        }

        case TR_PEER_ERROR:
            if ((e->err == ERANGE) || (e->err == EMSGSIZE) || (e->err == ENOTCONN))
            {
                /* some protocol error from the peer */
                peer->doPurge = 1;
                tordbg (t, "setting %s doPurge flag because we got an ERANGE, EMSGSIZE, or ENOTCONN error",
                        tr_atomAddrStr (peer->atom));
            }
            else
            {
                tordbg (t, "unhandled error: %s", tr_strerror (e->err));
            }
            break;

        default:
            assert (0);
    }

    torrentUnlock (t);
}

static int
getDefaultShelfLife (uint8_t from)
{
    /* in general, peers obtained from firsthand contact
     * are better than those from secondhand, etc etc */
    switch (from)
    {
        case TR_PEER_FROM_INCOMING : return 60 * 60 * 6;
        case TR_PEER_FROM_LTEP     : return 60 * 60 * 6;
        case TR_PEER_FROM_TRACKER  : return 60 * 60 * 3;
        case TR_PEER_FROM_DHT      : return 60 * 60 * 3;
        case TR_PEER_FROM_PEX      : return 60 * 60 * 2;
        case TR_PEER_FROM_RESUME   : return 60 * 60;
        case TR_PEER_FROM_LPD      : return 10 * 60;
        default                    : return 60 * 60;
    }
}

static void
ensureAtomExists (Torrent           * t,
                  const tr_address  * addr,
                  const tr_port       port,
                  const uint8_t       flags,
                  const int8_t        seedProbability,
                  const uint8_t       from)
{
    struct peer_atom * a;

    assert (tr_address_is_valid (addr));
    assert (from < TR_PEER_FROM__MAX);

    a = getExistingAtom (t, addr);

    if (a == NULL)
    {
        const int jitter = tr_cryptoWeakRandInt (60*10);
        a = tr_new0 (struct peer_atom, 1);
        a->addr = *addr;
        a->port = port;
        a->flags = flags;
        a->fromFirst = from;
        a->fromBest = from;
        a->shelf_date = tr_time () + getDefaultShelfLife (from) + jitter;
        a->blocklisted = -1;
        atomSetSeedProbability (a, seedProbability);
        tr_ptrArrayInsertSorted (&t->pool, a, compareAtomsByAddress);

        tordbg (t, "got a new atom: %s", tr_atomAddrStr (a));
    }
    else
    {
        if (from < a->fromBest)
            a->fromBest = from;

        if (a->seedProbability == -1)
            atomSetSeedProbability (a, seedProbability);

        a->flags |= flags;
    }
}

static int
getMaxPeerCount (const tr_torrent * tor)
{
    return tor->maxConnectedPeers;
}

static int
getPeerCount (const Torrent * t)
{
    return tr_ptrArraySize (&t->peers);/* + tr_ptrArraySize (&t->outgoingHandshakes); */
}

/* FIXME: this is kind of a mess. */
static bool
myHandshakeDoneCB (tr_handshake  * handshake,
                   tr_peerIo     * io,
                   bool            readAnythingFromPeer,
                   bool            isConnected,
                   const uint8_t * peer_id,
                   void          * vmanager)
{
    bool               ok = isConnected;
    bool               success = false;
    tr_port            port;
    const tr_address * addr;
    tr_peerMgr       * manager = vmanager;
    Torrent          * t;
    tr_handshake     * ours;

    assert (io);
    assert (tr_isBool (ok));

    t = tr_peerIoHasTorrentHash (io)
        ? getExistingTorrent (manager, tr_peerIoGetTorrentHash (io))
        : NULL;

    if (tr_peerIoIsIncoming (io))
        ours = tr_ptrArrayRemoveSorted (&manager->incomingHandshakes,
                                        handshake, handshakeCompare);
    else if (t)
        ours = tr_ptrArrayRemoveSorted (&t->outgoingHandshakes,
                                        handshake, handshakeCompare);
    else
        ours = handshake;

    assert (ours);
    assert (ours == handshake);

    if (t)
        torrentLock (t);

    addr = tr_peerIoGetAddress (io, &port);

    if (!ok || !t || !t->isRunning)
    {
        if (t)
        {
            struct peer_atom * atom = getExistingAtom (t, addr);
            if (atom)
            {
                ++atom->numFails;

                if (!readAnythingFromPeer)
                {
                    tordbg (t, "marking peer %s as unreachable... numFails is %d", tr_atomAddrStr (atom), (int)atom->numFails);
                    atom->flags2 |= MYFLAG_UNREACHABLE;
                }
            }
        }
    }
    else /* looking good */
    {
        struct peer_atom * atom;

        ensureAtomExists (t, addr, port, 0, -1, TR_PEER_FROM_INCOMING);
        atom = getExistingAtom (t, addr);
        atom->time = tr_time ();
        atom->piece_data_time = 0;
        atom->lastConnectionAt = tr_time ();

        if (!tr_peerIoIsIncoming (io))
        {
            atom->flags |= ADDED_F_CONNECTABLE;
            atom->flags2 &= ~MYFLAG_UNREACHABLE;
        }

        /* In principle, this flag specifies whether the peer groks uTP,
           not whether it's currently connected over uTP. */
        if (io->utp_socket)
            atom->flags |= ADDED_F_UTP_FLAGS;

        if (atom->flags2 & MYFLAG_BANNED)
        {
            tordbg (t, "banned peer %s tried to reconnect",
                    tr_atomAddrStr (atom));
        }
        else if (tr_peerIoIsIncoming (io)
               && (getPeerCount (t) >= getMaxPeerCount (t->tor)))

        {
        }
        else
        {
            tr_peer * peer = atom->peer;

            if (peer) /* we already have this peer */
            {
            }
            else
            {
                peer = getPeer (t, atom);
                tr_free (peer->client);

                if (!peer_id)
                    peer->client = NULL;
                else {
                    char client[128];
                    tr_clientForId (client, sizeof (client), peer_id);
                    peer->client = tr_strdup (client);
                }

                peer->io = tr_handshakeStealIO (handshake); /* this steals its refcount too, which is
                                                                balanced by our unref in peerDelete ()  */
                tr_peerIoSetParent (peer->io, &t->tor->bandwidth);
                tr_peerMsgsNew (t->tor, peer, peerCallbackFunc, t);

                success = true;
            }
        }
    }

    if (t)
        torrentUnlock (t);

    return success;
}

void
tr_peerMgrAddIncoming (tr_peerMgr * manager,
                       tr_address * addr,
                       tr_port      port,
                       int          socket,
                       struct UTPSocket * utp_socket)
{
    tr_session * session;

    managerLock (manager);

    assert (tr_isSession (manager->session));
    session = manager->session;

    if (tr_sessionIsAddressBlocked (session, addr))
    {
        tr_dbg ("Banned IP address \"%s\" tried to connect to us", tr_address_to_string (addr));
        if (socket >= 0)
            tr_netClose (session, socket);
        else
            UTP_Close (utp_socket);
    }
    else if (getExistingHandshake (&manager->incomingHandshakes, addr))
    {
        if (socket >= 0)
            tr_netClose (session, socket);
        else
            UTP_Close (utp_socket);
    }
    else /* we don't have a connection to them yet... */
    {
        tr_peerIo *    io;
        tr_handshake * handshake;

        io = tr_peerIoNewIncoming (session, &session->bandwidth, addr, port, socket, utp_socket);

        handshake = tr_handshakeNew (io,
                                     session->encryptionMode,
                                     myHandshakeDoneCB,
                                     manager);

        tr_peerIoUnref (io); /* balanced by the implicit ref in tr_peerIoNewIncoming () */

        tr_ptrArrayInsertSorted (&manager->incomingHandshakes, handshake,
                                 handshakeCompare);
    }

    managerUnlock (manager);
}

void
tr_peerMgrAddPex (tr_torrent * tor, uint8_t from,
                  const tr_pex * pex, int8_t seedProbability)
{
    if (tr_isPex (pex)) /* safeguard against corrupt data */
    {
        Torrent * t = tor->torrentPeers;
        managerLock (t->manager);

        if (!tr_sessionIsAddressBlocked (t->manager->session, &pex->addr))
            if (tr_address_is_valid_for_peers (&pex->addr, pex->port))
                ensureAtomExists (t, &pex->addr, pex->port, pex->flags, seedProbability, from);

        managerUnlock (t->manager);
    }
}

void
tr_peerMgrMarkAllAsSeeds (tr_torrent * tor)
{
    Torrent * t = tor->torrentPeers;
    const int n = tr_ptrArraySize (&t->pool);
    struct peer_atom ** it = (struct peer_atom**) tr_ptrArrayBase (&t->pool);
    struct peer_atom ** end = it + n;

    while (it != end)
        atomSetSeed (t, *it++);
}

tr_pex *
tr_peerMgrCompactToPex (const void *    compact,
                        size_t          compactLen,
                        const uint8_t * added_f,
                        size_t          added_f_len,
                        size_t *        pexCount)
{
    size_t          i;
    size_t          n = compactLen / 6;
    const uint8_t * walk = compact;
    tr_pex *        pex = tr_new0 (tr_pex, n);

    for (i = 0; i < n; ++i)
    {
        pex[i].addr.type = TR_AF_INET;
        memcpy (&pex[i].addr.addr, walk, 4); walk += 4;
        memcpy (&pex[i].port, walk, 2); walk += 2;
        if (added_f && (n == added_f_len))
            pex[i].flags = added_f[i];
    }

    *pexCount = n;
    return pex;
}

tr_pex *
tr_peerMgrCompact6ToPex (const void    * compact,
                         size_t          compactLen,
                         const uint8_t * added_f,
                         size_t          added_f_len,
                         size_t        * pexCount)
{
    size_t          i;
    size_t          n = compactLen / 18;
    const uint8_t * walk = compact;
    tr_pex *        pex = tr_new0 (tr_pex, n);

    for (i = 0; i < n; ++i)
    {
        pex[i].addr.type = TR_AF_INET6;
        memcpy (&pex[i].addr.addr.addr6.s6_addr, walk, 16); walk += 16;
        memcpy (&pex[i].port, walk, 2); walk += 2;
        if (added_f && (n == added_f_len))
            pex[i].flags = added_f[i];
    }

    *pexCount = n;
    return pex;
}

tr_pex *
tr_peerMgrArrayToPex (const void * array,
                      size_t       arrayLen,
                      size_t      * pexCount)
{
    size_t          i;
    size_t          n = arrayLen / (sizeof (tr_address) + 2);
    /*size_t          n = arrayLen / sizeof (tr_peerArrayElement);*/
    const uint8_t * walk = array;
    tr_pex        * pex = tr_new0 (tr_pex, n);

    for (i = 0 ; i < n ; i++) {
        memcpy (&pex[i].addr, walk, sizeof (tr_address));
        memcpy (&pex[i].port, walk + sizeof (tr_address), 2);
        pex[i].flags = 0x00;
        walk += sizeof (tr_address) + 2;
    }

    *pexCount = n;
    return pex;
}

/**
***
**/

static void
tr_peerMgrSetBlame (tr_torrent     * tor,
                    tr_piece_index_t pieceIndex,
                    int              success)
{
    if (!success)
    {
        int        peerCount, i;
        Torrent *  t = tor->torrentPeers;
        tr_peer ** peers;

        assert (torrentIsLocked (t));

        peers = (tr_peer **) tr_ptrArrayPeek (&t->peers, &peerCount);
        for (i = 0; i < peerCount; ++i)
        {
            tr_peer * peer = peers[i];
            if (tr_bitfieldHas (&peer->blame, pieceIndex))
            {
                tordbg (t, "peer %s contributed to corrupt piece (%d); now has %d strikes",
                        tr_atomAddrStr (peer->atom),
                        pieceIndex, (int)peer->strikes + 1);
                addStrike (t, peer);
            }
        }
    }
}

int
tr_pexCompare (const void * va, const void * vb)
{
    const tr_pex * a = va;
    const tr_pex * b = vb;
    int i;

    assert (tr_isPex (a));
    assert (tr_isPex (b));

    if ((i = tr_address_compare (&a->addr, &b->addr)))
        return i;

    if (a->port != b->port)
        return a->port < b->port ? -1 : 1;

    return 0;
}

/* better goes first */
static int
compareAtomsByUsefulness (const void * va, const void *vb)
{
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;

    assert (tr_isAtom (a));
    assert (tr_isAtom (b));

    if (a->piece_data_time != b->piece_data_time)
        return a->piece_data_time > b->piece_data_time ? -1 : 1;
    if (a->fromBest != b->fromBest)
        return a->fromBest < b->fromBest ? -1 : 1;
    if (a->numFails != b->numFails)
        return a->numFails < b->numFails ? -1 : 1;

    return 0;
}

static bool
isAtomInteresting (const tr_torrent * tor, struct peer_atom * atom)
{
    if (tr_torrentIsSeed (tor) && atomIsSeed (atom))
        return false;

    if (peerIsInUse (tor->torrentPeers, atom))
        return true;

    if (isAtomBlocklisted (tor->session, atom))
        return false;

    if (atom->flags2 & MYFLAG_BANNED)
        return false;

    return true;
}

int
tr_peerMgrGetPeers (tr_torrent   * tor,
                    tr_pex      ** setme_pex,
                    uint8_t        af,
                    uint8_t        list_mode,
                    int            maxCount)
{
    int i;
    int n;
    int count = 0;
    int atomCount = 0;
    const Torrent * t = tor->torrentPeers;
    struct peer_atom ** atoms = NULL;
    tr_pex * pex;
    tr_pex * walk;

    assert (tr_isTorrent (tor));
    assert (setme_pex != NULL);
    assert (af==TR_AF_INET || af==TR_AF_INET6);
    assert (list_mode==TR_PEERS_CONNECTED || list_mode==TR_PEERS_INTERESTING);

    managerLock (t->manager);

    /**
    ***  build a list of atoms
    **/

    if (list_mode == TR_PEERS_CONNECTED) /* connected peers only */
    {
        int i;
        const tr_peer ** peers = (const tr_peer **) tr_ptrArrayBase (&t->peers);
        atomCount = tr_ptrArraySize (&t->peers);
        atoms = tr_new (struct peer_atom *, atomCount);
        for (i=0; i<atomCount; ++i)
            atoms[i] = peers[i]->atom;
    }
    else /* TR_PEERS_INTERESTING */
    {
        int i;
        struct peer_atom ** atomBase = (struct peer_atom**) tr_ptrArrayBase (&t->pool);
        n = tr_ptrArraySize (&t->pool);
        atoms = tr_new (struct peer_atom *, n);
        for (i=0; i<n; ++i)
            if (isAtomInteresting (tor, atomBase[i]))
                atoms[atomCount++] = atomBase[i];
    }

    qsort (atoms, atomCount, sizeof (struct peer_atom *), compareAtomsByUsefulness);

    /**
    ***  add the first N of them into our return list
    **/

    n = MIN (atomCount, maxCount);
    pex = walk = tr_new0 (tr_pex, n);

    for (i=0; i<atomCount && count<n; ++i)
    {
        const struct peer_atom * atom = atoms[i];
        if (atom->addr.type == af)
        {
            assert (tr_address_is_valid (&atom->addr));
            walk->addr = atom->addr;
            walk->port = atom->port;
            walk->flags = atom->flags;
            ++count;
            ++walk;
        }
    }

    qsort (pex, count, sizeof (tr_pex), tr_pexCompare);

    assert ((walk - pex) == count);
    *setme_pex = pex;

    /* cleanup */
    tr_free (atoms);
    managerUnlock (t->manager);
    return count;
}

static void atomPulse    (int, short, void *);
static void bandwidthPulse (int, short, void *);
static void rechokePulse (int, short, void *);
static void reconnectPulse (int, short, void *);

static struct event *
createTimer (tr_session * session, int msec, void (*callback)(int, short, void *), void * cbdata)
{
    struct event * timer = evtimer_new (session->event_base, callback, cbdata);
    tr_timerAddMsec (timer, msec);
    return timer;
}

static void
ensureMgrTimersExist (struct tr_peerMgr * m)
{
    if (m->atomTimer == NULL)
        m->atomTimer = createTimer (m->session, ATOM_PERIOD_MSEC, atomPulse, m);

    if (m->bandwidthTimer == NULL)
        m->bandwidthTimer = createTimer (m->session, BANDWIDTH_PERIOD_MSEC, bandwidthPulse, m);

    if (m->rechokeTimer == NULL)
        m->rechokeTimer = createTimer (m->session, RECHOKE_PERIOD_MSEC, rechokePulse, m);

    if (m->refillUpkeepTimer == NULL)
        m->refillUpkeepTimer = createTimer (m->session, REFILL_UPKEEP_PERIOD_MSEC, refillUpkeep, m);
}

void
tr_peerMgrStartTorrent (tr_torrent * tor)
{
    Torrent * t = tor->torrentPeers;

    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));

    ensureMgrTimersExist (t->manager);

    t->isRunning = true;
    t->maxPeers = t->tor->maxConnectedPeers;
    t->pieceSortState = PIECES_UNSORTED;

    rechokePulse (0, 0, t->manager);
}

static void
stopTorrent (Torrent * t)
{
    tr_peer * peer;

    t->isRunning = false;

    replicationFree (t);
    invalidatePieceSorting (t);

    /* disconnect the peers. */
    while ((peer = tr_ptrArrayPop (&t->peers)))
        peerDelete (t, peer);

    /* disconnect the handshakes. handshakeAbort calls handshakeDoneCB (),
     * which removes the handshake from t->outgoingHandshakes... */
    while (!tr_ptrArrayEmpty (&t->outgoingHandshakes))
        tr_handshakeAbort (tr_ptrArrayNth (&t->outgoingHandshakes, 0));
}

void
tr_peerMgrStopTorrent (tr_torrent * tor)
{
    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));

    stopTorrent (tor->torrentPeers);
}

void
tr_peerMgrAddTorrent (tr_peerMgr * manager, tr_torrent * tor)
{
    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));
    assert (tor->torrentPeers == NULL);

    tor->torrentPeers = torrentNew (manager, tor);
}

void
tr_peerMgrRemoveTorrent (tr_torrent * tor)
{
    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));

    stopTorrent (tor->torrentPeers);
    torrentFree (tor->torrentPeers);
}

void
tr_peerUpdateProgress (tr_torrent * tor, tr_peer * peer)
{
    const tr_bitfield * have = &peer->have;

    if (tr_bitfieldHasAll (have))
    {
        peer->progress = 1.0;
    }
    else if (tr_bitfieldHasNone (have))
    {
        peer->progress = 0.0;
    }
    else
    {
        const float true_count = tr_bitfieldCountTrueBits (have);

        if (tr_torrentHasMetadata (tor))
        {
            peer->progress = true_count / tor->info.pieceCount;
        }
        else /* without pieceCount, this result is only a best guess... */
        {
            peer->progress = true_count / (have->bit_count + 1);
        }
    }

    /* clamp the progress range */
    if (peer->progress < 0.0)
        peer->progress = 0.0;
    if (peer->progress > 1.0)
        peer->progress = 1.0;

    if (peer->atom && (peer->progress >= 1.0))
        atomSetSeed (tor->torrentPeers, peer->atom);
}

void
tr_peerMgrOnTorrentGotMetainfo (tr_torrent * tor)
{
    int i;
    int peerCount;
    tr_peer ** peers;

    /* the webseed list may have changed... */
    rebuildWebseedArray (tor->torrentPeers, tor);

    /* some peer_msgs' progress fields may not be accurate if we
       didn't have the metadata before now... so refresh them all... */
    peerCount = tr_ptrArraySize (&tor->torrentPeers->peers);
    peers = (tr_peer**) tr_ptrArrayBase (&tor->torrentPeers->peers);
    for (i=0; i<peerCount; ++i)
        tr_peerUpdateProgress (tor, peers[i]);

}

void
tr_peerMgrTorrentAvailability (const tr_torrent * tor, int8_t * tab, unsigned int tabCount)
{
    assert (tr_isTorrent (tor));
    assert (torrentIsLocked (tor->torrentPeers));
    assert (tab != NULL);
    assert (tabCount > 0);

    memset (tab, 0, tabCount);

    if (tr_torrentHasMetadata (tor))
    {
        tr_piece_index_t i;
        const int peerCount = tr_ptrArraySize (&tor->torrentPeers->peers);
        const tr_peer ** peers = (const tr_peer**) tr_ptrArrayBase (&tor->torrentPeers->peers);
        const float interval = tor->info.pieceCount / (float)tabCount;
        const bool isSeed = tr_cpGetStatus (&tor->completion) == TR_SEED;

        for (i=0; i<tabCount; ++i)
        {
            const int piece = i * interval;

            if (isSeed || tr_cpPieceIsComplete (&tor->completion, piece))
                tab[i] = -1;
            else if (peerCount) {
                int j;
                for (j=0; j<peerCount; ++j)
                    if (tr_bitfieldHas (&peers[j]->have, piece))
                        ++tab[i];
            }
        }
    }
}

static bool
peerIsSeed (const tr_peer * peer)
{
    if (peer->progress >= 1.0)
        return true;

    if (peer->atom && atomIsSeed (peer->atom))
        return true;

    return false;
}

/* count how many bytes we want that connected peers have */
uint64_t
tr_peerMgrGetDesiredAvailable (const tr_torrent * tor)
{
    size_t i;
    size_t n;
    uint64_t desiredAvailable;
    const Torrent * t = tor->torrentPeers;

    /* common shortcuts... */

    if (tr_torrentIsSeed (t->tor))
        return 0;

    if (!tr_torrentHasMetadata (tor))
        return 0;

    n = tr_ptrArraySize (&t->peers);
    if (n == 0)
        return 0;
    else {
        const tr_peer ** peers = (const tr_peer**) tr_ptrArrayBase (&t->peers);
        for (i=0; i<n; ++i)
            if (peers[i]->atom && atomIsSeed (peers[i]->atom))
                return tr_cpLeftUntilDone (&tor->completion);
    }

    if (!t->pieceReplication || !t->pieceReplicationSize)
        return 0;

    /* do it the hard way */

    desiredAvailable = 0;
    for (i=0, n=MIN (tor->info.pieceCount, t->pieceReplicationSize); i<n; ++i)
        if (!tor->info.pieces[i].dnd && (t->pieceReplication[i] > 0))
            desiredAvailable += tr_cpMissingBytesInPiece (&t->tor->completion, i);

    assert (desiredAvailable <= tor->info.totalSize);
    return desiredAvailable;
}

void
tr_peerMgrTorrentStats (tr_torrent  * tor,
                        int         * setmePeersConnected,
                        int         * setmeWebseedsSendingToUs,
                        int         * setmePeersSendingToUs,
                        int         * setmePeersGettingFromUs,
                        int         * setmePeersFrom)
{
    int i, size;
    const Torrent * t = tor->torrentPeers;
    const tr_peer ** peers;

    assert (tr_torrentIsLocked (tor));

    peers = (const tr_peer **) tr_ptrArrayBase (&t->peers);
    size = tr_ptrArraySize (&t->peers);

    *setmePeersConnected       = 0;
    *setmePeersGettingFromUs   = 0;
    *setmePeersSendingToUs     = 0;
    *setmeWebseedsSendingToUs  = 0;

    for (i=0; i<TR_PEER_FROM__MAX; ++i)
        setmePeersFrom[i] = 0;

    for (i=0; i<size; ++i)
    {
        const tr_peer * peer = peers[i];
        const struct peer_atom * atom = peer->atom;

        if (peer->io == NULL) /* not connected */
            continue;

        ++*setmePeersConnected;

        ++setmePeersFrom[atom->fromFirst];

        if (clientIsDownloadingFrom (tor, peer))
            ++*setmePeersSendingToUs;

        if (clientIsUploadingTo (peer))
            ++*setmePeersGettingFromUs;
    }

    *setmeWebseedsSendingToUs = countActiveWebseeds (t);
}

double*
tr_peerMgrWebSpeeds_KBps (const tr_torrent * tor)
{
    int i;
    const Torrent * t = tor->torrentPeers;
    const int webseedCount = tr_ptrArraySize (&t->webseeds);
    const tr_webseed ** webseeds = (const tr_webseed**) tr_ptrArrayBase (&t->webseeds);
    const uint64_t now = tr_time_msec ();
    double * ret = tr_new0 (double, webseedCount);

    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));
    assert (t->manager != NULL);
    assert (webseedCount == tor->info.webseedCount);

    for (i=0; i<webseedCount; ++i) {
        unsigned int Bps;
        if (tr_webseedGetSpeed_Bps (webseeds[i], now, &Bps))
            ret[i] = Bps / (double)tr_speed_K;
        else
            ret[i] = -1.0;
    }

    return ret;
}

unsigned int
tr_peerGetPieceSpeed_Bps (const tr_peer * peer, uint64_t now, tr_direction direction)
{
    return peer->io ? tr_peerIoGetPieceSpeed_Bps (peer->io, now, direction) : 0.0;
}

struct tr_peer_stat *
tr_peerMgrPeerStats (const tr_torrent * tor, int * setmeCount)
{
    int i;
    const Torrent * t = tor->torrentPeers;
    const int size = tr_ptrArraySize (&t->peers);
    const tr_peer ** peers = (const tr_peer**) tr_ptrArrayBase (&t->peers);
    const uint64_t now_msec = tr_time_msec ();
    const time_t now = tr_time ();
    tr_peer_stat * ret = tr_new0 (tr_peer_stat, size);

    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));
    assert (t->manager);

    for (i=0; i<size; ++i)
    {
        char *                   pch;
        const tr_peer *          peer = peers[i];
        const struct peer_atom * atom = peer->atom;
        tr_peer_stat *           stat = ret + i;

        tr_address_to_string_with_buf (&atom->addr, stat->addr, sizeof (stat->addr));
        tr_strlcpy (stat->client, (peer->client ? peer->client : ""),
                   sizeof (stat->client));
        stat->port                = ntohs (peer->atom->port);
        stat->from                = atom->fromFirst;
        stat->progress            = peer->progress;
        stat->isUTP               = peer->io->utp_socket != NULL;
        stat->isEncrypted         = tr_peerIoIsEncrypted (peer->io) ? 1 : 0;
        stat->rateToPeer_KBps     = toSpeedKBps (tr_peerGetPieceSpeed_Bps (peer, now_msec, TR_CLIENT_TO_PEER));
        stat->rateToClient_KBps   = toSpeedKBps (tr_peerGetPieceSpeed_Bps (peer, now_msec, TR_PEER_TO_CLIENT));
        stat->peerIsChoked        = peer->peerIsChoked;
        stat->peerIsInterested    = peer->peerIsInterested;
        stat->clientIsChoked      = peer->clientIsChoked;
        stat->clientIsInterested  = peer->clientIsInterested;
        stat->isIncoming          = tr_peerIoIsIncoming (peer->io);
        stat->isDownloadingFrom   = clientIsDownloadingFrom (tor, peer);
        stat->isUploadingTo       = clientIsUploadingTo (peer);
        stat->isSeed              = peerIsSeed (peer);

        stat->blocksToPeer        = tr_historyGet (&peer->blocksSentToPeer,    now, CANCEL_HISTORY_SEC);
        stat->blocksToClient      = tr_historyGet (&peer->blocksSentToClient,  now, CANCEL_HISTORY_SEC);
        stat->cancelsToPeer       = tr_historyGet (&peer->cancelsSentToPeer,   now, CANCEL_HISTORY_SEC);
        stat->cancelsToClient     = tr_historyGet (&peer->cancelsSentToClient, now, CANCEL_HISTORY_SEC);

        stat->pendingReqsToPeer   = peer->pendingReqsToPeer;
        stat->pendingReqsToClient = peer->pendingReqsToClient;

        pch = stat->flagStr;
        if (stat->isUTP) *pch++ = 'T';
        if (t->optimistic == peer) *pch++ = 'O';
        if (stat->isDownloadingFrom) *pch++ = 'D';
        else if (stat->clientIsInterested) *pch++ = 'd';
        if (stat->isUploadingTo) *pch++ = 'U';
        else if (stat->peerIsInterested) *pch++ = 'u';
        if (!stat->clientIsChoked && !stat->clientIsInterested) *pch++ = 'K';
        if (!stat->peerIsChoked && !stat->peerIsInterested) *pch++ = '?';
        if (stat->isEncrypted) *pch++ = 'E';
        if (stat->from == TR_PEER_FROM_DHT) *pch++ = 'H';
        else if (stat->from == TR_PEER_FROM_PEX) *pch++ = 'X';
        if (stat->isIncoming) *pch++ = 'I';
        *pch = '\0';
    }

    *setmeCount = size;

    return ret;
}

/***
****
****
***/

void
tr_peerMgrClearInterest (tr_torrent * tor)
{
    int i;
    Torrent * t = tor->torrentPeers;
    const int peerCount = tr_ptrArraySize (&t->peers);

    assert (tr_isTorrent (tor));
    assert (tr_torrentIsLocked (tor));

    for (i=0; i<peerCount; ++i)
    {
        const tr_peer * peer = tr_ptrArrayNth (&t->peers, i);
        tr_peerMsgsSetInterested (peer->msgs, false);
    }
}

/* does this peer have any pieces that we want? */
static bool
isPeerInteresting (const tr_torrent  * const tor,
                   const bool        * const piece_is_interesting,
                   const tr_peer     * const peer)
{
    tr_piece_index_t i, n;

    /* these cases should have already been handled by the calling code... */
    assert (!tr_torrentIsSeed (tor));
    assert (tr_torrentIsPieceTransferAllowed (tor, TR_PEER_TO_CLIENT));

    if (peerIsSeed (peer))
        return true;

    for (i=0, n=tor->info.pieceCount; i<n; ++i)
        if (piece_is_interesting[i] && tr_bitfieldHas (&peer->have, i))
            return true;

    return false;
}

typedef enum
{
    RECHOKE_STATE_GOOD,
    RECHOKE_STATE_UNTESTED,
    RECHOKE_STATE_BAD
}
tr_rechoke_state;

struct tr_rechoke_info
{
    tr_peer * peer;
    int salt;
    int rechoke_state;
};

static int
compare_rechoke_info (const void * va, const void * vb)
{
    const struct tr_rechoke_info * a = va;
    const struct tr_rechoke_info * b = vb;

    if (a->rechoke_state != b->rechoke_state)
        return a->rechoke_state - b->rechoke_state;

    return a->salt - b->salt;
}

/* determines who we send "interested" messages to */
static void
rechokeDownloads (Torrent * t)
{
    int i;
    int maxPeers = 0;
    int rechoke_count = 0;
    struct tr_rechoke_info * rechoke = NULL;
    const int MIN_INTERESTING_PEERS = 5;
    const int peerCount = tr_ptrArraySize (&t->peers);
    const time_t now = tr_time ();

    /* some cases where this function isn't necessary */
    if (tr_torrentIsSeed (t->tor))
        return;
    if (!tr_torrentIsPieceTransferAllowed (t->tor, TR_PEER_TO_CLIENT))
        return;

    /* decide HOW MANY peers to be interested in */
    {
        int blocks = 0;
        int cancels = 0;
        time_t timeSinceCancel;

        /* Count up how many blocks & cancels each peer has.
         *
         * There are two situations where we send out cancels --
         *
         * 1. We've got unresponsive peers, which is handled by deciding
         *    -which- peers to be interested in.
         *
         * 2. We've hit our bandwidth cap, which is handled by deciding
         *    -how many- peers to be interested in.
         *
         * We're working on 2. here, so we need to ignore unresponsive
         * peers in our calculations lest they confuse Transmission into
         * thinking it's hit its bandwidth cap.
         */
        for (i=0; i<peerCount; ++i)
        {
            const tr_peer * peer = tr_ptrArrayNth (&t->peers, i);
            const int b = tr_historyGet (&peer->blocksSentToClient, now, CANCEL_HISTORY_SEC);
            const int c = tr_historyGet (&peer->cancelsSentToPeer, now, CANCEL_HISTORY_SEC);

            if (b == 0) /* ignore unresponsive peers, as described above */
                continue;

            blocks += b;
            cancels += c;
        }

        if (cancels > 0)
        {
            /* cancelRate: of the block requests we've recently made, the percentage we cancelled.
             * higher values indicate more congestion. */
            const double cancelRate = cancels / (double)(cancels + blocks);
            const double mult = 1 - MIN (cancelRate, 0.5);
            maxPeers = t->interestedCount * mult;
            tordbg (t, "cancel rate is %.3f -- reducing the "
                       "number of peers we're interested in by %.0f percent",
                       cancelRate, mult * 100);
            t->lastCancel = now;
        }

        timeSinceCancel = now - t->lastCancel;
        if (timeSinceCancel)
        {
            const int maxIncrease = 15;
            const time_t maxHistory = 2 * CANCEL_HISTORY_SEC;
            const double mult = MIN (timeSinceCancel, maxHistory) / (double) maxHistory;
            const int inc = maxIncrease * mult;
            maxPeers = t->maxPeers + inc;
            tordbg (t, "time since last cancel is %li -- increasing the "
                       "number of peers we're interested in by %d",
                       timeSinceCancel, inc);
        }
    }

    /* don't let the previous section's number tweaking go too far... */
    if (maxPeers < MIN_INTERESTING_PEERS)
        maxPeers = MIN_INTERESTING_PEERS;
    if (maxPeers > t->tor->maxConnectedPeers)
        maxPeers = t->tor->maxConnectedPeers;

    t->maxPeers = maxPeers;

    if (peerCount > 0)
    {
        bool * piece_is_interesting;
        const tr_torrent * const tor = t->tor;
        const int n = tor->info.pieceCount;

        /* build a bitfield of interesting pieces... */
        piece_is_interesting = tr_new (bool, n);
        for (i=0; i<n; i++)
            piece_is_interesting[i] = !tor->info.pieces[i].dnd && !tr_cpPieceIsComplete (&tor->completion, i);

        /* decide WHICH peers to be interested in (based on their cancel-to-block ratio) */
        for (i=0; i<peerCount; ++i)
        {
            tr_peer * peer = tr_ptrArrayNth (&t->peers, i);

            if (!isPeerInteresting (t->tor, piece_is_interesting, peer))
            {
                tr_peerMsgsSetInterested (peer->msgs, false);
            }
            else
            {
                tr_rechoke_state rechoke_state;
                const int blocks = tr_historyGet (&peer->blocksSentToClient, now, CANCEL_HISTORY_SEC);
                const int cancels = tr_historyGet (&peer->cancelsSentToPeer, now, CANCEL_HISTORY_SEC);

                if (!blocks && !cancels)
                    rechoke_state = RECHOKE_STATE_UNTESTED;
                else if (!cancels)
                    rechoke_state = RECHOKE_STATE_GOOD;
                else if (!blocks)
                    rechoke_state = RECHOKE_STATE_BAD;
                else if ((cancels * 10) < blocks)
                    rechoke_state = RECHOKE_STATE_GOOD;
                else
                    rechoke_state = RECHOKE_STATE_BAD;

                if (rechoke == NULL)
                    rechoke = tr_new (struct tr_rechoke_info, peerCount);

                 rechoke[rechoke_count].peer = peer;
                 rechoke[rechoke_count].rechoke_state = rechoke_state;
                 rechoke[rechoke_count].salt = tr_cryptoWeakRandInt (INT_MAX);
                 rechoke_count++;
            }

        }

        tr_free (piece_is_interesting);
    }

    /* now that we know which & how many peers to be interested in... update the peer interest */
    qsort (rechoke, rechoke_count, sizeof (struct tr_rechoke_info), compare_rechoke_info);
    t->interestedCount = MIN (maxPeers, rechoke_count);
    for (i=0; i<rechoke_count; ++i)
        tr_peerMsgsSetInterested (rechoke[i].peer->msgs, i<t->interestedCount);

    /* cleanup */
    tr_free (rechoke);
}

/**
***
**/

struct ChokeData
{
    bool            isInterested;
    bool            wasChoked;
    bool            isChoked;
    int             rate;
    int             salt;
    tr_peer *       peer;
};

static int
compareChoke (const void * va, const void * vb)
{
    const struct ChokeData * a = va;
    const struct ChokeData * b = vb;

    if (a->rate != b->rate) /* prefer higher overall speeds */
        return a->rate > b->rate ? -1 : 1;

    if (a->wasChoked != b->wasChoked) /* prefer unchoked */
        return a->wasChoked ? 1 : -1;

    if (a->salt != b->salt) /* random order */
        return a->salt - b->salt;

    return 0;
}

/* is this a new connection? */
static int
isNew (const tr_peer * peer)
{
    return peer && peer->io && tr_peerIoGetAge (peer->io) < 45;
}

/* get a rate for deciding which peers to choke and unchoke. */
static int
getRate (const tr_torrent * tor, struct peer_atom * atom, uint64_t now)
{
    unsigned int Bps;

    if (tr_torrentIsSeed (tor))
        Bps = tr_peerGetPieceSpeed_Bps (atom->peer, now, TR_CLIENT_TO_PEER);

    /* downloading a private torrent... take upload speed into account
     * because there may only be a small window of opportunity to share */
    else if (tr_torrentIsPrivate (tor))
        Bps = tr_peerGetPieceSpeed_Bps (atom->peer, now, TR_PEER_TO_CLIENT)
            + tr_peerGetPieceSpeed_Bps (atom->peer, now, TR_CLIENT_TO_PEER);

    /* downloading a public torrent */
    else
        Bps = tr_peerGetPieceSpeed_Bps (atom->peer, now, TR_PEER_TO_CLIENT);

    /* convert it to bytes per second */
    return Bps;
}

static inline bool
isBandwidthMaxedOut (const tr_bandwidth * b,
                     const uint64_t now_msec, tr_direction dir)
{
    if (!tr_bandwidthIsLimited (b, dir))
        return false;
    else {
        const unsigned int got = tr_bandwidthGetPieceSpeed_Bps (b, now_msec, dir);
        const unsigned int want = tr_bandwidthGetDesiredSpeed_Bps (b, dir);
        return got >= want;
    }
}

static void
rechokeUploads (Torrent * t, const uint64_t now)
{
    int i, size, unchokedInterested;
    const int peerCount = tr_ptrArraySize (&t->peers);
    tr_peer ** peers = (tr_peer**) tr_ptrArrayBase (&t->peers);
    struct ChokeData * choke = tr_new0 (struct ChokeData, peerCount);
    const tr_session * session = t->manager->session;
    const int chokeAll = !tr_torrentIsPieceTransferAllowed (t->tor, TR_CLIENT_TO_PEER);
    const bool isMaxedOut = isBandwidthMaxedOut (&t->tor->bandwidth, now, TR_UP);

    assert (torrentIsLocked (t));

    /* an optimistic unchoke peer's "optimistic"
     * state lasts for N calls to rechokeUploads (). */
    if (t->optimisticUnchokeTimeScaler > 0)
        t->optimisticUnchokeTimeScaler--;
    else
        t->optimistic = NULL;

    /* sort the peers by preference and rate */
    for (i = 0, size = 0; i < peerCount; ++i)
    {
        tr_peer * peer = peers[i];
        struct peer_atom * atom = peer->atom;

        if (peerIsSeed (peer)) /* choke seeds and partial seeds */
        {
            tr_peerMsgsSetChoke (peer->msgs, true);
        }
        else if (chokeAll) /* choke everyone if we're not uploading */
        {
            tr_peerMsgsSetChoke (peer->msgs, true);
        }
        else if (peer != t->optimistic)
        {
            struct ChokeData * n = &choke[size++];
            n->peer         = peer;
            n->isInterested = peer->peerIsInterested;
            n->wasChoked    = peer->peerIsChoked;
            n->rate         = getRate (t->tor, atom, now);
            n->salt         = tr_cryptoWeakRandInt (INT_MAX);
            n->isChoked     = true;
        }
    }

    qsort (choke, size, sizeof (struct ChokeData), compareChoke);

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke.
     *
     * If our bandwidth is maxed out, don't unchoke any more peers.
     */
    unchokedInterested = 0;
    for (i=0; i<size && unchokedInterested<session->uploadSlotsPerTorrent; ++i) {
        choke[i].isChoked = isMaxedOut ? choke[i].wasChoked : false;
        if (choke[i].isInterested)
            ++unchokedInterested;
    }

    /* optimistic unchoke */
    if (!t->optimistic && !isMaxedOut && (i<size))
    {
        int n;
        struct ChokeData * c;
        tr_ptrArray randPool = TR_PTR_ARRAY_INIT;

        for (; i<size; ++i)
        {
            if (choke[i].isInterested)
            {
                const tr_peer * peer = choke[i].peer;
                int x = 1, y;
                if (isNew (peer)) x *= 3;
                for (y=0; y<x; ++y)
                    tr_ptrArrayAppend (&randPool, &choke[i]);
            }
        }

        if ((n = tr_ptrArraySize (&randPool)))
        {
            c = tr_ptrArrayNth (&randPool, tr_cryptoWeakRandInt (n));
            c->isChoked = false;
            t->optimistic = c->peer;
            t->optimisticUnchokeTimeScaler = OPTIMISTIC_UNCHOKE_MULTIPLIER;
        }

        tr_ptrArrayDestruct (&randPool, NULL);
    }

    for (i=0; i<size; ++i)
        tr_peerMsgsSetChoke (choke[i].peer->msgs, choke[i].isChoked);

    /* cleanup */
    tr_free (choke);
}

static void
rechokePulse (int foo UNUSED, short bar UNUSED, void * vmgr)
{
    tr_torrent * tor = NULL;
    tr_peerMgr * mgr = vmgr;
    const uint64_t now = tr_time_msec ();

    managerLock (mgr);

    while ((tor = tr_torrentNext (mgr->session, tor))) {
        if (tor->isRunning) {
            Torrent * t = tor->torrentPeers;
            if (!tr_ptrArrayEmpty (&t->peers)) {
                rechokeUploads (t, now);
                rechokeDownloads (t);
            }
        }
    }

    tr_timerAddMsec (mgr->rechokeTimer, RECHOKE_PERIOD_MSEC);
    managerUnlock (mgr);
}

/***
****
****  Life and Death
****
***/

static bool
shouldPeerBeClosed (const Torrent    * t,
                    const tr_peer    * peer,
                    int                peerCount,
                    const time_t       now)
{
    const tr_torrent *       tor = t->tor;
    const struct peer_atom * atom = peer->atom;

    /* if it's marked for purging, close it */
    if (peer->doPurge)
    {
        tordbg (t, "purging peer %s because its doPurge flag is set",
                tr_atomAddrStr (atom));
        return true;
    }

    /* disconnect if we're both seeds and enough time has passed for PEX */
    if (tr_torrentIsSeed (tor) && peerIsSeed (peer))
        return !tr_torrentAllowsPex (tor) || (now-atom->time>=30);

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        const int relaxStrictnessIfFewerThanN = (int)((getMaxPeerCount (tor) * 0.9) + 0.5);
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        const float strictness = peerCount >= relaxStrictnessIfFewerThanN
                               ? 1.0
                               : peerCount / (float)relaxStrictnessIfFewerThanN;
        const int lo = MIN_UPLOAD_IDLE_SECS;
        const int hi = MAX_UPLOAD_IDLE_SECS;
        const int limit = hi - ((hi - lo) * strictness);
        const int idleTime = now - MAX (atom->time, atom->piece_data_time);
/*fprintf (stderr, "strictness is %.3f, limit is %d seconds... time since connect is %d, time since piece is %d ... idleTime is %d, doPurge is %d\n", (double)strictness, limit, (int)(now - atom->time), (int)(now - atom->piece_data_time), idleTime, idleTime > limit);*/
        if (idleTime > limit) {
            tordbg (t, "purging peer %s because it's been %d secs since we shared anything",
                       tr_atomAddrStr (atom), idleTime);
            return true;
        }
    }

    return false;
}

static tr_peer **
getPeersToClose (Torrent * t, const time_t now_sec, int * setmeSize)
{
    int i, peerCount, outsize;
    struct tr_peer ** ret = NULL;
    tr_peer ** peers = (tr_peer**) tr_ptrArrayPeek (&t->peers, &peerCount);

    assert (torrentIsLocked (t));

    for (i = outsize = 0; i < peerCount; ++i) {
        if (shouldPeerBeClosed (t, peers[i], peerCount, now_sec)) {
            if (ret == NULL)
                ret = tr_new (tr_peer *, peerCount);
            ret[outsize++] = peers[i];
        }
    }

    *setmeSize = outsize;
    return ret;
}

static int
getReconnectIntervalSecs (const struct peer_atom * atom, const time_t now)
{
    int sec;

    /* if we were recently connected to this peer and transferring piece
     * data, try to reconnect to them sooner rather that later -- we don't
     * want network troubles to get in the way of a good peer. */
    if ((now - atom->piece_data_time) <= (MINIMUM_RECONNECT_INTERVAL_SECS * 2))
        sec = MINIMUM_RECONNECT_INTERVAL_SECS;

    /* don't allow reconnects more often than our minimum */
    else if ((now - atom->time) < MINIMUM_RECONNECT_INTERVAL_SECS)
        sec = MINIMUM_RECONNECT_INTERVAL_SECS;

    /* otherwise, the interval depends on how many times we've tried
     * and failed to connect to the peer */
    else switch (atom->numFails) {
        case 0: sec = 0; break;
        case 1: sec = 5; break;
        case 2: sec = 2 * 60; break;
        case 3: sec = 15 * 60; break;
        case 4: sec = 30 * 60; break;
        case 5: sec = 60 * 60; break;
        default: sec = 120 * 60; break;
    }

    /* penalize peers that were unreachable the last time we tried */
    if (atom->flags2 & MYFLAG_UNREACHABLE)
        sec += sec;

    dbgmsg ("reconnect interval for %s is %d seconds", tr_atomAddrStr (atom), sec);
    return sec;
}

static void
removePeer (Torrent * t, tr_peer * peer)
{
    tr_peer * removed;
    struct peer_atom * atom = peer->atom;

    assert (torrentIsLocked (t));
    assert (atom);

    atom->time = tr_time ();

    removed = tr_ptrArrayRemoveSorted (&t->peers, peer, peerCompare);

    if (replicationExists (t))
        tr_decrReplicationFromBitfield (t, &peer->have);

    assert (removed == peer);
    peerDelete (t, removed);
}

static void
closePeer (Torrent * t, tr_peer * peer)
{
    struct peer_atom * atom;

    assert (t != NULL);
    assert (peer != NULL);

    atom = peer->atom;

    /* if we transferred piece data, then they might be good peers,
       so reset their `numFails' weight to zero. otherwise we connected
       to them fruitlessly, so mark it as another fail */
    if (atom->piece_data_time) {
        tordbg (t, "resetting atom %s numFails to 0", tr_atomAddrStr (atom));
        atom->numFails = 0;
    } else {
        ++atom->numFails;
        tordbg (t, "incremented atom %s numFails to %d", tr_atomAddrStr (atom), (int)atom->numFails);
    }

    tordbg (t, "removing bad peer %s", tr_peerIoGetAddrStr (peer->io));
    removePeer (t, peer);
}

static void
removeAllPeers (Torrent * t)
{
    while (!tr_ptrArrayEmpty (&t->peers))
        removePeer (t, tr_ptrArrayNth (&t->peers, 0));
}

static void
closeBadPeers (Torrent * t, const time_t now_sec)
{
    if (!tr_ptrArrayEmpty (&t->peers))
    {
        int i;
        int peerCount;
        struct tr_peer ** peers = getPeersToClose (t, now_sec, &peerCount);
        for (i=0; i<peerCount; ++i)
            closePeer (t, peers[i]);
        tr_free (peers);
    }
}

struct peer_liveliness
{
    tr_peer * peer;
    void * clientData;
    time_t pieceDataTime;
    time_t time;
    int speed;
    bool doPurge;
};

static int
comparePeerLiveliness (const void * va, const void * vb)
{
    const struct peer_liveliness * a = va;
    const struct peer_liveliness * b = vb;

    if (a->doPurge != b->doPurge)
        return a->doPurge ? 1 : -1;

    if (a->speed != b->speed) /* faster goes first */
        return a->speed > b->speed ? -1 : 1;

    /* the one to give us data more recently goes first */
    if (a->pieceDataTime != b->pieceDataTime)
        return a->pieceDataTime > b->pieceDataTime ? -1 : 1;

    /* the one we connected to most recently goes first */
    if (a->time != b->time)
        return a->time > b->time ? -1 : 1;

    return 0;
}

static void
sortPeersByLivelinessImpl (tr_peer  ** peers,
                           void     ** clientData,
                           int         n,
                           uint64_t    now,
                           int (*compare)(const void *va, const void *vb))
{
    int i;
    struct peer_liveliness *lives, *l;

    /* build a sortable array of peer + extra info */
    lives = l = tr_new0 (struct peer_liveliness, n);
    for (i=0; i<n; ++i, ++l)
    {
        tr_peer * p = peers[i];
        l->peer = p;
        l->doPurge = p->doPurge;
        l->pieceDataTime = p->atom->piece_data_time;
        l->time = p->atom->time;
        l->speed = tr_peerGetPieceSpeed_Bps (p, now, TR_UP)
                 + tr_peerGetPieceSpeed_Bps (p, now, TR_DOWN);
        if (clientData)
            l->clientData = clientData[i];
    }

    /* sort 'em */
    assert (n == (l - lives));
    qsort (lives, n, sizeof (struct peer_liveliness), compare);

    /* build the peer array */
    for (i=0, l=lives; i<n; ++i, ++l) {
        peers[i] = l->peer;
        if (clientData)
            clientData[i] = l->clientData;
    }
    assert (n == (l - lives));

    /* cleanup */
    tr_free (lives);
}

static void
sortPeersByLiveliness (tr_peer ** peers, void ** clientData, int n, uint64_t now)
{
    sortPeersByLivelinessImpl (peers, clientData, n, now, comparePeerLiveliness);
}


static void
enforceTorrentPeerLimit (Torrent * t, uint64_t now)
{
    int n = tr_ptrArraySize (&t->peers);
    const int max = tr_torrentGetPeerLimit (t->tor);
    if (n > max)
    {
        void * base = tr_ptrArrayBase (&t->peers);
        tr_peer ** peers = tr_memdup (base, n*sizeof (tr_peer*));
        sortPeersByLiveliness (peers, NULL, n, now);
        while (n > max)
            closePeer (t, peers[--n]);
        tr_free (peers);
    }
}

static void
enforceSessionPeerLimit (tr_session * session, uint64_t now)
{
    int n = 0;
    tr_torrent * tor = NULL;
    const int max = tr_sessionGetPeerLimit (session);

    /* count the total number of peers */
    while ((tor = tr_torrentNext (session, tor)))
        n += tr_ptrArraySize (&tor->torrentPeers->peers);

    /* if there are too many, prune out the worst */
    if (n > max)
    {
        tr_peer ** peers = tr_new (tr_peer*, n);
        Torrent ** torrents = tr_new (Torrent*, n);

        /* populate the peer array */
        n = 0;
        tor = NULL;
        while ((tor = tr_torrentNext (session, tor))) {
            int i;
            Torrent * t = tor->torrentPeers;
            const int tn = tr_ptrArraySize (&t->peers);
            for (i=0; i<tn; ++i, ++n) {
                peers[n] = tr_ptrArrayNth (&t->peers, i);
                torrents[n] = t;
            }
        }

        /* sort 'em */
        sortPeersByLiveliness (peers, (void**)torrents, n, now);

        /* cull out the crappiest */
        while (n-- > max)
            closePeer (torrents[n], peers[n]);

        /* cleanup */
        tr_free (torrents);
        tr_free (peers);
    }
}

static void makeNewPeerConnections (tr_peerMgr * mgr, const int max);

static void
reconnectPulse (int foo UNUSED, short bar UNUSED, void * vmgr)
{
    tr_torrent * tor;
    tr_peerMgr * mgr = vmgr;
    const time_t now_sec = tr_time ();
    const uint64_t now_msec = tr_time_msec ();

    /**
    ***  enforce the per-session and per-torrent peer limits
    **/

    /* if we're over the per-torrent peer limits, cull some peers */
    tor = NULL;
    while ((tor = tr_torrentNext (mgr->session, tor)))
        if (tor->isRunning)
            enforceTorrentPeerLimit (tor->torrentPeers, now_msec);

    /* if we're over the per-session peer limits, cull some peers */
    enforceSessionPeerLimit (mgr->session, now_msec);

    /* remove crappy peers */
    tor = NULL;
    while ((tor = tr_torrentNext (mgr->session, tor)))
        if (!tor->torrentPeers->isRunning)
            removeAllPeers (tor->torrentPeers);
        else
            closeBadPeers (tor->torrentPeers, now_sec);

    /* try to make new peer connections */
    makeNewPeerConnections (mgr, MAX_CONNECTIONS_PER_PULSE);
}

/****
*****
*****  BANDWIDTH ALLOCATION
*****
****/

static void
pumpAllPeers (tr_peerMgr * mgr)
{
    tr_torrent * tor = NULL;

    while ((tor = tr_torrentNext (mgr->session, tor)))
    {
        int j;
        Torrent * t = tor->torrentPeers;

        for (j=0; j<tr_ptrArraySize (&t->peers); ++j)
        {
            tr_peer * peer = tr_ptrArrayNth (&t->peers, j);
            tr_peerMsgsPulse (peer->msgs);
        }
    }
}

static void
queuePulse (tr_session * session, tr_direction dir)
{
    assert (tr_isSession (session));
    assert (tr_isDirection (dir));

    if (tr_sessionGetQueueEnabled (session, dir))
    {
        int i;
        const int n = tr_sessionCountQueueFreeSlots (session, dir);
        for (i=0; i<n; i++) {
            tr_torrent * tor = tr_sessionGetNextQueuedTorrent (session, dir);
            if (tor != NULL) {
                tr_torrentStartNow (tor);
                if (tor->queue_started_callback != NULL)
                  (*tor->queue_started_callback)(tor, tor->queue_started_user_data);
            }
        }
    }
}

static void
bandwidthPulse (int foo UNUSED, short bar UNUSED, void * vmgr)
{
    tr_torrent * tor;
    tr_peerMgr * mgr = vmgr;
    tr_session * session = mgr->session;
    managerLock (mgr);

    /* FIXME: this next line probably isn't necessary... */
    pumpAllPeers (mgr);

    /* allocate bandwidth to the peers */
    tr_bandwidthAllocate (&session->bandwidth, TR_UP, BANDWIDTH_PERIOD_MSEC);
    tr_bandwidthAllocate (&session->bandwidth, TR_DOWN, BANDWIDTH_PERIOD_MSEC);

    /* torrent upkeep */
    tor = NULL;
    while ((tor = tr_torrentNext (session, tor)))
    {
        /* possibly stop torrents that have seeded enough */
        tr_torrentCheckSeedLimit (tor);

        /* run the completeness check for any torrents that need it */
        if (tor->torrentPeers->needsCompletenessCheck) {
            tor->torrentPeers->needsCompletenessCheck  = false;
            tr_torrentRecheckCompleteness (tor);
        }

        /* stop torrents that are ready to stop, but couldn't be stopped
           earlier during the peer-io callback call chain */
        if (tor->isStopping)
            tr_torrentStop (tor);
    }

    /* pump the queues */
    queuePulse (session, TR_UP);
    queuePulse (session, TR_DOWN);

    reconnectPulse (0, 0, mgr);

    tr_timerAddMsec (mgr->bandwidthTimer, BANDWIDTH_PERIOD_MSEC);
    managerUnlock (mgr);
}

/***
****
***/

static int
compareAtomPtrsByAddress (const void * va, const void *vb)
{
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;

    assert (tr_isAtom (a));
    assert (tr_isAtom (b));

    return tr_address_compare (&a->addr, &b->addr);
}

/* best come first, worst go last */
static int
compareAtomPtrsByShelfDate (const void * va, const void *vb)
{
    time_t atime;
    time_t btime;
    const struct peer_atom * a = * (const struct peer_atom**) va;
    const struct peer_atom * b = * (const struct peer_atom**) vb;
    const int data_time_cutoff_secs = 60 * 60;
    const time_t tr_now = tr_time ();

    assert (tr_isAtom (a));
    assert (tr_isAtom (b));

    /* primary key: the last piece data time *if* it was within the last hour */
    atime = a->piece_data_time; if (atime + data_time_cutoff_secs < tr_now) atime = 0;
    btime = b->piece_data_time; if (btime + data_time_cutoff_secs < tr_now) btime = 0;
    if (atime != btime)
        return atime > btime ? -1 : 1;

    /* secondary key: shelf date. */
    if (a->shelf_date != b->shelf_date)
        return a->shelf_date > b->shelf_date ? -1 : 1;

    return 0;
}

static int
getMaxAtomCount (const tr_torrent * tor)
{
    const int n = tor->maxConnectedPeers;
    /* approximate fit of the old jump discontinuous function */
    if (n >= 55) return     n + 150;
    if (n >= 20) return 2 * n + 95;
    return               4 * n + 55;
}

static void
atomPulse (int foo UNUSED, short bar UNUSED, void * vmgr)
{
    tr_torrent * tor = NULL;
    tr_peerMgr * mgr = vmgr;
    managerLock (mgr);

    while ((tor = tr_torrentNext (mgr->session, tor)))
    {
        int atomCount;
        Torrent * t = tor->torrentPeers;
        const int maxAtomCount = getMaxAtomCount (tor);
        struct peer_atom ** atoms = (struct peer_atom**) tr_ptrArrayPeek (&t->pool, &atomCount);

        if (atomCount > maxAtomCount) /* we've got too many atoms... time to prune */
        {
            int i;
            int keepCount = 0;
            int testCount = 0;
            struct peer_atom ** keep = tr_new (struct peer_atom*, atomCount);
            struct peer_atom ** test = tr_new (struct peer_atom*, atomCount);

            /* keep the ones that are in use */
            for (i=0; i<atomCount; ++i) {
                struct peer_atom * atom = atoms[i];
                if (peerIsInUse (t, atom))
                    keep[keepCount++] = atom;
                else
                    test[testCount++] = atom;
            }

            /* if there's room, keep the best of what's left */
            i = 0;
            if (keepCount < maxAtomCount) {
                qsort (test, testCount, sizeof (struct peer_atom *), compareAtomPtrsByShelfDate);
                while (i<testCount && keepCount<maxAtomCount)
                    keep[keepCount++] = test[i++];
            }

            /* free the culled atoms */
            while (i<testCount)
                tr_free (test[i++]);

            /* rebuild Torrent.pool with what's left */
            tr_ptrArrayDestruct (&t->pool, NULL);
            t->pool = TR_PTR_ARRAY_INIT;
            qsort (keep, keepCount, sizeof (struct peer_atom *), compareAtomPtrsByAddress);
            for (i=0; i<keepCount; ++i)
                tr_ptrArrayAppend (&t->pool, keep[i]);

            tordbg (t, "max atom count is %d... pruned from %d to %d\n", maxAtomCount, atomCount, keepCount);

            /* cleanup */
            tr_free (test);
            tr_free (keep);
        }
    }

    tr_timerAddMsec (mgr->atomTimer, ATOM_PERIOD_MSEC);
    managerUnlock (mgr);
}

/***
****
****
****
***/

/* is this atom someone that we'd want to initiate a connection to? */
static bool
isPeerCandidate (const tr_torrent * tor, struct peer_atom * atom, const time_t now)
{
    /* not if we're both seeds */
    if (tr_torrentIsSeed (tor) && atomIsSeed (atom))
        return false;

    /* not if we've already got a connection to them... */
    if (peerIsInUse (tor->torrentPeers, atom))
        return false;

    /* not if we just tried them already */
    if ((now - atom->time) < getReconnectIntervalSecs (atom, now))
        return false;

    /* not if they're blocklisted */
    if (isAtomBlocklisted (tor->session, atom))
        return false;

    /* not if they're banned... */
    if (atom->flags2 & MYFLAG_BANNED)
        return false;

    return true;
}

struct peer_candidate
{
    uint64_t score;
    tr_torrent * tor;
    struct peer_atom * atom;
};

static bool
torrentWasRecentlyStarted (const tr_torrent * tor)
{
    return difftime (tr_time (), tor->startDate) < 120;
}

static inline uint64_t
addValToKey (uint64_t value, int width, uint64_t addme)
{
    value = (value << (uint64_t)width);
    value |= addme;
    return value;
}

/* smaller value is better */
static uint64_t
getPeerCandidateScore (const tr_torrent * tor, const struct peer_atom * atom, uint8_t salt)
{
    uint64_t i;
    uint64_t score = 0;
    const bool failed = atom->lastConnectionAt < atom->lastConnectionAttemptAt;

    /* prefer peers we've connected to, or never tried, over peers we failed to connect to. */
    i = failed ? 1 : 0;
    score = addValToKey (score, 1, i);

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = atom->lastConnectionAttemptAt;
    score = addValToKey (score, 32, i);

    /* prefer peers belonging to a torrent of a higher priority */
    switch (tr_torrentGetPriority (tor)) {
        case TR_PRI_HIGH:    i = 0; break;
        case TR_PRI_NORMAL:  i = 1; break;
        case TR_PRI_LOW:     i = 2; break;
    }
    score = addValToKey (score, 4, i);

    /* prefer recently-started torrents */
    i = torrentWasRecentlyStarted (tor) ? 0 : 1;
    score = addValToKey (score, 1, i);

    /* prefer torrents we're downloading with */
    i = tr_torrentIsSeed (tor) ? 1 : 0;
    score = addValToKey (score, 1, i);

    /* prefer peers that are known to be connectible */
    i = (atom->flags & ADDED_F_CONNECTABLE) ? 0 : 1;
    score = addValToKey (score, 1, i);

    /* prefer peers that we might have a chance of uploading to...
       so lower seed probability is better */
    if (atom->seedProbability == 100) i = 101;
    else if (atom->seedProbability == -1) i = 100;
    else i = atom->seedProbability;
    score = addValToKey (score, 8, i);

    /* Prefer peers that we got from more trusted sources.
     * lower `fromBest' values indicate more trusted sources */
    score = addValToKey (score, 4, atom->fromBest);

    /* salt */
    score = addValToKey (score, 8, salt);

    return score;
}

#ifndef NDEBUG
static int
checkPartition (const struct peer_candidate * candidates, int left, int right, uint64_t pivotScore, int storeIndex)
{
    int i;

    assert (storeIndex >= left);
    assert (storeIndex <= right);
    assert (candidates[storeIndex].score == pivotScore);

    for (i=left; i<storeIndex; ++i)
        assert (candidates[i].score < pivotScore);
    for (i=storeIndex+1; i<=right; ++i)
        assert (candidates[i].score >= pivotScore);

    return true;
}
#endif

/* Helper to selectBestCandidates ().
 * Adapted from http://en.wikipedia.org/wiki/Selection_algorithm */
static int
partitionPeerCandidates (struct peer_candidate * candidates, int left, int right, int pivotIndex)
{
    int i;
    int storeIndex;
    struct peer_candidate tmp;
    const struct peer_candidate pivotValue = candidates[pivotIndex];

    /* move pivot to end */
    tmp = candidates[right];
    candidates[right] = pivotValue;
    candidates[pivotIndex] = tmp;

    storeIndex = left;
    for (i=left; i<=right; ++i)
    {
        if (candidates[i].score < pivotValue.score)
        {
            tmp = candidates[storeIndex];
            candidates[storeIndex] = candidates[i];
            candidates[i] = tmp;
            storeIndex++;
        }
    }

    /* move pivot to its final place */
    tmp = candidates[right];
    candidates[right] = candidates[storeIndex];
    candidates[storeIndex] = tmp;

    /* sanity check */
    assert (checkPartition (candidates, left, right, pivotValue.score, storeIndex));

    return storeIndex;
}

/* Adapted from http://en.wikipedia.org/wiki/Selection_algorithm */
static void
selectPeerCandidates (struct peer_candidate * candidates, int left, int right, int k)
{
    if (right > left)
    {
        const int pivotIndex = left + (right-left)/2;

        int pivotNewIndex = partitionPeerCandidates (candidates, left, right, pivotIndex);

        if (pivotNewIndex > left + k) /* new condition */
            selectPeerCandidates (candidates, left, pivotNewIndex-1, k);
        else if (pivotNewIndex < left + k)
            selectPeerCandidates (candidates, pivotNewIndex+1, right, k+left-pivotNewIndex-1);
    }
}

#ifndef NDEBUG
static bool
checkBestScoresComeFirst (const struct peer_candidate * candidates, int n, int k)
{
    int i;
    uint64_t worstFirstScore = 0;
    const int x = MIN (n, k) - 1;

    for (i=0; i<x; i++)
        if (worstFirstScore < candidates[i].score)
            worstFirstScore = candidates[i].score;

    for (i=0; i<x; i++)
        assert (candidates[i].score <= worstFirstScore);

    for (i=x+1; i<n; i++)
        assert (candidates[i].score >= worstFirstScore);

    return true;
}
#endif /* NDEBUG */

/** @return an array of all the atoms we might want to connect to */
static struct peer_candidate*
getPeerCandidates (tr_session * session, int * candidateCount, int max)
{
    int atomCount;
    int peerCount;
    tr_torrent * tor;
    struct peer_candidate * candidates;
    struct peer_candidate * walk;
    const time_t now = tr_time ();
    const uint64_t now_msec = tr_time_msec ();
    /* leave 5% of connection slots for incoming connections -- ticket #2609 */
    const int maxCandidates = tr_sessionGetPeerLimit (session) * 0.95;

    /* count how many peers and atoms we've got */
    tor= NULL;
    atomCount = 0;
    peerCount = 0;
    while ((tor = tr_torrentNext (session, tor))) {
        atomCount += tr_ptrArraySize (&tor->torrentPeers->pool);
        peerCount += tr_ptrArraySize (&tor->torrentPeers->peers);
    }

    /* don't start any new handshakes if we're full up */
    if (maxCandidates <= peerCount) {
        *candidateCount = 0;
        return NULL;
    }

    /* allocate an array of candidates */
    walk = candidates = tr_new (struct peer_candidate, atomCount);

    /* populate the candidate array */
    tor = NULL;
    while ((tor = tr_torrentNext (session, tor)))
    {
        int i, nAtoms;
        struct peer_atom ** atoms;

        if (!tor->torrentPeers->isRunning)
            continue;

        /* if we've already got enough peers in this torrent... */
        if (tr_torrentGetPeerLimit (tor) <= tr_ptrArraySize (&tor->torrentPeers->peers))
            continue;

        /* if we've already got enough speed in this torrent... */
        if (tr_torrentIsSeed (tor) && isBandwidthMaxedOut (&tor->bandwidth, now_msec, TR_UP))
            continue;

        atoms = (struct peer_atom**) tr_ptrArrayPeek (&tor->torrentPeers->pool, &nAtoms);
        for (i=0; i<nAtoms; ++i)
        {
            struct peer_atom * atom = atoms[i];

            if (isPeerCandidate (tor, atom, now))
            {
                const uint8_t salt = tr_cryptoWeakRandInt (1024);
                walk->tor = tor;
                walk->atom = atom;
                walk->score = getPeerCandidateScore (tor, atom, salt);
                ++walk;
            }
        }
    }

    *candidateCount = walk - candidates;
    if (walk != candidates)
        selectPeerCandidates (candidates, 0, (walk-candidates)-1, max);

    assert (checkBestScoresComeFirst (candidates, *candidateCount, max));

    return candidates;
}

static void
initiateConnection (tr_peerMgr * mgr, Torrent * t, struct peer_atom * atom)
{
    tr_peerIo * io;
    const time_t now = tr_time ();
    bool utp = tr_sessionIsUTPEnabled (mgr->session) && !atom->utp_failed;

    if (atom->fromFirst == TR_PEER_FROM_PEX)
        /* PEX has explicit signalling for uTP support.  If an atom
           originally came from PEX and doesn't have the uTP flag, skip the
           uTP connection attempt.  Are we being optimistic here? */
        utp = utp && (atom->flags & ADDED_F_UTP_FLAGS);

    tordbg (t, "Starting an OUTGOING%s connection with %s",
            utp ? " µTP" : "",
            tr_atomAddrStr (atom));

    io = tr_peerIoNewOutgoing (mgr->session,
                               &mgr->session->bandwidth,
                               &atom->addr,
                               atom->port,
                               t->tor->info.hash,
                               t->tor->completeness == TR_SEED,
                               utp);

    if (io == NULL)
    {
        tordbg (t, "peerIo not created; marking peer %s as unreachable",
                tr_atomAddrStr (atom));
        atom->flags2 |= MYFLAG_UNREACHABLE;
        atom->numFails++;
    }
    else
    {
        tr_handshake * handshake = tr_handshakeNew (io,
                                                    mgr->session->encryptionMode,
                                                    myHandshakeDoneCB,
                                                    mgr);

        assert (tr_peerIoGetTorrentHash (io));

        tr_peerIoUnref (io); /* balanced by the initial ref
                                 in tr_peerIoNewOutgoing () */

        tr_ptrArrayInsertSorted (&t->outgoingHandshakes, handshake,
                                 handshakeCompare);
    }

    atom->lastConnectionAttemptAt = now;
    atom->time = now;
}

static void
initiateCandidateConnection (tr_peerMgr * mgr, struct peer_candidate * c)
{
#if 0
    fprintf (stderr, "Starting an OUTGOING connection with %s - [%s] seedProbability==%d; %s, %s\n",
             tr_atomAddrStr (c->atom),
             tr_torrentName (c->tor),
           (int)c->atom->seedProbability,
             tr_torrentIsPrivate (c->tor) ? "private" : "public",
             tr_torrentIsSeed (c->tor) ? "seed" : "downloader");
#endif

    initiateConnection (mgr, c->tor->torrentPeers, c->atom);
}

static void
makeNewPeerConnections (struct tr_peerMgr * mgr, const int max)
{
    int i, n;
    struct peer_candidate * candidates;

    candidates = getPeerCandidates (mgr->session, &n, max);

    for (i=0; i<n && i<max; ++i)
        initiateCandidateConnection (mgr, &candidates[i]);

    tr_free (candidates);
}
