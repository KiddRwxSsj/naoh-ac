#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*mqtt_message_cb)(const char *topic, const uint8_t *payload, unsigned len);

// host is what the broker's tls certificate gets checked against
bool mqtt_open(const char *host, uint32_t addr, uint16_t port, const char *client_id,
      const char *will_topic, unsigned keepalive_s);
void mqtt_abort(void);
void mqtt_close(void);
bool mqtt_is_open(void);
const char *mqtt_error(void);
bool mqtt_publish(const char *topic, const void *payload, unsigned len, bool retain);
bool mqtt_subscribe(const char *filter);
bool mqtt_unsubscribe(const char *filter);
bool mqtt_poll(mqtt_message_cb cb);

#endif
