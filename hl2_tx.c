//
//  hl2_tx.c for hl2_tcp.c
#define VERSION "v.1.4.105" // 2023-01-05  2020-06-19 01 
//

#define TX_OK
// #define TX_DISABLE

#define NO_MODULO

#define TX_OFFSET       ( 656.0)	//  default for USB SSB
static float cw_df  =    TX_OFFSET;	//  synthesized USB IQ offset
// default Tx power approx 1 mW
// #define TX_DRIVE	  (15) 	    //  dac drive 0..15 
#define TX_DRIVE	  ( 3) 	    //  dac drive 0..15 
float tx_drive      =     TX_DRIVE;
#define TX_LEVEL	 (512.0)    //  amplitude 16.0 to 32767.0
float tx_lvl        =     TX_LEVEL;

void set_tx_level(float x) {  tx_lvl   =  x; }
void set_tx_drive(float x) {  tx_drive =  x; }
void set_tx_offset(float f) { cw_df    =  f; }

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
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

extern long          sampRate;
extern void print_hl2_stats() ;
extern volatile int  hl2_tx_on;
static int           tx_key_down_0;
extern volatile int  tx_key_down_1;
extern long int tcp_tx_cmd;
extern int hermes_tx_freq;
extern int tx_param_w;
extern int tx_param_x;

#ifdef TX_OK

extern int last_key_down   		;
static int last_get_key_x	=  0;
int txRiseCntr 	    		=  0;
int txFallCntr      		=  0;
int txOn_DnCntr			=  0;
int txOffDnCntr			=  0;
// open collector outputs on HL2; Rx and Tx filters; default no HPF

float txRiseWin[257];

float m_srate =  48000.0;
float tx_ph   =      0.0;
float tx_dph  =      0.1;
float ditherBuffer[64*1024+1];
int   ditherIdx 	=  0;

extern long random(void);

static long int tx_counter = 0;

void tx_setup() 
{
    char *addr =  NULL;
    int  sz    =  2;
    int  i;

    last_key_down       =  0;
    txRiseCntr 	        =  0;
    txFallCntr          =  0;
    tx_counter          =  0;

    if (tx_param_x >= 16 && tx_param_x <= 32768) {
	tx_lvl  =  tx_param_x;
    }

    for (i=0;i<64*1024;i++) {
       int r1 = 0x0fff & random();	// 0 .. 4095
       int r2 = 0x0fff & random();
       int r0 = r1 - r2;		// triangular -8192 .. 8190
       float r = 1.0 * (float)r0 / (8192.0f);	// -2.0 .. 2.0
       ditherBuffer[i] = r; // ditherIdx 
    }

    tx_dph = 2.0 * M_PI * cw_df / m_srate;

    // 5.3 mS rise time
    for (i=0; i<=256; i++) {
        float x = 0.5 - 0.5 * cosf((float)i * M_PI / 256.0); // no 2pi
	txRiseWin[i] = x;
    }

}
void tx_cleanup()
{
    tcp_tx_cmd = 0;
}

extern void resetTxDotQueue() ;
extern int queueTxDotCommand(int k, int on, int off) ;
extern int getNextTxDotCommand(void) ;

#define TDC_LIM (1024)
static int td1c_rb[TDC_LIM];
static int td0c_rb[TDC_LIM];
static int tdc_in  =  0 ;
static int tdc_out =  0 ;

void resetTxDotQueue() {
    txOn_DnCntr   =  0; 
    txOffDnCntr   =  1;
    tx_key_down_1 =  0;		// reset in hl2_tcp.c
    tx_key_down_0 =  0;
    tdc_in        =  0;
    tdc_out       =  0;
    for (int i=0;i<TDC_LIM;i++) {
        td1c_rb[i] =  0;
        td0c_rb[i] =  0;
    }
}
int queueTxDotCommand(int k, int on, int off) {
    int r = 0;
    int n = tdc_out - tdc_in;
    if (n < 0) { n += TDC_LIM; }
    if (n < TDC_LIM - 16) {
        td1c_rb[tdc_in] =  on;	// sample times
        td0c_rb[tdc_in] =  off;
	tdc_in +=  1;
	if (tdc_in >= TDC_LIM) { tdc_in =  0; }
        td1c_rb[tdc_in] =  0;
        td0c_rb[tdc_in] =  0;
    }
    if (txOn_DnCntr <= 0 && txOffDnCntr <= 0) {
        getNextTxDotCommand() ;
    }
    return(r);
}

int getNextTxDotCommand() {
    int on  =  0;
    int off =  0;
    int n   =  tdc_out - tdc_in;
    int j   =  tdc_out;
    if (n < 0) { n += TDC_LIM; }
    if (n > 0) {
        on  = td1c_rb[tdc_out];
        off = td0c_rb[tdc_out];
	tdc_out +=  1;
	if (tdc_out >= TDC_LIM) { tdc_out =  0; }
    }
    if (on == 48 && off == 0) {
        resetTxDotQueue() ;
	on =  0;
    } else {
        txOn_DnCntr =  on ; 
        txOffDnCntr =  off;
    }
    if (on < 0) { on = 0; }
    return(on);
}

static int last_key = 0;

int get_tx_key()
{
    int key = 0;
    if (key == 0) {
        key =  ((tcp_tx_cmd + 1) > 1) ? 1 : 0;
    }
    if (key == 0) {
        key =  tx_key_down_0;
    }
#ifdef TX_DISABLE
    key = 0;
#endif
    last_get_key_x =  key;
    return(key);
}

static float save_tx_ph	=  0.0;
static int   save_ditherIdx 	=  0;

void tx_block_setup(int seqN) {
    int skip_modulo = (sampRate / 48000);   //  1,2,4,8
    tx_dph = 2.0 * M_PI * cw_df / m_srate;

#ifdef NO_MODULO
    skip_modulo = 1;	// 
#endif
    if (((seqN + 0) % skip_modulo) == 0) {
        save_tx_ph      =  tx_ph ;	    // save starting phase
        save_ditherIdx  =  ditherIdx ; 
    } else {
        tx_ph           =  save_tx_ph ;	    // repeat last tx IQ phase
	ditherIdx       =  save_ditherIdx;
    }	
}

void get_tx_sample(int *tx_i, int *tx_q)
{
    if (txOn_DnCntr > 0) {
        txOn_DnCntr -= 1;
        if (txOn_DnCntr <= 0) {
	    tx_key_down_0 =  0;
	} else {
	    tx_key_down_0 =  1;
	}
    } else if (txOffDnCntr > 0) {
        txOffDnCntr -= 1;
        if (txOffDnCntr <= 0) {
	    int t =  getNextTxDotCommand();  
	    if (t > 0) { 		// getCmd sets txOn_DnCntr =  t;
	        tx_key_down_0 =  1;
	    } else {
	        tx_key_down_0 =  0; 	// getCmd sets txOffDnCntr
	    }
	}
    }
    if (tx_key_down_1 != last_key_down) {  //  MOXtx_enb
	if (tx_key_down_1 == 1) {
	    txRiseCntr = 255;
	} else {
	    txFallCntr = 255;
	}
    }
    if ((tx_key_down_1 == 1) || (txFallCntr > 0)) {
	float x =  cosf(tx_ph);
	float y =  sinf(tx_ph);
	tx_ph += tx_dph;			// 
	if (       tx_ph >  2.0f * (float)M_PI) {
	           tx_ph -= 2.0f * (float)M_PI;
	} else if (tx_ph < -2.0f * (float)M_PI) {
	           tx_ph += 2.0f * (float)M_PI;
	}

	float w = 1.0;
	// rise time taper
	if (txRiseCntr >  0) { 
	    w *=  txRiseWin[255 - txRiseCntr] ; 
	    txRiseCntr -= 1; 
	}
	if (txFallCntr >  0) {
	    w *=  txRiseWin[txFallCntr] ; 
	    txFallCntr -= 1; 
	}
	// w = 1.0;

	float x2  =  x * w * tx_lvl;
	float y2  =  y * w * tx_lvl;
	if (1 && tx_lvl > 0 && tx_lvl <= 512) {
	    x2  +=  ditherBuffer[ditherIdx  ];
	    y2  +=  ditherBuffer[ditherIdx+1];
	}
	ditherIdx = (ditherIdx + 2) & 0x7ffe;
#ifdef TX_DISABLE
	if (tx_i != NULL) { *tx_i =  0; }
	if (tx_q != NULL) { *tx_q =  0; }
#else
	if (tx_i != NULL) { *tx_i =  floor(0.499 + x2); }
	if (tx_q != NULL) { *tx_q =  floor(0.499 + y2); } 	// 
#endif
    }
    last_key_down  =  tx_key_down_1 ;
    tx_counter    +=  1;
}

int get_tx_drive() 
{
#ifdef TX_DISABLE
    return(0);
#else
    return(tx_drive);
#endif
}
#else	//  no TX_OK
#endif

int      get_tx_offset() { return(cw_df); }

//   The above source code is:
//   Copyright 2017,2020,2022 Ronald H Nicholson Jr. All Rights Reserved.
//   This code may only be redistributed under terms of
//   the Mozilla Public License 2.0 plus Exhibit B (no copyleft exception)
//   See: https://www.mozilla.org/en-US/MPL/2.0/

// eof
