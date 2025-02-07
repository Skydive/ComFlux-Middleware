/*
 * on_data.c
 *
 *  Created on: 20 Mar 2017
 *      Author: Raluca Diaconu
 */

#include "core_callbacks.h"

#include <json.h>
#include <json_builds.h>
#include <core_module_api.h>
#include <core_module.h>
#include <protocol.h>
#include <utils.h>
#include <sync.h>

#include <conn_fifo.h>

#include <string.h>
#include <unistd.h> // for write

/* to check*/
#include "manifest.h"

extern STATE* app_state;

/* callbacks for the com modules */
void core_on_data(COM_MODULE* module, int conn, const void* data, unsigned int size)
{
	/*slog(SLOG_INFO, "CORE:core_on_data\n"
			"\tfrom: (%s:%d)\n"
			"\tdata: *%s*", module->name, conn, data);*/
	STATE* state_ptr = states_get(module, conn);
	buffer_update(state_ptr->buffer, data, size);
}

extern int map_sync_pipe[2];
void core_on_connect(COM_MODULE* module, int conn)
{
	/*
	 * these cases should not occur as long as the core connects to the app with sockpair
	 */
	if (app_state == NULL)
	{
		core_terminate();
		//slog(SLOG_FATAL, "CORE:core_on_connect: Component not connected.");
		exit(EXIT_FAILURE);
	}
	if (module == app_state->module && conn == app_state->conn)
	{
		core_terminate();
		//slog(SLOG_FATAL, "CORE:core_on_connect: Something went wrong.");
		exit(EXIT_FAILURE);
	}

	/*
	 * This is a new outside connection.
	 * If it connects to a bridge (transport layer) module
	 * 		then expect another component and perform the protocol
	 * Otherwise, themodule connect to outside, e.g. REST
	 *		go to the final mapped state.
	 */
	STATE* state_ptr = state_new(module, conn, STATE_HELLO_S);
	states_set(module, conn, state_ptr);

	if( (*module->fc_is_bridge)() )
	{
		state_ptr->on_message = &core_on_proto_message;

		JSON* manifest = manifest_get(MANIFEST_SIMPLE);
		json_set_str(manifest, "com_module", module->name);
		json_set_str(manifest, "address", module->address);

		JSON *hello_json = json_build_hello(manifest);
		state_send_json(state_ptr, NULL, hello_json, MSG_HELLO);
		json_free(hello_json);
		json_free(manifest);
	}
	else /* jump over the proto */
	{
		state_ptr->on_message = &core_on_message;

		state_ptr->state = STATE_MAP; //wating for map
		sync_trigger(map_sync_pipe[0], "mapped");
		sync_trigger(map_sync_pipe[0], "mapped");
	}
}

void core_on_disconnect(COM_MODULE* module, int conn)
{
	// If the app connection closed without terminating the core, something bad has happened.
	STATE *state_ptr = states_get(module, conn);

	if(state_ptr == NULL)
	{
		//slog(SLOG_ERROR, "CORE:core_on_disconnect: invalid null state");
		return;
	}

	if (state_ptr == app_state)
	{
		core_terminate();

		exit(EXIT_FAILURE);
	}

	ep_unmap_final(state_ptr->lep, state_ptr);

	if(state_ptr->access_module)
		(*(state_ptr->access_module->fc_disconnect))(state_ptr);

	state_free(state_ptr);


	(*module->fc_connection_close)(conn);
}



/* callbacks for the state / message stuff */

/*
 * Handles messages related to hello and auth handshake.
 * this handler is assigned to states of transport layer modules
 * -- those for which is_bridge returns 1.
 */
void core_on_proto_message(STATE* state_ptr, MESSAGE* _msg)
{
	if(state_ptr == NULL)
	{
		return;
	}
	if(_msg == NULL)
	{
		return;
	}

	COM_MODULE* module = state_ptr->module;
	int conn = state_ptr->conn;


	/* check message type and connection state */
	switch(_msg->status)
	{
	/* protocol messages */
	case MSG_HELLO:
		if(state_ptr->state == STATE_HELLO_S || state_ptr->state == STATE_HELLO_2)
			core_proto_hello(state_ptr, _msg);
		break;
	case MSG_HELLO_ACK:
		if(state_ptr->state == STATE_HELLO_S || state_ptr->state == STATE_HELLO_ACK_S)
			core_proto_hello_ack(state_ptr, _msg);
		break;
	case MSG_AUTH:
		if(state_ptr->state == STATE_AUTH || state_ptr->state == STATE_AUTH_2)
			core_proto_check_auth(state_ptr, _msg);
		if(state_ptr->am_auth && state_ptr->is_auth)
		{
			//slog(SLOG_INFO, "CORE:change callback 1\n");
			state_ptr->on_message = &core_on_message;
		}
		break;
	case MSG_AUTH_ACK:
		if(state_ptr->state == STATE_AUTH || state_ptr->state == STATE_AUTH_ACK)
			core_proto_check_auth_ack(state_ptr, _msg);
		if(state_ptr->am_auth && state_ptr->is_auth)
		{
			//slog(SLOG_INFO, "CORE:change callback 2\n");
			state_ptr->on_message = &core_on_message;
		}
		break;

	default:
		/* bad message status */
		;
	}

	//message_free(_msg);
}

/*
 * Handles messages between remote and local endpoints.
 */
void core_on_message(STATE* state_ptr, MESSAGE* _msg)
{
	if(state_ptr == NULL)
	{
		return;
	}
	if(_msg == NULL)
	{
		return;
	}

	COM_MODULE* module = state_ptr->module;
	int conn = state_ptr->conn;


	/* check message type and connection state */
	switch(_msg->status)
	{
	/* map messages */
	case MSG_MAP:
		if(state_ptr->state == STATE_MAP)
			core_proto_map(state_ptr, _msg);
		break;
	case MSG_MAP_ACK:
		if(state_ptr->state == STATE_MAP_ACK)
			core_proto_map_ack(state_ptr, _msg);
		break;
	case MSG_UNMAP:
		if(state_ptr->is_mapped)
			ep_unmap_recv(state_ptr->lep, state_ptr);
		break;
	case MSG_UNMAP_ACK:
		ep_unmap_final(state_ptr->lep, state_ptr);
		break;

	/* endpoint messages */
	case MSG_MSG:
	case MSG_REQ:
	case MSG_RESP_NEXT:
	case MSG_RESP_LAST:
		if(state_ptr->state == STATE_EXT_MSG)
		{
			//char* data = message_to_str(_msg);
			call_external_command_handler(state_ptr, _msg);
		}
		break;
	case MSG_STREAM:
		_msg->ep = state_ptr->lep->ep;
		recv_stream_msg(_msg);
		break;
	case MSG_STREAM_CMD:
		_msg->ep = state_ptr->lep->ep;
		recv_stream_cmd(_msg);
		break;

	case MSG_NONE:
	default:
		/* bad message status */
		;
	}
	//message_free(_msg);
}
//{a{MiFjpFCUJ7}{0000000184}{{ "status": 9, "msg_json": { "value": "41.24'12.2\"N 2.10'26.5\"E", "date": "2012-04-23T18:25:43.511Z" }, "ep_id": "MiFjpFCUJ7", "msg_id": "0000000008", "conn": 8, "module": "comtcp" }}}
/* core_on_component_message handles messages from the component.
 * This handler is assigned only to the app_state */

const char delim1 = '{';
const char delim2 = '}';
const char* delim21 = "}{";
const char a = 'a', b = 'b';

void core_on_component_message(STATE* state_ptr, const char* msg_id,
		const char* module_id, const char* function_id, const char* return_type,
		Array *args)
{
	char *return_msg = _core_call_array(module_id, function_id, return_type, args);

	/* get the result back to the component*/
	if(return_msg != NULL)
	{
		char str[11];
		sprintf(str, "%010lu", strlen(return_msg));

		COM_MODULE* sockpair_module = app_state->module;
		(*(sockpair_module->fc_send))(app_state->conn, &delim1, 1);

		(*(sockpair_module->fc_send))(app_state->conn, &b, 1);
		(*(sockpair_module->fc_send))(app_state->conn, &delim1, 1);
		(*(sockpair_module->fc_send))(app_state->conn, msg_id, 10);
		(*(sockpair_module->fc_send))(app_state->conn, delim21, 2);
		(*(sockpair_module->fc_send))(app_state->conn, return_type, 3);
		(*(sockpair_module->fc_send))(app_state->conn, delim21, 2);
		(*(sockpair_module->fc_send))(app_state->conn, str, 10);
		(*(sockpair_module->fc_send))(app_state->conn, delim21, 2);
		(*(sockpair_module->fc_send))(app_state->conn, return_msg, strlen(return_msg));

		(*(sockpair_module->fc_send))(app_state->conn, &delim2, 1);
		(*(sockpair_module->fc_send))(app_state->conn, &delim2, 1);

		free(return_msg);

		//printf("result: %s\n", message_to_str(return_msg));
		//return_msg->msg_id = (char*) malloc(11 * sizeof(char));
		//strncpy(return_msg->msg_id, msg_id, 10);
		//state_send_message(app_state, return_msg);

		//json_free(return_msg->_msg_json);
		//message_free(return_msg);
	}
}

/* This function is called only for the app to register to the core */
void core_on_first_message(STATE* state_ptr, MESSAGE* msg)
{
	if(state_ptr != app_state)
	{
		exit(EXIT_FAILURE);
	}
	if(state_ptr->state != STATE_FIRST_MSG)
	{
		return;
	}

	/*if (!strcmp(data, app_key))
	{
		slog(SLOG_ERROR, "CORE: core_on_first_message: invalid key; ignoring");
		return;
	}*/

	/* no errors */
	state_ptr->state = STATE_APP_MSG;
	state_ptr->on_message = &core_on_component_message;
}



/* does just the hello + mapping for now */
void call_external_command_handler(STATE* state_ptr, MESSAGE* msg)
{
	/*
	 * it is called from the core so the following should not arise:
	 * if(state_ptr== NULL)
	 * if (msg == NULL)
	 * if(state_ptr->lep == NULL)
	 * if(state_ptr->lep->ep == NULL) // never!!!
	 */

	if(msg->status == MSG_UNMAP)
	{
		ep_unmap_recv(state_ptr->lep, state_ptr); //TODO:state
		(*(state_ptr->module->fc_connection_close))(state_ptr->conn);
		return;
	}
	else if (state_ptr->lep->ep->type == EP_STR_SNK &&
			state_ptr->lep->fifo != 0)
		    //(msg->status == MSG_STREAM || sta)
	{
		fifo_send_message(state_ptr->lep->fifo, msg->msg_id); //was str
		return;
	}

	/*MESSAGE *in_msg = message_parse_json(msg->_msg_json);
	if(in_msg == NULL)
	{

		//message_free(msg);
		return;
	}*/
	msg->ep = state_ptr->lep->ep;
	msg->conn = state_ptr->conn;
	msg->module = strdup_null(state_ptr->module->name);

	/* apply the handler of the ep for incoming messages */
	if(state_ptr->lep->ep->handler != NULL)
	{
		(*state_ptr->lep->ep->handler)(msg);
	}
	else/* do nothing*/
	{	}
}

void recv_stream_cmd(MESSAGE* msg)
{
	if (msg->status != MSG_STREAM_CMD)
		return;

	if (msg->ep == NULL)
		return;

	if (msg->ep->type != EP_STR_SRC && msg->ep->type != EP_STR_SNK )
		return;



	LOCAL_EP* lep = (LOCAL_EP*)msg->ep->data;

	JSON* msg_json = msg->_msg_json;
	int command = json_get_int(msg_json, "command");
	if(command == 1 && lep->fifo <= 0)
	{
		sprintf(lep->fifo_name, "/tmp/%s", randstring(5));
		lep->fifo = fifo_init_server(lep->fifo_name);

		msg->msg_id = strdup_null(lep->fifo_name); //was str
		state_send_message(app_state, msg);
		return;
	}
	if(command == 0 && lep->fifo != 0)
	{
		fifo_close(lep->fifo);
		//unlink(lep->fifo_name);
	}
}
int total_stream = 0;
void recv_stream_msg(MESSAGE* msg)
{
	if (msg->status != MSG_STREAM)
		return;

	if (msg->ep == NULL)
		return;

	if (msg->ep->type != EP_STR_SNK )
		return;

	LOCAL_EP* lep = (LOCAL_EP*)msg->ep->data;
	char* data = json_get_str(msg->_msg_json, "stream");
	int data_size = strlen(data);
	//fifo_send_message(lep->fifo, data);//was str
	int tot = 0;
	do{
			tot += write(lep->fifo, data+tot, 500);
	}while(tot<data_size);
	free(data);
}


void send_error_message(STATE* state_ptr, char *msg)
{
	//slog(SLOG_ERROR, "CORE: send error %s to (%s:%d)", msg, state_ptr->module->name, state_ptr->conn);
	//conn_send_message(conn, msg);
}
