#ifndef MQTT_H
#define MQTT_H


#include <os_type.h>


#ifdef __cplusplus
extern "C" {
#endif


// Configuration
struct mqtt_config
{
	const char *host;
	uint16_t port;
	uint16_t keepalive;
	uint8_t q_max;
	
	const char *client, *user, *pass;
	const char *will_topic, *will_message;
	uint8_t will_qos, will_retain;
};

extern struct mqtt_config mqtt_config;


// API
uint8_t mqtt_connect(void);
uint8_t mqtt_publish(const char *topic, const char *msg, uint8_t qos, uint8_t retain);
uint8_t mqtt_subscribe(const char *topic, uint8_t qos);
uint8_t mqtt_unsubscribe(const char *topic);


// Callbacks
extern void mqtt_error_cb(void*);
extern void mqtt_open_cb(void*);
extern void mqtt_publish_cb(const char *topic, const char *msg, uint8_t qos, uint8_t retain);


#ifdef __cplusplus
}
#endif


#endif
