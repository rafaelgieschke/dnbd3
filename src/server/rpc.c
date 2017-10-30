#include "rpc.h"
#include "helper.h"
#include "net.h"
#include "uplink.h"
#include "locks.h"
#include "image.h"
#include "../shared/sockhelper.h"
#include "fileutil.h"
#include "picohttpparser/picohttpparser.h"
#include "urldecode.h"

#include <jansson.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define ACL_ALL        0x7fffffff
#define ACL_STATS               1
#define ACL_CLIENT_LIST         2
#define ACL_IMAGE_LIST          4

#define HTTP_CLOSE 4
#define HTTP_KEEPALIVE 9

// Make sure compiler does not reserve more space for static strings than required (or rather, does not tell so in sizeof calls)
// TODO Might be time for a dedicated string.h
_Static_assert( sizeof("test") == 5 && sizeof("test2") == 6, "Stringsize messup :/" );
#define STRCMP(str,chr) ( (str).s != NULL && (str).l == sizeof(chr)-1 && strncmp( (str).s, (chr), MIN((str).l, sizeof(chr)-1) ) == 0 )
#define STRSTART(str,chr) ( (str).s != NULL && (str).l >= sizeof(chr)-1 && strncmp( (str).s, (chr), MIN((str).l, sizeof(chr)-1) ) == 0 )
#define SETSTR(name,value) do { name.s = value; name.l = sizeof(value)-1; } while (0)
#define DEFSTR(name,value) static struct string name = { .s = value, .l = sizeof(value)-1 };
#define chartolower(c) ((char)( (c) >= 'A' && (c) <= 'Z' ? (c) + ('a'-'A') : (c) ))

//static struct string STR_CONNECTION, STR_KEEPALIVE;
DEFSTR(STR_CONNECTION, "connection")
DEFSTR(STR_CLOSE, "close")
DEFSTR(STR_QUERY, "/query")
DEFSTR(STR_Q, "q")

static inline bool equals(struct string *s1,struct string *s2)
{
	if ( s1->s == NULL ) {
		return s2->s == NULL;
	} else if ( s2->s == NULL || s1->l != s2->l ) {
		return false;
	}
	return memcmp( s1->s, s2->s, s1->l ) == 0;
}

static inline bool iequals(struct string *cmpMixed, struct string *cmpLower)
{
	if ( cmpMixed->s == NULL ) {
		return cmpLower->s == NULL;
	} else if ( cmpLower->s == NULL || cmpMixed->l != cmpLower->l ) {
		return false;
	}
	for ( size_t i = 0; i < cmpMixed->l; ++i ) {
		if ( chartolower( cmpMixed->s[i] ) != cmpLower->s[i] ) return false;
	}
	return true;
}

#define MAX_ACLS 100
static int aclCount = 0;
static dnbd3_access_rule_t aclRules[MAX_ACLS];
static json_int_t randomRunId;
static pthread_spinlock_t aclLock;

static bool handleStatus(int sock, int permissions, struct field *fields, size_t fields_num, int keepAlive);
static bool sendReply(int sock, const char *status, const char *ctype, const char *payload, ssize_t plen, int keepAlive);
static void parsePath(struct string *path, struct string *file, struct field *getv, size_t *getc);
static bool hasHeaderValue(struct phr_header *headers, size_t numHeaders, struct string *name, struct string *value);
static int getacl(dnbd3_host_t *host);
static void addacl(int argc, char **argv, void *data);
static void loadAcl();

void rpc_init()
{
	spin_init( &aclLock, PTHREAD_PROCESS_PRIVATE );
	randomRunId = (((json_int_t)getpid()) << 16) | (json_int_t)time(NULL);
	// </guard>
	if ( sizeof(randomRunId) > 4 ) {
		int fd = open( "/dev/urandom", O_RDONLY );
		if ( fd != -1 ) {
			uint32_t bla = 1;
			read( fd, &bla, 4 );
			randomRunId = (randomRunId << 32) | bla;
		}
		close( fd );
	}
	loadAcl();
}

void rpc_sendStatsJson(int sock, dnbd3_host_t* host, const void* data, const int dataLen)
{
	// TODO Parse Connection-header sent by client to see if keep-alive is supported
	bool ok;
	int keepAlive = HTTP_KEEPALIVE;
	int permissions = getacl( host );
	if ( permissions == 0 ) {
		sendReply( sock, "403 Forbidden", "text/plain", "Access denied", -1, HTTP_CLOSE );
		return;
	}
	char headerBuf[3000];
	if ( dataLen > 0 ) {
		// We call this function internally with a maximum data len of sizeof(dnbd3_request_t) so no bounds checking
		memcpy( headerBuf, data, dataLen );
	}
	size_t hoff = dataLen;
	do {
		// Read request from client
		struct phr_header headers[100];
		size_t numHeaders, prevLen = 0, consumed;
		struct string method, path;
		int minorVersion;
		do {
			// Parse before calling recv, there might be a complete pipelined request in the buffer already
			int pret;
			if ( hoff >= sizeof(headerBuf) ) return; // Request too large
			if ( hoff != 0 ) {
				numHeaders = 100;
				pret = phr_parse_request( headerBuf, hoff, &method, &path, &minorVersion, headers, &numHeaders, prevLen );
			} else {
				// Nothing in buffer yet, just set to -2 which is the phr return code for "partial request"
				pret = -2;
			}
			if ( pret > 0 ) {
				// > 0 means parsing completed without error
				consumed = (size_t)pret;
				break;
			}
			// Reaching here means partial request or parse error
			if ( pret == -2 ) { // Partial, keep reading
				prevLen = hoff;
#ifdef AFL_MODE
				ssize_t ret = recv( 0, headerBuf + hoff, sizeof(headerBuf) - hoff, 0 );
#else
				ssize_t ret = recv( sock, headerBuf + hoff, sizeof(headerBuf) - hoff, 0 );
#endif
				if ( ret == 0 ) return;
				if ( ret == -1 ) {
					if ( errno == EINTR ) continue;
					if ( errno != EAGAIN && errno != EWOULDBLOCK ) {
						sendReply( sock, "500 Internal Server Error", "text/plain", "Server made a boo-boo", -1, HTTP_CLOSE );
					}
					return; // Unknown error
				}
				hoff += ret;
			} else { // Parse error
				sendReply( sock, "400 Bad Request", "text/plain", "Server cannot understand what you're trying to say", -1, HTTP_CLOSE );
				return;
			}
		} while ( true );
		// Only keep the connection alive (and indicate so) if the client seems to support this
		if ( minorVersion == 0 || hasHeaderValue( headers, numHeaders, &STR_CONNECTION, &STR_CLOSE ) ) {
			keepAlive = HTTP_CLOSE;
		}
		if ( method.s != NULL && path.s != NULL ) {
			// Basic data filled from request parser
			// Handle stuff
			struct string file;
			struct field getv[10];
			size_t getc = 10;
			parsePath( &path, &file, getv, &getc );
			if ( method.s && method.s[0] == 'P' ) {
				// POST only methods
			}
			// Don't care if GET or POST
			if ( equals( &file, &STR_QUERY ) ) {
				ok = handleStatus( sock, permissions, getv, getc, keepAlive );
			} else {
				ok = sendReply( sock, "404 Not found", "text/plain", "Nothing", -1, keepAlive );
			}
			if ( !ok ) break;
		}
		// hoff might be beyond end if the client sent another request (burst)
		const ssize_t extra = hoff - consumed;
		if ( extra > 0 ) {
			memmove( headerBuf, headerBuf + consumed, extra );
		}
		hoff = extra;
	} while (true);
}

static bool handleStatus(int sock, int permissions, struct field *fields, size_t fields_num, int keepAlive)
{
	bool ok;
	bool stats = false, images = false, clients = false, space = false;
#define SETVAR(var) if ( !var && STRCMP(fields[i].value, #var) ) var = true
	for (size_t i = 0; i < fields_num; ++i) {
		if ( !equals( &fields[i].name, &STR_Q ) ) continue;
		SETVAR(stats);
		else SETVAR(space);
		else SETVAR(images);
		else SETVAR(clients);
	}
#undef SETVAR
	if ( ( stats || space ) && !(permissions & ACL_STATS) ) {
		return sendReply( sock, "403 Forbidden", "text/plain", "No permission to access statistics", -1, keepAlive );
	}
	if ( images && !(permissions & ACL_IMAGE_LIST) ) {
		return sendReply( sock, "403 Forbidden", "text/plain", "No permission to access image list", -1, keepAlive );
	}
	if ( clients && !(permissions & ACL_CLIENT_LIST) ) {
		return sendReply( sock, "403 Forbidden", "text/plain", "No permission to access client list", -1, keepAlive );
	}
	// Call this first because it will update the total bytes sent counter
	json_t *jsonClients = NULL;
	if ( stats || clients ) {
		jsonClients = net_clientsToJson( clients );
	}
	json_t *statisticsJson;
	if ( stats ) {
		const uint64_t bytesReceived = uplink_getTotalBytesReceived();
		const uint64_t bytesSent = net_getTotalBytesSent();
		statisticsJson = json_pack( "{sIsIsIsI}",
				"bytesReceived", (json_int_t) bytesReceived,
				"bytesSent", (json_int_t) bytesSent,
				"uptime", (json_int_t) dnbd3_serverUptime(),
				"runId", randomRunId );
	} else {
		statisticsJson = json_pack( "{sI}",
				"runId", randomRunId );
	}
	if ( space ) {
		uint64_t spaceTotal = 0, spaceAvail = 0;
		file_freeDiskSpace( _basePath, &spaceTotal, &spaceAvail );
		json_object_set_new( statisticsJson, "spaceTotal", json_integer( spaceTotal ) );
		json_object_set_new( statisticsJson, "spaceFree", json_integer( spaceAvail ) );
	}
	if ( jsonClients != NULL ) {
		if ( clients ) {
			json_object_set_new( statisticsJson, "clients", jsonClients );
		} else if ( stats ) {
			json_object_set_new( statisticsJson, "clientCount", jsonClients );
		}
	}
	if ( images ) {
		json_object_set_new( statisticsJson, "images", image_getListAsJson() );
	}

	char *jsonString = json_dumps( statisticsJson, 0 );
	json_decref( statisticsJson );
	ok = sendReply( sock, "200 OK", "application/json", jsonString, -1, keepAlive );
	free( jsonString );
	return ok;
}

static bool sendReply(int sock, const char *status, const char *ctype, const char *payload, ssize_t plen, int keepAlive)
{
	if ( plen == -1 ) plen = strlen( payload );
	char buffer[600];
	const char *connection = ( keepAlive == HTTP_KEEPALIVE ) ? "Keep-Alive" : "Close";
	int hlen = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %s\r\n"
			"Connection: %s\r\n"
			"Content-Type: %s; charset=utf-8\r\n"
			"Content-Length: %u\r\n"
			"\r\n",
			status, connection, ctype, (unsigned int)plen );
	if ( hlen < 0 || hlen >= (int)sizeof(buffer) ) return false; // Truncated
	if ( send( sock, buffer, hlen, MSG_MORE ) != hlen ) return false;
	if ( !sock_sendAll( sock, payload, plen, 10 ) ) return false;
	if ( keepAlive == HTTP_CLOSE ) {
		// Wait for flush
		shutdown( sock, SHUT_WR );
#ifdef AFL_MODE
		sock = 0;
#endif
		while ( read( sock, buffer, sizeof buffer ) > 0 );
		return false;
	}
	return true;
}

static void parsePath(struct string *path, struct string *file, struct field *getv, size_t *getc)
{
	size_t i = 0;
	while ( i < path->l && path->s[i] != '?' ) ++i;
	if ( i == path->l ) {
		*getc = 0;
		*file = *path;
		return;
	}
	file->s = path->s;
	file->l = i;
	++i;
	path->s += i;
	path->l -= i;
	urldecode( path, getv, getc );
	path->s -= i;
	path->l += i;
}

static bool hasHeaderValue(struct phr_header *headers, size_t numHeaders, struct string *name, struct string *value)
{
	for (size_t i = 0; i < numHeaders; ++i) {
		if ( !iequals( &headers[i].name, name ) ) continue;
		if ( iequals( &headers[i].value, value ) ) return true;
	}
	return false;
}

static int getacl(dnbd3_host_t *host)
{
	if ( aclCount == 0 ) return 0x7fffff; // For now compat mode - no rules defined == all access
	for (int i = 0; i < aclCount; ++i) {
		if ( aclRules[i].bytes == 0 && aclRules[i].bitMask == 0 ) return aclRules[i].permissions;
		if ( memcmp( aclRules[i].host, host->addr, aclRules[i].bytes ) != 0 ) continue;
		if ( aclRules[i].bitMask != 0 && aclRules[i].host[aclRules[i].bytes] != ( host->addr[aclRules[i].bytes] & aclRules[i].bitMask ) ) continue;
		return aclRules[i].permissions;
	}
#ifdef AFL_MODE
	return 0x7fffff;
#else
	return 0;
#endif
}

#define SETBIT(x) else if ( strcmp( argv[i], #x ) == 0 ) mask |= ACL_ ## x

static void addacl(int argc, char **argv, void *data UNUSED)
{
	if ( argv[0][0] == '#' ) return;
	spin_lock( &aclLock );
	if ( aclCount >= MAX_ACLS ) {
		logadd( LOG_WARNING, "Too many ACL rules, ignoring %s", argv[0] );
		goto unlock_end;
	}
	int mask = 0;
	for (int i = 1; i < argc; ++i) {
		if (false) {}
		SETBIT(ALL);
		SETBIT(STATS);
		SETBIT(CLIENT_LIST);
		SETBIT(IMAGE_LIST);
		else logadd( LOG_WARNING, "Invalid ACL flag '%s' for %s", argv[i], argv[0] );
	}
	if ( mask == 0 ) {
		logadd( LOG_INFO, "Ignoring empty rule for %s", argv[0] );
		goto unlock_end;
	}
	dnbd3_host_t host;
	char *slash = strchr( argv[0], '/' );
	if ( slash != NULL ) {
		*slash++ = '\0';
	}
	if ( !parse_address( argv[0], &host ) ) goto unlock_end;
	long int bits;
	if ( slash != NULL ) {
		char *last;
		bits = strtol( slash, &last, 10 );
		if ( last == slash ) slash = NULL;
		if ( host.type == HOST_IP4 && bits > 32 ) bits = 32;
		if ( bits > 128 ) bits = 128;
	}
	if ( slash == NULL ) {
		if ( host.type == HOST_IP4 ) {
			bits = 32;
		} else {
			bits = 128;
		}
	}
	memcpy( aclRules[aclCount].host, host.addr, 16 );
	aclRules[aclCount].bytes = (int)( bits / 8 );
	aclRules[aclCount].bitMask = 0;
	aclRules[aclCount].permissions = mask;
	bits %= 8;
	if ( bits != 0 ) {
		for (long int i = 0; i < bits; ++i) {
			aclRules[aclCount].bitMask = ( aclRules[aclCount].bitMask >> 1 ) | 0x80;
		}
		aclRules[aclCount].host[aclRules[aclCount].bytes] &= (uint8_t)aclRules[aclCount].bitMask;
	}
	// We now have .bytes set to the number of bytes to memcmp.
	// In case we have an odd bitmask, .bitMask will be != 0, so when comparing,
	// we need AND the host[.bytes] of the address to compare with the value
	// in .bitMask, and compate it, otherwise, a simple memcmp will do.
	aclCount++;
unlock_end:;
	spin_unlock( &aclLock );
}

static void loadAcl()
{
	static bool inProgress = false;
	char *fn;
	if ( asprintf( &fn, "%s/%s", _configDir, "rpc.acl" ) == -1 ) return;
	spin_lock( &aclLock );
	if ( inProgress ) {
		spin_unlock( &aclLock );
		return;
	}
	aclCount = 0;
	inProgress = true;
	spin_unlock( &aclLock );
	file_loadLineBased( fn, 1, 20, &addacl, NULL );
	spin_lock( &aclLock );
	inProgress = false;
	spin_unlock( &aclLock );
	free( fn );
	logadd( LOG_INFO, "%d HTTPRPC ACL rules loaded", (int)aclCount );
}

