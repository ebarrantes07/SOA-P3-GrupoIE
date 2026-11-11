/* Registro CSV del coordinador. Ver include/log.h. */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wire.h"

/* Largo máximo del nombre de componente, contando el terminador. */
#define LOG_COMPONENT_CAP 64

struct dsm_log {
    FILE              *f;
    char               component[LOG_COMPONENT_CAP];
    uint64_t           run_id;
    uint64_t           seq;
    struct log_traffic tr;
    bool               failed;   /* pegajoso: alguna escritura no salió */
};

/* Columnas, en el orden en que se escriben. Los cuatro grupos del enunciado
 * quedan contiguos para que el archivo se lea de corrido. */
static const char *HEADER =
    "run_id,component,seq,node_id,request_id,page_id,event,status,"
    "mode,version,writer,readers,readers_mask,inval_pending,"
    "msgs_in,msgs_out,bytes_in,bytes_out,page_xfers,invals,dur_ns\n";

const char *log_event_name(enum log_event e) {
    switch (e) {
    case LOG_SESSION_START:   return "SESSION_START";
    case LOG_SESSION_END:     return "SESSION_END";
    case LOG_NODE_JOIN:       return "NODE_JOIN";
    case LOG_NODE_LEAVE:      return "NODE_LEAVE";
    case LOG_ACQ_REQ:         return "ACQ_REQ";
    case LOG_ACQ_GRANT:       return "ACQ_GRANT";
    case LOG_INVALIDATE:      return "INVALIDATE";
    case LOG_INVALIDATE_ACK:  return "INVALIDATE_ACK";
    case LOG_REL_WRITE:       return "REL_WRITE";
    case LOG_WRITE_PUBLISHED: return "WRITE_PUBLISHED";
    case LOG_REL_READ:        return "REL_READ";
    case LOG_READ_RELEASED:   return "READ_RELEASED";
    case LOG_BARRIER_ARRIVE:  return "BARRIER_ARRIVE";
    case LOG_BARRIER_RELEASE: return "BARRIER_RELEASE";
    case LOG_TIMEOUT:         return "TIMEOUT";
    case LOG_ERROR:           return "ERROR";
    }
    return "?";
}

const char *log_status_name(enum dsm_status s) {
    switch (s) {
    case DSM_OK:       return "";          /* no aplica */
    case DSM_EINVAL:   return "EINVAL";
    case DSM_ESTATE:   return "ESTATE";
    case DSM_ETIMEOUT: return "ETIMEOUT";
    case DSM_ECONN:    return "ECONN";
    case DSM_EPROTO:   return "EPROTO";
    case DSM_EABORT:   return "EABORT";
    case DSM_ENOMEM:   return "ENOMEM";
    case DSM_EIO:      return "EIO";
    }
    return "?";
}

struct log_row log_row_empty(enum log_event event) {
    struct log_row r;
    r.event      = event;
    r.node_id    = LOG_NO_NODE;
    r.request_id = LOG_NO_REQUEST;
    r.page_id    = LOG_NO_PAGE;
    r.mode       = (enum dsm_mode)LOG_NO_MODE;
    r.page       = NULL;
    r.status     = DSM_OK;
    r.dur_ns     = LOG_NO_DURATION;
    return r;
}

dsm_log *log_open(const char *path, const char *component, uint64_t run_id) {
    if (path == NULL || component == NULL)
        return NULL;

    /* Un nombre con coma o salto de línea rompería el CSV. Se rechaza en vez
     * de escaparse: el nombre lo elegimos nosotros. */
    size_t n = strlen(component);
    if (n == 0 || n >= LOG_COMPONENT_CAP)
        return NULL;
    if (strchr(component, ',') != NULL || strchr(component, '\n') != NULL ||
        strchr(component, '\r') != NULL || strchr(component, '"') != NULL)
        return NULL;

    dsm_log *lg = calloc(1, sizeof *lg);
    if (lg == NULL)
        return NULL;

    lg->f = fopen(path, "w");
    if (lg->f == NULL) {
        free(lg);
        return NULL;
    }
    /* Buffer por línea: si el proceso muere a mitad de la corrida, lo ya
     * registrado está en disco. */
    setvbuf(lg->f, NULL, _IOLBF, 0);

    memcpy(lg->component, component, n + 1);
    lg->run_id = run_id;

    if (fputs(HEADER, lg->f) == EOF)
        lg->failed = true;
    return lg;
}

bool log_close(dsm_log *lg) {
    if (lg == NULL)
        return true;
    bool ok = !lg->failed;
    if (lg->f != NULL && fclose(lg->f) != 0)
        ok = false;
    free(lg);
    return ok;
}

/* Las celdas numéricas ausentes se escriben vacías (ver log.h). Cada una de
 * estas devuelve la cadena ya formateada en el buffer que se le pasa, para no
 * repetir el mismo `if` en veinte lugares. */
static const char *cell_u32(char *buf, size_t cap, uint32_t v, uint32_t none) {
    if (v == none) { buf[0] = '\0'; return buf; }
    snprintf(buf, cap, "%lu", (unsigned long)v);
    return buf;
}

static const char *cell_u64(char *buf, size_t cap, uint64_t v, uint64_t none) {
    if (v == none) { buf[0] = '\0'; return buf; }
    snprintf(buf, cap, "%llu", (unsigned long long)v);
    return buf;
}

void log_write(dsm_log *lg, const struct log_row *row) {
    if (lg == NULL || row == NULL)
        return;

    /* Los buffers se dimensionan por el RANGO DEL TIPO, no por los valores
     * que esperamos: `writer` es int32_t y aunque en la práctica vale de -1 a
     * 3, un buffer para esos valores es exactamente lo que
     * -Wformat-truncation rechaza, y con razón. */
    char c_node[16], c_req[24], c_page[16], c_dur[24];
    char c_mode[8]  = "";
    char c_ver[24]  = "", c_writer[16] = "", c_readers[16] = "";
    char c_mask[16] = "", c_pend[16]   = "";

    if (row->mode == DSM_READ)
        memcpy(c_mode, "READ", 5);
    else if (row->mode == DSM_WRITE)
        memcpy(c_mode, "WRITE", 6);

    if (row->page != NULL) {
        const struct dir_page_info *p = row->page;
        snprintf(c_ver, sizeof c_ver, "%llu", (unsigned long long)p->version);
        snprintf(c_writer, sizeof c_writer, "%ld", (long)p->writer);
        snprintf(c_readers, sizeof c_readers, "%lu",
                 (unsigned long)p->reader_count);
        snprintf(c_mask, sizeof c_mask, "0x%02X", (unsigned)p->readers);
        snprintf(c_pend, sizeof c_pend, "%lu",
                 (unsigned long)p->pending_acks);
    }

    /* La secuencia la asigna ESTA función y nadie más: es lo que la vuelve
     * global y creciente sin coordinación (D0.1, un solo hilo). */
    lg->seq++;

    int rc = fprintf(lg->f,
        "%llu,%s,%llu,%s,%s,%s,%s,%s,"
        "%s,%s,%s,%s,%s,%s,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%s\n",
        (unsigned long long)lg->run_id, lg->component,
        (unsigned long long)lg->seq,
        cell_u32(c_node, sizeof c_node, row->node_id, LOG_NO_NODE),
        cell_u64(c_req, sizeof c_req, row->request_id, LOG_NO_REQUEST),
        cell_u32(c_page, sizeof c_page, row->page_id, LOG_NO_PAGE),
        log_event_name(row->event), log_status_name(row->status),
        c_mode, c_ver, c_writer, c_readers, c_mask, c_pend,
        (unsigned long long)lg->tr.msgs_in,
        (unsigned long long)lg->tr.msgs_out,
        (unsigned long long)lg->tr.bytes_in,
        (unsigned long long)lg->tr.bytes_out,
        (unsigned long long)lg->tr.page_xfers,
        (unsigned long long)lg->tr.invals,
        cell_u64(c_dur, sizeof c_dur, row->dur_ns, LOG_NO_DURATION));

    if (rc < 0)
        lg->failed = true;
}

/* Una trama lleva página cuando su tipo la puede traer Y su carga la incluye.
 * Las dos condiciones son necesarias: un ACQ_GRANT sin datos lleva solo la
 * versión, y por eso no cuenta como transferencia de página. */
static bool carries_page(uint8_t type, uint32_t payload_len) {
    if (type != DSM_WIRE_REL_WRITE && type != DSM_WIRE_ACQ_GRANT)
        return false;
    return payload_len >= DSM_PAGE_SIZE;
}

void log_count_sent(dsm_log *lg, uint8_t type, uint32_t payload_len) {
    if (lg == NULL)
        return;
    lg->tr.msgs_out++;
    lg->tr.bytes_out += (uint64_t)DSM_HEADER_SIZE + payload_len;
    if (carries_page(type, payload_len))
        lg->tr.page_xfers++;
    if (type == DSM_WIRE_INVALIDATE)
        lg->tr.invals++;
}

void log_count_recv(dsm_log *lg, uint8_t type, uint32_t payload_len) {
    if (lg == NULL)
        return;
    lg->tr.msgs_in++;
    lg->tr.bytes_in += (uint64_t)DSM_HEADER_SIZE + payload_len;
    if (carries_page(type, payload_len))
        lg->tr.page_xfers++;
}

struct log_traffic log_traffic_now(const dsm_log *lg) {
    struct log_traffic zero = {0, 0, 0, 0, 0, 0};
    return lg == NULL ? zero : lg->tr;
}

uint64_t log_seq(const dsm_log *lg) {
    return lg == NULL ? 0u : lg->seq;
}
