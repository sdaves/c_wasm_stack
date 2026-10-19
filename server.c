#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "db.h"

#ifndef PORT
#define PORT 8080
#endif
#define MAX_EVENTS 64
#define BUFFER_SIZE 8192
#define MAX_RECORDS 4096
#define MAX_USERNAME 31

static const char HEALTH_TEXT[] = "C/WASM stack ready. Try /api/users";
static const char BENCH_TEXT[] = "BENCH_OK";

static FILE *db_file = NULL;
static DBHeader db_header;
static int epoll_fd_global = -1;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct {
    int fd;
    size_t length;
    char buffer[BUFFER_SIZE];
    char *response;
    size_t response_length;
    size_t response_sent;
    int writing;
} Connection;

/* ------------------------------------------------------------------ */
/* Growable output buffer                                              */
/* ------------------------------------------------------------------ */

static int buffer_reserve(Buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) return -1;
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return 0;

    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    char *data = realloc(buffer->data, capacity);
    if (!data) return -1;
    buffer->data = data;
    buffer->capacity = capacity;
    if (buffer->length == 0) buffer->data[0] = '\0';
    return 0;
}

static int buffer_append(Buffer *buffer, const char *data, size_t length) {
    if (length == 0) return 0;
    if (buffer_reserve(buffer, length) != 0) return -1;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 0;
}

static int buffer_printf(Buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return -1;
    }
    if (buffer_reserve(buffer, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(buffer->data + buffer->length,
              buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t)needed;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Flat-file database                                                 */
/* ------------------------------------------------------------------ */

static const char *db_path(void) {
    const char *path = getenv("DB_PATH");
    return (path && *path) ? path : DB_FILE;
}

static int load_header(FILE *file, DBHeader *header) {
    if (fread(header, sizeof(*header), 1, file) != 1) return -1;
    if (header->magic != DB_MAGIC || header->version != DB_VERSION) return -1;
    if (header->record_count > MAX_RECORDS) return -1;

    long position = ftell(file);
    if (position < 0 || fseek(file, 0, SEEK_END) != 0) return -1;
    long size = ftell(file);
    long expected = (long)(sizeof(DBHeader) +
                           header->record_count * sizeof(UserRecord));
    if (size < expected) return -1;
    return fseek(file, position, SEEK_SET) == 0 ? 0 : -1;
}

static void create_db(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "Cannot create database %s: %s\n",
                path, strerror(errno));
        return;
    }

    DBHeader header = { DB_MAGIC, DB_VERSION, 2 };
    UserRecord users[2] = {
        { 1, "Alice", 5000 },
        { 2, "Bob", 7500 }
    };

    if (fwrite(&header, sizeof(header), 1, file) != 1 ||
        fwrite(users, sizeof(UserRecord), 2, file) != 2) {
        fprintf(stderr, "Failed writing database %s: %s\n",
                path, strerror(errno));
    }
    fclose(file);
    printf("Created new database: %s\n", path);
}

static void init_or_load_db(void) {
    const char *path = db_path();
    db_file = fopen(path, "rb+");
    if (!db_file) {
        create_db(path);
        db_file = fopen(path, "rb+");
    }

    if (!db_file || load_header(db_file, &db_header) != 0) {
        if (db_file) {
            fclose(db_file);
            db_file = NULL;
            fprintf(stderr, "Invalid database at %s; recreating\n", path);
            create_db(path);
            db_file = fopen(path, "rb+");
        }
        if (!db_file || load_header(db_file, &db_header) != 0) {
            fprintf(stderr, "Cannot open database %s: %s\n",
                    path, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    printf("Loaded DB v%u, Records: %llu, Path: %s\n",
           db_header.version,
           (unsigned long long)db_header.record_count, path);
}

static int append_user(const char *username, uint32_t balance, uint64_t *id_out) {
    if (db_header.record_count >= MAX_RECORDS) return -2;

    uint64_t id = 1;
    if (db_header.record_count > 0) {
        UserRecord last;
        long offset = (long)(sizeof(DBHeader) +
                             (db_header.record_count - 1) * sizeof(UserRecord));
        if (fseek(db_file, offset, SEEK_SET) != 0 ||
            fread(&last, sizeof(last), 1, db_file) != 1) {
            return -1;
        }
        id = last.id + 1;
    }

    UserRecord record;
    memset(&record, 0, sizeof(record));
    record.id = id;
    snprintf(record.username, sizeof(record.username), "%s", username);
    record.balance = balance;

    long write_offset = (long)(sizeof(DBHeader) +
                               db_header.record_count * sizeof(UserRecord));
    if (fseek(db_file, write_offset, SEEK_SET) != 0 ||
        fwrite(&record, sizeof(record), 1, db_file) != 1) {
        return -1;
    }

    DBHeader header = { DB_MAGIC, DB_VERSION, db_header.record_count + 1 };
    if (fseek(db_file, 0, SEEK_SET) != 0 ||
        fwrite(&header, sizeof(header), 1, db_file) != 1) {
        return -1;
    }
    fflush(db_file);
    db_header = header;
    if (id_out) *id_out = id;
    return 0;
}

/* Keeps the load-test endpoint meaningful: bump the two seed accounts. */
static void increment_balances(void) {
    uint64_t updates = db_header.record_count < 2 ? db_header.record_count : 2;
    if (updates == 0) return;

    static const uint32_t deltas[2] = { 10, 20 };
    UserRecord records[2];
    long base = (long)sizeof(DBHeader);

    if (fseek(db_file, base, SEEK_SET) != 0 ||
        fread(records, sizeof(UserRecord), (size_t)updates, db_file) != updates) {
        return;
    }
    for (uint64_t i = 0; i < updates; i++) {
        records[i].balance += deltas[i];
    }
    if (fseek(db_file, base, SEEK_SET) == 0) {
        fwrite(records, sizeof(UserRecord), (size_t)updates, db_file);
        fflush(db_file);
    }
}

/* ------------------------------------------------------------------ */
/* Request parsing helpers                                            */
/* ------------------------------------------------------------------ */

static size_t parse_content_length(const char *data, size_t header_length) {
    size_t position = 0;
    while (position < header_length) {
        const char *line_end = memmem(data + position, header_length - position,
                                      "\r\n", 2);
        if (!line_end) break;
        size_t line_length = (size_t)(line_end - (data + position));
        if (line_length >= 15 &&
            strncasecmp(data + position, "Content-Length:", 15) == 0) {
            errno = 0;
            unsigned long long value = strtoull(data + position + 15, NULL, 10);
            if (errno == ERANGE) return SIZE_MAX;
            return (size_t)value;
        }
        position += line_length + 2;
    }
    return 0;
}

static int request_ready(const Connection *connection) {
    const char *end = memmem(connection->buffer, connection->length,
                             "\r\n\r\n", 4);
    if (!end) return connection->length == sizeof(connection->buffer);

    size_t header_length = (size_t)(end - connection->buffer) + 4;
    size_t content_length = parse_content_length(connection->buffer, header_length);
    if (content_length > sizeof(connection->buffer) - header_length) return 1;
    return connection->length >= header_length + content_length;
}

static int parse_request_line(const char *data, size_t length,
                              char *method, size_t method_capacity,
                              char *path, size_t path_capacity) {
    const char *space1 = memchr(data, ' ', length);
    if (!space1) return -1;
    const char *space2 = memchr(space1 + 1, ' ',
                                length - (size_t)(space1 + 1 - data));
    if (!space2) return -1;

    size_t method_length = (size_t)(space1 - data);
    size_t path_length = (size_t)(space2 - space1 - 1);
    if (method_length == 0 || method_length >= method_capacity ||
        path_length == 0 || path_length >= path_capacity) {
        return -1;
    }

    memcpy(method, data, method_length);
    method[method_length] = '\0';
    memcpy(path, space1 + 1, path_length);
    path[path_length] = '\0';
    return 0;
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *source, size_t length,
                      char *destination, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= length) return -1;
            int high = hex_value((unsigned char)source[i + 1]);
            int low = hex_value((unsigned char)source[i + 2]);
            if (high < 0 || low < 0) return -1;
            c = (unsigned char)((high << 4) | low);
            i += 2;
        }
        if (c == '\0' || out + 1 >= capacity) return -1;
        destination[out++] = (char)c;
    }
    destination[out] = '\0';
    return (int)out;
}

static int form_get(const char *body, size_t length, const char *key,
                    char *out, size_t capacity) {
    size_t key_length = strlen(key);
    size_t position = 0;

    while (position < length) {
        size_t end = position;
        while (end < length && body[end] != '&') end++;

        size_t equals = position;
        while (equals < end && body[equals] != '=') equals++;

        if (equals < end) {
            size_t found_key_length = equals - position;
            if (found_key_length == key_length &&
                memcmp(body + position, key, key_length) == 0) {
                return url_decode(body + equals + 1, end - equals - 1,
                                  out, capacity);
            }
        }

        position = end + (end < length ? 1 : 0);
    }
    return -1;
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static int username_is_valid(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length > MAX_USERNAME) return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int parse_balance(const char *text, uint32_t *out) {
    if (!*text) return -1;
    uint64_t value = 0;
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10) return -1;
        value = value * 10 + digit;
    }
    *out = (uint32_t)value;
    return 0;
}

static void json_escape(const char *source, char *destination, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; source[i] && out + 1 < capacity; i++) {
        unsigned char c = (unsigned char)source[i];
        const char *escape = NULL;
        switch (c) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }

        if (escape) {
            size_t length = strlen(escape);
            if (out + length >= capacity) break;
            memcpy(destination + out, escape, length);
            out += length;
        } else if (c < 0x20) {
            int written = snprintf(destination + out, capacity - out,
                                   "\\u%04x", c);
            if (written < 0 || (size_t)written >= capacity - out) break;
            out += (size_t)written;
        } else {
            destination[out++] = (char)c;
        }
    }
    destination[out] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP responses                                                     */
/* ------------------------------------------------------------------ */

static void close_connection(Connection *connection) {
    if (!connection) return;
    close(connection->fd);
    free(connection->response);
    free(connection);
}

static int flush_response(Connection *connection) {
    while (connection->response_sent < connection->response_length) {
        ssize_t sent = send(connection->fd,
                            connection->response + connection->response_sent,
                            connection->response_length - connection->response_sent,
                            MSG_NOSIGNAL);
        if (sent > 0) {
            connection->response_sent += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    return 1;
}

static int prepare_response(Connection *connection, int status,
                            const char *reason, const char *content_type,
                            const char *body, size_t body_length) {
    Buffer response = { 0 };
    if (buffer_printf(&response,
            "HTTP/1.1 %d %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Cache-Control: no-store\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n", status, reason, content_type, body_length) != 0 ||
        (body_length > 0 &&
         buffer_append(&response, body, body_length) != 0)) {
        free(response.data);
        return -1;
    }

    free(connection->response);
    connection->response = response.data;
    connection->response_length = response.length;
    connection->response_sent = 0;
    return 0;
}

static void start_response(Connection *connection) {
    int result = flush_response(connection);
    if (result != 0) {
        close_connection(connection);
        return;
    }

    connection->writing = 1;
    struct epoll_event event = {
        .events = EPOLLOUT | EPOLLET | EPOLLRDHUP,
        .data.ptr = connection
    };
    if (epoll_ctl(epoll_fd_global, EPOLL_CTL_MOD,
                  connection->fd, &event) != 0) {
        close_connection(connection);
    }
}

static void send_json(Connection *connection, int status, const char *reason,
                      Buffer *body) {
    if (!body->data ||
        prepare_response(connection, status, reason,
                         "application/json; charset=utf-8",
                         body->data, body->length) != 0) {
        free(body->data);
        close_connection(connection);
        return;
    }
    free(body->data);
    body->data = NULL;
    start_response(connection);
}

static void send_error(Connection *connection, int status,
                       const char *reason, const char *message) {
    Buffer body = { 0 };
    if (buffer_printf(&body, "{\"ok\":false,\"error\":\"%s\"}", message) != 0) {
        free(body.data);
        close_connection(connection);
        return;
    }
    send_json(connection, status, reason, &body);
}

static void send_text(Connection *connection, int status,
                      const char *reason, const char *text) {
    if (prepare_response(connection, status, reason,
                         "text/plain; charset=utf-8",
                         text, strlen(text)) != 0) {
        close_connection(connection);
        return;
    }
    start_response(connection);
}

static void send_data(Connection *connection, int status, const char *reason,
                      const char *content_type,
                      const char *data, size_t data_length) {
    if (prepare_response(connection, status, reason,
                         content_type, data, data_length) != 0) {
        close_connection(connection);
        return;
    }
    start_response(connection);
}

static void send_empty(Connection *connection, int status, const char *reason) {
    if (prepare_response(connection, status, reason,
                         "text/plain; charset=utf-8", "", 0) != 0) {
        close_connection(connection);
        return;
    }
    start_response(connection);
}

/* ------------------------------------------------------------------ */
/* Routes                                                             */
/* ------------------------------------------------------------------ */

static void handle_list_users(Connection *connection) {
    Buffer body = { 0 };
    if (buffer_append(&body, "{\"ok\":true,\"users\":[",
                      strlen("{\"ok\":true,\"users\":[")) != 0) {
        send_error(connection, 500, "Internal Server Error", "Out of memory");
        return;
    }

    for (uint64_t i = 0; i < db_header.record_count; i++) {
        UserRecord record;
        long offset = (long)(sizeof(DBHeader) + i * sizeof(UserRecord));
        if (fseek(db_file, offset, SEEK_SET) != 0 ||
            fread(&record, sizeof(record), 1, db_file) != 1) {
            free(body.data);
            send_error(connection, 500, "Internal Server Error",
                       "Could not read database");
            return;
        }

        char escaped[256];
        json_escape(record.username, escaped, sizeof(escaped));
        if (buffer_printf(&body,
                "%s{\"id\":%llu,\"username\":\"%s\",\"balance\":%u}",
                i ? "," : "", (unsigned long long)record.id,
                escaped, record.balance) != 0) {
            free(body.data);
            send_error(connection, 500, "Internal Server Error", "Out of memory");
            return;
        }
    }

    if (buffer_append(&body, "]}", 2) != 0) {
        free(body.data);
        send_error(connection, 500, "Internal Server Error", "Out of memory");
        return;
    }
    send_json(connection, 200, "OK", &body);
}

static void handle_add_user(Connection *connection,
                            const char *request_body, size_t body_length) {
    char username[32];
    char balance_text[32];

    if (form_get(request_body, body_length, "username",
                 username, sizeof(username)) < 0 ||
        form_get(request_body, body_length, "balance",
                 balance_text, sizeof(balance_text)) < 0) {
        send_error(connection, 400, "Bad Request",
                   "username and balance are required");
        return;
    }

    char *name = trim(username);
    if (!username_is_valid(name)) {
        send_error(connection, 400, "Bad Request",
                   "username must be 1-31 printable characters");
        return;
    }

    uint32_t balance;
    if (parse_balance(balance_text, &balance) != 0) {
        send_error(connection, 400, "Bad Request",
                   "balance must be an unsigned 32-bit integer");
        return;
    }

    uint64_t id;
    int result = append_user(name, balance, &id);
    if (result == -2) {
        send_error(connection, 507, "Insufficient Storage",
                   "record limit reached");
        return;
    }
    if (result != 0) {
        send_error(connection, 500, "Internal Server Error",
                   "could not save record");
        return;
    }

    char escaped[256];
    json_escape(name, escaped, sizeof(escaped));
    Buffer body = { 0 };
    if (buffer_printf(&body,
            "{\"ok\":true,\"user\":{\"id\":%llu,\"username\":\"%s\",\"balance\":%u}}",
            (unsigned long long)id, escaped, balance) != 0) {
        free(body.data);
        send_error(connection, 500, "Internal Server Error", "Out of memory");
        return;
    }
    send_json(connection, 201, "Created", &body);
}

/* The WASM frontend asks for the raw record array. This avoids a JSON
 * parser in C and keeps the browser view on the same binary layout. */
static void handle_users_bin(Connection *connection) {
    size_t count = (size_t)db_header.record_count;
    size_t bytes = count * sizeof(UserRecord);
    char *data = NULL;

    if (bytes > 0) {
        data = malloc(bytes);
        if (!data ||
            fseek(db_file, sizeof(DBHeader), SEEK_SET) != 0 ||
            fread(data, sizeof(UserRecord), count, db_file) != count) {
            free(data);
            send_error(connection, 500, "Internal Server Error",
                       "could not read database");
            return;
        }
    }

    send_data(connection, 200, "OK", "application/octet-stream",
              data, bytes);
    free(data);
}

typedef struct {
    const char *path;
    const char *file;
    const char *content_type;
} StaticFile;

static const StaticFile STATIC_FILES[] = {
    { "/",           "index.html", "text/html; charset=utf-8" },
    { "/index.html", "index.html", "text/html; charset=utf-8" },
    { "/ui.js",      "ui.js",      "text/javascript; charset=utf-8" },
    { "/ui.wasm",    "ui.wasm",    "application/wasm" }
};

static void handle_static(Connection *connection, const char *path) {
    for (size_t i = 0; i < sizeof(STATIC_FILES) / sizeof(STATIC_FILES[0]); i++) {
        if (strcmp(path, STATIC_FILES[i].path) != 0) continue;

        FILE *file = fopen(STATIC_FILES[i].file, "rb");
        if (!file) {
            send_error(connection, 404, "Not Found",
                       "frontend asset missing; run make build");
            return;
        }

        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            send_error(connection, 500, "Internal Server Error",
                       "could not read frontend asset");
            return;
        }
        long size = ftell(file);
        rewind(file);
        if (size < 0) {
            fclose(file);
            send_error(connection, 500, "Internal Server Error",
                       "could not read frontend asset");
            return;
        }

        char *data = malloc(size > 0 ? (size_t)size : 1);
        if (!data ||
            (size > 0 &&
             fread(data, 1, (size_t)size, file) != (size_t)size)) {
            free(data);
            fclose(file);
            send_error(connection, 500, "Internal Server Error",
                       "could not read frontend asset");
            return;
        }
        fclose(file);

        send_data(connection, 200, "OK", STATIC_FILES[i].content_type,
                  data, (size_t)size);
        free(data);
        return;
    }

    send_error(connection, 404, "Not Found", "unknown endpoint");
}

static void handle_request(Connection *connection) {
    const char *header_end = memmem(connection->buffer, connection->length,
                                    "\r\n\r\n", 4);
    if (!header_end) {
        send_error(connection, 400, "Bad Request", "Malformed request");
        return;
    }

    size_t header_length = (size_t)(header_end - connection->buffer) + 4;
    size_t content_length = parse_content_length(connection->buffer,
                                                 header_length);
    if (content_length > sizeof(connection->buffer) - header_length) {
        send_error(connection, 413, "Payload Too Large",
                   "request body too large");
        return;
    }
    if (connection->length < header_length + content_length) {
        send_error(connection, 400, "Bad Request", "incomplete body");
        return;
    }

    char method[8];
    char path[256];
    if (parse_request_line(connection->buffer, connection->length,
                           method, sizeof(method),
                           path, sizeof(path)) != 0) {
        send_error(connection, 400, "Bad Request", "Malformed request line");
        return;
    }

    char *query = strchr(path, '?');
    if (query) *query = '\0';

    if (strcmp(path, "/api/bench") != 0) {
        printf("%s %s\n", method, path);
    }

    const char *request_body = connection->buffer + header_length;

    if (strcmp(method, "OPTIONS") == 0) {
        send_empty(connection, 204, "No Content");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/users") == 0) {
        handle_list_users(connection);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/users.bin") == 0) {
        handle_users_bin(connection);
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/users") == 0) {
        handle_add_user(connection, request_body, content_length);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/bench") == 0) {
        increment_balances();
        send_text(connection, 200, "OK", BENCH_TEXT);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        send_text(connection, 200, "OK", HEALTH_TEXT);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
        send_empty(connection, 204, "No Content");
        return;
    }
    if (strcmp(method, "GET") == 0) {
        handle_static(connection, path);
        return;
    }

    send_error(connection, 404, "Not Found", "unknown endpoint");
}

/* ------------------------------------------------------------------ */
/* Socket server                                                      */
/* ------------------------------------------------------------------ */

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return (flags == -1) ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listener(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int option = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    if (set_nonblocking(server_fd) != 0) {
        perror("fcntl");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, SOMAXCONN) != 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    printf("Serving API + WASM UI on http://localhost:%d\n", PORT);
    fflush(stdout);
    return server_fd;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    init_or_load_db();
    int server_fd = create_listener();

    epoll_fd_global = epoll_create1(0);
    if (epoll_fd_global < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    /* A NULL data pointer marks the listening socket. */
    struct epoll_event listener_event = {
        .events = EPOLLIN | EPOLLET,
        .data.ptr = NULL
    };
    if (epoll_ctl(epoll_fd_global, EPOLL_CTL_ADD,
                  server_fd, &listener_event) != 0) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int count = epoll_wait(epoll_fd_global, events, MAX_EVENTS, -1);
        if (count < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < count; i++) {
            Connection *connection = events[i].data.ptr;

            if (!connection) {
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        perror("accept");
                        break;
                    }
                    if (set_nonblocking(client_fd) != 0) {
                        close(client_fd);
                        continue;
                    }

                    Connection *client = malloc(sizeof(*client));
                    if (!client) {
                        close(client_fd);
                        continue;
                    }
                    client->fd = client_fd;
                    client->length = 0;
                    client->response = NULL;
                    client->response_length = 0;
                    client->response_sent = 0;
                    client->writing = 0;

                    struct epoll_event client_event = {
                        .events = EPOLLIN | EPOLLET | EPOLLRDHUP,
                        .data.ptr = client
                    };
                    if (epoll_ctl(epoll_fd_global, EPOLL_CTL_ADD,
                                  client_fd, &client_event) != 0) {
                        close(client_fd);
                        free(client);
                    }
                }
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close_connection(connection);
                continue;
            }

            if (connection->writing) {
                if (events[i].events & EPOLLOUT) {
                    int result = flush_response(connection);
                    if (result != 0) close_connection(connection);
                }
                continue;
            }

            /* Edge triggered: drain the socket and keep partial requests
             * buffered until the headers and declared body have arrived. */
            int status = 0; /* 0 = wait, 1 = handle, -1 = drop */
            while (connection->length < sizeof(connection->buffer)) {
                ssize_t bytes = read(connection->fd,
                                     connection->buffer + connection->length,
                                     sizeof(connection->buffer) - connection->length);
                if (bytes > 0) {
                    connection->length += (size_t)bytes;
                    if (request_ready(connection)) {
                        status = 1;
                        break;
                    }
                    continue;
                }
                if (bytes == 0) {
                    status = 1;
                    break;
                }
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                status = -1;
                break;
            }
            if (status == 0 && connection->length == sizeof(connection->buffer)) {
                status = 1;
            }

            if (status == 1) {
                if (connection->length == 0) {
                    close_connection(connection);
                } else {
                    handle_request(connection);
                }
            } else if (status < 0) {
                close_connection(connection);
            }
        }
    }

    close(epoll_fd_global);
    close(server_fd);
    return 0;
}
