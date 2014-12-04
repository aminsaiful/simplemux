/**************************************************************************
 * simplemux.c            version 1.4.7                                   *
 *                                                                        *
 * Simplemux compresses headers using ROHC (RFC 3095), and multiplexes    *
 * these header-compressed packets between a pair of machines (called     *
 * optimizers). The multiplexed bundle is sent in an IP/UDP packet.       *
 *                                                                        *
 * Simplemux can be seen as a naive implementation of TCM , a protocol    *
 * combining Tunneling, Compressing and Multiplexing for the optimization *
 * of small-packet flows. TCM may use of a number of different standard   *
 * algorithms for header compression, multiplexing and tunneling,         *
 * combined in a similar way to RFC 4170.                                 *
 *                                                                        *
 * In 2014 Jose Saldana wrote this program, published under GNU GENERAL   *
 * PUBLIC LICENSE, Version 3, 29 June 2007                                *
 * Copyright (C) 2007 Free Software Foundation, Inc.                      *
 *                                                                        *
 * Thanks to Davide Brini for his simpletun.c program. (2010)             *
 * http://backreference.org/wp-content/uploads/2010/03/simpletun.tar.bz2  *
 *                                                                        *
 * Simplemux uses an implementation of ROHC by Didier Barvaux             *
 * (https://rohc-lib.org/).                                               *
 *                                                                        *
 * Simplemux has been written for research purposes, so if you find it    *
 * useful, I would appreciate that you send a message sharing your        *
 * experiences, and your improvement suggestions.                         *
 *                                                                        *
 * DISCLAIMER AND WARNING: this is all work in progress. The code is      *
 * ugly, the algorithms are naive, error checking and input validation    *
 * are very basic, and of course there can be bugs. If that's not enough, *
 * the program has not been thoroughly tested, so it might even fail at   *
 * the few simple things it should be supposed to do right.               *
 * Needless to say, I take no responsibility whatsoever for what the      *
 * program might do. The program has been written mostly for research     *
 * purposes, and can be used in the hope that is useful, but everything   *
 * is to be taken "as is" and without any kind of warranty, implicit or   *
 * explicit. See the file LICENSE for further details.                    *
 *************************************************************************/ 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h> 
#include <sys/select.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>			// for printing uint_64 numbers
#include <stdbool.h>			// for using the bool type
#include <rohc/rohc.h>			// for using header compression
#include <rohc/rohc_comp.h>
#include <rohc/rohc_decomp.h>


#define BUFSIZE 2000   			// buffer for reading from tun/tap interface, must be >= MTU
#define MTU 1500				// it has to be equal or higher than the one in the network
#define PORT 55555				// default port
#define MAXPKTS 100				// maximum number of packets to store
#define MAXTHRESHOLD 1472		// default threshold of the maximum number of bytes to store. When it is reached, the sending is triggered. By default it is 1500 - 28 (IP/UDP tunneling header)
#define MAXTIMEOUT 100000000.0	// maximum value of the timeout (microseconds). (default 100 seconds)


/* global variables */
int debug;						// 0:no debug; 1:minimum debug; 2:maximum debug 
char *progname;

/**************************************************************************
 * tun_alloc: allocates or reconnects to a tun/tap device. The caller     *
 *            must reserve enough space in *dev.                          *
 **************************************************************************/
int tun_alloc(char *dev, int flags) {

	struct ifreq ifr;
	int fd, err;
	char *clonedev = "/dev/net/tun";

	if( (fd = open(clonedev , O_RDWR)) < 0 ) {
    	perror("Opening /dev/net/tun");
    	return fd;
	}

	memset(&ifr, 0, sizeof(ifr));

	ifr.ifr_flags = flags;

	if (*dev) {
	 	strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	}

	if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
	    perror("ioctl(TUNSETIFF)");
	    close(fd);
	    return err;
	}

	strcpy(dev, ifr.ifr_name);

	return fd;
}

/**************************************************************************
 * cread: read routine that checks for errors and exits if an error is    *
 *        returned.                                                       *
 **************************************************************************/
int cread(int fd, char *buf, int n){
  
  int nread;

  if((nread=read(fd, buf, n)) < 0){
    perror("Reading data");
    exit(1);
  }
  return nread;
}

/**************************************************************************
 * cwrite: write routine that checks for errors and exits if an error is  *
 *         returned.                                                      *
 **************************************************************************/
int cwrite(int fd, char *buf, int n){
  
  int nwritten;

  if((nwritten = write(fd, buf, n)) < 0){
    perror("Writing data");
    exit(1);
  }
  return nwritten;
}

/**************************************************************************
 * read_n: ensures we read exactly n bytes, and puts them into "buf".     *
 *         (unless EOF, of course)                                        *
 **************************************************************************/
int read_n(int fd, char *buf, int n) {

  int nread, left = n;

  while(left > 0) {
    if ((nread = cread(fd, buf, left)) == 0){
      return 0 ;      
    }else {
      left -= nread;
      buf += nread;
    }
  }
  return n;  
}

/**************************************************************************
 * do_debug: prints debugging stuff (doh!)                                *
 **************************************************************************/
void do_debug(int level, char *msg, ...){
  
  va_list argp;
  
  if( debug >= level ) {
	va_start(argp, msg);
	vfprintf(stderr, msg, argp);
	va_end(argp);
  }
}

/**************************************************************************
 * my_err: prints custom error messages on stderr.                        *
 **************************************************************************/
void my_err(char *msg, ...) {

  va_list argp;
  
  va_start(argp, msg);
  vfprintf(stderr, msg, argp);
  va_end(argp);
}

/**************************************************************************
 * usage: prints usage and exits.                                         *
 **************************************************************************/
void usage(void) {
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "%s -i <ifacename> [-c <peerIP>] [-p <port>] [-u|-a] [-d <debug_level>] [-r] [-n <num_mux_tap>] [-b <num_bytes_threshold>] [-t <timeout (microsec)>] [-P <period (microsec)>] [-l <log file name>] [-L]\n" , progname);
  fprintf(stderr, "%s -h\n", progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "-i <ifacename>: Name of tun/tap interface to use (mandatory)\n");
  fprintf(stderr, "-e <ifacename>: Name of local interface to use (mandatory)\n");
  fprintf(stderr, "-c <peerIP>: specify peer destination address (-d <peerIP>) (mandatory)\n");
  fprintf(stderr, "-p <port>: port to listen on, and to connect to (default 55555)\n");
  fprintf(stderr, "-u|-a: use TUN (-u, default) or TAP (-a)\n");
  fprintf(stderr, "-d: outputs debug information while running. 0:no debug; 1:minimum debug; 2:medium debug; 3:maximum debug (incl. ROHC)\n");
  fprintf(stderr, "-r: compresses and decompresses headers using ROHC\n");
  fprintf(stderr, "-n: number of packets received, to be sent to the network at the same time, default 1, max 100\n");
  fprintf(stderr, "-b: size threshold (bytes) to trigger the departure of packets, default 1472 (1500 - 28)\n");
  fprintf(stderr, "-t: timeout (in usec) to trigger the departure of packets\n");
  fprintf(stderr, "-P: period (in usec) to trigger the departure of packets. If ( timeout < period ) then the timeout has no effect\n");
  fprintf(stderr, "-l: log file name\n");
  fprintf(stderr, "-L: use default log file name (day and hour Y-m-d_H.M.S)");
  fprintf(stderr, "-h: prints this help text\n");
  exit(1);
}

/**************************************************************************
 * GetTimeStamp: Get a timestamp in microseconds from the OS              *
 **************************************************************************/
uint64_t GetTimeStamp() {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec*(uint64_t)1000000+tv.tv_usec;
}


/**************************************************************************
 * ToByte: convert an array of booleans to a char                         *
 **************************************************************************/
// example:
/* char c;
// bits[0] is the less significant bit
bool bits[8]={false, true, false, true, false, true, false, false}; is character '*': 00101010
c = ToByte(bits);
do_debug(1, "%c\n",c );
// prints an asterisk*/
unsigned char ToByte(bool b[8]) 
{
	int i;
    unsigned char c = 0;
    for (i=0; i < 8; ++i)
        if (b[i])
            c |= 1 << i;
    return c;
}


/**************************************************************************
 * FromByte: return an array of booleans from a char                      *
 **************************************************************************/
// stores in 'b' the value 'true' or 'false' depending on each bite of the byte c
// b[0] is the less significant bit
// example: print the byte corresponding to an asterisk
/*bool bits2[8];
FromByte('*', bits2);
do_debug(1, "byte:%c%c%c%c%c%c%c%c\n", bits2[0], bits2[1], bits2[2], bits2[3], bits2[4], bits2[5], bits2[6], bits2[7]);
if (bits2[4]) {
	do_debug(1, "1\n");
} else {
	do_debug(1, "0\n");
}*/
void FromByte(unsigned char c, bool b[8])
{
	int i;
    for (i=0; i < 8; ++i)
        b[i] = (c & (1<<i)) != 0;
}


/**************************************************************************
 * PrintByte: prints the bits of a byte                                   *
 **************************************************************************/
void PrintByte(int debug_level, bool b[8])
{
	int i;
	for (i=7; i>=0; i--) {
		if (b[i]) {
			do_debug(debug_level, "1");
		} else {
			do_debug(debug_level, "0");
		}
	}
}

/************ Prototypes of functions used in the program ****************/

static int gen_random_num(const struct rohc_comp *const comp, void *const user_context);

//time_t time(time_t* timer);

//struct tm *localtime(const time_t *);



/**
 * @brief Callback to print traces of the ROHC library
 *
 * @param priv_ctxt  An optional private context, may be NULL
 * @param level      The priority level of the trace
 * @param entity     The entity that emitted the trace among:
 *                    \li ROHC_TRACE_COMP
 *                    \li ROHC_TRACE_DECOMP
 * @param profile    The ID of the ROHC compression/decompression profile
 *                   the trace is related to
 * @param format     The format string of the trace
 */
static void print_rohc_traces(void *const priv_ctxt,
                              const rohc_trace_level_t level,
                              const rohc_trace_entity_t entity,
                              const int profile,
                              const char *const format,
                              ...)
{
	// Only print ROHC messages if debug level is > 2
	if ( debug > 2 ) {
        va_list args;
        va_start(args, format);
        vfprintf(stdout, format, args);
        va_end(args);
	}
}


/* return an string with the date and the time in format %Y-%m-%d_%H.%M.%S*/
int date_and_time(char buffer[25])
{
	time_t timer;
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);
	strftime(buffer, 25, "%Y-%m-%d_%H.%M.%S", tm_info);
    return EXIT_SUCCESS;
}


/**************************************************************************
 ************************ main program ************************************
 **************************************************************************/
int main(int argc, char *argv[]) {

	/******************* declare variables ******************/
	int tap_fd, option;
	int flags = IFF_TUN;			// to express if a tun or a tap is to be used
	char if_name[IFNAMSIZ] = "";
	char interface[IFNAMSIZ]= "";
	int maxfd;
	uint16_t nread_from_net;
	uint16_t size_packet_read_from_tap;
	 char packet_read_from_tap[BUFSIZE];			// stores a packet received from tap, before storing it or sending it to the network
	 char buffer_from_net[BUFSIZE];					// stores the packet received from the network, before sending it to tap
	 char muxed_packet[MTU];						// stores the multiplexed packet
	 char demuxed_packet[MTU];						// stores each demultiplexed packet
	struct sockaddr_in local, remote;				// these are structs for storing sockets
	socklen_t slen = sizeof(remote);				// size of the socket. The type is like an int, but adequate for the size of the socket
	char remote_ip[16] = "";            			// dotted quad IP string with the IP of the remote machine
	char local_ip[16] = "";                  		// dotted quad IP string with the IP of the local machine     
	unsigned short int port = PORT;					// UDP port to be used for sending the multiplexed packets
	int net_fd = 1;									// the file descriptor of the socket
	unsigned long int tap2net = 0, net2tap = 0;		// number of packets read from tap and from net
	struct ifreq iface;								// network interface
	int limit_numpackets_tap = 0;					// limit of the number of tap packets that can be stored. it has to be smaller than MAXPKTS
	int size_threshold = MAXTHRESHOLD;				// if the number of bytes stored is higher than this, they are sent
	uint64_t timeout = MAXTIMEOUT;					// (microseconds) if a packet arrives and the timeout has expired (time from the  
													// previous sending), the sending is triggered. default 100 seconds
	uint64_t period= MAXTIMEOUT;					// period. If it expires, a packet is sent
	uint64_t microseconds_left = period;			// the time until the period expires	

	uint64_t time_last_sent_in_microsec, time_in_microsec, time_difference;		// very long unsigned integers for storing the system clock in microseconds

	int num_pkts_stored_from_tap = 0;				// number of packets received and not sent from tun/tap (stored)
	int size_muxed_packet = 0;						// acumulated size of the multiplexed packet
	int predicted_size_muxed_packet;				// size of the muxed packet if the arrived packet was added to it
	int l,j;
	int position;									// for reading the arrived multiplexed packet
	int packet_length;								// the length of each packet inside the multiplexed bundle
	int network_mtu;								// the maximum transfer unit of the interface
	int num_demuxed_packets;						// a counter of the number of packets inside a muxed one

    int ret;										// value returned by the "select" function
  	fd_set rd_set;									// rd_set is a set of file descriptors used to know which interface has received a packet

	struct timeval period_expires;					// it is used for the maximum time waiting for a new packet

	bool bits[8];									// it is used for printing the bits of a byte in debug mode


	// ROHC header compression variables
	int compress_headers = 0;						// it is 1 if the headers are to be compressed

	struct rohc_comp *compressor;           		// the ROHC compressor
	unsigned char ip_buffer[BUFSIZE];				// the buffer that will contain the IPv4 packet to compress
	struct rohc_buf ip_packet = rohc_buf_init_empty(ip_buffer, BUFSIZE);	
	unsigned char rohc_buffer[BUFSIZE];				// the buffer that will contain the resulting ROHC packet
	struct rohc_buf rohc_packet = rohc_buf_init_empty(rohc_buffer, BUFSIZE);
	unsigned int seed;
	rohc_status_t status;

	struct rohc_decomp *decompressor;       		// the ROHC decompressor
	unsigned char ip_buffer_d[BUFSIZE];				// the buffer that will contain the resulting IP decompressed packet
	struct rohc_buf ip_packet_d = rohc_buf_init_empty(ip_buffer_d, BUFSIZE);
	unsigned char rohc_buffer_d[BUFSIZE];			// the buffer that will contain the ROHC packet to decompress
	struct rohc_buf rohc_packet_d = rohc_buf_init_empty(rohc_buffer_d, BUFSIZE);

    /* we do not want to handle feedback */
    struct rohc_buf *rcvd_feedback = NULL;
    struct rohc_buf *feedback_send = NULL;

	/* variables for the log file */
	char log_file_name[100] = "";            		// name of the log file	
	FILE *log_file = NULL;							// file descriptor of the log file


	/************** Check command line options *********************/
	progname = argv[0];		// argument used when calling the program

	while((option = getopt(argc, argv, "i:e:c:p:n:b:t:P:l:d:uahrL")) > 0) {
	    switch(option) {
			case 'd':
				debug = atoi(optarg);	/* 0:no debug; 1:minimum debug; 2:medium debug; 3:maximum debug (incl. ROHC) */
				break;
			case 'r':
				compress_headers = 1;
				break;
			case 'h':					/*help*/
				usage();
				break;
			case 'i':					/* put the name of the tun/tap interface (e.g. "tun2") in "if_name" */
				strncpy(if_name, optarg, IFNAMSIZ-1);
				break;
			case 'e':					/* the name of the network interface (e.g. "eth0") in "interface" */
				strncpy(interface, optarg, IFNAMSIZ-1);
				break;
			case 'c':					/* destination address of the machine where the tunnel ends */
				strncpy(remote_ip, optarg, 15);
				break;
			case 'l':					/* name of the log file */
				strncpy(log_file_name, optarg, 100);
				break;
			case 'L':					/* name of the log file assigned automatically */
				date_and_time(log_file_name);
				break;
			case 'p':					/* port number */
				port = atoi(optarg);	/* atoi Parses a string interpreting its content as an int */
				break;
			case 'u':					/* use a TUN device */
				flags = IFF_TUN;
				break;
			case 'a':					/* use a TAP device */
				flags = IFF_TAP;
				break;
			case 'n':					/* limit of the number of packets for triggering a muxed packet */
				limit_numpackets_tap = atoi(optarg);
				break;
			case 'b':					/* size threshold (in bytes) for triggering a muxed packet */
				size_threshold = atoi(optarg);
				break;
			case 't':					/* timeout for triggering a muxed packet */
				timeout = atof(optarg);
				break;
			case 'P':					/* Period for triggering a muxed packet */
				period = atof(optarg);
				break;
			default:
				my_err("Unknown option %c\n", option);
				usage();
    	}
	}	//end while option

	argv += optind;
	argc -= optind;

	if(argc > 0) {
		my_err("Too many options\n");
		usage();
	}

	/* check the rest of the options */
	if(*if_name == '\0') {
		my_err("Must specify tun/tap interface name\n");
		usage();
	} else if(*remote_ip == '\0') {
		my_err("Must specify the address of the peer\n");
		usage();
	} else if(*interface == '\0') {
		my_err("Must specify local interface name\n");
		usage();
	}

	/* open the log file */
	if ( log_file_name != '\0' ) {
		log_file = fopen(log_file_name, "w");
		if (log_file == NULL) my_err("Error opening file!\n");
	}


	// check debug option
	if ( debug < 0 ) debug = 0;
	if ( debug > 3 ) debug = 3;
	do_debug ( 1 , "debug level set to %i\n", debug);


	/*** set the triggering parameters according to user selections (or default values) ***/
	
	// there are four possibilities for triggering the sending of the packets:
	// - a threshold of the acumulated packet size
	// - a number of packets
	// - a timeout. A packet arrives. If the timeout has been reached, a muxed packet is triggered
	// - a period. If the period has been reached, a muxed packet is triggered

	// if ( timeout < period ) then the timeout has no effect
	// as soon as one of the conditions is accomplished, all the accumulated packets are sent

	if (( (size_threshold < MAXTHRESHOLD) || (timeout < MAXTIMEOUT) || (period < MAXTIMEOUT) ) && (limit_numpackets_tap == 0)) limit_numpackets_tap = MAXPKTS;

	// if no option is set by the user, it is assumed that every packet will be sent immediately
	if (( (size_threshold == MAXTHRESHOLD) && (timeout == MAXTIMEOUT) && (timeout == MAXTIMEOUT)) && (limit_numpackets_tap == 0)) limit_numpackets_tap = 1;
	

	// I calculate now as the moment of the last sending
	time_last_sent_in_microsec = GetTimeStamp() ; 

	do_debug(1, "threshold: %i. numpackets: %i.timeout: %.2lf\n", size_threshold, limit_numpackets_tap, timeout);


	/*** initialize tun/tap interface ***/
	if ( (tap_fd = tun_alloc(if_name, flags | IFF_NO_PI)) < 0 ) {
		my_err("Error connecting to tun/tap interface %s\n", if_name);
		exit(1);
	}
	do_debug(1, "Successfully connected to interface %s\n", if_name);


	/*** Request a socket ***/
	// AF_INET (exactly the same as PF_INET)
	// transport_protocol: 	SOCK_DGRAM creates a UDP socket (SOCK_STREAM would create a TCP socket)	
	// net_fd is the file descriptor of the socket			
  	if ( ( net_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) ) < 0) {
    	perror("socket()");
    	exit(1);
  	}


    /*** assign the destination address ***/
    memset(&remote, 0, sizeof(remote));

    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = inet_addr(remote_ip);
    remote.sin_port = htons(port);

	// Use ioctl() to look up interface index which we will use to
	// bind socket descriptor net_fd to specified interface with setsockopt() since
	// none of the other arguments of sendto() specify which interface to use.
	memset (&iface, 0, sizeof (iface));
	snprintf (iface.ifr_name, sizeof (iface.ifr_name), "%s", interface);
	if (ioctl (net_fd, SIOCGIFINDEX, &iface) < 0) {
		perror ("ioctl() failed to find interface ");
		return (EXIT_FAILURE);
	}


	/*** get the IP address of the local interface ***/
	if (ioctl(net_fd, SIOCGIFADDR, &iface) < 0) {
		perror ("ioctl() failed to find the IP address for local interface ");
		return (EXIT_FAILURE);
	}


	/*** get the MTU of the local interface ***/
	if (ioctl(net_fd, SIOCGIFMTU, &iface) == -1) network_mtu = 0;
	else network_mtu = iface.ifr_mtu;
	do_debug(1, "MTU: %i\n", network_mtu);
	if (network_mtu > MTU) perror("predefined MTU is higher than the one in the network");


	// create the sockets for sending packets to the network
    // assign the local address. Source IPv4 address: it is the one of the interface
    strcpy (local_ip, inet_ntoa(((struct sockaddr_in *)&iface.ifr_addr)->sin_addr));
	
	// create the socket for sending multiplexed packets (with separator)
    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    //local.sin_addr.s_addr = htonl(INADDR_ANY); // this would take any interface
   	local.sin_addr.s_addr = inet_addr(local_ip);
    local.sin_port = htons(port);

 	if (bind(net_fd, (struct sockaddr *)&local, sizeof(local))==-1) perror("bind");

    do_debug(1, "Socket open. Remote IP  %s. Port %i. ", inet_ntoa(remote.sin_addr), port); 
    do_debug(1, "Local IP %s\n", inet_ntoa(local.sin_addr));    

	

  	/*** use select() to handle two interface descriptors at once ***/
  	maxfd = (tap_fd > net_fd)?tap_fd:net_fd;
	do_debug(1, "tap_fd: %i; net_fd: %i;\n",tap_fd, net_fd);

	if ( compress_headers == 1 ) {

		/* initialize the random generator */
		seed = time(NULL);
		srand(seed);

		/* Create a ROHC compressor with small CIDs and the largest MAX_CID
		 * possible for small CIDs */
		do_debug(1, "create the ROHC compressor\n");

		//! [create ROHC compressor]
		compressor = rohc_comp_new2(ROHC_SMALL_CID, ROHC_SMALL_CID_MAX, gen_random_num, NULL);
		if(compressor == NULL)
		{
			fprintf(stderr, "failed create the ROHC compressor\n");
			goto error;
		}

		/* Enable the compression profiles you need */
		do_debug(1, "enable several ROHC compression profiles\n");
		if(!rohc_comp_enable_profile(compressor, ROHC_PROFILE_UNCOMPRESSED))
		{
			fprintf(stderr, "failed to enable the Uncompressed profile\n");
			goto release_compressor;
		}
		if(!rohc_comp_enable_profile(compressor, ROHC_PROFILE_IP))
		{
			fprintf(stderr, "failed to enable the IP-only profile\n");
			goto release_compressor;
		}
		if(!rohc_comp_enable_profiles(compressor, ROHC_PROFILE_UDP, ROHC_PROFILE_UDPLITE, -1))
		{
			fprintf(stderr, "failed to enable the IP/UDP and IP/UDP-Lite profiles\n");
			goto release_compressor;
		}
		if(!rohc_comp_enable_profile(compressor, ROHC_PROFILE_TCP))
		{
			fprintf(stderr, "failed to enable the TCP profile\n");
			goto release_compressor;
		}

        /* Create a ROHC decompressor to operate:
         *  - with large CIDs,
         *  - with the maximum of 5 streams (MAX_CID = 4),
         *  - in Bidirectional Optimistic mode (O-mode) (use ROHC_U_MODE for Unidirectional mode (U-mode).    */
        decompressor = rohc_decomp_new2 (ROHC_SMALL_CID, ROHC_SMALL_CID_MAX, ROHC_O_MODE);
        if(decompressor == NULL)
        {
                fprintf(stderr, "failed create the ROHC decompressor\n");
                goto release_decompressor;
        }

		// enable rohc decompression profiles
		status = rohc_decomp_enable_profiles(decompressor,
                                     ROHC_PROFILE_UNCOMPRESSED,
                                     ROHC_PROFILE_UDP,
                                     ROHC_PROFILE_IP,
                                     ROHC_PROFILE_UDPLITE,
                                     ROHC_PROFILE_RTP,
                                     ROHC_PROFILE_ESP,
                                     ROHC_PROFILE_TCP, -1);
		if(!status)
		{
    		fprintf(stderr, "failed to enable the decompression profiles\n");
            goto release_decompressor;
		}

		// set the function that will manage the ROHC compressing traces (it will be 'print_rohc_traces')
        if(!rohc_comp_set_traces_cb2(compressor, print_rohc_traces, NULL))
        {
                fprintf(stderr, "failed to set the callback for traces on compressor\n");
                goto release_compressor;
        }

		// set the function that will manage the ROHC decompressing traces (it will be 'print_rohc_traces')
		if(!rohc_decomp_set_traces_cb2(decompressor, print_rohc_traces, NULL))
        {
                fprintf(stderr, "failed to set the callback for traces on decompressor\n");
                goto release_decompressor;
        }
	}


	// Main loop
  	while(1) {

   		FD_ZERO(&rd_set);			/* FD_ZERO() clears a set */
   		FD_SET(tap_fd, &rd_set);	/* FD_SET() adds a given file descriptor to a set */
		FD_SET(net_fd, &rd_set);

		/* Initialize the timeout data structure. */
		time_in_microsec = GetTimeStamp();
		if ( period > (time_in_microsec - time_last_sent_in_microsec)) {
			microseconds_left = (period - (time_in_microsec - time_last_sent_in_microsec));			
		} else {
			microseconds_left = 0;
		}
		// do_debug (1, "microseconds_left: %i\n", microseconds_left);

		period_expires.tv_sec = 0;
		period_expires.tv_usec = microseconds_left;		// this is the moment when the period will expire


    	/* select () allows a program to monitor multiple file descriptors, */ 
		/* waiting until one or more of the file descriptors become "ready" */
		/* for some class of I/O operation*/
		ret = select(maxfd + 1, &rd_set, NULL, NULL, &period_expires); 	//this line stops the program until something
																		//happens or the period expires

		// if the program gets here, it means that a packet has arrived (from tun/tap or from the network), or the period has expired
    	if (ret < 0 && errno == EINTR) continue;

    	if (ret < 0) {
      		perror("select()");
      		exit(1);
    	}

		/***************** NET to TAP. demux and decompress **************************/

    	/*** data arrived from the network: read it, and write it to the tun/tap interface. ***/
    	if(FD_ISSET(net_fd, &rd_set)) {		/* FD_ISSET tests to see if a file descriptor is part of the set */

	  		// received a packet from the network. slen is the length of the IP address
			nread_from_net = recvfrom ( net_fd, buffer_from_net, BUFSIZE, 0, (struct sockaddr *)&remote, &slen );
			if (nread_from_net==-1) perror ("recvfrom()");

	  		/* increase the counter of the number of packets read from the network */
      		net2tap++;
	  		do_debug(1, "NET2TAP %lu: Read muxed packet (%i bytes) from %s:%d\n", net2tap, nread_from_net, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));


			// now buffer_from_net contains a full packet or frame.
			// check if the packet comes from the multiplexing port
			if (port == ntohs(remote.sin_port)) {
				// write the log file
				if ( log_file != NULL ) {
					fprintf (log_file, "%"PRIu64"\trec\tmuxed\t%i\t%lu\tfrom\t%s\t%d\n", GetTimeStamp(), nread_from_net, net2tap, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
					fflush(log_file);	// If the IO is buffered, I have to insert fflush(fp) after the write in order to avoid things lost when pressing Ctrl+C.
				}

				// if the packet comes from the multiplexing port, I have to demux it and write each packet to the tun/tap interface
				position = 0; //this is the index for reading the packet/frame
				num_demuxed_packets = 0;

				while (position < nread_from_net) {
					// the first thing I expect is a Mux separator. Check if the first bit is 0. Otherwise, the separator is bad
					FromByte(buffer_from_net[position], bits);
					if (bits[7]) {
						//bad packet. The first bit of the separator is 0
						do_debug(1, " NET2TAP: bad multiplexed packet received. Begins with: %02x. Packet dropped\n", buffer_from_net[position]);
						position = nread_from_net;

						// write the log file
						if ( log_file != NULL ) {
							fprintf (log_file, "%"PRIu64"\terror\tbad_separator\t%i\t%lu\n", GetTimeStamp(), nread_from_net, net2tap );	// the packet is not good
							fflush(log_file);
						}
						
					} else {
						num_demuxed_packets ++;
						//do_debug(2, "\n");
						do_debug(1, " NET2TAP: packet #%i demuxed", num_demuxed_packets);
						if ((debug == 1) && (compress_headers == 0) ) do_debug (1,"\n");
						do_debug(2, ": ");

						// Check the second bit. 
						if (bits[6]== false) {
							// if the second bit is 0, it means that the separator is one-byte
							// I have to convert the six less significant bits to an integer, which means the length of the packet
							// since the two most significant bits are 0, the length is the value of the char
							packet_length = buffer_from_net[position] % 128;
							do_debug(2, " Mux separator:(%02x) ", buffer_from_net[position]);
							PrintByte(2, bits);

							position = position + 1;
						} else {
							// if the second bit is 1, it means that the separator is two bytes
							// I get the six less significant bits by using modulo 64
							// I do de product by 256 and add the resulting number to the second byte
							packet_length = ((buffer_from_net[position] % 64) * 256 ) + buffer_from_net[position+1];
							if (debug ) {
								do_debug(2, " Mux separator:(%02x) ", buffer_from_net[position]);
								PrintByte(2, bits);
								FromByte(buffer_from_net[position+1], bits);
								do_debug(2, " (%02x) ",buffer_from_net[position+1]);
								PrintByte(2, bits);	
							}					
							position = position + 2;
						}
						do_debug(2, " (%i bytes)\n", packet_length);

						// copy the packet to a new string
						for (l = 0; l < packet_length ; l++) {
							demuxed_packet[l] = buffer_from_net[position];
							position ++;
						}

						// Check if the position has gone beyond the size of the packet
						if (position > nread_from_net) {
							// The last length read from the separator goes beyond the end of the packet
							do_debug (1, "  The length of the packet does not fit. Packet discarded\n");

							// write the log file
							if ( log_file != NULL ) {
								fprintf (log_file, "%"PRIu64"\terror\tdemux_bad_length\t%i\t%lu\n", GetTimeStamp(), nread_from_net, net2tap );	// the packet is bad so I add a line
								fflush(log_file);
							}
							
						} else {
							/************ decompress the packet ***************/
							if ( compress_headers == 1 ) {

							// reset the buffer where the rohc and ip packets are to be stored
							rohc_buf_reset (&ip_packet_d);
							rohc_buf_reset (&rohc_packet_d);

							// Copy the compressed length and the compressed packet
							rohc_packet_d.len = packet_length;
							
							for (l = 0; l < packet_length ; l++) {
								rohc_buf_byte_at(rohc_packet_d, l) = demuxed_packet[l];
							}

							// dump the ROHC packet on terminal
							if (debug) {
								do_debug(2, " ");
								do_debug(1, " ROHC ");
								do_debug(2, "packet\n   ");
								for(j = 0; j < rohc_packet_d.len; j++)
								{
									do_debug(2, "%02x ", rohc_buf_byte_at(rohc_packet_d, j));
									if(j != 0 && ((j + 1) % 16) == 0)
									{
										do_debug(2, "\n");
										if ( j != (ip_packet_d.len -1 )) do_debug(2,"   ");
									}
									// separate in groups of 8 bytes
									else if((j != 0 ) && ((j + 1) % 8 == 0 ) && (( j + 1 ) % 16 != 0))
									{
										do_debug(2, "  ");
									}
								}
								if(j != 0 && ((j ) % 16) != 0) /* be sure to go to the line */
								{
									do_debug(2, "\n");
								}
							}

							// decompress the packet
							status = rohc_decompress3 (decompressor, rohc_packet_d, &ip_packet_d, rcvd_feedback, feedback_send);
							if(status == ROHC_STATUS_OK) {
								/* decompression is successful */

									if(!rohc_buf_is_empty(ip_packet_d))	// packet is not empty
									{
										// ip_packet.len bytes of decompressed IP data available in ip_packet
										packet_length = ip_packet_d.len;

										/* copy the packet */
										memcpy(demuxed_packet, rohc_buf_data_at(ip_packet_d, 0), packet_length);

										//dump the IP packet on the standard output
										do_debug(2, "  ");
										do_debug(1, "IP packet resulting from the ROHC decompression (%i bytes) written to TAP\n", packet_length);
										do_debug(2, "   ");

										if (debug) {

											/* dump the decompressed IP packet on terminal */
											for(j = 0; j < ip_packet_d.len; j++)
											{
												do_debug(2, "%02x ", rohc_buf_byte_at(ip_packet_d, j));
												if(j != 0 && ((j + 1) % 16) == 0)
												{
													do_debug(2, "\n");
													if ( j != (ip_packet_d.len -1 )) do_debug(2,"   ");
												}
												// separate in groups of 8 bytes
												else if((j != 0 ) && ((j + 1) % 8 == 0 ) && (( j + 1 ) % 16 != 0))
												{
													do_debug(2, "  ");
												}
											}
											if(j != 0 && ((j ) % 16) != 0) /* be sure to go to the line */
											{
												do_debug(2, "\n");
											}
										}
									}
									else
									{
										/* no IP packet was decompressed because of ROHC segmentation or
										 * feedback-only packet:
										 *  - the ROHC packet was a non-final segment, so at least another
										 *    ROHC segment is required to be able to decompress the full
										 *    ROHC packet
										 *  - the ROHC packet was a feedback-only packet, it contained only
										 *    feedback information, so there was nothing to decompress */
										do_debug(1, "  no IP packet decompressed\n");

										// write the log file
										if ( log_file != NULL ) {
											fprintf (log_file, "%"PRIu64"\trec\tROHC_feedback\t%i\t%lu\tfrom\t%s\t%d\n", GetTimeStamp(), nread_from_net, net2tap, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));	// the packet is bad so I add a line
											fflush(log_file);
										}
									}
								}
								else
								{
									/* failure: decompressor failed to decompress the ROHC packet */
									do_debug(2, "  decompression of ROHC packet failed\n");
									fprintf(stderr, "  decompression of ROHC packet failed\n");

									// write the log file
									if ( log_file != NULL ) {
										fprintf (log_file, "%"PRIu64"\terror\tdecomp_failed\t%i\t%lu\n", GetTimeStamp(), nread_from_net, net2tap);	// the packet is bad
										fflush(log_file);
									}
								}


							} /*********** end decompression **************/

							// write the demuxed (and perhaps decompressed) packet to the tun/tap interface
							// if compression is used, check that ROHC has decompressed correctly
							if ( ( compress_headers == 0 ) || ((compress_headers == 1) && ( status == ROHC_STATUS_OK))) {
								cwrite ( tap_fd, demuxed_packet, packet_length );

								// write the log file
								if ( log_file != NULL ) {
									fprintf (log_file, "%"PRIu64"\tsent\tdemuxed\t%i\t%lu\n", GetTimeStamp(), packet_length, net2tap);	// the packet is good
									fflush(log_file);
								}
							}
						}
					}
				}

			} else {
				// if the packet does not come from the multiplexing port, write it directly into the tun/tap interface
				cwrite ( tap_fd, buffer_from_net, nread_from_net);
				do_debug(1, "NET2TAP %lu: Non-multiplexed-packet. Written %i bytes to tap\n", net2tap, nread_from_net);

				// write the log file
				if ( log_file != NULL ) {
					fprintf (log_file, "%"PRIu64"\tforward\tnative\t%i\t%lu\tfrom\t%s\t%d\n", GetTimeStamp(), nread_from_net, net2tap, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));	// the packet is good
					fflush(log_file);
				}
			}
  		}

		/***************** TAP to NET: compress and multiplex **************************/

    	/*** data arrived from tun/tap: read it, and check if the stored packets should be written to the network ***/	
    	else if(FD_ISSET(tap_fd, &rd_set)) {		/* FD_ISSET tests to see if a file descriptor is part of the set */

	  		/* read the packet from tun/tap, store it in the array 'packet_read_from_tap', and store its size */
      		size_packet_read_from_tap = cread (tap_fd, packet_read_from_tap, BUFSIZE);
		
	  		/* increase the counter of the number of packets read from tun/tap*/
      		tap2net++;
			if (debug > 1 ) do_debug (2,"\n");
      		do_debug(1, "TAP2NET %lu: Read packet (%i bytes) from tap. ", tap2net, size_packet_read_from_tap);

			// write the log file
			if ( log_file != NULL ) {
				fprintf (log_file, "%"PRIu64"\trec\tnative\t%i\t%lu\n", GetTimeStamp(), size_packet_read_from_tap, tap2net);
				fflush(log_file);	// If the IO is buffered, I have to insert fflush(fp) after the write in order to avoid things lost when pressing
			}

			/******************** compress the headers if the option has been set ****************/
			if ( compress_headers == 1 ) {

				// copy the length read from tap to the buffer where the packet to be compressed is stored
				ip_packet.len = size_packet_read_from_tap;

				// copy the packet
				memcpy(rohc_buf_data_at(ip_packet, 0), packet_read_from_tap, size_packet_read_from_tap);

				if (debug) {
					do_debug(2, "\n   ");
					/* dump the newly-created IP packet on terminal */
					for(j = 0; j < ip_packet.len; j++)
					{
						do_debug(2, "%02x ", rohc_buf_byte_at(ip_packet, j));
						if(j != 0 && ((j + 1) % 16) == 0)
						{
							do_debug(2, "\n");
							if ( j != (ip_packet_d.len -1 )) do_debug(2,"   ");
						}
						// separate in groups of 8 bytes
						else if((j != 0 ) && ((j + 1) % 8 == 0 ) && (( j + 1 ) % 16 != 0))
						{
							do_debug(2, "  ");
						}
					}
					if(j != 0 && ((j ) % 16) != 0) /* be sure to go to the line */
					{
						do_debug(2, "\n");
					}
				}

				/* Now, compress this IP packet */
				// reset the buffer where the rohc packet is to be stored
				rohc_buf_reset (&rohc_packet);

				// compress IP packet
				status = rohc_compress4(compressor, ip_packet, &rohc_packet);

				if(status == ROHC_STATUS_SEGMENT) {
					/* success: compression succeeded, but resulting ROHC packet was too
					 * large for the Maximum Reconstructed Reception Unit (MRRU) configured
					 * with \ref rohc_comp_set_mrru, the rohc_packet buffer contains the
					 * first ROHC segment and \ref rohc_comp_get_segment can be used to
					 * retrieve the next ones. */
				} else if(status == ROHC_STATUS_OK)	{
					/* success: compression succeeded, and resulting ROHC packet fits the
					* Maximum Reconstructed Reception Unit (MRRU) configured with
					* \ref rohc_comp_set_mrru, the rohc_packet buffer contains the
					* rohc_packet_len bytes of the ROHC packet */

					if (debug) {
						/* dump the ROHC packet on terminal */
						do_debug(2, "  ROHC packet resulting from the ROHC compression:\n   ");
						for(j = 0; j < rohc_packet.len; j++)
						{
							do_debug(2, "%02x ", rohc_buf_byte_at(rohc_packet, j));
							if(j != 0 && ((j + 1) % 16) == 0)
							{
								do_debug(2, "\n");
								if ( j != (ip_packet_d.len -1 )) do_debug(2,"   ");
							}
							// separate in groups of 8 bytes
							else if((j != 0 ) && ((j + 1) % 8 == 0 ) && (( j + 1 ) % 16 != 0))
							{
								do_debug(2, "  ");
							}
						}
						if(j != 0 && ((j ) % 16) != 0)  /* be sure to go to the line */
						{
							do_debug(2, "\n");
						}
					}
				}
				else
				{
					/* compressor failed to compress the IP packet */
					fprintf(stderr, "compression of IP packet failed\n");
					if ( log_file != NULL ) {
						fprintf (log_file, "%"PRIu64"\terror\tcompr_failed\t%i\t%lu\\n", GetTimeStamp(), size_packet_read_from_tap, tap2net);
						fflush(log_file);	// If the IO is buffered, I have to insert fflush(fp) after the write in order to avoid things lost when pressing
					}
					//goto release_compressor;
				}

				// Copy the compressed length and the compressed packet
				size_packet_read_from_tap = rohc_packet.len;
				for (l = 0; l < size_packet_read_from_tap ; l++) {
					packet_read_from_tap[l] = rohc_buf_byte_at(rohc_packet, l);
				}

			} /*************** end compression ***************/

			if (size_packet_read_from_tap < 64 ) {
				predicted_size_muxed_packet = size_muxed_packet + 1 + size_packet_read_from_tap;
			} else {
				predicted_size_muxed_packet = size_muxed_packet + 2 + size_packet_read_from_tap;
			}

			// if this packet was muxed, the MTU would be overriden. So I first empty the buffer
			if (predicted_size_muxed_packet > MTU ) {
	      		do_debug(1, " MTU reached. Sending muxed packet without this one (%i bytes).", size_muxed_packet);

				if (sendto(net_fd, muxed_packet, size_muxed_packet, 0, (struct sockaddr *)&remote, sizeof(remote))==-1) perror("sendto()");

				// write the log file
				if ( log_file != NULL ) {
					fprintf (log_file, "%"PRIu64"\tsent\tmuxed\t%i\t%lu\tto\t%s\t%d\t%i\tMTU\n", GetTimeStamp(), size_muxed_packet, tap2net, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port), num_pkts_stored_from_tap);
					fflush(log_file);	// If the IO is buffered, I have to insert fflush(fp) after the write in order to avoid things lost when pressing
				}

				size_muxed_packet = 0;
				num_pkts_stored_from_tap = 0;
			}


			// I accumulate this packet in the buffer
			num_pkts_stored_from_tap ++;

			// I have to add the packet length separator. It is 1 byte if the length is smaller than 64. 
			// it is 2 bytes if the lengh is 64 or more
			if (size_packet_read_from_tap  < 64 ) {

				// add the length to the string. the MSB is always 0 (PFF field of Mux)
				// since the value is <64, the two most significant bits will always be 0
				muxed_packet[size_muxed_packet] = size_packet_read_from_tap;

				// print the first byte of the Mux separator
				if(debug) {
					FromByte(muxed_packet[size_muxed_packet], bits);
					do_debug(2, " Mux separator:(%02x) ", muxed_packet[size_muxed_packet]);
					PrintByte(2, bits);
					do_debug(2, "\n");
				}
				size_muxed_packet ++;

			} else {
				// first byte of the Mux separator (MSB=0, PFF=1 and 6 bits with the most significant bytes of the length)
				// get the most significant byte by dividing by 256
				// add 64 in order to put a '1' in the second bit
				muxed_packet[size_muxed_packet] = (size_packet_read_from_tap / 256 ) + 64;

				// second byte: the 8 less significant bytes of the length. Use modulo
				muxed_packet[size_muxed_packet + 1] = size_packet_read_from_tap % 256;

				// print the two bytes
				if(debug) {
					FromByte(muxed_packet[size_muxed_packet], bits);
					do_debug(2, " Mux separator:(%02x) ",muxed_packet[size_muxed_packet]);
					PrintByte(2, bits);
					FromByte(muxed_packet[size_muxed_packet + 1], bits);
					do_debug(2, " (%02x) ",muxed_packet[size_muxed_packet + 1]);
					PrintByte(2, bits);
					do_debug(2, "\n");
				}	
				size_muxed_packet = size_muxed_packet + 2;
			}

			// I add the packet itself to the muxed packet
			for (l = 0; l < size_packet_read_from_tap ; l++) {
				muxed_packet[size_muxed_packet] = packet_read_from_tap[l];
				size_muxed_packet ++;
				// if you want to see the packet, uncomment the next line
				// do_debug("%02x ",muxed_packet[size_muxed_packet + l]);
			}
			if (debug == 1 ) do_debug (1,"\n");
			do_debug(1, " TAP2NET: Packet stopped and multiplexed: accumulated %i pkts (%i bytes).", num_pkts_stored_from_tap, size_muxed_packet);
			time_in_microsec = GetTimeStamp();
			time_difference = time_in_microsec - time_last_sent_in_microsec;		
			do_debug(1, " time since last trigger: %" PRIu64 " usec\n", time_difference);//PRIu64 is used for printing uint64_t numbers

			// if the packet limit or the size threshold or the MTU are reached, send all the stored packets to the network
			// do not worry about the MTU. if it is reached, a number of packets will be sent
			if ((num_pkts_stored_from_tap == limit_numpackets_tap) || (size_muxed_packet > size_threshold) || (time_difference > timeout )){
				// send all the packets
				if (debug) {
					do_debug(1, " TAP2NET**Sending triggered**. ");
					if (num_pkts_stored_from_tap == limit_numpackets_tap)
						do_debug(1, "num packet limit reached. ");
					if (size_muxed_packet > size_threshold)
						do_debug(1," size limit reached. ");
					if (time_difference > timeout)
						do_debug(1, "timeout reached. ");		
					do_debug(1, "Writing %i packets (%i bytes) to network\n", num_pkts_stored_from_tap, size_muxed_packet);						
				}
				if (sendto(net_fd, muxed_packet, size_muxed_packet, 0, (struct sockaddr *)&remote, sizeof(remote))==-1) perror("sendto()");

				// write the log file
				if ( log_file != NULL ) {
					fprintf (log_file, "%"PRIu64"\tsent\tmuxed\t%i\t%lu\tto\t%s\t%d\t%i", GetTimeStamp(), size_muxed_packet, tap2net, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port), num_pkts_stored_from_tap);
					if (num_pkts_stored_from_tap == limit_numpackets_tap)
						fprintf(log_file, "\tnumpacket_limit");
					if (size_muxed_packet > size_threshold)
						fprintf(log_file, "\tsize_limit");
					if (time_difference > timeout)
						fprintf(log_file, "\ttimeout");
					fprintf(log_file, "\n");
					fflush(log_file);	// If the IO is buffered, I have to insert fflush(fp) after the write in order to avoid things lost when pressing
				}

				size_muxed_packet = 0 ;
				num_pkts_stored_from_tap = 0;
				time_last_sent_in_microsec = time_in_microsec;
			}
    	} 


		/******************** Period expired: multiplex ****************************/
		// Check if there is something stored, and send it
		// since there is no new packet, here it is not necessary to compress anything
		else {
			time_in_microsec = GetTimeStamp();
			if ( num_pkts_stored_from_tap > 0 ) {
				// There are some packets stored
				time_difference = time_in_microsec - time_last_sent_in_microsec;		

				if (debug) {
					do_debug(1, "TAP2NET**Period expired. Sending triggered**. time since last trigger: %" PRIu64 " usec\n", time_difference);	
					do_debug(1, "Writing %i packets (%i bytes) to network\n", num_pkts_stored_from_tap, size_muxed_packet);						

				}
				if (sendto(net_fd, muxed_packet, size_muxed_packet, 0, (struct sockaddr *)&remote, sizeof(remote))==-1) perror("sendto()");

				// write the log file
				if ( log_file != NULL ) {
					fprintf (log_file, "%"PRIu64"\tsent\tmuxed\t%i\t%lu\tto\t%s\t%d\t%i\tperiod\n", GetTimeStamp(), size_muxed_packet, tap2net, inet_ntoa(remote.sin_addr), ntohs(remote.sin_port), num_pkts_stored_from_tap);	
				}

				size_muxed_packet = 0 ;
				num_pkts_stored_from_tap = 0;

			} else {
				// No packet arrived
				//do_debug(2, "Period expired. Nothing to be sent\n");
			}
			time_last_sent_in_microsec = time_in_microsec;
		}

  	}	// end while(1)
  
  	return(0);



/******* labels ************/
release_compressor:
	rohc_comp_free(compressor);

release_decompressor:
	rohc_decomp_free(decompressor);

error:
	fprintf(stderr, "an error occured during program execution, "
	        "abort program\n");
	if ( log_file_name != '\0' ) fclose (log_file);
	return 1;
}



static int gen_random_num(const struct rohc_comp *const comp,
                          void *const user_context)
{
	return rand();
}


