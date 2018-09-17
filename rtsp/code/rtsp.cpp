// Scream sender side wrapper#include "sys/socket.h"#include "sys/types.h"#include "netinet/in.h"#include <string.h> /* needed for memset */#include <sys/socket.h>#include <netinet/in.h>#include <arpa/inet.h>#include <sys/time.h>#include <iostream>#include <pthread.h>#include <fcntl.h>#include <unistd.h>#include <sys/time.h>#include <signal.h>struct itimerval timer;struct sigaction sa;using namespace std;#define BUF_SIZE 10000char buf[BUF_SIZE];uint32_t ssrc = 0;// We don't bother about ssrc in this implementation, it is only one streamchar *ENCODER_IP = "192.168.0.10";struct sockaddr_in http_addr;int http_sock = 0;/** Create socket to Video encoder* for rate commands etc.*/int create_tcp_socket() {	int sock;	if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0){		cerr << "Can't create TCP socket" << endl;		exit(1);	}	return sock;}int setup_rtsp() {	http_addr.sin_family = AF_INET;	http_addr.sin_addr.s_addr = htonl(INADDR_ANY);	inet_aton(ENCODER_IP, (in_addr*)&http_addr.sin_addr.s_addr);	http_addr.sin_port = htons(554);	if (connect(http_sock, (struct sockaddr *)&http_addr, sizeof(struct sockaddr)) < 0){		cerr << "Could not connect to Video coder HTTP server" << endl;		exit(1);	}}bool verbose = true;const void sendRtspCommand() {  int sent = 0;  int tmpres = 0;	if (verbose) {    cerr << "--------------------------------------------" << endl;    cerr << "Send RTSP : " << endl << buf << endl;  }  bool errSend = false;  /*  * Send HTTP GET  */  while (sent < strlen(buf)) {    tmpres = send(http_sock, buf + sent, strlen(buf) - sent, 0);    if (tmpres == -1){      cerr << "Can't send RTSP" << endl;      errSend = true;    }    sent += tmpres;  }  if (true && !errSend) {    memset(buf, 0, sizeof(buf));    tmpres = recv(http_sock, buf, BUF_SIZE, 0);	  if (verbose) {      cerr << "response: " << endl << buf << endl;	  }  }  if (strstr(buf,"Bad Request")) {    close(http_sock);    cerr << "Dishonorable discharge due to malformed request" << endl;    exit(0);  }}long getTimeInUs(){  struct timeval tp;  gettimeofday(&tp, NULL);  long us = tp.tv_sec * 1000000 + tp.tv_usec;  return us;}// ROberts ugly hacks#include <signal.h> //  our new library volatile sig_atomic_t flag = 0;void my_function(int sig){ // can be called asynchronously  flag = 1; // set flag}// this fiunction below is badly hijacked by robert to support ctrl-cbool terminateRtsp = false;const void *keyboardThread(void *arg) {    cerr << "RTSP streaming started, press ctrl+c to stop" << endl;	//	cin.ignore();	signal(SIGINT, my_function);	 //  Which-Signal   |-- which user defined function registered  	while(1){    		if(flag){ // my action when signal set it 1        		printf("\n Signal caught!\n");        		printf("\n default action it not termination!\n");        		flag = 0;        		break;    		}		usleep(10000);	}  	//return 0;	terminateRtsp = true;}//#include <curses.h>char session[100];int iter = 0;int INCOMING_RTP_PORT = 30120;int main(int argc, char* argv[]) {  int ix = 1;  int cSeq = 1;  uint64_t t0 = getTimeInUs();  /*  * Parse command line  */  if (argc <= 1) {    cerr << "Usage : " << endl << " > rtsp encoder_ip client_rtp_port" << endl;    exit(-1);  }  ENCODER_IP = argv[ix];ix++;  INCOMING_RTP_PORT = atoi(argv[ix]);ix++;  http_sock = create_tcp_socket();  setup_rtsp();  sprintf(buf, "OPTIONS rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\n\r\n",ENCODER_IP,cSeq++);  sendRtspCommand();  sprintf(buf, "DESCRIBE rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nAccept: application/sdp\r\n\r\n",ENCODER_IP,cSeq++);  sendRtspCommand();  sprintf(buf, "SETUP rtsp://%s/MediaInput/h264/trackID=1 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nTransport: RTP/AVP;unicast;client_port=%d-%d\r\n\r\n",ENCODER_IP,cSeq++,INCOMING_RTP_PORT,INCOMING_RTP_PORT+1);  sendRtspCommand();  char ssrc[100];  if (strlen(buf) > 0) {    char *sp = strstr(buf,"ssrc=");		if (sp) {			strcpy(ssrc,sp+5);		}    sp = strstr(buf,"Session:");    sp = strtok(sp," ;\r\n");    sp = strtok(0," ;\r\n");    strcpy(session,sp);  }  sprintf(buf, "PLAY rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nSession: %s\r\nRange: npt=0.000-\r\n\r\n",ENCODER_IP,cSeq++,session);  sendRtspCommand();  sprintf(buf, "GET_PARAMETER rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nSession: %s\r\n\r\n",ENCODER_IP,cSeq++,session);  sendRtspCommand();  pthread_t keyboard_thread;    /* Create keyboard thread */  pthread_create(&keyboard_thread,NULL,keyboardThread,"Keybaord thread...");  verbose = false;  cerr << "Streaming started. IP:" << ENCODER_IP << " port:" << INCOMING_RTP_PORT << " SSRC:" << ssrc << endl;  while (!terminateRtsp) {    //if (getch() != 0) {    //  exit = true;    //}    sleep(1);    if (getTimeInUs()-t0 > 1000000) {			 iter++;			 if (iter % 30 == 0) {				 cerr << "Send GET_PARAMETER keep-alive message to keep RTSP session running.\n Press ENTER to stop RTSP" << endl;         sprintf(buf, "GET_PARAMETER rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nSession: %s\r\n\r\n",ENCODER_IP,cSeq++,session);         sendRtspCommand();		   }       t0 = getTimeInUs();    }  }  verbose = true;  sprintf(buf, "TEARDOWN rtsp://%s/MediaInput/h264 RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: Dummy SCReAM RTSP client\r\nSession: %s\r\n\r\n",ENCODER_IP,cSeq++,session);  sendRtspCommand();  close(http_sock);}