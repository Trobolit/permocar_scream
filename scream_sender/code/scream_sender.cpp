// Scream sender side wrapper
#include "ScreamTx.h"
#include "RtpQueue.h"
#include "sys/socket.h"
#include "sys/types.h"
#include "netinet/in.h"
#include <string.h> /* needed for memset */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <iostream>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <math.h>
struct itimerval timer;
struct sigaction sa;

using namespace std;

#define BUFSIZE 2048
#define MIN_PACE_INTERVAL 0.001
#define HTTP_BUF_SIZE 10000
#define MAX_SOURCES 6
#define KEEP_ALIVE_PKT_SIZE 1

int PT = 98;
int http_sock = 0;
int fd_out_rtp;
int fd_in_rtp[MAX_SOURCES];
ScreamTx *screamTx = 0;
RtpQueue *rtpQueue[MAX_SOURCES] = { 0, 0, 0, 0, 0, 0 };
int nSources = 0;
float delayTarget = 0.1f;

char *auth;

#define ECN_CAPABLE
/*
* ECN capable
* 0 = Not-ECT
* 1 = ECT(0)
* 2 = ECT(1)
*/
int ect = 0;

char *out_ip = "192.168.0.21";
int out_port = 30110;
char *in_ip[MAX_SOURCES];
int in_port[MAX_SOURCES];
uint32_t in_ssrc[MAX_SOURCES] = { 1, 2, 3, 4, 5, 6 };
uint32_t in_ssrc_network[MAX_SOURCES]; // Network ordered versions of in_ssrc
float priority[MAX_SOURCES] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
float congestionScaleFactor = 0.9;
int bytesInFlightHistSize = 5;
float maxRate = 8192e3f;

struct sockaddr_in in_rtp_addr[MAX_SOURCES];
struct sockaddr_in out_rtp_addr;
struct sockaddr_in in_rtcp_addr;
struct sockaddr_in http_addr;

socklen_t addrlen_out_rtp;
socklen_t addrlen_dummy_rtcp;
uint32_t lastLogT_ntp = 0;

socklen_t addrlen_in_rtp[MAX_SOURCES] = {
    sizeof(in_rtp_addr[0]),
    sizeof(in_rtp_addr[1]),
    sizeof(in_rtp_addr[2]),
    sizeof(in_rtp_addr[3]),
    sizeof(in_rtp_addr[4]),
    sizeof(in_rtp_addr[5]) };
socklen_t addrlen_in_rtcp = sizeof(in_rtcp_addr);
pthread_mutex_t lock_scream;
pthread_mutex_t lock_rtp_queue;

// Accumulated pace time, used to avoid starting very short pace timers
//  this can save some complexity at very higfh bitrates
float accumulatedPaceTime = 0.0f;
bool paceTimerRunning = false;

int lastQuantRate[MAX_SOURCES] = { 0, 0, 0, 0, 0, 0 };

const void sendCoderCommand(char *buf, char *ip);
void *txRtpThread(void *arg);
void *videoControlThread(void *arg);
int setup();
void *rxRtcpThread(void *arg);
void *rxRtpThread5(void *arg);
void *rxRtpThread4(void *arg);
void *rxRtpThread3(void *arg);
void *rxRtpThread2(void *arg);
void *rxRtpThread1(void *arg);
void *rxRtpThread0(void *arg);
//void trySendRtp(uint32_t time);
/*
long getTimeInUs(){
    struct timeval tp;
    gettimeofday(&tp, NULL);
    long us = tp.tv_sec * 1000000 + tp.tv_usec;
    return us;
}
*/
uint32_t t0; // = getTimeInUs();
uint32_t lastT_ntp = 0;

uint32_t getTimeInNtp(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  double time = tp.tv_sec + tp.tv_usec*1e-6-t0;
  uint64_t ntp64 = uint64_t(time*65536.0);
  uint32_t ntp = 0xFFFFFFFF & ntp64;
  return ntp;
}

volatile sig_atomic_t done = 0;
bool stopThread = false;
void stopAll(int signum)
{
    stopThread = true;
}

void closeSockets(int signum)
{
    exit(0);
}

/*
* Send a packet
*/
void sendPacket(char* buf, int size) {
    sendto(fd_out_rtp, buf, size, 0, (struct sockaddr *)&out_rtp_addr, sizeof(out_rtp_addr));
}

int main(int argc, char* argv[]) {

   struct timeval tp;
   gettimeofday(&tp, NULL);
   t0 = tp.tv_sec + tp.tv_usec*1e-6 - 1e-3;
   lastT_ntp = getTimeInNtp();lastLogT_ntp = lastT_ntp;
   /*
    * Parse command line
    */
    if (argc <= 1) {
        cerr << "Usage : " << endl << " scream_sender <options> auth nsources out_ip out_port in_ip_1 in_port_1 prio_1 .. in_ip_n in_port_n prio_n" << endl;
        cerr << " -ect n       : ECN capable transport, n = 1 or 2 for ECT(0) or ECT(1), 0 for not-ECT" << endl;
        cerr << " -scale val   : Congestion scale factor, range [0.5..1.0], default = 0.9" << endl;
        cerr << "                 it can be necessary to set scale 1.0 if the LTE modem drops packets" << endl;
        cerr << "                 already at low congestion levels." << endl;
        cerr << " -delaytarget : Sets a queue delay target (default = 0.1s) " << endl;
        cerr << " -cwvmem      : Sets the memory of the congestion window validation (default 5s), max 60s" << endl;
        cerr << "                 a larger memory can be beneficial in remote applications where the video input" << endl;
        cerr << "                 is static for long periods. " << endl;
        cerr << " -maxrate     : Set max rate [kbps], default 8192." << endl;
        cerr << " auth         : User authorization string base64 encoded version of user:password of IP camera" << endl;
        cerr << "                 use eg. the Linux command " << endl;
        cerr << "                 echo -n auser:apassword | base64 " << endl;
        cerr << " nsources     : Number of sources, min=1, max=" << MAX_SOURCES << endl;
        cerr << " out_ip       : remote (SCReAM receiver) IP address" << endl;
        cerr << " out_port     : remote (SCReAM receiver) port" << endl;
        cerr << " in_ip_1      : IP address for video coder 1 " << endl;
        cerr << " in_port_1    : port for RTP media from video coder 1 " << endl;
        cerr << " prio_1       : Bitrate priority for video coder 1, range [0.1..1.0] " << endl;
        cerr << " ." << endl;
        cerr << " ." << endl;
        cerr << " in_ip_n      : IP address for video coder n " << endl;
        cerr << " in_port_n    : port for RTP media from video coder n " << endl;
        cerr << " prio_n       : Bitrate priority for video coder n, range [0.1..1.0] " << endl;
        exit(-1);
    }
    int ix = 1;
    int nExpectedArgs = 2 + 2 + 1;
    while (strstr(argv[ix], "-")) {
        if (strstr(argv[ix], "-ect")) {
            ect = atoi(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            if (ect < 0 || ect > 2) {
                cerr << "ect must be 0, 1 or 2 " << endl;
                exit(0);
            }
        }
        if (strstr(argv[ix], "-scale")) {
            congestionScaleFactor = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
        }
        if (strstr(argv[ix], "-delaytarget")) {
            delayTarget = atof(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
        }
        if (strstr(argv[ix], "-cwvmem")) {
            bytesInFlightHistSize = atoi(argv[ix + 1]);
            ix += 2;
            nExpectedArgs += 2;
            if (bytesInFlightHistSize > kBytesInFlightHistSizeMax || bytesInFlightHistSize < 2) {
                cerr << "cwvmem must be in range [2 .. " << kBytesInFlightHistSizeMax << "]" << endl;
                exit(0);
            }
        }
        if (strstr(argv[ix], "-maxrate")) {
            maxRate = atoi(argv[ix + 1])*1000.0f;
            ix += 2;
            nExpectedArgs += 2;
        }
    }
    auth = argv[ix]; ix++;
    nSources = atoi(argv[ix]);
    nExpectedArgs += nSources * 3; // IP, port, prio
    ix++;
    if (nSources < 1 || nSources > MAX_SOURCES) {
        cerr << "number of sources must be in interval [0.." << MAX_SOURCES << "]" << endl;
        exit(0);
    }
    if (argc - 1 != nExpectedArgs - 1) {
        cerr << "expected " << (nExpectedArgs - 1) << " arguments, but see " << (argc - 1) << " ditto ?" << endl;
        exit(0);
    }
    out_ip = argv[ix]; ix++;
    out_port = atoi(argv[ix]); ix++;
    for (int n = 0; n < nSources; n++) {
        char s[20];
        in_ip[n] = argv[ix]; ix++;
        in_port[n] = atoi(argv[ix]); ix++;
        priority[n] = atof(argv[ix]); ix++;
    }

    if (setup() == 0)
        return 0;

    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = stopAll;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    char buf[HTTP_BUF_SIZE];

    cerr << "Configure media sources  " << endl;
    for (int n = 0; n < nSources; n++) {
        sprintf(buf, "GET /cgi-bin/set_h264?nr_h264_bandwidth=%d HTTP/1.0\r\nHost: %s\r\nAuthorization: Basic %s\r\n\r\n",
            1024, in_ip[n], auth);
        sendCoderCommand(buf, in_ip[n]);
        sprintf(buf, "GET /cgi-bin/set_h264?nr_h264_quality=9 HTTP/1.0\r\nHost: %s\r\nAuthorization: Basic %s\r\n\r\n",
            in_ip[n], auth);
        sendCoderCommand(buf, in_ip[n]);
    }

    cerr << "Scream sender started " << endl;
    pthread_mutex_init(&lock_scream, NULL);

    pthread_t tx_rtp_thread;
    pthread_t rx_rtp_thread[MAX_SOURCES];
    pthread_t rx_rtcp_thread;
    pthread_t video_thread;

    /* Create Transmit RTP thread */
    pthread_create(&tx_rtp_thread, NULL, txRtpThread, "RTCP thread...");
    cerr << "RX RTP thread(s) started" << endl;

    /* Create Receive RTP thread(s) */
    pthread_create(&rx_rtp_thread[0], NULL, rxRtpThread0, "RTP thread 0...");
    if (nSources > 1)
        pthread_create(&rx_rtp_thread[1], NULL, rxRtpThread1, "RTP thread 1...");
    if (nSources > 2)
        pthread_create(&rx_rtp_thread[2], NULL, rxRtpThread2, "RTP thread 2...");
    if (nSources > 3)
        pthread_create(&rx_rtp_thread[3], NULL, rxRtpThread3, "RTP thread 3...");
    if (nSources > 4)
        pthread_create(&rx_rtp_thread[4], NULL, rxRtpThread4, "RTP thread 4...");
    if (nSources > 5)
        pthread_create(&rx_rtp_thread[5], NULL, rxRtpThread5, "RTP thread 5...");
    cerr << "RX RTP thread(s) started" << endl;

    /* Create RTCP thread */
    pthread_create(&rx_rtcp_thread, NULL, rxRtcpThread, "RTCP thread...");
    cerr << "RTCP thread started" << endl;

    /* Create Video control thread */
    pthread_create(&video_thread, NULL, videoControlThread, "Video control thread...");
    cerr << "Media control thread started" << endl;

    while (!stopThread) {
        uint32_t time_ntp = getTimeInNtp();
        if (time_ntp - lastLogT_ntp > 13107) { // 0.2s in Q16
            char s[1000];
            char s1[1000];
            float time_s = (time_ntp) / 65536.0f;
            screamTx->getShortLog(time_s, s1);
            sprintf(s, "%8.3f, %s ", time_s, s1);
            cout << s << endl;


            /*
            * Send statistics to receiver this can be used to
            * verify reliability of remote control
            */
            s1[0] = 0x80;
            s1[1] = 0x7F; // Set PT = 0x7F for statistics packet
            memcpy(&s1[2], s, strlen(s));
            sendPacket(s1, strlen(s) + 2);

            /*
            * Send SSRC map to receiver to enable
            * correct mapping of cameras to displays
            */
            s1[0] = 0x80;
            s1[1] = 0x7E; // Set PT = 0x7E for SSRC map
            for (int n = 0; n < nSources; n++) {
                /*
                * Write the SSRCs (in network byte order)
                */
                uint32_t tmp_l;
                memcpy(s1 + 2 + n * 4, &in_ssrc_network[n], 4);
            }
            sendPacket(s1, 2 + nSources * 4);

            lastLogT_ntp = time_ntp;
        }

        usleep(10000);
    };

    usleep(500000);
    close(fd_out_rtp);
    for (int n = 0; n < nSources; n++)
        close(fd_in_rtp[n]);
}

/*
* Lotsalotsa functions...
*/

/*
* Quantize table for Pansonic WV-SBV111M at 1280*960 resolution
*/
int panasonicRates[] = { 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192 };
int panasonicQuality[] = { 9, 9, 9, 7, 5, 3, 3, 3, 1, 1, 1 };
int panasonicRatesN = 11;
/*
* Quantize the target bitrate to the closest value in a geometric sense
*/
int panasonicQuantizeRate(int rate) {
    int ix = 0;
    double minErr = 1e6;

    while (rate > panasonicRates[ix + 1])
        ix++;
    if (ix > 0) {

        double r1 = panasonicRates[ix];
        double r2 = panasonicRates[ix + 1];

        double d1 = (rate - r1) / (r2 - r1);
        double d2 = (r2 - rate) / (r2 - r1);

        if (d1 * 4 > d2)
            ix++;
    }
    return panasonicRates[ix];
}

/*
* Get the appropriate quality setting for the given target bitrate
*/
int panasonicGetQuality(int rate) {
    for (int n = 0; n < panasonicRatesN; n++) {
        if (rate == panasonicRates[n])
            return panasonicQuality[n];
    }
}

/*
  Extract the sequence number and the timestamp from the RTP header
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |V=2|P|X|  CC   |M|     PT      |       sequence number         |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |                           timestamp                           |
  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  |           synchronization source (SSRC) identifier            |
  +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
  |            contributing source -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  */
void parseRtp(unsigned char *buf, uint16_t* seqNr, uint32_t* timeStamp, unsigned char *pt) {
    uint16_t rawSeq;
    uint32_t rawTs;
    memcpy(&rawSeq, buf + 2, 2);
    memcpy(&rawTs, buf + 4, 4);
    memcpy(pt, buf + 1, 1);
    *seqNr = ntohs(rawSeq);
    *timeStamp = ntohl(rawTs);
}

/*
 * Transmit a packet if possible.
 * If not allowed due to packet pacing restrictions,
 * then sleep
 */
void *txRtpThread(void *arg) {
    int size;
    uint16_t seqNr;
    char buf[2000];
    uint32_t time_ntp = getTimeInNtp();
    int sleepTime_us = 10;
    float retVal = 0.0f;
    int sizeOfQueue;
    uint32_t ssrc;

    for (;;) {
        if (stopThread) {
            return NULL;
        }
        time_ntp = getTimeInNtp();
        sleepTime_us = 10;
        retVal = 0.0f;

        /*
        * Check if send window allows transmission and there is atleast one stream
        *  with RTP packets in queue
        */
        pthread_mutex_lock(&lock_scream);
        retVal = screamTx->isOkToTransmit(getTimeInNtp(), ssrc);
        pthread_mutex_unlock(&lock_scream);

        if (retVal != -1.0f) {
            /*
            * Send window allows transmission and atleast one stream has packets in RTP queue
            * Get RTP queue for selected stream (ssrc)
            */
            RtpQueue *rtpQueue = (RtpQueue*)screamTx->getStreamQueue(ssrc);

            pthread_mutex_lock(&lock_rtp_queue);
            sizeOfQueue = rtpQueue->sizeOfQueue();
            pthread_mutex_unlock(&lock_rtp_queue);
            do {
                if (retVal == -1.0f) {
                    sizeOfQueue = 0;
                }
                else {
                    if (retVal > 0.0f)
                        accumulatedPaceTime += retVal;
                    if (retVal != -1.0 && accumulatedPaceTime <= MIN_PACE_INTERVAL) {
                        /*
                        * Get RTP packet from the selected RTP queue
                        */
                        pthread_mutex_lock(&lock_rtp_queue);
                        rtpQueue->pop(buf, size, seqNr);
                        pthread_mutex_unlock(&lock_rtp_queue);

                        /*
                        * Transmit RTP packet
                        */
                        sendPacket(buf, size);

                        /*
                        * Register transmitted RTP packet
                        */
                        pthread_mutex_lock(&lock_scream);
                        retVal = screamTx->addTransmitted(getTimeInNtp(), ssrc, size, seqNr);
                        pthread_mutex_unlock(&lock_scream);
                    }

                    /*
                    * Check if send window allows transmission and there is atleast one stream
                    *  with RTP packets in queue
                    */
                    retVal = screamTx->isOkToTransmit(getTimeInNtp(), ssrc);
                    if (retVal == -1.0f) {
                        /*
                        * Send window full or no packets in any RTP queue
                        */
                        sizeOfQueue = 0;
                    }
                    else {
                        /*
                        * Send window allows transmission and atleast one stream has packets in RTP queue
                        * Get RTP queue for selected stream (ssrc)
                        */
                        rtpQueue = (RtpQueue*)screamTx->getStreamQueue(ssrc);
                        pthread_mutex_lock(&lock_rtp_queue);
                        sizeOfQueue = rtpQueue->sizeOfQueue();
                        pthread_mutex_unlock(&lock_rtp_queue);
                    }
                }
            } while (accumulatedPaceTime <= MIN_PACE_INTERVAL &&
                retVal != -1.0f &&
                sizeOfQueue > 0);

            if (accumulatedPaceTime > 0) {
                /*
                * Sleep for a while, this paces out packets
                */
                sleepTime_us = int(accumulatedPaceTime*1e6f);
                accumulatedPaceTime = 0.0f;
            }
        }
        usleep(sleepTime_us);
    }
    return NULL;
}


int recvRtp(unsigned char *buf_rtp, int ix) {
    /*
    * Wait for RTP packets from the coder
    */
    int recvlen = recvfrom(fd_in_rtp[ix],
        buf_rtp,
        BUFSIZE,
        0,
        (struct sockaddr *)&in_rtp_addr[ix], &addrlen_in_rtp[ix]);
    if (stopThread)
        return 0;

    return recvlen;
}

void processRtp(unsigned char *buf_rtp, int recvlen, int ix) {
    uint16_t seqNr;
    uint32_t ts;
    unsigned char pt;

    parseRtp(buf_rtp, &seqNr, &ts, &pt);
    uint32_t pt_ = pt & 0x7F;
    if ((pt & 0x7F) == PT) {
        /*
        * Overwrite SSRC with new value
        * The Panasonic IP cameras suffer from some kind of birthday problem
        * with the result that they can all generate the same SSRC.
        * Therefore we overwrite the SSRC with new values
        */
        //uint32_t tmp;
        //memcpy(&tmp, &in_ssrc_network[ix], 4); 
        //cout << "rewriting ssrc!\n" << ntohl(tmp) << endl;
	
	memcpy(&buf_rtp[8], &in_ssrc_network[ix], 4);
        pthread_mutex_lock(&lock_rtp_queue);
        rtpQueue[ix]->push(buf_rtp, recvlen, seqNr, (getTimeInNtp())/65536.0f);
        pthread_mutex_unlock(&lock_rtp_queue);

        pthread_mutex_lock(&lock_scream);
        screamTx->newMediaFrame(getTimeInNtp(), in_ssrc[ix], recvlen);
        pthread_mutex_unlock(&lock_scream);
    }
}

/*
* One thread for each media source (camera)
*/
void *rxRtpThread0(void *arg) {
    cout << "debug1" << endl;
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 0);
        if (len > 0) {
            processRtp(buf_rtp, len, 0);
        }
    }
    return NULL;
}
void *rxRtpThread1(void *arg) {
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 1);
        if (len > 0) {
            processRtp(buf_rtp, len, 1);
        }
    }
    return NULL;
}
void *rxRtpThread2(void *arg) {
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 2);
        if (len > 0) {
            processRtp(buf_rtp, len, 2);
        }
    }
    return NULL;
}
void *rxRtpThread3(void *arg) {
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 3);
        if (len > 0) {
            processRtp(buf_rtp, len, 3);
        }
    }
    return NULL;
}
void *rxRtpThread4(void *arg) {
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 4);
        if (len > 0) {
            processRtp(buf_rtp, len, 4);
        }
    }
    return NULL;
}
void *rxRtpThread5(void *arg) {
    unsigned char buf_rtp[BUFSIZE];
    for (;;) {
        int len = recvRtp(buf_rtp, 5);
        if (len > 0) {
            processRtp(buf_rtp, len, 5);
        }
    }
    return NULL;
}

void *rxRtcpThread(void *arg) {
    /*
    * Wait for RTCP packets from receiver
    */
    unsigned char buf_rtcp[BUFSIZE];
    for (;;) {
        int recvlen = recvfrom(fd_out_rtp, buf_rtcp, BUFSIZE, 0, (struct sockaddr *)&in_rtcp_addr, &addrlen_in_rtcp);
        if (stopThread)
            return;

        if (recvlen > KEEP_ALIVE_PKT_SIZE) {
//cout << "1 " ;
            pthread_mutex_lock(&lock_scream);
            screamTx->incomingStandardizedFeedback(getTimeInNtp(), buf_rtcp, recvlen);
            pthread_mutex_unlock(&lock_scream);
//cout << "2 " <<  endl;	
        }
    }
    return NULL;
}

int setup() {
    for (int n = 0; n < nSources; n++) {
        in_ssrc_network[n] = htonl(in_ssrc[n]);
        in_rtp_addr[n].sin_family = AF_INET;
        in_rtp_addr[n].sin_addr.s_addr = htonl(INADDR_ANY);
        in_rtp_addr[n].sin_port = htons(in_port[n]);
        if ((fd_in_rtp[n] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            char s[100];
            sprintf(s, "cannot create socket for incoming RTP media %d", n + 1);
            perror(s);
            return 0;
        }
        if (bind(fd_in_rtp[n], (struct sockaddr *)&in_rtp_addr[n], sizeof(in_rtp_addr[n])) < 0) {
            char s[100];
            sprintf(s, "bind incoming_rtp_addr %d failed", n + 1);
            perror(s);
            return 0;
        }
        else{
            cerr << "Listen on port " << in_port[n] << " to receive RTP media " << (n + 1) << endl;
        }
    }

    memset(&out_rtp_addr, 0, sizeof(struct sockaddr_in));
    out_rtp_addr.sin_family = AF_INET;
    inet_aton(out_ip, (in_addr*)&out_rtp_addr.sin_addr.s_addr);
    out_rtp_addr.sin_port = htons(out_port);
    addrlen_out_rtp = sizeof(out_rtp_addr);

    if ((fd_out_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create socket for outgoing RTP media");
        return 0;
    }

    /*
    * Set send buf reasonably high to avoid socket blocking
    */
    int sendBuff = 1000000;
    int res = setsockopt(fd_out_rtp, SOL_SOCKET, SO_SNDBUF, &sendBuff, sizeof(sendBuff));

    /*
    * Set ECN capability for outgoing socket using IP_TOS
    */
#ifdef ECN_CAPABLE
    int iptos = ect; // Check with wireshark
    res = setsockopt(fd_out_rtp, IPPROTO_IP, IP_TOS, &iptos, sizeof(iptos));
    if (res < 0) {
        cerr << "Not possible to set ECN bits" << endl;
    }
    int tmp = 0;
    res = getsockopt(fd_out_rtp, IPPROTO_IP, IP_TOS, &tmp, sizeof(tmp));
    if (iptos == tmp) {
        cerr << "ECN set successfully" << endl;
    }
    else {
        cerr << "ECN bits _not_ set successfully ? " << iptos << " " << tmp << endl;
    }
#endif
    /*
    * Socket for incoming RTP media
    */
    in_rtcp_addr.sin_family = AF_INET;
    in_rtcp_addr.sin_port = htons(out_port);
    in_rtcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd_out_rtp, (struct sockaddr *)&in_rtcp_addr, sizeof(in_rtcp_addr)) < 0) {
        perror("bind outgoing_rtp_addr failed");
        return 0;
    }
    else {
        cerr << "Listen on port " << out_port << " to receive RTCP from encoder " << endl;
    }

    screamTx = new ScreamTx(congestionScaleFactor,
        congestionScaleFactor,
        delayTarget,
        false,
        1.0f,
        10.0f,
        12500,
        false,
        bytesInFlightHistSize,
        false,
        false);
    for (int n = 0; n < nSources; n++) {
        rtpQueue[n] = new RtpQueue();

        screamTx->registerNewStream(rtpQueue[n],
            in_ssrc[n], priority[n],
            256e3f, 1000e3f, maxRate, 500e5f,
            0.5f, 0.1f, 0.05f,
            congestionScaleFactor, congestionScaleFactor);
    }
    return 1;
}

/*
* Create socket to Video encoder
* for rate commands etc.
*/
int create_tcp_socket() {
    int sock;
    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0){
        cerr << "Can't create TCP socket" << endl;
        exit(1);
    }
    return sock;
}

int setup_http(char *ip) {
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    inet_aton(ip, (in_addr*)&http_addr.sin_addr.s_addr);
    http_addr.sin_port = htons(80);
    if (connect(http_sock, (struct sockaddr *)&http_addr, sizeof(struct sockaddr)) < 0){
        cerr << "Could not connect to Video coder HTTP server " << ip << endl;
        exit(1);
    }
}

/*
* Send rate change request [bps]
*/
const void sendCoderCommand(char *buf, char *ip) {
    http_sock = create_tcp_socket();
    setup_http(ip);
    //Send the query to the server
    int sent = 0;
    int tmpres = 0;
    //cerr << "Send HTTP : " << buf << endl;
    bool errSend = false;
    /*
    * Send HTTP GET
    */
    while (sent < strlen(buf)) {
        tmpres = send(http_sock, buf + sent, strlen(buf) - sent, 0);
        if (tmpres == -1){
            cerr << "Can't send HTTP GET" << endl;
            errSend = true;
        }
        sent += tmpres;
    }
    if (true && !errSend) {
        memset(buf, 0, sizeof(buf));
        tmpres = recv(http_sock, buf, HTTP_BUF_SIZE, 0);
        if (tmpres > 0) {
            //cout << "HTTP response: " << buf << endl;
        }
    }
    close(http_sock);
}

void *videoControlThread(void *arg) {

    char buf[HTTP_BUF_SIZE];
    while (!stopThread) {
        for (int n = 0; n < nSources; n++) {
            /*
            * Poll rate change for all media sources
            */
            float rate = screamTx->getTargetBitrate(in_ssrc[n]);
            if (rate > 0) {
                int rateQ = (int)(std::min(8192.0f, rate / 1000.0f) + 0.5f);
                rateQ = panasonicQuantizeRate(rateQ);
                if (lastQuantRate[n] != rateQ) {
                    /*
                    * HTTP access to media coder is slow, send command only if rate is changed
                    */
                    lastQuantRate[n] = rateQ;
                    sprintf(buf, "GET /cgi-bin/set_h264?nr_h264_bandwidth=%d HTTP/1.0\r\nHost: %s\r\nAuthorization: Basic %s\r\n\r\n",
                        rateQ, in_ip[n], auth);
                    sendCoderCommand(buf, in_ip[n]);
                    sprintf(buf, "GET /cgi-bin/set_h264?nr_h264_quality=%d HTTP/1.0\r\nHost: %s\r\nAuthorization: Basic %s\r\n\r\n",
                        panasonicGetQuality(rateQ), in_ip[n], auth);
                    sendCoderCommand(buf, in_ip[n]);
                }
            }
        }
        usleep(10000);
    }
}
