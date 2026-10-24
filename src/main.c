#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { ADBSession *session; bool json, interactive, quit; } Console;

static void usage(FILE *output) {
    fputs("AtlasDB " ADB_VERSION " - base de datos SQL escrita en C17\n\n"
          "Uso: atlasdb [opciones]\n"
          "  --db DIRECTORIO       Base persistente; por defecto :memory:\n"
          "  -e, --execute SQL     Ejecutar SQL y salir (admite varias sentencias)\n"
          "  -f, --file ARCHIVO    Ejecutar un archivo SQL; '-' usa stdin\n"
          "  --json                Una respuesta JSON por sentencia\n"
          "  --server [PUERTO]     Servidor TCP; puerto por defecto 7373\n"
          "  --host DIRECCION      Interfaz del servidor; por defecto 127.0.0.1\n"
          "  --max-clients N       Maximo de conexiones, de 1 a 1024 (64)\n"
          "  --client HOST:PUERTO  Enviar -e al servidor (una sola linea)\n"
          "  --help                Mostrar esta ayuda\n"
          "  --version             Mostrar la version\n\n"
          "Sin -e, -f o --server se lee SQL de stdin. La consola interactiva\n"
          "acepta .help, .tables, .schema TABLA, .checkpoint y .quit.\n"
          "El protocolo TCP usa una sentencia por linea y una respuesta JSON.\n"
          "El servidor carece de autenticacion y TLS: usalo en redes de confianza.\n",
          output);
}

static void print_error(const ADBError *error, bool json) {
    if (json) {
        ADBBuffer buffer = {0};
        adb_buffer_printf(&buffer, "{\"ok\":false,\"code\":%d,\"error\":", (int)error->code);
        adb_buffer_json_string(&buffer, error->message);
        adb_buffer_printf(&buffer, ",\"offset\":%zu}", error->offset);
        if (!buffer.failed) puts(buffer.data);
        else fputs("{\"ok\":false,\"code\":6,\"error\":\"Sin memoria\"}\n", stdout);
        adb_buffer_free(&buffer);
    } else {
        fprintf(stderr, "Error %d (posicion %zu): %s\n", (int)error->code, error->offset, error->message);
    }
}

/* Decode UTF-8 independently of LC_CTYPE, so truncation never splits a
 * character even when the user runs under the plain C locale. */
static size_t utf8_character(const unsigned char *text, uint32_t *codepoint) {
    unsigned char first = text[0];
    if (first < 128) { *codepoint = first; return 1; }
    size_t length;
    uint32_t value;
    if (first >= 0xc2 && first <= 0xdf) { length = 2; value = first & 0x1fu; }
    else if (first >= 0xe0 && first <= 0xef) { length = 3; value = first & 0x0fu; }
    else if (first >= 0xf0 && first <= 0xf4) { length = 4; value = first & 0x07u; }
    else { *codepoint = UINT32_MAX; return 1; }
    for (size_t i = 1; i < length; ++i) {
        if (!text[i] || (text[i] & 0xc0u) != 0x80u) { *codepoint = UINT32_MAX; return 1; }
        value = (value << 6) | (text[i] & 0x3fu);
    }
    if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
        *codepoint = UINT32_MAX; return 1;
    }
    *codepoint = value;
    return length;
}

static size_t character_width(uint32_t codepoint) {
    int width = wcwidth((wchar_t)codepoint);
    if (width >= 0) return (size_t)width;
    /* A conservative fallback for a locale without Unicode tables. */
    if ((codepoint >= 0x0300 && codepoint <= 0x036f) ||
        (codepoint >= 0x1ab0 && codepoint <= 0x1aff) ||
        (codepoint >= 0xfe00 && codepoint <= 0xfe0f)) return 0;
    if ((codepoint >= 0x1100 && codepoint <= 0x115f) ||
        (codepoint >= 0x2e80 && codepoint <= 0xa4cf) ||
        (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
        (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
        (codepoint >= 0xff01 && codepoint <= 0xff60) ||
        (codepoint >= 0x1f300 && codepoint <= 0x1faff) ||
        (codepoint >= 0x20000 && codepoint <= 0x3ffff)) return 2;
    return 1;
}

static size_t text_width(const char *text) {
    size_t width = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t codepoint;
        p += utf8_character(p, &codepoint);
        width += character_width(codepoint);
    }
    return width;
}

static char *display_text(const char *raw) {
    ADBBuffer buffer = {0};
    size_t width = 0;
    const unsigned char *p = (const unsigned char *)raw;
    while (*p && width <= 48) {
        uint32_t codepoint;
        size_t length = utf8_character(p, &codepoint);
        if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') {
            const char *escape = codepoint == '\n' ? "\\n" : codepoint == '\r' ? "\\r" : "\\t";
            adb_buffer_append(&buffer, escape, 2); width += 2;
        } else if (codepoint < 32 || codepoint == 127 || codepoint == UINT32_MAX) {
            adb_buffer_printf(&buffer, "\\x%02x", *p); width += 4;
        } else if ((codepoint >= 0x80 && codepoint <= 0x9f) ||
                   (codepoint >= 0x2028 && codepoint <= 0x202e) ||
                   (codepoint >= 0x2066 && codepoint <= 0x2069)) {
            adb_buffer_printf(&buffer, "\\u%04x", (unsigned)codepoint); width += 6;
        } else {
            adb_buffer_append(&buffer, p, length); width += character_width(codepoint);
        }
        p += length;
    }
    if (buffer.failed) { adb_buffer_free(&buffer); return NULL; }
    if (!buffer.data) return strdup("");
    return buffer.data;
}

static char *display_value(const ADBValue *value) {
    char *raw = adb_value_string(value);
    if (!raw) return NULL;
    char *display = display_text(raw);
    free(raw);
    return display;
}

static void print_cell(const char *text, size_t width) {
    bool truncated = text_width(text) > width;
    size_t available = truncated && width >= 3 ? width - 3 : width;
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t codepoint;
        size_t length = utf8_character(p, &codepoint);
        size_t columns = character_width(codepoint);
        if (columns > available - used) break;
        fwrite(p, 1, length, stdout);
        used += columns;
        p += length;
    }
    while (used++ < available) putchar(' ');
    if (truncated && width >= 3) fputs("...", stdout);
}

static void print_rule(const size_t *widths, size_t count) {
    putchar('+');
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < widths[i] + 2; ++j) putchar('-');
        putchar('+');
    }
    putchar('\n');
}

static bool print_result(const ADBResult *result, bool json) {
    if (json) {
        char *text = adb_result_json(result);
        if (!text) return false;
        if (text[0] == '{') printf("{\"ok\":true,%s\n", text + 1);
        else puts(text);
        free(text);
        return !ferror(stdout);
    }
    if (!result->ncolumns) {
        if (result->message) puts(result->message);
        else printf("OK (%" PRIu64 " filas afectadas)\n", result->affected);
        return !ferror(stdout);
    }
    size_t *widths = calloc(result->ncolumns, sizeof(*widths));
    char **headers = calloc(result->ncolumns, sizeof(*headers));
    if (!widths || !headers) { free(widths); free(headers); return false; }
    bool success = false;
    for (size_t column = 0; column < result->ncolumns; ++column) {
        headers[column] = display_text(result->columns[column]);
        if (!headers[column]) goto cleanup;
        widths[column] = text_width(headers[column]);
        if (widths[column] > 48) widths[column] = 48;
        for (size_t row = 0; row < result->nrows; ++row) {
            char *text = display_value(&result->cells[row * result->ncolumns + column]);
            if (!text) goto cleanup;
            size_t length = text_width(text);
            free(text);
            if (length > 48) length = 48;
            if (length > widths[column]) widths[column] = length;
        }
    }
    print_rule(widths, result->ncolumns);
    putchar('|');
    for (size_t i = 0; i < result->ncolumns; ++i) {
        putchar(' '); print_cell(headers[i], widths[i]); fputs(" |", stdout);
    }
    putchar('\n');
    print_rule(widths, result->ncolumns);
    for (size_t row = 0; row < result->nrows; ++row) {
        putchar('|');
        for (size_t column = 0; column < result->ncolumns; ++column) {
            char *text = display_value(&result->cells[row * result->ncolumns + column]);
            if (!text) goto cleanup;
            putchar(' '); print_cell(text, widths[column]); fputs(" |", stdout);
            free(text);
        }
        putchar('\n');
    }
    print_rule(widths, result->ncolumns);
    printf("%zu fila%s\n", result->nrows, result->nrows == 1 ? "" : "s");
    success = !ferror(stdout);
cleanup:
    for (size_t i = 0; i < result->ncolumns; ++i) free(headers[i]);
    free(headers);
    free(widths);
    return success;
}

static bool execute_sql(Console *console, const char *sql) {
    ADBResult result = {0};
    ADBError error = {0};
    bool success = adb_execute(console->session, sql, &result, &error);
    if (success) {
        if (!print_result(&result, console->json)) {
            error.code = ADB_ERR_IO;
            (void)snprintf(error.message, sizeof(error.message), "No se pudo escribir la respuesta");
            print_error(&error, console->json);
            success = false;
        }
    } else print_error(&error, console->json);
    adb_result_free(&result);
    return success;
}

static bool dot_command(Console *console, char *command) {
    char *end = command + strlen(command);
    while (end > command && isspace((unsigned char)end[-1])) *--end = '\0';
    if (!strcmp(command, ".quit") || !strcmp(command, ".exit")) { console->quit = true; return true; }
    if (!strcmp(command, ".tables")) return execute_sql(console, "SHOW TABLES");
    if (!strcmp(command, ".checkpoint")) return execute_sql(console, "CHECKPOINT");
    if (!strcmp(command, ".help")) {
        puts("SQL termina con ';'. Comandos de consola:\n"
             "  .tables          Listar tablas\n"
             "  .schema TABLA    Describir columnas de una tabla\n"
             "  .checkpoint      Guardar snapshot y compactar WAL\n"
             "  .quit            Salir (revierte transacciones pendientes)\n"
             "  .help            Mostrar esta ayuda");
        return true;
    }
    if (!strncmp(command, ".schema", 7) && isspace((unsigned char)command[7])) {
        const char *name = command + 7;
        while (isspace((unsigned char)*name)) ++name;
        ADBBuffer sql = {0};
        adb_buffer_append(&sql, "DESCRIBE \"", 10);
        for (const char *p = name; *p; ++p) {
            if (*p == '"') adb_buffer_append(&sql, "\"", 1);
            adb_buffer_append(&sql, p, 1);
        }
        adb_buffer_append(&sql, "\"", 1);
        bool success = !sql.failed && execute_sql(console, sql.data);
        adb_buffer_free(&sql);
        return success;
    }
    ADBError error = {.code = ADB_ERR_PARSE};
    (void)snprintf(error.message, sizeof(error.message), "Comando desconocido; usa .help");
    print_error(&error, console->json);
    return false;
}

static void prompt(const Console *console, bool continuation) {
    if (!console->interactive) return;
    fputs(continuation ? "    ...> " : (adb_session_in_transaction(console->session) ? "atlasdb*> " : "atlasdb> "), stdout);
    fflush(stdout);
}

/* A streaming statement framer bounds each statement rather than the entire
 * file. Quotes/comments retain their exact bytes for the SQL lexer. */
static int run_stream(Console *console, FILE *input) {
    enum { NORMAL, STRING, IDENTIFIER, LINE_COMMENT, BLOCK_COMMENT } state = NORMAL;
    ADBBuffer sql = {0};
    bool has_sql = false, halted = false;
    int exit_status = 0;
    prompt(console, false);
    for (;;) {
        int c = fgetc(input);
        if (c == EOF) break;
        if (c == '\0') {
            ADBError error = {.code = ADB_ERR_PARSE};
            (void)snprintf(error.message, sizeof(error.message), "La entrada contiene un byte NUL");
            print_error(&error, console->json); exit_status = 1; halted = true; break;
        }
        if (console->interactive && !has_sql && state == NORMAL && c == '.') {
            ADBBuffer command = {0};
            do {
                if (!adb_buffer_append(&command, &((char){(char)c}), 1) || command.length > 4096) break;
                c = fgetc(input);
            } while (c != EOF && c != '\n');
            bool success = !command.failed && command.length <= 4096 && dot_command(console, command.data);
            if (!success) exit_status = 1;
            adb_buffer_free(&command);
            sql.length = 0; if (sql.data) sql.data[0] = '\0';
            if (console->quit) break;
            prompt(console, false);
            continue;
        }
        char byte = (char)c;
        if (sql.length >= ADB_MAX_SQL || !adb_buffer_append(&sql, &byte, 1)) {
            ADBError error = {.code = ADB_ERR_LIMIT};
            (void)snprintf(error.message, sizeof(error.message), "Sentencia demasiado larga (limite: 1 MiB)");
            print_error(&error, console->json); exit_status = 1; halted = true; break;
        }
        if (state == NORMAL) {
            if (c == '\'' || c == '"') { state = c == '\'' ? STRING : IDENTIFIER; has_sql = true; }
            else if (c == '-' || c == '/') {
                int next = fgetc(input);
                if ((c == '-' && next == '-') || (c == '/' && next == '*')) {
                    byte = (char)next;
                    if (!adb_buffer_append(&sql, &byte, 1)) { exit_status = 1; halted = true; break; }
                    state = c == '-' ? LINE_COMMENT : BLOCK_COMMENT;
                } else {
                    if (next != EOF) ungetc(next, input);
                    has_sql = true;
                }
            } else if (c == ';') {
                if (has_sql && !execute_sql(console, sql.data)) {
                    exit_status = 1;
                    if (!console->interactive) { halted = true; break; }
                }
                sql.length = 0; sql.data[0] = '\0'; has_sql = false;
            } else if (!isspace((unsigned char)c)) has_sql = true;
        } else if (state == STRING || state == IDENTIFIER) {
            if (c == (state == STRING ? '\'' : '"')) {
                int next = fgetc(input);
                if (next == c) {
                    byte = (char)next;
                    if (!adb_buffer_append(&sql, &byte, 1)) { exit_status = 1; halted = true; break; }
                } else {
                    if (next != EOF) ungetc(next, input);
                    state = NORMAL;
                }
            }
        } else if (state == LINE_COMMENT) {
            if (c == '\n') state = NORMAL;
        } else if (state == BLOCK_COMMENT && c == '*') {
            int next = fgetc(input);
            if (next == '/') {
                byte = (char)next;
                if (!adb_buffer_append(&sql, &byte, 1)) { exit_status = 1; halted = true; break; }
                state = NORMAL;
            } else if (next != EOF) ungetc(next, input);
        }
        if (c == '\n') prompt(console, has_sql || state != NORMAL);
    }
    if (!halted && (has_sql || state == BLOCK_COMMENT || state == STRING || state == IDENTIFIER))
        if (!execute_sql(console, sql.data ? sql.data : "")) exit_status = 1;
    if (ferror(input)) { perror("No se pudo leer la entrada"); exit_status = 1; }
    adb_buffer_free(&sql);
    return exit_status;
}

static bool parse_unsigned(const char *text, unsigned long minimum, unsigned long maximum, unsigned long *value) {
    if (!text || !*text || *text == '-' || *text == '+' || isspace((unsigned char)*text)) return false;
    errno = 0;
    char *end;
    unsigned long number = strtoul(text, &end, 10);
    if (errno || *end || number < minimum || number > maximum) return false;
    *value = number;
    return true;
}

int main(int argc, char **argv) {
    /* Numeric SQL/JSON formatting deliberately retains the C locale. */
    (void)setlocale(LC_CTYPE, "");
    const char *directory = ":memory:", *execute = NULL, *filename = NULL;
    const char *host = "127.0.0.1", *client_address = NULL;
    bool json = false, server = false;
    unsigned long port = 7373, max_clients = 64;
    for (int i = 1; i < argc; ++i) {
        const char *argument = argv[i];
        if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) { usage(stdout); return 0; }
        if (!strcmp(argument, "--version")) { puts("AtlasDB " ADB_VERSION); return 0; }
        if (!strcmp(argument, "--json")) { json = true; continue; }
        if (!strcmp(argument, "--server")) {
            server = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!parse_unsigned(argv[++i], 1, 65535, &port)) goto invalid;
            }
            continue;
        }
        if (i + 1 >= argc) goto invalid;
        if (!strcmp(argument, "--db")) directory = argv[++i];
        else if (!strcmp(argument, "--execute") || !strcmp(argument, "-e")) execute = argv[++i];
        else if (!strcmp(argument, "--file") || !strcmp(argument, "-f")) filename = argv[++i];
        else if (!strcmp(argument, "--host")) host = argv[++i];
        else if (!strcmp(argument, "--client")) client_address = argv[++i];
        else if (!strcmp(argument, "--max-clients")) {
            if (!parse_unsigned(argv[++i], 1, 1024, &max_clients)) goto invalid;
        } else goto invalid;
    }
    if ((execute && filename) || (server && (execute || filename || client_address)) ||
        (client_address && (!execute || filename))) goto invalid;
    if (client_address) {
        char *copy = strdup(client_address);
        if (!copy) return 1;
        char *client_host = copy, *separator;
        if (copy[0] == '[') {
            char *closing = strchr(copy, ']');
            if (!closing || closing[1] != ':') { free(copy); goto invalid; }
            *closing = '\0'; client_host = copy + 1; separator = closing + 1;
        } else separator = strrchr(copy, ':');
        if (!separator || !parse_unsigned(separator + 1, 1, 65535, &port)) { free(copy); goto invalid; }
        *separator = '\0';
        if (!*client_host) { free(copy); goto invalid; }
        int status = adb_run_client(client_host, (unsigned)port, execute);
        free(copy);
        return status;
    }
    ADBError error = {0};
    ADBDatabase *database = adb_open(directory, &error);
    if (!database) { print_error(&error, json); return 1; }
    if (server) {
        int status = adb_run_server(database, host, (unsigned)port, (size_t)max_clients);
        adb_close(database);
        return status;
    }
    ADBSession *session = adb_session_create(database, &error);
    if (!session) { print_error(&error, json); adb_close(database); return 1; }
    Console console = {.session = session, .json = json};
    FILE *input = stdin;
    bool close_input = false;
    if (execute) {
        input = fmemopen((void *)execute, strlen(execute), "r");
        close_input = true;
    } else if (filename && strcmp(filename, "-")) {
        input = fopen(filename, "rb");
        close_input = true;
    } else if (!filename) console.interactive = isatty(STDIN_FILENO) != 0;
    int status = 1;
    if (!input) perror("No se pudo abrir la entrada");
    else {
        if (console.interactive) puts("AtlasDB " ADB_VERSION " | SQL con ';' | .help para ayuda");
        status = run_stream(&console, input);
        if (close_input) fclose(input);
    }
    adb_session_destroy(session);
    adb_close(database);
    return status;
invalid:
    fputs("Opciones invalidas. Usa atlasdb --help.\n", stderr);
    return 2;
}
