/*
 * middleware.h
 *
 *  Created on: 29 Feb 2016
 *	  Author: Raluca Diaconu
 */

#include "middleware.h"

#include "load_mw_config.h"

#include "utils.h"
#include "environment.h"
#include "message.h"
#include "hashmap.h"
#include "json.h"
#include "com_wrapper.h"
//#include "slog.h"
#include "file.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h> /* CTRL-C handler */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include <errno.h>
#include <limits.h> //for PATH_MAX

#include <conn_fifo.h>

#include <sts_queue.h>

/* indicates weather the app awaits a blocking return */
int waiting_blocking_call = 0;

/*the message id that is blocking */
char blocking_msg_id[13];

/*
 * sync for blocking calls
 * send message when corresp blocking call response has arrived
 */
int fds_blocking_call[2];

/* main module to communicate with the core */
COM_MODULE *sockpair_module;

/* public app id to the core */
char* app_name = NULL;

/* key to uniquely identify to the core during app - core handshake */
char* rand_key;

/* fd corresponding to the core sock */
int app_core_conn = -1;

/* FIXME: module for stream endpoints */
COM_MODULE * _module_fifo;

/* fd for stream endpoints */
int stream_fd_global = 0;

/* helper functions declaration */
int core_spawn_addr(char* core_addr);
int core_spawn_fd(int fds, char* core_addr);
int core_spawn_fifo(char* app_name); // not used yet

void api_on_data(COM_MODULE* module, int conn, const void * msg, unsigned int size);
void api_on_connect(void* module, int conn);
void api_on_disconnect(void* module, int conn);

void* api_on_message(void* data);
void api_on_first_data(COM_MODULE* module, int conn, const void* msg, unsigned int size);

/* message thread */
typedef struct {
	char* msg_str;
} api_msg_t;
volatile bool api_msg_queue_active;
StsHeader* api_msg_queue;
pthread_t api_msg_thread;

void api_thread_create();
void* api_thread_func(void* _blank);
void api_thread_destroy();

void api_thread_push(const char* msg_str);

/* buffer stuff */

typedef struct _BUFFER{
	char* data;
	int size;

	int buffer_state;
	int brackets;
}BUFFER;

BUFFER* api_buffer = NULL;

void buffer_set(BUFFER* buffer, const void* new_buf, unsigned int new_start, unsigned int new_end);

void buffer_update(BUFFER* buffer, const void* new_data, unsigned int new_size);

void buffer_reset(BUFFER* buffer);

/* functionality implementation */

void atexit_cb()
{
	(*(sockpair_module->fc_connection_close))(app_core_conn);

#ifdef __ANDROID__
	exit(0);
#endif // __ANDROID__

}

void atexit_app()
{
	printf("AT EXIT APP: MEME MEME\n");
	mw_terminate_core();
	atexit_cb();
}

void int_handler(int sig)
{
	// Kill properly...
	//atexit_app();
	//atexit_cb();
	printf("SIGINT: Terminating process...\n");
	exit(1);
}


char* mw_init(const char* cpt_name, int log_lvl, bool use_socketpair)
{
	//pthread_attr_setstacksize(1024);
	app_name = strdup_null(cpt_name);
	/* seed random for endpoint ids */
	srand (time(NULL));

	/* init socketpair for blocking calls */
	int sockpair_err = socketpair(AF_LOCAL, SOCK_STREAM, 0, fds_blocking_call);
	if(sockpair_err)
	{
		//slog(SLOG_ERROR, "MW: Blocking call socketpair error: %d.", sockpair_err);
		return NULL;
	}

	/* Set 5 Sec Timeout for blocking function calls */
	const struct timeval sock_timeout={.tv_sec=5, .tv_usec=0};
	sockpair_err = setsockopt(fds_blocking_call[1], SOL_SOCKET, SO_RCVTIMEO, (char*)&sock_timeout, sizeof(sock_timeout));
	if(sockpair_err)
	{
		//slog(SLOG_ERROR, "MW: Blocking call socketpair error: %d.", sockpair_err);
		return NULL;
	}

	api_buffer = (BUFFER*) malloc(sizeof(BUFFER));
	api_buffer->data = NULL;
	api_buffer->size = 0;
	api_buffer->buffer_state = 0;
	api_buffer->brackets = 0;

	slog_init_args(
			log_lvl,
			log_lvl,
			1, 1,
			NULL);//config_get_app_log_file());

	memset(blocking_msg_id,'\0',13);

#ifndef __ANDROID__ // On Android we manually call the exit handler.
	/* at exit / int close the socket to the core */
	atexit(atexit_cb);
#endif

	struct sigaction int_action;
	memset(&int_action, 0, sizeof(int_action));
	int_action.sa_handler = int_handler;
	sigaction(SIGINT, &int_action, NULL);

	rand_key = randstring(APP_KEY_SIZE);

	/* spawn the core mw and block connect to it */
#ifdef __ANDROID__ // On Android the core runs in a separate service.
	if (core_connection < 0) {
		//slog(SLOG_FATAL, "MW: Unable to connect to core.");
		exit(1);
	}
#else // __ANDROID__
	if (use_socketpair)
	{
		//int fds[2];
		init_com_wrapper();

#ifdef __linux__
		sockpair_module = com_module_new(
				"/usr/local/etc/middleware/com_modules/libcommodulesockpair.so",
				"{\"is_server\":1}");
#elif __APPLE__
		sockpair_module = com_module_new(
				"/usr/local/etc/middleware/com_modules/libcommodulesockpair.so",
				"{\"is_server\":1}");
#endif

		(*(sockpair_module->fc_set_on_data))((void (*)(void *, int, const void *, unsigned int))api_on_first_data);
		(*(sockpair_module->fc_set_on_connect))(api_on_connect);
		(*(sockpair_module->fc_set_on_disconnect))(api_on_disconnect);

		if (sockpair_module == NULL)
		{
			return NULL;
		}
		app_core_conn =(*(sockpair_module->fc_connect))(NULL);

		core_spawn_fd(app_core_conn, app_name);
	}
	else
	{
		app_core_conn = fifo_init_server(app_name);
		if (app_core_conn <= 0)
		{
			//slog(SLOG_FATAL, "MW: Unable to start fifo with the core.");
			exit(1);
		}
		fifo_run_receive_thread(app_core_conn);
		core_spawn_fifo(app_name);
	}
	char key_msg[20];
	sprintf(key_msg,"{%s}", rand_key);
	(*(sockpair_module->fc_send_data))(app_core_conn, key_msg);
#endif // __ANDROID__

	return app_name;
}

void mw_terminate_core()
{
	mw_call_module_function(
			"core", "terminate________", "voi",
			NULL);
	api_thread_destroy();
}

void mw_add_manifest(const char* manifest_str)
{
	MESSAGE *md_msg = message_new(manifest_str, MSG_CMD);
	char* md_str = message_to_str(md_msg);
	mw_call_module_function(
			"core", "add_manifest", "voi",
			md_str, NULL);

	free(md_str);
	json_free(md_msg->_msg_json);
	message_free(md_msg);
}

const char* mw_get_manifest()
{
	const char* manifest_str = (char*) mw_call_module_function_blocking(
			"core", "get_manifest", "str",
			NULL, NULL);

	return manifest_str;
}

int mw_call_module_function2(
		const char* module_id,
		const char* function_id_,
		const char* return_type,
		...)
{
	// pad function_id to 17 characters...
	char function_id[18] = {[0 ...sizeof(function_id)-2]='_', [sizeof(function_id)-1] = '\0'}; // Length 17
	strncpy(function_id, function_id_, strlen(function_id_) < strlen(function_id) ? strlen(function_id_) : strlen(function_id));

	va_list arguments;
	Array * argv = array_new(ELEM_TYPE_STR);

	va_start(arguments, return_type);

	const char *tmp=va_arg(arguments, const char*);
	while(tmp!=NULL){
		array_add(argv, (void*)tmp);  // Strings are NOT freed by array_free, so create duplicates here.
		tmp=va_arg(arguments, const char*);
	}

	va_end(arguments);

	JSON *msg_json = json_new(NULL);
	json_set_str(msg_json, "module_id", module_id);
	json_set_str(msg_json, "function_id", function_id);
	json_set_str(msg_json, "return_type", return_type);
	json_set_array(msg_json, "args", argv);

	MESSAGE* send_msg =  message_new_json(msg_json, MSG_CMD);
	char* send_str = message_to_str(send_msg);
	int result = (*(sockpair_module->fc_send_data))(app_core_conn, send_str);

	array_free(argv);
	json_free(msg_json);
	free(send_str);
	message_free(send_msg);

	return result;
}

const char delim1 = '{';
const char delim2 = '}';
//const char delim_a = ' ';
//const char delim_b = ',';
const char* delim21 = "}{";
const char* cmd = "15";
const char* arg_start = "\"a\":{";

char str[14];
int mw_call_module_function(
		const char* module_id,
		const char* function_id_,
		const char* return_type,
		...)
{

	// pad function_id to 17 characters...
	char function_id[18] = {[0 ...sizeof(function_id)-2]='_', [sizeof(function_id)-1] = '\0'}; // Length 17
	strncpy(function_id, function_id_, strlen(function_id_) < strlen(function_id) ? strlen(function_id_) : strlen(function_id));

	printf("Function ID: %s\n", function_id);

	char *msg_id = message_generate_id();
	(*(sockpair_module->fc_send))(app_core_conn, &delim1, 1);

	(*(sockpair_module->fc_send))(app_core_conn, &delim1, 1);
	(*(sockpair_module->fc_send))(app_core_conn, cmd, 2);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, module_id, 4);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, function_id, 17);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, return_type, 3);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, msg_id, 10);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);


	//(*(sockpair_module->fc_send))(app_core_conn, delim, 5);

	va_list arguments;
	va_start(arguments, return_type);
	const char *tmp=va_arg(arguments, const char*);
	while(tmp!=NULL){
		sprintf(str, "{%010lu}", strlen(tmp));
		(*(sockpair_module->fc_send))(app_core_conn, str, 12);
		(*(sockpair_module->fc_send))(app_core_conn, tmp, strlen(tmp));

		tmp=va_arg(arguments, const char*);
		//if(tmp)
		//	(*(sockpair_module->fc_send))(app_core_conn, &delim_b, 1);
	}

	sprintf(str, "{%010d}", 0);
	(*(sockpair_module->fc_send))(app_core_conn, str, 12);
	(*(sockpair_module->fc_send))(app_core_conn, &delim2, 1);
	(*(sockpair_module->fc_send))(app_core_conn, &delim2, 1);

	va_end(arguments);

	free(msg_id);
	return 0;
}

void* mw_call_module_function_blocking2(
		const char * module_id,
		const char * function_id_,
		const char * return_type,
		...)
{
	// pad function_id to 17 characters...
	char function_id[18] = {[0 ...sizeof(function_id)-2]='_', [sizeof(function_id)-1] = '\0'}; // Length 17
	strncpy(function_id, function_id_, strlen(function_id_) < strlen(function_id) ? strlen(function_id_) : strlen(function_id));

	va_list arguments;
	Array * argv = array_new(ELEM_TYPE_STR);

	va_start(arguments, return_type);

	const char *tmp=va_arg(arguments, const char*);
	while(tmp!=NULL){
		array_add(argv, (void*)tmp);  // Strings are freed by array_free, so create duplicates here.
		tmp=va_arg(arguments, const char*);
	}

	va_end(arguments);

	JSON *msg_json = json_new(NULL);
	json_set_str(msg_json, "module_id", strdup_null(module_id));
	json_set_str(msg_json, "function_id", strdup_null(function_id));
	json_set_str(msg_json, "return_type", strdup_null(return_type));
	json_set_array(msg_json, "args", argv);


	MESSAGE* send_msg =  message_new_json(msg_json, MSG_CMD);
	char* send_str = message_to_str(send_msg);
	(*(sockpair_module->fc_send_data))(app_core_conn, send_str);

	array_free(argv);
	json_free(msg_json);
	strcpy(blocking_msg_id, send_msg->msg_id);
	free(send_str);
	message_free(send_msg);

	waiting_blocking_call = 1;

	int count = 0;
	ioctl(fds_blocking_call[1], FIONBIO, &count);

	slog(SLOG_DEBUG, "FUNCTION ID SW: %s", function_id); //XAXA
	char* result = sync_wait(fds_blocking_call[1]);
	slog(SLOG_DEBUG, "RESULT: %s", result);

	waiting_blocking_call = 0;

	return result;
}

void* mw_call_module_function_blocking(
		const char* module_id,
		const char* function_id_,
		const char* return_type,
		...)
{
	// pad function_id to 17 characters...
	char function_id[18] = {[0 ...sizeof(function_id)-2]='_', [sizeof(function_id)-1] = '\0'}; // Length 17
	strncpy(function_id, function_id_, strlen(function_id_) < strlen(function_id) ? strlen(function_id_) : strlen(function_id));

	char *msg_id = message_generate_id();
	(*(sockpair_module->fc_send))(app_core_conn, &delim1, 1);

	(*(sockpair_module->fc_send))(app_core_conn, &delim1, 1);
	(*(sockpair_module->fc_send))(app_core_conn, cmd, 2);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, module_id, 4);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, function_id, 17);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, return_type, 3);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);
	(*(sockpair_module->fc_send))(app_core_conn, msg_id, 10);
	(*(sockpair_module->fc_send))(app_core_conn, delim21, 2);

	//(*(sockpair_module->fc_send))(app_core_conn, arg_start, 5);

	va_list arguments;
	va_start(arguments, return_type);
	const char *tmp=va_arg(arguments, const char*);
	while(tmp!=NULL){
		sprintf(str, "{%010lu}", strlen(tmp));
		(*(sockpair_module->fc_send))(app_core_conn, str, 12);
		(*(sockpair_module->fc_send))(app_core_conn, tmp, strlen(tmp));

		tmp=va_arg(arguments, const char*);
		//if(tmp)
		//	(*(sockpair_module->fc_send))(app_core_conn, &delim_b, 1);
	}

	sprintf(str, "{%010d}", 0);
	(*(sockpair_module->fc_send))(app_core_conn, str, 12);
	(*(sockpair_module->fc_send))(app_core_conn, &delim2, 1);
	(*(sockpair_module->fc_send))(app_core_conn, &delim2, 1);

	va_end(arguments);

	strcpy(blocking_msg_id, msg_id);
	waiting_blocking_call = 1;
	slog(SLOG_DEBUG, "XAXA: CALLING: %s", function_id);
	char* result = sync_wait(fds_blocking_call[1]);
	slog(SLOG_DEBUG, "XAXA: RESULT BLOCKING: %s", result);

	return result;
}

void mw_add_rdc(const char* module, const char* address)
{
	mw_call_module_function(
			"core", "add_rdc", "voi",
			module, address, NULL);
}

void mw_register_rdcs()
{
	slog(SLOG_DEBUG, "%s", __func__);
	mw_call_module_function(
			"core", "rdc_register", "voi",
			NULL);
}

void mw_tell_register_rdcs(const char* module, const char* address)  // TODO: maybe a better alternative to 'tell'.
{
	if (module == NULL)
		return;

	mw_call_module_function(
			"core", "rdc_register", "voi",
			address, NULL);
}

void mw_unregister_rdcs()
{
	mw_call_module_function(
			"core", "rdc_unregister", "voi",
			NULL);
}

void mw_tell_unregister_rdcs(const char* module, const char* address)
{
	if (address == NULL)
		return;

	mw_call_module_function(
			"core", "rdc_unregister", "voi",
			address, NULL);
}

int mw_load_com_module(const char* libpath, const char* cfgpath)
{
	char resolved_lib_path[PATH_MAX+1], resolved_cfg_path[PATH_MAX+1];
	char* abs_lib_path = realpath(libpath, resolved_lib_path);
	char* abs_cfg_path = realpath(cfgpath, resolved_cfg_path);

	int return_value = -1;

	if(abs_lib_path==NULL)
	{
		return -1;
	}
	if(abs_cfg_path==NULL)
	{
		return -1;
	}

	char* config_json = text_load_from_file(cfgpath);

	char* result = (char*) mw_call_module_function_blocking(
			"core", "load_com_module__", "int",
			abs_lib_path, config_json, NULL);

	if(result == NULL)
		return -1;

	sscanf(result, "%010d", &return_value);
	free(result);
	return return_value;
}

int mw_load_access_module(const char* libpath, const char* cfgpath)
{
	char resolved_lib_path[PATH_MAX+1], resolved_cfg_path[PATH_MAX+1];
	char* abs_lib_path = realpath(libpath, resolved_lib_path);
	char* abs_cfg_path = realpath(cfgpath, resolved_cfg_path);

	int return_value = -1;

	if(abs_lib_path==NULL)
	{
		return -1;
	}
	if(abs_cfg_path==NULL)
	{
		return -1;
	}

	char* config_json = text_load_from_file(cfgpath);

	char* result = (char*)mw_call_module_function_blocking(
			"core", "load_acc_module__", "int",
			abs_lib_path, config_json, NULL);

	if(result == NULL)
		return -1;

	sscanf(result, "%010d", &return_value);
	free(result);
	return return_value;
}

char* mw_get_remote_metdata(const char* module, int conn)
{
	if (module == NULL || conn == 0)
		return NULL;

	char conn_str[10];
	sprintf(conn_str, "%d", conn);
	char* resp = (char*) mw_call_module_function_blocking(
			"core", "get_remote_manif_", "str",
			module, conn_str, NULL);
	if(resp == NULL)
		return NULL;

	MESSAGE* resp_msg = message_parse(resp);
	JSON* resp_json = resp_msg->_msg_json;

	return json_get_str(resp_json, "return_value");
}

/* helper function implementation */

int core_spawn_addr(char *core_addr)
{
	rand_key = randstring(APP_KEY_SIZE);

	int core_pid = fork();
	if (core_pid == 0)/*child*/
	{
		chdir(BIN);

		char* args[] = {
				"core",
				core_addr,
				rand_key,
				NULL};
		const char* path = "core";
		execv(path, args);

		printf( "MW: Bad core middleware executable path.\n");
		exit(1);
	}
	else if(core_pid>0)/*parent*/
	{
		atexit(atexit_app);
		int count = 0;
		ioctl(fds_blocking_call[1], FIONBIO, &count);
		char* result = sync_wait(fds_blocking_call[1]);
	}
	else /* error */
	{   /*todo error handling*/
		exit(1);
	}

	return EXIT_SUCCESS;
}

int core_spawn_fd(int fds, char* app_name)
{

	int core_pid = fork();
	/* FIXME: argument must be a non null value to connect to the core */
	int core_fd = (*(sockpair_module->fc_connect))(sockpair_module->address);

	if (core_pid == 0)/*child*/
	{
		//close(fds[1]);
		chdir(BIN);


		char fd_str[11];
		snprintf(fd_str, 11, "%d", core_fd);

		char* args[] = {"core",
		//char* args[] = {"valgrind", "--leak-check=yes", "core",
				"-f", fd_str,
				"-a", app_name,
				"-k", rand_key,
				"-c", config_get_absolute_path(),
				NULL};
		const char* path = "core";
		//const char* path = "/usr/bin/valgrind";
		execv(path, args);

		printf("MW: Bad core middleware executable path.\n");
		exit(1);
	}
	else if(core_pid>0)/*parent*/
	{
		atexit(atexit_app);
		int count = 0;
		ioctl(fds_blocking_call[1], FIONBIO, &count);
		char* result = sync_wait(fds_blocking_call[1]);
		free(result);

		api_thread_create();
	}
	else /* error */
	{   /*todo error handling*/
		printf("MW: Error forking\n");
		exit(1);
	}

	return EXIT_SUCCESS;
}

int core_spawn_fifo(char* app_name)
{
	int core_pid = fork();

	if (core_pid == 0)/*child*/
	{
		chdir(BIN);

		char* args[] = {
				"core",
				"-f", "0",
				"-a", app_name,
				"-k", rand_key,
				NULL};
		const char* path = "core";
		execv(path, args);

		exit(1);
	}
	else if(core_pid>0)/*parent*/
	{
		int count = 0;
		ioctl(fds_blocking_call[1], FIONBIO, &count);
		char* result = sync_wait(fds_blocking_call[1]);
	}
	else /* error */
	{   /*todo error handling*/
		exit(1);
	}

	return EXIT_SUCCESS;
}

void api_on_first_data(COM_MODULE* module, int conn, const void* msg, unsigned int size)
{
	(*(sockpair_module->fc_set_on_data))((void (*)(void *, int, const void *, unsigned int))api_on_data);
	waiting_blocking_call = 0;
	sync_trigger(fds_blocking_call[0], "{}");
}

void* api_on_message(void* data)
{
	const char * msg = (const char*) data;
	//printf("***** received: %s\n\n", msg);

	char cmd = msg[1];
	char msg_id[11], ep_id[11];

	int size;
	char* msg_data;

	if(cmd == 'a')/* from core */
	{
		strncpy(ep_id, data+3, 10);
		ep_id[10]='\0';

		sscanf(data+14, "{%010d}", &size);
		msg_data = (char*) malloc((size+1)*sizeof(char));
		strncpy(msg_data, data+27, size);
		msg_data[size]='\0';

		slog(SLOG_DEBUG, "BROKEN MSG?: %s", msg);
		slog(SLOG_DEBUG, "CORE API ON MSG: %s", msg_data);
		//slog(SLOG_ERROR, "EP HANDLER: %s", msg_data);
		//(*(ep->handler))(msg);
		//TODO: message problems...
		api_thread_push(msg_data);

	}
	else if(cmd == 'b') /* external */
	{
		strncpy(msg_id, data+3, 10);
		msg_id[10]='\0';

		sscanf(data+19, "{%010d}", &size);
		msg_data = (char*) malloc((size+1)*sizeof(char));
		strncpy(msg_data, data+32, size);
		msg_data[size]='\0';

		slog(SLOG_DEBUG, "EXT API ON MSG: %s", msg_data);
	}





	//printf("***** size: %d; msg: %s\n", size, msg_data);
	//MESSAGE *msg_ = message_parse(msg);

	//printf("**** >>> %s, %s :: %d, \n", msg_id, blocking_msg_id, waiting_blocking_call);

	if (waiting_blocking_call == 1)
	{
		if (strcmp(blocking_msg_id, msg_id) == 0)
		{
			waiting_blocking_call = 0;
			sync_trigger(fds_blocking_call[0], msg_data);
		}
	}
/*
	if (msg_->ep==NULL)
	{
		goto final;
	}
	else if(msg_->status == MSG_STREAM_CMD && msg_->ep->type == EP_STR_SNK)
	{
		//printf("\n\nstream start ..... %s \n\n", message_to_str(msg_));//was str

		(*msg_->ep->handler)(msg_);
		goto final;
	}
	else if (msg_->ep->handler)
	{
		(*msg_->ep->handler)(msg_);
		goto final;
	}
	*/

	free(msg_data);
		//json_free(msg_->_msg_json);
		//message_free(msg_data);

	return NULL;
}

void api_on_connect(void* module, int conn)
{
	slog(SLOG_INFO, "MW %s: Core successfully connected.", __func__);
}

void api_on_disconnect(void* module, int conn)
{
	slog(SLOG_ERROR, "MW %s: Core disconnected. Closing.", __func__);
	shutdown(app_core_conn,1);
	exit(0);
}

void api_on_data(COM_MODULE* module, int conn, const void* data, unsigned int size)
{
	buffer_update(api_buffer, data, size);
}


#define BUFFER_FINAL 	0
#define BUFFER_JSON  	1
#define BUFFER_STR_2 	2
#define BUFFER_ESC_2 	3
#define BUFFER_STR_1 	4
#define BUFFER_ESC_1 	5

void buffer_set(BUFFER* buffer, const void* new_buf, unsigned int new_start, unsigned int new_end)
{
	char* old_buf = buffer->data;
	int new_size = new_end-new_start;
	buffer->data = (char*) malloc(buffer->size + new_size + 1);
	memcpy(buffer->data,             old_buf,           buffer->size);
	memcpy(buffer->data+buffer->size, new_buf+new_start, new_size   );
	buffer->size = buffer->size + new_size;
	buffer->data[buffer->size] = '\0';

	//printf("----buffer set %d %d %d: %s\n\n", new_start, new_end, buffer->size, buffer->data);
	free(old_buf);
}

void buffer_reset(BUFFER* buffer)
{
	if(buffer->data)
		free(buffer->data);

	buffer->data = (char*)(malloc(1));
	buffer->data[0]='\0';
}

void buffer_update(BUFFER* buffer, const void* new_data, unsigned int new_size)
{
	unsigned int i=0;
	unsigned int word_start = i;
	unsigned int word_end = i;
	for(i=0; i<new_size; i++)
	{
		switch(buffer->buffer_state){
			case BUFFER_FINAL:
				switch (((char*)new_data)[i])
				{
					case '{':
						buffer->brackets += 1;
						word_start = i;
						buffer->buffer_state = BUFFER_JSON;
						break;
					/* ignore spaces and new lines */
					case ' ': case '\n': case '\r':
						break;
					default:
						//slog(SLOG_ERROR, "%c", new_data[i]);
						continue;

				}
				break;

			case BUFFER_JSON:
				switch (((char*)new_data)[i])
				{
					case '{':
						buffer->brackets += 1;
						//data->buffer_state = 1;
						break;
					case '}':
						buffer->brackets -= 1;

						if(buffer->brackets == 0) // finished 1 word
					 	{
							buffer->buffer_state = BUFFER_FINAL;
							word_end = i+1;
							buffer_set(buffer, new_data, word_start, word_end);
							/* apply the callback for this connection */


							/*char* new_data = strdup(buffer->data);
							pthread_t cbthread;
							int err;
							err = pthread_create(&cbthread, NULL, &api_on_message, new_data);
							if(err != 0) {
								slog(SLOG_ERROR, "CONN buffer_update: can't create receive thread");
								goto skip;
							}
							err = pthread_detach(cbthread);
							if(err != 0) {
								slog(SLOG_ERROR, "CONN buffer_update: Could not detach cb thread");
								goto skip;
							}
							skip:*/

							api_on_message(buffer->data);

							buffer_reset(buffer);

							buffer->size = 0;
							word_start = i+1;
						}

						break;
					case '\"':
						buffer->buffer_state = BUFFER_STR_2;
						break;
					case '\'':
						buffer->buffer_state = BUFFER_STR_1;
						break;

					default:
						continue;
				}
				break;
			case BUFFER_STR_1:
				switch (((char*)new_data)[i])
				{
					case '\\':
						buffer->buffer_state = BUFFER_ESC_1;
						break;
					case '\'':
						buffer->buffer_state = BUFFER_JSON;
						break;
					default:
						continue;
				}
				break;
			case BUFFER_ESC_1:
				buffer->buffer_state = BUFFER_STR_1;
				break;
			case BUFFER_STR_2:
				switch (((char*)new_data)[i])
				{
					case '\\':
						buffer->buffer_state = BUFFER_ESC_2;
						break;
					case '\"':
						buffer->buffer_state = BUFFER_JSON;
						break;
					default:
						continue;
				}
				break;
			case BUFFER_ESC_2:
				buffer->buffer_state = BUFFER_STR_2;
				break;
			default: //impossible
				return;
		}
	}

	if(word_start<new_size && buffer->buffer_state != BUFFER_FINAL)
		buffer_set(buffer, new_data, word_start, new_size);
}

void api_thread_create() {
	api_msg_queue_active = true;
	api_msg_queue = StsQueue.create();

	int err;
	err = pthread_create(&api_msg_thread, NULL, &api_thread_func, NULL);
	if(err != 0) {
		slog(SLOG_ERROR, "API THREAD: api_thread_create: can't create thread");
		return;
	}
	/*err = pthread_detach(api_msg_thread);
	if(err != 0) {
		slog(SLOG_ERROR, "API THREAD: api_thread_create: can't detach thread");
		return;
		}*/
}

void api_thread_destroy() {
	api_msg_queue_active = false;
	pthread_join(api_msg_thread, NULL);
	api_msg_t* ret;
	while((ret = StsQueue.pop(api_msg_queue)) != NULL) {
		free(ret->msg_str);
		free(ret);
	}
	StsQueue.destroy(api_msg_queue);
	printf("destroy!");
}

void* api_thread_func(void* _blank) {
	while(api_msg_queue_active) {
		api_msg_t* ret = StsQueue.pop(api_msg_queue);
		if(ret == NULL)
			continue;

		MESSAGE* msg = message_parse(ret->msg_str);
		ENDPOINT* ep = msg->ep;

		(*ep->handler)(msg);

		message_free(msg);
		free(ret->msg_str);

		free(ret);
	}
}

void api_thread_push(const char* msg_str) {
	api_msg_t* api_msg = (api_msg_t*)malloc(sizeof(api_msg_t));
	api_msg->msg_str = strdup(msg_str);
	StsQueue.push(api_msg_queue, api_msg);
}
