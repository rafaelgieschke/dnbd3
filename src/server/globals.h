#ifndef _GLOBALS_H_
#define _GLOBALS_H_

#include "../types.h"
#include "../shared/fdsignal.h"
#include "../serverconfig.h"
#include <stdint.h>
#include <time.h>
#include <pthread.h>

typedef struct timespec ticks;

// ######### All structs/types used by the server ########

typedef struct _dnbd3_connection dnbd3_connection_t;
typedef struct _dnbd3_image dnbd3_image_t;
typedef struct _dnbd3_client dnbd3_client_t;

// Slot is free, can be used.
// Must only be set in uplink_handle_receive() or uplink_remove_client()
#define ULR_FREE 0
// Slot has been filled with a request that hasn't been sent to the upstream server yet, matching request can safely rely on reuse.
// Must only be set in uplink_request()
#define ULR_NEW 1
// Slot is occupied, reply has not yet been received, matching request can safely rely on reuse.
// Must only be set in uplink_mainloop() or uplink_request()
#define ULR_PENDING 2
// Slot is being processed, do not consider for hop on.
// Must only be set in uplink_handle_receive()
#define ULR_PROCESSING 3
typedef struct
{
	uint64_t handle;  // Client defined handle to pass back in reply
	uint64_t from;    // First byte offset of requested block (ie. 4096)
	uint64_t to;      // Last byte + 1 of requested block (ie. 8192, if request len is 4096, resulting in bytes 4096-8191)
	dnbd3_client_t * client; // Client to send reply to
	int status;      // status of this entry: ULR_*
#ifdef _DEBUG
	ticks entered;           // When this request entered the queue (for debugging)
#endif
	uint8_t hopCount;      // How many hops this request has already taken across proxies
} dnbd3_queued_request_t;

#define RTT_IDLE 0 // Not in progress
#define RTT_INPROGRESS 1 // In progess, not finished
#define RTT_DONTCHANGE 2 // Finished, but no better alternative found
#define RTT_DOCHANGE 3 // Finished, better alternative written to .betterServer + .betterFd
#define RTT_NOT_REACHABLE 4 // No uplink was reachable
struct _dnbd3_connection
{
	int fd;                     // socket fd to remote server
	int version;                // remote server protocol version
	dnbd3_signal_t* signal;     // used to wake up the process
	pthread_t thread;           // thread holding the connection
	pthread_spinlock_t queueLock; // lock for synchronization on request queue etc.
	dnbd3_image_t *image;       // image that this uplink is used for; do not call get/release for this pointer
	dnbd3_host_t currentServer; // Current server we're connected to
	pthread_spinlock_t rttLock; // When accessing rttTestResult, betterFd or betterServer
	int rttTestResult;          // RTT_*
	int cacheFd;                // used to write to the image, in case it is relayed. ONLY USE FROM UPLINK THREAD!
	int betterVersion;          // protocol version of better server
	int betterFd;               // Active connection to better server, ready to use
	dnbd3_host_t betterServer;  // The better server
	uint8_t *recvBuffer;        // Buffer for receiving payload
	uint32_t recvBufferLen;     // Len of ^^
	volatile bool shutdown;     // signal this thread to stop, must only be set from uplink_shutdown() or cleanup in uplink_mainloop()
	bool replicatedLastBlock;   // bool telling if the last block has been replicated yet
	bool cycleDetected;         // connection cycle between proxies detected for current remote server
	int nextReplicationIndex;   // Which index in the cache map we should start looking for incomplete blocks at
	                            // If BGR == BGR_HASHBLOCK, -1 means "currently no incomplete block"
	uint64_t replicationHandle; // Handle of pending replication request
	uint64_t bytesReceived;     // Number of bytes received by the connection.
	uint64_t lastBytesReceived; // Number of bytes received last time we updated the global counter.
	int queueLen;               // length of queue
	int idleCount;              // How many iterations of keepalive check connection was idle
	dnbd3_queued_request_t queue[SERVER_MAX_UPLINK_QUEUE];
};

typedef struct
{
	char comment[COMMENT_LENGTH];
	dnbd3_host_t host;
	unsigned int rtt[SERVER_RTT_PROBES];
	unsigned int rttIndex;
	bool isPrivate, isClientOnly;
	ticks lastFail;
	int numFails;
} dnbd3_alt_server_t;

typedef struct
{
	uint8_t host[16];
	int bytes;
	int bitMask;
	int permissions;
} dnbd3_access_rule_t;

/**
 * Image struct. An image path could be something like
 * /mnt/images/rz/zfs/Windows7 ZfS.vmdk.r1
 * and the name would then be
 * rz/zfs/windows7 zfs.vmdk
 */
struct _dnbd3_image
{
	char *path;            // absolute path of the image
	char *name;            // public name of the image (usually relative path minus revision ID)
	dnbd3_connection_t *uplink; // pointer to a server connection
	uint8_t *cache_map;    // cache map telling which parts are locally cached, NULL if complete
	uint64_t virtualFilesize;   // virtual size of image (real size rounded up to multiple of 4k)
	uint64_t realFilesize;      // actual file size on disk
	ticks atime;                // last access time
	ticks lastWorkCheck;   // last time a non-working image has been checked
	ticks nextCompletenessEstimate; // next time the completeness estimate should be updated
	uint32_t *crc32;       // list of crc32 checksums for each 16MiB block in image
	uint32_t masterCrc32;  // CRC-32 of the crc-32 list
	int readFd;            // used to read the image. Used from multiple threads, so use atomic operations (pread et al)
	int completenessEstimate; // Completeness estimate in percent
	int users;             // clients currently using this image
	int id;                // Unique ID of this image. Only unique in the context of this running instance of DNBD3-Server
	bool working;          // true if image exists and completeness is == 100% or a working upstream proxy is connected
	uint16_t rid;          // revision of image
	pthread_spinlock_t lock;
};

struct _dnbd3_client
{
#define HOSTNAMELEN (48)
	uint64_t bytesSent;     // Byte counter for this client. Use statsLock when accessing.
	uint64_t lastBytesSent; // Byte counter from when we last added to global counter. Use statsLock when accessing.
	dnbd3_image_t *image;
	int sock;
	bool isServer;          // true if a server in proxy mode, false if real client
	dnbd3_host_t host;
	char hostName[HOSTNAMELEN];
	pthread_mutex_t sendMutex;
	pthread_spinlock_t lock;
	pthread_spinlock_t statsLock;
};

// #######################################################
#define CONFIG_FILENAME "server.conf"

/**
 * Base directory where the configuration files reside. Will never have a trailing slash.
 */
extern char *_configDir;

/**
 * Base directory where all images are stored in. Will never have a trailing slash.
 */
extern char *_basePath;

/**
 * Whether or not simple *.vmdk files should be treated as revision 1
 */
extern bool _vmdkLegacyMode;

/**
 * How much artificial delay should we add when a server connects to us?
 */
extern int _serverPenalty;

/**
 * How much artificial delay should we add when a client connects to us?
 */
extern int _clientPenalty;

/**
 * Is server shutting down?
 */
extern volatile bool _shutdown;

/**
 * Is server allowed to provide images in proxy mode?
 */
extern bool _isProxy;

/**
 * Only use servers as upstream proxy which are private?
 */
extern bool _proxyPrivateOnly;

/**
 * Whether to remove missing images from image list on SIGHUP
 */
extern bool _removeMissingImages;

/**
 * Read timeout when waiting for or sending data on an uplink
 */
extern int _uplinkTimeout;

/**
 * Read timeout when waiting for or sending data from/to client
 */
extern int _clientTimeout;

/**
 * If true, images with no active client will have their fd closed after some
 * idle time.
 */
extern bool _closeUnusedFd;

/**
 * Should we replicate incomplete images in the background?
 * Otherwise, only blocks that were explicitly requested will be cached.
 */
extern int _backgroundReplication;
#define BGR_DISABLED (0)
#define BGR_FULL (1)
#define BGR_HASHBLOCK (2)

/**
 * Minimum connected clients for background replication to kick in
 */
extern int _bgrMinClients;

/**
 * (In proxy mode): If connecting client is a proxy, and the requested image
 * is not known locally, should we ask our known alt servers for it?
 * Otherwise the request is rejected.
 */
extern bool _lookupMissingForProxy;

/**
 * Should we preallocate proxied images right at the start to make
 * sure we can cache it entirely, or rather create sparse files
 * with holes in them? With sparse files, we just keep writing
 * cached blocks to disk until it is full, and only then will we
 * start to delete old images. This might be a bit flaky so use
 * only in space restricted environments. Also make sure your
 * file system actually supports sparse files / files with holes
 * in them, or you might get really shitty performance.
 * This setting will have no effect if background replication is
 * turned on.
 */
extern bool _sparseFiles;

/**
 * Port to listen on (default: #define PORT (5003))
 */
extern int _listenPort;

/**
 * Max number of DNBD3 clients we accept
 */
extern int _maxClients;

/**
 * Max number of Images we support (in baseDir)
 */
extern int _maxImages;

/**
 * Maximum payload length we accept on uplinks and thus indirectly
 * from clients in case the requested range is not cached locally.
 * Usually this isn't even a megabyte for "real" clients (blockdev
 * or fuse).
 */
extern int _maxPayload;

/**
 * If in proxy mode, don't replicate images that are
 * larger than this according to the uplink server.
 */
extern uint64_t _maxReplicationSize;

/**
 * Load the server configuration.
 */
void globals_loadConfig();

/**
 * Dump the effective configuration in use to given buffer.
 */
size_t globals_dumpConfig(char *buffer, size_t size);

#endif /* GLOBALS_H_ */
