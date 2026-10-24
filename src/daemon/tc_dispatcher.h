#ifndef TC_DISPATCHER_H
#define TC_DISPATCHER_H

#include "tc_common.h"

/* Handle an accepted client socket connection */
void tc_dispatcher_handle_client(int client_fd);
void tc_dispatcher_shutdown(void);

#endif /* TC_DISPATCHER_H */
