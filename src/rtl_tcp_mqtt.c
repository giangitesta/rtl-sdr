/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012-2013 by Hoernchen <la@tfc-server.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#include "getopt/getopt.h"
#endif

#include <pthread.h>
#include <MQTTClient.h>
#include "rtl-sdr.h"
#include "convenience/convenience.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")

typedef int socklen_t;

#else
#define closesocket close
#define SOCKADDR struct sockaddr
#define SOCKET int
#define SOCKET_ERROR -1
#endif

static SOCKET s;

static pthread_t mqtt_worker_thread;
static pthread_t tcp_worker_thread;
static pthread_t command_thread;

//static pthread_cond_t pub_cond;
//static pthread_mutex_t pub_cond_lock;

static pthread_mutex_t param_lock;

static pthread_cond_t exit_cond;
static pthread_mutex_t exit_cond_lock;

static pthread_mutex_t ll_mutex;
static pthread_cond_t cond;

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

static rtlsdr_dev_t *dev = NULL;


static int global_numq = 0;
static struct llist *ll_buffers = 0;
static int llbuf_num = 500;

static volatile int do_exit = 0;
static volatile int do_mqtt_exit = 0;


typedef struct command_msg {
	unsigned char cmd;
	unsigned int param; 
} command_msg;

// struct node - struttura di un nodo della coda thread-safe
typedef struct node {
    //int value;
	struct command_msg value;
    struct node *next;
} node;
// struct Queue_r - struttura della coda thread-safe (usa una linked list)
typedef struct {
    node *front;
    node *rear;
    pthread_mutex_t mutex;
	pthread_cond_t cond;
} Queue_r;

// qcreate() - crea una coda vuota
Queue_r* qcreate()
{
    // crea la coda
    Queue_r *queue = malloc(sizeof(Queue_r));
    // inizializza la coda
    queue->front = NULL;
    queue->rear  = NULL;
    pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->cond, NULL);

    return queue;
}

// enqueue() - aggiunge un elemento alla coda
void enqueue(Queue_r* queue, unsigned char cmd, unsigned int param)
{
    // crea un nuovo nodo
    node *temp = malloc(sizeof(struct node));
    temp->value.cmd = cmd;
	temp->value.param = param;
    temp->next  = NULL;
    // blocco l'accesso
    pthread_mutex_lock(&queue->mutex);
    // test se la coda è vuota
    if (queue->front == NULL) {
        // con la coda vuota front e rear coincidono
        queue->front = temp;
        queue->rear  = temp;
    }
    else {
        // aggiungo un elemento
        node *old_rear = queue->rear;
        old_rear->next = temp;
        queue->rear    = temp;
    }

	//signal creation of new node to consumer
	pthread_cond_signal(&queue->cond);

    // sblocco l'accesso ed esco
    pthread_mutex_unlock(&queue->mutex);
}

// dequeue() - toglie un elemento dalla coda
bool dequeue(Queue_r* queue, unsigned char *cmd, unsigned int *param)
{
	// blocco l'accesso
    pthread_mutex_lock(&queue->mutex);
    // test se la coda è vuota
    node *front = queue->front;
    if (front == NULL) {
        // sblocco l'accesso ed esco
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    // leggo il valore ed elimino l'elemento dalla coda
    *cmd = front->value.cmd;
	*param = front->value.param;
    queue->front = front->next;
    free(front);
    // sblocco l'accesso ed esco
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

// dequeue() - toglie un elemento dalla coda con timeout
int dequeue_with_timeout(Queue_r* queue, int timeout, unsigned char *cmd, unsigned int *param)
{
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	int r = -1;

	int ret = -1;

	gettimeofday(&tp, NULL);
	ts.tv_sec  = tp.tv_sec + timeout;
	ts.tv_nsec = tp.tv_usec * 1000;

	pthread_mutex_lock(&queue->mutex);
	r = pthread_cond_timedwait(&queue->cond, &queue->mutex, &ts);
	if(r == ETIMEDOUT) { //TIMEOUT
		*cmd = 0xff;
		*param = 0;	
  		ret = 1;	
	} else if (r == 0) { //SIGNALED 
		// test se la coda è vuota
		node *front = queue->front;
		if (front == NULL) {
			//coda vuota
			ret = 2;
		} else {
			// coda piena - leggo il valore ed elimino l'elemento dalla coda
			*cmd = front->value.cmd;
			*param = front->value.param;
			queue->front = front->next;
			free(front);
			ret=0;
		}
	}
	//sblocco accesso
	pthread_mutex_unlock(&queue->mutex);
	return(ret);
}



typedef struct {
	int index;
	char *name;
	char *product;
	char *vendor;
	char *serial; 

} device_info_t;


typedef struct {
	uint32_t frequency;
	uint32_t samp_rate;
	int tuner_gain_mode;
	int tuner_gain;
	int agc_mode;
	int direct_sampling;
	int offset_tuning;
	int rtl_xtal_freq;
	int tuner_xtal_freq;
	int gain_by_index;
	int enable_biastee;
	int ppm_error;
} rtl_params_t;

typedef struct {
	char* uri;
	char* base_topic;
	char* tele_topic;
	char* stat_topic;
	char* lwt_topic;
	char* client_id;
	int tele_period;
	int qos;
} mqtt_params_t;

typedef struct {
	char* addr;
	char* port;
	char* client_ip;
} server_params_t;

static rtl_params_t *rtl_p = NULL;
static mqtt_params_t *mqtt_p = NULL;
static server_params_t *server_p = NULL;
static device_info_t *di_p = NULL;

static MQTTClient mqtt_client;

static Queue_r *my_queue = NULL;


void usage(void)
{
	printf("rtl_tcp, an I/Q spectrum server for RTL2832 based DVB-T receivers\n\n"
		"Usage:\t[-a listen address]\n"
		"\t[-p listen port (default: 1234)]\n"
		"\t[-f frequency to tune to [Hz]]\n"
		"\t[-g gain (default: 0 for auto)]\n"
		"\t[-s samplerate in Hz (default: 2048000 Hz)]\n"
		"\t[-b number of buffers (default: 15, set by library)]\n"
		"\t[-n max number of linked list buffers to keep (default: 500)]\n"
		"\t[-d device index (default: 0)]\n"
		"\t[-P ppm_error (default: 0)]\n"
		"\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n"
		"\t[-h Mqtt URI address]\n"
		"\t[-t Mqtt topic]\n"
		"\t[-c Mqtt client ID]\n"
		"\t[-y Mqtt Telemetry period\n");
	exit(1);
}





#ifdef _WIN32
int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
#ifdef _MSC_VER
		tmp -= 11644473600000000Ui64;
#else
		tmp -= 11644473600000000ULL;
#endif
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	rtlsdr_cancel_async(dev);
	do_exit = 1;
}
#endif

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	if(!do_exit) {
		struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
		rpt->data = (char*)malloc(len);
		memcpy(rpt->data, buf, len);
		rpt->len = len;
		rpt->next = NULL;

		pthread_mutex_lock(&ll_mutex);

		if (ll_buffers == NULL) {
			ll_buffers = rpt;
		} else {
			struct llist *cur = ll_buffers;
			int num_queued = 0;

			while (cur->next != NULL) {
				cur = cur->next;
				num_queued++;
			}

			if(llbuf_num && llbuf_num == num_queued-2){
				struct llist *curelem;

				free(ll_buffers->data);
				curelem = ll_buffers->next;
				free(ll_buffers);
				ll_buffers = curelem;
			}

			cur->next = rpt;

			if (num_queued > global_numq)
				printf("ll+, now %d\n", num_queued);
			else if (num_queued < global_numq)
				printf("ll-, now %d\n", num_queued);

			global_numq = num_queued;
		}
		pthread_cond_signal(&cond);
		pthread_mutex_unlock(&ll_mutex);
	}
}


static int publishLastWillTestamentMessage(char* lwt_topic, char* payload)
{
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
  	MQTTClient_deliveryToken token;
	int rc;

	pubmsg.payload = payload;
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = 1;
    pubmsg.retained = 1;
    MQTTClient_publishMessage(mqtt_client, lwt_topic, &pubmsg, &token);

	rc = MQTTClient_waitForCompletion(mqtt_client, token, 10000);
	return (rc);
}


/*
void createTelemetryPayload(radio_params_t* rp, char **payload)
{

	char op[255];
	switch(rp->op_state)
	{
		case UNKNOW:
			strcpy(op,"UNKNOW");
			break;
		case IDLE:
			strcpy(op, "WAITING");
			break;
		case RUNNING:
			strcpy(op, "RUNNING");
			break;
		default:
			strcpy(op, "");
	};


	*payload =  malloc(sizeof(char) * 2048);
	sprintf (*payload, 
  	        "{ \"dev_id\" : %d,\"dev_name\" : \"%s\",\"dev_serial\" : \"%s\", \"client_ip\" : \"%s\", \"op_mode\" : \"%s\", \"frequency\" : %u, \"agc_mode\" : %d, \"gain\" : %d, \"sample rate\" : %u, \"ppm error\" : %d }", 
			   rp->dev_index,
			   rp->dev_name,
			   rp->dev_serial,
			   rp->client_ip,
			   op,
			   rp->frequency,
			   rp->tuner_gain_mode,
			   rp->tuner_gain, 
			   rp->samp_rate,
			   rp->ppm_error);
}
*/


/*
static int publishTelemetryMessage(radio_params_t *rp)
{
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
  	MQTTClient_deliveryToken token;
	
	char *payload="";
	int rc;

	createTelemetryPayload(rp,&payload);
	pubmsg.payload = payload;
	pubmsg.payloadlen = strlen(payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;				
	
	rc = MQTTClient_publishMessage(mqtt_client, rp->mqtt_tele_topic, &pubmsg, &token);
	//printf("Telemetry\n");
	free(payload);
	return (rc);
}
*/
/*
int publishStatMessage(char *topic, node_value_t *nv)
{
	MQTTClient_message pubmsg = MQTTClient_message_initializer;
  	MQTTClient_deliveryToken token;
	
	char *stat_topics[] = {"/FREQ","/SAMP_RATE",
						   "/GAIN_MODE","/GAIN",
						   "/PPM_ERR","/IF_GAIN",
						   "/TEST_MODE","/AGC_MODE",
						   "/DIRECT_SAMP","/OFFSET_TUNE",
						   "/RTL_XTAL","/TUNER_XTAL",
						   "/GAIN_BY_IDX","/ENABLE_TEE","/OP_MODE"};
	char *stat_topic;					   
	int rc;
	char *payload="";

	stat_topic = strdup(topic);
	strcat(stat_topic, stat_topics[nv->cmd]);
	payload = malloc(sizeof(char)*255);
	sprintf(payload, "%u", nv->param);
	pubmsg.payload = payload;
	pubmsg.payloadlen = strlen(payload);
	pubmsg.qos = 0;
	pubmsg.retained = 0;
	rc = MQTTClient_publishMessage(mqtt_client, stat_topic, &pubmsg, &token);
	printf("STAT: %s - %s\n", stat_topic, payload);
	free(payload);
	
	return (rc);
}
*/



static void *mqtt_worker(void *arg)
{

	u_int32_t appo_freq = 0;
	
	//struct timeval tv= {1,0};
	//struct timespec ts;
	//struct timeval tp;
	int r = 0;
	char *payload="";
	int rc;

	char* lwt_topic;
	int tele_period;
	unsigned char q_cmd;
	unsigned int q_param;


	MQTTClient_willOptions mqtt_lwt_opts = MQTTClient_willOptions_initializer;	 
	MQTTClient_connectOptions mqtt_conn_opts = { {'M', 'Q', 'T', 'C'}, 8, 60, 1, 1, NULL, NULL, NULL, 30, 0, NULL,\
												   0, NULL, MQTTVERSION_DEFAULT, {NULL, 0, 0}, {0, NULL}, -1, 0, NULL, NULL, NULL };
	
	pthread_mutex_lock(&param_lock);
	lwt_topic = strdup(mqtt_p->lwt_topic);
	tele_period = mqtt_p->tele_period;
	pthread_mutex_unlock(&param_lock);

	mqtt_lwt_opts.topicName = lwt_topic;
	mqtt_lwt_opts.message = "Offline";
	mqtt_lwt_opts.retained = 1;
	mqtt_lwt_opts.qos = 1;
	mqtt_conn_opts.httpsProxy = NULL;
	mqtt_conn_opts.keepAliveInterval = tele_period + 5;
	mqtt_conn_opts.will = &mqtt_lwt_opts;
    mqtt_conn_opts.cleansession = 1;
	
    if ((rc = MQTTClient_connect(mqtt_client, &mqtt_conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        fprintf(stderr, "Failed to connect at MQTT broker, return code %d\n", rc);
		sighandler(0);
		pthread_exit(NULL);
    }

	publishLastWillTestamentMessage(lwt_topic, "Online");

	//pthread_mutex_lock(&pub_cond_lock);
	//publishTelemetryMessage(rp);
	//pthread_mutex_unlock(&pub_cond_lock);
	printf("INITIAL TELEMETRY\n");
	
	while(1) {
		if(do_mqtt_exit) {
			publishLastWillTestamentMessage(lwt_topic, "Offline");
			MQTTClient_disconnect(mqtt_client, 10000);
			//TO DO: WAIT DISCONNECT ?
			printf("MQTT Worker exit\n");
			pthread_exit(0);
		}
		
		/*
			Attende il signal dal Command Worker nel caso ci siano comandi da notificare sul topic Stat
			Il timeout impostato equivale al tele_period in modo da effettuare la pubblicazione del topic tele a cadenza regolare
		*/

		r = dequeue_with_timeout(my_queue, tele_period, &q_cmd, &q_param);

		switch (r) {
			case 0: //send COMMAND UPDATE
				printf("SEND COMMAND\n");
				//publishStatMessage(rp->mqtt_stat_topic, nv);
				break;
			case 1: //send TELEMETRY
				printf("SEND TELEMETRY\n");
				//publishTelemetryMessage(rp);
				break;
			default:
				break;
		}
	}
}

static void *tcp_worker(void *arg)
{
	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	fd_set writefds;
	int r = 0;

	while(1) {
		if(do_exit)
			pthread_exit(0);

		pthread_mutex_lock(&ll_mutex);
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec+5;
		ts.tv_nsec = tp.tv_usec * 1000;
		r = pthread_cond_timedwait(&cond, &ll_mutex, &ts);
		if(r == ETIMEDOUT) {
			pthread_mutex_unlock(&ll_mutex);
			printf("tcp worker cond timeout\n");
			sighandler(0);
			pthread_exit(NULL);
		}

		curelem = ll_buffers;
		ll_buffers = 0;
		pthread_mutex_unlock(&ll_mutex);

		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(s, &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(s+1, NULL, &writefds, NULL, &tv);
				if(r) {
					bytessent = send(s,  &curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(bytessent == SOCKET_ERROR || do_exit) {
						printf("tcp worker socket bye\n");
						sighandler(0);
						pthread_exit(NULL);
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	}
}

static int set_gain_by_index(rtlsdr_dev_t *_dev, unsigned int index)
{
	int res = 0;
	int* gains;
	int count = rtlsdr_get_tuner_gains(_dev, NULL);

	if (count > 0 && (unsigned int)count > index) {
		gains = malloc(sizeof(int) * count);
		count = rtlsdr_get_tuner_gains(_dev, gains);

		res = rtlsdr_set_tuner_gain(_dev, gains[index]);

		free(gains);
	}

	return res;
}

#ifdef _WIN32
#define __attribute__(x)
#pragma pack(push, 1)
#endif
struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));
#ifdef _WIN32
#pragma pack(pop)
#endif
static void *command_worker(void *arg)
{
	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	int r = 0;
	uint32_t tmp;

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
			FD_ZERO(&readfds);
			FD_SET(s, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(s+1, &readfds, NULL, NULL, &tv);
			if(r) {
				received = recv(s, (char*)&cmd+(sizeof(cmd)-left), left, 0);
				left -= received;
			}
			if(received == SOCKET_ERROR || do_exit) {
				printf("comm recv bye\n");
				sighandler(0);
				pthread_exit(NULL);
			}
		}

		switch(cmd.cmd) {
		case 0x01:
			printf("set freq %d\n", ntohl(cmd.param));
			rtlsdr_set_center_freq(dev,ntohl(cmd.param));
			pthread_mutex_lock(&param_lock);
			rtl_p->frequency = ntohl(cmd.param);
			pthread_mutex_unlock(&param_lock);
			break;
		case 0x02:
			//rp->samp_rate = ntohl(cmd.param);
			printf("set sample rate %d\n", ntohl(cmd.param));
			rtlsdr_set_sample_rate(dev, ntohl(cmd.param));
			break;
		case 0x03:
			//rp->tuner_gain_mode = ntohl(cmd.param);
			printf("set gain mode %d\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain_mode(dev, ntohl(cmd.param));
			break;
		case 0x04:
			//rp->tuner_gain = ntohl(cmd.param);
			printf("set gain %d\n", ntohl(cmd.param));
			rtlsdr_set_tuner_gain(dev, ntohl(cmd.param));
			break;
		case 0x05:
	//		rp->ppm_error = ntohl(cmd.param);
			printf("set freq correction %d\n", ntohl(cmd.param));
			rtlsdr_set_freq_correction(dev, ntohl(cmd.param));
			break;
		case 0x06:
			tmp = ntohl(cmd.param);
			printf("set if stage %d gain %d\n", tmp >> 16, (short)(tmp & 0xffff));
			rtlsdr_set_tuner_if_gain(dev, tmp >> 16, (short)(tmp & 0xffff));
			break;
		case 0x07:
			printf("set test mode %d\n", ntohl(cmd.param));
			rtlsdr_set_testmode(dev, ntohl(cmd.param));
			break;
		case 0x08:
			//rp->agc_mode = ntohl(cmd.param);
			printf("set agc mode %d\n", ntohl(cmd.param));
			rtlsdr_set_agc_mode(dev, ntohl(cmd.param));
			break;
		case 0x09:
			//rp->direct_sampling = ntohl(cmd.param);
			printf("set direct sampling %d\n", ntohl(cmd.param));
			rtlsdr_set_direct_sampling(dev, ntohl(cmd.param));
			break;
		case 0x0a:
//			rp->offset_tuning = ntohl(cmd.param);
			printf("set offset tuning %d\n", ntohl(cmd.param));
			rtlsdr_set_offset_tuning(dev, ntohl(cmd.param));
;	
			break;
		case 0x0b:
//			rp->rtl_xtal_freq = ntohl(cmd.param);
			printf("set rtl xtal %d\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, ntohl(cmd.param), 0);
			break;
		case 0x0c:
//			rp->tuner_xtal_freq = ntohl(cmd.param);
			printf("set tuner xtal %d\n", ntohl(cmd.param));
			rtlsdr_set_xtal_freq(dev, 0, ntohl(cmd.param));
			break;
		case 0x0d:
			printf("set tuner gain by index %d\n", ntohl(cmd.param));
			set_gain_by_index(dev, ntohl(cmd.param));
			break;
		case 0x0e:
//			rp->enable_biastee = ntohl(cmd.param);
			printf("set bias tee %d\n", ntohl(cmd.param));
			rtlsdr_set_bias_tee(dev, (int)ntohl(cmd.param));
			break;
		default:
			break;
		}	

		enqueue(my_queue,cmd.cmd,ntohl(cmd.param));		
		cmd.cmd = 0xff;
	}
}

int main(int argc, char **argv)
{
	int r, opt, i;
	struct sockaddr_storage local, remote;
	struct addrinfo *ai;
	struct addrinfo *aiHead;
	struct addrinfo  hints;
	char hostinfo[NI_MAXHOST];
	char portinfo[NI_MAXSERV];
	char remhostinfo[NI_MAXHOST];
	char remportinfo[NI_MAXSERV];
	int aiErr;
	uint32_t buf_num = 0;
	int dev_given = 0;
	struct llist *curelem,*prev;
	pthread_attr_t attr;
	void *status;
	struct timeval tv = {1,0};
	struct linger ling = {1,0};
	SOCKET listensocket = 0;
	socklen_t rlen;
	fd_set readfds;
	u_long blockmode = 1;
	dongle_info_t dongle_info;
	int rc;


#ifdef _WIN32
	WSADATA wsd;
	i = WSAStartup(MAKEWORD(2,2), &wsd);
#else
	struct sigaction sigact, sigign;
#endif

	//RTL params initialization
	rtl_p = (rtl_params_t*) malloc(sizeof(rtl_params_t)); 
	rtl_p->frequency = 100000000;
	rtl_p->samp_rate = 2048000;
	rtl_p->agc_mode = 0;
	rtl_p->enable_biastee = 0;
	rtl_p->direct_sampling = 0;
	rtl_p->gain_by_index = 0;
	rtl_p->offset_tuning = 0;
	rtl_p->ppm_error = 0;
	rtl_p->tuner_gain = 0;
	rtl_p->tuner_gain_mode = 0;
	rtl_p->rtl_xtal_freq = 0;
	rtl_p->tuner_xtal_freq = 0;


	//Device info init
	di_p = (device_info_t*) malloc(sizeof(device_info_t)); 
	di_p->index = -1;
	di_p->name = NULL;
	di_p->product = NULL;
	di_p->serial = NULL;
	di_p->vendor = NULL;

	//Server params init
	server_p = (server_params_t*) malloc(sizeof(server_params_t)); 
	server_p->addr = "192.168.1.6";
	server_p->port = "1234";
	server_p->client_ip = "0.0.0.0";

	//MQTT params init
	mqtt_p = (mqtt_params_t*) malloc(sizeof(mqtt_params_t)); 
	mqtt_p->uri = "tcp://192.168.1.11:1883";
	mqtt_p->base_topic = "home/rtl_tcp/radio1";
	mqtt_p->tele_topic="home/rtl_tcp/radio1/tele/STATE";
	mqtt_p->stat_topic="home/rtl_tcp/radio1/stat";
	mqtt_p->lwt_topic="home/rtl_tcp/radio1/tele/LWT";
	mqtt_p->client_id = "Client_1234";
	mqtt_p->qos = 1;
	mqtt_p->tele_period = 10;



	while ((opt = getopt(argc, argv, "a:p:f:g:s:b:n:d:P:T:h:t:c:y")) != -1) {
		switch (opt) {
		case 'd':
			di_p->index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'f':
			rtl_p->frequency = (uint32_t)atofs(optarg);
			break;
		case 'g':
			rtl_p->tuner_gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			rtl_p->samp_rate  = (uint32_t)atofs(optarg);
			break;
		case 'a':
		    server_p->addr = strdup(optarg);
			break;
		case 'p':
		    server_p->port = strdup(optarg);
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		case 'n':
			llbuf_num = atoi(optarg);
			break;
		case 'P':
			rtl_p->ppm_error = atoi(optarg);
			break;
		case 'T':
			rtl_p->enable_biastee = 1;
			break;
		case 'h':
			mqtt_p->uri = strdup(optarg);
			break;
		case 't':
			mqtt_p->base_topic = strdup(optarg);
			mqtt_p->tele_topic = strdup(optarg);
			mqtt_p->stat_topic = strdup(optarg);
			mqtt_p->lwt_topic = strdup(optarg);
			strcat(mqtt_p->tele_topic, "/tele/STATE");
			strcat(mqtt_p->lwt_topic, "/tele/LWT");
			strcat(mqtt_p->stat_topic, "/stat");
			break;
		case 'c':
			mqtt_p->client_id = strdup(optarg);
			break;
		case 'y':
			mqtt_p->tele_period = atoi(optarg);
			break;				
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (!dev_given) {
		di_p->index = verbose_device_search("0");
	}

	if (di_p->index < 0) {
	    exit(1);
	}

	rtlsdr_open(&dev, (uint32_t)di_p->index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", di_p->index);
		exit(1);
	}

#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	/* Set the tuner error */
	verbose_ppm_set(dev, rtl_p->ppm_error);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, rtl_p->samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	/* Set the frequency */
	r = rtlsdr_set_center_freq(dev, rtl_p->frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %i Hz.\n", rtl_p->frequency);

	if (0 == rtl_p->tuner_gain) {
		 /* Enable automatic gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable automatic gain.\n");
	} else {
		/* Enable manual gain */
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to enable manual gain.\n");

		/* Set the tuner gain */
		r = rtlsdr_set_tuner_gain(dev, rtl_p->tuner_gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", rtl_p->tuner_gain/10.0);
	}

	rtlsdr_set_bias_tee(dev, rtl_p->enable_biastee);
	if (rtl_p->enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");

	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_mutex_init(&param_lock,NULL);

	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&exit_cond, NULL);


	hints.ai_flags  = AI_PASSIVE; /* Server mode. */
	hints.ai_family = PF_UNSPEC;  /* IPv4 or IPv6. */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if ((aiErr = getaddrinfo(server_p->addr,
				 server_p->port,
				 &hints,
				 &aiHead )) != 0)
	{
		fprintf(stderr, "local address %s ERROR - %s.\n",
		        server_p->addr, gai_strerror(aiErr));
		return(-1);
	}
	memcpy(&local, aiHead->ai_addr, aiHead->ai_addrlen);

	for (ai = aiHead; ai != NULL; ai = ai->ai_next) {
		aiErr = getnameinfo((struct sockaddr *)ai->ai_addr, ai->ai_addrlen,
				    hostinfo, NI_MAXHOST,
				    portinfo, NI_MAXSERV, NI_NUMERICSERV | NI_NUMERICHOST);
		if (aiErr)
			fprintf( stderr, "getnameinfo ERROR - %s.\n",hostinfo);

		listensocket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (listensocket < 0)
			continue;

		r = 1;
		setsockopt(listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
		setsockopt(listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		if (bind(listensocket, (struct sockaddr *)&local, sizeof(local)))
			fprintf(stderr, "rtl_tcp bind error: %s", strerror(errno));
		else
			break;
	}

	// populate device info 
	di_p->vendor = malloc(sizeof(char) * 256);
	di_p->product = malloc(sizeof(char) * 256);
	di_p->serial= malloc(sizeof(char) * 256);
	rtlsdr_get_device_usb_strings(di_p->index, di_p->vendor, di_p->product, di_p->serial);
	di_p->name= malloc(sizeof(char) * 256);
	strcpy(di_p->name, rtlsdr_get_device_name((uint32_t)di_p->index));

	
	if ((rc = MQTTClient_create(&mqtt_client, mqtt_p->uri, mqtt_p->client_id,
        MQTTCLIENT_PERSISTENCE_NONE, NULL)) != MQTTCLIENT_SUCCESS)
    {
         fprintf(stderr, "Failed to create MQTT client, return code %d\n", rc);
         exit(1);
    }

	my_queue = qcreate();

	//start MQTT Worker thread	
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	r = pthread_create(&mqtt_worker_thread, &attr, mqtt_worker,NULL);
	pthread_attr_destroy(&attr);

#ifdef _WIN32
	ioctlsocket(listensocket, FIONBIO, &blockmode);
#else
	r = fcntl(listensocket, F_GETFL, 0);
	r = fcntl(listensocket, F_SETFL, r | O_NONBLOCK);
#endif

	while(1) {
		printf("listening...\n");
		printf("Use the device argument 'rtl_tcp=%s:%s' in OsmoSDR "
		       "(gr-osmosdr) source\n"
		       "to receive samples in GRC and control "
		       "rtl_tcp parameters (frequency, gain, ...).\n",
		       hostinfo, portinfo);
		listen(listensocket,1);

		while(1) {
			FD_ZERO(&readfds);
			FD_SET(listensocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(listensocket+1, &readfds, NULL, NULL, &tv);
			if(do_exit) {
				goto out;
			} else if(r) {
				rlen = sizeof(remote);
				s = accept(listensocket,(struct sockaddr *)&remote, &rlen);
				break;
			}
		}

		setsockopt(s, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		getnameinfo((struct sockaddr *)&remote, rlen,
			    remhostinfo, NI_MAXHOST,
			    remportinfo, NI_MAXSERV, NI_NUMERICSERV);
		printf("client accepted! %s %s\n", remhostinfo, remportinfo);
		
		memset(&dongle_info, 0, sizeof(dongle_info));
		memcpy(&dongle_info.magic, "RTL0", 4);

		r = rtlsdr_get_tuner_type(dev);
		if (r >= 0)
			dongle_info.tuner_type = htonl(r);

		r = rtlsdr_get_tuner_gains(dev, NULL);
		if (r >= 0)
			dongle_info.tuner_gain_count = htonl(r);

		r = send(s, (const char *)&dongle_info, sizeof(dongle_info), 0);
		if (sizeof(dongle_info) != r)
			printf("failed to send dongle information\n");

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
		r = pthread_create(&tcp_worker_thread, &attr, tcp_worker, NULL);
		r = pthread_create(&command_thread, &attr, command_worker, NULL);
		pthread_attr_destroy(&attr);

		r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, buf_num, 0);

		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);
		
		closesocket(s);

		printf("TCP and Command threads dead..\n");
		curelem = ll_buffers;
		ll_buffers = 0;

		while(curelem != 0) {
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}

		do_exit = 0;
		global_numq = 0;
	} //end while(1) Listen for TCP Connection
out:

	//Wait MQTT Worker thread for exit
	do_mqtt_exit = 1;
	pthread_join(mqtt_worker_thread, &status);
    MQTTClient_destroy(&mqtt_client);
	free(my_queue);
	rtlsdr_close(dev);
	closesocket(listensocket);
	closesocket(s);

	free(mqtt_p);
	free(server_p);
	free(rtl_p);
	free(di_p);
	

#ifdef _WIN32
	WSACleanup();
#endif
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
