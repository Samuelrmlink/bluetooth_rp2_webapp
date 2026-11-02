#ifndef C5_HTTP_API_H
#define C5_HTTP_API_H

void http_api_start(void);
/* Queue one {"evt","data"} line. http_api_poll drains it and, once a second,
 * queues tel.sample (heap and uptime). File chunks wait while the queue
 * is not empty. */
void http_api_emit(const char *topic, const char *json);
void http_api_poll(void);

#endif
