#ifndef MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_

#include <sys/socket.h>

extern "C" {

int mocktail_bionic_setsockopt(int socket, int level, int option_name,
                               const void *option_value,
                               socklen_t option_length);

ssize_t mocktail_bionic_sendmsg(int socket, const struct msghdr *message,
                                int flags);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_SOCKET_RUNTIME_H_
