//******************************************************************************************************************
//  File           : tagline_driver.c
//  Description    : This is the implementation of the driver interface
//                   between the OS and the low-level hardware.
//
//  Author         : Raquel Alvarez
//  Created        : 9/25/2015
//
//  Assumptions:
//	Version 1.0								Version 1.1:			
//	- We R/W one block at a time, more than one is not supported yet	--> multiple blocks supported
//	- We store map tagline blocks to RAID blocks as requests come: LINEAR	--> no changes
//	- We create a linked list of tags during init, which is 1 here		--> there are maxlines taglines now
//	- Blocks in a tagline are the same size as blocks in a RAID disk	--> no changes
//
//******************************************************************************************************************

// Include Files
#include <stdlib.h>
#include <string.h>
#include <cmpsc311_log.h>

// Project Includes
#include "raid_bus.h"
#include "tagline_driver.h"

// Alias
typedef char bitfield;

//-----------  Declaration of Structures -------------
// RAID bus opcode definition
typedef struct {
	uint8_t		request_type;		// 8 bits
	uint8_t		number_of_blocks;	// 8 bits
	RAIDDiskID 	disk_number;		// 8 bits
	uint8_t		reserved;		// 6 bits
	uint8_t		status;			// 1 bit
	RAIDBlockID	blockid;		// 31 bits
} RAID_REQUEST, RAID_RESPONSE;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Definition of a block node for a linked-list of blocks
typedef struct block_node {
	TagLineBlockNumber 	block;			// block j
	RAIDDiskID 		RAID_disk;		// RAID disk where this block is mapped to
	RAIDBlockID 		RAID_block;		// RAID block where this block is mapped to
	RAIDDiskID		backup_disk;		// RAID disk where the BACKUP is mapped to
	RAIDBlockID		backup_block;		// RAID block where the BACKUP is mapped to
	struct block_node	*next_block;		// pointer to block j+1
} BLOCK;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Definition of a tagline node for a linked-list of taglines
typedef struct tag_node {
	TagLineNumber 		tagline;		// tagline i
	TagLineBlockNumber 	max_start_allowed;	// last block + 1, since we can't allocate a new block after that
	uint64_t   		blocks_in_tagline;	// number of blocks in this tagline
	struct tag_node 	*next_tagline;		// pointer to tagline i+1
	BLOCK 		 	*block_0;		// pointer to block 0 (of tagline i)
} TAGLINE;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Definition of a scheduled block in RAID (used to schedule a spot on the RAID array,
// and then to update the structres once the WRITE/READ is successful).
typedef struct {
	RAIDDiskID	disk;				// RAID disk number
	RAIDBlockID	block;				// RAID block number for the disk specified above
} SCHEDULED_BLOCK, BLOCK_TO_READ;			// SCHEDULED_BLOCK: for WRITEs, BLOCK_TO_READ: for READs
// ---------------------------------------------------


// --------- Declaration of static variables ---------
// Array of integers that contains, for each disk, the block last allocated
static int 		last_block_added[RAID_DISKS] = { [0 ... RAID_DISKS-1] = -1 };	// intialize to -1 (since blocks are 0 indexed)
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Head node of the linked-list of taglines
static TAGLINE 		*head_tag = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Stores the number of taglines currently in use
static uint32_t 	taglines_in_use = 0;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static RAID_REQUEST 	*init = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init response
static RAID_RESPONSE 	*init_response = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for a format request
static RAID_REQUEST	*format = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for a format response
static RAID_RESPONSE	*format_response = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for a read request
static RAID_REQUEST	*read = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for a read response
static RAID_RESPONSE	*read_response = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static RAID_REQUEST	*close = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static RAID_RESPONSE	*close_response = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static RAID_REQUEST	*status = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static RAID_RESPONSE	*status_response = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static char		*temp_buf = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static char		*buf_read = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static SCHEDULED_BLOCK	*new_scheduled_block = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Pointer to a container for an init request
static SCHEDULED_BLOCK	*new_scheduled_block_backup = NULL;
// - - - - - - - - - - - - - - - - - - - - - - - - - -
// Variables to schedule blocks
static RAIDDiskID current_disk = 0;
static RAIDBlockID current_block = 0;
// ---------------------------------------------------


// ------------------ Function Prototypes ------------------
void 	free_tag_linked_list	(TAGLINE *ptr_tag);
void 	free_block_linked_list	(BLOCK *ptr_block); 
int 	add_new_tag		();
int 	RAID_scheduler		(SCHEDULED_BLOCK *ptr4, RAIDDiskID);
void 	decode_RAIDOpCode 	(RAIDOpCode, RAID_RESPONSE *ptr3);
int 	check_response 		(RAID_REQUEST *ptr0, RAID_RESPONSE *ptr1); 
RAIDOpCode 	   generate_RAIDOpCode 		(RAID_REQUEST *ptr2); 
TagLineBlockNumber get_max_start_allowed	(TagLineNumber); 
// ---------------------------------------------------------


// --------------------------       FUNCTIONS       -----------------------------
// Function	: raid_disk_signal
// Description	: check for the failing disk and repair it
//
// Input	: N/A
// Output	: 0 if successful, -1 if failure
int raid_disk_signal(void) {
	RAIDOpCode	status_opcode = 0;
	RAIDOpCode	status_opcode_response = 0;

	RAIDOpCode	format_opcode = 0;
	RAIDOpCode	format_opcode_response = 0;

	RAIDOpCode	read_opcode = 0;
	RAIDOpCode	read_opcode_response = 0;

	TAGLINE	*current_tag = NULL;
	BLOCK 	*current_block = NULL;	

	RAID_REQUEST_TYPES request_type_status = RAID_STATUS;
	RAID_REQUEST_TYPES request_type_format = RAID_FORMAT;
	RAID_REQUEST_TYPES request_type_read = RAID_READ;

	char *buf = NULL;
	buf_read = malloc(sizeof(char)*RAID_BLOCK_SIZE);
	RAIDDiskID disk = 0;

	read->request_type = request_type_read;
	read->number_of_blocks = 1;
	read->status = 0;
	read->reserved = 0;

	// setup opcode for status
	status->request_type = request_type_status;
	status->number_of_blocks = 0;
	status->status = 0;
	status->reserved = 0;
	status->blockid = 0;

	// setup opcode for format
	format->request_type = request_type_format;
	format->number_of_blocks = 0;
	format->status = 0;
	format->reserved = 0;
	format->blockid = 0;

	// loop through all disks
	for (disk = 0; disk < RAID_DISKS; disk++) {
		// request disk status
		buf = NULL;
		status->disk_number = disk;
		status_opcode = generate_RAIDOpCode(status);
		status_opcode_response = client_raid_bus_request(status_opcode, buf);
		decode_RAIDOpCode(status_opcode_response, status_response);
		if (status_response->status != 0) {
			logMessage(LOG_INFO_LEVEL, "STATUS REQUEST FAILED!!!!");
			return(-1);
		}
		// check if failed
		if (status_response->blockid == 2) {
			// 1- format the disk

			buf = NULL;
			format->disk_number = disk;
			format_opcode = generate_RAIDOpCode(format);
			format_opcode_response = client_raid_bus_request(format_opcode, buf);
			decode_RAIDOpCode(format_opcode_response, format_response);
			if (format_response->status != 0) {
				logMessage(LOG_INFO_LEVEL, "FORMATTING for FAILING DISK FAILED");
				return(-1);	
			}
			// 2- loop through tags and find every single backup/primary corresponding to that disk
			current_tag = head_tag;
			while (current_tag != NULL) {
				current_block = current_tag->block_0;
				while (current_block != NULL) {
					if (current_block->RAID_disk == disk) {
						// primary block lost, recover`
						// read backup_disk,backup_block into buf
						buf = NULL;
						// Check Cache
						buf = get_raid_cache(current_block->backup_disk, current_block->backup_block);
						if (buf == NULL) {
							logMessage(LOG_INFO_LEVEL, "Primary Block not in Cache, recovery from READ");
							// Perform a read from the disk
							read->disk_number = current_block->backup_disk;
							read->blockid = current_block->backup_block;
							read_opcode = generate_RAIDOpCode(read);
							logMessage(LOG_INFO_LEVEL, "Request Type: %d", read_opcode >> 56);
							read_opcode_response = client_raid_bus_request(read_opcode, buf_read);
							decode_RAIDOpCode(read_opcode_response, read_response);
							if (read_response->status != 0) {
								logMessage(LOG_INFO_LEVEL, "READ of RECOVERY BLOCK FAILED at primary fail");
								return(-1);
							}
							//Update cache
							if (put_raid_cache(current_block->RAID_disk, current_block->RAID_block, buf_read) != 0) {
								logMessage(LOG_ERROR_LEVEL, "Error adding backup block from recovery of primary to cache");
								return(-1);
							}
						}
						else {
 
							memcpy(buf_read, buf, RAID_BLOCK_SIZE);

							// Update Cache
							if (put_raid_cache(current_block->RAID_disk, current_block->RAID_block, buf_read) != 0) {
								logMessage(LOG_INFO_LEVEL, "RECOVER WRITE of BLOCK FAILED at primary fail");
								return(-1);
							}		
						}
					}
					else if (current_block->backup_disk == disk) {
						// backup block lost, recover
						// read primary_disk,primary_block into buf
						buf = NULL;
											
						// Check Cache
						buf = get_raid_cache(current_block->RAID_disk, current_block->RAID_block);
						if (buf == NULL) {
							logMessage(LOG_INFO_LEVEL, "Backup Block to recover, primary not in cache");
							read->disk_number = current_block->RAID_disk;
							read->blockid = current_block->RAID_block;
							read_opcode = generate_RAIDOpCode(read);
							read_opcode_response = client_raid_bus_request(read_opcode, buf_read);
							decode_RAIDOpCode(read_opcode_response, read_response);
							if (read_response->status != 0) {
								logMessage(LOG_ERROR_LEVEL, "Error reading primary from disk for backup recovery");
								return(-1);
							}
							if(put_raid_cache(current_block->backup_disk, current_block->backup_block, buf_read) != 0) {
								logMessage(LOG_ERROR_LEVEL, "Error adding updated backup to cache");
								return(-1);
							}
						}
						else {

							// write buf into backup_disk,backup_block 
							// Update Cache
							if (put_raid_cache(current_block->backup_disk, current_block->backup_block, buf) != 0) {
								logMessage(LOG_INFO_LEVEL, "RECOVER WRITE of BLOCK FAILED at backup fail");
								return(-1);
							}
						}
					}
					else {
						// the block is okay!
					}		
					current_block = current_block->next_block;
				}
				current_tag = current_tag->next_tagline;
			}
		}
	}
	return(0);
}

// Function     : tagline_driver_init
// Description  : Initialize the driver with a number of maximum lines to process
//
// Inputs       : maxlines - the maximum number of tag lines in the system
// Outputs      : 0 if successful, -1 if failure
int tagline_driver_init(uint32_t maxlines) {

	// declaration of variables needed
	uint64_t total_number_of_blocks = 0;			// total number of blocks to initialize
	uint64_t total_number_of_disks = 0;			// total number of disks to initialize
	uint64_t total_number_of_tracks = 0;			// total number of tracks to initialize
	char *buf = NULL;					// pointer to location with an int, to pass as an arg
	RAIDDiskID disk = 0;					// counter variable to loop through the disks to format them
	int tag = 0;				// counters to initialize RAID_bitmap_array (d,b) and add taglines to the list (tag)
	RAID_REQUEST_TYPES request_type_init = RAID_INIT;	// variables that store the type of request: init and format
	RAID_REQUEST_TYPES request_type_format = RAID_FORMAT;
	// RAID_INIT setup variables:
	RAIDOpCode 	init_opcode = 0;					// 64-bit uint to store RAID_INIT bits together defined by the fields in the struct above
	RAIDOpCode 	init_opcode_response = 0;				// stores the response after the bus processed the request sent through init_opcode
	// RAID_FORMAT setup variables:
	RAIDOpCode 	format_opcode = 0;					// 64-bit uint to store RAID_FORMAT complete opcode
	RAIDOpCode	format_opcode_response = 0;				// stores the response after the bus processed the request sent through format_opcode
	// Tag node for the linked-list of tags
	TAGLINE		*new_tag = malloc(sizeof(TAGLINE));

	init = malloc(sizeof(RAID_REQUEST));
	init_response = malloc(sizeof(RAID_RESPONSE));
	format = malloc(sizeof(RAID_REQUEST));
	format_response = malloc(sizeof(RAID_RESPONSE));
	read = malloc(sizeof(RAID_REQUEST));
	read_response = malloc(sizeof(RAID_RESPONSE));
	//write = malloc(sizeof(RAID_REQUEST));
	//write_response = malloc(sizeof(RAID_RESPONSE));
	close = malloc(sizeof(RAID_REQUEST));
	close_response = malloc(sizeof(RAID_RESPONSE));
	status = malloc(sizeof(RAID_REQUEST));
	status_response = malloc(sizeof(RAID_RESPONSE));
	//temp_buf = malloc(sizeof(char)*RAID_BLOCK_SIZE);
	buf_read = malloc(sizeof(char)*RAID_BLOCK_SIZE);
	new_scheduled_block = malloc(sizeof(SCHEDULED_BLOCK));
	new_scheduled_block_backup = malloc(sizeof(SCHEDULED_BLOCK));


	// Initialize Cache
	if( init_raid_cache(TAGLINE_CACHE_SIZE) != 0 ) {
		logMessage(LOG_INFO_LEVEL, "Error initializing cache");
		return(-1);
	}

	// check status of malloc
	if (((init == NULL) | (init_response == NULL)) | ((format == NULL) | (format_response == NULL))) {
		logMessage(LOG_INFO_LEVEL, "MALLOC IS NULL FOR INIT or FORMAT! \n");
		return(-1);
	}
	if (((read == NULL) | (read_response == NULL)) ){  /*((write == NULL) | (write_response == NULL))*/
		logMessage(LOG_INFO_LEVEL, "Malloc is NULL for read or write! \n");
		return(-1);
	}
	if (((close == NULL) | (close_response == NULL)) | ((status == NULL) | (status_response == NULL))) {
		logMessage(LOG_INFO_LEVEL, "Malloc is NULL for close or status! \n");
		return(-1);
	}
	if (buf_read == NULL) {
		logMessage(LOG_INFO_LEVEL, "MALLOC IS NULL FOR buf_read!");
		return(-1);
	}
	if (new_tag == NULL) {
		logMessage(LOG_INFO_LEVEL, "MALLOC IS NULL FOR NEW TAG!");
		return(-1);
	}

// 1. Calculate the number of tracks to create: 
	total_number_of_blocks = RAID_DISKS * RAID_DISKBLOCKS;
	total_number_of_tracks = total_number_of_blocks / RAID_TRACK_BLOCKS;
// 2. Calculate the number of disks to create:
	total_number_of_disks = RAID_DISKS;
// 3. Call RAID_INIT:
	// Initialize init with values for each field
	init->request_type = request_type_init;					
	init->number_of_blocks = total_number_of_tracks;	// we send tracks because the number will fit in 8 bits 
	init->disk_number = total_number_of_disks;
	init->reserved = 0;
	init->status = 0;
	init->blockid = 0;
	buf = NULL; 						// NULL because we don't have to reference it
	// Generate opcode
	init_opcode = generate_RAIDOpCode(init);		// generates opcode based on fields of the (pointer to) structure passed
	// Call raid_bus function to request RAID_INIT:
	init_opcode_response = client_raid_bus_request(init_opcode,buf);	
	// Decode the fields of the repsonse from above:
	decode_RAIDOpCode(init_opcode_response,init_response);
	// If the initialization fails, then error out
	if (check_response(init, init_response) != 0) {
		logMessage(LOG_INFO_LEVEL, "Driver initialization FAILED at RAID_INIT.");
		return(-1);
	}
// 4. Format the disks initialized:
	format->request_type = request_type_format;
	format->number_of_blocks = 0;
	format->reserved = 0;
	format->status = 0;
	format->blockid = 0;
	buf = NULL;
	// Format every disk allocated by RAID_INIT
	for (disk = 0; disk < total_number_of_disks; disk++) {
		format->disk_number = disk;			
		// Generate RAID_FORMAT opcode
		format_opcode = generate_RAIDOpCode(format);
		// Request the command generated
		format_opcode_response = client_raid_bus_request(format_opcode, buf);
		// Decode the response
		decode_RAIDOpCode(format_opcode_response, format_response);
		// Check that process was successful			
		if (check_response(format, format_response) != 0) {
			logMessage(LOG_INFO_LEVEL, "Driver initialization failed at RAID_FORMAT of disk %d." , disk);
			return(-1);
		}
	}

// 6. Setup linked-list for "maxlines" tags:
	for (tag = 0; tag < maxlines; tag++) {
		if (add_new_tag() != 0) {
			logMessage(LOG_INFO_LEVEL, "TAGLINE: intialized not complete, malloc fails when adding a new tag");
			return(-1);	
		}
	}
// 7. Free pointers
		// Return successfully
	logMessage(LOG_INFO_LEVEL, "TAGLINE: initialized storage (maxline=%u)", maxlines);
	return(0);
}
// ----------------------------------------------------------------------------------------------------


// Function     : tagline_read
// Description  : Read a number of blocks from the tagline driver
//
// Inputs       : tag - the number of the tagline to read from
//                bnum - the starting block to read from
//                blks - the number of blocks to read
//                bug - memory block to read the blocks into
// Outputs      : 0 if successful, -1 if failure
int tagline_read(TagLineNumber tag, TagLineBlockNumber bnum, uint8_t blks, char *buf) {

	// declaration of variables needed
	RAIDOpCode read_opcode;
	RAIDOpCode read_opcode_response;
	
	RAID_REQUEST_TYPES request_type_read = RAID_READ;

	TAGLINE *current_tag = NULL;
	BLOCK *current_block = NULL;

	char *fixbuf = NULL;

	//logMessage(LOG_INFO_LEVEL, "Reading Tag %d and Block %d", tag, bnum);


	// variables to handle more than one block
	TagLineBlockNumber block = 0;	

	// more than 1 block?
	if (blks > 1) {
		for (block = 0; block < blks; block++) {
			temp_buf = buf+(RAID_BLOCK_SIZE*block);
			if (tagline_read(tag,bnum+block,1,temp_buf) != 0) {
				logMessage(LOG_INFO_LEVEL, "ERROR: Failure in reading block number %d",block);
				return(-1);
			}
		}
			return(0);
	}
	// does the tag exist?
	if (tag >= taglines_in_use) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Attempt to read from a non-existent tagline");
		return(-1);
	}
	// is starting block valid?
	if (bnum >= get_max_start_allowed(tag)) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Attemp to read an unallocated block");
		return(-1);
	}
	// find the block that we want to read and retrieve RAID disk and block
	current_tag = head_tag;
	while (current_tag->tagline != tag) {
		current_tag = current_tag->next_tagline;
	}
	current_block = current_tag->block_0;
	while (current_block->block != bnum) {
		current_block = current_block->next_block;
	}

	// ----  Check Cache ----
//	logMessage(LOG_INFO_LEVEL, "disk: %d block: %d to read from cache", current_block->RAID_disk, current_block->RAID_block);
	fixbuf = get_raid_cache(current_block->RAID_disk, current_block->RAID_block);
//	logMessage(LOG_INFO_LEVEL, "Driver buf = %c", *buf);
	
	if (fixbuf == NULL) {
		logMessage(LOG_INFO_LEVEL, "Block not found in cache.");
		// block not found in cache
		// save the disk and block in the RAID storage to call RAID_READ
		read->blockid = current_block->RAID_block;
		read->disk_number = current_block->RAID_disk;
		read->request_type = request_type_read;
		read->number_of_blocks = 1;
		read->reserved = 0;
		read->status = 0;
		//  generate opcode
		read_opcode = generate_RAIDOpCode(read);
		// call RAID_READ
		read_opcode_response = client_raid_bus_request(read_opcode, buf);
		// decode read response 
		decode_RAIDOpCode(read_opcode_response, read_response);
		// check if read was successful
		if (read_response->status != 0) {
			logMessage(LOG_INFO_LEVEL, "Error reading from disk");
			return(-1);
		}
		
		logMessage(LOG_INFO_LEVEL, "Sending to cache Disk %d and Block %d", current_block->RAID_disk, current_block->RAID_block);

		if ( put_raid_cache(current_block->RAID_disk, current_block->RAID_block, buf) != 0 ) {
			logMessage(LOG_INFO_LEVEL, "Error adding block to cache");
		}
	} 
	else {
		// block found in cache
		logMessage(LOG_INFO_LEVEL, "Block found in cache");
		memcpy(buf, fixbuf, RAID_BLOCK_SIZE);
	}
	// -----------------------
	// Return successfully
	logMessage(LOG_INFO_LEVEL, "TAGLINE : read %u blocks from tagline %u, starting block %u.",
			blks, tag, bnum);
	return(0);
}
// ----------------------------------------------------------------------------------------------------------



// Function     : tagline_write
// Description  : Write a number of blocks from the tagline driver
//
// Inputs       : tag - the number of the tagline to write from
//                bnum - the starting block to write from
//                blks - the number of blocks to write
//                bug - the place to write the blocks into
// Outputs      : 0 if successful, -1 if failure
int tagline_write(TagLineNumber tag, TagLineBlockNumber bnum, uint8_t blks, char *buf) {

	//variables
	
	TagLineBlockNumber max_start = 0;

	TAGLINE	*current_tag = NULL;
	BLOCK *current_block = NULL;
	BLOCK *new_block = NULL;
	new_block = malloc(sizeof(BLOCK));

	// variables for handling more than one block
	TagLineBlockNumber block = 0;

	// Valid number of blocks requested?
	if (blks > 1) {
		for (block = 0; block < blks; block++) {
			temp_buf = buf+(RAID_BLOCK_SIZE*block);
			if (tagline_write(tag,bnum+block,1,temp_buf) != 0) {
				logMessage(LOG_INFO_LEVEL, "ERROR: Block number %d was not written properly.", block);
				return(-1);
			}
		}
		// return successfully
		logMessage(LOG_INFO_LEVEL, "TAGLINE : wrote %u blocks to tagline %u, starting block %u.",blks, tag, bnum);	
		return(0);
	}
	// Does the tag exist?
	if (tag >= taglines_in_use) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Attempting to write to a nonexisting tagline");
	}
	// Does the starting block make sense?
	max_start = get_max_start_allowed(tag);
	if (bnum > max_start) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Attemping to write beyond allowed start");
	}
	// Are we writing a new block or overwriting an old one?
	// New Block:
	if (bnum == max_start) {
		//new = 1;
	// --- PRIMARY BLOCK ---
	     // look for a disk and block in the RAID array for the new block
		if (RAID_scheduler(new_scheduled_block,-1) != 0) {
			logMessage(LOG_INFO_LEVEL, "ERROR: finding a disk and block in RAID to store new block");
			return(-1);
		}
		// store the disk and block scheduled for this tag and block
		// Update Cache		
		if (put_raid_cache(new_scheduled_block->disk, new_scheduled_block->block, buf) != 0) {
			logMessage(LOG_INFO_LEVEL, "ERROR : WRITE UNSUCCESSFUL");
			return(-1);
		}

	     //UPDATE Structures
		// update last_block_added[disk]
		last_block_added[new_scheduled_block->disk]++;
		// find the last block in the tag
		current_tag = head_tag;
		while(current_tag->tagline != tag) {
			current_tag = current_tag->next_tagline;
		}
		// add 1 to the block count for that tag
		current_tag->blocks_in_tagline = current_tag->blocks_in_tagline + 1;
		current_tag->max_start_allowed = current_tag->max_start_allowed + 1;
		// find the last block and append the new one to it
		current_block = current_tag->block_0;
		// check if block_0 is empty first:
		if (current_block == NULL) {
			new_block->block = bnum;
			new_block->RAID_disk = new_scheduled_block->disk;
			new_block->RAID_block = new_scheduled_block->block;
			new_block->next_block = NULL;
			current_block = new_block;
			current_tag->block_0 = current_block;	
		// if it's not empty, find the last block and append the new one to it
		} else {
			while (current_block->next_block != NULL) {
				current_block = current_block->next_block;
			}
			new_block->block = bnum;
			new_block->RAID_disk = new_scheduled_block->disk;
			new_block->RAID_block = new_scheduled_block->block;
			new_block->next_block = NULL;
			current_block->next_block = new_block;
		}
	// --- BACKUP BLOCK ---
	     // look for a disk and block in the RAID array for the new block
		if (RAID_scheduler(new_scheduled_block_backup, new_scheduled_block->disk) != 0) {
			logMessage(LOG_INFO_LEVEL, "ERROR: finding a disk and block in RAID to store new block");
			return(-1);
		}
		
		// store the disk and block scheduled for this tag and block
		// Update Cache
		if (put_raid_cache(new_scheduled_block_backup->disk, new_scheduled_block_backup->block, buf) != 0) {
			logMessage(LOG_INFO_LEVEL, "ERROR : WRITE UNSUCCESSFUL");
			return(-1);
		}

	     //UPDATE Structures
		// update last_block_added[disk]
		last_block_added[new_scheduled_block_backup->disk]++;
		// find the last block in the tag
		current_tag = head_tag;
		while(current_tag->tagline != tag) {
			current_tag = current_tag->next_tagline;
		}
		// find the corresponding block and add the backup information
		current_block = current_tag->block_0;
		while (current_block->block != new_block->block) {
			current_block = current_block->next_block;
		}
		new_block->backup_disk = new_scheduled_block_backup->disk;
		new_block->backup_block = new_scheduled_block_backup->block;
	}
	// Old Block:
	if (bnum < max_start) {
	// --- PRIMARY BLOCK ---
		current_tag = head_tag;
		// loop through the tags and find the one we need
		while (current_tag->tagline != tag) {
			current_tag = current_tag->next_tagline;
		}
		current_block = current_tag->block_0;
		// loop through the blocks and find the one we need
		while (current_block->block != bnum) {
			current_block = current_block->next_block;
		}
		
		// store the disk and block for the tag and block we want to modify
		// Update Cache
		if (put_raid_cache(current_block->RAID_disk, current_block->RAID_block, buf) != 0) {
			logMessage(LOG_INFO_LEVEL, "ERROR : WRITE UNSUCCESSFUL");
			return(-1);
		}


	// --- BACKUP BLOCK ---
		// Update Cache
		if (put_raid_cache(current_block->backup_disk, current_block->backup_block, buf)) {
			logMessage(LOG_INFO_LEVEL, "ERROR : WRITE UNSUCCESSFUL");
			return(-1);
		}	
	}
	
	// Return successfully
	logMessage(LOG_INFO_LEVEL, "TAGLINE : wrote %u blocks to tagline %u, starting block %u.",
			blks, tag, bnum);
	return(0);
}
// ----------------------------------------------------------------------------------------------------------


////////////////////////////////////////////////////////////////////////////////
// Function     : tagline_close
// Description  : Close the tagline interface
//
// Inputs       : none
// Outputs      : 0 if successful, -1 if failure
int tagline_close(void) {
	
	// variables
	void *buf = NULL;
	// RAID_CLOSE setup
	RAIDOpCode close_opcode;
	RAIDOpCode close_opcode_response;
	RAID_REQUEST_TYPES request_type_close = RAID_CLOSE;

	if (close_raid_cache() != 0) {
		logMessage(LOG_INFO_LEVEL, "ERROR Closing Cache.");
		return(-1);
	}

	// initialize close
	close->request_type = request_type_close;
	close->number_of_blocks = 0;
	close->disk_number = 0;
	close->reserved = 0;
	close->status = 0;
	close->blockid = 0;

	// call RAID_CLOSE
	close_opcode = generate_RAIDOpCode(close);
	close_opcode_response = client_raid_bus_request(close_opcode, buf);
	decode_RAIDOpCode(close_opcode_response, close_response);
	// check if not successful
	if (check_response(close, close_response) != 0) {
		logMessage(LOG_INFO_LEVEL, "ERROR: TAGLINE storage device closing FAILED.");
		return(-1);
	}

	// free the linked-lists:
	free_tag_linked_list(head_tag);

	// free global containers
	free(init);
	init = NULL;
	free(init_response);
	init_response = NULL;
	
	free(format);
	format = NULL;
	free(format_response);
	format_response = NULL;

	free(read);
	read = NULL;
	free(read_response);
	read_response = NULL;

	free(close);
	close = NULL;
	free(close_response);
	close_response = NULL;

	free(status);
	status = NULL;
	free(status_response);
	status_response = NULL;

	//free(temp_buf);
	//temp_buf = NULL;
	free(buf_read);
	buf_read = NULL;

	// Return successfully
	logMessage(LOG_INFO_LEVEL, "TAGLINE storage device: closing completed.");
	return(0);
}


//////////////////////////////////////////////////////////////////////////////////
// Function     : free_tag_linked_list
// Description  : recursively go through the list, free the list of blocks associated with each node
//		  and then free the tag
//
// Inputs       : tag - pointer to a tag node on the list
// Outputs      : N/A
void free_tag_linked_list(TAGLINE *tag) {
	if (tag->next_tagline != NULL) {
		free_tag_linked_list(tag->next_tagline);
	}
	free_block_linked_list(tag->block_0);
	free(tag);
	return;
}


//////////////////////////////////////////////////////////////////////////////////
// Function     : free_block_linked_list
// Description  : recursively goes thorugh the blocks, free a block and go back to the previous until block_0 
//
// Inputs       : block - pointer to a block node on the list
// Outputs      : N/A 
void free_block_linked_list(BLOCK *block) {
	if (block->next_block != NULL) {
		free_block_linked_list(block->next_block);
	}
	free(block);
	return;
}



//////////////////////////////////////////////////////////////////////////////////
// Function     : check_response
// Description  : compares the response and request opcodes and returns unsuccessful in there is a mismatch
//
// Inputs       : request - pointer to a structure that stores the request decoded opcode
//		  response - pointer to a structure that stores the response decoded opcode
// Outputs      :  0 if successful
//		  -1 if not successful
int check_response (RAID_REQUEST *request, RAID_RESPONSE *response) {
	// check every field to make sure they match
	if (request->request_type != response->request_type) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Request type mismatch");
		return(-1);
	}
	if (request->number_of_blocks != response->number_of_blocks) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Number of blocks mismatch");
		return(-1);
	}
	if (request->disk_number != response->disk_number) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Disk number mismatch");
		return(-1);
	}
	if (response->status != 0) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Status not successful");
		return(-1);
	}
	if (request->blockid != response->blockid) {
		logMessage(LOG_INFO_LEVEL, "ERROR: Request type mismatch");
		return(-1);
	}
	return(0);
}


//////////////////////////////////////////////////////////////////////////////////
// Function     : generate_RAIDOpCode
// Description  : generate a raid opcode for a particular set of values
//
// Inputs       : opcode - pointer to a structure that contains the fields that make up the opcode
// Outputs      : generated_opcode - RAIDOpCode type that has the final opcode bits
RAIDOpCode generate_RAIDOpCode (RAID_REQUEST *opcode) {
	RAIDOpCode generated_opcode = 0;
		
	// add request type
	generated_opcode = opcode->request_type;			// 0000 0000 0000 0012
	generated_opcode = generated_opcode << 8;			// 0000 0000 0000 1200
	generated_opcode = generated_opcode + opcode->number_of_blocks; // 0000 0000 0000 1234
	generated_opcode = generated_opcode << 8;			// 0000 0000 0012 3400
	generated_opcode = generated_opcode + opcode->disk_number;	// 0000 0000 0012 3456
	generated_opcode = generated_opcode << 7;			// 0000 0000 1234 5600
	generated_opcode = generated_opcode + opcode->reserved;		// 0000 0000 1234 5678
	generated_opcode = generated_opcode << 1;			// 0000 0000 2468 xxxx
	generated_opcode = generated_opcode + opcode->status;		// 0000 0000 xxxx xxxx
	generated_opcode = generated_opcode << 32;			// xxxx xxxx 0000 0000
	generated_opcode = generated_opcode + opcode->blockid;
	//generated_opcode = 0x0080050000000000;

	return(generated_opcode);
}


//////////////////////////////////////////////////////////////////////////////////
// Function     : decode_RAIDOpCode
// Description  : decode a raid opcode into its fields
//
// Inputs       : opcode - RAIDOpCode to decode
//		  response - pointer to a struct that will store the decoded values
// Outputs      : N/A
void decode_RAIDOpCode (RAIDOpCode opcode, RAID_RESPONSE *response) {

	// 0x00 80 05 00 000000
	
	// extract block ID
	response->blockid = opcode & 0x00000000FFFFFFFF;	// get the value of the last 31 bits and store it in block ID
	opcode = opcode >> 32;					// move left by 31 bits to get rid of bits already read

	// extract status
	response->status = opcode & 0x0000000000000001;		// get the value of the last bit and store it in status
	opcode = opcode >> 1;

	// extract reserved
	response->reserved = opcode & 0x000000000000003F;	// get the last 6 bits and store them in reserved
	opcode = opcode >> 7;

	// extract disk number
	response->disk_number = opcode & 0x00000000000000FF;	// get the last 8 bits and store them in disk number
	opcode = opcode >> 8;
	
	// extract number of blocks
	response->number_of_blocks = opcode & 0x00000000000000FF;	// get the last 8 bits and store them in number of blocks
	opcode = opcode >> 8;

	// extract request type
	response->request_type = opcode;					// no need to mask since it will only take the last 8 bits anyway
}

//////////////////////////////////////////////////////////////////////////////////
// Function     : add_new_tag 
// Description  : adds a new tag to the linked-list of tags, if the list is empty
//		  it adds the first tag, otherwise it finds the last tag and appends
//		  the new tag to it
//
// Inputs       : N/A
// Outputs      : 0 if successful, -1 if something goes wrong
int add_new_tag() {
	// create a new pointer to a tag
	TAGLINE *new_tag = malloc(sizeof(TAGLINE));
	TAGLINE *current = NULL;
	int tag_counter = 0;						
	// malloc successful?
	if (new_tag == NULL) {
		logMessage(LOG_INFO_LEVEL, "Malloc returns NULL when trying to allocate a new tag!");
		return(-1);
	}
	// initialize fields
	new_tag->tagline = tag_counter;
	new_tag->max_start_allowed = 0;
	new_tag->blocks_in_tagline = 0;
	new_tag->next_tagline = NULL;
	new_tag->block_0 = NULL;
	// if head is null (in other words, list is empty), then point head to the new tag
	if (head_tag == NULL) {
		head_tag = new_tag;
	}
	// otherwise, find the last tag and add the new tag to it
	else {
		current = head_tag;			// start iterating from the first tag
		tag_counter = 0;			// current points to tag 0 initially
		while (current->next_tagline != NULL) {		// iterate until we find last tag (last tag will have pointer next to NULL)
			current = current->next_tagline;
			tag_counter++;	
		}
		new_tag->tagline = tag_counter + 1;	// tag_counter contains the number of the last tag, so the new tag added will be +1
		current->next_tagline = new_tag;
	}
	// add 1 to the number of tags in use
	taglines_in_use++;
	// Return successfully
	return(0);
}


//////////////////////////////////////////////////////////////////////////////////
// Function     : RAID_scheduler
// Description  : finds a disk and block ID corresponding to the RAID array for a new block to be written
//
// Inputs       : new_scheduled_block - pointer to a block that stores a disk and block to store it in
// Outputs      : 0 if successful
//		 -1 if no space on RAID for this block
int RAID_scheduler(SCHEDULED_BLOCK *new_scheduled_block, RAIDDiskID primary_disk) {
//	int disk = 0;
//	int capacity = RAID_DISKBLOCKS - 1;
//	if (primary_disk == -1) {
//		for (disk = 0; disk < RAID_DISKS; disk++) {
//			if (last_block_added[disk] < (capacity - 1)) {
//				new_scheduled_block->disk = disk;
//				new_scheduled_block->block = last_block_added[disk] + 1;
//				return(0);
//			}
//		}
//	}
///	else {
//		for (disk = 0; disk < RAID_DISKS; disk++) {
//			if (disk != primary_disk) {
//				if (last_block_added[disk] < (capacity - 1)) {
//					new_scheduled_block->disk = disk;
//					new_scheduled_block->block = last_block_added[disk] + 1;
//					return(0);
//				}
//			}
//		}
//	}
//	logMessage(LOG_INFO_LEVEL, "ERROR: RAID disks ran out of space");
//	return(-1);

	if (current_block == RAID_DISKBLOCKS && current_disk == RAID_DISKS) {
		logMessage(LOG_ERROR_LEVEL, "Tagline : All Disks are full!");
		return(-1);
	}

	if (current_block == RAID_DISKBLOCKS-1 && current_disk == RAID_DISKS) {
		new_scheduled_block->disk = current_disk;
		new_scheduled_block->block = current_block;
		current_disk++;
		current_block++;
		return(0);
	}

	if (current_disk == RAID_DISKS-1) {
		new_scheduled_block->disk = current_disk;
		new_scheduled_block->block = current_block;
		current_disk = 0;
		if (current_block == RAID_DISKBLOCKS) {
			current_block = 0;
		}
		else {
			current_block++;
		}
		return(0);
	}
	else {
		new_scheduled_block->disk = current_disk;
		new_scheduled_block->block = current_block;
		current_disk++;
		return(0);
	}

	return(0);

}


//////////////////////////////////////////////////////////////////////////////////
// Function     : get_max_start_allowed
// Description  : given a tag, find the max start allowed to start allocating/reading blocks
//
// Inputs       : tag - number of the tag to read/write a block from/to
// Outputs      : max_start_allowed - max block number to read/write
TagLineBlockNumber get_max_start_allowed(TagLineNumber tag) {
	TAGLINE *current = NULL;
	current = head_tag;
	while (current->tagline != tag) {
		current = current->next_tagline;
	}
	return current->max_start_allowed;
}
