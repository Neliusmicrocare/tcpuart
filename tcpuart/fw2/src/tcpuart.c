#include "common/cs_dbg.h"

#include "user_interface.h"

#include "fw/src/sj_app.h"
#include "fw/src/sj_hal.h"
#include "fw/src/sj_mongoose.h"
#include "fw/src/sj_pwm.h"
#include "fw/src/sj_sys_config.h"
#include "fw/src/mg_uart.h"

#include "common/platforms/esp8266/esp_mg_net_if.h"
#include "fw/platforms/esp8266/user/esp_uart.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#ifndef IRAM
#define IRAM
#endif

static struct sys_config_tcp *s_tcfg = NULL;
static struct sys_config_uart *s_ucfg = NULL;
static struct sys_config_misc *s_mcfg = NULL;

static struct mg_uart_state *s_us = NULL;
static struct mg_connection *s_conn = NULL;
static struct mg_connection *s_mgr_conn = NULL;
static struct mg_connection *s_client_conn = NULL;
static double s_last_connect_attempt = 0;
static struct mbuf s_tcp_rx_tail;
static double s_last_activity = 0;
static double s_last_tcp_status_report = 0;
static double s_last_uart_status_report = 0;
static struct mg_uart_stats s_prev_stats;

static void tu_conn_mgr(struct mg_connection *nc, int ev, void *ev_data);
static void tu_conn_handler(struct mg_connection *nc, int ev, void *ev_data);
static IRAM void tu_dispatcher(struct mg_uart_state *us);

static int init_tcp(struct sys_config_tcp *cfg) {
  char listener_spec[100];
  struct mg_bind_opts bopts;
  memset(&bopts, 0, sizeof(bopts));
  if (cfg->listener.port > 0) {
    sprintf(listener_spec, "%d", cfg->listener.port);
    if (cfg->listener.tls.cert) {
      bopts.ssl_cert = cfg->listener.tls.cert;
    }
  } else {
    /*
     * User doesn't want us to listen on a port, but we need a persistent
     * connection to manage UART buffers when there isn't any active one.
     * I'm not proud of this, but this is the easiest way to achieve that:
     * listen on a port that nobody is going to reach from the outside.
     */
    strcpy(listener_spec, "127.0.0.1:1234");
  }
  LOG(LL_INFO, ("Listening on %s (%s)", listener_spec,
                (bopts.ssl_cert ? bopts.ssl_cert : "-")));
  s_mgr_conn = mg_bind_opt(&sj_mgr, listener_spec, tu_conn_mgr, bopts);
  if (s_mgr_conn == NULL) {
    LOG(LL_ERROR, ("Failed to create listener"));
    return 0;
  }
  s_tcfg = cfg;
  return 1;
}

static int init_uart(struct sys_config_uart *ucfg) {
  struct mg_uart_config *cfg = mg_uart_default_config();
  cfg->baud_rate = ucfg->baud_rate;
  cfg->rx_buf_size = ucfg->rx_buf_size;
  cfg->rx_fc_ena = ucfg->rx_fc_ena;
  cfg->rx_fifo_full_thresh = ucfg->rx_fifo_full_thresh;
  cfg->rx_fifo_fc_thresh = ucfg->rx_fifo_fc_thresh;
  cfg->rx_fifo_alarm = ucfg->rx_fifo_alarm;
  cfg->rx_linger_micros = ucfg->rx_linger_micros;
  cfg->tx_buf_size = ucfg->tx_buf_size;
  cfg->tx_fc_ena = ucfg->tx_fc_ena;
  cfg->tx_fifo_empty_thresh = ucfg->tx_fifo_empty_thresh;
  cfg->tx_fifo_full_thresh = ucfg->tx_fifo_full_thresh;
  cfg->swap_rxcts_txrts = ucfg->swap_rxcts_txrts;
  //  cfg->status_interval_ms = ucfg->status_interval_ms;
  s_us = mg_uart_init(ucfg->uart_no, cfg, tu_dispatcher, NULL);
  if (s_us == NULL) {
    LOG(LL_ERROR, ("UART init failed"));
    free(cfg);
    return 0;
  }
  LOG(LL_INFO, ("UART%d configured: %d fc %d/%d", ucfg->uart_no, cfg->baud_rate,
                cfg->rx_fc_ena, cfg->tx_fc_ena));
  s_ucfg = ucfg;
  return 1;
}

size_t tu_dispatch_tcp_to_uart(struct mbuf *mb, struct mg_uart_state *us) {
  size_t len = 0;
  cs_rbuf_t *utxb = &us->tx_buf;
  len = MIN(mb->len, utxb->avail);
  if (len > 0) {
    cs_rbuf_append(utxb, mb->buf, len);
    mbuf_remove(mb, len);
  }
  return len;
}

void check_beeper() {
  static int beeping_on_gpio = -1;
  static double last_change = 0;
  if (beeping_on_gpio >= 0) {
    if (s_mcfg->beeper.timeout_seconds == 0 ||
        s_mcfg->beeper.gpio_no != beeping_on_gpio ||
        (mg_time() - last_change > 0.9)) {
      sj_pwm_set(beeping_on_gpio, 0, 0);
      beeping_on_gpio = -1;
      last_change = mg_time();
      return;
    }
    /* Continue beeping. */
    return;
  }
  /* Is beeping on inactivity enabled? */
  if (s_mcfg->beeper.timeout_seconds <= 0 || s_mcfg->beeper.gpio_no < 0) {
    return;
  }
  /* Should we be beeping? */
  const double now = mg_time();
  if ((now - s_last_activity > s_mcfg->beeper.timeout_seconds) &&
      (now - last_change > 0.9)) {
    beeping_on_gpio = s_mcfg->beeper.gpio_no;
    sj_pwm_set(beeping_on_gpio, 250, 125); /* BEEEP! (4 KHz) */
    last_change = now;
    LOG(LL_WARN,
        ("No activity for %d seconds - BEEP!", (int) (now - s_last_activity)));
  }
}

static void report_status(struct mg_connection *nc, int force) {
  double now = mg_time();
  if (nc != NULL && s_tcfg->status_interval_ms > 0 &&
      (force ||
       (now - s_last_tcp_status_report) * 1000 >= s_tcfg->status_interval_ms)) {
    char addr[32];
    mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                        MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
    fprintf(stderr, "TCP %p %s f %d rb %d sb %d\n", nc, addr, (int) nc->flags,
            (int) nc->recv_mbuf.len, (int) nc->send_mbuf.len);
    s_last_tcp_status_report = now;
  }
  if (s_us != NULL && s_ucfg->status_interval_ms > 0 &&
      (force ||
       (now - s_last_uart_status_report) * 1000 >=
           s_ucfg->status_interval_ms)) {
    struct mg_uart_state *us = s_us;
    struct mg_uart_stats *s = &us->stats;
    struct mg_uart_stats *ps = &s_prev_stats;
    int uart_no = us->uart_no;
    fprintf(stderr,
            "UART%d ints %u/%u/%u; rx en %d bytes %u buf %u fifo %u, ovf %u, "
            "lcs %u; "
            "tx %u %u %u, thr %u; hf %u i 0x%03x ie 0x%03x cts %d\n",
            uart_no, s->ints - ps->ints, s->rx_ints - ps->rx_ints,
            s->tx_ints - ps->tx_ints, us->rx_enabled,
            s->rx_bytes - ps->rx_bytes, us->rx_buf.used,
            esp_uart_rx_fifo_len(uart_no), s->rx_overflows - ps->rx_overflows,
            s->rx_linger_conts - ps->rx_linger_conts,
            s->tx_bytes - ps->tx_bytes, us->tx_buf.used,
            esp_uart_tx_fifo_len(uart_no), s->tx_throttles - ps->tx_throttles,
            system_get_free_heap_size(), READ_PERI_REG(UART_INT_RAW(uart_no)),
            READ_PERI_REG(UART_INT_ENA(uart_no)), esp_uart_cts(uart_no));
    memcpy(ps, s, sizeof(*s));
    s_last_uart_status_report = now;
  }
}

static IRAM void tu_dispatcher(struct mg_uart_state *us) {
  size_t len = 0;
  /* TCP -> UART */
  /* Drain buffer left from a previous connection, if any. */
  if (s_tcp_rx_tail.len > 0) {
    tu_dispatch_tcp_to_uart(&s_tcp_rx_tail, us);
    mbuf_trim(&s_tcp_rx_tail);
  }
  /* UART -> TCP */
  if (s_conn != NULL) {
    cs_rbuf_t *urxb = &us->rx_buf;
    while (urxb->used > 0 &&
           (len = (s_tcfg->tx_buf_size - s_conn->send_mbuf.len)) > 0) {
      uint8_t *data;
      len = cs_rbuf_get(urxb, len, &data);
      mbuf_append(&s_conn->send_mbuf, data, len);
      cs_rbuf_consume(urxb, len);
    }
    if (len > 0) {
      LOG(LL_DEBUG, ("UART -> %d -> TCP", (int) len));
      s_last_activity = mg_time();
    }
  }
}

static void tu_conn_handler(struct mg_connection *nc, int ev, void *ev_data) {
  (void) ev_data;

  sj_wdt_feed();

  switch (ev) {
    case MG_EV_POLL:
    case MG_EV_RECV: {
      /* If there is a tail from previous conn, we need it to drain first. */
      if (s_tcp_rx_tail.len > 0) break;
      /* TCP -> UART */
      size_t len = tu_dispatch_tcp_to_uart(&nc->recv_mbuf, s_us);
      if (len > 0) {
        LOG(LL_DEBUG, ("UART <- %d <- TCP", (int) len));
        s_last_activity = mg_time();
      }
    }
    case MG_EV_SEND: {
      mg_uart_schedule_dispatcher(s_ucfg->uart_no);
      break;
    }
    case MG_EV_CLOSE: {
      LOG(LL_INFO, ("%p closed", nc));
      report_status(nc, 1 /* force */);
      if (nc == s_conn) {
        mg_uart_set_rx_enabled(s_ucfg->uart_no, 0);
        if (nc->recv_mbuf.len > 0) {
          /* Rescue the bytes remaining in the rx buffer - if we have space. */
          if (s_tcp_rx_tail.len == 0) {
            mbuf_free(&s_tcp_rx_tail);
            s_tcp_rx_tail.len = nc->recv_mbuf.len;
            s_tcp_rx_tail.buf = nc->recv_mbuf.buf;
            nc->recv_mbuf.buf = NULL;
            nc->recv_mbuf.len = 0;
          } else {
            LOG(LL_WARN,
                ("Dropped %d bytes on the floor", (int) nc->recv_mbuf.len));
          }
        }
        s_conn = NULL;
      }
      break;
    }
  }
}

static void tu_set_conn(struct mg_connection *nc) {
  LOG(LL_INFO, ("New conn: %p", nc));
  nc->handler = tu_conn_handler;
  mg_lwip_set_keepalive_params(nc, s_tcfg->keepalive.idle,
                               s_tcfg->keepalive.interval,
                               s_tcfg->keepalive.count);
  s_last_tcp_status_report = mg_time();
  s_conn = nc;
  mg_uart_set_rx_enabled(s_ucfg->uart_no, 1);
}

static void tu_conn_mgr(struct mg_connection *nc, int ev, void *ev_data) {
  switch (ev) {
    case MG_EV_ACCEPT: {
      char addr[32];
      mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                          MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
      LOG(LL_INFO, ("%p Connection from %s", nc, addr));
      if (s_conn != NULL) {
        LOG(LL_INFO, ("Evicting %p", s_conn));
        s_conn->flags |= MG_F_SEND_AND_CLOSE;
      }
      tu_set_conn(nc);
      break;
    }
    case MG_EV_POLL: {
      check_beeper();
      report_status(s_conn, 0 /* force */);
      if (s_conn == NULL) {
        /* Initiate outgoing connection, if configured. */
        if (s_client_conn == NULL && s_tcfg->client.remote_addr != NULL &&
            (mg_time() - s_last_connect_attempt) >=
                s_tcfg->client.reconnect_interval) {
          const char *error;
          struct mg_connect_opts copts;
          memset(&copts, 0, sizeof(copts));
          copts.ssl_cert = s_tcfg->client.tls.cert;
          copts.ssl_ca_cert = s_tcfg->client.tls.ca_cert;
          copts.ssl_server_name = s_tcfg->client.tls.server_name;
          copts.error_string = &error;
          LOG(LL_INFO, ("%p Connecting to %s (%s %s %s)", s_client_conn,
                        s_tcfg->client.remote_addr,
                        (copts.ssl_cert ? copts.ssl_cert : "-"),
                        (copts.ssl_ca_cert ? copts.ssl_ca_cert : "-"),
                        (copts.ssl_server_name ? copts.ssl_server_name : "-")));
          s_last_connect_attempt = mg_time();
          s_client_conn = mg_connect_opt(nc->mgr, s_tcfg->client.remote_addr,
                                         tu_conn_mgr, copts);
          if (s_client_conn == NULL) {
            LOG(LL_ERROR, ("Connection error: %s", error));
          }
        }
      }
      break;
    }
    case MG_EV_CONNECT: {
      int res = *((int *) ev_data);
      LOG(LL_INFO, ("%p Connect result: %d", nc, res));
      if (res == 0) {
        if (s_conn == NULL) {
          tu_set_conn(nc);
        } else {
          /* We already have a connection (probably accepted one while
           * connecting), drop it. */
          LOG(LL_INFO, ("%p Already have %p, closing this one", nc, s_conn));
          nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        }
      } else {
        /* Do nothing, wait for close event. */
      }
    }
    case MG_EV_CLOSE: {
      if (nc == s_client_conn) {
        s_client_conn = NULL;
        s_last_connect_attempt = mg_time();
      }
    }
  }
}

enum mg_app_init_result sj_app_init() {
  s_mcfg = &get_cfg()->misc;
  s_last_activity = mg_time();
  LOG(LL_INFO, ("TCPUART init, SDK %s", system_get_sdk_version()));
  if (!init_tcp(&get_cfg()->tcp)) return MG_APP_INIT_ERROR;
  if (!init_uart(&get_cfg()->uart)) return MG_APP_INIT_ERROR;
  return MG_APP_INIT_SUCCESS;
}
