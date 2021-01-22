/*****************************************************************************
 * mqtt.c : mqtt module for vlc
 *****************************************************************************
 * Copyright (C) 2021- the Tifaifai team
 * $Id$
 *
 * Authors: Tifaifai Maupiti <tifaifi.maupiti@gmail.com>
 *          Florent Carlier <florent.carlier@univ-lemans.fr>
 * inspirated by Documentation:Modules/mqtt on
 *                         https://wiki.videolan.org/Documentation:Modules/mqtt
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <mosquitto.h>
#include <mqtt_protocol.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* VLC core API headers */
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_actions.h>

/* Local protypes. */
struct mosquitto *mosq;
int rc;
intf_thread_t *p_intf;

static char alphanum[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static int run = 1;

static int Open (vlc_object_t *);
static void Close (vlc_object_t *);
static void *Run( void * );
static int  ActionEvent( vlc_object_t *, char const *,
                         vlc_value_t, vlc_value_t, void * );

static void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
static void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos);
static void on_publish(struct mosquitto *mosq, void *obj, int mid);
static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg);

/* Module descriptor */
#define N_(str) (str) // Remove international for compilation with outtree

#define HOST_TEXT N_("MQTT Host")
#define HOST_LONGTEXT N_("Hostname of MQTT broker to connect to default value: localhost")
#define HOST_DEFAULT "127.0.0.1"

#define PORT_TEXT N_("MQTT Port")
#define PORT_LONGTEXT N_("Port number of MQTT broker to connect to default value: 1883")

#define USERNAME_TEXT N_("MQTT Username")
#define USERNAME_LONGTEXT N_("The username to connect to the broker with default value: none")
#define USERNAME_DEFAULT ""

#define PASSWORD_TEXT N_("MQTT Password")
#define PASSWORD_LONGTEXT N_("The password to connect to the broker with default value: none")
#define PASSWORD_DEFAULT ""

#define PREFIX_TEXT N_("MQTT Prefix")
#define PREFIX_LONGTEXT N_("The topic name prefix to use default value: vlc/")
#define PREFIX_DEFAULT "vlc/"

#define KEEPALIVE_TEXT N_("MQTT Keepalive")
#define KEEPALIVE_LONGTEXT N_("The keep alive time for the MQTT protocol (in seconds) default value: 10")

#define CLIENTID_TEXT N_("MQTT Clientid")
#define CLIENTID_LONGTEXT N_("The client identifier to connect to the broker as default value: random")
#define CLIENTID_DEFAULT "random"

#define QOS_TEXT N_("MQTT QoS")
#define QOS_LONGTEXT N_("The QoS level to publish and subscribe usinf (0, 1, 2) default value: 1")

#define PUB_STATUS_TEXT N_("MQTT Active publishing status")
#define PUB_STATUS_LONGTEXT N_("The status notification is activating by this option to publish status on MQTT topic /status/")

#define CFG_PREFIX "mqtt-"

vlc_module_begin ()
    set_shortname( N_("MQTT HotKeys") )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )

      set_section( N_("MQTT Broker"), 0)
      add_string( CFG_PREFIX "host", HOST_DEFAULT , HOST_TEXT, HOST_LONGTEXT, false)
      add_integer( CFG_PREFIX "port", 1884, PORT_TEXT, PORT_LONGTEXT, true)
        change_integer_range (1, 65534)
        change_safe ()
      set_section( N_("MQTT Subscribe : /command/"), 0)
      add_string( CFG_PREFIX "clientid", CLIENTID_DEFAULT, CLIENTID_TEXT, CLIENTID_LONGTEXT, false)
      add_string( CFG_PREFIX "username", USERNAME_DEFAULT, USERNAME_TEXT, USERNAME_LONGTEXT, false)
      add_password( CFG_PREFIX "password", PASSWORD_DEFAULT, PASSWORD_TEXT, PASSWORD_LONGTEXT, false)
      add_string( CFG_PREFIX "prefix", PREFIX_DEFAULT, PREFIX_TEXT, PREFIX_LONGTEXT, false)
      add_integer( CFG_PREFIX "keepalive", 10, KEEPALIVE_TEXT, KEEPALIVE_LONGTEXT, false)
      add_integer( CFG_PREFIX "qos", 1, QOS_TEXT, QOS_LONGTEXT, false)
        change_integer_range (0, 2)
        change_safe ()
      set_section( N_("MQTT Publish : /status/"), 0)
      add_bool( CFG_PREFIX "pub_status", false, PUB_STATUS_TEXT, PUB_STATUS_LONGTEXT, true )

    set_description( N_("MQTT control interface") )
    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
vlc_module_end ()

/* Internal state for an instance of the module */
struct intf_sys_t
{
  vlc_thread_t       thread;
  input_thread_t     *p_input;

  char *host;
  int port;

  char *clientid;
  char *username;
  char *password;
  char *prefix;
  int keepalive;
  int qos;

  bool pub_status;

  struct mosquitto *mosq;
};

/*****************************************************************************
 * Open: initialize interface
 *****************************************************************************/
static int Open(vlc_object_t *obj)
{
    /* Allocate internal state */
    p_intf = (intf_thread_t *)obj;
    intf_sys_t *p_sys;
    p_sys = malloc( sizeof( intf_sys_t ) );
    if( !p_sys )
      return VLC_ENOMEM;

    /* Read settings */
    char *mhost = var_InheritString(p_intf, "mqtt-host");
    if (mhost == NULL) goto error;
    p_sys->host = mhost;

    int mport = var_InheritInteger(p_intf, "mqtt-port");
    p_sys->port = mport;

    char *who = var_InheritString(p_intf, "mqtt-clientid");
    if ( who == NULL ) goto error;
    if ( strcmp( who, "random" ) == 0)
    {
      int i;
      char *mqttcid = malloc(24* sizeof(char));
      mqttcid[0] = 'm';
      mqttcid[1] = 'o';
      mqttcid[2] = 's';
      mqttcid[3] = 'q';
      mqttcid[4] = '-';

      for( i=5; i<23; i++ ){
          ((uint8_t *)mqttcid)[i] = (uint8_t )(random()&0xFF);
      }

      for( i=5; i<23; i++ ){
        mqttcid[i] = alphanum[(mqttcid[i]&0x7F)%(sizeof(alphanum)-1)];
      }
      strcpy(who, mqttcid);
    }
    p_sys->clientid = who;

    int major, minor, revision;
		mosquitto_lib_version(&major, &minor, &revision);
    msg_Info(p_intf, "MQTT %s running on libmosquitto %d.%d.%d", p_sys->clientid, major, minor, revision);

		char *musername = var_InheritString(p_intf, "mqtt-username");
		p_sys->username = musername;

		char *mpassword = var_InheritString(p_intf, "mqtt-password");
		p_sys->password = mpassword;

		char *mprefix = var_InheritString(p_intf, "mqtt-prefix");
		p_sys->prefix = mprefix;

		int mkeepalive = var_InheritInteger(p_intf, "mqtt-keepalive");
		p_sys->keepalive = mkeepalive;

    int mqos = var_InheritInteger(p_intf, "mqtt-qos");
    p_sys->qos = mqos;
    p_intf->p_sys = p_sys;

    bool mpub_status = var_InheritBool(p_intf, "mqtt-pub_status");
    p_sys->pub_status = mpub_status;

    mosquitto_lib_init();
    mosq = mosquitto_new( p_sys->clientid, true, p_intf);
    if( mosq == NULL ){
		    msg_Info(p_intf, "Error: Out of memory.\n");
		    goto error;
	  }
    p_sys->mosq = mosq;

		if( p_sys->username ){
			rc = mosquitto_username_pw_set(mosq, p_sys->username, p_sys->password);
			if( rc ){
				mosquitto_destroy(mosq);
				goto error;
			}
		}

    p_sys->p_input = NULL;
    var_AddCallback( p_intf->obj.libvlc, "key-action", ActionEvent, p_intf );

	  mosquitto_connect_callback_set(mosq, on_connect);
	  mosquitto_subscribe_callback_set(mosq, on_subscribe);
    mosquitto_publish_callback_set(mosq, on_publish);
	  mosquitto_message_callback_set(mosq, on_message);

	  rc = mosquitto_connect(mosq, p_sys->host, p_sys->port, p_sys->keepalive);
	  if( rc != MOSQ_ERR_SUCCESS ){
	  	mosquitto_destroy(mosq);
		  msg_Info(p_intf, "Error connecting: %s\n", mosquitto_strerror(rc));
		  goto error;
	  }

		char* str_command;
		str_command = "/command/";
		char * str_topic = (char *) malloc(1 + strlen(p_sys->prefix) + strlen(p_sys->clientid) + strlen(str_command) );
		strcpy(str_topic, p_sys->prefix);
		strcat(str_topic, p_sys->clientid);
		strcat(str_topic, str_command);

    rc = mosquitto_subscribe(mosq, NULL, str_topic, 0);
    if( rc != MOSQ_ERR_SUCCESS ){
      msg_Info(p_intf, "Error subscribing: %s\n", mosquitto_strerror(rc));
      /* We might as well disconnect if we were unable to subscribe */
      mosquitto_disconnect_v5(mosq, 0, NULL);
    }

    if( vlc_clone( &p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW ) )
    {
           goto error;
    }

    return VLC_SUCCESS;
error:
    free(p_sys);
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: destroy interface
 *****************************************************************************/
static void Close(vlc_object_t *obj)
{
    intf_thread_t *p_intf = (intf_thread_t *)obj;
    intf_sys_t *p_sys = p_intf->p_sys;

    msg_Info(p_intf, "Good bye MQTT Client %s!", p_sys->clientid);
    run = 0;

    var_DelCallback( p_intf->obj.libvlc, "key-action", ActionEvent, p_intf );
    // Close MQTT Connection
    mosquitto_lib_cleanup();

    vlc_cancel( p_sys->thread );
    vlc_join( p_sys->thread, NULL );

    /* Free internal state */
    free(p_sys->clientid);
    free(p_sys);
}

/*****************************************************************************
 * Run: main loop
 *****************************************************************************/
static void *Run( void *obj )
{
  intf_thread_t *p_intf = (intf_thread_t *)obj;
  intf_sys_t *p_sys = p_intf->p_sys;
	rc = 0;

  msg_Info(p_intf, "Run subscribe %s!", p_sys->clientid);
  while( run ){
    			rc = mosquitto_loop(p_sys->mosq, -1, 1);
    			if( run && rc ){
            msg_Info(p_intf, "Error Run subscribe %s!", p_sys->clientid);
    				sleep(10);
    				mosquitto_reconnect(mosq);
    			}
    		}
  mosquitto_destroy(mosq);
  return NULL;
}

/*****************************************************************************
 * ActionEvent: callback for hotkey actions
 *****************************************************************************/
static int ActionEvent( vlc_object_t *libvlc, char const *psz_var,
                        vlc_value_t oldval, vlc_value_t newval, void *obj )
{
    intf_thread_t *p_intf = (intf_thread_t *)obj;
    intf_sys_t *p_sys = p_intf->p_sys;
    VLC_UNUSED(libvlc);
    VLC_UNUSED(psz_var);
    VLC_UNUSED(oldval);
    VLC_UNUSED(newval);

    if( p_sys->pub_status ) {
      input_thread_t *p_input = p_sys->p_input ? vlc_object_hold( p_sys->p_input ) : NULL;
      //input_state_e toto = input_GetState( p_input );
      VLC_UNUSED(p_input); int state = 0;

      msg_Info(p_intf, "%s - ActionEvent! State %d", p_sys->clientid, state);

      char* str_status;
		  str_status = "/status/state/";
		  char * str_topic = (char *) malloc(1 + strlen(p_sys->prefix) + strlen(p_sys->clientid) + strlen(str_status) );
		  strcpy(str_topic, p_sys->prefix);
		  strcat(str_topic, p_sys->clientid);
		  strcat(str_topic, str_status);

      char* payload ;
      payload = "Hello World! Coming soon...";
      rc = mosquitto_publish(mosq, NULL, str_topic, strlen(payload), payload, p_sys->qos, false);
	    if( rc != MOSQ_ERR_SUCCESS ){
		    msg_Info(p_intf, "Error publishing: %s\n", mosquitto_strerror(rc));
	    }
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * MQTT function
 *****************************************************************************/
 static void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
  intf_thread_t *p_intf = (intf_thread_t *)obj;
  intf_sys_t *p_sys = p_intf->p_sys;

  if( reason_code == 0 ){
    /*    if(ctrl->cfg.protocol_version == MQTT_PROTOCOL_V5){
  		  msg_Info(p_intf, "%s: on_connect - %s", p_sys->clientid, mosquitto_reason_string(reason_code));*/
      msg_Info(p_intf, "%s: on_connect - %s", p_sys->clientid, mosquitto_connack_string(reason_code));
  }else{
  	/*	if(ctrl->cfg.protocol_version == MQTT_PROTOCOL_V5){
  			if(reason_code == MQTT_RC_UNSUPPORTED_PROTOCOL_VERSION){
  				fprintf(stderr, "Connection error: %s. Try connecting to an MQTT v5 broker, or use MQTT v3.x mode.\n", mosquitto_reason_string(reason_code));
  			}else{
  				fprintf(stderr, "Connection error: %s\n", mosquitto_reason_string(reason_code));
  			}*/
  		msg_Info(p_intf, "%s: Connection error: %s\n", p_sys->clientid, mosquitto_connack_string(reason_code));
  		run = 0;
  		mosquitto_disconnect_v5(mosq, 0, NULL);
  	}
}

/* Callback called when the client knows to the best of its abilities that a
 * PUBLISH has been successfully sent. For QoS 0 this means the message has
 * been completely written to the operating system. For QoS 1 this means we
 * have received a PUBACK from the broker. For QoS 2 this means we have
 * received a PUBCOMP from the broker. */
static void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
  VLC_UNUSED(mosq);
  intf_thread_t *p_intf = (intf_thread_t *)obj;
  intf_sys_t *p_sys = p_intf->p_sys;
  if( p_sys->pub_status )
	  msg_Info(p_intf, "%s: Message with mid %d has been published.", p_sys->clientid, mid);
}

/* Callback called when the broker sends a SUBACK in response to a SUBSCRIBE. */
static void on_subscribe(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
	int i;
	bool have_subscription = false;
  intf_thread_t *p_intf = (intf_thread_t *)obj;
  intf_sys_t *p_sys = p_intf->p_sys;
	/* In this example we only subscribe to a single topic at once, but a
	 * SUBSCRIBE can contain many topics at once, so this is one way to check
	 * them all. */
	for( i=0; i<qos_count; i++ ){
		msg_Info(p_intf, "%s: on_subscribe - mid:%d - %d:granted qos = %d", p_sys->clientid, mid, i, granted_qos[i]);
		if(granted_qos[i] <= 2){
			have_subscription = true;
		}
	}
	if(have_subscription == false){
		/* The broker rejected all of our subscriptions, we know we only sent
		 * the one SUBSCRIBE, so there is no point remaining connected. */
		msg_Info(p_intf, "%s: Error: All subscriptions rejected.", p_sys->clientid);
		mosquitto_disconnect_v5(mosq, 0, NULL);
	}
}


/* Callback called when the client receives a message. */
static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	VLC_UNUSED(mosq);
  intf_thread_t *p_intf = (intf_thread_t *)obj;
  intf_sys_t *p_sys = p_intf->p_sys;

	msg_Info(p_intf, "%s: %s %s", p_sys->clientid, msg->topic, (char *)msg->payload);

  // Process
  if( !strncmp( "key-", (char *)msg->payload, 4 ) )
  { //https://github.com/RPi-Distro/vlc/blob/buster-rpt/src/misc/actions.c#L260 : list of key action
    vlc_action_id_t i_key = vlc_actions_get_id((char *)msg->payload);
    if( i_key )
    {
        var_SetInteger( p_intf->obj.libvlc, "key-action", i_key );
    }
    else
        msg_Err( p_intf, "Unknown hotkey '%s'", (char *)msg->payload);
    }
  else
  {
    msg_Err( p_intf, "this doesn't appear to be a valid key command ");
  }
}
