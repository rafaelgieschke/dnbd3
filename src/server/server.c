		/*
 * This file is part of the Distributed Network Block Device 3
 *
 * Copyright(c) 2011-2012 Johann Latocha <johann@latocha.de>
 *
 * This file may be licensed under the terms of of the
 * GNU General Public License Version 2 (the ``GPL'').
 *
 * Software distributed under the License is distributed
 * on an ``AS IS'' basis, WITHOUT WARRANTY OF ANY KIND, either
 * express or implied. See the GPL for the specific language
 * governing rights and limitations.
 *
 * You should have received a copy of the GPL along with this
 * program. If not, go to http://www.gnu.org/licenses/gpl.html
 * or write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "server.h"
#include "helper.h"

#include "locks.h"
#include "image.h"
#include "uplink.h"
#include "net.h"
#include "altservers.h"
#include "integrity.h"
#include "threadpool.h"

#include "../version.h"
#include "../shared/sockhelper.h"

#include <signal.h>
#include <getopt.h>
#include <assert.h>

#define LONGOPT_CRC4		1000
#define LONGOPT_ASSERT		1001
#define LONGOPT_CREATE		1002
#define LONGOPT_REVISION	1003
#define LONGOPT_SIZE		1004

poll_list_t *listeners = NULL;

/**
 * Time the server was started
 */
static time_t startupTime = 0;
static bool sigReload = false, sigLogCycle = false;

static dnbd3_client_t* dnbd3_prepareClient(struct sockaddr_storage *client, int fd);

static void dnbd3_handleSignal(int signum);

static void* server_asyncImageListLoad(void *data);

/**
 * Print help text for usage instructions
 */
void dnbd3_printHelp(char *argv_0)
{
	printf( "Version: %s\n\n", VERSION_STRING );
	printf( "Usage: %s [OPTIONS]...\n", argv_0 );
	printf( "Start the DNBD3 server\n" );
	printf( "-c or --config      Configuration directory (default /etc/dnbd3-server/)\n" );
	printf( "-n or --nodaemon    Start server in foreground\n" );
	printf( "-b or --bind        Local Address to bind to\n" );
	//printf( "-r or --reload      Reload configuration file\n" );
	//printf( "-s or --stop        Stop running dnbd3-server\n" );
	//printf( "-i or --info        Print connected clients and used images\n" );
	printf( "-h or --help        Show this help text and quit\n" );
	printf( "-v or --version     Show version and quit\n" );
	printf( "\nManagement functions:\n" );
	printf( "--crc [image-file]  Generate crc block list for given image\n" );
	printf( "--create [image-name] --revision [rid] --size [filesize]\n"
			"\tCreate a local empty image file with a zeroed cache-map for the specified image\n" );
	printf( "\n" );
	exit( 0 );
}

/**
 * Print version information
 */
void dnbd3_printVersion()
{
	printf( "Version: %s\n", VERSION_STRING );
	exit( 0 );
}

/**
 * Clean up structs, connections, write out data, then exit
 */
void dnbd3_cleanup()
{
	int retries;

	_shutdown = true;
	debug_locks_stop_watchdog();
	logadd( LOG_INFO, "Cleanup..." );

	if ( listeners != NULL ) sock_destroyPollList( listeners );
	listeners = NULL;

	// Kill connection to all clients
	net_disconnectAll();

	// Disable threadpool
	threadpool_close();

	// Terminate the altserver checking thread
	altservers_shutdown();

	// Terminate all uplinks
	image_killUplinks();

	// Terminate integrity checker
	integrity_shutdown();

	// Wait for clients to disconnect
	net_waitForAllDisconnected();

	// Clean up images
	retries = 5;
	while ( !image_tryFreeAll() && --retries > 0 ) {
		logadd( LOG_INFO, "Waiting for images to free...\n" );
		sleep( 1 );
	}

	free( _basePath );
	free( _configDir );
	exit( EXIT_SUCCESS );
}

/**
 * Program entry point
 */
int main(int argc, char *argv[])
{
	int demonize = 1;
	int opt = 0;
	int longIndex = 0;
	char *paramCreate = NULL;
	char *bindAddress = NULL;
	int64_t paramSize = -1;
	int paramRevision = -1;
	static const char *optString = "c:d:b:nrsihv?";
	static const struct option longOpts[] = {
			{ "config", required_argument, NULL, 'c' },
			{ "nodaemon", no_argument, NULL, 'n' },
			{ "reload", no_argument, NULL, 'r' },
			{ "stop", no_argument, NULL, 's' },
			{ "info", no_argument, NULL, 'i' },
			{ "help", no_argument, NULL, 'h' },
			{ "version", no_argument, NULL, 'v' },
			{ "bind", required_argument, NULL, 'b' },
			{ "crc", required_argument, NULL, LONGOPT_CRC4 },
			{ "assert", no_argument, NULL, LONGOPT_ASSERT },
			{ "create", required_argument, NULL, LONGOPT_CREATE },
			{ "revision", required_argument, NULL, LONGOPT_REVISION },
			{ "size", required_argument, NULL, LONGOPT_SIZE },
			{ 0, 0, 0, 0 }
	};

	opt = getopt_long( argc, argv, optString, longOpts, &longIndex );

	while ( opt != -1 ) {
		switch ( opt ) {
		case 'c':
			_configDir = strdup( optarg );
			break;
		case 'n':
			demonize = 0;
			break;
		case 'r':
			logadd( LOG_INFO, "Reloading configuration file..." );
			//dnbd3_rpc_send(RPC_RELOAD);
			return EXIT_SUCCESS;
		case 's':
			logadd( LOG_INFO, "Stopping running server..." );
			//dnbd3_rpc_send(RPC_EXIT);
			return EXIT_SUCCESS;
		case 'i':
			logadd( LOG_INFO, "Requesting information..." );
			//dnbd3_rpc_send(RPC_IMG_LIST);
			return EXIT_SUCCESS;
		case 'h':
		case '?':
			dnbd3_printHelp( argv[0] );
			break;
		case 'v':
			dnbd3_printVersion();
			break;
		case 'b':
			bindAddress = strdup( optarg );
			break;
		case LONGOPT_CRC4:
			return image_generateCrcFile( optarg ) ? 0 : EXIT_FAILURE;
		case LONGOPT_ASSERT:
			printf( "Testing a failing assertion:\n" );
			assert( 4 == 5 );
			printf( "Assertion 4 == 5 seems to hold. ;-)\n" );
			return EXIT_SUCCESS;
		case LONGOPT_CREATE:
			paramCreate = strdup( optarg );
			break;
		case LONGOPT_REVISION:
			paramRevision = atoi( optarg );
			break;
		case LONGOPT_SIZE:
			paramSize = strtoll( optarg, NULL, 10 );
			break;
		}
		opt = getopt_long( argc, argv, optString, longOpts, &longIndex );
	}

	// Load general config

	if ( _configDir == NULL ) _configDir = strdup( "/etc/dnbd3-server" );
	globals_loadConfig();
	if ( _basePath == NULL ) {
		logadd( LOG_ERROR, "basePath not set in %s/%s", _configDir, CONFIG_FILENAME );
		exit( EXIT_FAILURE );
	}

	// One-shots first:

	if ( paramCreate != NULL ) {
		return image_create( paramCreate, paramRevision, paramSize ) ? 0 : EXIT_FAILURE;
	}

	// No one-shot detected, normal server operation

	if ( demonize ) daemon( 1, 0 );
	image_serverStartup();
	altservers_init();
	integrity_init();
	net_init();
	uplink_globalsInit();
	logadd( LOG_INFO, "DNBD3 server starting.... Machine type: " ENDIAN_MODE );

	if ( altservers_load() < 0 ) {
		logadd( LOG_WARNING, "Could not load alt-servers. Does the file exist in %s?", _configDir );
	}

#ifdef _DEBUG
	debug_locks_start_watchdog();
#endif

	// setup signal handler
	signal( SIGTERM, dnbd3_handleSignal );
	signal( SIGINT, dnbd3_handleSignal );
	signal( SIGUSR1, dnbd3_handleSignal );
	signal( SIGHUP, dnbd3_handleSignal );
	signal( SIGUSR2, dnbd3_handleSignal );

	logadd( LOG_INFO, "Loading images...." );
	// Load all images in base path
	if ( !image_loadAll( NULL ) || _shutdown ) {
		logadd( LOG_ERROR, "Could not load images." );
		free( bindAddress );
		dnbd3_cleanup();
		return 0;
	}

	startupTime = time( NULL );

	// Give other threads some time to start up before accepting connections
	sleep( 1 );

	// setup network
	listeners = sock_newPollList();
	if ( listeners == NULL ) {
		logadd( LOG_ERROR, "Didnt get a poll list!" );
		exit( EXIT_FAILURE );
	}
	if ( !sock_listen( listeners, bindAddress, _listenPort ) ) {
		logadd( LOG_ERROR, "Could not listen on any local interface." );
		exit( EXIT_FAILURE );
	}
	struct sockaddr_storage client;
	socklen_t len;
	int fd;

	// Initialize thread pool
	if ( !threadpool_init( 8 ) ) {
		logadd( LOG_ERROR, "Could not init thread pool!\n" );
		exit( EXIT_FAILURE );
	}

	logadd( LOG_INFO, "Server is ready..." );

	// +++++++++++++++++++++++++++++++++++++++++++++++++++ main loop
	while ( !_shutdown ) {
		// Handle signals
		if ( sigReload ) {
			sigReload = false;
			logadd( LOG_INFO, "SIGHUP received, re-scanning image directory" );
			threadpool_run( &server_asyncImageListLoad, NULL );
		}
		if ( sigLogCycle ) {
			sigLogCycle = false;
			logadd( LOG_INFO, "SIGUSR2 received, reopening log file..." );
			if ( log_openLogFile( NULL ) )
				logadd( LOG_INFO, "Log file has been reopened." );
			else
				logadd( LOG_WARNING, "Could not cycle log file." );
		}
		//
		len = sizeof(client);
		fd = sock_accept( listeners, &client, &len );
		if ( fd < 0 ) {
			const int err = errno;
			if ( err == EINTR || err == EAGAIN ) continue;
			logadd( LOG_ERROR, "Client accept failure (err=%d)", err );
			usleep( 10000 ); // 10ms
			continue;
		}

		dnbd3_client_t *dnbd3_client = dnbd3_prepareClient( &client, fd );
		if ( dnbd3_client == NULL ) {
			close( fd );
			continue;
		}

		if ( !threadpool_run( &net_handleNewConnection, (void *)dnbd3_client ) ) {
			logadd( LOG_ERROR, "Could not start thread for new connection." );
			free( dnbd3_client );
			continue;
		}
	}
	free( bindAddress );
	dnbd3_cleanup();
}

/**
 * Initialize and partially populate the client struct - called when an incoming
 * connection is accepted. As this might be an HTTP request we don't initialize the
 * locks, that would happen later once we know.
 */
static dnbd3_client_t* dnbd3_prepareClient(struct sockaddr_storage *client, int fd)
{
	dnbd3_client_t *dnbd3_client = calloc( 1, sizeof(dnbd3_client_t) );
	if ( dnbd3_client == NULL ) { // This will never happen thanks to memory overcommit
		logadd( LOG_ERROR, "Could not alloc dnbd3_client_t for new client." );
		return NULL;
	}

	if ( client->ss_family == AF_INET ) {
		struct sockaddr_in *v4 = (struct sockaddr_in *)client;
		dnbd3_client->host.type = AF_INET;
		memcpy( dnbd3_client->host.addr, &(v4->sin_addr), 4 );
		dnbd3_client->host.port = v4->sin_port;
	} else if ( client->ss_family == AF_INET6 ) {
		struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)client;
		dnbd3_client->host.type = AF_INET6;
		memcpy( dnbd3_client->host.addr, &(v6->sin6_addr), 16 );
		dnbd3_client->host.port = v6->sin6_port;
	} else {
		logadd( LOG_ERROR, "New client has unknown address family %d, disconnecting...", (int)client->ss_family );
		free( dnbd3_client );
		return NULL;
	}
	dnbd3_client->sock = fd;
	return dnbd3_client;
}

static void dnbd3_handleSignal(int signum)
{
	if ( signum == SIGINT || signum == SIGTERM ) {
		_shutdown = true;
	} else if ( signum == SIGUSR1 || signum == SIGHUP ) {
		sigReload = true;
	} else if ( signum == SIGUSR2 ) {
		sigLogCycle = true;
	}
}

int dnbd3_serverUptime()
{
	return (int)(time( NULL ) - startupTime);
}

static void* server_asyncImageListLoad(void *data UNUSED)
{
	setThreadName( "img-list-loader" );
	image_loadAll( NULL );
	return NULL;
}

