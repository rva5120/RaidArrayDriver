////////////////////////////////////////////////////////////////////////////////
//
//  File           : raid_cache.c
//  Description    : This is the implementation of the cache for the TAGLINE
//                   driver.
//
//  Author         : Raquel Alvarez
//  Last Modified  : Thursday, November 19th 2015
// ****************************************************************************
// The following code implements an LRU cache. A hash table, with HASHTABLE_SIZE
// number of entries, maps to a doubly-linked list (with 2*TAGLINE_CACHE_SIZE). 
// The doubly-linked list will be performing the task of a queue, where the first 
// item is the block least recently read/written.
// To decide how many entries the hash table should have, I wrote a short program
// (https://www.github.com/rva5120/hashalyzer). The program will 
// calculate the distribution of hash values (using a modular hash function).
// For this assignment, I started by analyzing constant access time and comparing
// values reducing the number of buckets by 2 until a good balance in between 
// memory allocation and access time was reached.
// 
//	Buckets	   O(x)		  Time		 Power
//		Worst Case   (10,000 accesses)		
//     ------------------------------------------------
//	 36864	   O(1)		10,000		  --
//	 32768	   O(2)		20,000		 2^15
//	 18432	   O(3)		30,000		  --
//	 16384	   O(4)		40,000		 2^14
//	  9216	   O(6)		60,000		  --
//	  8192	   O(5)		50,000		 2^13
//	  4608	   O(9)		90,000		  --
//	  4096	   O(9)		90,000		 2^12
//	  2304	   O(18)       180,000		  --
//	  2048	   O(18)       180,000		 2^11
//
// Based on the results, I decided to go with 8192 buckets (which is 2*blocks_per_disk)
// because we get a better time, for less memory. After that point, the rest
// of the values are not as efficient access timewise, and the amount of memory
// saved is not worth the extra access time. Since this is a cache implementation,
// we should be prioritizing the optimization of access time more than memory
// allocation. So we go with 2*RAID_DISKBLOCKS buckets on the hash table.
// Each bucket on the hash table will have a linked-list of extra values to fix
// any collision issue (which will happen since the number of buckets is less than
// the total number of addressable block).
// Each bucket has a pointer to a node on the queue or a null pointer. The queue is
// organized so the front of the queue is the least recently accessed block, and the 
// back of the queue is the last accessed block. Each node has a corresponding bucket
// mapped to it.


// Includes
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

// Project includes
#include <cmpsc311_log.h>
#include <cmpsc311_util.h>
#include <raid_cache.h>

// Defines
#define HASHTABLE_SIZE	8192
#define RAIDOpCode_c	uint64_t

struct hash_node;

// Data Structures Definitions
//	Nodes for queue
// next_node: points towards the front of the queue (LRU)
// prev_node: points towards the back of the queue (MRU)
typedef struct queue_node {
	char			*value_buf;
	int			age_bit;
	struct queue_node	*next_node;
	struct queue_node	*prev_node;
	struct hash_node	*parent_hash_node;
} QUEUE_NODE;
//	Nodes for hashtable
typedef struct hash_node {
	RAIDDiskID		disk;
	RAIDBlockID		block;
	QUEUE_NODE		*cache_block_ptr;
	struct hash_node	*next_node;
} HASH_NODE;
// 	Queue structure
typedef struct {
	int 		capacity;
	QUEUE_NODE 	*back_ptr;
	QUEUE_NODE	*front_ptr;
} QUEUE;
// -----------------------------
// Structures to add to separate file later on
typedef struct {
	uint8_t		request_type;		// 8 bits
	uint8_t		number_of_blocks;	// 8 bits
	RAIDDiskID 	disk_number;		// 8 bits
	uint8_t		reserved;		// 6 bits
	uint8_t		status;			// 1 bit
	RAIDBlockID	blockid;		// 31 bits
} RAID_REQUEST_c, RAID_RESPONSE_c;
// -----------------------------

// Data Structures - Declarations
static QUEUE			cache_queue;
static HASH_NODE		*hashtable[HASHTABLE_SIZE] = {[0 ... HASHTABLE_SIZE-1] = NULL};
static int			max_cache_size;
static RAID_REQUEST_c		*write = NULL;
static RAID_RESPONSE_c		*write_response = NULL;
static RAID_REQUEST_c		*read = NULL;
static RAID_RESPONSE_c		*read_response = NULL;
static RAIDOpCode_c		write_opcode;
static RAIDOpCode_c		write_opcode_response;
static RAID_REQUEST_TYPES	read_request_code;
static RAID_REQUEST_TYPES	write_request_code;
static int 			total_cache_inserts = 0;
static int			total_cache_gets = 0;
static int			total_cache_hits = 0;
static int			total_cache_misses = 0;
static double			cache_efficiency = 0;
// -----------------------------

// Function Prototypes:
int hashfunction (RAIDDiskID, RAIDBlockID);
void free_hash_nodes (HASH_NODE *n);
int evict_lru();
int update_block_in_queue(QUEUE_NODE *n);
int insert_in_queue(QUEUE_NODE *n);
// -----------------------------


// AUXILIARY FUNCTIONS - Might get moved to a separate file shared with tagline driver
// ----------------------------------------------------------------------
// Function     : generate_RAIDOpCode
// Description  : generate a raid opcode for a particular set of values
//
// Inputs       : opcode - pointer to a structure that contains the fields that make up the opcode
// Outputs      : generated_opcode - RAIDOpCode type that has the final opcode bits
RAIDOpCode_c generate_RAIDOpCode_c (RAID_REQUEST_c *opcode) {
	RAIDOpCode_c generated_opcode = 0;
		
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
void decode_RAIDOpCode_c (RAIDOpCode_c opcode, RAID_RESPONSE_c *response) {

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
	response->request_type = opcode;				// no need to mask since it will only take the last 8 bits anyway
}
// ----------------------------------------------------------------------



// TAGLINE Cache interface


////////////////////////////////////////////////////////////////////////////////
//
// Function     : init_raid_cache
// Description  : Initialize the cache and note maximum blocks
//
// Inputs       : max_items - the maximum number of items your cache can hold
// Outputs      : 0 if successful, -1 if failure

int init_raid_cache(uint32_t max_items) {

	// Initialize queue
	cache_queue.capacity = 0;	// no items have been stored in the cache yet
	cache_queue.back_ptr = NULL;	// no nodes have been allocated yet
	cache_queue.front_ptr = NULL;	

	// Set max number of cache blocks based on requirements
	max_cache_size = max_items;

	// Init raid_bus variables
	write = malloc(sizeof(RAID_REQUEST_c));
	write_response = malloc(sizeof(RAID_RESPONSE_c));
	read = malloc(sizeof(RAID_REQUEST_c));
	read_response = malloc(sizeof(RAID_RESPONSE_c));

	if (read == NULL) {
		logMessage(LOG_INFO_LEVEL, "READ is null in cache!");
		return(-1);
	}
	else {
		logMessage(LOG_INFO_LEVEL, "REAd is not null %p", read);
	}

	// Initialize request codes
	read_request_code = RAID_READ;
	write_request_code = RAID_WRITE;

	// Return successfully
	return(0);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : close_raid_cache
// Description  : Clear all of the contents of the cache, cleanup
//
// Inputs       : none
// Outputs      : o if successful, -1 if failure

int close_raid_cache(void) {

	// Loop Through Hashtable and free queue node
	int 		entry = 0;
	HASH_NODE	*current_node = NULL;

	for (entry = 0; entry < HASHTABLE_SIZE; entry++) {
		current_node = hashtable[entry];
		if (current_node != NULL) {
			free_hash_nodes(current_node);
		}	
	}
	
	logMessage(LOG_INFO_LEVEL, "CACHE : HashTable and Queue Blocks Free'd");

	cache_efficiency = ((double)total_cache_hits/(total_cache_hits+total_cache_misses))*100;

	logMessage(LOG_OUTPUT_LEVEL, "** Cache Statistics **");
	logMessage(LOG_OUTPUT_LEVEL, "Total cache inserts: %7d", total_cache_inserts);
	logMessage(LOG_OUTPUT_LEVEL, "Total cache gets: %7d", total_cache_gets);
	logMessage(LOG_OUTPUT_LEVEL, "Total cache hits: %7d", total_cache_hits);
	logMessage(LOG_OUTPUT_LEVEL, "Total cache misses: %7d", total_cache_misses);
	logMessage(LOG_OUTPUT_LEVEL, "Cache efficiency:  %.2f%%", cache_efficiency);

	
	// Return successfully
	return(0);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : put_raid_cache
// Description  : Put an object into the block cache
//
// Inputs       : dsk - this is the disk number of the block to cache
//                blk - this is the block number of the block to cache
//                buf - the buffer to insert into the cache
// Outputs      : 0 if successful, -1 if failure

int put_raid_cache(RAIDDiskID dsk, RAIDBlockID blk, void *buf)  {
	// Variables
	int hash_value = 0;
	HASH_NODE *current_hash_node = NULL;	// used to iterate through nodes attached to hashtable indexes	
	HASH_NODE *prev_hash_node = NULL;	// used to keep track of the previous node, 
						// useful to add a new node when current_hash_node is NULL
//	int null_node = 0;
	HASH_NODE 	*new_hash_node = NULL;
	QUEUE_NODE 	*new_queue_node = NULL;

	logMessage(LOG_INFO_LEVEL, "Disk %d  Block %d", dsk, blk);

	// Call hashfunction to get the corresponding index on the hashtable for the disk,block pair
	hash_value = hashfunction(dsk,blk);

	//total_cache_gets++;
	
	// Go to the hashtable and check if the disk and block pair has an entry on the table
	if (hashtable[hash_value] == NULL) {	// Not disk,block entry found
		// Add node to hashtable index
		logMessage(LOG_INFO_LEVEL, "Adding a new node to the hashtable");
		new_hash_node = malloc(sizeof(HASH_NODE));
		new_hash_node->disk = dsk;
		new_hash_node->block = blk;
		new_hash_node->cache_block_ptr = NULL;
		new_hash_node->next_node = NULL;

		hashtable[hash_value] = new_hash_node;

		// Add a new entry to the queue
		logMessage(LOG_INFO_LEVEL, "Adding a new entry to the queue");
		new_queue_node = malloc(sizeof(QUEUE_NODE));
		new_queue_node->value_buf = malloc(sizeof(char)*RAID_BLOCK_SIZE);
		memcpy(new_queue_node->value_buf, buf, sizeof(char)*RAID_BLOCK_SIZE);
		logMessage(LOG_INFO_LEVEL, "Current value in buffer of block: %c",*( new_queue_node->value_buf ));
		new_queue_node->parent_hash_node = new_hash_node;
		new_queue_node->age_bit = 0; //will be updated at insertion
		new_queue_node->next_node = NULL;
		new_queue_node->prev_node = NULL;
		new_hash_node->cache_block_ptr = new_queue_node;
		
		if (cache_queue.capacity == 0) {
			// set front and back pointers
			logMessage(LOG_INFO_LEVEL, "Adding first element to the cache");
			cache_queue.front_ptr = new_queue_node;
			cache_queue.back_ptr = new_queue_node;
			cache_queue.capacity++;
			total_cache_misses++;
			total_cache_inserts++;
		}
		else if (cache_queue.capacity == 1) {
			logMessage(LOG_INFO_LEVEL, "Adding second element to the cache");
			cache_queue.back_ptr = new_queue_node;
			cache_queue.front_ptr->prev_node = new_queue_node;
			new_queue_node->next_node = cache_queue.front_ptr;
			cache_queue.capacity++;
			total_cache_misses++;
			total_cache_inserts++;
		}	
		else {
			logMessage(LOG_INFO_LEVEL, "Adding a new element to the cache - Disk %d Block %d", new_hash_node->disk, new_hash_node->block);
			total_cache_inserts++;
			total_cache_misses++;
			if (insert_in_queue(new_queue_node)) {
				logMessage(LOG_INFO_LEVEL, "Error inserting new node on the queue!\n");
				return(-1);
			}
		}
	}	
	else { // hashtable[hash_value] != NULL 	
		
		// There are entries on the map, check to see if disk,block pair is present
		// Loop through nodes in hashtable entry to see if disk,block pair is found
		current_hash_node = hashtable[hash_value];	// current_hash_node points at the node pointed at by hashtable[hash_value]
		prev_hash_node = hashtable[hash_value];		// prev_hash_node will keep track of the node before the current

		int i = 0;

		while ( current_hash_node != NULL ) {
			if (current_hash_node->disk == dsk && current_hash_node->block == blk) {
				break;
			}
			logMessage(LOG_INFO_LEVEL, "%d", i);
			i++;
			prev_hash_node = current_hash_node;
			current_hash_node = current_hash_node->next_node;
			logMessage(LOG_INFO_LEVEL, "%x", current_hash_node);
		}
		// disk,block pair was not found in hashtable
		if (current_hash_node == NULL) {
			logMessage(LOG_INFO_LEVEL, "Already an entry in hashtable, but not pair wanted, adding it now");
			// add pair to hash table
			// create a new node		
			new_hash_node = malloc(sizeof(HASH_NODE));
			new_hash_node->disk = dsk;
			new_hash_node->block = blk;
			new_hash_node->next_node = NULL;
			// add it to the end of the list of nodes for that hashtable index
			prev_hash_node->next_node = new_hash_node;
			// create a new queue node for this hash node and insert it on the queue
			new_queue_node = malloc(sizeof(QUEUE_NODE));
			new_queue_node->value_buf = malloc(sizeof(char)*RAID_BLOCK_SIZE);
			new_queue_node->parent_hash_node = new_hash_node;
			new_hash_node->cache_block_ptr = new_queue_node;
			memcpy(new_queue_node->value_buf, buf, sizeof(char)*RAID_BLOCK_SIZE);
			new_queue_node->age_bit = 0;
			new_queue_node->next_node = NULL;
			new_queue_node->prev_node = NULL;
			//insert it into the queue
			logMessage(LOG_INFO_LEVEL, "Adding the new pair to the queue");
			total_cache_inserts++;
			total_cache_misses++;
			if (insert_in_queue(new_queue_node) != 0) {
				logMessage(LOG_INFO_LEVEL, "Error inserting node in queue! For new appended hashtable hash node");
				return(-1);
			}
		} 
		else {  // disk,block pair was found
			// check to see if this disk,block pair has an entry in the cache queue
			if (current_hash_node->cache_block_ptr == NULL) {
				logMessage(LOG_INFO_LEVEL, "The disk-block pair is in hashtable, and has a node, but it's not in the cache, so we add it");
				// if the pair is not currently in the cache, add it
				new_queue_node = malloc(sizeof(QUEUE_NODE));
				new_queue_node->value_buf = malloc(sizeof(char)*RAID_BLOCK_SIZE);
				new_queue_node->parent_hash_node = current_hash_node;
				memcpy(new_queue_node->value_buf, buf, sizeof(char)*RAID_BLOCK_SIZE);
				new_queue_node->age_bit = 0;
				new_queue_node->next_node = NULL;
				new_queue_node->prev_node = NULL;
				current_hash_node->cache_block_ptr = new_queue_node;
				// insert node
				total_cache_inserts++;
				total_cache_misses++;
				if (insert_in_queue(new_queue_node) != 0) {
					logMessage(LOG_INFO_LEVEL, "Error adding a new node to the queue, from existing node in hashtable!");
					return(-1);
				}
			}
			else {
				// if the pair is already on the cache, update it
				total_cache_hits++;
				logMessage(LOG_INFO_LEVEL, "The block is in the cache, so we move it to the end.");
				memcpy(current_hash_node->cache_block_ptr->value_buf, buf, RAID_BLOCK_SIZE);
				if (update_block_in_queue(current_hash_node->cache_block_ptr) != 0) {
					logMessage(LOG_INFO_LEVEL, "Error updating cache value");
					return(-1);
				}
			}
		}
	}
	// Return successfully
	logMessage(LOG_INFO_LEVEL, "Value successfully put/updated in cache");
	return(0);
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : get_raid_cache
// Description  : Get an object from the cache (and return it)
//
// Inputs       : dsk - this is the disk number of the block to find
//                blk - this is the block number of the block to find
// Outputs      : pointer to cached object or NULL if not found
void * get_raid_cache(RAIDDiskID dsk, RAIDBlockID blk) {

	int hash_value = 0;
	HASH_NODE *current_hash_node = NULL;	// used to iterate through nodes attached to hashtable indexes	

	total_cache_gets++;

	// Get corresponding entry in hashtable
	hash_value = hashfunction(dsk,blk);

	
	// Check to see if the disk block pair is present in the hashtable
	if (hashtable[hash_value] == NULL) {
		total_cache_misses++;
		return(NULL);
	}	
	else { // hashtable[hash_value] != NULL 	
		
		// There are entries on the map, check to see if disk,block pair is present
		// Loop through nodes in hashtable entry to see if disk,block pair is found
		current_hash_node = hashtable[hash_value];	// current_hash_node points at the node pointed at by hashtable[hash_value]
		while ( current_hash_node != NULL ) { 
			if (current_hash_node->disk == dsk && current_hash_node->block == blk) {
				break;	
			}
			current_hash_node = current_hash_node->next_node;
		}
		//logMessage(LOG_INFO_LEVEL, "After while loop: disk %d block %d vs %d %d", current_hash_node->disk, current_hash_node->block, dsk, blk);
		// disk,block pair was not found in hashtable
		if (current_hash_node == NULL) {
			total_cache_misses++;	
			return(NULL);
		} 
		else {  // disk,block pair was found
			// check to see if this disk,block pair has an entry in the cache queue
			if (current_hash_node->cache_block_ptr == NULL) {
				total_cache_misses++;
				return(NULL);
			}
			else {
				// if the pair is already on the cache, update it
				total_cache_hits++;
				logMessage(LOG_INFO_LEVEL, "Disk: %d  Block: %d on read", current_hash_node->disk, current_hash_node->block);
				logMessage(LOG_INFO_LEVEL, "Print value in buffer: %c", *(current_hash_node->cache_block_ptr->value_buf));
				return(current_hash_node->cache_block_ptr->value_buf); 
			}
		}
	}

	logMessage(LOG_INFO_LEVEL, "ERROR in GET_RAID!!");
	return(NULL);
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : insert_in_queue
// Description  : Insert a node in the back of the queue
//
// Inputs       : node - a pointer to the node to add
// Outputs      : 0 if successful, -1 otherwise
int insert_in_queue(QUEUE_NODE *node) {
	//int last_age_bit = 0;
	// Add node to the back of the queue
	node->next_node = cache_queue.back_ptr;
	node->prev_node = NULL;
	cache_queue.back_ptr->prev_node = node;
	//node->age_bit = (cache_queue.back_ptr->age_bit) + 1;
	cache_queue.back_ptr = node;
	cache_queue.capacity++;
	// if the cache is larger than the alloted amount, evict front item
	if (cache_queue.capacity > TAGLINE_CACHE_SIZE) {
		logMessage(LOG_INFO_LEVEL, "Evicting LRU after insert");
		if (evict_lru() != 0) {
			logMessage(LOG_INFO_LEVEL, "Error trying to evict a node from the queue!");
			return(-1);
		}
	}

	return(0);
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : update_block_in_queue
// Description  : Modify age-bit and move block to the back of the queue
//
// Inputs       : node - a pointer to the node to add
// Outputs      : 0 if successful, -1 otherwise
int update_block_in_queue(QUEUE_NODE *node) {
	
	// move block to the back of the queue
	// set prev_node ptr to next_node ptr
	
	if (node->prev_node == NULL && node->next_node == NULL) {
		// new node to add
		cache_queue.back_ptr->prev_node = node;
		node->next_node = cache_queue.back_ptr;
		node->prev_node = NULL;
		cache_queue.back_ptr = node;
	}
	else if (node->prev_node == NULL && node->next_node != NULL) {
		// updating the back of the queue
		// nothing to do, already at the LRU position
	}
	else if (node->prev_node != NULL && node->next_node == NULL) {
		// updating the front of the queue
		cache_queue.front_ptr->next_node = cache_queue.back_ptr;
		cache_queue.back_ptr->prev_node = cache_queue.front_ptr;
		cache_queue.front_ptr->prev_node->next_node = NULL;
		cache_queue.back_ptr = cache_queue.front_ptr;
		cache_queue.front_ptr = cache_queue.front_ptr->prev_node;
		cache_queue.back_ptr->prev_node = NULL;
	}
	else {
		// updating from the middle of the queue
		node->prev_node->next_node = node->next_node;
		node->next_node->prev_node = node->prev_node;
		cache_queue.back_ptr->prev_node = node;
		node->next_node = cache_queue.back_ptr;
		node->prev_node = NULL;
		cache_queue.back_ptr = node;
	}

	return(0);
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : evict_lru
// Description  : Eject the LRU item fromt the cache
//
// Inputs       : N/A
// Outputs      : 0 if successful, -1 otherwise
int evict_lru() {

	QUEUE_NODE 	*eject_queue_node;
	RAIDDiskID	disk;
	RAIDBlockID	block;

	eject_queue_node = cache_queue.front_ptr;			// record the location in the queue of the block to evict
	if (cache_queue.front_ptr != NULL) {
		disk = eject_queue_node->parent_hash_node->disk;			// save disk and block to perform a write to the RAID
		block = eject_queue_node->parent_hash_node->block;		
		
		logMessage(LOG_INFO_LEVEL, "Disk %d and Block %d to be updated in disk by eviction", disk, block);

	}
	else {
		logMessage(LOG_ERROR_LEVEL, "Parent hash node null!!");
		return(-1);
	}

	// Update the evicted block on the disk
	write->request_type = write_request_code;
	write->number_of_blocks = 1;
	write->disk_number = disk;		
	write->reserved = 0;
	write->status = 0;
	write->blockid = block;
												
	write_opcode = generate_RAIDOpCode_c(write);						
	write_opcode_response = client_raid_bus_request(write_opcode, eject_queue_node->value_buf);	// WRITE block to RAID Disk
	decode_RAIDOpCode_c(write_opcode_response, write_response);
	
	if (write_response->status != 0) {
		logMessage(LOG_INFO_LEVEL, "Error writing to the disk an evicted block!");
		return(-1);
	}

	// update queue
	cache_queue.front_ptr = eject_queue_node->prev_node;		// update front pointer
	eject_queue_node->prev_node->next_node = NULL;			// update new front pointer
	eject_queue_node->parent_hash_node->cache_block_ptr = NULL;	// update node on hashtable	
	eject_queue_node->parent_hash_node = NULL;
	eject_queue_node->prev_node = NULL;
	free(eject_queue_node->value_buf);	

	cache_queue.capacity = cache_queue.capacity - 1;

	free(eject_queue_node);		// free memory of evicted block

	return(0);
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : free_hash_nodes
// Description  : Frees every node in the hash table and every queue node (and value_buf)
//		  associated with each hash node
//
// Inputs       : first hash node of the entry, and next nodes recursively until the last one
// Outputs      : N/A
void free_hash_nodes(HASH_NODE *node) {
	
	if (node->next_node != NULL) {
		free_hash_nodes(node->next_node);
	}

	// free value_buf in queue node and queue node
	if (node->cache_block_ptr != NULL) {
		free(node->cache_block_ptr->value_buf);
		free(node->cache_block_ptr);
	}

	// free hash node
	free(node);
	
	return;
}


////////////////////////////////////////////////////////////////////////////////
//
// Function     : hashfunction
// Description  : Returns the corresponding hash value for a disk and block pair
//
// Inputs       : disk, block
// Outputs      : hash value
int hashfunction(RAIDDiskID disk, RAIDBlockID block) {

	int hash_value = 0;
	int temp = 0;

	temp = disk*10000;
	temp = temp + block;
	hash_value = temp % HASHTABLE_SIZE;	

	return(hash_value);
}
