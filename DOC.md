# ComFlux Documentation #

Associated [GitHub repository](https://github.com/Skydive/ComFlux-Middleware)


## Introduction ##
It is highly suggested that this guide is read, especially before going into the internals of the ComFlux middleware. It should provide a brief overview, and save weeks of time figuring out how the codebase works.


## Relevant repositories ##

Refer to the original repository [Original repository](https://github.com/ComFlux/ComFlux-Middleware)
Refer to the testing repository [Testing repository](https://github.com/Skydive/ComFlux-Testing)

From the testing repository: (bin/)
```
./mwwrap improved_source.out 1600
./mwwrap improved_sink.out 1601 127.0.0.1:1600
```


## High level overview ##
### Description ###
ComFlux is an architecture for decentalised communication between embedded devices. This middleware is an implementation of this framework.
Each program/executable that implements ComFlux can be thought of as a component. Components have distinct endpoints, and each endpoint may have a different name or type.

### Endpoints ###
Endpoint Types:
- `src`	source
- `snk` sink
- `req` request
- `res` response

Crucially, sink endpoints may only be mapped (connected) to source endpoints, and response endpoints may only be mapped to request endpoints.

A simple example of a source-sink interaction is as follows:
```
+----------------+	+----------------+
|  Thermometer 1 |	|      Screen    |
+----------------+	+----------------+
| EP: Temp (src) |	| EP: Temp (snk) |
+----------------+	+----------------+
	^         	         ^
	+------------------------+

```
The thermometer component contains the code to output the measured temperature, from some probe, to the source endpoint called 'Temp'.
The screen component contains only the code to capture the received temperature at the sink endpoint 'Temp', and display it on a screen.
Crucially, the thermometer and screen may be connected together at runtime, the screen may be disconnected from the thermometer, and mapped to another temperature probe without the need to restart the programs.

This introduces the further layer of complexity, command and control. Components themselves can map to, and in the process reconfigure other components. Default endpoints exist for this purpose. (Refer to default_eps.c)

### Default Endpoints ###

```
+---------------+-----------------+
|               |  EP: MAP (res)  |
| Any component	+-----------------+
|               |  EP: UNMAP (res)|
+---------------+-----------------+
| Other EP      |  EP: TERMINATE  |
+---------------+-----------------+
```

Default EPs include:
- MAP
- UNMAP
- LOOKUP
- MAP LOOKUP
- ADD_RDC
- REG_RDC
- TERMINATE 

The existence of default endpoints such as these, that exist on all components, allows a variety of decentalised behaviour to be programmed.

Example:
```
 T1
 T2<--------->Screen
 T3             ^
                |
 GPS<-----------+

```
The screen may contain code to read the recieved GPS position, and based on location, UNMAP from T2, and map to T3.




### Resource Discovery ###
The existence of decentralised behaviour over a network leads to a series of problems regarding the distribution of information, notably that every component must know the addresses of all of the other components - this isn't ideal.
In practice, we introduce the resource discovery component.

All components map to and register with the RDC, sending the RDC associated metadata at fixed intervals of time, this includes the name of the component, the endpoints of the component, but also any other relevant information that needs to be networked to all other components.

As such, all components only need the network address of the RDC, and may fetch a table of network addresses of all the other components.

The lookup endpoint, instructs a component to connect to its associated RDC and query it for the network addresses that match the associated metadata filter, and return their network addresses, after which a map to the component may be performed. As such, mapping directly by network addresses is not necessary, as complex behaviour can indeed emerge without hardcoding all of these addresses in.


## Very brief walkthrough of internal behaviour ##
Consider /examples/simple_source.c:


### mw_init(...) ###
```
	char* app_name = mw_init(
			"source_cpt", /* name of the app */
			config_get_app_log_lvl(), /* log level for app */
			1); /* use sockpair, only available option atm */
	printf("Initialising core: %s\n", app_name!=NULL?"ok":"error");
	printf("\tApp name: %s\n", app_name);

	/* load coms modules for the core */
	load_cfg_result = config_load_com_libs();
	printf("Load coms module result: %s\n", load_cfg_result==0?"ok":"error");
```


In order to launch ComFlux, your program must first call the function `mw_init(...)`,
this forks your program, and creates a subprocess called `core`. (refer to core_spawn_fd(...) in /src/api/middleware.c)



The ComFlux headers that are part of your program are responsible for, primarily, sending and receiving ICP (interprocess communication) messages from the core subprocess.

config_load_com_libs() is responsible for telling the app to message the core, instructing the core to start the TCP communications module.


### endpoint_new_src_file ###
```
	ENDPOINT *ep_src = endpoint_new_src_file(
			"ep_source",
			"example src endpoint",
			"example_schemata/datetime_value.json");
```
Creates a new endpoint, and inserts it into a global variable called 'endpoints' which is a HashMap, with the endpoint id (some randomly generated string) as the key.
After adding to the HashMap, a function called endpoint_register is called, which communicates with the core, registering the new endpoint, and its associated ID in the core.


### endpoint_send_message(...) ###

```
		msg_json = json_new(NULL);
		json_set_int(msg_json, "value", rand() % 10);
		json_set_str(msg_json, "datetime", asctime(timeinfo));

		message = json_to_str(msg_json);
		printf("Sending message: %s\n", message);
		endpoint_send_message(ep_src, message);

        free(message);
        json_free(msg_json);
```
This sends a message to core, instructing the core to call core_ep_send_message(...) in core_module.c, with the message and appropriate endpoint specified.
Core will then call a function called state_send_message, which will send the message over the network via TCP to the remote endpoint.


### ep_get_all_connections(...) ###
``` 
        connections = ep_get_all_connections(ep_src);
        printf("Number of connections = %d\n", array_size(connections));
        for (i=0; i<array_size(connections); i++) {
		...
		}
		array_free(connections);
```
ep_get_all_connections is a blocking call.
mw_call_module_function_blocking(...) is called, sending a message to core to return a string of active connections of the endpoint 'ep_src', and network it back to the app.
This thread STALLS, as afterwards it calls sync_wait(...), waiting for a message to come into the fds_blocking_call socket.

A background thread with function name sockpair_receive_function(...) in the app, reads all messages sent from the core to app.
When a full message is received, api_on_message(...) is called. If the message is that of a return value of a function call from core, it is sent to the fds_blocking_call socket to be received by whatever thread sync_wait() is running in.

The main thread, still in ep_get_all_connections, captures the return value sent to sync_wait, and then returns it.


This WAS broken in the past - refer to 'Fixed blocking calls' under 'Bugfixes implemented'


## Explanation of internal behaviour ##

### Diagram ###

The structure of the program:
```
		APP <-- SOCKPAIR MODULE --> CORE <-- TCP COMM MODULE---> NETWORK
```

### Core ###
Core is created in core_spawn_fd(), by a fork(), then execv().
Arguments to the core are passed, which allow it to connect to the file descriptor of the socketpair created in the app.

### COM Modules ###
Refer to load_all_com_functions(...) in com_wrapper.c
com_wrapper.c -> com_module_new calls dlopen on the appropriate com module shared object library.
It then passes the handle into load_all_com_functions, assigning fc_function_name to dlsym(handle, "com_function_name") for all relevant function names.

Most importantly, there is a sockpair communications module, and a tcp communications module.
Sockpair is for bidirectional interprocess communication between the app and core of a single component, while the tcp module is for networking processes in the core.


### Background Threads ###
Since ComFlux is written in C, all socket behaviour MUST be implemented using the associated linux kernel functions: send and recv.

### Thread blocking ###
The global variable fds, represents the bidirectional sockets for communicating between app and core, but this occurs in other parts of the code too, most notably in the application during thread blocking.

The global variable fds_blocking_call, represents the sockets for which data is exchanged between seperate threads.


## Explanation of internal objects ##

### Buffer ###
A buffer struct and functions exist to reallocate the memory and copy over a string of a partially sent message, fully constructing the message from the individual chunks sent over a socket.

buffer_update exists in state.c within the core, and in api/middleware.c within the app. In either case the function does the same thing in principle, constructing a full message from the individual pieces until a full JSON block has been received. This full block is then processed differently in the case of the message being received in the core, or the app, as dictated by the respective buffer_update functions in each file.

A thread that listens to and receieves data from any socket, tcp_recieve_function, or sockpair_recieve_function, will call a function pointer to core_on_data, or api_on_data respectively. These functions all 

### State ###
The state struct and functions exist to manage and control the networking of a single endpoint from within the core. Each distinct endpoint is associated with its own state struct. The state struct represents the state of the tcp connection.

Thus, state_send_message can take a state pointer of an endpoint, and a message and send the appropriate message to the appropriate endpoint.



## Bugfixes implemented ##
Refer to GitHub commit history:

- Proper SIGINT handling 
- Double wrapped messages 
- Function name padding 
- Fixed blocking calls & sync_wait
- Single IPC socket race condition 


## Remaining problems ##

### RDC ###
RDC is not fully memory safe, expect memory leaks, double free or corruption errors, and segmentation faults.

### UNMAP ALL ###
Unmapping multiple sinks from the same source at the same time results in undefined behaviour, with unallocated memory pointed to by the state pointers in the mapping_states table. Refer to the branch: unmapfix.

