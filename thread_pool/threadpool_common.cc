/* Copyright (C) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */



#include "threadpool.h"
#include "my_thread_local.h"
#include "mysql/psi/mysql_idle.h"
#include "mysql/thread_pool_priv.h"
#include "sql/debug_sync.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_connect.h"
#include "sql/protocol_classic.h"
#include "sql/sql_parse.h"
#include "mysql/plugin.h"
#include <dlfcn.h>


#define MYSQL_SERVER 1

/* Threadpool parameters */
uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;
uint threadpool_toobusy;
uint threadpool_high_prio_tickets;
ulong threadpool_high_prio_mode;

/* Stats */
TP_STATISTICS tp_stats;

/*
  Worker threads contexts, and THD contexts.
  =========================================

  Both worker threads and connections have their sets of thread local variables
  At the moment it is mysys_var (this has specific data for dbug, my_error and
  similar goodies), and PSI per-client structure.

  Whenever query is executed following needs to be done:

  1. Save worker thread context.
  2. Change TLS variables to connection specific ones using thread_attach(THD*).
     This function does some additional work.
  3. Process query
  4. Restore worker thread context.

  Connection login and termination follows similar schema w.r.t saving and
  restoring contexts.

  For both worker thread, and for the connection, mysys variables are created
  using my_thread_init() and freed with my_thread_end().

*/
class Worker_thread_context {
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *const psi_thread;
#endif
#ifndef DBUG_OFF
  const my_thread_id thread_id;
#endif
 public:
  Worker_thread_context() noexcept
      :
#ifdef HAVE_PSI_THREAD_INTERFACE
        psi_thread(PSI_THREAD_CALL(get_thread)())
#endif
#ifndef DBUG_OFF
        ,
        thread_id(my_thread_var_id())
#endif
  {
  }

  ~Worker_thread_context() noexcept {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
#ifndef DBUG_OFF
    set_my_thread_var_id(thread_id);
#endif
    THR_MALLOC = nullptr;
  }
};

/*
  Attach/associate the connection with the OS thread,
*/
static bool thread_attach(THD *thd) {
#ifndef DBUG_OFF
  set_my_thread_var_id(thd->thread_id());
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->get_psi());
#endif
  mysql_socket_set_thread_owner(
      thd->get_protocol_classic()->get_vio()->mysql_socket);
  return 0;
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
extern PSI_statement_info stmt_info_new_packet;
#endif

static void threadpool_net_before_header_psi_noop(NET * /* net */,
                                                  void * /* user_data */,
                                                  size_t /* count */) {}

static void threadpool_init_net_server_extension(THD *thd) {
#ifdef HAVE_PSI_INTERFACE
  // socket_connection.cc:init_net_server_extension should have been called
  // already for us. We only need to overwrite the "before" callback
  DBUG_ASSERT(thd->m_net_server_extension.m_user_data == thd);
  thd->m_net_server_extension.m_before_header =
      threadpool_net_before_header_psi_noop;
#else
  DBUG_ASSERT(thd->get_protocol_classic()->get_net()->extension == NULL);
#endif
}

int threadpool_add_connection(THD *thd) {
  int retval = 1;
  Worker_thread_context worker_context;

  my_thread_init();

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
  thd->set_psi(PSI_THREAD_CALL(new_thread)(key_thread_one_connection, thd,
                                           thd->thread_id()));
#endif

  /* Login. */
  thread_attach(thd);
  thd->start_utime = my_micro_time();
  thd->store_globals();

  if (thd_prepare_connection(thd)) {
    goto end;
  }

  /*
    Check if THD is ok, as prepare_new_connection_state()
    can fail, for example if init command failed.
  */
  if (thd_connection_alive(thd)) {
    retval = 0;
    thd_set_net_read_write(thd, 1);
    //thd->skip_wait_timeout = true; // !! todo
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    thd->m_server_idle = true;
    threadpool_init_net_server_extension(thd);
  }

end:
  if (retval) {
    Connection_handler_manager *handler_manager =
        Connection_handler_manager::get_instance();
    handler_manager->inc_aborted_connects();
  }
  return retval;
}


static Connection_handler_functions tp_chf = {
  0,
  tp_add_connection,
  tp_end
};

THD_event_functions tp_event_functions = {
  tp_wait_begin,
  tp_wait_end,
  tp_post_kill_notification
};


void threadpool_remove_connection(THD *thd) {
  Worker_thread_context worker_context;

  thread_attach(thd);
  thd_set_net_read_write(thd, 0);

  end_connection(thd);
  close_connection(thd, 0);

  thd->release_resources();

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(delete_thread)(thd->get_psi());
#endif

  Global_THD_manager::get_instance()->remove_thd(thd);
  Connection_handler_manager::dec_connection_count();
  delete thd;
}

/**
 Process a single client request or a single batch.
*/
int threadpool_process_request(THD *thd) {
  int retval = 0;
  Worker_thread_context worker_context;

  thread_attach(thd);

  if (thd->killed == THD::KILL_CONNECTION) {
    /*
      killed flag was set by timeout handler
      or KILL command. Return error.
    */
    retval = 1;
    goto end;
  }

  /*
    In the loop below, the flow is essentially the copy of thead-per-connections
    logic, see do_handle_one_connection() in sql_connect.c

    The goal is to execute a single query, thus the loop is normally executed
    only once. However for SSL connections, it can be executed multiple times
    (SSL can preread and cache incoming data, and vio->has_data() checks if it
    was the case).
  */
  for (;;) {
    Vio *vio;
    thd_set_net_read_write(thd, 0);

    if ((retval = do_command(thd)) != 0) goto end;

    if (!thd_connection_alive(thd)) {
      retval = 1;
      goto end;
    }

    vio = thd->get_protocol_classic()->get_vio();
    if (!vio->has_data(vio)) {
      /* More info on this debug sync is in sql_parse.cc*/
      DEBUG_SYNC(thd, "before_do_command_net_read");
      thd_set_net_read_write(thd, 1);
      goto end;
    }
    if (!thd->m_server_idle) {
      MYSQL_SOCKET_SET_STATE(vio->mysql_socket, PSI_SOCKET_STATE_IDLE);
      MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
      thd->m_server_idle = true;
    }
  }

end:
  if (!retval && !thd->m_server_idle) {
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state);
    thd->m_server_idle = true;
  }

  return retval;
}

static void fix_threadpool_size(THD*,
  struct SYS_VAR *, void*, const void* value)
{
  threadpool_size = *static_cast<const uint*>(value);
  tp_set_threadpool_size(threadpool_size);
}

static void fix_threadpool_stall_limit(THD*, struct SYS_VAR *, void*, const void* value)
{
  threadpool_stall_limit = *static_cast<const uint*>(value);
  tp_set_threadpool_stall_limit(threadpool_stall_limit);
}

static MYSQL_SYSVAR_UINT(threadpool_idle_timeout, threadpool_idle_timeout,
  PLUGIN_VAR_RQCMDARG,
  "Timeout in seconds for an idle thread in the thread pool."
  "Worker thread will be shut down after timeout",
  NULL, NULL, 60, 1, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(threadpool_oversubscribe, threadpool_oversubscribe,
  PLUGIN_VAR_RQCMDARG,
  "How many additional active worker threads in a group are allowed.",
  NULL, NULL, 3, 1, 1000, 1);

static MYSQL_SYSVAR_UINT(threadpool_toobusy, threadpool_toobusy,
  PLUGIN_VAR_RQCMDARG,
  "How many additional active and waiting worker threads in a group are allowed.",
  NULL, NULL, 13, 1, 1000, 1);

static MYSQL_SYSVAR_UINT(threadpool_size, threadpool_size,
 PLUGIN_VAR_RQCMDARG,
 "Number of thread groups in the pool. "
 "This parameter is roughly equivalent to maximum number of concurrently "
 "executing threads (threads in a waiting state do not count as executing).",
 NULL, fix_threadpool_size, (uint)16/*my_getncpus()*/, 1, MAX_THREAD_GROUPS, 1); // !! get_cpus

static MYSQL_SYSVAR_UINT(threadpool_stall_limit, threadpool_stall_limit, 
 PLUGIN_VAR_RQCMDARG,
 "Maximum query execution time in milliseconds,"
 "before an executing non-yielding thread is considered stalled."
 "If a worker thread is stalled, additional worker thread "
 "may be created to handle remaining clients.",
 NULL, fix_threadpool_stall_limit, 500, 10, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(threadpool_high_prio_tickets, threadpool_high_prio_tickets, 
  PLUGIN_VAR_RQCMDARG,
  "Number of tickets to enter the high priority event queue for each "
  "transaction.",
  NULL, NULL , UINT_MAX, 0, UINT_MAX, 1);

const char *threadpool_high_prio_mode_names[] = {"transactions", "statements",
                                                 "none", NullS};
TYPELIB threadpool_high_prio_mode_typelib =
{
   array_elements(threadpool_high_prio_mode_names) - 1, "threadpool_high_prio_mode",
   threadpool_high_prio_mode_names, NULL
};

static MYSQL_SYSVAR_ENUM(threadpool_high_prio_mode, threadpool_high_prio_mode,
  PLUGIN_VAR_RQCMDARG,
  "High priority queue mode: one of 'transactions', 'statements' or 'none'. "
  "In the 'transactions' mode the thread pool uses both high- and low-priority "
  "queues depending on whether an event is generated by an already started "
  "transaction and whether it has any high priority tickets (see "
  "thread_pool_high_prio_tickets). In the 'statements' mode all events (i.e. "
  "individual statements) always go to the high priority queue, regardless of "
  "the current transaction state and high priority tickets. "
  "'none' is the opposite of 'statements', i.e. disables the high priority queue "
  "completely.",
  NULL, NULL, TP_HIGH_PRIO_MODE_TRANSACTIONS, &threadpool_high_prio_mode_typelib);

static MYSQL_SYSVAR_UINT(threadpool_max_threads, threadpool_max_threads, 
  PLUGIN_VAR_RQCMDARG,
  "Maximum allowed number of worker threads in the thread pool", 
  NULL, NULL, MAX_CONNECTIONS, 1, MAX_CONNECTIONS, 1);

static int threadpool_plugin_init(void *)
{
  DBUG_ENTER("threadpool_plugin_init");
  // struct st_plugin_int *plugin = (struct st_plugin_int *)p;

  // if (plugin == nullptr ||
  //     plugin->plugin_dl == nullptr ||
  //     plugin->plugin_dl->handle == nullptr) {
  //   DBUG_RETURN(-1);
  // }

  my_connection_handler_set(&tp_chf, &tp_event_functions);
  tp_init(); 
  DBUG_RETURN(0);
}


static int threadpool_plugin_deinit(void *)
{
  DBUG_ENTER("threadpool_plugin_deinit");

  my_connection_handler_reset();
  DBUG_RETURN(0);
}

static struct SYS_VAR* system_variables[]= {
  MYSQL_SYSVAR(threadpool_idle_timeout),
  MYSQL_SYSVAR(threadpool_size),
  MYSQL_SYSVAR(threadpool_max_threads),
  MYSQL_SYSVAR(threadpool_stall_limit),
  MYSQL_SYSVAR(threadpool_oversubscribe),
  MYSQL_SYSVAR(threadpool_toobusy),
  MYSQL_SYSVAR(threadpool_high_prio_tickets),
  MYSQL_SYSVAR(threadpool_high_prio_mode),
  NULL
};

struct st_mysql_daemon thread_pool_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION  };

mysql_declare_plugin(thread_pool)
{
  MYSQL_DAEMON_PLUGIN,
  &thread_pool_plugin,
  "thread_pool",
  "TEST_TEST",
  "thread pool plugin extracted from percona server",
  PLUGIN_LICENSE_GPL,
  threadpool_plugin_init,       /* Plugin Init */
  nullptr,                      /* Plugin Check uninstall */
  threadpool_plugin_deinit,     /* Plugin Deinit */
  0x0100 /* 1.0 */,
  nullptr,                      /* status variables                */
  system_variables,             /* system variables                */
  nullptr,                      /* config options                  */
  0,                            /* flags                           */
}
mysql_declare_plugin_end;


