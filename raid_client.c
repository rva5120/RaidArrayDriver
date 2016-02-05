////////////////////////////////////////////////////////////////////////////////
//
//  File          : raid_client.c
//  Description   : This is the client side of the RAID communication protocol.
//
//  Author        : Raquel Alvarez
//  Last Modified : December 6th 2015
//

// Include Files
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>

// Project Include Files
#include <raid_network.h>
#include <cmpsc311_log.h>
#include <cmpsc311_util.h>

// Global data
unsigned char *raid_network_address = NULL; // Address of CRUD server
unsigned short raid_network_port = 0; // Port of CRUD server
static int new_connection = 1;
static int socket_fd = -1;

//
// Functions

////////////////////////////////////////////////////////////////////////////////
//
// Function     : client_raid_bus_request
// Description  : This the client operation that sends a request to the RAID
//                server.   It will:
//
//                1) if INIT make a connection to the server
//                2) send any request to the server, returning results
//                3) if CLOSE, will close the connection
//
// Inputs       : op - the request opcode for the command
//                buf - the block to be read/written from (READ/WRITE)
// Outputs      : the response structure encoded as needed

RAIDOpCode client_raid_bus_request(RAIDOpCode op, void *buf) {

	// Variables	
	uint8_t request_type = 0;
	RAID_REQUEST_TYPES init = RAID_INIT;
	RAID_REQUEST_TYPES close_req = RAID_CLOSE;
	RAID_REQUEST_TYPES read_req = RAID_READ;
	RAID_REQUEST_TYPES write_req = RAID_WRITE;
	RAID_REQUEST_TYPES format = RAID_FORMAT;
	// Network Variables
	char 	*ip = RAID_DEFAULT_IP;
	struct 	sockaddr_in client_addr;
	uint64_t data;
	RAIDOpCode response_op;

	request_type = (op >> 56);			// Store last 8 bits of opcode (request type)
	raid_network_port = RAID_DEFAULT_PORT;
	//*raid_network_address = RAID_DEFAULT_IP;

	logMessage(LOG_INFO_LEVEL, "Request type %d", request_type);

	if (request_type == init) {		// Handle INIT command
		
		if (new_connection == 1) {

			logMessage(LOG_INFO_LEVEL, "Network : Initializing connection with server....");

			// get address
			client_addr.sin_family = AF_INET;
			client_addr.sin_port = htons(raid_network_port);
			if ( inet_aton(ip, &client_addr.sin_addr) == 0 ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error getting address.");
				return(-1);
			}
			logMessage(LOG_INFO_LEVEL, "Network : Address resolved.");		
	


			// create a socket
			socket_fd = socket(PF_INET, SOCK_STREAM, 0);
			if ( socket_fd == -1 ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error on socket creation.");
				return(-1);
			}
			logMessage(LOG_INFO_LEVEL, "Network : Socket created.");


	
			// connect to the server
			if ( connect(socket_fd, (const struct sockaddr *)&client_addr, sizeof(client_addr)) == -1 ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error connecting to server.");
				return(-1);
			}
			logMessage(LOG_INFO_LEVEL, "Network : ....Successfully connected to server.");		
			
			new_connection = 0; // no need to run this code again for the next INITs
		
		}
	
		// Send INIT commands to RAID Bus	
		// data transfer to server
		logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : INIT Opcode sent.");

		// --- length transfer ---
		data = htonll64(0);	// send 0 because buf has 0 length for INIT
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
		// No buffer to transfer for INIT
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Length received.");

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, RAID_BLOCK_SIZE) != RAID_BLOCK_SIZE ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
			data = ntohll64(data);
			logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");
		
		// Return response opcode
		return( response_op );


	}
	else if ( request_type == format ) {					// Handle the rest of commands

		// Send FORMAT commands to the RAID Bus
		// data transfer to server
		logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : FORMAT Opcode sent.");

		// --- length transfer ---
		data = htonll64(0);	// send 0 because buf has 0 length for INIT
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
		// No buffer needs to be transferred for FORMAT
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Length received.");

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, RAID_BLOCK_SIZE) != RAID_BLOCK_SIZE ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
			data = ntohll64(data);
			logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");
	
		// Return response opcode
		return ( response_op );


	}
	else if ( request_type == read_req ) {
		
		// Send READ command to the RAID Bus
		// data transfer to server
		//logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		//logMessage(LOG_INFO_LEVEL, "Network : READ Opcode sent.");

		// --- length transfer ---
		data = htonll64(0);	// send 0 because buf has 0 length for INIT
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		//logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
		data = htonll64(0);
	//	if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
	//		logMessage(LOG_ERROR_LEVEL, "Network : Error sending buffer data to server.");
	//		return(-1);
	//	}
		//logMessage(LOG_INFO_LEVEL, "Network : Buffer sent.");
		//logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		//logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		//logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		//logMessage(LOG_INFO_LEVEL, "Network : Length received.");

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, RAID_BLOCK_SIZE) != RAID_BLOCK_SIZE ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
		//logMessage(LOG_INFO_LEVEL, "Network : Data in buffer %c.", *((char *)buf));
		//logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");

		// Return response opcode	
		return (response_op);
	
	}
	else if ( request_type == write_req ) {
		
		// Send WRITE commands to the RAID Bus
		// data transfer to server
		//logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : WRITE Opcode sent.");

		// --- length transfer ---
		data = htonll64(RAID_BLOCK_SIZE);
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
		if ( write(socket_fd, buf, RAID_BLOCK_SIZE) != RAID_BLOCK_SIZE ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending buffer data to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Buffer sent.");
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Length received %d.", data);

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, RAID_BLOCK_SIZE) != RAID_BLOCK_SIZE ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
			//logMessage(LOG_INFO_LEVEL, "Network : Data in buffer %c.", *((char *)buf));
			//logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");

		// Return response opcode	
		return (response_op);

	
	}
	else if (request_type == close_req) {

		// Send CLOSE commands to RAID Bus
		// data transfer to server
		logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : CLOSE Opcode sent.");

		// --- length transfer ---
		data = htonll64(0);	// send 0 because buf has 0 length for INIT
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
	//	data = htonll64(0);
	//	if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
	//		logMessage(LOG_ERROR_LEVEL, "Network : Error sending buffer data to server.");
	//		return(-1);
	//	}
	//	logMessage(LOG_INFO_LEVEL, "Network : Buffer sent.");
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Length received.");

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, data) != data ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
			data = ntohll64(data);
			logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");


		// Close connection with server
		close(socket_fd);

		// Return response opcode
		return( response_op );
	
	}
	else {
		// data transfer to server
		logMessage(LOG_INFO_LEVEL, "Network : Starting data transfer to server....");
		// --- opcode transfer ---
		data = htonll64(op);	// convert opcode to network byte order and send it
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending opcode to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : OTHER Opcode sent.");

		// --- length transfer ---
		data = htonll64(0);	// send 0 because buf has 0 length for INIT
		if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error sending length of buf to server.");
			return(-1);
		}
		logMessage(LOG_INFO_LEVEL, "Network : Length sent.");

		// --- buffer transfer ---
	//	data = htonll64(0);
	//	if ( write(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
	//		logMessage(LOG_ERROR_LEVEL, "Network : Error sending buffer data to server.");
	//		return(-1);
	//	}
	//	logMessage(LOG_INFO_LEVEL, "Network : Buffer sent.");
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully transferred data to server.");


		// data transfer from server
		logMessage(LOG_INFO_LEVEL, "Network : Starting to receive data from server....");
		// --- opcode receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving opcode confirmation.");
			return(-1);
		}
		response_op = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Opcode received.");
		
		// --- length receive ---
		if ( read(socket_fd, &data, sizeof(data)) != sizeof(data) ) {
			logMessage(LOG_ERROR_LEVEL, "Network : Error receiving length confirmation.");
			return(-1);
		}
		data = ntohll64(data);
		logMessage(LOG_INFO_LEVEL, "Network : Length received.");

		// --- buffer receive ---
		if (data > 0) {
			if ( read(socket_fd, buf, data) != data ) {
				logMessage(LOG_ERROR_LEVEL, "Network : Error receiving buffer confirmation.");
				return(-1);
			}
			data = ntohll64(data);
			logMessage(LOG_INFO_LEVEL, "Network : Buffer received.");
		}
		logMessage(LOG_INFO_LEVEL, "Network : ....Successfully received data from server.");


		// Return response opcode
		return( response_op );

	}

	return(0);

}



