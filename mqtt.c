#include "mqtt.h"

#include <os_type.h>
#include <osapi.h>
#include <user_interface.h>
#include <espconn.h>
#include <mem.h>

#include <pt/pt.h>
#include <sched.h>


#ifdef EBUG
	#define debug(...)	do{ os_printf(__VA_ARGS__); }while(0)
	
	static void hexdump(const uint8_t *data, int len)
	{
		for (int i=0; i<len; i++)
			os_printf(" %02X", data[i]);
		os_printf("\n");
	}
#else
	#define debug(...)			do{}while(0)
	#define hexdump(data, len)	do{}while(0)
#endif


// Типы пакетов
#define CONNECT			0x10
#define CONNACK			0x20
#define PUBLISH			0x30
#define PUBACK			0x40
#define PUBREC			0x50
#define PUBREL			0x60
#define PUBCOMP			0x70
#define SUBSCRIBE		0x80
#define SUBACK			0x90
#define UNSUBSCRIBE		0xA0
#define UNSUBACK		0xB0
#define PINGREQ			0xC0
#define PINGRESP		0xD0
#define DISCONNECT		0xE0


enum
{
	CLOSED=0,
	CONNECTING,
	OPEN,
	DATA_SENT,
	CLOSING,
};


struct Q
{
	const uint8_t *data;
	uint16_t len;
	
	struct Q *next;
};


struct mqtt_config mqtt_config;

static struct espconn conn;
static esp_tcp tcp_conn;
static ip_addr_t conn_ip;
static os_timer_t tmr_timeout, tmr_keepalive;
static uint8_t state;
static struct pt pt;
static uint8_t *rxbuf=0;
static struct Q *sendQ=0;
static uint16_t nextId=0;


static void sendFromQ(void)
{
	if ( (! sendQ) || (state != OPEN) ) return;
	
	debug("MQTT: sending from Q\n");
	espconn_send(&conn, (uint8_t*)sendQ->data, sendQ->len);
	os_free((void*)sendQ->data);
	struct Q *next=sendQ->next;
	os_free((void*)sendQ);
	sendQ=next;
	
	state=DATA_SENT;
	
	// Перевзводим таймер ping
	os_timer_disarm(&tmr_keepalive);
	os_timer_arm(&tmr_keepalive, mqtt_config.keepalive*1000, 0);
}


static uint8_t enQ(const uint8_t *data, uint16_t len, uint8_t force)
{
	struct Q **l=&sendQ;
	int count=0;
	while (*l)
	{
		count++;
		if ( (! force) && (count >= mqtt_config.q_max) )
		{
			os_free((void*)data);
			return 0;
		}
		l=&((*l)->next);
	};
	
	(*l)=(struct Q*)os_malloc(sizeof(struct Q));
	(*l)->data=data;
	(*l)->len=len;
	(*l)->next=0;
	
	sendFromQ();
	
	return 1;
}


static void removeQ(void)
{
	while (sendQ)
	{
		os_free((void*)sendQ->data);
		struct Q *next=sendQ->next;
		os_free((void*)sendQ);
		sendQ=next;
	}
}


static uint8_t put_header(uint8_t *p, uint8_t type, uint16_t len)
{
	(*p++)=type;
	if (len > 127)
	{
		(*p++)=0x80 + (len & 0x7f);
		len>>=7;
		(*p++)=len;
		return 3;
	} else
	{
		(*p++)=len;
		return 2;
	}
}


static uint8_t put_u16(uint8_t *p, uint16_t value)
{
	(*p++)=value >> 8;
	(*p++)=value & 0xff;
	return 2;
}


static uint16_t put_string(uint8_t *p, const char *s)
{
	uint16_t l=os_strlen(s);
	
	(*p++)=l >> 8;
	(*p++)=l & 0xff;
	os_memcpy(p, s, l);
	
	return 2+l;
}


static uint8_t handle_rxbuf(uint8_t type, uint16_t len)
{
	// Перевзводим таймаут приема
	os_timer_disarm(&tmr_timeout);
	os_timer_arm(&tmr_timeout, mqtt_config.keepalive*1500, 0);
	
	switch (type & 0xF0)
	{
		case CONNACK:
			if ( (state == CONNECTING) && (len == 2) && (rxbuf[1] == 0) )
			{
				// Успешное открытие соединения
				debug("MQTT: CONNACK\n");
				state=OPEN;
				sched(mqtt_open_cb, 0);
				
				// Взводим таймер отправки ping
				os_timer_arm(&tmr_keepalive, mqtt_config.keepalive*1000, 0);
				return 1;
			} else
			{
				// Ошибка
				debug("MQTT: CONNACK error %d (state=%d len=%d)\n", rxbuf[1], state, len);
				return 0;
			}
		
		case PUBLISH:
			{
				debug("MQTT: incoming publish\n");
				if ( ( (state != OPEN) && (state != DATA_SENT) ) || (len < 4) )
				{
					debug("MQTT: bad state/len\n");
					return 0;
				}
				if (type & 0x08) return 1;	// DUP пропускаем
				uint8_t qos=(type >> 1) & 0x03;
				uint8_t retain=(type & 0x01);
				uint8_t *p=rxbuf;
				
				// Получаем топик
				uint16_t topic_len=(p[0] << 8) | p[1]; p+=2; len-=2;
				if (topic_len > len)
				{
					debug("MQTT: bad topic len %d (left %d)\n", topic_len, len);
					return 0;
				}
				char *topic=(char*)p, *topic_end=(char*)(p+topic_len);
				p+=topic_len;
				len-=topic_len;
				
				// Получаем ID
				uint16_t id=0;
				if (qos > 0)
				{
					if (len < 2)
					{
						debug("MQTT: bad len (id)\n");
						return 0;
					}
					id=(p[0] << 8) | p[1];
					p+=2;
					len-=2;
				}
				
				// Получаем сообщение
				char *msg=(char*)p, *msg_end=(char*)(p+len);
				
				// Устанавливаем концы строк (один лишний байт есть в буфере)
				(*topic_end)=0;
				(*msg_end)=0;
				
				// Вызываем callback
				mqtt_publish_cb(topic, msg, qos, retain);
				
				// Отправляем ответ, если надо
				if (qos > 0)
				{
					debug("MQTT: sending %s\n", (qos == 1) ? "PUBACK" : "PUBREL");
					uint8_t *buf=(uint8_t*)os_malloc(4);
					buf[0]=(qos == 1) ? PUBACK : PUBREC;
					buf[1]=2;
					buf[2]=id >> 8;
					buf[3]=id & 0xff;
					enQ(buf, 4, 1);
				}
			}
			return 1;
		
		case PUBREC:
		case PUBREL:
			debug("MQTT: PUBREC/PUBREL\n");
			if (len == 2)
			{
				debug("MQTT: sending %s\n", ((type & 0xF0) == PUBREC) ? "PUBREL" : "PUBCOMP");
				uint8_t *buf=(uint8_t*)os_malloc(4);
				buf[0]=((type & 0xF0) == PUBREC) ? PUBREL : PUBCOMP;
				buf[1]=2;
				buf[2]=rxbuf[0];
				buf[3]=rxbuf[1];
				enQ(buf, 4, 1);
			} else
			{
				debug("MQTT: bad len\n");
				return 0;
			}
			return 1;
		
		case PUBACK:
		case PUBCOMP:
		case SUBACK:
		case UNSUBACK:
		case PINGRESP:
			debug("MQTT: *ack/pingresp\n");
			return 1;
		
		default:
			debug("MQTT: bad packet\n");
			return 0;
	}
}


static PT_THREAD(recv_pt(char *data, unsigned short size))
{
	static uint8_t b, type;
	static uint16_t len, pos;
	
#define BYTE(var)	do{ if (size == 0) PT_YIELD(&pt); var=(*data++); size--; }while(0)
	PT_BEGIN(&pt);
		
again:
		// Получаем тип
		BYTE(type);
		debug("MQTT: recv type=0x%02X\n", type);
		b=(type & 0xF0);
		if ( (b != CONNACK) &&
			 (b != PUBLISH) &&
			 (b != PUBACK) &&
			 (b != PUBREC) &&
			 (b != PUBREL) &&
			 (b != PUBCOMP) &&
			 (b != SUBACK) &&
			 (b != UNSUBACK) &&
			 (b != PINGRESP) )
		{
			// Неверный тип пакета
			debug("MQTT: bad type\n");
			goto error;
		}
		
		// Получаем размер
		BYTE(b);
		if (! (b & 0x80)) len=b; else
		{
			len=b & 0x7f;
			BYTE(b);
			len|=b << 7;
		}
		debug("MQTT: recv len=%d\n", len);
		
		// Проверяем максимальный размер
		if (len >= 256)
		{
			// Слишком длинный пакет
			debug("MQTT: packet too big\n");
			goto error;
		}
		
		if (len > 0)
		{
			// Создаем буфер
			rxbuf=(uint8_t*)os_malloc(len+1);	// 1 байт добавляем, чтобы можно было установить нулевой символ для publish
			
			// Принимаем данные
			pos=0;
			while (pos < len)
			{
				// Ждем прием, если надо
				if (size == 0) PT_YIELD(&pt);
				
				// Копируем данные
				uint16_t l=(len-pos);
				if (l > size) l=size;
				os_memcpy(rxbuf+pos, data, l);
				pos+=l;
				data+=l;
				size-=l;
			}
		}
		
		// Обрабатываем пакет
		debug("MQTT: recv done\n");
		if (! handle_rxbuf(type, len)) goto error;
		
		// Удаляем буфер
		os_free((void*)rxbuf);
		rxbuf=0;
		
		// На начало приема
		goto again;
		
		
error:
		// Ошибка - надо закрыть соединение
		state=CLOSING;
		if (rxbuf)
		{
			os_free((void*)rxbuf);
			rxbuf=0;
		}
		os_timer_disarm(&tmr_timeout);
		os_timer_disarm(&tmr_keepalive);
		sched_espconn_disconnect(&conn);
		debug("MQTT: close by recv error\n");
		
	PT_END(&pt);
#undef BYTE
}


static void recv_cb(void *arg, char *data, unsigned short size)
{
	debug("MQTT: recv_cb(size=%d)\n", size);
	hexdump((const uint8_t*)data, size);
	
	if ( (state != CONNECTING) &&
		 (state != OPEN) &&
		 (state != DATA_SENT) )
	{
		// Соединение закрыто
		debug("MQTT: skipped\n");
		return;
	}
	
	(void)PT_SCHEDULE(recv_pt(data, size));
}


static void sent_cb(void *arg)
{
	debug("MQTT: sent_cb\n");
	if (state == DATA_SENT)
	{
		state=OPEN;
		sendFromQ();
	}
}


static void connect_cb(void *arg)
{
	// Подключено
	debug("MQTT: connected\n");
	state=CONNECTING;
	
	// Собираем пакет CONNECT
#define mstrlen(s)	((s) ? (2+os_strlen(s)) : 0)
	uint16_t len=
		6+	// "MQTT"
		1+	// protocol level
		1+	// connect flags
		2+	// keep alive
		mstrlen(mqtt_config.client)+
		mstrlen(mqtt_config.will_topic)+
		mstrlen(mqtt_config.will_message)+
		mstrlen(mqtt_config.user)+
		mstrlen(mqtt_config.pass);
#undef mstrlen
	
	// Создаем буфер
	uint16_t buflen=1 + ((len < 128) ? 1 : 2) + len;
	uint8_t *buf=(uint8_t*)os_malloc(buflen), *p=buf;
	
	// Заполняем данные
	p+=put_header(p, CONNECT, len);
	p+=put_string(p, "MQTT");
	(*p++)=4;	// protocol level
	(*p++)=		// connect flags
		(mqtt_config.user ? 0x80 : 0) |
		(mqtt_config.pass ? 0x40 : 0) |
		(mqtt_config.will_retain ? 0x20 : 0) |
		(mqtt_config.will_qos << 3) |
		(mqtt_config.will_topic ? 0x08 : 0) |
		0x02;	// clean session
	p+=put_u16(p, mqtt_config.keepalive);
	p+=put_string(p, mqtt_config.client);
	if (mqtt_config.will_topic)
	{
		p+=put_string(p, mqtt_config.will_topic);
		p+=put_string(p, mqtt_config.will_message);
	}
	if (mqtt_config.user)
		p+=put_string(p, mqtt_config.user);
	if (mqtt_config.pass)
		p+=put_string(p, mqtt_config.pass);
	
	// Отправляем
	debug("MQTT: sending CONNECT\n");
	hexdump(buf, buflen);
	espconn_send(&conn, buf, buflen);
	
	// Удаляем буфер
	os_free((void*)buf);
	
	// Взводим таймаут
	os_timer_arm(&tmr_timeout, 10000, 0);
}


static void discon_cb(void *arg)
{
	debug("MQTT: discon_cb\n");
	state=CLOSED;
	if (rxbuf)
	{
		os_free((void*)rxbuf);
		rxbuf=0;
	}
	removeQ();
	os_timer_disarm(&tmr_timeout);
	os_timer_disarm(&tmr_keepalive);
	sched_espconn_delete(&conn);
	sched(mqtt_error_cb, 0);
}


static void recon_cb(void *arg, sint8 err)
{
	debug("MQTT: recon_cb\n");
	discon_cb(arg);
}


static void timeout_cb(void *arg)
{
	debug("MQTT: timeout_cb\n");
	state=CLOSING;
	if (rxbuf)
	{
		os_free((void*)rxbuf);
		rxbuf=0;
	}
	os_timer_disarm(&tmr_timeout);
	os_timer_disarm(&tmr_keepalive);
	sched_espconn_disconnect(&conn);
}


static void keepalive_cb(void *arg)
{
	debug("MQTT: keepalive_cb\n");
	
	// Отправляем PING
	uint8_t buf[2]={ PINGREQ, 0 };
	espconn_send(&conn, buf, 2);
	
	// Перевзводим таймер на повторную отправку
	os_timer_disarm(&tmr_keepalive);
	os_timer_arm(&tmr_keepalive, mqtt_config.keepalive*1000, 0);
}


static void dns_cb(const char *name, ip_addr_t *ipaddr, void *arg)
{
	if (! ipaddr)
	{
		debug("MQTT: DNS error\n");
		sched(mqtt_error_cb, 0);
	} else
	{
		// Настраиваем соединение
		conn.proto.tcp->local_port=espconn_port();
		conn.proto.tcp->remote_port=mqtt_config.port;
		os_memcpy(conn.proto.tcp->remote_ip, &ipaddr->addr, 4);
		
		// Регистрируем вызовы
		espconn_regist_connectcb(&conn, connect_cb);
		espconn_regist_recvcb(&conn, recv_cb);
		espconn_regist_sentcb(&conn, sent_cb);
		espconn_regist_reconcb(&conn, recon_cb);
		espconn_regist_disconcb(&conn, discon_cb);
		
		// Инитим состояние соединения
		state=CLOSED;
		PT_INIT(&pt);
		
		// Подключаемся
		espconn_connect(&conn);
		debug("MQTT: connecting...\n");
	}
}


uint8_t mqtt_connect(void)
{
	debug("MQTT: mqtt_connect()\n");
	
	if (state != CLOSED)
	{
		debug("MQTT: not closed !\n");
		return 0;
	}
	
	// Инитим соединение
	ets_memset(&conn, 0, sizeof(conn));
	ets_memset(&tcp_conn, 0, sizeof(tcp_conn));
	conn.type=ESPCONN_TCP;
	conn.state=ESPCONN_NONE;
	conn.proto.tcp=&tcp_conn;
	espconn_set_opt(&conn, ESPCONN_REUSEADDR | ESPCONN_NODELAY);
	
	// Настраиваем таймеры
	os_timer_setfn(&tmr_timeout, timeout_cb, 0);
	os_timer_setfn(&tmr_keepalive, keepalive_cb, 0);
	
	// Определяем хост
	debug("MQTT: resolving '%s'...\n", mqtt_config.host);
	switch (espconn_gethostbyname(&conn, mqtt_config.host, &conn_ip, dns_cb))
	{
		case ESPCONN_OK:
			dns_cb(mqtt_config.host, &conn_ip, 0);
			break;
		
		case ESPCONN_INPROGRESS:
			break;
		
		default:
			debug("MQTT: gethostbyname error\n");
			return 0;
	}
	
	return 1;
}


uint8_t mqtt_publish(const char *topic, const char *msg, uint8_t qos, uint8_t retain)
{
	if ( (state != OPEN) && (state != DATA_SENT) ) return 0;
	
	uint16_t len=
		2+os_strlen(topic)+
		((qos > 0) ? 2 : 0)+
		os_strlen(msg);
	
	uint16_t buflen=1 + ((len < 128) ? 1 : 2) + len;
	uint8_t *buf=(uint8_t*)os_malloc(buflen), *p=buf;
	
	p+=put_header(p, PUBLISH | (qos << 1) | (retain ? 0x01 : 0), len);
	p+=put_string(p, topic);
	if (qos > 0)
	{
		nextId++;
		if (nextId == 0) nextId=1;
		p+=put_u16(p, nextId);
	}
	os_memcpy(p, msg, os_strlen(msg));
	
	debug("MQTT: publish\n");
	hexdump(buf, buflen);
	
	return enQ(buf, buflen, 0);
}


uint8_t mqtt_subscribe(const char *topic, uint8_t qos)
{
	if ( (state != OPEN) && (state != DATA_SENT) ) return 0;
	
	uint16_t len=
		2+
		2+os_strlen(topic)+
		1;
	
	uint16_t buflen=1 + ((len < 128) ? 1 : 2) + len;
	uint8_t *buf=(uint8_t*)os_malloc(buflen), *p=buf;
	
	p+=put_header(p, SUBSCRIBE | 0x02, len);
	nextId++;
	if (nextId == 0) nextId=1;
	p+=put_u16(p, nextId);
	p+=put_string(p, topic);
	(*p++)=qos;
	
	debug("MQTT: subscribe\n");
	hexdump(buf, buflen);
	
	return enQ(buf, buflen, 0);
}


uint8_t mqtt_unsubscribe(const char *topic)
{
	if ( (state != OPEN) && (state != DATA_SENT) ) return 0;
	
	uint16_t len=
		2+
		2+os_strlen(topic);
	
	uint16_t buflen=1 + ((len < 128) ? 1 : 2) + len;
	uint8_t *buf=(uint8_t*)os_malloc(buflen), *p=buf;
	
	p+=put_header(p, UNSUBSCRIBE | 0x02, len);
	nextId++;
	if (nextId == 0) nextId=1;
	p+=put_u16(p, nextId);
	p+=put_string(p, topic);
	
	debug("MQTT: unsubscribe\n");
	hexdump(buf, buflen);
	
	return enQ(buf, buflen, 0);
}
