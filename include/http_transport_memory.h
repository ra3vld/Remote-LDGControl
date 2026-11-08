#pragma once
#include <stddef.h>

// Set the Sunton's bounded outgoing segment size on a newly accepted socket.
// Other board profiles retain the SDK's negotiated MSS.
void configureHttpTransport(int socket);

// Discard a failed HTTP transmission instead of leaving its TCP buffers
// queued for minutes after the application has already abandoned the reply.
void abortHttpTransport(int socket);

// Available application send budget; does not change TCP PCB bookkeeping.
size_t httpSendAllowance(int socket);
