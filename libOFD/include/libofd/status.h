#ifndef LIBOFD_STATUS_H
#define LIBOFD_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum libofd_status {
    LIBOFD_OK = 0,
    LIBOFD_ERR_INVALID_ARGUMENT = 1,
    LIBOFD_ERR_IO = 2,
    LIBOFD_ERR_NOT_FOUND = 3,
    LIBOFD_ERR_PARSE = 4,
    LIBOFD_ERR_UNSUPPORTED = 5
} libofd_status_t;

const char* libofd_status_message(libofd_status_t status);

#ifdef __cplusplus
}
#endif

#endif

