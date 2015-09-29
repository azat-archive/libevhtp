#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include "evhtp2/internal.h"
#include "evhtp2/evhtp_parser.h"

#ifdef PARSER_DEBUG
#define __QUOTE(x)                       # x
#define  _QUOTE(x)                       __QUOTE(x)
#define evhtp_parser_debug_strlen(x)     strlen(x)

#define evhtp_parser_log_debug(fmt, ...) do {                                                             \
        time_t      t  = time(NULL);                                                                      \
        struct tm * dm = localtime(&t);                                                                   \
                                                                                                          \
        fprintf(stdout, "[%02d:%02d:%02d] evhtp_parser.c:[" _QUOTE(__LINE__) "]\t                %-26s: " \
                fmt "\n", dm->tm_hour, dm->tm_min, dm->tm_sec, __func__, ## __VA_ARGS__);                 \
        fflush(stdout);                                                                                   \
} while (0)

#else
#define evhtp_parser_debug_strlen(x)     0
#define evhtp_parser_log_debug(fmt, ...) do {} while (0)
#endif

#if '\n' != '\x0a' || 'A' != 65
#error "You have somehow found a non-ASCII host. We can't build here."
#endif

#define PARSER_STACK_MAX 8192
#define LF               (unsigned char)10
#define CR               (unsigned char)13
#define CRLF             "\x0d\x0a"

enum eval_hdr_val {
    eval_hdr_val_none = 0,
    eval_hdr_val_connection,
    eval_hdr_val_proxy_connection,
    eval_hdr_val_content_length,
    eval_hdr_val_transfer_encoding,
    eval_hdr_val_hostname,
    eval_hdr_val_content_type
};

enum parser_flags {
    parser_flag_chunked               = (1 << 0),
    parser_flag_connection_keep_alive = (1 << 1),
    parser_flag_connection_close      = (1 << 2),
    parser_flag_trailing              = (1 << 3),
};

enum parser_state {
    s_start = 0,
    s_method,
    s_spaces_before_uri,
    s_schema,
    s_schema_slash,
    s_schema_slash_slash,
    s_host,
    s_host_ipv6,
    s_host_done,
    s_port,
    s_after_slash_in_uri,
    s_check_uri,
    s_uri,
    s_http_09,
    s_http_H,
    s_http_HT,
    s_http_HTT,
    s_http_HTTP,
    s_first_major_digit,
    s_major_digit,
    s_first_minor_digit,
    s_minor_digit,
    s_spaces_after_digit,
    s_almost_done,
    s_done,
    s_hdrline_start,
    s_hdrline_hdr_almost_done,
    s_hdrline_hdr_done,
    s_hdrline_hdr_key,
    s_hdrline_hdr_space_before_val,
    s_hdrline_hdr_val,
    s_hdrline_almost_done,
    s_hdrline_done,
    s_body_read,
    s_chunk_size_start,
    s_chunk_size,
    s_chunk_size_almost_done,
    s_chunk_data,
    s_chunk_data_almost_done,
    s_chunk_data_done,
    s_status,
    s_space_after_status,
    s_status_text
};

typedef enum eval_hdr_val eval_hdr_val;
typedef enum parser_flags parser_flags;
typedef enum parser_state parser_state;


struct evhtp_parser {
    evhtp_parser_error error;
    parser_state       state;
    parser_flags       flags;
    eval_hdr_val       heval;

    evhtp_parser_type   type;
    evhtp_parser_scheme scheme;
    evhtp_method        method;

    unsigned char multipart;
    unsigned char major;
    unsigned char minor;
    uint64_t      content_len;      /* this gets decremented as data passes through */
    uint64_t      orig_content_len; /* this contains the original length of the body */
    uint64_t      bytes_read;
    uint64_t      total_bytes_read;
    unsigned int  status;           /* only for responses */
    unsigned int  status_count;     /* only for responses */

    char * scheme_offset;
    char * host_offset;
    char * port_offset;
    char * path_offset;
    char * args_offset;

    void * userdata;

    unsigned int buf_idx;
    /* Must be last since evhtp_parser_init memsets up to the offset of this buffer */
    char buf[PARSER_STACK_MAX];
};

static uint32_t     usual[] = {
    0xffffdbfe,
    0x7fff37d6,
    0xffffffff,
    0xffffffff,
    0xffffffff,
    0xffffffff,
    0xffffffff,
    0xffffffff
};

static int8_t       unhex[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static const char * errstr_map[] = {
    "evhtp_parser_error_none",
    "evhtp_parser_error_too_big",
    "evhtp_parser_error_invalid_method",
    "evhtp_parser_error_invalid_requestline",
    "evhtp_parser_error_invalid_schema",
    "evhtp_parser_error_invalid_protocol",
    "evhtp_parser_error_invalid_version",
    "evhtp_parser_error_invalid_header",
    "evhtp_parser_error_invalid_chunk_size",
    "evhtp_parser_error_invalid_chunk",
    "evhtp_parser_error_invalid_state",
    "evhtp_parser_error_user",
    "evhtp_parser_error_unknown"
};

static const char * method_strmap[] = {
    "GET",
    "HEAD",
    "POST",
    "PUT",
    "DELETE",
    "MKCOL",
    "COPY",
    "MOVE",
    "OPTIONS",
    "PROPFIND",
    "PROPATCH",
    "LOCK",
    "UNLOCK",
    "TRACE",
    "CONNECT",
    "PATCH",
};

#define _MIN_READ(a, b) ((a) < (b) ? (a) : (b))

#ifndef HOST_BIG_ENDIAN
/* Little-endian cmp macros */
#define _str3_cmp(m, c0, c1, c2, c3) \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define _str3Ocmp(m, c0, c1, c2, c3) \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define _str4cmp(m, c0, c1, c2, c3) \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define _str5cmp(m, c0, c1, c2, c3, c4)                          \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0) \
    && m[4] == c4

#define _str6cmp(m, c0, c1, c2, c3, c4, c5)                      \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0) \
    && (((uint32_t *)m)[1] & 0xffff) == ((c5 << 8) | c4)

#define _str7_cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)             \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0) \
    && ((uint32_t *)m)[1] == ((c7 << 24) | (c6 << 16) | (c5 << 8) | c4)

#define _str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)              \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0) \
    && ((uint32_t *)m)[1] == ((c7 << 24) | (c6 << 16) | (c5 << 8) | c4)

#define _str9cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8)                 \
    *(uint32_t *)m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)        \
    && ((uint32_t *)m)[1] == ((c7 << 24) | (c6 << 16) | (c5 << 8) | c4) \
    && m[8] == c8
#else
/* Big endian cmp macros */
#define _str3_cmp(m, c0, c1, c2, c3) \
    m[0] == c0 && m[1] == c1 && m[2] == c2

#define _str3Ocmp(m, c0, c1, c2, c3) \
    m[0] == c0 && m[2] == c2 && m[3] == c3

#define _str4cmp(m, c0, c1, c2, c3) \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3

#define _str5cmp(m, c0, c1, c2, c3, c4) \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4

#define _str6cmp(m, c0, c1, c2, c3, c4, c5)              \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 \
    && m[4] == c4 && m[5] == c5

#define _str7_cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)     \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 \
    && m[4] == c4 && m[5] == c5 && m[6] == c6

#define _str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)      \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 \
    && m[4] == c4 && m[5] == c5 && m[6] == c6 && m[7] == c7

#define _str9cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8)  \
    m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 \
    && m[4] == c4 && m[5] == c5 && m[6] == c6 && m[7] == c7 && m[8] == c8

#endif


#define __HTPARSE_GENHOOK(__n)                                                             \
    static inline int hook_ ## __n ## _run(evhtp_parser * p, evhtp_parser_hooks * hooks) { \
        evhtp_parser_log_debug("enter");                                                   \
        if (hooks && (hooks)->__n) {                                                       \
            return (hooks)->__n(p);                                                        \
        }                                                                                  \
                                                                                           \
        return 0;                                                                          \
    }

#define __HTPARSE_GENDHOOK(__n)                                                                                      \
    static inline int hook_ ## __n ## _run(evhtp_parser * p, evhtp_parser_hooks * hooks, const char * s, size_t l) { \
        evhtp_parser_log_debug("enter");                                                                             \
        if (hooks && (hooks)->__n) {                                                                                 \
            return (hooks)->__n(p, s, l);                                                                            \
        }                                                                                                            \
                                                                                                                     \
        return 0;                                                                                                    \
    }

__HTPARSE_GENHOOK(on_msg_begin)
__HTPARSE_GENHOOK(on_hdrs_begin)
__HTPARSE_GENHOOK(on_hdrs_complete)
__HTPARSE_GENHOOK(on_new_chunk)
__HTPARSE_GENHOOK(on_chunk_complete)
__HTPARSE_GENHOOK(on_chunks_complete)
__HTPARSE_GENHOOK(on_msg_complete)

__HTPARSE_GENDHOOK(method)
__HTPARSE_GENDHOOK(scheme)
__HTPARSE_GENDHOOK(host)
__HTPARSE_GENDHOOK(port)
__HTPARSE_GENDHOOK(path)
__HTPARSE_GENDHOOK(args)
__HTPARSE_GENDHOOK(uri)
__HTPARSE_GENDHOOK(hdr_key)
__HTPARSE_GENDHOOK(hdr_val)
__HTPARSE_GENDHOOK(body)
__HTPARSE_GENDHOOK(hostname)


static inline uint64_t
str_to_uint64(char * str, size_t n, int * err) {
    uint64_t value;

    if (n > 20) {
        /* 18446744073709551615 is 20 bytes */
        *err = 1;
        return 0;
    }

    for (value = 0; n--; str++) {
        uint64_t check;

        if (*str < '0' || *str > '9') {
            *err = 1;
            return 0;
        }

        check = value * 10 + (*str - '0');

        if ((value && check <= value) || check > UINT64_MAX) {
            *err = 1;
            return 0;
        }

        value = check;
    }

    return value;
}

static inline ssize_t
_str_to_ssize_t(char * str, size_t n) {
    ssize_t value;

    if (n == 0) {
        return -1;
    }

    for (value = 0; n--; str++) {
        if (*str < '0' || *str > '9') {
            return -1;
        }

        value = value * 10 + (*str - '0');

#if 0
        if (value > INTMAX_MAX) {
            return -1;
        }
#endif
    }

    return value;
}

inline evhtp_parser_error
evhtp_parser_get_error(evhtp_parser * p) {
    return p->error;
}

inline const char *
evhtp_parser_get_strerror(evhtp_parser * p) {
    evhtp_parser_error e = evhtp_parser_get_error(p);

    if (e > evhtp_parser_error_generic) {
        return "evhtp_parser_no_such_error";
    }

    return errstr_map[e];
}

inline unsigned int
evhtp_parser_get_status(evhtp_parser * p) {
    return p->status;
}

inline int
evhtp_parser_should_keep_alive(evhtp_parser * p) {
    if (p->major > 0 && p->minor > 0) {
        if (p->flags & parser_flag_connection_close) {
            return 0;
        } else {
            return 1;
        }
    } else {
        if (p->flags & parser_flag_connection_keep_alive) {
            return 1;
        } else {
            return 0;
        }
    }

    return 0;
}

inline evhtp_parser_scheme
evhtp_parser_get_scheme(evhtp_parser * p) {
    return p->scheme;
}

inline evhtp_method
evhtp_parser_get_method(evhtp_parser * p) {
    return p->method;
}

inline const char *
evhtp_parser_get_methodstr_m(evhtp_method meth) {
    if (meth >= evhtp_method_UNKNOWN) {
        return NULL;
    }

    return method_strmap[meth];
}

inline const char *
evhtp_parser_get_methodstr(evhtp_parser * p) {
    return evhtp_parser_get_methodstr_m(p->method);
}

inline void
evhtp_parser_set_major(evhtp_parser * p, unsigned char major) {
    p->major = major;
}

inline void
evhtp_parser_set_minor(evhtp_parser * p, unsigned char minor) {
    p->minor = minor;
}

inline unsigned char
evhtp_parser_get_major(evhtp_parser * p) {
    return p->major;
}

inline unsigned char
evhtp_parser_get_minor(evhtp_parser * p) {
    return p->minor;
}

inline unsigned char
evhtp_parser_get_multipart(evhtp_parser * p) {
    return p->multipart;
}

inline void *
evhtp_parser_get_userdata(evhtp_parser * p) {
    return p->userdata;
}

inline void
evhtp_parser_set_userdata(evhtp_parser * p, void * ud) {
    p->userdata = ud;
}

inline uint64_t
evhtp_parser_get_content_pending(evhtp_parser * p) {
    return p->content_len;
}

inline uint64_t
evhtp_parser_get_content_length(evhtp_parser * p) {
    return p->orig_content_len;
}

inline uint64_t
evhtp_parser_get_bytes_read(evhtp_parser * p) {
    return p->bytes_read;
}

inline uint64_t
evhtp_parser_get_total_bytes_read(evhtp_parser * p) {
    return p->total_bytes_read;
}

void
evhtp_parser_init(evhtp_parser * p, evhtp_parser_type type) {
    /* Do not memset entire string buffer. */
    memset(p, 0, offsetof(evhtp_parser, buf));
    p->buf[0] = '\0';
    p->state  = s_start;
    p->error  = evhtp_parser_error_none;
    p->method = evhtp_method_UNKNOWN;
    p->type   = type;
}

inline evhtp_parser *
evhtp_parser_new(void) {
    return malloc(sizeof(evhtp_parser));
}

size_t
evhtp_parser_run(evhtp_parser * p, evhtp_parser_hooks * hooks, const char * data, size_t len) {
    unsigned char ch;
    char          c;
    size_t        i;

    evhtp_parser_log_debug("enter");
    evhtp_parser_log_debug("p == %p", p);

    p->error      = evhtp_parser_error_none;
    p->bytes_read = 0;

    for (i = 0; i < len; i++) {
        int res;
        int err;

        ch = data[i];

        evhtp_parser_log_debug("[%p] data[%d] = %c (%x)", p, i, isprint(ch) ? ch : ' ', ch);

        if (p->buf_idx >= sizeof(p->buf)) {
            p->error = evhtp_parser_error_too_big;
            return i + 1;
        }

        p->total_bytes_read += 1;
        p->bytes_read       += 1;

        switch (p->state) {
            case s_start:
                evhtp_parser_log_debug("[%p] s_start", p);

                p->flags            = 0;
                p->error            = evhtp_parser_error_none;
                p->method           = evhtp_method_UNKNOWN;
                p->multipart        = 0;
                p->major            = 0;
                p->minor            = 0;
                p->content_len      = 0;
                p->orig_content_len = 0;
                p->status           = 0;
                p->status_count     = 0;
                p->scheme_offset    = NULL;
                p->host_offset      = NULL;
                p->port_offset      = NULL;
                p->path_offset      = NULL;
                p->args_offset      = NULL;


                if (ch == CR || ch == LF) {
                    break;
                }

                if ((ch < 'A' || ch > 'Z') && ch != '_') {
                    p->error = evhtp_parser_error_inval_reqline;
                    return i + 1;
                }

                res = hook_on_msg_begin_run(p, hooks);

                p->buf[p->buf_idx++] = ch;
                p->buf[p->buf_idx]   = '\0';

                if (p->type == evhtp_parser_type_request) {
                    p->state = s_method;
                } else if (p->type == evhtp_parser_type_response && ch == 'H') {
                    p->state = s_http_H;
                } else {
                    p->error = evhtp_parser_error_inval_reqline;
                    return i + 1;
                }

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_method:
                evhtp_parser_log_debug("[%p] s_method", p);

                if (ch == ' ') {
                    char * m = p->buf;

                    switch (p->buf_idx) {
                        case 3:
                            if (_str3_cmp(m, 'G', 'E', 'T', '\0')) {
                                p->method = evhtp_method_GET;
                                break;
                            }

                            if (_str3_cmp(m, 'P', 'U', 'T', '\0')) {
                                p->method = evhtp_method_PUT;
                                break;
                            }

                            break;
                        case 4:
                            if (m[1] == 'O') {
                                if (_str3Ocmp(m, 'P', 'O', 'S', 'T')) {
                                    p->method = evhtp_method_POST;
                                    break;
                                }

                                if (_str3Ocmp(m, 'C', 'O', 'P', 'Y')) {
                                    p->method = evhtp_method_COPY;
                                    break;
                                }

                                if (_str3Ocmp(m, 'M', 'O', 'V', 'E')) {
                                    p->method = evhtp_method_MOVE;
                                    break;
                                }

                                if (_str3Ocmp(m, 'L', 'O', 'C', 'K')) {
                                    p->method = evhtp_method_LOCK;
                                    break;
                                }
                            } else {
                                if (_str4cmp(m, 'H', 'E', 'A', 'D')) {
                                    p->method = evhtp_method_HEAD;
                                    break;
                                }
                            }
                            break;
                        case 5:
                            if (_str5cmp(m, 'M', 'K', 'C', 'O', 'L')) {
                                p->method = evhtp_method_MKCOL;
                                break;
                            }

                            if (_str5cmp(m, 'T', 'R', 'A', 'C', 'E')) {
                                p->method = evhtp_method_TRACE;
                                break;
                            }

                            if (_str5cmp(m, 'P', 'A', 'T', 'C', 'H')) {
                                p->method = evhtp_method_PATCH;
                                break;
                            }
                            break;
                        case 6:
                            if (_str6cmp(m, 'D', 'E', 'L', 'E', 'T', 'E')) {
                                p->method = evhtp_method_DELETE;
                                break;
                            }

                            if (_str6cmp(m, 'U', 'N', 'L', 'O', 'C', 'K')) {
                                p->method = evhtp_method_UNLOCK;
                                break;
                            }
                            break;
                        case 7:
                            if (_str7_cmp(m, 'O', 'P', 'T', 'I', 'O', 'N', 'S', '\0')) {
                                p->method = evhtp_method_OPTIONS;
                            }

                            if (_str7_cmp(m, 'C', 'O', 'N', 'N', 'E', 'C', 'T', '\0')) {
                                p->method = evhtp_method_CONNECT;
                            }
                            break;
                        case 8:
                            if (_str8cmp(m, 'P', 'R', 'O', 'P', 'F', 'I', 'N', 'D')) {
                                p->method = evhtp_method_PROPFIND;
                            }

                            break;

                        case 9:
                            if (_str9cmp(m, 'P', 'R', 'O', 'P', 'P', 'A', 'T', 'C', 'H')) {
                                p->method = evhtp_method_PROPPATCH;
                            }
                            break;
                    } /* switch */

                    res        = hook_method_run(p, hooks, p->buf, p->buf_idx);
                    p->buf_idx = 0;
                    p->state   = s_spaces_before_uri;

                    if (res) {
                        p->error = evhtp_parser_error_user;
                        return i + 1;
                    }

                    break;
                }

                if ((ch < 'A' || ch > 'Z') && ch != '_') {
                    p->error = evhtp_parser_error_inval_method;
                    return i + 1;
                }

                p->buf[p->buf_idx++] = ch;
                p->buf[p->buf_idx]   = '\0';

                break;
            case s_spaces_before_uri:
                evhtp_parser_log_debug("[%p] s_spaces_before_uri", p);

                switch (ch) {
                    case ' ':
                        break;
                    case '/':
                        p->path_offset       = &p->buf[p->buf_idx];

                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->state = s_after_slash_in_uri;
                        break;
                    default:
                        c        = (unsigned char)(ch | 0x20);

                        if (c >= 'a' && c <= 'z') {
                            p->scheme_offset     = &p->buf[p->buf_idx];
                            p->buf[p->buf_idx++] = ch;
                            p->buf[p->buf_idx]   = '\0';
                            p->state = s_schema;
                            break;
                        }

                        p->error = evhtp_parser_error_inval_reqline;
                        return i + 1;
                } /* switch */

                break;
            case s_schema:
                evhtp_parser_log_debug("[%p] s_schema", p);

                c = (unsigned char)(ch | 0x20);

                if (c >= 'a' && c <= 'z') {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                switch (ch) {
                    case ':':
                        p->scheme = evhtp_parser_scheme_unknown;

                        switch (p->buf_idx) {
                            case 3:
                                if (_str3_cmp(p->scheme_offset, 'f', 't', 'p', '\0')) {
                                    p->scheme = evhtp_parser_scheme_ftp;
                                    break;
                                }

                                if (_str3_cmp(p->scheme_offset, 'n', 'f', 's', '\0')) {
                                    p->scheme = evhtp_parser_scheme_nfs;
                                    break;
                                }

                                break;
                            case 4:
                                if (_str4cmp(p->scheme_offset, 'h', 't', 't', 'p')) {
                                    p->scheme = evhtp_parser_scheme_http;
                                    break;
                                }
                                break;
                            case 5:
                                if (_str5cmp(p->scheme_offset, 'h', 't', 't', 'p', 's')) {
                                    p->scheme = evhtp_parser_scheme_https;
                                    break;
                                }
                                break;
                        } /* switch */

                        res = hook_scheme_run(p, hooks, p->scheme_offset,
                                              (&p->buf[p->buf_idx] - p->scheme_offset));

                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state = s_schema_slash;

                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }

                        break;
                    default:
                        p->error = evhtp_parser_error_inval_schema;
                        return i + 1;
                } /* switch */

                break;
            case s_schema_slash:
                evhtp_parser_log_debug("[%p] s_schema_slash", p);

                switch (ch) {
                    case '/':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state = s_schema_slash_slash;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_schema;
                        return i + 1;
                }
                break;
            case s_schema_slash_slash:
                evhtp_parser_log_debug("[%p] s_schema_slash_slash", p);

                switch (ch) {
                    case '/':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->host_offset       = &p->buf[p->buf_idx];

                        p->state = s_host;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_schema;
                        return i + 1;
                }
                break;
            case s_host:
                if (ch == '[') {
                    /* Literal IPv6 address start. */
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    p->host_offset       = &p->buf[p->buf_idx];

                    p->state = s_host_ipv6;
                    break;
                }
                c = (unsigned char)(ch | 0x20);

                if (c >= 'a' && c <= 'z') {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                res = hook_host_run(p, hooks, p->host_offset,
                                    (&p->buf[p->buf_idx] - p->host_offset));

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

            /* successfully parsed a NON-IPV6 hostname, knowing this, the
             * current character in 'ch' is actually the next state, so we
             * we fall through to avoid another loop.
             */
            case s_host_done:
                res = 0;

                switch (ch) {
                    case ':':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->port_offset       = &p->buf[p->buf_idx];
                        p->state = s_port;
                        break;
                    case ' ':
                        /* this technically should never happen, but we should
                         * check anyway
                         */
                        if (i == 0) {
                            p->error = evhtp_parser_error_inval_state;
                            return i + 1;
                        }

                        i--;
                        ch = '/';
                    /* to accept requests like <method> <proto>://<host> <ver>
                     * we fallthrough to the next case.
                     */
                    case '/':
                        p->path_offset       = &p->buf[p->buf_idx];

                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state = s_after_slash_in_uri;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_schema;
                        return i + 1;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_host_ipv6:
                c = (unsigned char)(ch | 0x20);

                if ((c >= 'a' && c <= 'f') ||
                    (ch >= '0' && ch <= '9') || ch == ':' || ch == '.') {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                switch (ch) {
                    case ']':
                        res = hook_host_run(p, hooks, p->host_offset,
                                            (&p->buf[p->buf_idx] - p->host_offset));
                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->state = s_host_done;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_schema;
                        return i + 1;
                }
                break;
            case s_port:
                res = 0;

                if (ch >= '0' && ch <= '9') {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                res = hook_port_run(p, hooks, p->port_offset,
                                    (&p->buf[p->buf_idx] - p->port_offset));

                switch (ch) {
                    case ' ':
                        /* this technically should never happen, but we should
                         * check anyway
                         */
                        if (i == 0) {
                            p->error = evhtp_parser_error_inval_state;
                            return i + 1;
                        }

                        i--;
                        ch = '/';
                    /* to accept requests like <method> <proto>://<host> <ver>
                     * we fallthrough to the next case.
                     */
                    case '/':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->path_offset       = &p->buf[p->buf_idx - 1];

                        p->state = s_after_slash_in_uri;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_reqline;
                        return i + 1;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_after_slash_in_uri:
                evhtp_parser_log_debug("[%p] s_after_slash_in_uri", p);

                res = 0;

                if (usual[ch >> 5] & (1 << (ch & 0x1f))) {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    p->state = s_check_uri;
                    break;
                }

                switch (ch) {
                    case ' ':
                    {
                        int r1 = hook_path_run(p, hooks, p->path_offset,
                                               (&p->buf[p->buf_idx] - p->path_offset));
                        int r2 = hook_uri_run(p, hooks, p->buf, p->buf_idx);

                        p->state   = s_http_09;
                        p->buf_idx = 0;

                        if (r1 || r2) {
                            res = 1;
                        }
                    }

                    break;
                    case CR:
                        p->minor = 9;
                        p->state = s_almost_done;
                        break;
                    case LF:
                        p->minor = 9;
                        p->state = s_hdrline_start;
                        break;
                    case '.':
                    case '%':
                    case '/':
                    case '#':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->state             = s_uri;
                        break;
                    case '?':
                        res                  = hook_path_run(p, hooks, p->path_offset,
                                                             (&p->buf[p->buf_idx] - p->path_offset));

                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->args_offset       = &p->buf[p->buf_idx];
                        p->state             = s_uri;

                        break;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state             = s_check_uri;
                        break;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_check_uri:
                evhtp_parser_log_debug("[%p] s_check_uri", p);

                res = 0;

                if (usual[ch >> 5] & (1 << (ch & 0x1f))) {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                switch (ch) {
                    case ' ':
                    {
                        int r1 = 0;
                        int r2 = 0;

                        if (p->args_offset) {
                            r1 = hook_args_run(p, hooks, p->args_offset,
                                               (&p->buf[p->buf_idx] - p->args_offset));
                        } else {
                            r1 = hook_path_run(p, hooks, p->path_offset,
                                               (&p->buf[p->buf_idx] - p->path_offset));
                        }

                        r2         = hook_uri_run(p, hooks, p->buf, p->buf_idx);
                        p->buf_idx = 0;
                        p->state   = s_http_09;

                        if (r1 || r2) {
                            res = 1;
                        }
                    }
                    break;
                    case '/':
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->state = s_after_slash_in_uri;
                        break;
                    case CR:
                        p->minor = 9;
                        p->buf_idx           = 0;
                        p->state = s_almost_done;
                        break;
                    case LF:
                        p->minor = 9;
                        p->buf_idx           = 0;

                        p->state             = s_hdrline_start;
                        break;
                    case '?':
                        res                  = hook_path_run(p, hooks,
                                                             p->path_offset,
                                                             (&p->buf[p->buf_idx] - p->path_offset));

                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->args_offset       = &p->buf[p->buf_idx];
                        p->state             = s_uri;
                        break;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state             = s_uri;
                        break;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_uri:
                evhtp_parser_log_debug("[%p] s_uri", p);

                res = 0;

                if (usual[ch >> 5] & (1 << (ch & 0x1f))) {
                    p->buf[p->buf_idx++] = ch;
                    p->buf[p->buf_idx]   = '\0';
                    break;
                }

                switch (ch) {
                    case ' ':
                    {
                        int r1 = 0;
                        int r2 = 0;

                        if (p->args_offset) {
                            r1 = hook_args_run(p, hooks, p->args_offset,
                                               (&p->buf[p->buf_idx] - p->args_offset));
                        } else {
                            r1 = hook_path_run(p, hooks, p->path_offset,
                                               (&p->buf[p->buf_idx] - p->path_offset));
                        }

                        p->buf_idx = 0;
                        p->state   = s_http_09;

                        if (r1 || r2) {
                            res = 1;
                        }
                    }
                    break;
                    case CR:
                        p->minor   = 9;
                        p->buf_idx = 0;
                        p->state   = s_almost_done;
                        break;
                    case LF:
                        p->minor   = 9;
                        p->buf_idx = 0;
                        p->state   = s_hdrline_start;
                        break;
                    case '?':
                        /* RFC 3986 section 3.4:
                         * The query component is indicated by the
                         * first question mark ("?") character and
                         * terminated by a number sign ("#") character
                         * or by the end of the URI. */
                        if (!p->args_offset) {
                            res = hook_path_run(p, hooks, p->path_offset,
                                                (&p->buf[p->buf_idx] - p->path_offset));

                            p->buf[p->buf_idx++] = ch;
                            p->buf[p->buf_idx]   = '\0';
                            p->args_offset       = &p->buf[p->buf_idx];
                            break;
                        }
                    /* Fall through. */
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        break;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_http_09:
                evhtp_parser_log_debug("[%p] s_http_09", p);

                switch (ch) {
                    case ' ':
                        break;
                    case CR:
                        p->minor   = 9;
                        p->buf_idx = 0;
                        p->state   = s_almost_done;
                        break;
                    case LF:
                        p->minor   = 9;
                        p->buf_idx = 0;
                        p->state   = s_hdrline_start;
                        break;
                    case 'H':
                        p->buf_idx = 0;
                        p->state   = s_http_H;
                        break;
                    default:
                        p->error   = evhtp_parser_error_inval_proto;
                        return i + 1;
                } /* switch */

                break;
            case s_http_H:
                evhtp_parser_log_debug("[%p] s_http_H", p);

                switch (ch) {
                    case 'T':
                        p->state = s_http_HT;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_proto;
                        return i + 1;
                }
                break;
            case s_http_HT:
                switch (ch) {
                    case 'T':
                        p->state = s_http_HTT;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_proto;
                        return i + 1;
                }
                break;
            case s_http_HTT:
                switch (ch) {
                    case 'P':
                        p->state = s_http_HTTP;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_proto;
                        return i + 1;
                }
                break;
            case s_http_HTTP:
                switch (ch) {
                    case '/':
                        p->state = s_first_major_digit;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_proto;
                        return i + 1;
                }
                break;
            case s_first_major_digit:
                if (ch < '1' || ch > '9') {
                    p->error = evhtp_parser_error_inval_ver;
                    return i + 1;
                }

                p->major = ch - '0';
                p->state = s_major_digit;
                break;
            case s_major_digit:
                if (ch == '.') {
                    p->state = s_first_minor_digit;
                    break;
                }

                if (ch < '0' || ch > '9') {
                    p->error = evhtp_parser_error_inval_ver;
                    return i + 1;
                }

                p->major = p->major * 10 + ch - '0';
                break;
            case s_first_minor_digit:
                if (ch < '0' || ch > '9') {
                    p->error = evhtp_parser_error_inval_ver;
                    return i + 1;
                }

                p->minor = ch - '0';
                p->state = s_minor_digit;
                break;
            case s_minor_digit:
                switch (ch) {
                    case ' ':
                        if (p->type == evhtp_parser_type_request) {
                            p->state = s_spaces_after_digit;
                        } else if (p->type == evhtp_parser_type_response) {
                            p->state = s_status;
                        }

                        break;
                    case CR:
                        p->state = s_almost_done;
                        break;
                    case LF:
                        /* LF without a CR? error.... */
                        p->error = evhtp_parser_error_inval_reqline;
                        return i + 1;
                    default:
                        if (ch < '0' || ch > '9') {
                            p->error = evhtp_parser_error_inval_ver;
                            return i + 1;
                        }

                        p->minor = p->minor * 10 + ch - '0';
                        break;
                } /* switch */
                break;
            case s_status:
                /* http response status code */
                if (ch == ' ') {
                    if (p->status) {
                        p->state = s_status_text;
                    }
                    break;
                }

                if (ch < '0' || ch > '9') {
                    p->error = evhtp_parser_error_status;
                    return i + 1;
                }

                p->status = p->status * 10 + ch - '0';

                if (++p->status_count == 3) {
                    p->state = s_space_after_status;
                }

                break;
            case s_space_after_status:
                switch (ch) {
                    case ' ':
                        p->state = s_status_text;
                        break;
                    case CR:
                        p->state = s_almost_done;
                        break;
                    case LF:
                        p->state = s_hdrline_start;
                        break;
                    default:
                        p->error = evhtp_parser_error_generic;
                        return i + 1;
                }
                break;
            case s_status_text:
                switch (ch) {
                    case CR:
                        p->state = s_almost_done;
                        break;
                    case LF:
                        p->state = s_hdrline_start;
                        break;
                    default:
                        break;
                }
                break;
            case s_spaces_after_digit:
                switch (ch) {
                    case ' ':
                        break;
                    case CR:
                        p->state = s_almost_done;
                        break;
                    case LF:
                        p->state = s_hdrline_start;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_ver;
                        return i + 1;
                }
                break;

            case s_almost_done:
                switch (ch) {
                    case LF:
                        if (p->type == evhtp_parser_type_response && p->status >= 100 && p->status < 200) {
                            res = hook_on_hdrs_begin_run(p, hooks);

                            if (res) {
                                p->error = evhtp_parser_error_user;
                                return i + 1;
                            }

                            p->status       = 0;
                            p->status_count = 0;
                            p->state        = s_start;
                            break;
                        }

                        p->state = s_done;
                        res      = hook_on_hdrs_begin_run(p, hooks);
                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_reqline;
                        return i + 1;
                } /* switch */
                break;
            case s_done:
                switch (ch) {
                    case CR:
                        p->state = s_hdrline_almost_done;
                        break;
                    case LF:
                        return i + 1;
                    default:
                        goto hdrline_start;
                }
                break;
hdrline_start:
            case s_hdrline_start:
                evhtp_parser_log_debug("[%p] s_hdrline_start", p);

                p->buf_idx = 0;

                switch (ch) {
                    case CR:
                        p->state             = s_hdrline_hdr_almost_done;
                        break;
                    case LF:
                        p->state             = s_hdrline_hdr_done;
                        break;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state             = s_hdrline_hdr_key;
                        break;
                }

                break;
            case s_hdrline_hdr_key:
                evhtp_parser_log_debug("[%p] s_hdrline_hdr_key", p);

                res = 0;
                switch (ch) {
                    case ':':
                        res      = hook_hdr_key_run(p, hooks, p->buf, p->buf_idx);

                        /* figure out if the value of this header is valueable */
                        p->heval = eval_hdr_val_none;

                        switch (p->buf_idx + 1) {
                            case 5:
                                if (!strcasecmp(p->buf, "host")) {
                                    p->heval = eval_hdr_val_hostname;
                                }
                                break;
                            case 11:
                                if (!strcasecmp(p->buf, "connection")) {
                                    p->heval = eval_hdr_val_connection;
                                }
                                break;
                            case 13:
                                if (!strcasecmp(p->buf, "content-type")) {
                                    p->heval = eval_hdr_val_content_type;
                                }
                                break;
                            case 15:
                                if (!strcasecmp(p->buf, "content-length")) {
                                    p->heval = eval_hdr_val_content_length;
                                }
                                break;
                            case 17:
                                if (!strcasecmp(p->buf, "proxy-connection")) {
                                    p->heval = eval_hdr_val_proxy_connection;
                                }
                                break;
                            case 18:
                                if (!strcasecmp(p->buf, "transfer-encoding")) {
                                    p->heval = eval_hdr_val_transfer_encoding;
                                }
                                break;
                        } /* switch */

                        p->buf_idx           = 0;
                        p->state             = s_hdrline_hdr_space_before_val;

                        break;
                    case CR:
                        p->state             = s_hdrline_hdr_almost_done;
                        break;
                    case LF:
                        p->state             = s_hdrline_hdr_done;
                        break;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        break;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_hdrline_hdr_space_before_val:
                evhtp_parser_log_debug("[%p] s_hdrline_hdr_space_before_val", p);

                switch (ch) {
                    case ' ':
                        break;
                    case CR:
                        /*
                         * we have an empty header value here, so we set the buf
                         * to empty, set the state to hdrline_hdr_val, and
                         * decrement the start byte counter.
                         */
                        p->buf[p->buf_idx++] = ' ';
                        p->buf[p->buf_idx]   = '\0';
                        p->state = s_hdrline_hdr_val;

                        /*
                         * make sure the next pass comes back to this CR byte,
                         * so it matches in s_hdrline_hdr_val.
                         */
                        i--;
                        break;
                    case LF:
                        /* never got a CR for an empty header, this is an
                         * invalid state.
                         */
                        p->error             = evhtp_parser_error_inval_hdr;
                        return i + 1;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        p->state             = s_hdrline_hdr_val;
                        break;
                } /* switch */
                break;
            case s_hdrline_hdr_val:
                evhtp_parser_log_debug("[%p] s_hdrline_hdr_val", p);
                err = 0;
                res = 0;

                switch (ch) {
                    case CR:
                        switch (p->heval) {
                            case eval_hdr_val_none:
                                break;
                            case eval_hdr_val_hostname:
                                res = hook_hostname_run(p, hooks,
                                                        p->buf, p->buf_idx);
                                break;
                            case eval_hdr_val_content_length:
                                p->content_len      = str_to_uint64(p->buf, p->buf_idx, &err);
                                p->orig_content_len = p->content_len;

                                evhtp_parser_log_debug("[%p] s_hdrline_hdr_val content-lenth = %zu",
                                                       p, p->content_len);

                                if (err == 1) {
                                    p->error = evhtp_parser_error_too_big;
                                    return i + 1;
                                }

                                break;
                            case eval_hdr_val_connection:
                                switch (p->buf[0]) {
                                    case 'K':
                                    case 'k':
                                        if (p->buf_idx != 10) {
                                            break;
                                        }

                                        if (_str9cmp((p->buf + 1),
                                                     'e', 'e', 'p', '-', 'A', 'l', 'i', 'v', 'e')) {
                                            p->flags |= parser_flag_connection_keep_alive;
                                        }
                                        break;
                                    case 'c':
                                    case 'C':
                                        if (p->buf_idx != 5) {
                                            break;
                                        }

                                        if (_str5cmp(p->buf, 'c', 'l', 'o', 's', 'e')) {
                                            p->flags |= parser_flag_connection_close;
                                        }
                                        break;
                                } /* switch */
                                break;
                            case eval_hdr_val_transfer_encoding:
                                if (p->buf_idx != 7) {
                                    break;
                                }

                                switch (p->buf[0]) {
                                    case 'c':
                                    case 'C':
                                        if (p->buf_idx != 7) {
                                            break;
                                        }

                                        if (_str6cmp((p->buf + 1), 'h', 'u', 'n', 'k', 'e', 'd')) {
                                            p->flags |= parser_flag_chunked;
                                        }

                                        break;
                                }

                                break;
                            case eval_hdr_val_content_type:
                                if (p->buf_idx != 9) {
                                    break;
                                }

                                switch (p->buf[0]) {
                                    case 'm':
                                    case 'M':
                                        if (_str8cmp((p->buf + 1), 'u', 'l', 't', 'i', 'p', 'a', 'r', 't')) {
                                            p->multipart = 1;
                                        }

                                        break;
                                }

                                break;
                            case eval_hdr_val_proxy_connection:
                            default:
                                break;
                        } /* switch */

                        p->state             = s_hdrline_hdr_almost_done;
                        break;
                    case LF:
                        /* LF before CR? invalid */
                        p->error             = evhtp_parser_error_inval_hdr;
                        return i + 1;
                    default:
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';
                        break;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_hdrline_hdr_almost_done:
                evhtp_parser_log_debug("[%p] s_hdrline_hdr_almost_done", p);

                res = 0;
                switch (ch) {
                    case LF:
                        if (p->flags & parser_flag_trailing) {
                            res      = hook_on_msg_complete_run(p, hooks);
                            p->state = s_start;
                            break;
                        }

                        p->state = s_hdrline_hdr_done;
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_hdr;
                        return i + 1;
                }

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_hdrline_hdr_done:
                evhtp_parser_log_debug("[%p] s_hdrline_hdr_done", p);

                switch (ch) {
                    case CR:
                        res      = hook_hdr_val_run(p, hooks, p->buf, p->buf_idx);
                        p->state = s_hdrline_almost_done;

                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }

                        res = hook_on_hdrs_complete_run(p, hooks);

                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }

                        break;
                    case LF:
                        /* got LFLF? is this valid? */
                        p->error             = evhtp_parser_error_inval_hdr;

                        return i + 1;
                    case '\t':
                        /* this is a multiline header value, we must go back to
                         * reading as a header value */
                        p->state             = s_hdrline_hdr_val;
                        break;
                    default:
                        res                  = hook_hdr_val_run(p, hooks, p->buf, p->buf_idx);

                        p->buf_idx           = 0;
                        p->buf[p->buf_idx++] = ch;
                        p->buf[p->buf_idx]   = '\0';

                        p->state             = s_hdrline_hdr_key;

                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }

                        break;
                } /* switch */
                break;
            case s_hdrline_almost_done:
                evhtp_parser_log_debug("[%p] s_hdrline_almost_done", p);

                res = 0;

                switch (ch) {
                    case LF:
                        p->buf_idx = 0;
                        evhtp_parser_log_debug("[%p] HERE", p);

                        if (p->flags & parser_flag_trailing) {
                            res      = hook_on_msg_complete_run(p, hooks);
                            p->state = s_start;
                        } else if (p->flags & parser_flag_chunked) {
                            p->state = s_chunk_size_start;
                        } else if (p->content_len > 0) {
                            p->state = s_body_read;
                        } else if (p->content_len == 0) {
                            res      = hook_on_msg_complete_run(p, hooks);
                            p->state = s_start;
                        } else {
                            p->state = s_hdrline_done;
                        }

                        if (res) {
                            p->error = evhtp_parser_error_user;
                            return i + 1;
                        }
                        break;
                    default:
                        p->error = evhtp_parser_error_inval_hdr;
                        return i + 1;
                } /* switch */

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_hdrline_done:
                evhtp_parser_log_debug("[%p] s_hdrline_done", p);

                res = 0;

                if (p->flags & parser_flag_trailing) {
                    res      = hook_on_msg_complete_run(p, hooks);
                    p->state = s_start;
                    break;
                } else if (p->flags & parser_flag_chunked) {
                    p->state = s_chunk_size_start;
                    i--;
                } else if (p->content_len > 0) {
                    p->state = s_body_read;
                    i--;
                } else if (p->content_len == 0) {
                    res      = hook_on_msg_complete_run(p, hooks);
                    p->state = s_start;
                }
                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;
            case s_chunk_size_start:
                c = unhex[(unsigned char)ch];

                if (c == -1) {
                    p->error = evhtp_parser_error_inval_chunk_sz;
                    return i + 1;
                }

                p->content_len = c;
                p->state       = s_chunk_size;
                break;
            case s_chunk_size:
                if (ch == CR) {
                    p->state = s_chunk_size_almost_done;
                    break;
                }

                c = unhex[(unsigned char)ch];

                if (c == -1) {
                    p->error = evhtp_parser_error_inval_chunk_sz;
                    return i + 1;
                }

                p->content_len *= 16;
                p->content_len += c;
                break;

            case s_chunk_size_almost_done:
                res = 0;

                if (ch != LF) {
                    p->error = evhtp_parser_error_inval_chunk_sz;
                    return i + 1;
                }

                p->orig_content_len = p->content_len;

                if (p->content_len == 0) {
                    res       = hook_on_chunks_complete_run(p, hooks);

                    p->flags |= parser_flag_trailing;
                    p->state  = s_hdrline_start;
                } else {
                    res      = hook_on_new_chunk_run(p, hooks);

                    p->state = s_chunk_data;
                }

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_chunk_data:
                res = 0;
                {
                    const char * pp      = &data[i];
                    const char * pe      = (const char *)(data + len);
                    size_t       to_read = _MIN_READ(pe - pp, p->content_len);

                    if (to_read > 0) {
                        res = hook_body_run(p, hooks, pp, to_read);

                        i  += to_read - 1;
                    }

                    if (to_read == p->content_len) {
                        p->state = s_chunk_data_almost_done;
                    }

                    p->content_len -= to_read;
                }

                if (res) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_chunk_data_almost_done:
                if (ch != CR) {
                    p->error = evhtp_parser_error_inval_chunk;
                    return i + 1;
                }

                p->state = s_chunk_data_done;
                break;

            case s_chunk_data_done:
                if (ch != LF) {
                    p->error = evhtp_parser_error_inval_chunk;
                    return i + 1;
                }

                p->orig_content_len = 0;
                p->state = s_chunk_size_start;

                if (hook_on_chunk_complete_run(p, hooks)) {
                    p->error = evhtp_parser_error_user;
                    return i + 1;
                }

                break;

            case s_body_read:
                res = 0;

                {
                    const char * pp      = &data[i];
                    const char * pe      = (const char *)(data + len);
                    size_t       to_read = _MIN_READ(pe - pp, p->content_len);

                    if (to_read > 0) {
                        res = hook_body_run(p, hooks, pp, to_read);

                        i  += to_read - 1;
                        p->content_len -= to_read;
                    }

                    if (p->content_len == 0) {
                        res      = hook_on_msg_complete_run(p, hooks);
                        p->state = s_start;
                    }

                    if (res) {
                        p->error = evhtp_parser_error_user;
                        return i + 1;
                    }
                }

                break;

            default:
                evhtp_parser_log_debug("[%p] This is a silly state....", p);
                p->error = evhtp_parser_error_inval_state;
                return i + 1;
        } /* switch */
    }

    return i;
}         /* evhtp_parser_run */