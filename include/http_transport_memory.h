#pragma once
#include <stddef.h>

// Discard a failed HTTP transmission instead of leaving its TCP buffers
// queued for minutes after the application has already abandoned the reply.
void abortHttpTransport(int socket);

// Available application send budget; does not change TCP PCB bookkeeping.
size_t httpSendAllowance(int socket);
