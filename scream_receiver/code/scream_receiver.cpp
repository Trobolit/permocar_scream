// Scream sender side wrapper
#include "ScreamRx.h"
#include "sys/socket.h"
#include "sys/types.h"
#include "netinet/in.h"
#include <string.h> /* needed for memset */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
using namespace std;

/*
* Scream receiver side wrapper
* Receives SCReAM congestion controlled RTP media and
*  generates RTCP feedback to the sender, over the same RTP port
* Media sources (max 6) are demultiplexed and forwarded to local RTP ports
*  given by local_port list
*/

#define BUFSIZE 2048

#define MAX_SOURCES 6
uint32_t SSRC_RTCP=10;


// Input UDP socket, RTP packets come here and we send RTCP packets in the
// reverse direction through this socket
int fd_in_rtp;

ScreamRx *screamRx = 0;


int fd_local_rtp[MAX_SOURCES];
uint32_t ssrcMap[MAX_SOURCES];

char* in_ip = "10.10.10.2";
int in_port = 30110;
struct sockaddr_in in_rtp_addr, out_rtcp_addr, sender_rtcp_addr;
struct sockaddr_in local_rtp_addr[MAX_SOURCES];

char* local_ip[MAX_SOURCES] =
   {"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1",
	"127.0.0.1"};

int local_port[MAX_SOURCES] = {30112,30114,30116,30118,30120,30122};
int nSources = 0;

pthread_mutex_t lock_scream;

double t0 = 0;

/*
* Time in 32 bit NTP format
* 16 most  significant bits are seconds
* 16 least significant bits is fraction
*/
uint32_t getTimeInNtp(){
  struct timeval tp;
  gettimeofday(&tp, NULL);
  double time = (tp.tv_sec + tp.tv_usec*1e-6)-t0;
  uint64_t ntp64 = uint64_t(time*65536);
  uint32_t ntp = 0xFFFFFFFF & (ntp64); // NTP in Q16
  return ntp;
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
|            contributing source (CSRC) identifiers             |
|                             ....                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

void parseRtp(unsigned char *buf, uint16_t* seqNr, uint32_t* timeStamp, uint32_t* ssrc) {
  uint16_t tmp_s;
  uint32_t tmp_l;
  memcpy(&tmp_s, buf + 2, 2);
  *seqNr = ntohs(tmp_s);
  memcpy(&tmp_l, buf + 4, 4);
  *timeStamp  = ntohl(tmp_l);
  memcpy(&tmp_l, buf + 8, 4);
  *ssrc  = ntohl(tmp_l);
}

uint32_t lastPunchNatT_ntp = 0;

#define KEEP_ALIVE_PKT_SIZE 1

void *rtcpPeriodicThread(void *arg) {
  unsigned char buf[BUFSIZE];
  int rtcpSize;
  uint32_t rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();
  for (;;) {
    uint32_t time_ntp = getTimeInNtp();

    if (getTimeInNtp() - lastPunchNatT_ntp > 32768) { // 0.5s in !16
      /*
      * Send a small packet just to punch open a hole in the NAT,
      *  just one single byte will do.
      * This makes in possible to receive packets on the same port
      */
      int ret = sendto(fd_in_rtp, buf, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
      lastPunchNatT_ntp = getTimeInNtp();
      cerr << "." << endl;
    }

    if (screamRx->isFeedback(time_ntp) &&
         (screamRx->checkIfFlushAck() ||
         (time_ntp - screamRx->getLastFeedbackT() > rtcpFbInterval_ntp))) {
      rtcpFbInterval_ntp = screamRx->getRtcpFbInterval();

      pthread_mutex_lock(&lock_scream);
      screamRx->createStandardizedFeedback(getTimeInNtp(), buf, rtcpSize);
      pthread_mutex_unlock(&lock_scream);
      sendto(fd_in_rtp, buf, rtcpSize, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
      lastPunchNatT_ntp = getTimeInNtp();
    }
    usleep(500);
  }
}

#define MAX_CTRL_SIZE 8192
#define MAX_BUF_SIZE 65536
#define ALL_CODE

int main(int argc, char* argv[])
{
  struct timeval tp;
  gettimeofday(&tp, NULL);
  t0 = (tp.tv_sec + tp.tv_usec*1e-6)-1e-3;

  unsigned char bufRtp[BUFSIZE];
  if (argc <= 1) {
    cerr << "Usage :" << endl << " >scream_receiver incoming_ip incoming_port <forward_ip forward_port ...>" << endl;
    cerr << "   forward ip and port pairs are specified for the case that RTP packets should " << endl;
    cerr << "   be forwarded for rendering in another machine, for example: " << endl;
    cerr << "  > scream_receiver 10.10.10.2 30110 10.10.10.12 30112 10.10.10.13 30114 " << endl;
    cerr << "   dictates that the 1st stream is forwarded to 10.10.10.12:30112 and " << endl;
    cerr << "   the 2nd stream is forwarded to 10.10.10.13:30114 " << endl;

    exit(-1);
  }
  in_ip = argv[1];
  in_port = atoi(argv[2]);
  if (argc >= 4) {
	// RTP packets should be forwarded to ip port pairs
	int N = (argc-3)/2;
	for (int n=0; n < N; n++) {
		local_ip[n] = argv[n*2+3];
		local_port[n] = atoi(argv[n*2+4]);
		printf("port to forward: %d\n", local_port[n]);
    }
  }


  screamRx = new ScreamRx(SSRC_RTCP);

  in_rtp_addr.sin_family = AF_INET;
  in_rtp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  in_rtp_addr.sin_port = htons(in_port);

  if ((fd_in_rtp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("cannot create socket for incoming RTP packets");
    return 0;
  }

  out_rtcp_addr.sin_family = AF_INET;
  inet_aton(in_ip,(in_addr*) &out_rtcp_addr.sin_addr.s_addr);
  out_rtcp_addr.sin_port = htons(in_port);

  for (int n=0; n < MAX_SOURCES; n++) {
    local_rtp_addr[n].sin_family = AF_INET;
    inet_aton(local_ip[n],(in_addr*) &local_rtp_addr[n].sin_addr.s_addr);
    local_rtp_addr[n].sin_port = htons(local_port[n]);
    if ((fd_local_rtp[n] = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
      perror("cannot create socket for outgoing RTP packets to renderer (video decoder)");
      return 0;
    }
  }

  int enable = 1;
  if (setsockopt(fd_in_rtp, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }
  unsigned char set = 0x03;
  if (setsockopt(fd_in_rtp, IPPROTO_IP, IP_RECVTOS, &set,sizeof(set)) < 0) {
    cerr << "cannot set recvtos on incoming socket" << endl;
  } else {
    cerr << "socket set to recvtos" << endl;
  }

  if (bind(fd_in_rtp, (struct sockaddr *)&in_rtp_addr, sizeof(in_rtp_addr)) < 0) {
    perror("bind incoming_rtp_addr failed");
    return 0;
  } else{
    cerr << "Listen on port " << in_port <<" to receive RTP from sender, this is the new version " << endl;
  }

  struct sockaddr_in sender_rtp_addr;
  socklen_t addrlen_sender_rtp_addr = sizeof(sender_rtp_addr);

  int recvlen;

  uint32_t last_received_time_ntp = 0;
  uint32_t receivedRtp = 0;

  /*
  * Send a small packet just to punch open a hole in the NAT,
  *  just one single byte will do.
  * This makes in possible to receive packets on the same port
  */
  sendto(fd_in_rtp, bufRtp, KEEP_ALIVE_PKT_SIZE, 0, (struct sockaddr *)&out_rtcp_addr, sizeof(out_rtcp_addr));
  lastPunchNatT_ntp = getTimeInNtp();

  pthread_t rtcp_thread;
  pthread_mutex_init(&lock_scream, NULL);
  pthread_create(&rtcp_thread, NULL, rtcpPeriodicThread, "Periodic RTCP thread...");

  int *ecnptr;
  unsigned char received_ecn;

  struct msghdr rcv_msg;
  struct iovec rcv_iov[1];
  char rcv_ctrl_data[MAX_CTRL_SIZE];
  char rcv_buf[MAX_BUF_SIZE];

   /* Prepare message for receiving */
  rcv_iov[0].iov_base = rcv_buf;
  rcv_iov[0].iov_len = MAX_BUF_SIZE;

  rcv_msg.msg_name = NULL;	// Socket is connected
  rcv_msg.msg_namelen = 0;
  rcv_msg.msg_iov = rcv_iov;
  rcv_msg.msg_iovlen = 1;
  rcv_msg.msg_control = rcv_ctrl_data;
  rcv_msg.msg_controllen = MAX_CTRL_SIZE;

  for (;;) {
    /*
    * Wait for incoing RTP packet, this call can be blocking
    */

    /*
    * Extract ECN bits
    */
    int recvlen = recvmsg(fd_in_rtp, &rcv_msg, 0);
    bool isEcnCe = false;
    if (recvlen == -1) {
	    perror("recvmsg()");
	    close(fd_in_rtp);
	    return EXIT_FAILURE;
    } else {
	    struct cmsghdr *cmptr;
	    int *ecnptr;
	    unsigned char received_ecn;
	    for (cmptr = CMSG_FIRSTHDR(&rcv_msg);
			  cmptr != NULL;
			  cmptr = CMSG_NXTHDR(&rcv_msg, cmptr)) {
		    if (cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_TOS) {
			    ecnptr = (int*)CMSG_DATA(cmptr);
			    received_ecn = *ecnptr;
          if (received_ecn == 0x3)
            isEcnCe = true;
		    }
	    }
      memcpy(bufRtp,rcv_msg.msg_iov[0].iov_base,recvlen);
    }
    uint32_t time_ntp = getTimeInNtp();
    if (recvlen > 1) {
      if (bufRtp[1] == 0x7F) {
        // Packet contains statistics
        recvlen -= 2; // 2 bytes
        char s[1000];
        memcpy(s,&bufRtp[2],recvlen);
        s[recvlen] = 0x00;
        cout << s << endl;
      } else if (bufRtp[1] == 0x7E) {
        // Packet contains an SSRC map
	nSources = (recvlen-2)/4;
        for (int n=0; n < nSources; n++) {
          uint32_t tmp_l;
          memcpy(&tmp_l, bufRtp+2+n*4, 4);
          ssrcMap[n] = ntohl(tmp_l);
	}
      } else {
        if (time_ntp - last_received_time_ntp > 131072) { // 2s in Q16
          /*
          * It's been more than 5 seconds since we last received an RTP packet
          *  let's reset everything to be on the safe side
          */
          receivedRtp = 0;
          pthread_mutex_lock(&lock_scream);
          delete screamRx;
          screamRx = new ScreamRx(SSRC_RTCP);
          pthread_mutex_unlock(&lock_scream);
          cerr << "Receiver state reset due to idle input" << endl;
        }
        last_received_time_ntp = time_ntp;
        receivedRtp++;

        /*
        * Parse RTP header
        */
        uint16_t seqNr;
        uint32_t ts;
        uint32_t ssrc;
        parseRtp(bufRtp,&seqNr, &ts, &ssrc);

        /*
        * Map the RTP packets to correct display by means of the ssrcMap
        */
        int ix = -1;
        for (int n=0; n < nSources; n++) {
           if (ssrc == ssrcMap[n]) {
             ix = n;
             break;
           }
        }
        if (ix != -1) {
          /*
          * Forward RTP packet to the correct internal port, for e.g GStreamer playout
          */
          sendto(fd_local_rtp[ix], bufRtp, recvlen, 0, (struct sockaddr *)&local_rtp_addr[ix], sizeof(local_rtp_addr[ix]));
	}

        /*
        * Register received RTP packet with ScreamRx
        */
        pthread_mutex_lock(&lock_scream);
        screamRx->receive(getTimeInNtp(), 0, ssrc, recvlen, seqNr, isEcnCe);
        pthread_mutex_unlock(&lock_scream);
      }
    }
  }
}
