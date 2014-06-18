/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2014 pooler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <curses.h>

#include "cpuminer-config.h"
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <hw_nvidia.h>
#ifdef WIN32
#include <windows.h>
#include "nvapi.h"
#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <jansson.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <openssl/sha.h>
#include "compat.h"
#include "miner.h"

#ifdef WIN32
#include <Mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

#define PROGRAM_NAME		"ccminer"
#define PROGRAM_VERSION "1.2"
#define	PROGRAM_VERSION_SPLIT_SCREEN "1.2.3"
#define LP_SCANTIME		60
#define HEAVYCOIN_BLKHDR_SZ		84
#define MNR_BLKHDR_SZ 80

// from heavy.cu
#ifdef __cplusplus
extern "C"
{
#endif
int cuda_num_devices();
void cuda_bus_ids();
void cuda_devicenames();
int cuda_finddevice(char *name);
#ifdef __cplusplus
}
#endif

WINDOW *info_screen, *out_screen, *menu_screen;

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void)
{
	struct sched_param param;
	param.sched_priority = 0;

#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
#ifdef SCHED_BATCH
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(&set), &set);
}
#elif defined(__FreeBSD__) /* FreeBSD specific policy and affinity management */
#include <sys/cpuset.h>
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpuset_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(cpuset_t), &set);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif
		
enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
};

typedef enum {
	ALGO_HEAVY,		/* Heavycoin hash */
	ALGO_MJOLLNIR,		/* Mjollnir hash */
	ALGO_FUGUE256,		/* Fugue256 */
	ALGO_GROESTL,
	ALGO_MYR_GR,
	ALGO_JACKPOT,
	ALGO_QUARK,
	ALGO_ANIME,
	ALGO_NIST5,
	ALGO_X11,
	ALGO_X13,
	ALGO_DMD_GR
} sha256_algos;

static const char *algo_names[] = {
	"heavy",
	"mjollnir",
	"fugue256",
	"groestl",
	"myr-gr",
	"jackpot",
	"quark",
	"anime",
	"nist5",
	"x11",
	"x13",
	"dmd-gr"
};

bool opt_debug = false;
bool opt_protocol = false;
bool opt_benchmark = false;
bool want_longpoll = true;
bool have_longpoll = false;
bool want_stratum = true;
bool have_stratum = false;
static bool submit_old = false;
bool use_syslog = false;
static bool opt_background = false;
static bool opt_quiet = false;
static int opt_retries = -1;
static int opt_fail_pause = 30;
int opt_timeout = 270;
static int opt_scantime = 5;
static json_t *opt_config;
static const bool opt_time = true;
static sha256_algos opt_algo = ALGO_HEAVY;
static int opt_n_threads = 0;
static double opt_difficulty = 1; // CH
bool opt_trust_pool = false;
uint16_t opt_vote = 9999;
static int num_processors;
int device_map[8] =  {0,1,2,3,4,5,6,7}; // CB {9,9,9,9,9,9,9,9};
int invert[8] = {0};
int bus_ids[8] = {0};
int device_map_invert[8] = {0,1,2,3,4,5,6,7};
unsigned int thermal_max[8] = {0};
char *device_name[8]; // CB
static char *rpc_url;
static char *rpc_userpass;
static char *rpc_user, *rpc_pass;
char *opt_cert;
char *opt_proxy;
long opt_proxy_type;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
struct work_restart *work_restart = NULL;
static struct stratum_ctx stratum;

pthread_mutex_t applog_lock;
static pthread_mutex_t stats_lock;

static unsigned long accepted_count = 0L;
static unsigned long rejected_count = 0L;
static double *thr_hashrates;

struct upload_buffer { const void *buf; size_t len; };
struct MemoryStruct { char *memory; size_t size; };

int menukey, cx, dx;

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else
struct option {
	const char *name;
	int has_arg;
	int *flag;
	int val;
};
#endif

int parent_x, parent_y, new_x, new_y;

void SetWindow(int Width, int Height) 
{ 
    _COORD coord; 
    coord.X = Width; 
    coord.Y = Height; 

    _SMALL_RECT Rect; 
    Rect.Top = 0; 
    Rect.Left = 0; 
    Rect.Bottom = Height - 1; 
    Rect.Right = Width - 1; 

    HANDLE Handle = GetStdHandle(STD_OUTPUT_HANDLE);      // Get Handle 
    SetConsoleScreenBufferSize(Handle, coord);            // Set Buffer Size 
    SetConsoleWindowInfo(Handle, TRUE, &Rect);            // Set Window Size 
} 

void updatescr()
{
	//wclear(info_screen);
	wrefresh(menu_screen);
	wrefresh(out_screen);
	wrefresh(info_screen);
}

int printline(WINDOW *win, bool newline, const char *fmt, ...) {
    va_list args;
    int retval;

    va_start(args, fmt);
		char *f;
		int len;
		time_t now;
		struct tm tm, *tm_p;
	
		time(&now);

		pthread_mutex_lock(&applog_lock);
		tm_p = localtime(&now);
		memcpy(&tm, tm_p, sizeof(tm));
		pthread_mutex_unlock(&applog_lock);

		len = (int)(40 + strlen(fmt) + 2);
		f = (char*)alloca(len);
		
		sprintf(f, "[%d-%02d-%02d %02d:%02d:%02d] %s",
			tm.tm_year + 1900,
			tm.tm_mon + 1,
			tm.tm_mday,
			tm.tm_hour,
			tm.tm_min,
			tm.tm_sec,
			fmt);

		pthread_mutex_lock(&applog_lock);
		retval = vwprintw(win, f, args);
		if (newline)
			wprintw(win, "\n");
		updatescr();
		pthread_mutex_unlock(&applog_lock);
    va_end(args);
	return retval;
}

static void destroywins(void) {
	delwin(info_screen);
	delwin(out_screen);
    endwin();
}


int nvapi_init(){
	NvDisplayHandle hDisplay_a[NVAPI_MAX_PHYSICAL_GPUS*2] = {0};
	NvAPI_Status ret = NVAPI_OK;
	ret = NvAPI_Initialize();
    if(ret != NVAPI_OK)
    {
        printline(info_screen, true, "NvAPI_Initialize() failed = 0x%x", ret);
		return 1; // Initialization failed
    }
	printline(info_screen, false, "\nDisplay Configuration Successful!\n");
	
	NvU32 cnt;
	NvPhysicalGpuHandle phys;
    ret = NvAPI_EnumPhysicalGPUs(&phys, &cnt);
    if (!ret == NVAPI_OK){
        NvAPI_ShortString string;
        NvAPI_GetErrorMessage(ret, string);
        printline(info_screen, false, "NVAPI NvAPI_EnumPhysicalGPUs: %s\n", string);
    }
	NV_GPU_THERMAL_SETTINGS thermal;
    thermal.version =NV_GPU_THERMAL_SETTINGS_VER;
    ret = NvAPI_GPU_GetThermalSettings(phys,0, &thermal);

    if (!ret == NVAPI_OK){
        NvAPI_ShortString string;
        NvAPI_GetErrorMessage(ret, string);
        printline(info_screen, false, "NVAPI NvAPI_GPU_GetThermalSettings: %s\n", string);
    }

   printline(info_screen, false, "Temp: %u C\n", thermal.sensor[0].currentTemp);
    return 0;
}

int gpuinfo(int id, double dif, double balance) {
	int ret;
	char *s;
	unsigned int thermal_cur = 0;

	pthread_mutex_lock(&applog_lock);
	mvwprintw(info_screen, 0, 1, "Split Screen ccMiner %s by zelante [ core: ccMiner %s ]", PROGRAM_VERSION_SPLIT_SCREEN, PROGRAM_VERSION);
	//mvwprintw(info_screen, parent_y/2 - 1, 1, "ccMiner %s for nVidia GPUs by Christian Buchner and Christian H.", PROGRAM_VERSION);
	/*for (int i=0; i<opt_n_threads; i++)	
			{	
				if (i+1 == get_bus_id(id))
				{
					thermal_cur = hw_nvidia_gettemperature(invert[i]);
					if (thermal_max[invert[i]] < thermal_cur) 
						thermal_max[invert[i]] = thermal_cur;
					ret=mvwprintw(info_screen, i+2, 0, "GPU #%d: %s %.0fkhash/s %uC/%uC %u%% %uMHz %uMHz %uMb          ", 
						device_map[id], 
						device_name[id],
						thr_hashrates[id] * 1e-3,
						thermal_cur,
						thermal_max[invert[i]],
						hw_nvidia_DynamicPstateInfoEx(invert[i]),
						hw_nvidia_clock(invert[i]),
						hw_nvidia_clockMemory(invert[i]),
						hw_nvidia_memory(invert[i]));
				}
			}*/
	
				thermal_cur = hw_nvidia_gettemperature(invert[id]);
				if (thermal_max[invert[id]] < thermal_cur) 
					thermal_max[invert[id]] = thermal_cur;
				ret=mvwprintw(info_screen, id+2, 0, "GPU #%d[%d]: %s %.0fkhash/s %uC/%uC %u%% %uMHz %uMHz %uMb          ", 
					device_map[id], 
					invert[id]+1,
					device_name[id],
					thr_hashrates[id] * 1e-3,
					thermal_cur,
					thermal_max[invert[id]],
					hw_nvidia_DynamicPstateInfoEx(invert[id]),
					hw_nvidia_clock(invert[id]),
					hw_nvidia_clockMemory(invert[id]),
					hw_nvidia_memory(invert[id]));

	/*mvwprintw(info_screen, 7, 0, "buses: %d %d %d invert(cuda bus id): %d %d %d", bus_ids[0],bus_ids[1],bus_ids[2],invert[0],invert[1],invert[2]);
	mvwprintw(info_screen, 9, 0, "hw_temp = %d,%d,%d device_map = [%d,%d,%d,%d,%d,%d,%d,%d] device_map_invert[%d,%d,%d,%d,%d,%d,%d,%d]",
		hw_nvidia_gettemperature(0),hw_nvidia_gettemperature(1),hw_nvidia_gettemperature(2),
		device_map[0],device_map[1],device_map[2],device_map[3],device_map[4],device_map[5],device_map[6],device_map[7],
		device_map_invert[0],device_map_invert[1],device_map_invert[2],device_map_invert[3],device_map_invert[4],device_map_invert[5],device_map_invert[6],device_map_invert[7]);*/
	if (rpc_url)
		mvwprintw(info_screen, parent_y/2-2, 0, "%s", rpc_url);
	/*else if (rpc_url && !have_stratum) 
		mvwprintw(info_screen, parent_y/2-2, 0, "%s diff:%.4f balance:%.8f", rpc_url, dif, balance);*/
	updatescr();
	pthread_mutex_unlock(&applog_lock);
	return ret;
}


static char const usage[] = "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
  -a, --algo=ALGO       specify the algorithm to use\n\
                        fugue256  Fuguecoin hash\n\
                        heavy     Heavycoin hash\n\
                        mjollnir  Mjollnircoin hash\n\
                        groestl   Groestlcoin hash\n\
                        myr-gr    Myriad-Groestl hash\n\
                        jackpot   Jackpot hash\n\
                        quark     Quark hash\n\
                        anime     Animecoin hash\n\
                        nist5     NIST5 (TalkCoin) hash\n\
                        x11       X11 (DarkCoin) hash\n\
                        x13       X13 (MaruCoin) hash\n\
                        dmd-gr    Diamond-Groestl hash\n\
  -d, --devices         takes a comma separated list of CUDA devices to use.\n\
                        Device IDs start counting from 0! Alternatively takes\n\
                        string names of your cards like gtx780ti or gt640#2\n\
                        (matching 2nd gt640 in the PC)\n\
  -f, --diff            Divide difficulty by this factor (std is 1) \n\
  -v, --vote=VOTE       block reward vote (for HeavyCoin)\n\
  -m, --trust-pool      trust the max block reward vote (maxvote) sent by the pool\n\
  -o, --url=URL         URL of mining server\n\
  -O, --userpass=U:P    username:password pair for mining server\n\
  -u, --user=USERNAME   username for mining server\n\
  -p, --pass=PASSWORD   password for mining server\n\
      --cert=FILE       certificate for mining server using SSL\n\
  -x, --proxy=[PROTOCOL://]HOST[:PORT]  connect through a proxy\n\
  -t, --threads=N       number of miner threads (default: number of nVidia GPUs)\n\
  -r, --retries=N       number of times to retry if a network call fails\n\
                          (default: retry indefinitely)\n\
  -R, --retry-pause=N   time to pause between retries, in seconds (default: 30)\n\
  -T, --timeout=N       network timeout, in seconds (default: 270)\n\
  -s, --scantime=N      upper bound on time spent scanning current work when\n\
                          long polling is unavailable, in seconds (default: 5)\n\
      --no-longpoll     disable X-Long-Polling support\n\
      --no-stratum      disable X-Stratum support\n\
  -q, --quiet           disable per-thread hashmeter output\n\
  -D, --debug           enable debug output\n\
  -P, --protocol-dump   verbose dump of protocol-level activities\n"
#ifdef HAVE_SYSLOG_H
"\
  -S, --syslog          use system log for output messages\n"
#endif
#ifndef WIN32
"\
  -B, --background      run the miner in the background\n"
#endif
"\
      --benchmark       run in offline benchmark mode\n\
  -c, --config=FILE     load a JSON-format configuration file\n\
  -V, --version         display version information and exit\n\
  -h, --help            display this help text and exit\n\
";

static char const short_options[] =
#ifndef WIN32
	"B"
#endif
#ifdef HAVE_SYSLOG_H
	"S"
#endif
	"a:c:Dhp:Px:qr:R:s:t:T:o:u:O:Vd:f:mv:";

static struct option const options[] = {
	{ "algo", 1, NULL, 'a' },
#ifndef WIN32
	{ "background", 0, NULL, 'B' },
#endif
	{ "benchmark", 0, NULL, 1005 },
	{ "cert", 1, NULL, 1001 },
	{ "config", 1, NULL, 'c' },
	{ "debug", 0, NULL, 'D' },
	{ "help", 0, NULL, 'h' },
	{ "no-longpoll", 0, NULL, 1003 },
	{ "no-stratum", 0, NULL, 1007 },
	{ "pass", 1, NULL, 'p' },
	{ "protocol-dump", 0, NULL, 'P' },
	{ "proxy", 1, NULL, 'x' },
	{ "quiet", 0, NULL, 'q' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "scantime", 1, NULL, 's' },
#ifdef HAVE_SYSLOG_H
	{ "syslog", 0, NULL, 'S' },
#endif
	{ "threads", 1, NULL, 't' },
	{ "vote", 1, NULL, 'v' },
	{ "trust-pool", 0, NULL, 'm' },
	{ "timeout", 1, NULL, 'T' },
	{ "url", 1, NULL, 'o' },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 'O' },
	{ "version", 0, NULL, 'V' },
	{ "devices", 1, NULL, 'd' },
	{ "diff", 1, NULL, 'f' },
	{ 0, 0, 0, 0 }
};

struct work {
	uint32_t data[32];
	uint32_t target[8];
	uint32_t maxvote;

	char job_id[128];
	size_t xnonce2_len;
	unsigned char xnonce2[32];
};

static struct work g_work;
static time_t g_work_time;
static pthread_mutex_t g_work_lock;

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		//applog(LOG_ERR, "JSON key '%s' not found", key);
		printline(out_screen, true, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		//applog(LOG_ERR, "JSON key '%s' is not a string", key);
		printline(out_screen, true, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin((unsigned char*)buf, hexstr, buflen))
		return false;

	return true;
}

static bool work_decode(const json_t *val, struct work *work)
{
	int i;
	
	if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
		//applog(LOG_ERR, "JSON inval data");
		printline(out_screen, true, "JSON inval data");
		goto err_out;
	}
	if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
		//applog(LOG_ERR, "JSON inval target");
		printline(out_screen, true, "JSON inval target");
		goto err_out;
	}
	if (opt_algo == ALGO_HEAVY) {
		if (unlikely(!jobj_binary(val, "maxvote", &work->maxvote, sizeof(work->maxvote)))) {
			work->maxvote = 1024;
		}
	} else work->maxvote = 0;

	for (i = 0; i < ARRAY_SIZE(work->data); i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[i] = le32dec(work->target + i);

	return true;

err_out:
	return false;
}

static void share_result(int result, const char *reason)
{
	char s[345];
	double hashrate;
	int i;

	hashrate = 0.;
	pthread_mutex_lock(&stats_lock);
	for (i = 0; i < opt_n_threads; i++)
		hashrate += thr_hashrates[i];
	result ? accepted_count++ : rejected_count++;
	pthread_mutex_unlock(&stats_lock);
	
	sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
	printline(out_screen, true, "accepted: %lu/%lu (%.2f%%), %s khash/s %s",
		   accepted_count,
		   accepted_count + rejected_count,
		   100. * accepted_count / (accepted_count + rejected_count),
		   s,
		   result ? "(yay!!!)" : "(booooo)");

	/*applog(LOG_INFO, "accepted: %lu/%lu (%.2f%%), %s khash/s %s",
		   accepted_count,
		   accepted_count + rejected_count,
		   100. * accepted_count / (accepted_count + rejected_count),
		   s,
		   result ? "(yay!!!)" : "(booooo)");*/

	if (opt_debug && reason)
	//if (reason)
		//applog(LOG_DEBUG, "DEBUG: reject reason: %s", reason);
		printline(out_screen, true, "DEBUG: reject reason: %s", reason);
}


static size_t WriteMemoryCallback(void *ptr,size_t size,size_t nmemb,void *data)
{
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)data;
    //printf("WriteMemoryCallback\n");
    mem->memory = (char *)realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory) {
        memcpy(&(mem->memory[mem->size]), ptr, realsize);
        mem->size += realsize;
        mem->memory[mem->size] = 0;
    }
    return realsize;
}

static size_t upload_data_cb(void *ptr,size_t size,size_t nmemb,void *user_data)
{
	//struct upload_buffer *ub = user_data;
	struct upload_buffer *ub = (upload_buffer *)user_data;
    int len = (int)(size * nmemb);
    if ( len > ub->len )
        len = (int)ub->len;
    if ( len != 0 )
    {
        memcpy(ptr,ub->buf,len);
		//ub->buf += len;
		ub->buf = (char *)ub->buf + len;
        ub->len -= len;
    }
    return len;
}

char *bitcoind_RPC(int numretries,char *url,char *userpass,char *command,char *args)
{
    char *retstr,*quote;
    CURL *curl_handlez;
    CURLcode res;
    char len_hdr[1024],databuf[1024];
    struct curl_slist *headers = NULL;
    struct upload_buffer upload_data;
    struct MemoryStruct chunk;
    if ( args == 0 )
        args = "";
retry:
    chunk.memory = (char *)malloc(1);     // will be grown as needed by the realloc above
    chunk.size = 0;                 // no data at this point
    //curl_global_init(CURL_GLOBAL_ALL); //init the curl session
    curl_handlez = curl_easy_init();
    curl_easy_setopt(curl_handlez,CURLOPT_SSL_VERIFYHOST,0);
    curl_easy_setopt(curl_handlez,CURLOPT_SSL_VERIFYPEER,0);
    curl_easy_setopt(curl_handlez,CURLOPT_URL,url);
    curl_easy_setopt(curl_handlez,CURLOPT_WRITEFUNCTION,WriteMemoryCallback); // send all data to this function
    curl_easy_setopt(curl_handlez,CURLOPT_WRITEDATA,(void *)&chunk); // we pass our 'chunk' struct to the callback function
    curl_easy_setopt(curl_handlez,CURLOPT_USERAGENT,"libcurl-agent/1.0"); // some servers don't like requests that are made without a user-agent field, so we provide one
    if ( userpass != 0 )
        curl_easy_setopt(curl_handlez,CURLOPT_USERPWD,userpass);
    if ( command != 0 )
    {
        curl_easy_setopt(curl_handlez,CURLOPT_READFUNCTION,upload_data_cb);
        curl_easy_setopt(curl_handlez,CURLOPT_READDATA,&upload_data);
        curl_easy_setopt(curl_handlez,CURLOPT_POST,1);
        if ( args[0] != 0 )
            quote = "\"";
        else quote = "";
        sprintf(databuf,"{\"id\":\"jl777\",\"method\":\"%s\",\"params\":[%s%s%s]}",command,quote,args,quote);
        upload_data.buf = databuf;
        upload_data.len = strlen(databuf);
        sprintf(len_hdr, "Content-Length: %lu",(unsigned long)upload_data.len);
        headers = curl_slist_append(headers,"Content-type: application/json");
        headers = curl_slist_append(headers,len_hdr);
        curl_easy_setopt(curl_handlez,CURLOPT_HTTPHEADER,headers);
    }
    res = curl_easy_perform(curl_handlez);
    if ( res != CURLE_OK )
    {
        fprintf(stderr, "curl_easy_perform() failed: %s (%s %s %s %s)\n",curl_easy_strerror(res),url,userpass,command,args);
        sleep(30);
        if ( numretries-- > 0 )
        {
            free(chunk.memory);
            curl_easy_cleanup(curl_handlez);
            goto retry;
        }
    }
    else
    {
        // printf("%lu bytes retrieved [%s]\n", (int64_t )chunk.size,chunk.memory);
    }
    curl_easy_cleanup(curl_handlez);
    retstr = (char *)malloc(strlen(chunk.memory)+1);
    strcpy(retstr,chunk.memory);
    free(chunk.memory);
    return(retstr);
}


static bool submit_upstream_work(CURL *curl, struct work *work)
{
	char *str = NULL;
	json_t *val, *res, *reason;
	char s[345];
	int i;
	bool rc = false;

	/* pass if the previous hash is not the current previous hash */
	if (memcmp(work->data + 1, g_work.data + 1, 32)) {
		if (opt_debug)
			//applog(LOG_DEBUG, "DEBUG: stale work detected, discarding");
			printline(out_screen, true, "DEBUG: stale work detected, discarding");
		return true;
	}

	if (have_stratum) {
		uint32_t ntime, nonce;
		uint16_t nvote;
		char *ntimestr, *noncestr, *xnonce2str, *nvotestr;

		le32enc(&ntime, work->data[17]);
		le32enc(&nonce, work->data[19]);
		be16enc(&nvote, *((uint16_t*)&work->data[20]));

		ntimestr = bin2hex((const unsigned char *)(&ntime), 4);
		noncestr = bin2hex((const unsigned char *)(&nonce), 4);
		xnonce2str = bin2hex(work->xnonce2, work->xnonce2_len);
		nvotestr = bin2hex((const unsigned char *)(&nvote), 2);
		if (opt_algo == ALGO_HEAVY) {
			sprintf(s,
				"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
				rpc_user, work->job_id, xnonce2str, ntimestr, noncestr, nvotestr);
		} else {
			sprintf(s,
				"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":4}",
				rpc_user, work->job_id, xnonce2str, ntimestr, noncestr);
		}
		free(ntimestr);
		free(noncestr);
		free(xnonce2str);
		free(nvotestr);

		if (unlikely(!stratum_send_line(&stratum, s))) {
			//applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			printline(out_screen, true, "submit_upstream_work stratum_send_line failed");
			goto out;
		}
	} else {

		/* build hex string */

		if (opt_algo != ALGO_HEAVY && opt_algo != ALGO_MJOLLNIR) {
			for (i = 0; i < ARRAY_SIZE(work->data); i++)
				le32enc(work->data + i, work->data[i]);
			}
			str = bin2hex((unsigned char *)work->data, sizeof(work->data));
			if (unlikely(!str)) {
				//applog(LOG_ERR, "submit_upstream_work OOM");
				printline(out_screen, true, "submit_upstream_work OOM");
				goto out;
		}

		/* build JSON-RPC request */
		sprintf(s,
			"{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
			str);

		/* issue JSON-RPC request */
		val = json_rpc_call(curl, rpc_url, rpc_userpass, s, false, false, NULL);
		if (unlikely(!val)) {
			//applog(LOG_ERR, "submit_upstream_work json_rpc_call failed");
			printline(out_screen, true, "submit_upstream_work json_rpc_call failed");
			goto out;
		}

		res = json_object_get(val, "result");
		reason = json_object_get(val, "reject-reason");
		share_result(json_is_true(res), reason ? json_string_value(reason) : NULL);

		json_decref(val);
	}

	rc = true;

out:
	free(str);
	return rc;
}

static const char *rpc_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	bool rc;
	struct timeval tv_start, tv_end, diff;

	gettimeofday(&tv_start, NULL);
	val = json_rpc_call(curl, rpc_url, rpc_userpass, rpc_req,
			    want_longpoll, false, NULL);
	gettimeofday(&tv_end, NULL);

	if (have_stratum) {
		if (val)
			json_decref(val);
		return true;
	}

	if (!val)
		return false;

	rc = work_decode(json_object_get(val, "result"), work);

	if (opt_debug && rc) {
		timeval_subtract(&diff, &tv_end, &tv_start);
		/*applog(LOG_DEBUG, "DEBUG: got new work in %d ms",
		       diff.tv_sec * 1000 + diff.tv_usec / 1000);*/
		printline(out_screen, true, "DEBUG: got new work in %d ms",
		       diff.tv_sec * 1000 + diff.tv_usec / 1000);
	}

	json_decref(val);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		free(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static bool workio_get_work(struct workio_cmd *wc, CURL *curl)
{
	struct work *ret_work;
	int failures = 0;

	ret_work = (struct work*)calloc(1, sizeof(*ret_work));
	if (!ret_work)
		return false;

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(curl, ret_work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			//applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			printline(out_screen, true, "json_rpc_call failed, terminating workio thread");
			free(ret_work);
			return false;
		}

		/* pause, then restart work-request loop */
		/*applog(LOG_ERR, "json_rpc_call failed, retry after %d seconds",
			opt_fail_pause);*/
		printline(out_screen, true, "json_rpc_call failed, retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	/* send work to requesting thread */
	if (!tq_push(wc->thr->q, ret_work))
		free(ret_work);

	return true;
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(curl, wc->u.work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			printline(out_screen, true, "...terminating workio thread");
			//applog(LOG_ERR, "...terminating workio thread");
			return false;
		}

		/* pause, then restart work-request loop */
		/*applog(LOG_ERR, "...retry after %d seconds",
			opt_fail_pause);*/
		printline(out_screen, true, "...retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	return true;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info*)userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		printline(out_screen, true, "CURL initialization failed");
		//applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok) {
		struct workio_cmd *wc;

		/* wait for workio_cmd sent to us, on our queue */
		wc = (struct workio_cmd *)tq_pop(mythr->q, NULL);
		if (!wc) {
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_GET_WORK:
			ok = workio_get_work(wc, curl);
			break;
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc, curl);
			break;

		default:		/* should never happen */
			ok = false;
			break;
		}

		workio_cmd_free(wc);
	}

	tq_freeze(mythr->q);
	curl_easy_cleanup(curl);

	return NULL;
}

static bool get_work(struct thr_info *thr, struct work *work)
{
	struct workio_cmd *wc;
	struct work *work_heap;

	if (opt_benchmark) {
		memset(work->data, 0x55, 76);
		work->data[17] = swab32((uint32_t)time(NULL));
		memset(work->data + 19, 0x00, 52);
		work->data[20] = 0x80000000;
		work->data[31] = 0x00000280;
		memset(work->target, 0x00, sizeof(work->target));
		return true;
	}

	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->cmd = WC_GET_WORK;
	wc->thr = thr;

	/* send work request to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc)) {
		workio_cmd_free(wc);
		return false;
	}

	/* wait for response, a unit of work */
	work_heap = (struct work *)tq_pop(thr->q, NULL);
	if (!work_heap)
		return false;

	/* copy returned work into storage provided by caller */
	memcpy(work, work_heap, sizeof(*work));
	free(work_heap);

	return true;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;
	/* fill out work request message */
	wc = (struct workio_cmd *)calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = (struct work *)malloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(*work_in));

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	unsigned char merkle_root[64];
	int i;

	pthread_mutex_lock(&sctx->work_lock);

	strcpy(work->job_id, sctx->job.job_id);
	work->xnonce2_len = sctx->xnonce2_size;
	memcpy(work->xnonce2, sctx->job.xnonce2, sctx->xnonce2_size);

	/* Generate merkle root */
	if (opt_algo == ALGO_HEAVY || opt_algo == ALGO_MJOLLNIR)
		heavycoin_hash(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);
	else
	if (opt_algo == ALGO_FUGUE256 || opt_algo == ALGO_GROESTL)
		SHA256((unsigned char*)sctx->job.coinbase, sctx->job.coinbase_size, (unsigned char*)merkle_root);
	else
		sha256d(merkle_root, sctx->job.coinbase, (int)sctx->job.coinbase_size);

	for (i = 0; i < sctx->job.merkle_count; i++) {
		memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
		if (opt_algo == ALGO_HEAVY || opt_algo == ALGO_MJOLLNIR)
			heavycoin_hash(merkle_root, merkle_root, 64);
		else
			sha256d(merkle_root, merkle_root, 64);
	}
	
	/* Increment extranonce2 */
	for (i = 0; i < (int)sctx->xnonce2_size && !++sctx->job.xnonce2[i]; i++);

	/* Assemble block header */
	memset(work->data, 0, 128);
	work->data[0] = le32dec(sctx->job.version);
	for (i = 0; i < 8; i++)
		work->data[1 + i] = le32dec((uint32_t *)sctx->job.prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
	work->data[17] = le32dec(sctx->job.ntime);
	work->data[18] = le32dec(sctx->job.nbits);
	if (opt_algo == ALGO_MJOLLNIR)
	{
		for (i = 0; i < 20; i++)
			work->data[i] = be32dec((uint32_t *)&work->data[i]);
	}

	work->data[20] = 0x80000000;
	work->data[31] = (opt_algo == ALGO_MJOLLNIR) ? 0x000002A0 : 0x00000280;

	// HeavyCoin
	if (opt_algo == ALGO_HEAVY) {
		uint16_t *ext;
		work->maxvote = 1024;
		ext = (uint16_t*)(&work->data[20]);
		ext[0] = opt_vote;
		ext[1] = be16dec(sctx->job.nreward);

		for (i = 0; i < 20; i++)
			work->data[i] = be32dec((uint32_t *)&work->data[i]);
	}
	//

	pthread_mutex_unlock(&sctx->work_lock);

	if (opt_debug) {
		char *xnonce2str = bin2hex(work->xnonce2, sctx->xnonce2_size);
		/*applog(LOG_DEBUG, "DEBUG: job_id='%s' extranonce2=%s ntime=%08x",
		       work->job_id, xnonce2str, swab32(work->data[17]));*/
		printline(out_screen, true, "DEBUG: job_id='%s' extranonce2=%s ntime=%08x",
		       work->job_id, xnonce2str, swab32(work->data[17]));
		free(xnonce2str);
	}

	if (opt_algo == ALGO_JACKPOT)
		diff_to_target(work->target, sctx->job.diff / (65536.0 * opt_difficulty));
	else if (opt_algo == ALGO_FUGUE256 || opt_algo == ALGO_GROESTL || opt_algo == ALGO_DMD_GR)
		diff_to_target(work->target, sctx->job.diff / (256.0 * opt_difficulty));
	else
		diff_to_target(work->target, sctx->job.diff / opt_difficulty);
}

static void *miner_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	int thr_id = mythr->id;
	struct work work;
	uint32_t max_nonce;
	uint32_t end_nonce = 0xffffffffU / opt_n_threads * (thr_id + 1) - 0x20;
	unsigned char *scratchbuf = NULL;
	char s[16];
	int i;
    static int rounds = 0;
	
	const char *key;
	json_t *valu;
	json_error_t error;
	json_t *value, *array;
	double balance,dif;


	memset(&work, 0, sizeof(work)); // prevent work from being used uninitialized

	/* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
	 * and if that fails, then SCHED_BATCH. No need for this to be an
	 * error if it fails */
	if (!opt_benchmark) {
		setpriority(PRIO_PROCESS, 0, 19);
		drop_policy();
	}

	/* Cpu affinity only makes sense if the number of threads is a multiple
	 * of the number of CPUs */
	if (num_processors > 1 && opt_n_threads % num_processors == 0) {
		if (!opt_quiet)
			printline(out_screen, true, "Binding thread %d to cpu %d",
			       thr_id, thr_id % num_processors);
			//applog(LOG_INFO, "Binding thread %d to cpu %d",
			//      thr_id, thr_id % num_processors);
		affine_to_cpu(thr_id, thr_id % num_processors);
	}

	while (1) {
		unsigned long hashes_done;
		struct timeval tv_start, tv_end, diff;
		int64_t max64;
		int rc;

		if (have_stratum) {
			while (time(NULL) >= g_work_time + 60)
				sleep(1);
			pthread_mutex_lock(&g_work_lock);
			if (work.data[19] >= end_nonce)
				stratum_gen_work(&stratum, &g_work);
		} else {
			/* obtain new work from internal workio thread */
			pthread_mutex_lock(&g_work_lock);
			if (!have_stratum && (!have_longpoll ||
					time(NULL) >= g_work_time + LP_SCANTIME*3/4 ||
					work.data[19] >= end_nonce)) {
				if (unlikely(!get_work(mythr, &g_work))) {
					printline(out_screen, true, "work retrieval failed, exiting "
						"mining thread %d", mythr->id);
					//applog(LOG_ERR, "work retrieval failed, exiting "
					//	"mining thread %d", mythr->id);
					pthread_mutex_unlock(&g_work_lock);
					goto out;
				}
				g_work_time = have_stratum ? 0 : time(NULL);
			}
			if (have_stratum) {
				pthread_mutex_unlock(&g_work_lock);
				continue;
			}
		}
		if (memcmp(work.data, g_work.data, 76)) {
			memcpy(&work, &g_work, sizeof(struct work));
			work.data[19] = 0xffffffffU / opt_n_threads * thr_id;
		} else
			work.data[19]++;
		pthread_mutex_unlock(&g_work_lock);
		work_restart[thr_id].restart = 0;

		/* adjust max_nonce to meet target scan time */
		if (have_stratum)
			max64 = LP_SCANTIME;
		else
			max64 = g_work_time + (have_longpoll ? LP_SCANTIME : opt_scantime)
			      - time(NULL);
		max64 *= (int64_t)thr_hashrates[thr_id];
		if (max64 <= 0)
			max64 = (opt_algo == ALGO_JACKPOT) ? 0x1fffLL : 0xfffffLL;
		if ((int64_t)work.data[19] + max64 > end_nonce)
			max_nonce = end_nonce;
		else
			max_nonce = (uint32_t)(work.data[19] + max64);

		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {

		case ALGO_HEAVY:
			rc = scanhash_heavy(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, work.maxvote, HEAVYCOIN_BLKHDR_SZ);
			break;

		case ALGO_MJOLLNIR:
			rc = scanhash_heavy(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done, 0, MNR_BLKHDR_SZ);
			break;

		case ALGO_FUGUE256:
			rc = scanhash_fugue256(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_GROESTL:
		case ALGO_DMD_GR:
			rc = scanhash_groestlcoin(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_MYR_GR:
			rc = scanhash_myriad(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_JACKPOT:
			rc = scanhash_jackpot(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_QUARK:
			rc = scanhash_quark(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_ANIME:
			rc = scanhash_anime(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_NIST5:
			rc = scanhash_nist5(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_X11:
			rc = scanhash_x11(thr_id, work.data, work.target,
			                      max_nonce, &hashes_done);
			break;

		case ALGO_X13:
			rc = scanhash_x13(thr_id, work.data, work.target,
 			                      max_nonce, &hashes_done);
 			break;

		default:
			/* should never happen */
			goto out;
		}

        if (opt_benchmark)
            if (++rounds == 1) {
				destroywins();
				exit(0);
			}

		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);
		timeval_subtract(&diff, &tv_end, &tv_start);
		if (diff.tv_usec || diff.tv_sec) {
			pthread_mutex_lock(&stats_lock);
			thr_hashrates[thr_id] =
				hashes_done / (diff.tv_sec + 1e-6 * diff.tv_usec);
			pthread_mutex_unlock(&stats_lock);
		}
		
		if (thr_hashrates[thr_id] > 1e8){
			printline(out_screen, true, "abnormal hashes %f, exiting with code 211!", thr_hashrates[thr_id]);
			//applog(LOG_ERR, "abnormal hashes %f, exiting with code 211!", thr_hashrates[thr_id]);
			destroywins();
			exit(211);
        }

		/*if (have_stratum) {
			
		} else {
			char *rpcinfo = bitcoind_RPC(3,rpc_url,rpc_userpass,"getinfo",0);
			//char *rpcinfo;
			//mvwprintw(info_screen, 6, 0, rpcinfo);
			value = json_loads( rpcinfo, &error);
			if ( !value )
				mvwprintw(info_screen, 6, 0, "error: on line %d: %s", error.line, error.text );
			array = json_object_get(value, "result");
			balance=json_real_value(json_object_get(array,"balance"));
			dif=json_real_value(json_object_get(array,"difficulty"));
			//json_decref(array);
			//json_decref(value);
		}*/

		gpuinfo(thr_id,dif,balance);

		menukey = wgetch(info_screen);

		switch(menukey)
		{
			case KEY_F(10):
				//destroywins();
				//applog(LOG_INFO, "Normal exit by user request...");
				delwin(info_screen);
				delwin(out_screen);
				endwin();
				printf("Normal exit by user request...\n");
				exit(0);
				break;
			default:
				break;
		}

		if (!opt_quiet) {
			sprintf(s, thr_hashrates[thr_id] >= 1e6 ? "%.0f" : "%.2f",
				1e-3 * thr_hashrates[thr_id]);
			printline(out_screen, true, "GPU #%d: %s, %s khash/s",
				device_map[thr_id], device_name[thr_id], s);

			/*applog(LOG_INFO, "GPU #%d: %s, %s khash/s",
				device_map[thr_id], device_name[thr_id], s);*/
//			applog(LOG_INFO, "thread %d: %lu hashes, %s khash/s",
//				thr_id, hashes_done, s);
		}
		if (opt_benchmark && thr_id == opt_n_threads - 1) {
			double hashrate = 0.;
			for (i = 0; i < opt_n_threads && thr_hashrates[i]; i++)
				hashrate += thr_hashrates[i];
			if (i == opt_n_threads) {
				sprintf(s, hashrate >= 1e6 ? "%.0f" : "%.2f", 1e-3 * hashrate);
				printline(out_screen, true, "Total: %s khash/s", s);
				//applog(LOG_INFO, "Total: %s khash/s", s);
			}
		}

		/* if nonce found, submit work */
		if (rc && !opt_benchmark && !submit_work(mythr, &work))
			break;
	}

out:
	tq_freeze(mythr->q);

	return NULL;
}

static void restart_threads(void)
{
	int i;

	for (i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	CURL *curl = NULL;
	char *copy_start, *hdr_path = NULL, *lp_url = NULL;
	bool need_slash = false;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		//applog(LOG_ERR, "CURL initialization failed");
		printline(out_screen, true, "CURL initialization failed");
		goto out;
	}

start:
	hdr_path = (char*)tq_pop(mythr->q, NULL);
	if (!hdr_path)
		goto out;

	/* full URL */
	if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	
	/* absolute path, on current server */
	else {
		copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (rpc_url[strlen(rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = (char*)malloc(strlen(rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

		sprintf(lp_url, "%s%s%s", rpc_url, need_slash ? "/" : "", copy_start);
	}

	//applog(LOG_INFO, "Long-polling activated for %s", lp_url);
	printline(out_screen, true, "Long-polling activated for %s", lp_url);

	while (1) {
		json_t *val, *soval;
		int err;

		val = json_rpc_call(curl, lp_url, rpc_userpass, rpc_req,
				    false, true, &err);
		if (have_stratum) {
			if (val)
				json_decref(val);
			goto out;
		}
		if (likely(val)) {
			if (!opt_quiet) printline(out_screen, true, "LONGPOLL detected new block");//applog(LOG_INFO, "LONGPOLL detected new block");
			soval = json_object_get(json_object_get(val, "result"), "submitold");
			submit_old = soval ? json_is_true(soval) : false;
			pthread_mutex_lock(&g_work_lock);
			if (work_decode(json_object_get(val, "result"), &g_work)) {
				if (opt_debug)
					//applog(LOG_DEBUG, "DEBUG: got new work");
					printline(out_screen, true, "DEBUG: got new work");
				time(&g_work_time);
				restart_threads();
			}
			pthread_mutex_unlock(&g_work_lock);
			json_decref(val);
		} else {
			pthread_mutex_lock(&g_work_lock);
			g_work_time -= LP_SCANTIME;
			pthread_mutex_unlock(&g_work_lock);
			if (err == CURLE_OPERATION_TIMEDOUT) {
				restart_threads();
			} else {
				have_longpoll = false;
				restart_threads();
				free(hdr_path);
				free(lp_url);
				lp_url = NULL;
				sleep(opt_fail_pause);
				goto start;
			}
		}
	}

out:
	free(hdr_path);
	free(lp_url);
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;
}

static bool stratum_handle_response(char *buf)
{
	json_t *val, *err_val, *res_val, *id_val;
	json_error_t err;
	bool ret = false;

	val = JSON_LOADS(buf, &err);
	if (!val) {
		//applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
		printline(out_screen, true, "JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");

	if (!id_val || json_is_null(id_val) || !res_val)
		goto out;

	share_result(json_is_true(res_val),
		err_val ? json_string_value(json_array_get(err_val, 1)) : NULL);

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}

static void *stratum_thread(void *userdata)
{
	struct thr_info *mythr = (struct thr_info *)userdata;
	char *s;

	stratum.url = (char*)tq_pop(mythr->q, NULL);
	if (!stratum.url)
		goto out;
	printline(out_screen, true, "Starting Stratum on %s", stratum.url);
	//applog(LOG_INFO, "Starting Stratum on %s", stratum.url);

	while (1) {
		int failures = 0;

		while (!stratum.curl) {
			pthread_mutex_lock(&g_work_lock);
			g_work_time = 0;
			pthread_mutex_unlock(&g_work_lock);
			restart_threads();

			if (!stratum_connect(&stratum, stratum.url) ||
			    !stratum_subscribe(&stratum) ||
			    !stratum_authorize(&stratum, rpc_user, rpc_pass)) {
				stratum_disconnect(&stratum);
				if (opt_retries >= 0 && ++failures > opt_retries) {
					printline(out_screen, true, "..terminating workio thread");
					//applog(LOG_ERR, "...terminating workio thread");
					tq_push(thr_info[work_thr_id].q, NULL);
					goto out;
				}
				printline(out_screen, true, "...retry after %d seconds", opt_fail_pause);
				//applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
		}

		if (stratum.job.job_id &&
		    (strcmp(stratum.job.job_id, g_work.job_id) || !g_work_time)) {
			pthread_mutex_lock(&g_work_lock);
			stratum_gen_work(&stratum, &g_work);
			time(&g_work_time);
			pthread_mutex_unlock(&g_work_lock);
			if (stratum.job.clean) {
				if (!opt_quiet) printline(out_screen, true, "Stratum detected new block");//applog(LOG_INFO, "Stratum detected new block");
				restart_threads();
			}
		}
		
		if (!stratum_socket_full(&stratum, 60)) {
			printline(out_screen, true, "Stratum connection timed out");
			//applog(LOG_ERR, "Stratum connection timed out");
			s = NULL;
		} else
			s = stratum_recv_line(&stratum);
		if (!s) {
			stratum_disconnect(&stratum);
			printline(out_screen, true, "Stratum connection interrupted");
			//applog(LOG_ERR, "Stratum connection interrupted");
			continue;
		}
		if (!stratum_handle_method(&stratum, s))
			stratum_handle_response(s);
		free(s);
	}

out:
	return NULL;
}

static void show_version_and_exit(void)
{
	printline(out_screen, false, "%s\n%s\n", PACKAGE_STRING, curl_version());
	//printf("%s\n%s\n", PACKAGE_STRING, curl_version());
	destroywins();
	exit(0);
}

static void pdcurs_loop()
{
	initscr();
	refresh();
	idlok(stdscr, true);
	scrollok(stdscr, true);
	endwin();
	printf("     *** ccMiner for nVidia GPUs by Christian Buchner and Christian H. ***\n");
	printf("\t             This is version "PROGRAM_VERSION" (beta)\n");
	printf("\t  based on pooler-cpuminer 2.3.2 (c) 2010 Jeff Garzik, 2012 pooler\n");
	printf("\t  based on pooler-cpuminer extension for HVC from\n\t       https://github.com/heavycoin/cpuminer-heavycoin\n");
	printf("\t\t\tand\n\t       http://hvc.1gh.com/\n");
	printf("\tCuda additions Copyright 2014 Christian Buchner, Christian H.\n");
	printf("\t  LTC donation address: LKS1WDKGED647msBQfLBHV3Ls8sveGncnm\n");
	printf("\t  BTC donation address: 16hJF5mceSojnTD3ZTUDqdRhDyPJzoRakM\n");
	printf("\t  YAC donation address: Y87sptDEcpLkLeAuex6qZioDbvy1qXZEj4\n\n");
}

static void show_usage_and_exit(int status)
{
	//destroywins();
	pdcurs_loop();
	if (status)
		//printline(out_screen, false, "Try `" PROGRAM_NAME " --help' for more information.\n");
		fprintf(stderr, "Try `" PROGRAM_NAME " --help' for more information.\n");
	else
		printf(usage);
	exit(status);
}

static int compare (const void * a, const void * b)
{
  return ( *(int*)a - *(int*)b );
}

static void parse_arg (int key, char *arg)
{
	char *p;
	int v, i;
	double d;

	switch(key) {
	case 'a':
		for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
			if (algo_names[i] &&
			    !strcmp(arg, algo_names[i])) {
				opt_algo = (sha256_algos)i;
				break;
			}
		}
		if (i == ARRAY_SIZE(algo_names))
			show_usage_and_exit(1);
		break;
	case 'B':
		opt_background = true;
		break;
	case 'c': {
		json_error_t err;
		if (opt_config)
			json_decref(opt_config);
#if JANSSON_VERSION_HEX >= 0x020000
		opt_config = json_load_file(arg, 0, &err);
#else
		opt_config = json_load_file(arg, &err);
#endif
		if (!json_is_object(opt_config)) {
			pdcurs_loop();
			//applog(LOG_ERR, "JSON decode of %s failed", arg);
			printf("JSON decode of %s failed", arg);
			exit(1);
		}
		break;
	}
	case 'q':
		opt_quiet = true;
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'p':
		free(rpc_pass);
		rpc_pass = strdup(arg);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 's':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_scantime = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999)	/* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 't':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_n_threads = v;
		break;
	case 'v':
		v = atoi(arg);
		if (v < 0 || v > 1024)	/* sanity check */
			show_usage_and_exit(1);
		opt_vote = (uint16_t)v;
		break;
	case 'm':
		opt_trust_pool = true;
		break;
	case 'u':
		free(rpc_user);
		rpc_user = strdup(arg);
		break;
	case 'o':			/* --url */
		p = strstr(arg, "://");
		if (p) {
			if (strncasecmp(arg, "http://", 7) && strncasecmp(arg, "https://", 8) &&
					strncasecmp(arg, "stratum+tcp://", 14))
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = strdup(arg);
		} else {
			if (!strlen(arg) || *arg == '/')
				show_usage_and_exit(1);
			free(rpc_url);
			rpc_url = (char*)malloc(strlen(arg) + 8);
			sprintf(rpc_url, "http://%s", arg);
		}
		p = strrchr(rpc_url, '@');
		if (p) {
			char *sp, *ap;
			*p = '\0';
			ap = strstr(rpc_url, "://") + 3;
			sp = strchr(ap, ':');
			if (sp) {
				free(rpc_userpass);
				rpc_userpass = strdup(ap);
				free(rpc_user);
				rpc_user = (char*)calloc(sp - ap + 1, 1);
				strncpy(rpc_user, ap, sp - ap);
				free(rpc_pass);
				rpc_pass = strdup(sp + 1);
			} else {
				free(rpc_user);
				rpc_user = strdup(ap);
			}
			memmove(ap, p + 1, strlen(p + 1) + 1);
		}
		have_stratum = !opt_benchmark && !strncasecmp(rpc_url, "stratum", 7);
		break;
	case 'O':			/* --userpass */
		p = strchr(arg, ':');
		if (!p)
			show_usage_and_exit(1);
		free(rpc_userpass);
		rpc_userpass = strdup(arg);
		free(rpc_user);
		rpc_user = (char*)calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		free(rpc_pass);
		rpc_pass = strdup(p + 1);
		break;
	case 'x':			/* --proxy */
		if (!strncasecmp(arg, "socks4://", 9))
			opt_proxy_type = CURLPROXY_SOCKS4;
		else if (!strncasecmp(arg, "socks5://", 9))
			opt_proxy_type = CURLPROXY_SOCKS5;
#if LIBCURL_VERSION_NUM >= 0x071200
		else if (!strncasecmp(arg, "socks4a://", 10))
			opt_proxy_type = CURLPROXY_SOCKS4A;
		else if (!strncasecmp(arg, "socks5h://", 10))
			opt_proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
#endif
		else
			opt_proxy_type = CURLPROXY_HTTP;
		free(opt_proxy);
		opt_proxy = strdup(arg);
		break;
	case 1001:
		free(opt_cert);
		opt_cert = strdup(arg);
		break;
	case 1005:
		opt_benchmark = true;
		want_longpoll = false;
		want_stratum = false;
		have_stratum = false;
		break;
	case 1003:
		want_longpoll = false;
		break;
	case 1007:
		want_stratum = false;
		break;
	case 'S':
		use_syslog = true;
		break;
	case 'd': // CB
		{
			char * pch = strtok (arg,",");
			opt_n_threads = 0;
			while (pch != NULL) {
				if (pch[0] >= '0' && pch[0] <= '9' && pch[1] == '\0')
				{
					if (atoi(pch) < num_processors)
						device_map[opt_n_threads++] = atoi(pch);
					else {
						pdcurs_loop();
						//applog(LOG_ERR, "Non-existant CUDA device #%d specified in -d option", atoi(pch));
						printf("Non-existant CUDA device #%d specified in -d option", atoi(pch));
						exit(1);
					}
				} else {
					int device = cuda_finddevice(pch);
					if (device >= 0 && device < num_processors)
						device_map[opt_n_threads++] = device;
					else {
						pdcurs_loop();
						//applog(LOG_ERR, "Non-existant CUDA device '%s' specified in -d option", pch);
						printf("Non-existant CUDA device '%s' specified in -d option", pch);
						//destroywins();
						exit(1);
					}
				}
				//qsort (device_map, 8, sizeof(int), compare);
				//device_map = device_map_invert;
				pch = strtok (NULL, ",");
			}
		}
		break;
	case 'f': // CH - Divisor for Difficulty
		d = atof(arg);
		if (d == 0)	/* sanity check */
			show_usage_and_exit(1);
		opt_difficulty = d;
		break;
	case 'V':
		show_version_and_exit();
	case 'h':
		show_usage_and_exit(0);
	default:
		show_usage_and_exit(1);
	}
}

static void parse_config(void)
{
	int i;
	json_t *val;

	if (!json_is_object(opt_config))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		if (!options[i].name)
			break;
		if (!strcmp(options[i].name, "config"))
			continue;

		val = json_object_get(opt_config, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				break;
			parse_arg(options[i].val, s);
			free(s);
		} else if (!options[i].has_arg && json_is_true(val))
			parse_arg(options[i].val, "");
		else
			/*applog(LOG_ERR, "JSON option %s invalid",
				options[i].name);*/
			printline(out_screen, true, "JSON option %s invalid",
				options[i].name);
	}

	if (opt_algo == ALGO_HEAVY && opt_vote == 9999) {
		printline(out_screen, false, "Heavycoin hash requires block reward vote parameter (see --vote)\n");
		//fprintf(stderr, "Heavycoin hash requires block reward vote parameter (see --vote)\n");
		show_usage_and_exit(1);
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
#if HAVE_GETOPT_LONG
		key = getopt_long(argc, argv, short_options, options, NULL);
#else
		key = getopt(argc, argv, short_options);
#endif
		if (key < 0)
			break;

		parse_arg(key, optarg);
	}
	if (optind < argc) {
		//printline(out_screen, false, "%s: unsupported non-option argument '%s'\n",
		//	argv[0], argv[optind]);
		fprintf(stderr, "%s: unsupported non-option argument '%s'\n",
			argv[0], argv[optind]);
		show_usage_and_exit(1);
	}

	if (opt_algo == ALGO_HEAVY && opt_vote == 9999) {
		//printline(out_screen, false, "%s: Heavycoin hash requires block reward vote parameter (see --vote)\n",
		//	argv[0]);
		fprintf(stderr, "%s: Heavycoin hash requires block reward vote parameter (see --vote)\n",
			argv[0]);
		show_usage_and_exit(1);
	}

	parse_config();
}

#ifndef WIN32
static void signal_handler(int sig)
{
	switch (sig) {
	case SIGHUP:
		//applog(LOG_INFO, "SIGHUP received");
		printline(out_screen, true, "SIGHUP received");
		break;
	case SIGINT:
		//applog(LOG_INFO, "SIGINT received, exiting");
		printline(out_screen, true, "SIGINT received, exiting");
		exit(0);
		break;
	case SIGTERM:
		//applog(LOG_INFO, "SIGTERM received, exiting");
		printline(out_screen, true, "SIGTERM received, exiting");
		exit(0);
		break;
	}
}
#endif

void bubbleSort(int *array, int *array2, char *array3, int length)//Bubble sort function 
{
	int i,j;
	for(i=0;i<10;i++)
	{
		for(j=0;j<i;j++)
		{
			if(array[i]>array[j])
			{
				int temp=array[i]; //swap 
				array[i]=array[j];
				array[j]=temp;
				temp=array2[i]; //swap 
				array2[i]=array2[j];
				array2[j]=temp;
				char str=array3[i]; //swap 
				array3[i]=array3[j];
				array3[j]=str;
			}

		}

	}

}

int main(int argc, char *argv[])
{
	struct thr_info *thr;
	long flags;
	int i;
	char *gpuByPhysicalStr;

#ifdef WIN32
	SYSTEM_INFO sysinfo;
#endif
	/*initscr();
	scrollok(stdscr, true);
	idlok(stdscr, true);*/

	rpc_user = strdup("");
	rpc_pass = strdup("");

	//cuda_devicenames();
	//num_processors = cuda_num_devices();
	//gpuinfo();

	pthread_mutex_init(&applog_lock, NULL);
	num_processors = cuda_num_devices();

	/* parse command line */
	parse_cmdline(argc, argv);

	cuda_devicenames();
	nw_nvidia_init();

	get_bus_ids();


//	gpuinfo();

	//for (int i = 0; i < num_processors-1; i++)

	if (!opt_benchmark && !rpc_url) {
		//printline(out_screen, false, "%s: no URL supplied\n", argv[0]);
		fprintf(stderr, "%s: no URL supplied\n", argv[0]);
		show_usage_and_exit(1);
	}

	if (!rpc_userpass) {
		rpc_userpass = (char*)malloc(strlen(rpc_user) + strlen(rpc_pass) + 2);
		if (!rpc_userpass)
			return 1;
		sprintf(rpc_userpass, "%s:%s", rpc_user, rpc_pass);
	}

	pthread_mutex_init(&stats_lock, NULL);
	pthread_mutex_init(&g_work_lock, NULL);
	pthread_mutex_init(&stratum.sock_lock, NULL);
	pthread_mutex_init(&stratum.work_lock, NULL);

	flags = !opt_benchmark && strncmp(rpc_url, "https:", 6)
	      ? (CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL)
	      : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		//printline(out_screen, true, "CURL initialization failed");
		applog(LOG_ERR, "CURL initialization failed");
		return 1;
	}

#ifndef WIN32
	if (opt_background) {
		i = fork();
		if (i < 0) exit(1);
		if (i > 0) exit(0);
		i = setsid();
		if (i < 0)
			applog(LOG_ERR, "setsid() failed (errno = %d)", errno);
			//printline(out_screen, true, "setsid() failed (errno = %d)", errno);
		i = chdir("/");
		if (i < 0)
			applog(LOG_ERR, "chdir() failed (errno = %d)", errno);
			//printline(out_screen, true, "chdir() failed (errno = %d)", errno);
		signal(SIGHUP, signal_handler);
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);
	}
#endif

	if (num_processors == 0)
	{
		//printline(out_screen, true, "No CUDA devices found! terminating.");
		applog(LOG_ERR, "No CUDA devices found! terminating.");
		//destroywins();
		exit(1);
	}
	if (!opt_n_threads)
		opt_n_threads = num_processors;

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog("cpuminer", LOG_PID, LOG_USER);
#endif

	SetWindow(85,30);
	initscr();
	noecho();
	raw();
	cbreak();
	
	getmaxyx(stdscr, parent_y, parent_x);
	// set up initial windows
	info_screen = newwin(parent_y / 2, parent_x, 0, 0);
	out_screen = newwin(parent_y /2 - 1, parent_x, parent_y / 2 , 0);
	menu_screen = newwin(1, parent_x, parent_y - 1, 0);
	wborder(info_screen,' ', ' ', '_', '_', '_', '_', '_', '_');
	scrollok(out_screen, TRUE);
	scrollok(info_screen, TRUE);
	keypad(info_screen, TRUE);
	nodelay(info_screen,TRUE);

	start_color();
	init_pair(1, COLOR_GREEN, COLOR_BLACK); 
	init_pair(2, COLOR_WHITE, COLOR_BLACK);
	init_pair(3, COLOR_CYAN, COLOR_BLACK);

	wcolor_set(out_screen, 1, NULL);
	wcolor_set(menu_screen, 3, NULL);
	vwprintw(menu_screen," F10 Exit ", NULL);

	//updatescr();

	//printf
	vwprintw(out_screen, "     *** ccMiner for nVidia GPUs by Christian Buchner and Christian H. ***\n", NULL);
	vwprintw(out_screen, "\t             This is version "PROGRAM_VERSION" (beta)\n", NULL);
	vwprintw(out_screen, "\t  based on pooler-cpuminer 2.3.2 (c) 2010 Jeff Garzik, 2012 pooler\n", NULL);
	vwprintw(out_screen, "\t  based on pooler-cpuminer extension for HVC from\n\t       https://github.com/heavycoin/cpuminer-heavycoin\n", NULL);
	vwprintw(out_screen, "\t\t\tand\n\t       http://hvc.1gh.com/\n", NULL);
	vwprintw(out_screen, "\tCuda additions Copyright 2014 Christian Buchner, Christian H.\n", NULL);
	vwprintw(out_screen, "\t  LTC donation address: LKS1WDKGED647msBQfLBHV3Ls8sveGncnm\n", NULL);
	vwprintw(out_screen, "\t  BTC donation address: 16hJF5mceSojnTD3ZTUDqdRhDyPJzoRakM\n", NULL);
	vwprintw(out_screen, "\t  YAC donation address: Y87sptDEcpLkLeAuex6qZioDbvy1qXZEj4\n", NULL);
	updatescr();
	wcolor_set(out_screen, 2, NULL);

	work_restart = (struct work_restart *)calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return 1;

	thr_info = (struct thr_info *)calloc(opt_n_threads + 3, sizeof(*thr));
	if (!thr_info)
		return 1;
	
	thr_hashrates = (double *) calloc(opt_n_threads, sizeof(double));
	if (!thr_hashrates)
		return 1;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return 1;

	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		//applog(LOG_ERR, "workio thread create failed");
		printline(out_screen, true, "workio thread create failed");
		return 1;
	}

	if (want_longpoll && !have_stratum) {
		/* init longpoll thread info */
		longpoll_thr_id = opt_n_threads + 1;
		thr = &thr_info[longpoll_thr_id];
		thr->id = longpoll_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start longpoll thread */
		if (unlikely(pthread_create(&thr->pth, NULL, longpoll_thread, thr))) {
			//applog(LOG_ERR, "longpoll thread create failed");
			printline(out_screen, true, "longpoll thread create failed");
			return 1;
		}
	}
	if (want_stratum) {
		/* init stratum thread info */
		stratum_thr_id = opt_n_threads + 2;
		thr = &thr_info[stratum_thr_id];
		thr->id = stratum_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
			//applog(LOG_ERR, "stratum thread create failed");
			printline(out_screen, true, "stratum thread create failed");
			return 1;
		}

		if (have_stratum)
			tq_push(thr_info[stratum_thr_id].q, strdup(rpc_url));
	}

	//mvwprintw(info_screen, 8, 0, "GPU phys -d ");

	/* start mining threads */
	for (i = 0; i < opt_n_threads; i++) {
		thr = &thr_info[i];

		thr->id = i;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		if (unlikely(pthread_create(&thr->pth, NULL, miner_thread, thr))) {
			//applog(LOG_ERR, "thread %d create failed", i);
			printline(out_screen, true, "thread %d create failed", i);
			return 1;
		}

		/*wprintw(info_screen, "%u", invert[i]);

		if (i < opt_n_threads - 1)
			wprintw(info_screen, ",");*/
		
	}

	
	//mvwprintw(info_screen, 8, i+10, "%s", gpuByPhysicalStr);

	printline(out_screen, true, "%d miner threads started, using '%s' algorithm.", opt_n_threads, algo_names[opt_algo]);

	/*applog(LOG_INFO, "%d miner threads started, "
		"using '%s' algorithm.",
		opt_n_threads,
		algo_names[opt_algo]);*/

#ifdef WIN32
	timeBeginPeriod(1); // enable high timer precision (similar to Google Chrome Trick)
#endif

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);

#ifdef WIN32
	timeEndPeriod(1); // be nice and forego high timer precision
#endif

	//applog(LOG_INFO, "workio thread dead, exiting.");
	printline(out_screen, true, "workio thread dead, exiting.");

	destroywins();
	return 0;
}
