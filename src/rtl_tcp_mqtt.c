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

static pthread_cond_t pub_cond;
static pthread_mutex_t pub_cond_lock;


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

typedef enum  {
	UNKNOW,
	IDLE,
	RUNNING,
} op_states;


typedef struct state_msg {
	unsigned char cmd;
	unsigned int param; 
} state_msg;

// struct node - struttura di un nodo della coda thread-safe
typedef struct node {
    //int value;
	struct state_msg value;
    struct node *next;
} node;
// struct Queue_r - struttura della coda thread-safe (usa una linked list)
typedef struct {
    node *front;
    node *rear;
    pthread_mutex_t mutex;
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


typedef struct {
	int dev_index;
	char *dev_name;
	char *dev_product;
	char *dev_vendor;
	char *dev_serial; 
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
	char* client_ip;
	op_states op_state;
	char* addr;
	char* port;
	char* mqtt_uri;
	char* mqtt_topic;
	char* mqtt_tele_topic;
	char* mqtt_stat_topic;
	char* mqtt_lwt_topic;
	char* mqtt_client_id;
	int mqtt_tele_period;
	int mqtt_qos;
	//node_t *fifo_cmds;
} radio_params_t;

static radio_params_t *rp = NULL;

static MQTTClient mqtt_client;

static Queue_r *my_queue = NULL;
/*
void enqueue(node_t **head, node_value_t *cmd) {
   node_t *new_node = malloc(sizeof(node_t));
   if (!new_node) return;
   	
   new_node->value = cmd;
   new_node->next = *head;

   *head = new_node;
}


node_value_t* dequeue(node_t **head) {
   node_t *current, *prev = NULL;
   node_value_t* retval = NULL;

   if (*head == NULL) return NULL;

   current = *head;
   while (current->next != NULL) {
      prev = current;
      current = current->next;
   }

   retval = current->value;
   free(current);

   if (prev)
      prev->next = NULL;
   else
      *head = NULL;

   return retval;
}

void print_list(node_t *head) {
    node_t *current = head;

    while (current != NULL) {
        printf("%d %d\n", current->value->cmd, current->value->param);
        current = current->next;
    }
}
*/

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
	free(payload);
	return (rc);
}

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
	
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
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
	
	pthread_mutex_lock(&pub_cond_lock);
	lwt_topic = strdup(rp->mqtt_lwt_topic);
	tele_period = rp->mqtt_tele_period;
	pthread_mutex_unlock(&pub_cond_lock);

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

	pthread_mutex_lock(&pub_cond_lock);
	rp->op_state = IDLE;
	publishTelemetryMessage(rp);
	pthread_mutex_unlock(&pub_cond_lock);

	while(1) {
		if(do_mqtt_exit) {
			publishLastWillTestamentMessage(lwt_topic, "Offline");
			MQTTClient_disconnect(mqtt_client, 10000);
			printf("MQTT Worker exit\n");
			pthread_exit(0);
		}
		
		/*
			Attende il signal dal Command Worker nel caso ci siano comandi da notificare sul topic Stat
			Il timeout impostato equivale al tele_period in modo da effettuare la pubblicazione del topic tele a cadenza regolare
		*/
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec + tele_period;
		ts.tv_nsec = tp.tv_usec * 1000;

		pthread_mutex_lock(&pub_cond_lock);
		r = pthread_cond_timedwait(&pub_cond, &pub_cond_lock, &ts);
		if(r == ETIMEDOUT) {
			publishTelemetryMessage(rp);

		} else if (r == 0) { //SUCCESS
			dequeue(my_queue, &q_cmd, &q_param);
			printf("STAT - %d %u \n", q_cmd, q_param);

			//print_list(rp->fifo_cmds);
			/*
			while ((nv = dequeue(&rp->fifo_cmds)) != NULL)
			{
				if (nv->cmd == 0xfe) {
					//force telemetry message update
					publishTelemetryMessage(rp);
				} else {
					//command worker notification
					publishStatMessage(rp->mqtt_stat_topic, nv);
				}
				free(nv);
			}
			*/
		}
		pthread_mutex_unlock(&pub_cond_lock);
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
	//static node_value_t *nv; 

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
		
		/*
		if (cmd.cmd > 0 && cmd.cmd < 0x0f) 
		{
			pthread_mutex_lock(&pub_cond_lock);
			nv = (node_value_t *) malloc(sizeof(node_value_t));
			nv->cmd = cmd.cmd-1;
			nv->param = ntohl(cmd.param);
			enqueue(&rp->fifo_cmds, nv);
			pthread_cond_signal(&pub_cond);
		}
			*/

		pthread_mutex_lock(&pub_cond_lock);
		switch(cmd.cmd) {
		case 0x01:
			rp->frequency = ntohl(cmd.param);
			printf("set freq %d\n", rp->frequency);
			rtlsdr_set_center_freq(dev,rp->frequency);
			enqueue(my_queue, 1, ntohl(cmd.param));
			pthread_cond_signal(&pub_cond);
			break;
		case 0x02:
			rp->samp_rate = ntohl(cmd.param);
			printf("set sample rate %d\n", rp->samp_rate);
			rtlsdr_set_sample_rate(dev, rp->samp_rate);
			break;
		case 0x03:
			rp->tuner_gain_mode = ntohl(cmd.param);
			printf("set gain mode %d\n", rp->tuner_gain_mode);
			rtlsdr_set_tuner_gain_mode(dev, rp->tuner_gain_mode);
			break;
		case 0x04:
			rp->tuner_gain = ntohl(cmd.param);
			printf("set gain %d\n", rp->tuner_gain);
			rtlsdr_set_tuner_gain(dev, rp->tuner_gain);
			break;
		case 0x05:
			rp->ppm_error = ntohl(cmd.param);
			printf("set freq correction %d\n", rp->ppm_error);
			rtlsdr_set_freq_correction(dev, rp->ppm_error);
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
			rp->agc_mode = ntohl(cmd.param);
			printf("set agc mode %d\n", rp->agc_mode);
			rtlsdr_set_agc_mode(dev, rp->agc_mode);
			break;
		case 0x09:
			rp->direct_sampling = ntohl(cmd.param);
			printf("set direct sampling %d\n", rp->direct_sampling);
			rtlsdr_set_direct_sampling(dev, rp->direct_sampling);
			break;
		case 0x0a:
			rp->offset_tuning = ntohl(cmd.param);
			printf("set offset tuning %d\n", rp->offset_tuning);
			rtlsdr_set_offset_tuning(dev, rp->offset_tuning);
			break;
		case 0x0b:
			rp->rtl_xtal_freq = ntohl(cmd.param);
			printf("set rtl xtal %d\n", rp->rtl_xtal_freq);
			rtlsdr_set_xtal_freq(dev, rp->rtl_xtal_freq, 0);
			break;
		case 0x0c:
			rp->tuner_xtal_freq = ntohl(cmd.param);
			printf("set tuner xtal %d\n", rp->tuner_xtal_freq);
			rtlsdr_set_xtal_freq(dev, 0, rp->tuner_xtal_freq);
			break;
		case 0x0d:
			printf("set tuner gain by index %d\n", ntohl(cmd.param));
			set_gain_by_index(dev, ntohl(cmd.param));
			break;
		case 0x0e:
			rp->enable_biastee = ntohl(cmd.param);
			printf("set bias tee %d\n", rp->enable_biastee);
			rtlsdr_set_bias_tee(dev, (int)rp->enable_biastee);
			break;
		default:
			break;
		}
		cmd.cmd = 0xff;
		pthread_mutex_unlock(&pub_cond_lock);
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
	//node_value_t *nv;

#ifdef _WIN32
	WSADATA wsd;
	i = WSAStartup(MAKEWORD(2,2), &wsd);
#else
	struct sigaction sigact, sigign;
#endif

	//Radio params initialization
	rp = (radio_params_t*) malloc(sizeof(radio_params_t)); 
	rp->frequency = 100000000;
	rp->samp_rate = 2048000;
	rp->agc_mode = 0;
	rp->enable_biastee = 0;
	rp->client_ip = "0.0.0.0";
	rp->dev_index = 0;
	rp->dev_name = "";
	rp->direct_sampling = 0;
	rp->gain_by_index = 0;
	rp->offset_tuning = 0;
	rp->op_state = UNKNOW;
	rp->ppm_error = 0;
	rp->tuner_gain = 0;
	rp->tuner_gain_mode = 0;
	rp->rtl_xtal_freq = 0;
	rp->tuner_xtal_freq = 0;
	rp->addr = "192.168.1.6";
	rp->port = "1234";
	rp->mqtt_uri = "tcp://192.168.1.11:1883";
	rp->mqtt_topic = "home/rtl_tcp/radio1";
	rp->mqtt_tele_topic="home/rtl_tcp/radio1/tele/STATE";
	rp->mqtt_stat_topic="home/rtl_tcp/radio1/stat";
	rp->mqtt_lwt_topic="home/rtl_tcp/radio1/tele/LWT";
	rp->mqtt_client_id = "Client_1234";
	rp->mqtt_qos = 1;
	rp->mqtt_tele_period = 10;
	//rp->fifo_cmds = NULL;


	while ((opt = getopt(argc, argv, "a:p:f:g:s:b:n:d:P:T:h:t:c:y")) != -1) {
		switch (opt) {
		case 'd':
			rp->dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'f':
			rp->frequency = (uint32_t)atofs(optarg);
			break;
		case 'g':
			rp->tuner_gain = (int)(atof(optarg) * 10); /* tenths of a dB */
			break;
		case 's':
			rp->samp_rate  = (uint32_t)atofs(optarg);
			break;
		case 'a':
		    rp->addr = strdup(optarg);
			break;
		case 'p':
		    rp->port = strdup(optarg);
			break;
		case 'b':
			buf_num = atoi(optarg);
			break;
		case 'n':
			llbuf_num = atoi(optarg);
			break;
		case 'P':
			rp->ppm_error = atoi(optarg);
			break;
		case 'T':
			rp->enable_biastee = 1;
			break;
		case 'h':
			rp->mqtt_uri = strdup(optarg);
			break;
		case 't':
			rp->mqtt_topic = strdup(optarg);
			rp->mqtt_tele_topic = strdup(optarg);
			rp->mqtt_stat_topic = strdup(optarg);
			rp->mqtt_lwt_topic = strdup(optarg);
			strcat(rp->mqtt_tele_topic, "/tele/STATE");
			strcat(rp->mqtt_lwt_topic, "/tele/LWT");
			strcat(rp->mqtt_stat_topic, "/stat");
			break;
		case 'c':
			rp->mqtt_client_id = strdup(optarg);
			break;
		case 'y':
			rp->mqtt_tele_period = atoi(optarg);
			break;				
		default:
			usage();
			break;
		}
	}

	if (argc < optind)
		usage();

	if (!dev_given) {
		rp->dev_index = verbose_device_search("0");
	}

	if (rp->dev_index < 0) {
	    exit(1);
	}

	rtlsdr_open(&dev, (uint32_t)rp->dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", rp->dev_index);
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
	verbose_ppm_set(dev, rp->ppm_error);

	/* Set the sample rate */
	r = rtlsdr_set_sample_rate(dev, rp->samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

	/* Set the frequency */
	r = rtlsdr_set_center_freq(dev, rp->frequency);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set center freq.\n");
	else
		fprintf(stderr, "Tuned to %i Hz.\n", rp->frequency);

	if (0 == rp->tuner_gain) {
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
		r = rtlsdr_set_tuner_gain(dev, rp->tuner_gain);
		if (r < 0)
			fprintf(stderr, "WARNING: Failed to set tuner gain.\n");
		else
			fprintf(stderr, "Tuner gain set to %f dB.\n", rp->tuner_gain/10.0);
	}

	rtlsdr_set_bias_tee(dev, rp->enable_biastee);
	if (rp->enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	r = rtlsdr_reset_buffer(dev);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to reset buffers.\n");

	pthread_mutex_init(&exit_cond_lock, NULL);
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_mutex_init(&exit_cond_lock, NULL);

	pthread_mutex_init(&pub_cond_lock,NULL);

	pthread_cond_init(&cond, NULL);
	pthread_cond_init(&exit_cond, NULL);

	hints.ai_flags  = AI_PASSIVE; /* Server mode. */
	hints.ai_family = PF_UNSPEC;  /* IPv4 or IPv6. */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if ((aiErr = getaddrinfo(rp->addr,
				 rp->port,
				 &hints,
				 &aiHead )) != 0)
	{
		fprintf(stderr, "local address %s ERROR - %s.\n",
		        rp->addr, gai_strerror(aiErr));
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
	rp->dev_vendor = malloc(sizeof(char) * 256);
	rp->dev_product = malloc(sizeof(char) * 256);
	rp->dev_serial= malloc(sizeof(char) * 256);
	rtlsdr_get_device_usb_strings(rp->dev_index, rp->dev_vendor, rp->dev_product, rp->dev_serial);
	rp->dev_name= malloc(sizeof(char) * 256);
	strcpy(rp->dev_name, rtlsdr_get_device_name((uint32_t)rp->dev_index));

	
	if ((rc = MQTTClient_create(&mqtt_client, rp->mqtt_uri, rp->mqtt_client_id,
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

		
		/*
		nv = (node_value_t *) malloc(sizeof(node_value_t));
		nv->cmd = 0xfe;
		nv->param = 0;
		pthread_mutex_lock(&pub_cond_lock);
		rp->op_state = RUNNING;
		rp->client_ip = remhostinfo;
		enqueue(&rp->fifo_cmds, nv);
		pthread_cond_signal(&pub_cond);
		pthread_mutex_unlock(&pub_cond_lock);
		*/

		r = rtlsdr_read_async(dev, rtlsdr_callback, NULL, buf_num, 0);

		pthread_join(tcp_worker_thread, &status);
		pthread_join(command_thread, &status);
		
		//pthread_mutex_lock(&pub_cond_lock);
		/*
		nv = (node_value_t *) malloc(sizeof(node_value_t));
		nv->cmd = 0xfe;
		nv->param = 0;
		pthread_mutex_lock(&pub_cond_lock);
		rp->op_state = IDLE;
		rp->client_ip = remhostinfo;
		enqueue(&rp->fifo_cmds, nv);
		pthread_cond_signal(&pub_cond);
		pthread_mutex_unlock(&pub_cond_lock);
		*/		
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
/*
	nv = (node_value_t *) malloc(sizeof(node_value_t));
	nv->cmd = 0xfe;
	nv->param = 0;
	pthread_mutex_lock(&pub_cond_lock);
	rp->op_state = UNKNOW;
	rp->client_ip = remhostinfo;
	enqueue(&rp->fifo_cmds, nv);
	pthread_cond_signal(&pub_cond);
	pthread_mutex_unlock(&pub_cond_lock);
*/
//	sleep(1);



	do_mqtt_exit = 1;
	pthread_join(mqtt_worker_thread, &status);
    MQTTClient_destroy(&mqtt_client);
	free(my_queue);
	rtlsdr_close(dev);
	closesocket(listensocket);
	closesocket(s);
#ifdef _WIN32
	WSACleanup();
#endif
	printf("bye!\n");
	return r >= 0 ? r : -r;
}
