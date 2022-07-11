#include "cowfile.h"
#include "math.h"
extern void image_ll_getattr( fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi );

static int cowFileVersion = 1;
static bool statStdout;
static bool statFile;
static pthread_t tidCowUploader;
static pthread_t tidStatUpdater;
static char *cowServerAddress;
static CURL *curl;
static cowfile_metadata_header_t *metadata = NULL;
static atomic_uint_fast64_t bytesUploaded;
static uint64_t totalBlocksUploaded = 0;
static atomic_int activeUploads = 0;
atomic_bool uploadLoop = true;
atomic_bool uploadLoopDone = false;

static struct cow
{
	pthread_mutex_t l2CreateLock;
	int fhm;
	int fhd;
	int fhs;
	char *metadata_mmap;
	l1 *l1;
	l2 *firstL2;
	size_t maxImageSize;
	size_t l1Size; //size of l1 array

} cow;

/**
 * @brief Computes the l1 offset from the absolute file offset
 * 
 * @param offset absolute file offset
 * @return int l2 offset
 */
static int getL1Offset( size_t offset )
{
	return (int)( offset / COW_L2_STORAGE_CAPACITY );
}

/**
 * @brief Computes the l2 offset from the absolute file offset
 * 
 * @param offset absolute file offset
 * @return int l2 offset
 */
static int getL2Offset( size_t offset )
{
	return (int)( ( offset % COW_L2_STORAGE_CAPACITY ) / COW_METADATA_STORAGE_CAPACITY );
}

/**
 * @brief Computes the bit in the bitfield from the absolute file offset
 * 
 * @param offset absolute file offset
 * @return int bit(0-319) in the bitfield
 */
static int getBitfieldOffset( size_t offset )
{
	return (int)( offset / DNBD3_BLOCK_SIZE ) % ( COW_BITFIELD_SIZE * 8 );
}

/**
 * @brief Sets the specified bits in the specified range threadsafe to 1.
 * 
 * @param byte of a bitfield
 * @param from start bit
 * @param to end bit
 * @param value set bits to 1 or 0
 */
static void setBits( char *byte, int from, int to, bool value )
{
	char mask = (char)( ( 255 >> ( 7 - ( to - from ) ) ) << from );
	if (value) {
	    atomic_fetch_or( byte,  mask );
	} else {
	    atomic_fetch_and( byte, ~mask );
	}
}

/**
 * @brief Sets the specified bits in the specified range threadsafe to 1.
 * 
 * @param bitfield of a cow_block_metadata
 * @param from start bit
 * @param to end bit
 * @param value set bits to 1 or 0
 */
static void setBitsInBitfield( atomic_char *bitfield, int from, int to, bool value )
{
	assert( from >= 0 || to < COW_BITFIELD_SIZE * 8 );
	int start = from / 8;
	int end = to / 8;

	for ( int i = start; i <= end; i++ ) {
		setBits( ( bitfield + i ), from - i * 8, MIN( 7, to - i * 8 ), value );
		from = ( i + 1 ) * 8;
	}
}

/**
 * @brief Checks if the n bit of a bit field is 0 or 1.
 * 
 * @param bitfield of a cow_block_metadata
 * @param n the bit which should be checked
 */
static bool checkBit( atomic_char *bitfield, int n )
{
	return ( atomic_load( ( bitfield + ( n / 8 ) ) ) >> ( n % 8 ) ) & 1;
}


/**
 * @brief Implementation of CURLOPT_WRITEFUNCTION , this function will be called when
 * the server sends back data.
 * for more details see: https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION .html
 *  
 * @param buffer that contains the response data from the server
 * @param itemSize size of one item
 * @param nitems number of items
 * @param response userdata which will later contain the uuid
 * @return size_t size that have been read
 */
size_t curlCallbackCreateSession( char *buffer, size_t itemSize, size_t nitems, void *response )
{
	size_t bytes = itemSize * nitems;
	if ( strlen( response ) + bytes != 36 ) {
		logadd( LOG_INFO, "strlen(response): %lu bytes: %lu \n", strlen( response ), bytes );
		return bytes;
	}

	strncat( response, buffer, 36 );
	return bytes;
}

/**
 * @brief Create a Session with the cow server and gets the session guid.
 * 
 * @param imageName 
 * @param version of the original Image
 */
bool createSession( const char *imageName, uint16_t version )
{
	CURLcode res;
	char url[COW_URL_STRING_SIZE];
	snprintf( url, COW_URL_STRING_SIZE, COW_API_CREATE, cowServerAddress );
	logadd( LOG_INFO, "COW_API_CREATE URL: %s", url );
	curl_easy_setopt( curl, CURLOPT_POST, 1L );
	curl_easy_setopt( curl, CURLOPT_URL, url );

	curl_mime *mime;
	curl_mimepart *part;
	mime = curl_mime_init( curl );
	part = curl_mime_addpart( mime );
	curl_mime_name( part, "imageName" );
	curl_mime_data( part, imageName, CURL_ZERO_TERMINATED );
	part = curl_mime_addpart( mime );
	curl_mime_name( part, "version" );
	char buf[sizeof( int ) * 3 + 2];
	snprintf( buf, sizeof buf, "%d", version );
	curl_mime_data( part, buf, CURL_ZERO_TERMINATED );

	part = curl_mime_addpart( mime );
	curl_mime_name( part, "bitfieldSize" );
	snprintf( buf, sizeof buf, "%d", metadata->bitfieldSize );
	curl_mime_data( part, buf, CURL_ZERO_TERMINATED );

	curl_easy_setopt( curl, CURLOPT_MIMEPOST, mime );

	metadata->uuid[0] = '\0';
	curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, curlCallbackCreateSession );
	curl_easy_setopt( curl, CURLOPT_WRITEDATA, &metadata->uuid );

	res = curl_easy_perform( curl );
	curl_mime_free( mime );

	/* Check for errors */
	if ( res != CURLE_OK ) {
		logadd( LOG_ERROR, "COW_API_CREATE  failed: %s\n", curl_easy_strerror( res ) );
		return false;
	}

	long http_code = 0;
	curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &http_code );
	if ( http_code != 200 ) {
		logadd( LOG_ERROR, "COW_API_CREATE  failed http: %ld\n", http_code );
		return false;
	}
	curl_easy_reset( curl );
	metadata->uuid[36] = '\0';
	logadd( LOG_DEBUG1, "Cow session started, guid: %s\n", metadata->uuid );
	return true;
}

/**
 * @brief Implementation of CURLOPT_READFUNCTION, this function will first send the bit field and
 * then the block data in one bitstream. this function is usually called multiple times per block,
 * because the buffer is usually not large for one block and its bitfield.
 * for more details see: https://curl.se/libcurl/c/CURLOPT_READFUNCTION.html
 * 
 * @param ptr to the buffer
 * @param size of one element in buffer
 * @param nmemb number of elements in buffer
 * @param userdata from CURLOPT_READFUNCTION
 * @return size_t size written in buffer
 */
size_t curlReadCallbackUploadBlock( char *ptr, size_t size, size_t nmemb, void *userdata )
{
	cow_curl_read_upload_t *uploadBlock = (cow_curl_read_upload_t *)userdata;
	size_t len = 0;
	if ( uploadBlock->position < (size_t)metadata->bitfieldSize ) {
		size_t lenCpy = MIN( metadata->bitfieldSize - uploadBlock->position, size * nmemb );
		memcpy( ptr, uploadBlock->block->bitfield + uploadBlock->position, lenCpy );
		uploadBlock->position += lenCpy;
		len += lenCpy;
	}
	if ( uploadBlock->position >= (size_t)metadata->bitfieldSize ) {
		size_t lenRead = MIN( COW_METADATA_STORAGE_CAPACITY - ( uploadBlock->position - ( metadata->bitfieldSize ) ),
				( size * nmemb ) - len );
		off_t inBlockOffset = uploadBlock->position - metadata->bitfieldSize;
		size_t lengthRead = pread( cow.fhd, ( ptr + len ), lenRead, uploadBlock->block->offset + inBlockOffset );

		if ( lenRead != lengthRead ) {
			// fill up since last block may not be a full block
			lengthRead = lenRead;
		}
		uploadBlock->position += lengthRead;
		len += lengthRead;
	}
	return len;
}


/**
 * @brief Requests the merging of the image on the cow server.

 */
bool mergeRequest()
{
	CURLcode res;
	curl_easy_setopt( curl, CURLOPT_POST, 1L );

	char url[COW_URL_STRING_SIZE];
	snprintf( url, COW_URL_STRING_SIZE, COW_API_START_MERGE, cowServerAddress );
	curl_easy_setopt( curl, CURLOPT_URL, url );


	curl_mime *mime;
	curl_mimepart *part;
	mime = curl_mime_init( curl );
	part = curl_mime_addpart( mime );

	curl_mime_name( part, "guid" );
	curl_mime_data( part, metadata->uuid, CURL_ZERO_TERMINATED );
	part = curl_mime_addpart( mime );

	curl_mime_name( part, "fileSize" );
	char buf[21];
	snprintf( buf, sizeof buf, "%" PRIu64, metadata->imageSize );
	curl_mime_data( part, buf, CURL_ZERO_TERMINATED );
	curl_easy_setopt( curl, CURLOPT_MIMEPOST, mime );


	res = curl_easy_perform( curl );
	if ( res != CURLE_OK ) {
		logadd( LOG_WARNING, "COW_API_START_MERGE  failed: %s\n", curl_easy_strerror( res ) );
		curl_easy_reset( curl );
		return false;
	}
	long http_code = 0;
	curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, &http_code );
	if ( http_code != 200 ) {
		logadd( LOG_WARNING, "COW_API_START_MERGE  failed http: %ld\n", http_code );
		curl_easy_reset( curl );
		return false;
	}
	curl_easy_reset( curl );
	curl_mime_free( mime );
	return true;
}

/**
 * @brief Wrapper for mergeRequest so if its fails it will be tried again.
 * 
 */
void startMerge()
{
	int fails = 0;
	bool success = false;
	success = mergeRequest();
	while ( fails <= 5 && !success ) {
		fails++;
		logadd( LOG_WARNING, "Trying again. %i/5", fails );
		mergeRequest();
	}
}

/**
 * @brief Implementation of the CURLOPT_XFERINFOFUNCTION.
 * For more infos see: https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html
 * 
 * Each active transfer callbacks this function.
 * This function computes the uploaded bytes between each call and adds it to
 * bytesUploaded, which is used to compute the kb/s uploaded over all transfers.
 * 
 * @param clientp 
 * @param ulNow number of bytes uploaded by this transfer so far.
 * @return int always returns 0 to continue the callbacks.
 */
int progress_callback( void *clientp, __attribute__( ( unused ) ) curl_off_t dlTotal,
		__attribute__( ( unused ) ) curl_off_t dlNow, __attribute__( ( unused ) ) curl_off_t ulTotal, curl_off_t ulNow )
{
	CURL *eh = (CURL *)clientp;
	cow_curl_read_upload_t *curlUploadBlock;
	CURLcode res;
	res = curl_easy_getinfo( eh, CURLINFO_PRIVATE, &curlUploadBlock );
	if ( res != CURLE_OK ) {
		logadd( LOG_ERROR, "ERROR" );
		return 0;
	}
	bytesUploaded += ( ulNow - curlUploadBlock->ulLast );
	curlUploadBlock->ulLast = ulNow;
	return 0;
}

/**
 * @brief Updates the status to the stdout/statfile depending on the startup parameters.
 * 
 * @param inQueue Blocks that have changes old enough to be uploaded.
 * @param modified Blocks that have been changed but whose changes are not old enough to be uploaded.
 * @param idle Blocks that do not contain changes that have not yet been uploaded.
 * @param speedBuffer ptr to char array that contains the current upload speed.
 */

void updateCowStatsFile( uint64_t inQueue, uint64_t modified, uint64_t idle, char *speedBuffer )
{
	char buffer[300];
	char state[30];
	if ( uploadLoop ) {
		snprintf( state, 30, "%s", "backgroundUpload" );
	} else if ( !uploadLoopDone ) {
		snprintf( state, 30, "%s", "uploading" );
	} else {
		snprintf( state, 30, "%s", "done" );
	}

	int len = snprintf( buffer, 300,
			"state=%s\n"
			"inQueue=%" PRIu64 "\n"
			"modifiedBlocks=%" PRIu64 "\n"
			"idleBlocks=%" PRIu64 "\n"
			"totalBlocksUploaded=%" PRIu64 "\n"
			"activeUploads:%i\n"
			"%s=%s",
			state, inQueue, modified, idle, totalBlocksUploaded, activeUploads, COW_SHOW_UL_SPEED ? "ulspeed" : "",
			speedBuffer );

	if ( statStdout ) {
		logadd( LOG_INFO, "%s", buffer );
	}

	if ( statFile ) {
		if ( pwrite( cow.fhs, buffer, len, 43 ) != len ) {
			logadd( LOG_WARNING, "Could not update cow status file" );
		}
		if ( ftruncate( cow.fhs, 43 + len ) ) {
			logadd( LOG_WARNING, "Could not truncate cow status file" );
		}
#ifdef COW_DUMP_BLOCK_UPLOADS
		if ( !uploadLoop && uploadLoopDone ) {
			dumpBlockUploads();
		}
#endif
	}
}
int cmpfunc( const void *a, const void *b )
{
	return (int)( ( (cow_block_upload_statistics_t *)b )->uploads - ( (cow_block_upload_statistics_t *)a )->uploads );
}
/**
 * @brief Writes all block numbers sorted by the number of uploads into the statsfile.
 * 
 */
void dumpBlockUploads()
{
	long unsigned int l1MaxOffset = 1 + ( ( metadata->imageSize - 1 ) / COW_L2_STORAGE_CAPACITY );

	cow_block_upload_statistics_t blockUploads[l1MaxOffset * COW_L2_SIZE];
	uint64_t currentBlock = 0;
	for ( long unsigned int l1Offset = 0; l1Offset < l1MaxOffset; l1Offset++ ) {
		if ( cow.l1[l1Offset] == -1 ) {
			continue;
		}
		for ( int l2Offset = 0; l2Offset < COW_L2_SIZE; l2Offset++ ) {
			cow_block_metadata_t *block = ( cow.firstL2[cow.l1[l1Offset]] + l2Offset );

			blockUploads[currentBlock].uploads = block->uploads;
			blockUploads[currentBlock].blocknumber = ( l1Offset * COW_L2_SIZE + l2Offset );
			currentBlock++;
		}
	}
	qsort( blockUploads, currentBlock, sizeof( cow_block_upload_statistics_t ), cmpfunc );
	lseek( cow.fhs, 0, SEEK_END );

	dprintf( cow.fhs, "\n\nblocknumber: uploads\n==Block Upload Dump===\n" );
	for ( uint64_t i = 0; i < currentBlock; i++ ) {
		dprintf( cow.fhs, "%" PRIu64 ": %" PRIu64 " \n", blockUploads[i].blocknumber, blockUploads[i].uploads );
	}
}

/**
 * @brief Starts the upload of a given block.
 * 
 * @param cm Curl_multi
 * @param curlUploadBlock containing the data for the block to upload.
 */
bool addUpload( CURLM *cm, cow_curl_read_upload_t *curlUploadBlock, struct curl_slist *headers )
{
	CURL *eh = curl_easy_init();

	char url[COW_URL_STRING_SIZE];

	snprintf( url, COW_URL_STRING_SIZE, COW_API_UPDATE, cowServerAddress, metadata->uuid, curlUploadBlock->blocknumber );

	curl_easy_setopt( eh, CURLOPT_URL, url );
	curl_easy_setopt( eh, CURLOPT_POST, 1L );
	curl_easy_setopt( eh, CURLOPT_READFUNCTION, curlReadCallbackUploadBlock );
	curl_easy_setopt( eh, CURLOPT_READDATA, (void *)curlUploadBlock );
	curl_easy_setopt( eh, CURLOPT_PRIVATE, (void *)curlUploadBlock );
	// min upload speed of 1kb/s over 10 sec otherwise the upload is canceled.
	curl_easy_setopt( eh, CURLOPT_LOW_SPEED_TIME, 10L);
  	curl_easy_setopt( eh, CURLOPT_LOW_SPEED_LIMIT, 1000L);

	curl_easy_setopt(
			eh, CURLOPT_POSTFIELDSIZE_LARGE, (long)( metadata->bitfieldSize + COW_METADATA_STORAGE_CAPACITY ) );
	if ( COW_SHOW_UL_SPEED ) {
		curlUploadBlock->ulLast = 0;
		curl_easy_setopt( eh, CURLOPT_NOPROGRESS, 0L );
		curl_easy_setopt( eh, CURLOPT_XFERINFOFUNCTION, progress_callback );
		curl_easy_setopt( eh, CURLOPT_XFERINFODATA, eh );
	}
	curl_easy_setopt( eh, CURLOPT_HTTPHEADER, headers );
	curl_multi_add_handle( cm, eh );

	return true;
}

/**
 * @brief After an upload completes, either successful or unsuccessful this
 * function cleans everything up. If unsuccessful and there are some tries left
 * retries to upload the block.
 * 
 * @param cm Curl_multi
 * @param msg CURLMsg
 * @return true returned if the upload was successful or retries are still possible.
 * @return false returned if the upload was unsuccessful.
 */
bool finishUpload( CURLM *cm, CURLMsg *msg, struct curl_slist *headers )
{
	bool status = true;
	cow_curl_read_upload_t *curlUploadBlock;
	CURLcode res;
	CURLcode res2;
	res = curl_easy_getinfo( msg->easy_handle, CURLINFO_PRIVATE, &curlUploadBlock );

	long http_code = 0;
	res2 = curl_easy_getinfo( msg->easy_handle, CURLINFO_RESPONSE_CODE, &http_code );

	if ( res != CURLE_OK || res2 != CURLE_OK || http_code != 200 || msg->msg != CURLMSG_DONE ) {
		curlUploadBlock->fails++;
		logadd( LOG_ERROR, "COW_API_UPDATE  failed %i/5: %s\n", curlUploadBlock->fails,
				curl_easy_strerror( msg->data.result ) );
		if ( curlUploadBlock->fails <= 5 ) {
			addUpload( cm, curlUploadBlock, headers );
			goto CLEANUP;
		}
		free( curlUploadBlock );
		status = false;
		goto CLEANUP;
	}

	// everything went ok, update timeChanged
	atomic_compare_exchange_strong( &curlUploadBlock->block->timeChanged, &curlUploadBlock->time, 0 );

	curlUploadBlock->block->uploads++;

	totalBlocksUploaded++;
	free( curlUploadBlock );
CLEANUP:
	curl_multi_remove_handle( cm, msg->easy_handle );
	curl_easy_cleanup( msg->easy_handle );
	return status;
}

/**
 * @brief 
 * 
 * @param cm Curl_multi
 * @param activeUploads ptr to integer which holds the number of current uploads
 * @param breakIfNotMax will return as soon as there are not all upload slots used, so they can be filled up.
 * @param foregroundUpload used to determine the number of max uploads. If true COW_MAX_PARALLEL_UPLOADS will be the limit,
 * else COW_MAX_PARALLEL_BACKGROUND_UPLOADS.
 * @return true returned if all upload's were successful 
 * @return false returned if  one ore more upload's failed.
 */
bool MessageHandler(
		CURLM *cm, atomic_int *activeUploads, bool breakIfNotMax, bool foregroundUpload, struct curl_slist *headers )
{
	CURLMsg *msg;
	int msgsLeft = -1;
	bool status = true;
	do {
		curl_multi_perform( cm, activeUploads );

		while ( ( msg = curl_multi_info_read( cm, &msgsLeft ) ) ) {
			if ( !finishUpload( cm, msg, headers ) ) {
				status = false;
			}
		}
		if ( breakIfNotMax
				&& *activeUploads
						< ( foregroundUpload ? COW_MAX_PARALLEL_UPLOADS : COW_MAX_PARALLEL_BACKGROUND_UPLOADS ) ) {
			break;
		}
		// ony wait if there are active uploads
		if ( *activeUploads ) {
			curl_multi_wait( cm, NULL, 0, 1000, NULL );
		}

	} while ( *activeUploads );
	return status;
}

/**
 * @brief loops through all blocks and uploads them.
 * 
 * @param ignoreMinUploadDelay If true uploads all blocks that have changes while
 * ignoring COW_MIN_UPLOAD_DELAY
 * @param cm Curl_multi
 * @return true if all blocks uploaded successful
 * @return false if one ore more blocks failed to upload
 */
bool uploaderLoop( bool ignoreMinUploadDelay, CURLM *cm )
{
	bool success = true;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append( headers, "Content-Type: application/octet-stream" );

	long unsigned int l1MaxOffset = 1 + ( ( metadata->imageSize - 1 ) / COW_L2_STORAGE_CAPACITY );
	for ( long unsigned int l1Offset = 0; l1Offset < l1MaxOffset; l1Offset++ ) {
		if ( cow.l1[l1Offset] == -1 ) {
			continue;
		}
		for ( int l2Offset = 0; l2Offset < COW_L2_SIZE; l2Offset++ ) {
			cow_block_metadata_t *block = ( cow.firstL2[cow.l1[l1Offset]] + l2Offset );
			if ( block->offset == -1 ) {
				continue;
			}
			if ( block->timeChanged != 0 ) {
				if ( ( time( NULL ) - block->timeChanged > COW_MIN_UPLOAD_DELAY ) || ignoreMinUploadDelay ) {
					do {
						if ( !MessageHandler( cm, &activeUploads, true, ignoreMinUploadDelay, headers ) ) {
							success = false;
						}
					} while ( !( activeUploads < ( ignoreMinUploadDelay ? COW_MAX_PARALLEL_UPLOADS
																						 : COW_MAX_PARALLEL_BACKGROUND_UPLOADS ) )
							&& activeUploads );
					cow_curl_read_upload_t *b = malloc( sizeof( cow_curl_read_upload_t ) );
					b->block = block;
					b->blocknumber = ( l1Offset * COW_L2_SIZE + l2Offset );
					b->fails = 0;
					b->position = 0;
					b->time = block->timeChanged;
					addUpload( cm, b, headers );
					if ( !ignoreMinUploadDelay && !uploadLoop ) {
						goto DONE;
					}
				}
			}
		}
	}
DONE:
	while ( activeUploads > 0 ) {
		MessageHandler( cm, &activeUploads, false, ignoreMinUploadDelay, headers );
	}
	curl_slist_free_all( headers );
	return success;
}


/**
 * @brief Computes the data for the status to the stdout/statfile every COW_STATS_UPDATE_TIME seconds.
 * 
 */

void *cowfile_statUpdater( __attribute__( ( unused ) ) void *something )
{
	uint64_t lastUpdateTime = time( NULL );

	while ( !uploadLoopDone ) {
		sleep( COW_STATS_UPDATE_TIME );
		int modified = 0;
		int inQueue = 0;
		int idle = 0;
		long unsigned int l1MaxOffset = 1 + ( ( metadata->imageSize - 1 ) / COW_L2_STORAGE_CAPACITY );
		uint64_t now = time( NULL );
		for ( long unsigned int l1Offset = 0; l1Offset < l1MaxOffset; l1Offset++ ) {
			if ( cow.l1[l1Offset] == -1 ) {
				continue;
			}
			for ( int l2Offset = 0; l2Offset < COW_L2_SIZE; l2Offset++ ) {
				cow_block_metadata_t *block = ( cow.firstL2[cow.l1[l1Offset]] + l2Offset );
				if ( block->offset == -1 ) {
					continue;
				}
				if ( block->timeChanged != 0 ) {
					if ( !uploadLoop || now > block->timeChanged + COW_MIN_UPLOAD_DELAY ) {
						inQueue++;
					} else {
						modified++;
					}
				} else {
					idle++;
				}
			}
		}
		char speedBuffer[20];

		if ( COW_SHOW_UL_SPEED ) {
			now = time( NULL );
			uint64_t bytes = atomic_exchange( &bytesUploaded, 0 );
			snprintf( speedBuffer, 20, "%.2f", (double)( ( bytes ) / ( 1 + now - lastUpdateTime ) / 1000 ) );

			lastUpdateTime = now;
		}


		updateCowStatsFile( inQueue, modified, idle, speedBuffer );
	}
}

/**
 * @brief main loop for blockupload in the background
 */
void *cowfile_uploader( __attribute__( ( unused ) ) void *something )
{
	CURLM *cm;

	cm = curl_multi_init();
	curl_multi_setopt(
			cm, CURLMOPT_MAXCONNECTS, (long)MAX( COW_MAX_PARALLEL_UPLOADS, COW_MAX_PARALLEL_BACKGROUND_UPLOADS ) );


	while ( uploadLoop ) {
		uploaderLoop( false, cm );
		sleep( 2 );
	}
	logadd( LOG_DEBUG1, "start uploading the remaining blocks." );

	// force the upload of all remaining blocks because the user dismounted the image
	if ( !uploaderLoop( true, cm ) ) {
		logadd( LOG_ERROR, "one or more blocks failed to upload" );
		curl_multi_cleanup( cm );
		uploadLoopDone = true;
		return NULL;
	}
	uploadLoopDone = true;
	curl_multi_cleanup( cm );
	logadd( LOG_DEBUG1, "all blocks uploaded" );
	if ( cow_merge_after_upload ) {
		startMerge();
		logadd( LOG_DEBUG1, "Requesting merge." );
	}
	return NULL;
}

/**
 * @brief Create a Cow Stats File  an inserts the session guid
 * 
 * @param path where the file is created
 * @return true 
 * @return false if failed to create or to write into the file
 */
bool createCowStatsFile( char *path )
{
	char pathStatus[strlen( path ) + 12];

	snprintf( pathStatus, strlen( path ) + 12, "%s%s", path, "/status.txt" );

	char buffer[100];
	int len = snprintf( buffer, 100, "uuid=%s\nstate: active\n", metadata->uuid );
	if ( statStdout ) {
		logadd( LOG_INFO, "%s", buffer );
	}
	if ( statFile ) {
		if ( ( cow.fhs = open( pathStatus, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR ) ) == -1 ) {
			logadd( LOG_ERROR, "Could not create cow status file. Bye.\n" );
			return false;
		}

		if ( pwrite( cow.fhs, buffer, len, 0 ) != len ) {
			logadd( LOG_ERROR, "Could not write to cow status file. Bye.\n" );
			return false;
		}
	}
	return true;
}

/**
 * @brief initializes the cow functionality, creates the data & meta file.
 * 
 * @param path where the files should be stored
 * @param image_Name name of the original file/image
 * @param imageSizePtr 
 */
bool cowfile_init( char *path, const char *image_Name, uint16_t imageVersion, atomic_uint_fast64_t **imageSizePtr,
		char *serverAddress, bool sStdout, bool sfile )
{
	statStdout = sStdout;
	statFile = sfile;
	char pathMeta[strlen( path ) + 6];
	char pathData[strlen( path ) + 6];

	snprintf( pathMeta, strlen( path ) + 6, "%s%s", path, "/meta" );
	snprintf( pathData, strlen( path ) + 6, "%s%s", path, "/data" );

	if ( ( cow.fhm = open( pathMeta, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR ) ) == -1 ) {
		logadd( LOG_ERROR, "Could not create cow meta file. Bye.\n %s \n", pathMeta );
		return false;
	}

	if ( ( cow.fhd = open( pathData, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR ) ) == -1 ) {
		logadd( LOG_ERROR, "Could not create cow data file. Bye.\n" );
		return false;
	}

	int maxPageSize = 8192;


	size_t metaDataSizeHeader = sizeof( cowfile_metadata_header_t ) + strlen( image_Name );


	cow.maxImageSize = COW_MAX_IMAGE_SIZE;
	cow.l1Size = ( ( cow.maxImageSize + COW_L2_STORAGE_CAPACITY - 1LL ) / COW_L2_STORAGE_CAPACITY );

	// size of l1 array + number of l2's * size of l2
	size_t metadata_size = cow.l1Size * sizeof( l1 ) + cow.l1Size * sizeof( l2 );

	// compute next fitting multiple of getpagesize()
	size_t meta_data_start = ( ( metaDataSizeHeader + maxPageSize - 1 ) / maxPageSize ) * maxPageSize;

	size_t metadataFileSize = meta_data_start + metadata_size;
	if ( pwrite( cow.fhm, "", 1, metadataFileSize ) != 1 ) {
		logadd( LOG_ERROR, "Could not write cow meta_data_table to file. Bye.\n" );
		return false;
	}

	cow.metadata_mmap = mmap( NULL, metadataFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, cow.fhm, 0 );


	if ( cow.metadata_mmap == MAP_FAILED ) {
		logadd( LOG_ERROR, "Error while mapping mmap:\n%s \n Bye.\n", strerror( errno ) );
		return false;
	}

	metadata = (cowfile_metadata_header_t *)( cow.metadata_mmap );
	metadata->magicValue = COW_FILE_META_MAGIC_VALUE;
	metadata->version = cowFileVersion;
	metadata->dataFileSize = ATOMIC_VAR_INIT( 0 );
	metadata->metadataFileSize = ATOMIC_VAR_INIT( 0 );
	metadata->metadataFileSize = metadataFileSize;
	metadata->blocksize = DNBD3_BLOCK_SIZE;
	metadata->originalImageSize = **imageSizePtr;
	metadata->imageSize = metadata->originalImageSize;
	metadata->creationTime = time( NULL );
	*imageSizePtr = &metadata->imageSize;
	metadata->metaDataStart = meta_data_start;
	metadata->bitfieldSize = COW_BITFIELD_SIZE;
	metadata->maxImageSize = cow.maxImageSize;
	snprintf( metadata->imageName, 200, "%s", image_Name );
	cow.l1 = (l1 *)( cow.metadata_mmap + meta_data_start );
	metadata->nextL2 = 0;

	for ( size_t i = 0; i < cow.l1Size; i++ ) {
		cow.l1[i] = -1;
	}
	cow.firstL2 = (l2 *)( ( (char *)cow.l1 ) + cow.l1Size );

	// write header to data file
	uint64_t header = COW_FILE_DATA_MAGIC_VALUE;
	if ( pwrite( cow.fhd, &header, sizeof( uint64_t ), 0 ) != sizeof( uint64_t ) ) {
		logadd( LOG_ERROR, "Could not write header to cow data file. Bye.\n" );
		return false;
	}
	// move the dataFileSize to make room for the header
	atomic_store( &metadata->dataFileSize, COW_METADATA_STORAGE_CAPACITY );

	pthread_mutex_init( &cow.l2CreateLock, NULL );


	cowServerAddress = serverAddress;
	curl_global_init( CURL_GLOBAL_ALL );
	curl = curl_easy_init();
	if ( !curl ) {
		logadd( LOG_ERROR, "Error on curl init. Bye.\n" );
		return false;
	}
	if ( !createSession( image_Name, imageVersion ) ) {
		return false;
	}

	createCowStatsFile( path );
	pthread_create( &tidCowUploader, NULL, &cowfile_uploader, NULL );
	if ( statFile || statStdout ) {
		pthread_create( &tidStatUpdater, NULL, &cowfile_statUpdater, NULL );
	}
	return true;
}

/**
 * @brief loads an existing cow state from the meta & data files
 * 
 * @param path where the meta & data file is located 
 * @param imageSizePtr 
 */
bool cowfile_load( char *path, atomic_uint_fast64_t **imageSizePtr, char *serverAddress, bool sStdout, bool sFile )
{
	statStdout = sStdout;
	statFile = sFile;
	cowServerAddress = serverAddress;
	curl_global_init( CURL_GLOBAL_ALL );
	curl = curl_easy_init();
	char pathMeta[strlen( path ) + 6];
	char pathData[strlen( path ) + 6];

	snprintf( pathMeta, strlen( path ) + 6, "%s%s", path, "/meta" );
	snprintf( pathData, strlen( path ) + 6, "%s%s", path, "/data" );


	if ( ( cow.fhm = open( pathMeta, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR ) ) == -1 ) {
		logadd( LOG_ERROR, "Could not open cow meta file. Bye.\n" );
		return false;
	}
	if ( ( cow.fhd = open( pathData, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR ) ) == -1 ) {
		logadd( LOG_ERROR, "Could not open cow data file. Bye.\n" );
		return false;
	}

	cowfile_metadata_header_t header;
	{
		size_t sizeToRead = sizeof( cowfile_metadata_header_t );
		size_t readBytes = 0;
		while ( readBytes < sizeToRead ) {
			ssize_t bytes = pread( cow.fhm, ( ( &header ) + readBytes ), sizeToRead, 0 );
			if ( bytes <= 0 ) {
				logadd( LOG_ERROR, "Error while reading meta file header. Bye.\n" );
				return false;
			}
			readBytes += bytes;
		}


		if ( header.magicValue != COW_FILE_META_MAGIC_VALUE ) {
			if ( __builtin_bswap64( header.magicValue ) == COW_FILE_META_MAGIC_VALUE ) {
				logadd( LOG_ERROR, "cow meta file of wrong endianess. Bye.\n" );
				return false;
			}
			logadd( LOG_ERROR, "cow meta file of unkown format. Bye.\n" );
			return false;
		}
		struct stat st;
		stat( pathMeta, &st );
		if ( (long)st.st_size < (long)header.metaDataStart + (long)header.nextL2 * (long)sizeof( l2 ) ) {
			logadd( LOG_ERROR, "cow meta file to small. Bye.\n" );
			return false;
		}
	}
	{
		uint64_t magicValueDataFile;
		if ( pread( cow.fhd, &magicValueDataFile, sizeof( uint64_t ), 0 ) != sizeof( uint64_t ) ) {
			logadd( LOG_ERROR, "Error while reading cow data file, wrong file?. Bye.\n" );
			return false;
		}

		if ( magicValueDataFile != COW_FILE_DATA_MAGIC_VALUE ) {
			if ( __builtin_bswap64( magicValueDataFile ) == COW_FILE_DATA_MAGIC_VALUE ) {
				logadd( LOG_ERROR, "cow data file of wrong endianess. Bye.\n" );
				return false;
			}
			logadd( LOG_ERROR, "cow data file of unkown format. Bye.\n" );
			return false;
		}
		struct stat st;
		stat( pathData, &st );
		if ( (long)header.dataFileSize < st.st_size ) {
			logadd( LOG_ERROR, "cow data file to small. Bye.\n" );
			return false;
		}
	}

	cow.metadata_mmap = mmap( NULL, header.metadataFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, cow.fhm, 0 );

	if ( cow.metadata_mmap == MAP_FAILED ) {
		logadd( LOG_ERROR, "Error while mapping mmap:\n%s \n Bye.\n", strerror( errno ) );
		return false;
	}
	if ( header.version != cowFileVersion ) {
		logadd( LOG_ERROR, "Error wrong file version got: %i expected: 1. Bye.\n", metadata->version );
		return false;
	}


	metadata = (cowfile_metadata_header_t *)( cow.metadata_mmap );

	*imageSizePtr = &metadata->imageSize;
	cow.l1 = (l1 *)( cow.metadata_mmap + metadata->metaDataStart );
	cow.maxImageSize = metadata->maxImageSize;
	cow.l1Size = ( ( cow.maxImageSize + COW_L2_STORAGE_CAPACITY - 1LL ) / COW_L2_STORAGE_CAPACITY );

	cow.firstL2 = (l2 *)( ( (char *)cow.l1 ) + cow.l1Size );
	pthread_mutex_init( &cow.l2CreateLock, NULL );
	createCowStatsFile( path );
	pthread_create( &tidCowUploader, NULL, &cowfile_uploader, NULL );

	if ( statFile || statStdout ) {
		pthread_create( &tidStatUpdater, NULL, &cowfile_statUpdater, NULL );
	}

	return true;
}

/**
 * @brief writes the given data in the data file 
 * 
 * @param buffer containing the data
 * @param size of the buffer
 * @param netSize which actually contributes to the fuse write request (can be different from size if partial full blocks are written)
 * @param cowRequest 
 * @param block 
 * @param inBlockOffset 
 */
static void writeData( const char *buffer, ssize_t size, size_t netSize, cow_request_t *cowRequest,
		cow_block_metadata_t *block, off_t inBlockOffset )
{
	ssize_t totalBytesWritten = 0;
	while ( totalBytesWritten < size ) {
		ssize_t bytesWritten = pwrite( cow.fhd, ( buffer + totalBytesWritten ), size - totalBytesWritten,
				block->offset + inBlockOffset + totalBytesWritten );
		if ( bytesWritten == -1 ) {
			cowRequest->errorCode = errno;
			break;
		} else if ( bytesWritten == 0 ) {
			cowRequest->errorCode = EIO;
			break;
		}
		totalBytesWritten += bytesWritten;
	}
	atomic_fetch_add( &cowRequest->bytesWorkedOn, netSize );
	setBitsInBitfield( block->bitfield, (int)( inBlockOffset / DNBD3_BLOCK_SIZE ),
			(int)( ( inBlockOffset + totalBytesWritten - 1 ) / DNBD3_BLOCK_SIZE ), 1 );

	block->timeChanged = time( NULL );
}

/**
 * @brief Increases the metadata->dataFileSize by COW_METADATA_STORAGE_CAPACITY.
 * The space is not reserved on disk.
 * 
 * @param block for which the space should be reserved.
 */
static bool allocateMetaBlockData( cow_block_metadata_t *block )
{
	block->offset = (atomic_long)atomic_fetch_add( &metadata->dataFileSize, COW_METADATA_STORAGE_CAPACITY );
	return true;
}

/**
 * @brief Get the cow_block_metadata_t from l1Offset and l2Offset
 * 
 * @param l1Offset 
 * @param l2Offset 
 * @return cow_block_metadata_t* 
 */
static cow_block_metadata_t *getBlock( int l1Offset, int l2Offset )
{
	cow_block_metadata_t *block = ( cow.firstL2[cow.l1[l1Offset]] + l2Offset );
	if ( block->offset == -1 ) {
		allocateMetaBlockData( block );
	}
	return block;
}

/**
 * @brief creates an new L2 Block and initializes the containing cow_block_metadata_t blocks
 * 
 * @param l1Offset 
 */
static bool createL2Block( int l1Offset )
{
	pthread_mutex_lock( &cow.l2CreateLock );
	if ( cow.l1[l1Offset] == -1 ) {
		for ( int i = 0; i < COW_L2_SIZE; i++ ) {
			cow.firstL2[metadata->nextL2][i].offset = -1;
			cow.firstL2[metadata->nextL2][i].timeChanged = ATOMIC_VAR_INIT( 0 );
			cow.firstL2[metadata->nextL2][i].uploads = ATOMIC_VAR_INIT( 0 );
			for ( int j = 0; j < COW_BITFIELD_SIZE; j++ ) {
				cow.firstL2[metadata->nextL2][i].bitfield[j] = ATOMIC_VAR_INIT( 0 );
			}
		}
		cow.l1[l1Offset] = metadata->nextL2;
		metadata->nextL2 += 1;
	}
	pthread_mutex_unlock( &cow.l2CreateLock );
	return true;
}

/**
 * @brief Is called once an fuse write request ist finished. 
 * Calls the corrsponding fuse reply depending on the type and
 * success of the request.
 * 
 * @param req fuse_req_t
 * @param cowRequest 
 */

static void finishWriteRequest( fuse_req_t req, cow_request_t *cowRequest )
{
	if ( cowRequest->errorCode != 0 ) {
		fuse_reply_err( req, cowRequest->errorCode );

	} else {
		metadata->imageSize = MAX( metadata->imageSize, cowRequest->bytesWorkedOn + cowRequest->fuseRequestOffset );
		fuse_reply_write( req, cowRequest->bytesWorkedOn );
	}
	free( cowRequest );
}

/**
 * @brief Called after the padding data was received from the dnbd3 server.
 * The data from the write request will be combined witch the data from the server
 * so that we get a full DNBD3_BLOCK and is then written on the disk.
 * @param sRequest 
 */
static void writePaddedBlock( cow_sub_request_t *sRequest )
{
	//copy write Data
	memcpy( ( sRequest->writeBuffer + ( sRequest->inBlockOffset % DNBD3_BLOCK_SIZE ) ), sRequest->writeSrc,
			sRequest->size );
	writeData( sRequest->writeBuffer, DNBD3_BLOCK_SIZE, (ssize_t)sRequest->size, sRequest->cowRequest, sRequest->block,
			( sRequest->inBlockOffset - ( sRequest->inBlockOffset % DNBD3_BLOCK_SIZE ) ) );


	if ( atomic_fetch_sub( &sRequest->cowRequest->workCounter, 1 ) == 1 ) {
		finishWriteRequest( sRequest->dRequest.fuse_req, sRequest->cowRequest );
	}
	free( sRequest );
}

/**
 * @brief If an block does not start or finishes on an multiple of DNBD3_BLOCK_SIZE, the blocks needs to be
 * padded. If this block is inside the original image size, the padding data will be read fro  the server
 * otherwise it will be padded with 0 since the it must be the block at the end of the image.
 * 
 */
static void padBlockFromRemote( fuse_req_t req, off_t offset, cow_request_t *cowRequest, const char *buffer,
		size_t size, cow_block_metadata_t *block, off_t inBlockOffset )
{
	if ( offset > (off_t)metadata->originalImageSize ) {
		//pad 0 and done
		char buf[DNBD3_BLOCK_SIZE] = { 0 };
		memcpy( buf, buffer, size );

		writeData( buf, DNBD3_BLOCK_SIZE, (ssize_t)size, cowRequest, block, inBlockOffset );
		return;
	}
	cow_sub_request_t *sRequest = malloc( sizeof( cow_sub_request_t ) + DNBD3_BLOCK_SIZE );
	sRequest->callback = writePaddedBlock;
	sRequest->inBlockOffset = inBlockOffset;
	sRequest->block = block;
	sRequest->size = size;
	sRequest->writeSrc = buffer;
	sRequest->cowRequest = cowRequest;
	off_t start = offset - ( offset % DNBD3_BLOCK_SIZE );

	sRequest->dRequest.length = DNBD3_BLOCK_SIZE;
	sRequest->dRequest.offset = start;
	sRequest->dRequest.fuse_req = req;
	sRequest->cowRequest = cowRequest;

	if ( ( (size_t)( offset + DNBD3_BLOCK_SIZE ) ) > metadata->originalImageSize ) {
		sRequest->dRequest.length =
				(uint32_t)MIN( DNBD3_BLOCK_SIZE, offset + DNBD3_BLOCK_SIZE - metadata->originalImageSize );
	}

	atomic_fetch_add( &cowRequest->workCounter, 1 );
	if ( !connection_read( &sRequest->dRequest ) ) {
		cowRequest->errorCode = EIO;
		free( sRequest );
		if ( atomic_fetch_sub( &sRequest->cowRequest->workCounter, 1 ) == 1 ) {
			finishWriteRequest( sRequest->dRequest.fuse_req, sRequest->cowRequest );
		}
		return;
	}
}

/**
 * @brief Will be called after a dnbd3_async_t is finished.
 * Calls the corrsponding callback function, either writePaddedBlock or readRemoteData
 * depending if the original fuse request was a write or read.
 * 
 */
void cowfile_handleCallback( dnbd3_async_t *request )
{
	cow_sub_request_t *sRequest = container_of( request, cow_sub_request_t, dRequest );
	sRequest->callback( sRequest );
}


/**
 * @brief called once dnbd3_async_t is finished. Increases bytesWorkedOn by the number of bytes
 * this request had. Also checks if it was the last dnbd3_async_t to finish the fuse request, if
 * so replys to fuse and cleans up the request.
 * 
 */
void readRemoteData( cow_sub_request_t *sRequest )
{
	atomic_fetch_add( &sRequest->cowRequest->bytesWorkedOn, sRequest->dRequest.length );

	if ( atomic_fetch_sub( &sRequest->cowRequest->workCounter, 1 ) == 1 ) {
		fuse_reply_buf(
				sRequest->dRequest.fuse_req, sRequest->cowRequest->readBuffer, sRequest->cowRequest->bytesWorkedOn );
		free( sRequest->cowRequest->readBuffer );
		free( sRequest->cowRequest );
	}
	free( sRequest );
}


/**
 * @brief changes the imageSize
 * 
 * @param req fuse request
 * @param size new size the image should have
 * @param ino fuse_ino_t
 * @param fi fuse_file_info
 */
void cowfile_setSize( fuse_req_t req, size_t size , fuse_ino_t ino, struct fuse_file_info *fi) {
	if( metadata->imageSize < size ) {
		int l1EndOffset  = getL1Offset( size );
		int l2EndOffset = getL2Offset( size );
		int l1Offset = getL1Offset( metadata->imageSize  );
		int l2Offset = getL2Offset( metadata->imageSize  );
		off_t offset = size;
		// imagesize is not on block border
		if ( size %4096 != 0 ) {
			off_t inBlockOffset = (size % 4096);
			size_t sizeToWrite = 4096 - inBlockOffset;
			if ( cow.l1[l1Offset] != -1 ) {
				char buf[sizeToWrite];
				memset( buf, 0, sizeToWrite );
				cow_block_metadata_t * block = getBlock( l1Offset, l2Offset );
				ssize_t bytesWritten = pwrite( cow.fhd, buf, sizeToWrite,
					block->offset + inBlockOffset );

				if( bytesWritten < (ssize_t) sizeToWrite ) {
					fuse_reply_err( req, bytesWritten == -1? errno : EIO);
					return;
				}
				off_t blockOffset = l1Offset * COW_L2_STORAGE_CAPACITY + l2Offset * COW_METADATA_STORAGE_CAPACITY;
				int start = MIN((int)( ( metadata->imageSize - blockOffset ) / DNBD3_BLOCK_SIZE ) , 0 );
				setBitsInBitfield(block->bitfield, start, COW_BITFIELD_SIZE * 8, 0);
				l2Offset++;
				if(l2Offset >= COW_L2_SIZE ){
					l2Offset = 0;
					l1Offset++;
				}
			}
		}

		// null all bitfields

		while( !( l1Offset > l1EndOffset || ( l1Offset == l1EndOffset && l2EndOffset < l2Offset ) ) ) {

			if ( cow.l1[l1Offset] == -1 ) {
				l1Offset++;
				l2Offset = 0;
				continue;
			}
			
			cow_block_metadata_t * block = getBlock( l1Offset, l2Offset );
			setBitsInBitfield(block->bitfield, 0, COW_BITFIELD_SIZE * 8, 0);
			l2Offset++;
			if(l2Offset >= COW_L2_SIZE ){
				l2Offset = 0;
				l1Offset++;
			}
		}
	}

	if( size < metadata->originalImageSize ) {
		metadata->originalImageSize = size;
	}
	
	metadata->imageSize = size;
	image_ll_getattr( req, ino, fi );
}

/**
 * @brief Implementation of a write request or an truncate.
 * 
 * @param req fuse_req_t
 * @param cowRequest 
 * @param offset Offset where the write starts,
 * @param size Size of the write.
 */
void cowfile_write( fuse_req_t req, cow_request_t *cowRequest, off_t offset, size_t size )
{
	// if beyond end of file, pad with 0
	if ( offset > (off_t)metadata->imageSize ) {
		size_t pSize = offset - metadata->imageSize;
		// half end block will be padded with original write
		pSize = pSize - ( ( pSize + offset ) % DNBD3_BLOCK_SIZE );
		atomic_fetch_add( &cowRequest->workCounter, 1 );
		cowfile_write( req, cowRequest, metadata->imageSize, pSize );
	}


	off_t currentOffset = offset;
	off_t endOffset = offset + size;

	// write data

	int l1Offset = getL1Offset( currentOffset );
	int l2Offset = getL2Offset( currentOffset );
	while ( currentOffset < endOffset ) {
		if ( cow.l1[l1Offset] == -1 ) {
			createL2Block( l1Offset );
		}
		//loop over L2 array (metadata)
		while ( currentOffset < (off_t)endOffset && l2Offset < COW_L2_SIZE ) {
			cow_block_metadata_t *metaBlock = getBlock( l1Offset, l2Offset );


			size_t metaBlockStartOffset = l1Offset * COW_L2_STORAGE_CAPACITY + l2Offset * COW_METADATA_STORAGE_CAPACITY;

			size_t inBlockOffset = currentOffset - metaBlockStartOffset;
			size_t sizeToWriteToBlock =
					MIN( (size_t)( endOffset - currentOffset ), COW_METADATA_STORAGE_CAPACITY - inBlockOffset );


			/////////////////////////
			// lock for the half block probably needed
			if ( currentOffset % DNBD3_BLOCK_SIZE != 0
					&& !checkBit( metaBlock->bitfield, (int)( inBlockOffset / DNBD3_BLOCK_SIZE ) ) ) {
				// write remote
				size_t padSize = MIN( sizeToWriteToBlock, DNBD3_BLOCK_SIZE - ( (size_t)currentOffset % DNBD3_BLOCK_SIZE ) );
				const char *sbuf = cowRequest->writeBuffer + ( ( currentOffset - offset ) );
				padBlockFromRemote( req, offset, cowRequest, sbuf, padSize, metaBlock, (off_t)inBlockOffset );
				currentOffset += padSize;
				continue;
			}

			size_t endPaddedSize = 0;
			if ( ( currentOffset + sizeToWriteToBlock ) % DNBD3_BLOCK_SIZE != 0 ) {
				off_t currentEndOffset = currentOffset + sizeToWriteToBlock;
				off_t padStartOffset = currentEndOffset - ( currentEndOffset % 4096 );
				off_t inBlockPadStartOffset = padStartOffset - metaBlockStartOffset;
				if ( !checkBit( metaBlock->bitfield, (int)( inBlockPadStartOffset / DNBD3_BLOCK_SIZE ) ) ) {
					const char *sbuf = cowRequest->writeBuffer + ( ( padStartOffset - offset ) );
					padBlockFromRemote( req, padStartOffset, cowRequest, sbuf, (currentEndOffset)-padStartOffset, metaBlock,
							inBlockPadStartOffset );


					sizeToWriteToBlock -= (currentEndOffset)-padStartOffset;
					endPaddedSize = (currentEndOffset)-padStartOffset;
				}
			}


			writeData( cowRequest->writeBuffer + ( ( currentOffset - offset )  ),
					(ssize_t)sizeToWriteToBlock, sizeToWriteToBlock, cowRequest, metaBlock, inBlockOffset );

			currentOffset += sizeToWriteToBlock;
			currentOffset += endPaddedSize;


			l2Offset++;
		}
		l1Offset++;
		l2Offset = 0;
	}
	if ( atomic_fetch_sub( &cowRequest->workCounter, 1 ) == 1 ) {
		finishWriteRequest( req, cowRequest );
	}
}


/**
 * @brief Request data, that is not available locally, via the network.
 * 
 * @param req fuse_req_t 
 * @param offset from the start of the file
 * @param size of data to request
 * @param buffer into which the data is to be written
 * @param workCounter workCounter is increased by one and later reduced by one again when the request is completed.
 */
static void readRemote( fuse_req_t req, off_t offset, ssize_t size, char *buffer, cow_request_t *cowRequest )
{
	cow_sub_request_t *sRequest = malloc( sizeof( cow_sub_request_t ) );
	sRequest->callback = readRemoteData;
	sRequest->dRequest.length = (uint32_t)size;
	sRequest->dRequest.offset = offset;
	sRequest->dRequest.fuse_req = req;
	sRequest->cowRequest = cowRequest;
	sRequest->buffer = buffer;

	atomic_fetch_add( &cowRequest->workCounter, 1 );
	if ( !connection_read( &sRequest->dRequest ) ) {
		cowRequest->errorCode = EIO;
		free( sRequest );
		if ( atomic_fetch_sub( &cowRequest->workCounter, 1 ) == 1 ) {
			fuse_reply_buf( req, cowRequest->readBuffer, cowRequest->bytesWorkedOn );
		}
		free( cowRequest->readBuffer );
		free( cowRequest );
		return;
	}
}

/**
 * @brief Get the Block Data Source object
 * 
 * @param block 
 * @param bitfieldOffset 
 * @param offset 
 * @return enum dataSource 
 */
enum dataSource getBlockDataSource( cow_block_metadata_t * block , off_t bitfieldOffset, off_t offset ) {

	if( block != NULL && checkBit( block->bitfield, bitfieldOffset ) ) {
		return local;
	}
	if( offset >= metadata->originalImageSize ) {
		return zero;
	}
	return remote;
}

/**
 * @brief Reads data at given offset. If the data are available locally,
 * they are read locally, otherwise they are requested remotely.
 * 
 * @param req fuse_req_t
 * @param size of date to read
 * @param offset offset where the read starts.
 * @return uint64_t Number of bytes read.
 */
void cowfile_read( fuse_req_t req, size_t size, off_t offset )
{
	cow_request_t *cowRequest = malloc( sizeof( cow_request_t ) );
	cowRequest->fuseRequestSize = size;
	cowRequest->bytesWorkedOn = ATOMIC_VAR_INIT( 0 );
	cowRequest->workCounter = ATOMIC_VAR_INIT( 1 );
	cowRequest->errorCode = ATOMIC_VAR_INIT( 0 );
	cowRequest->readBuffer = malloc( size );
	cowRequest->fuseRequestOffset = offset;
	off_t lastReadOffset = offset;
	off_t endOffset = offset + size;
	off_t searchOffset = offset;
	int l1Offset = getL1Offset( offset );
	int l2Offset = getL2Offset( offset );
	int bitfieldOffset = getBitfieldOffset( offset );
	enum dataSource dataState;
	cow_block_metadata_t *block = NULL;

	if ( cow.l1[l1Offset] != -1 ) {
		block = getBlock( l1Offset, l2Offset );
	}

	bool doRead = false;
	bool firstLoop = true;
	bool updateBlock = false;
	while ( searchOffset < endOffset ) {
		if ( firstLoop ) {
			firstLoop = false;
			lastReadOffset = searchOffset;
			dataState = getBlockDataSource( block , bitfieldOffset, searchOffset );
		} else if ( getBlockDataSource( block , bitfieldOffset, searchOffset ) != dataState ) {
			doRead = true;
		} else {
			bitfieldOffset++;
		}

		if ( bitfieldOffset >= COW_BITFIELD_SIZE * 8 ) {
			bitfieldOffset = 0;
			l2Offset++;
			if ( l2Offset >= COW_L2_SIZE ) {
				l2Offset = 0;
				l1Offset++;
			}
			updateBlock = true;
			if ( dataState == local ) {
				doRead = true;
			}
		}
		// compute the original file offset from bitfieldOffset, l2Offset and l1Offset
		searchOffset = DNBD3_BLOCK_SIZE * ( bitfieldOffset ) + l2Offset * COW_METADATA_STORAGE_CAPACITY
				+ l1Offset * COW_L2_STORAGE_CAPACITY;
		if ( doRead || searchOffset >= endOffset ) {
			ssize_t sizeToRead = MIN( searchOffset, endOffset ) - lastReadOffset;
			if ( dataState == remote) {
				readRemote(
						req, lastReadOffset, sizeToRead, cowRequest->readBuffer + ( lastReadOffset - offset ), cowRequest );
			}else if( dataState == zero) {
				memset( cowRequest->readBuffer + ( lastReadOffset - offset ), 0 , sizeToRead );
				atomic_fetch_add( &cowRequest->bytesWorkedOn, sizeToRead );
			} else {
				// Compute the offset in the data file where the read starts
				off_t localRead =
						block->offset + ( ( lastReadOffset % COW_L2_STORAGE_CAPACITY ) % COW_METADATA_STORAGE_CAPACITY );
				ssize_t totalBytesRead = 0;
				while ( totalBytesRead < sizeToRead ) {
					ssize_t bytesRead =
							pread( cow.fhd, cowRequest->readBuffer + ( lastReadOffset - offset ), sizeToRead, localRead );
					if ( bytesRead == -1 ) {
						cowRequest->errorCode = errno;
						goto fail;
					} else if ( bytesRead <= 0 ) {
						cowRequest->errorCode = EIO;
						goto fail;
					}
					totalBytesRead += bytesRead;
				}

				atomic_fetch_add( &cowRequest->bytesWorkedOn, totalBytesRead );
			}
			lastReadOffset = searchOffset;
			doRead = false;
			firstLoop = true;
		}

		if ( updateBlock ) {
			if ( cow.l1[l1Offset] != -1 ) {
				block = getBlock( l1Offset, l2Offset );
			} else {
				block = NULL;
			}
			updateBlock = false;
		}
	}
fail:;
	if ( atomic_fetch_sub( &cowRequest->workCounter, 1 ) == 1 ) {
		if ( cowRequest->errorCode != 0 ) {
			fuse_reply_err( req, cowRequest->errorCode );

		} else {
			fuse_reply_buf( req, cowRequest->readBuffer, cowRequest->bytesWorkedOn );
		}
		free( cowRequest->readBuffer );
		free( cowRequest );
	}
}


/**
 * @brief stops the StatUpdater and CowUploader threads
 * and waits for them to finish, then cleans up curl.
 * 
 */
void cowfile_close()
{
	uploadLoop = false;
	if ( statFile || statStdout ) {
		pthread_join( tidStatUpdater, NULL );
	}
	pthread_join( tidCowUploader, NULL );

	if ( curl ) {
		curl_global_cleanup();
		curl_easy_cleanup( curl );
	}
}
