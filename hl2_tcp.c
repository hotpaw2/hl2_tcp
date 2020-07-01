//
//  hl2_tcp.c
//
#define VERSION "v.1.3.129" // 2020-06-29 01
//   macOS : clang -lm -lpthread -Os -o hl2_tcp hl2_tcp.c 
//   pi    :    cc -lm -lpthread -Os -o hl2_tcp hl2_tcp.c 
//
//  Serves IQ data using the rtl_tcp protocol
//    from an Hermes Lite 2 on port 1024
//    to iPv6 port 1234
//
//   initial version 2020-01-27  rhn 
//
//   Copyright 2017,2020 Ronald H Nicholson Jr. All Rights Reserved.
//   This code may only be redistributed under terms of 
//   the Mozilla Public License 2.0 plus Exhibit B (no copyleft exception)
//   See: https://www.mozilla.org/en-US/MPL/2.0/

// #define TX_OK
// #define SPECIAL_CMD_OK
// #define NO_MAIN

#ifdef __clang__
#endif

#define TITLE ("hl2_tcp ")
#define SOCKET_READ_TIMEOUT     ( 30.0 * 60.0 )    // 30 minutes in seconds
#define SAMPLE_BITS     ( 8)    // default to match rtl_tcp
// #define SAMPLE_BITS  (16)    // default to match rsp_tcp
// #define SAMPLE_BITS  (32)    // HPSDR 24-bit->float32 IQ data ?
#define GAIN8           (4096.0)    // default gain ?
#define TCP_PORT        (1234)      // default rtp_tcp server port
#define HERMES_PORT     (1024)        // UDP port

#define RING_BUFFER_ALLOCATION  (2L * 1024L * 1024L)  // 2MB
#define MAX_NUMBER_OF_START_COMMANDS     (8)
#define START_LOOP_DELAY         (50000)     // microseconds

#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <fcntl.h>

#include <pthread.h>
#include <sys/time.h>

#ifndef _UNISTD_H_
int usleep(unsigned long int usec);
#endif

#ifndef M_PI
#define M_PI (3.14159265358979323846264)
typedef void *caddr_t;
#endif

static void sighandler(int signum);
void *tcp_connection_handler(void);        // main thread
void *udp_rcv_thread_runner(void *param);    // on a pthread

int     discover_main(void);
void    hl2_stop(void);
void     print_hl2_stats(void) ;
void    setFilterEnables(void) ;

uint32_t hl2_ipAddr     =  0x7f000001;     // 127.0.0.1 == localhost
int portno              =  TCP_PORT;     //
int hl2_running         =  0;        // Metis response
int udp_running         =  0;        // udp socket live
int C0_addr             =  0;           //
volatile int hermes_rx_freq     =  14100000;    // 100k to 30M
volatile int hermes_tx_freq     =  0;        //
volatile int hermes_tx_offset   =  0;          //
volatile int hermes_lna_gain    =  19;        // LNA gain -12 to +48 dB
int hermes_tx_drive_level       =  0;        // 0 .. 15
int hermes_enable_power_amp     =  0;           //
int hermes_Q5_switch_ext_ptt_lp =  0;
int tx_gear_ratio               =  1;
int tx_gear_counter             =  0;
volatile int seqNum             =  0;        // tx sequence number

// Rx and Tx filters; default no HPF ?
int     n2adr_filter_rx         =  0;
int     n2adr_filter_tx         =  0;

int         sendErrorFlag       =  0;
int         sampleBits          =  SAMPLE_BITS;
int         sampleRates         =  1;
long int    totalSamples        =  0;
long        sampRate            =  192000; // 768000;
long        previousSRate       = -1;
int         gClientSocketID     = -1;

char        *ring_buffer_ptr    =  NULL;
int        decimateFlag         =  1;
int        decimateCntr         =  0;

volatile float gain0            =  GAIN8;

struct sigaction    sigact, sigign;

int   tcp_listen_sockfd         = -1;
static volatile int  do_exit    =  0;
float    acc_r              =  0.0;    // accumulated rounding
float    sMax               =  0.0;    // for debug
float    sMin               =  0.0;
int     sendblockcount      =  0;
int      threads_running    =  0;

char     *hl2_ip_string     =  "127.0.0.1";   // 0x7f000001 == localhost

// for TX_OK
long int tcp_tx_cmd         =  0;
int tx_key_down             =  0;        // Morse code key
int hl2_tx_on               =  0;        // tx ptt
int tx_delay                =  0;        // tx/ptt hold time
int last_key_down           =  0;
int     tx_param_x          =  0;
int     tx_param_w          =  0;

#ifdef TX_OK
extern void     tx_setup();
extern void     tx_cleanup();
extern int     get_tx_key(void) ;
extern void     tx_block_setup(int seqN);
extern void     get_tx_sample(int *tx_i, int *tx_q) ;
extern int     get_tx_drive(void) ;
extern int    get_tx_offset();
#else
void         tx_setup() {   return; }
void         tx_cleanup() { return; }
#define get_tx_key()         (0)
#define tx_block_setup(X)     // nop
#define get_tx_sample(X,Y)     // nop
#define get_tx_drive()         (0)
#define get_tx_offset()        (0)
#endif

#ifdef SPECIAL_CMD_OK
extern int      specialCommandCounter;
extern void     checkForSpecialCommands(int seqN); // for special commands
#else
int         specialCommandCounter    =  0;
void            checkForSpecialCommands(int n) { return; }
#endif

#ifdef DEBUG_FILE
char *dump_fname        =  DEBUG_FILE ;
#endif

char UsageString1[]
= "Usage: hl2_tcp -a hermes_IPaddr [-p local_port] [-b 8/16]";
char UsageString2[]
= "       hl2_tcp -d ";

void print_usage_and_exit()
{
    printf("%s\n", UsageString1);
    printf("%s\n", UsageString2);
    exit(0);
}

int hl2_tcp(void);

#ifndef NO_MAIN
// int tcp_main(int argc, char *argv[])
int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "-d") == 0) {
            int e = discover_main();
            return(e);
        }
        if ((argc % 2) != 1) {
            print_usage_and_exit();
        }
        for (int arg=3; arg<=argc; arg+=2) {
            if (strcmp(argv[arg-2], "-p") == 0) {
                portno = atoi(argv[arg-1]);
                if (portno == 0) {
                    printf("invalid port number entry %s\n", argv[arg-1]);
                    exit(0);
                }
            } else if (strcmp(argv[arg-2], "-b") == 0) {
                if (strcmp(argv[arg-1],"16") == 0) {
                    sampleBits = 16;
                } else if (strcmp(argv[arg-1],"8") == 0) {
                    sampleBits =  8;
                } else {
                    print_usage_and_exit();
                }
            } else if (strcmp(argv[arg-2], "-a") == 0) {
                hl2_ip_string = argv[arg-1];
            } else if (strcmp(argv[arg-2], "-x") == 0) {
                tx_param_x = atoi(argv[arg-1]);
            } else {
                print_usage_and_exit();
            }
        }
    } else {
        print_usage_and_exit();
    }
    
    return(hl2_tcp());
}
#endif

int hl2_tcp()
{
    struct sockaddr_in6 serv_addr ;
    char client_addr_ipv6[100];
    
    printf("hl2_tcp Version %s\n", VERSION);
    printf("Will look for Hermes Lite 2 at IP: %s UDP Port: %d \n",
           hl2_ip_string, HERMES_PORT);
    
    ring_buffer_ptr = (char *)malloc(RING_BUFFER_ALLOCATION + 4);
    if (ring_buffer_ptr == NULL) { exit(-1); }
    bzero(ring_buffer_ptr, RING_BUFFER_ALLOCATION + 2);
    
    printf("Converts Metis to rtl_tcp format %d-bit IQ samples \n",
           sampleBits);
    printf("Starting hl2_tcp server on TCP port %d\n", portno);
    
    tx_setup();
    
    sigact.sa_handler = sighandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT,  &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGQUIT, &sigact, NULL);
#ifdef __APPLE__
    signal(SIGPIPE, SIG_IGN);
#else
    sigign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sigign, NULL);
#endif
    
    previousSRate =  sampRate;
    
    // printf("\nhl2_tcp server started on port %d\n", portno);
    
    tcp_listen_sockfd = socket(AF_INET6, SOCK_STREAM, 0);
    if (tcp_listen_sockfd < 0) {
        printf("ERROR opening socket");
        return(-1);
    }
    
    struct linger ling = {1,0};
    int rr = 1;
    setsockopt(tcp_listen_sockfd, SOL_SOCKET, SO_REUSEADDR,
               (char *)&rr, sizeof(int));
    setsockopt(tcp_listen_sockfd, SOL_SOCKET, SO_LINGER,
               (char *)&ling, sizeof(ling));
    
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin6_flowinfo = 0;
    serv_addr.sin6_family = AF_INET6;
    serv_addr.sin6_addr = in6addr_any;
    serv_addr.sin6_port = htons(portno);
    
    // Sockets Layer Call: bind()
    if (bind( tcp_listen_sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr) ) < 0) {
        printf("ERROR on bind to listen\n");
        return(-1);
    }
    
    listen(tcp_listen_sockfd, 5);
    fprintf(stdout, "listening for socket connection \n");
    
    while (1) {
        
        // accept a connection
        
        struct sockaddr_in6 cli_addr;
        socklen_t claddrlen = sizeof(cli_addr);
        gClientSocketID = accept( tcp_listen_sockfd,
                                 (struct sockaddr *) &cli_addr,
                                 &claddrlen );
        if (gClientSocketID < 0) {
            printf("ERROR on accept\n");
            break;
        }
        
        inet_ntop(AF_INET6, &(cli_addr.sin6_addr), client_addr_ipv6, 100);
        printf("\nConnected to client with IP address: %s\n",
               client_addr_ipv6);
        
        tcp_connection_handler();
        
        printf("tcp connection ended \n");
    }
    
    udp_running  = -1;
    hl2_stop();
    tx_cleanup();
    
    fflush(stdout);
    return 0;
}  //  main

static void sighandler(int signum)
{
    fprintf(stderr, "Signal caught, exiting!\n");
    fflush(stderr);
    close(tcp_listen_sockfd);
    if (gClientSocketID != 0) {
        close(gClientSocketID);
        gClientSocketID = -1;
    }
    udp_running  = -1;
    hl2_stop();
    tx_cleanup();
    exit(-1);
    // do_exit = 1;
}

int stop_send_thread = 0;
int thread_counter = 0;
int thread_running = 0;

int  ring_buffer_size   =  RING_BUFFER_ALLOCATION;
volatile long int ring_wr_index  =  0;
volatile long int ring_rd_index  =  0;

void ring_init()
{
    ring_wr_index  =  0;
    ring_rd_index  =  0;
}

int ring_data_available()
{
    long int n = 0;
    long int w_index = ring_wr_index;  //
    long int r_index = ring_rd_index;  //
    n = w_index - r_index;
    if (n < 0) { n += ring_buffer_size; }
    if (n < 0) { n = 0; }                    // error condition ?
    if (n >= ring_buffer_size) { n = 0; }       // error condition
    return(n);
}

int ring_write(unsigned char *from_ptr, int amount)
{
    int wrap = 0;
    long int w_index = ring_wr_index;  // my index
    long int r_index = ring_rd_index;  // other threads index
    if (  ring_buffer_ptr == NULL ) { return(-1); }
    if (   (w_index < 0)
        || (w_index >= ring_buffer_size) ) { return(-1); }  // error !
    if (decimateFlag > 1) {
        int i;
        for (i = 0; i < amount; i += 2) {
            if (decimateCntr == 0) {
                ring_buffer_ptr[w_index  ] = from_ptr[i  ];
                ring_buffer_ptr[w_index+1] = from_ptr[i+1];
                w_index += 2;
                if (w_index >= ring_buffer_size) { w_index = 0; }
            }
            decimateCntr += 1;
            if (decimateCntr >= decimateFlag) { decimateCntr = 0; }
        }
    } else if (w_index + amount < ring_buffer_size) {
        memcpy(&ring_buffer_ptr[w_index], from_ptr, amount);
        w_index += amount;
    } else {
        int i;
        for (i = 0; i < amount; i += 1) {
            ring_buffer_ptr[w_index] = from_ptr[i];
            w_index += 1;
            if (w_index >= ring_buffer_size) { w_index = 0; }
        }
    }
    //
    /// ToDo: insert memory barrier here
    //
    ring_wr_index = w_index;     // update lock free input info
    int m = ring_data_available();
    if (m > ring_buffer_size/2) { wrap = 1; }
    return(wrap);
}

int ring_read(unsigned char *to_ptr, int amount, int always)
{
    int bytes_read = 0;
    long int r_index = ring_rd_index;  // my index
    long int w_index = ring_wr_index;  // other threads index
    long int available = w_index - r_index;
    if (available < 0) { available += ring_buffer_size; }
    if (always != 0) {
        bzero(to_ptr, amount);
    }
    if (available <= 0) { return(bytes_read); }
    long int n = amount;
    if (n > available) { n = available; }    // min(n, available)
    if (r_index + n < ring_buffer_size) {
        memcpy(to_ptr, &ring_buffer_ptr[r_index], n);
        r_index += n;
    } else {
        int i;
        for (i = 0; i < n; i += 1) {
            to_ptr[i] = ring_buffer_ptr[r_index];
            r_index += 1;
            if (r_index >= ring_buffer_size) { r_index = 0; }
        }
    }
    bytes_read = n;
    ring_rd_index = r_index;       // update lock free extract info
    return(bytes_read);
}

float       tmpFPBuf[4*32768];
uint8_t     tmpBuf[  4*32768];
long int     *param =  NULL;

int tcp_send_poll()
{
    int sz0   =     1408;           // MTU size or 1008 ??
    int pad   =    32768 * 2;
    ssize_t b =        0;
    int send_sockfd = gClientSocketID ;
    
    if (send_sockfd > 0) {
        if (ring_data_available() >= (sz0 + pad)) {
            int sz = ring_read(tmpBuf, sz0, 0);
            if (sz > 0) {
                if (totalSamples == 0) {
                    // fprintf(stderr, "hl2 udp IQ data received \n");
                }
#ifdef __APPLE__
                b = send(send_sockfd, tmpBuf, sz, 0);
#else
                b = send(send_sockfd, tmpBuf, sz, MSG_NOSIGNAL);
#endif
                if (b <= 0) { sendErrorFlag = -1; }
                if (totalSamples == 0) {
                    fprintf(stderr,
                            "Started rtl_tcp IQ streaming with %ld bytes\n", b);
                }
                totalSamples   +=  sz;
                sendblockcount +=  1;
            }
            pad = 0;
        }
    }
    return(b);
}

int  hl2_udp_setup(void);
void hl2_send_start_cmds(int n);
void hl2_udp_rcv(int loopFlag);
int  hl2_udp_rcv_end(void);

void *tcp_connection_handler()
{
    char buffer[256];
    int m = 0;
    int printCmdBytes = 0; //
    int i;
    
    if (do_exit != 0) { return(NULL); }
    
    if (1) {        // send 12 or 16-byte rtl_tcp header
        ssize_t b = 0;
        int sz = 16;
        if (sampleBits == 8) { sz = 12; }
        //  "HL20"  in 16 byte header
        char header[16] = { 0x48,0x4C,0x32,0x30,
            0x30,0x30,0x30+sampleRates,0x30+sampleBits,
            0,0,0,1, 0,0,0,2 };
#ifdef __APPLE__
        b = send(gClientSocketID, header, sz, 0);
#else
        b = send(gClientSocketID, header, sz, MSG_NOSIGNAL);
#endif
        /*
         fprintf(stdout, "rtl_tcp header sent , %d bytes\n", n); //
         fflush(stdout);
         */
    }
    
    sendErrorFlag       =  0;
    stop_send_thread    =  0;
    ring_wr_index       =  0;
    ring_rd_index       =  0;
    
    param = (long int *)malloc(4 * sizeof(long int));  // ToDo: fix leak
    for (i=0;i<4;i++) { param[i] = 0; }
    
    if (hl2_running == 0) {
        ring_init();
        hl2_udp_setup();
        hl2_send_start_cmds(1);
        
        pthread_t udp_rcv_thread;
        if ( pthread_create( &udp_rcv_thread,
                            NULL ,
                            udp_rcv_thread_runner,
                            (void *)param       ) < 0 ) {
            printf("could not create udp streaming thread");
            return(NULL);
        } else {
            printf("udp streaming thread started \n");
        }
        
        int n = MAX_NUMBER_OF_START_COMMANDS;
        hl2_send_start_cmds(n);
    }
    
    acc_r         =  0.0;
    totalSamples  =  0;
    // printf("hl2 start status = %d\n", m);
    if (m < 0) { exit(-1); }
    usleep(250L * 1000L);
    
    // set a timeout so receive call won't block forever
    struct timeval timeout;
    timeout.tv_sec = SOCKET_READ_TIMEOUT;    // seconds
    timeout.tv_usec = 0;
    setsockopt( gClientSocketID, SOL_SOCKET, SO_RCVTIMEO,
               &timeout, sizeof(timeout) );
    
    ssize_t b = 1;
    while ((b > 0) && (sendErrorFlag == 0)) {
        int i, j;
        // receive 5 byte commands (or a multiple thereof)
        memset(buffer,0, 256);
        b = recv(gClientSocketID, buffer, 255, 0);
        if ((b <= 0) || (sendErrorFlag != 0)) {
            udp_running  = -1;
            close(gClientSocketID);
            gClientSocketID = -1;
            // fprintf(stdout, "hl2 stop status = %d\n", m);
            fflush(stdout);
            break;
        }
        if (b > 0) {
            for (i=0; i < b; i+=5) {
                // decode 5 byte rtl_tcp command messages
                int msg  = 0x00ff & buffer[i];
                if (printCmdBytes) { printf("0x%02x ", msg); }
                int data = 0;
                for (j=1;j<5;j++) {
                    int byte = (0x00ff & buffer[i+j]);
                    data = (256 * data) + byte;
                    if (printCmdBytes) { printf("0x%02x ", byte); }
                }
                if (printCmdBytes) { printf(" = %d\n", data); }
                
                if (msg == 0x01) {    // set frequency
                    int f0 = data;
                    if (f0 >= 100000 && f0 <= 30000000) {
                        hermes_rx_freq     =  f0;  //  hl2
                        setFilterEnables();
                        hermes_tx_offset =  get_tx_offset();
                    }
                    fprintf(stdout, "setting frequency to: %d\n", f0);
                } else if (msg == 0x02) {    // set sample rate
                    int r = data;
                    if (   (r ==  48000)
                        || (r ==  96000)
                        || (r == 192000)
                        || (r == 384000)) {
                        // if (r != previousSRate) {
                        sampRate = r;
                        tx_gear_ratio = r / 48000;
                        printf("setting samplerate to : %d\n", r);
                        // }
                    } else {
                        // ignore
                    }
                } else if (msg == 0x03) {            // other
                    fprintf(stdout, "message = %d, data = %d\n", msg, data);
                } else if (msg == 0x04) {            // gain
                    if (   (sampleBits ==  8)
                        || (sampleBits == 16) ) {
                        // set gain
                        // hermes_lna_gain : LNA gain -12 to +48 dB
                        //
                        float g1 = data; // data : in 10th dB's
                        float g2 = 0.1 * (float)(data); // undo 10ths
                        // fprintf(stdout, "setting gain to: %f dB\n", g2);
                        float g2h = g2 * 60.0 / 40.0 ;
                        int g9 = roundf(g2h - 12.0);
                        hermes_lna_gain = g9;
                        fprintf(stdout, "set hl2 lna gain to : %d\n", g9);
                        
                        if (sampleBits == 16) {
                            gain0 = GAIN8 *  0.125;
                        } else {
                            gain0 = GAIN8 *  8.0;
                        }
                    }
#ifdef TX_OK
                } else if (msg == 77) {       // 0x4d tx command extension
                    tcp_tx_cmd = 3 * data;  //  381 = 127 * 3
                    // printf("tcp_tx_cmd = %d \n", tcp_tx_cmd); //
#endif
                } else {            // other
                    fprintf(stdout, "message = %d, data = %d\n", msg, data);
                    if (msg == 8) {
                        fprintf(stdout, "set agc mode ignored\n");
                    }
                }
            }
        }
        if (b < 0) {
            fprintf(stdout, "read socket timeout %ld \n", b);
            fflush(stdout);
        }
        // loop until error (socket close) or timeout
    } ;
    
    if (m) {
        fprintf(stdout,"stopping now 00 \n");
        printf("hl2 stop status = %d\n", m);
    }
    udp_running  = -1;
    
    close(gClientSocketID);
    gClientSocketID = -1;
    return(param);
} // tcp_connection_handler()

// uint8_t tmpBuf[4*32768];

typedef union
{
    uint32_t i;
    float    f;
} Float32_t;

float rand_float_co()
{
    Float32_t x;
    x.i = 0x3f800000 | (rand() & 0x007fffff);
    return(x.f - 1.0f);
}

//
//

#define FILTER_HPF          (0x40)
#define FILTER_160        (0x01)
#define FILTER_80           (0x02)
#define FILTER_40           (0x04)
#define FILTER_30_20        (0x08)
#define FILTER_17_15        (0x10)
#define FILTER_10           (0x20)

struct sockaddr_in     recv_Addr;
socklen_t addrLen       =  sizeof(recv_Addr);
unsigned char udpBuffer[1600];    // Original Protocol Command & Control

//

double samp_db = 0.0;
float rnd0v = 0.0;
float rnd0u = 0.0;

float tmp_temperature     =  0.0;
float tmp_fwd_power      =  0.0;
float tmp_rev_power       =  0.0;
float tmp_pa_current      =  0.0;
int   tmp_temp_count    =  0;
int   tmp_revp_count    =  0;

float hermes_temperature  =  0;
float hermes_fwd_power    =  0;
float hermes_rev_power    =  0;
float hermes_pa_current   =  0;

int hwCmdState          =  0;
int hwCmdSeqNum        =  0;
unsigned char hwCmd[8]    =  { 0,0,0,0,0 };   // 1+4 fpga command bytes

void *udp_rcv_thread_runner(void *param)
{
    if (hl2_running == 0) {
        hl2_udp_rcv(1);
        hl2_udp_rcv_end();
    }
    return(param);
}

// extract command replies, status, and 24-bit IQ
// transcode to 8 or 16 bit IQ

int handleRcvData(unsigned char *hl2Buf, int n)
{
    unsigned char *buf = hl2Buf;
    int j, jj;
    int rcvSeqNum;
    double samp_m2  = 0.0;
    double g8  =  gain0; // GAIN8;
    unsigned char uv[1024];
    
    int syncErr =  (   (buf[11 - 3] != 0x7F)
                    || (buf[11 - 2] != 0x7F)
                    || (buf[11 - 1] != 0x7F));
    if (syncErr) { return(-1); }
    rcvSeqNum = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | (buf[7]);
    
    for (jj=0; jj<1024; jj+=512) {
        int replyBit = (buf[jj+11] & 0x80) ;
        int dt = rcvSeqNum - hwCmdSeqNum;
        if (replyBit) {
            int i;
            int cmdEcho = (buf[jj+11] & 0x7F) >> 1;
            printf("******** # %d hw cmd ack 0x%02x : ", dt, cmdEcho);
            for (i=0;i<5;i++) { printf("0x%02x ", buf[jj+11+i]); }
            printf("\n");
            cmdEcho =  hwCmd[0];    // ToDo: test or fix
            if ( cmdEcho ==  hwCmd[0] ) {
                hwCmdState  =  0;
                // command acknowledged ?
                specialCommandCounter -= 1;
                // hwCmdSeqNum    =  rcvSeqNum;
            }
            if (buf[jj+11] == 0xff) {
                hwCmdState  = -1;
                fprintf(stdout,"******** hw cmd error \n" );
            }
        }
    }
    
    int dtype0 =  buf[11    ] >> 3;
    int dtype1 =  buf[11+512] >> 3;
    if (dtype0 == 1) {
        int jj = 0;
        tmp_temperature  +=  ((buf[11+jj+1] << 8) | (buf[11+jj+2]));
        tmp_fwd_power    +=  ((buf[11+jj+3] << 8) | (buf[11+jj+4]));
        tmp_temp_count     +=  1;
    }
    if (dtype1 == 1) {
        int jj = 512;
        tmp_temperature  += ((buf[11+jj+1] << 8) | (buf[11+jj+2]));
        tmp_fwd_power    += ((buf[11+jj+3] << 8) | (buf[11+jj+4]));
        tmp_temp_count     +=  1;
    }
    if (dtype0 == 2) {
        tmp_rev_power    += ((buf[11 + 1] << 8) | (buf[11 + 2]));
        tmp_pa_current   += ((buf[11 + 3] << 8) | (buf[11 + 4]));
        tmp_revp_count     +=  1;
    }
    if (dtype1 == 2) {
        int jj = 512;
        tmp_rev_power    += ((buf[11+jj+1] << 8) | (buf[11+jj+2]));
        tmp_pa_current   += ((buf[11+jj+3] << 8) | (buf[11+jj+4]));
        tmp_revp_count     +=  1;
    }
    double scale   =  32768.0 * 32768.0 * 2.0; // 33 bit shift
    double scale_r =  1.0 / scale;
    int16_t *tmp16ptr = (int16_t *)&uv[0];
    int    k  =  0;
    int    kk =  0;
    for (jj=0; jj<1024; jj+=512) {
        for (j=8+jj+8; j<8+jj+512; j += 8) {
            int    imagp, realp;
            double u,v;
            // reversed order IQ
            imagp =  buf[j    ] << 24 | buf[j + 1] << 16 | buf[j + 2] << 8;
            realp =  buf[j + 3] << 24 | buf[j + 4] << 16 | buf[j + 5] << 8;
            v  = realp;
            u  = imagp;
            if (sampleBits == 16) {          // for HERMES !!!
                float  g16;
                g16 = 64.0 * 32768.0;    // shift 22 bits up
                // scale_r == shift 33 bits down
                // total shift == 22 - 33 == 11 bits down
                // 24 - 11 = 13 bits used (sign + 12 bits?)
                float v1 = g16 * scale_r * v;
                float u1 = g16 * scale_r * u;
                int   vv = (int)roundf(v1);
                int   uu = (int)roundf(u1);
                tmp16ptr[kk  ] = vv;
                tmp16ptr[kk+1] = uu;
                kk += 2;            // 2 16-bit samples
                k  += 4;            // is 4 bytes
            } else {  // convert to 8-bit samples for rtl_tcp compatibility
                // magnitude for testing
                samp_m2 += u*u + v*v;
                // scale and add triangular noise shaping (dither)
                float vv = g8 * scale_r * v;
                float rnd1v = rand_float_co();
                float rv = rnd1v - rnd0v;
                vv = vv + rv;
                float rvv = roundf(vv);
                int vi  = (int)rvv;
                rnd0v = rnd1v;
                float uu = g8 * scale_r * u;
                float rnd1u = rand_float_co();
                float ru = rnd1u - rnd0u;
                uu = uu + ru;
                float ruu = roundf(uu);
                int ui  = (int)ruu;
                rnd0u = rnd1u;
                // unsigned 8-bit samples
                uv[k]   = vi + 128;
                uv[k+1] = ui + 128;
                k += 2;        // 2 bytes for 8-bit IQ
            }
        }
    }
    ring_write(&uv[0], k);
    //
    int nSamples = (2 * (512-8))/4;  // == 1008/4 == 252
    double samp_rms = sqrt(samp_m2 / nSamples) / scale;
    if (samp_rms > 0.0) {
        samp_db = 20.0 * log10(samp_rms);
    }
    //
    if (tmp_temp_count >= 16) {
        hermes_temperature  =  tmp_temperature / (float)tmp_temp_count;
        hermes_rev_power    =  tmp_fwd_power   / (float)tmp_temp_count;
        tmp_temperature     =  0.0;
        tmp_fwd_power       =  0.0;
        tmp_temp_count      =  0;
    }
    if (tmp_revp_count >= 16) {
        hermes_fwd_power    =  tmp_rev_power   / (float)tmp_revp_count;
        tmp_rev_power       =  0.0;
        
        float tmp2_pa_current ;
        float tmp3_pa_current ;
        tmp2_pa_current   =  tmp_pa_current  / (float)tmp_revp_count;
        tmp3_pa_current   =  3.26 * tmp2_pa_current / (4096.0 * 50.0 * 0.04);
        hermes_pa_current =  1270.0 * tmp3_pa_current / 1000.0;
        tmp_pa_current      =  0.0;
        tmp_revp_count      =  0;
    }
    return(k);
}

void dump_iq_buf(unsigned char *buf)
{
    int k;
    // 1032 - 8 = 1024 // 8 -> effe0406 sequence4
    // 1024 = 2 * 512
    // 512 - 8 = 63 * 4+4 IQ // 8 -> 7f 7f 7f c0 c1 c2 c3 c4
    // 63 * 8 = 504
    // 504 * 2 = 1008
    for (k=0;k<16;k++) {
        int c = buf[k];
        fprintf(stdout,"%02x ",c);
    }
    fprintf(stdout,"\n                        ");
    for (k=512+8;k<512+16;k++) {
        int c = buf[k];
        fprintf(stdout,"%02x ",c);
    }
    fprintf(stdout,"\n");
    int seq0 = buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7];
    printf("seq = %d \n", seq0);
    
#ifdef DEBUG_FILE
    double samp_msq = 0.0;    // magnitude squared
    double samp_rms = 0.0;
    double scale = 32768.0 * 32768.0 * 2.0; // 33 bit shift
    
    if (dump_fname == NULL) { return; }
    FILE *f = fopen(dump_fname,"w");
    if (f == NULL) { return; }
    // printf("file 0x%08x \n", f);
    
    for (k=0;k<16;k++) {
        int c = buf[k];
        fprintf(f,"%02x ",c);
    }
    printf(" : ");
    for (k=512+8;k<512+16;k++) {
        int c = buf[k];
        fprintf(f,"%02x ",c);
    }
    fprintf(f,"\n");
    
    int j;
    int xr, xi ;
    double v, u;
    
    for (j=8+8; j<8+512; j += 8) {
        xr = buf[j + 3] << 24 | buf[j + 4] << 16 | buf[j + 5] << 8;
        xi = buf[j    ] << 24 | buf[j + 1] << 16 | buf[j + 2] << 8;
        v = xr;
        u = xi;
        fprintf(f,"%4d 0x%08x 0x%08x \t%lf \t%lf \n",
                (j-16)/8, xr, xi, v/scale, u/scale);
        samp_msq += u*u + v*v;
    }
    for (j=8+512+8; j<8+1024; j+= 8) {
        xr = buf[j + 3] << 24 | buf[j + 4] << 16 | buf[j + 5] << 8;
        xi = buf[j    ] << 24 | buf[j + 1] << 16 | buf[j + 2] << 8;
        v = xr;
        u = xi;
        //
        fprintf(f,"%4d 0x%08x 0x%08x \t%lf \t%lf \n",
                (j-16)/8, xr, xi, v/scale, u/scale);
        samp_msq += u*u + v*v;
    }
    fclose(f);
    
    int nSamples = (2 * (512-8))/4;  // == 1008/4 == 252
    samp_rms = sqrt(samp_msq / nSamples) / scale;
    if (samp_rms > 0.0) {
        samp_db = 20.0 * log10(samp_rms);
    }
    printf("rms db = %lf \n", samp_db);
#endif  //  DEBUG_FILE
}

//
//

uint32_t getDecimalValueOfIPV4_String(const char* ipAddress)
{
    uint8_t ipbytes[4]={};
    int i =0;
    int8_t j=3;
    while (ipAddress+i && i<strlen(ipAddress))
    {
        char digit = ipAddress[i];
        if (isdigit(digit) == 0 && digit!='.'){
            return 0;
        }
        j=digit=='.'?j-1:j;
        ipbytes[j]= ipbytes[j]*10 + atoi(&digit);
        
        i++;
    }
    
    uint32_t a = ipbytes[0];
    uint32_t b =  ( uint32_t)ipbytes[1] << 8;
    uint32_t c =  ( uint32_t)ipbytes[2] << 16;
    uint32_t d =  ( uint32_t)ipbytes[3] << 24;
    return a+b+c+d;
}

/*
 */

void configStartCmd(char* buf, int flag)
{
    if (buf == NULL) { return; }
    char myData[64];
    
    bzero(myData, 64);
    
    myData[0] = 0xef;
    myData[1] = 0xfe;
    myData[2] = 0x04;
    myData[3] = flag; // 0x01;
    bcopy(myData, buf, 64);
}

void setFilterEnables()    // open collector
{
    n2adr_filter_rx     =  0;
    if (hermes_rx_freq >  3000000) {
        n2adr_filter_rx =  FILTER_HPF;
    }
    
    n2adr_filter_tx     =  FILTER_160;
    if (hermes_tx_freq >  3500000) {
        n2adr_filter_tx =  FILTER_80;
    }
    if (hermes_tx_freq >  7000000) {
        n2adr_filter_tx =  FILTER_40;
    }
    if (hermes_tx_freq > 14000000) {
        n2adr_filter_tx =  FILTER_30_20;
    }
    if (hermes_tx_freq > 21000000) {
        n2adr_filter_tx =  FILTER_10;     // ToDo: bug
    }
}

void configSendUDP_packet(unsigned char *opccBuf)
{
    int i;
    
    bzero(opccBuf, 1032);
    opccBuf[0]  =  0xEF;
    opccBuf[1]  =  0xFE;
    opccBuf[2]  =  0x01;
    opccBuf[3]  =  0x02;  //  Metis UDP/IP for USB EP2
    
    opccBuf[4]  =  seqNum >> 24 & 0xFF;
    opccBuf[5]  =  seqNum >> 16 & 0xFF;
    opccBuf[6]  =  seqNum >>  8 & 0xFF;
    opccBuf[7]  =  seqNum       & 0xFF;
    
    opccBuf[  8] =  0x7F;
    opccBuf[  9] =  0x7F;
    opccBuf[ 10] =  0x7F;
    opccBuf[520] =  0x7F;
    opccBuf[521] =  0x7F;
    opccBuf[522] =  0x7F;
    
    tx_key_down =  get_tx_key() ? 1 : 0; //
    //
    if (hermes_tx_freq     <  1800000) { tx_key_down = 0; }
    if (tx_key_down) {
        hermes_tx_drive_level =  get_tx_drive();      //  lvl 0..15
        tx_delay = 1.5 * 382.0 * (sampRate/48000.0);      // ? 1.5 second delay
    } else {
        // hermes_tx_drive_level =  0;
        if (tx_delay > 0) { tx_delay -= 1; }
    }
    hl2_tx_on   =  tx_key_down | ((tx_delay > 0) ? 1 : 0);
    if (hl2_tx_on == 0) {
        // hermes_tx_drive_level =  0;         // yyy
    }
    
    opccBuf[ 11] = (C0_addr + 0) << 1 | hl2_tx_on;        //
    for (i=0;i<4;i++) { opccBuf[ 12+i] = 0; }
    opccBuf[523] = (C0_addr + 1) << 1 | hl2_tx_on;     //
    for (i=0;i<4;i++) { opccBuf[524+i] = 0; }
    
    hermes_tx_freq = hermes_rx_freq + hermes_tx_offset ;
    // setFilterEnables();
    
    if (C0_addr == 0) {
        opccBuf[11] = (C0_addr << 1) | hl2_tx_on;    // 0
        
        // [25:24]    Speed (00=48kHz, 01=96kHz, 10=192kHz, 11=384kHz)
        int r0 = 0;
        if (       sampRate == 384000) {
            r0 = 3;
        } else if (sampRate == 192000) {
            r0 = 2;
        } else if (sampRate ==  96000) {
            r0 = 1;
        }
        opccBuf[12] =  r0;
        
        // [23:17]    Open Collector Outputs on Penelope or Hermes
        opccBuf[13] =  0;
        if (hl2_tx_on) {            // send filter selection on J16
            opccBuf[13] = n2adr_filter_tx << 1;    // C2
        } else {
            opccBuf[13] = n2adr_filter_rx << 1;
        }
        
        // [10]    VNA fixed RX Gain (0=-6dB, 1=+6dB)
        opccBuf[14] =  0;
        
        // [6:3]    Number of Receivers (0000=1 to max 1011=12)
        // [2]    Duplex (0=off, 1=on)
        opccBuf[15] =  0x04;        // C4    duplex on
        
        opccBuf[523] = ((C0_addr + 1) << 1) | hl2_tx_on; // addr 1
        // transmitter frequency ?
        opccBuf[524] = (hermes_tx_freq >> 24) & 0xFF;        // C1
        opccBuf[525] = (hermes_tx_freq >> 16) & 0xFF;            // C2
        opccBuf[526] = (hermes_tx_freq >>  8) & 0xFF;            // C3
        opccBuf[527] = (hermes_tx_freq      ) & 0xFF;            // C4
    }
    
    if (C0_addr == 2) {
        // opccBuf[4]  =  seqNum >> 24 & 0xFF;
        // opccBuf[5]  =  seqNum >> 16 & 0xFF;
        // opccBuf[6]  =  seqNum >>  8 & 0xFF;
        // opccBuf[7]  =  seqNum       & 0xFF;
        // seqNum += 1;
        
        opccBuf[11] = (C0_addr << 1) | hl2_tx_on;        // C0
        opccBuf[12] = (hermes_rx_freq >> 24) & 0xFF;         // C1
        opccBuf[13] = (hermes_rx_freq >> 16) & 0xFF;        // C2
        opccBuf[14] = (hermes_rx_freq >>  8) & 0xFF;        // C3
        opccBuf[15] = (hermes_rx_freq      ) & 0xFF;        // C4
        
        opccBuf[523  ] = ((C0_addr + 1) << 1) | hl2_tx_on;     // C0
        opccBuf[523+1] = (hermes_rx_freq >> 24) & 0xFF;         // C1
        opccBuf[523+2] = (hermes_rx_freq >> 16) & 0xFF;        // C2
        opccBuf[523+3] = (hermes_rx_freq >>  8) & 0xFF;        // C3
        opccBuf[523+4] = (hermes_rx_freq      ) & 0xFF;        // C4
    }
    if (C0_addr ==  4) {
        opccBuf[ 11] = (C0_addr << 1) | hl2_tx_on;            // C0 == 4
        opccBuf[523] = ((C0_addr + 1) << 1) | hl2_tx_on;     // 5
    }
    if (C0_addr ==  6) {
        // special commands requiring an ack
        opccBuf[ 11] = (C0_addr << 1) | hl2_tx_on;            // 6
        opccBuf[523] = ((C0_addr + 1) << 1) | hl2_tx_on;     // 7
        //
        checkForSpecialCommands(seqNum);  //  sets hwCmdState
        if ((hwCmdState == 3) || (hwCmdState == 7)) {
            opccBuf[523] = (hwCmd[0] << 1) | hl2_tx_on | 0x80;     // ?
            for (i=1;i<=4;i++) {
                opccBuf[523+i] = hwCmd[i];  // 4 fpga command bytes
            }
            hwCmdState  = hwCmdState & 0x0e;    // clear LSB (2,6)
            hwCmdSeqNum    =  seqNum;
        } else {
            // opccBuff 524..527 are already zero'd
        }
    }
    if (C0_addr ==  8) {
        // filter selection
        opccBuf[ 11] = (C0_addr << 1) | hl2_tx_on;            // 8
        opccBuf[523] = ((C0_addr + 1) << 1) | hl2_tx_on;     // 9
        
        // 0x09    [31:24]    Hermes TX Drive Level (only [31:28] used)
        opccBuf[524] = (  (hermes_tx_drive_level << 4)
                        +  hermes_tx_drive_level )  ;        // 0..15
        if (hl2_tx_on) {
            hermes_enable_power_amp     =  1;
            hermes_Q5_switch_ext_ptt_lp =  1;
            opccBuf[525] = (  (hermes_enable_power_amp     << 3)
                            | (hermes_Q5_switch_ext_ptt_lp << 2) );
            opccBuf[526] = 0;                // C3
            opccBuf[527] = 0;                // C4
        } else {
            opccBuf[524] = 0;
            opccBuf[525] = 0;
            opccBuf[526] = 0;
            opccBuf[527] = 0;    // something called "Alex" ???
        }
    }
    if (C0_addr == 10) {
        // C0_addr is 10, 11 (0x0a,0x0b)
        opccBuf[ 11] = (C0_addr << 1) | hl2_tx_on;            // 10
        opccBuf[ 15] = ((hermes_lna_gain + 12) & 0x3F) | 0x40;
        opccBuf[523] = ((C0_addr + 1) << 1) | hl2_tx_on;     // 11
    }
    C0_addr += 2;         // receiver 1 frequency
    if (C0_addr > 10) { C0_addr = 0; }
    
    // get IQ transmit samples, if any, or zero fill
    
    tx_block_setup(seqNum);
    
    int jj;
    for (jj = 0; jj < 1024; jj+=512) {
        for (i = 0; i < 63; i++) {
            // update audio & tx IQ sent to hl2
            int txI =  0;
            int txQ =  0;
            // #ifdef TX_OK
            get_tx_sample(&txI, &txQ);
            // #endif
            int k = 16 + jj + 8 * i;
            
            opccBuf[k  ] =  0;            // Left Audio
            opccBuf[k+1] =  0;            //     lsb
            opccBuf[k+2] =  0;            // Right Audio
            opccBuf[k+3] =  0;            //     lsb
            
            // normal IQ order
            opccBuf[k+6] =  (txI >> 8) & 0xff;    // bigEndian I  yyy
            opccBuf[k+7] =  (txI     ) & 0xff;    //     lsb
            opccBuf[k+4] =  (txQ >> 8) & 0xff;    // Q
            opccBuf[k+5] =  (txQ     ) & 0xff;    //     lsb
        }
    }
    seqNum += 1;
}

struct sockaddr_in hl2_sockAddr;

int udp_send_count = 0;

int hl2_udp_rcv_loop(int fd, int mode)
{
    unsigned char       hl2RcvBuf[1500];
    struct sockaddr_in  from_Addr;
    socklen_t addrLen2  =  sizeof(struct sockaddr_in);
    int     k  =  0;
    ssize_t b  =  0;
    int     n2 =  1500;

    bzero(hl2RcvBuf, 1500);
    from_Addr = hl2_sockAddr;
    while (udp_running  > 0) {
        // fd = hl2_fd
        b = recvfrom(fd, &hl2RcvBuf[0], n2, 0,
                     (struct sockaddr *)&from_Addr, &addrLen2);
        if (b > 0) {                // got UDP data from HL2
            ssize_t bb;
            
            // unpack and write IQ to ring buffer
            handleRcvData(hl2RcvBuf, b);    // stuff IQ into FIFO
            // then if enough IQ samples send from FIFO via rtl_tcp
            tcp_send_poll();              // remove from FIFO
            if (sendErrorFlag < 0) {
                fprintf(stdout, "tcp send error \n");
                udp_running  = -3;
            }
            
            if (tx_gear_counter % tx_gear_ratio ==  0) {
                configSendUDP_packet(udpBuffer);  // inc send sequence #
                int msgLen = 1032;          // send UDP pkt to HL2
                bb = sendto( fd, &udpBuffer[0], msgLen, 0,
                            (struct sockaddr *)&hl2_sockAddr, addrLen);
                if (bb > 0) {
                    udp_send_count += 1;
                    if ((tcp_tx_cmd & 0x00ff) != 0) {
                        tcp_tx_cmd -= 1;
                        // printf("tcp_tx_cmd = %d ", tcp_tx_cmd);
                    }
                } else {            // quit loop on error
                    fprintf(stdout, "udp to hl2 send error \n");
                    udp_running  = -2;
                    return(bb);
                }
            }
            tx_gear_counter += 1;
        } else {
            fprintf(stdout, "udp from hl2 rcv error \n");
            udp_running  = -1;
            return(b);
        }
        k += 1;
    }                        // loop forever
    return(0);
}

void udp_rcv_n_bytes(int fd, int num)    // for testing
{
    int      k =  0;
    ssize_t  b =  0;
    char hl2RcvBuf[1500];
    bzero(hl2RcvBuf, 1500);
    int n2 = 1500;
    while (k < num) {
        b = recvfrom(fd, &hl2RcvBuf[0], n2, 0, NULL, NULL);
        if (b > 0) {
            configSendUDP_packet(udpBuffer); // inc sequence
            int msgLen = 1032;
            b = sendto( fd, &udpBuffer[0], msgLen, 0,
                       (struct sockaddr *)&hl2_sockAddr, addrLen);
            if (b > 0) { udp_send_count += 1; }
        } else {
            return;
        }
        k += 1;
    }
    if (fd != 0) {
        b = recvfrom(fd, &hl2RcvBuf[0], n2, 0, NULL, NULL);
        if (b > 0) {
            dump_iq_buf((unsigned char *)hl2RcvBuf);
        }
    }
    printf("udp packets sent = %d \n", udp_send_count );
}  // udp_rcv_n_bytes() is a print dump for testing

unsigned int getDecimalValueOfIPV4_String(const char* ipAddress);

// int hl2_running = 0;
int hl2_fd  = -1;

int hl2_udp_setup()
{
    // uint32_t hl2_ipAddr  =  0x7f000001;     // 127.0.0.1 == localhost
    // int port         =  HERMES_PORT;
    int fd              =  -1;
    
    tmp_temperature     =  0.0;
    tmp_fwd_power       =  0.0;
    tmp_rev_power       =  0.0;
    tmp_pa_current      =  0.0;
    tmp_temp_count      =  0;
    tmp_revp_count      =  0;
    bzero(udpBuffer, 1032);
    
    if (1) {
        char *s = hl2_ip_string;
        if (s) {
            hl2_ipAddr = getDecimalValueOfIPV4_String(s);
            printf("hl2 ip addr: %s = 0x%lx\n", s, (long int)hl2_ipAddr);
        } else {
            return(-1);
        }
    }
    
    // if (hl2_fd != -1) {
        if ( (fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) {
            perror("socket failed");
            return(-1);
        }
        hl2_fd = fd;
    // }

    memset( &hl2_sockAddr, 0, sizeof(hl2_sockAddr) );
    hl2_sockAddr.sin_family = AF_INET;
    hl2_sockAddr.sin_port = htons( HERMES_PORT );
    // hl2_sockAddr.sin_addr.s_addr = htonl( 0x7f000001 );  // 127.0.0.1
    hl2_sockAddr.sin_addr.s_addr = htonl( hl2_ipAddr );
        
    return(0);
}

void hl2_send_start_cmds(int n)
{
    char myMsg[1500];
    // int n = MAX_NUMBER_OF_START_COMMANDS;
    
    for ( int i = 0; i < n; i++ ) {
        
        int flag = 0x01;        // send start command to HL2
        configStartCmd(myMsg, flag);    //     64 bytes
        int msgLen = 64;
        
        int addrLen = sizeof(hl2_sockAddr);
        if (sendto( hl2_fd, &myMsg[0], msgLen, 0,
                   (struct sockaddr *)&hl2_sockAddr, addrLen) < 0 ) {
            perror( "sendto failed" );
            break;
        }
        if (i == 0) {
            // printf( "Cmd %d sent, %d bytes of UDP\n", flag, msgLen);
        }
        if (ring_data_available() > 0) {
            int nt = i + 1;
            printf("hl2 start cmd sent %d times\n", nt);
            printf("hl2 udp IQ data received \n");
            break;            // yyy yyy
        }
        
        usleep(START_LOOP_DELAY);
#ifdef RESPONSE_WAIT
        char ackvar[1500];
        bzero(ackvar, 1500);
        int n2 =  1500;
        int b  =    -1;
        if ((b = recvfrom(hl2_fd, &ackvar[0], n2, 0, NULL, NULL)) < 0 ) {
            printf("hl2 receive error: errno %d\n", errno);
            // exit(1);
        } else {
            if (b > 0) {
                printf("received %d bytes of data >%s< \n", b, "x");
                hl2_running = 1;
                break;
            }
        }
#endif
    }
    
    /*
     int b = 0;
     int msgLen = 1032;
     C0_addr = 0;         // for rx 1 & transmitter 1 frequency
     
     configSendUDP_packet(udpBuffer);
     b = sendto( hl2_fd, &udpBuffer[0], msgLen, 0,
     (struct sockaddr *)&hl2_sockAddr, addrLen);
     printf("sent %d bytes of f0 cmd \n", b);
     C0_addr = 2;
     configSendUDP_packet(udpBuffer);
     b = sendto( hl2_fd, &udpBuffer[0], msgLen, 0,
     (struct sockaddr *)&hl2_sockAddr, addrLen);
     // printf("sent %d bytes of f0 cmd \n", b);
     */
}


void hl2_udp_rcv(int loopFlag)
{
    if (loopFlag) {
        udp_running  = 1;
        int mode = 1;
        hl2_udp_rcv_loop(hl2_fd, mode);    // loop until error
    } else {
        int count = 2048;
        udp_rcv_n_bytes(hl2_fd, count);    // unused
    }
}

int hl2_udp_rcv_end()
{
    char myMsg[1500];
    int msgLen = 64;
    int flag = 0x00;            // send stop UDP to HL2 twice
    
    print_hl2_stats() ;
    
    configStartCmd(myMsg, flag);
    
    msgLen = 64;
    sendto( hl2_fd, &myMsg[0], msgLen, 0,
           (struct sockaddr *)&hl2_sockAddr, addrLen);
    usleep(50000);
    sendto( hl2_fd, &myMsg[0], msgLen, 0,
           (struct sockaddr *)&hl2_sockAddr, addrLen);
    if (0) {
        // printf( "Cmd %d sent, %d bytes of UDP\n", flag, msgLen);
    }
    usleep(50000);
    
    hl2_running = 0;
    
    // cleanup
    close( hl2_fd );
    seqNum         =  0;
    tx_gear_counter =  0;
    fprintf(stdout, "hl2 UDP stream stopped \n\n");
    hl2_fd = -1;
    return 0;
}

void hl2_stop()
{
    if (hl2_fd >= 0) {
        char myMsg[1500] ;
        int msgLen = 64;
        int flag = 0x00;            // stop
        configStartCmd(myMsg, flag);
        sendto( hl2_fd, &myMsg[0], msgLen, 0,
               (struct sockaddr *)&hl2_sockAddr, addrLen);
        usleep(50000);
        sendto( hl2_fd, &myMsg[0], msgLen, 0,
               (struct sockaddr *)&hl2_sockAddr, addrLen);
        hl2_running = 0;
        close(hl2_fd);
        hl2_fd = -1;
        
        print_hl2_stats() ;
        seqNum = 0;
        fprintf(stdout, "hl2 UDP stream stopped \n");
    }
}

//
//

// hermes_discovery.c
// 2020-02-04 2019-12-23  rhn  2017-jun-xx
//
#define D_VERSION ("0.2.114")

#define DISCOVER
#define USE_FD
#define ECHO_WAIT     (1)

/*
 #include <stdio.h>
 #include <stdlib.h>
 #include <math.h>
 #include <unistd.h>
 #include <string.h>
 #include <ctype.h>
 #include <pthread.h>
 #include <sys/time.h>
 #include <errno.h>
 
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <netinet/in.h>
 #include <netinet/ip.h>
 #include <netdb.h>
 #include <ifaddrs.h>
 #include <arpa/inet.h>
 #include <fcntl.h>
 */

#define HERMES_PORT     (1024)

#define ADR_SIZE     (32)
#define INVALID_SOCKET    (-1)

char     ip_string[ADR_SIZE];
unsigned char sendbuf[1500];
int rx_discover_socket  = -1;
int sockfd             = -1;
int rx_udp_socket       =  INVALID_SOCKET;     // -1
// int seqNum             =  0;

int total         =  0;

#ifndef USE_FD
struct sockaddr_in     hermes_Addr;
#endif
struct sockaddr_in     recv_Addr;
// socklen_t addrLen       =  sizeof(recv_Addr);

void do_discovery(void);

// int main(int argc, char **argv)
int discover_main()
{
    printf("Hermes Lite 2 UDP IP Address Discovery %s\n", VERSION);
    
#ifdef DISCOVER
    do_discovery();
#endif
    
    printf("Done.\n");
    return(0);
}

//

void broadcast_discover(int rx_discover_socket);

void do_discovery()
{
    // short int  port =  HERMES_PORT;
    int     i;
    ssize_t b;
    int     fl;
    unsigned char  data[1500];
    
    rx_discover_socket = socket(PF_INET, SOCK_DGRAM, 0);
    setsockopt(rx_discover_socket, SOL_SOCKET, SO_BROADCAST,
               (char *)&i, sizeof(i));
    fl = fcntl(rx_discover_socket, F_GETFL);
    fcntl(rx_discover_socket, F_SETFL, fl | O_NONBLOCK);
    
    broadcast_discover(rx_discover_socket);
    
    usleep(50000);
    usleep(50000);
    b = recvfrom(rx_discover_socket, (char *)data, 1500, 0,
                 (struct sockaddr *)&recv_Addr, &addrLen);
    if (b > 0) {
        uint32_t ipAddr;
        strncpy(ip_string, inet_ntoa(recv_Addr.sin_addr), ADR_SIZE);
        printf("discover ip = %s : ", ip_string);
        ipAddr = *(uint32_t *)&recv_Addr.sin_addr;
        printf("0x%08X \n", ntohl(ipAddr));
        int p = ntohs(recv_Addr.sin_port);
        printf("discover port = %d \n", p);
    }
    if (b >= 60) {
        int j;
        printf("Received %ld bytes from UDP broadcast discover \n", b);
        for (j=0;j<3;j++) {
            printf("0x%02x ", data[j]);
        }
        printf("\n");
        printf("MAC Address: ");
        for (j=3;j<9;j++) {
            printf("%02x", data[j]);
            if (j != 8) { printf("."); }
        }
        printf("\n");
        printf("Protocol: %d\n", data[10]);
        printf("Gateware Version: %d\n", data[ 9]);
        
        /*
         for (j=0;j<64;j++) {
         printf("%02x ", data[j]);
         if (j % 8 == 7) { printf("\n"); }
         }
         printf("\n");
         */
    } else {
        exit(-1);
    }
    // is set_ip(rx_discover_socket) needed ???
}

void print_hl2_stats()
{
    float tc = ((3.26 * hermes_temperature / 4096.0) - 0.5) / 0.01;
    float tf = (tc * 9.0 / 5.0) + 32.0;
    printf("temp = %5.1f C = %5.1f F\n", tc, tf);
    printf("Rx rms db = %lf \n", samp_db);
    // printf("hermes_fwd_power:  %f \n", hermes_fwd_power );
    // printf("hermes_rev_power:  %f \n", hermes_rev_power );
    // printf("hermes_pa_current:  %f \n", hermes_pa_current);
    printf("seqNum = %d \n", seqNum);
}

//
// broadcast_discoverer
//

void broadcast_discover(int rx_discover_socket)
{
    unsigned char data[64];
    int           i, n = 0;
    ssize_t       b = 0;
    int           port = 1024;
    static struct sockaddr_in  bcast_Addr;
    
    struct ifaddrs * ifap, * p;
    
    data[0] = 0xEF;
    data[1] = 0xFE;
    data[2] = 0x02;
    for (i = 3; i < 64; i++) { data[i] = 0; }
    memset(&bcast_Addr, 0, sizeof(bcast_Addr));
    bcast_Addr.sin_family = AF_INET;
    bcast_Addr.sin_port = htons(port);
    if (getifaddrs(&ifap) == 0) {
        p = ifap;
        while(p) {
            if ((p->ifa_addr) && p->ifa_addr->sa_family == AF_INET) {
                bcast_Addr.sin_addr
                        = ((struct sockaddr_in *)(p->ifa_broadaddr))->sin_addr;
                b = sendto(rx_discover_socket, (char *)data, 63, 0,
                           (const struct sockaddr *)&bcast_Addr, sizeof(bcast_Addr));
            }
            // printf("bcast %d %d\n", i, n);
            n++;
            if (n > 255) { break; }
            usleep(50000);
            p = p->ifa_next;
        }
        freeifaddrs(ifap);
    }
    printf("%d UDP broadcasts of %ld bytes\n", n, b);
}

//   Copyright 2017,2020 Ronald H Nicholson Jr. All Rights Reserved.
//   This code may only be redistributed under terms of
//   the Mozilla Public License, Version 2.0.
//   See: https://www.mozilla.org/en-US/MPL/2.0/
//   Exhibit B - "Incompatible With Secondary Licenses" Notice
//   This Source Code Form is "Incompatible With Secondary Licenses",
//   as defined by the Mozilla Public License, v. 2.0.

// eof
