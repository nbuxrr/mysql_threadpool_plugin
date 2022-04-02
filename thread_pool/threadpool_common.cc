/* Copyright (C) 2012 Monty Program Ab
   Copyright (C) 2022 Huawei Technologies Co., Ltd

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
#include "threadpool_unix.h"
#include "my_thread_local.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysql/psi/mysql_idle.h"
#include "mysql/thread_pool_priv.h"
#include "sql/debug_sync.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_connect.h"
#include "sql/protocol_classic.h"
#include "sql/sql_parse.h"
#include "sql/sql_table.h"
#include "sql/field.h"
#include "sql/sql_show.h"
#include "sql/sql_class.h"
#include <dlfcn.h>
#include <memory>

#define MYSQL_SERVER 1

/* Threadpool parameters */
uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;
uint threadpool_toobusy;

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
#ifndef NDEBUG
  const my_thread_id thread_id;
#endif
 public:
  Worker_thread_context() noexcept
      :
#ifdef HAVE_PSI_THREAD_INTERFACE
        psi_thread(PSI_THREAD_CALL(get_thread)())
#endif
#ifndef NDEBUG
        ,
        thread_id(my_thread_var_id())
#endif
  {
  }

  ~Worker_thread_context() noexcept {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
#ifndef NDEBUG
    set_my_thread_var_id(thread_id);
#endif
    THR_MALLOC = nullptr;
  }
};

/*
  Attach/associate the connection with the OS thread,
*/
static bool thread_attach(THD *thd) {
#ifndef NDEBUG
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
  assert(thd->m_net_server_extension.m_user_data == thd);
  thd->m_net_server_extension.m_before_header =
      threadpool_net_before_header_psi_noop;
#else
  assert(thd->get_protocol_classic()->get_net()->extension == NULL);
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

static inline int my_getncpus() noexcept {
#ifdef _SC_NPROCESSORS_ONLN
  return sysconf(_SC_NPROCESSORS_ONLN);
#else
  return 2; /* The value returned by the old my_getncpus implementation */
#endif
}

static MYSQL_SYSVAR_UINT(idle_timeout, threadpool_idle_timeout,
  PLUGIN_VAR_RQCMDARG,
  "Timeout in seconds for an idle thread in the thread pool."
  "Worker thread will be shut down after timeout",
  NULL, NULL, 60, 1, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(oversubscribe, threadpool_oversubscribe,
  PLUGIN_VAR_RQCMDARG,
  "How many additional active worker threads in a group are allowed.",
  NULL, NULL, 3, 1, 1000, 1);

static MYSQL_SYSVAR_UINT(toobusy, threadpool_toobusy,
  PLUGIN_VAR_RQCMDARG,
  "How many additional active and waiting worker threads in a group are allowed.",
  NULL, NULL, 13, 1, 1000, 1);

static MYSQL_SYSVAR_UINT(size, threadpool_size,
 PLUGIN_VAR_RQCMDARG,
 "Number of thread groups in the pool. "
 "This parameter is roughly equivalent to maximum number of concurrently "
 "executing threads (threads in a waiting state do not count as executing).",
 NULL, fix_threadpool_size, (uint)my_getncpus(), 1, MAX_THREAD_GROUPS, 1);

static MYSQL_SYSVAR_UINT(stall_limit, threadpool_stall_limit, 
 PLUGIN_VAR_RQCMDARG,
 "Maximum query execution time in milliseconds,"
 "before an executing non-yielding thread is considered stalled."
 "If a worker thread is stalled, additional worker thread "
 "may be created to handle remaining clients.",
 NULL, fix_threadpool_stall_limit, 500, 10, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(max_threads, threadpool_max_threads, 
  PLUGIN_VAR_RQCMDARG,
  "Maximum allowed number of worker threads in the thread pool", 
  NULL, NULL, MAX_CONNECTIONS, 1, MAX_CONNECTIONS, 1);

static int threadpool_plugin_init(void *)
{
  DBUG_ENTER("threadpool_plugin_init");

  tp_init();
  my_connection_handler_set(&tp_chf, &tp_event_functions); 
  DBUG_RETURN(0);
}

static int threadpool_plugin_deinit(void *)
{
  DBUG_ENTER("threadpool_plugin_deinit");
  my_connection_handler_reset();
  DBUG_RETURN(0);
}

static MYSQL_THDVAR_UINT(high_prio_tickets, 
  PLUGIN_VAR_NOCMDARG,
  "Number of tickets to enter the high priority event queue for each "
  "transaction.",
  NULL, NULL, UINT_MAX, 0, UINT_MAX, 1);

const char *threadpool_high_prio_mode_names[] = {"transactions", "statements",
                                                 "none", NullS};
TYPELIB threadpool_high_prio_mode_typelib = {
   array_elements(threadpool_high_prio_mode_names) - 1, "",
   threadpool_high_prio_mode_names, NULL
};

static MYSQL_THDVAR_ENUM(high_prio_mode,
  PLUGIN_VAR_NOCMDARG,
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

static uint &idle_timeout = threadpool_idle_timeout;
static uint &size = threadpool_size;
static uint &stall_limit = threadpool_stall_limit;
static uint &max_threads = threadpool_max_threads;
static uint &oversubscribe = threadpool_oversubscribe;
static uint &toobusy = threadpool_toobusy;

SYS_VAR *system_variables[] = {
  MYSQL_SYSVAR(idle_timeout),
  MYSQL_SYSVAR(size),
  MYSQL_SYSVAR(max_threads),
  MYSQL_SYSVAR(stall_limit),
  MYSQL_SYSVAR(oversubscribe),
  MYSQL_SYSVAR(toobusy),
  MYSQL_SYSVAR(high_prio_tickets),
  MYSQL_SYSVAR(high_prio_mode),
  NULL
};

namespace Show {

static ST_FIELD_INFO groups_fields_info[] =
{
  {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"CONNECTIONS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"ACTIVE_THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"STANDBY_THREADS", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"QUEUE_LENGTH", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"HAS_LISTENER", 1, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {"IS_STALLED", 1, MYSQL_TYPE_TINY, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

} // namespace Show


static int groups_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint i = 0; i < MAX_THREAD_GROUPS && all_groups[i].pollfd != -1; i++)
  {
    thread_group_t* group = &all_groups[i];

    mysql_mutex_lock(&group->mutex);

    /* ID */
    table->field[0]->store(i, true);
    /* CONNECTION_COUNT */
    table->field[1]->store(group->connection_count, true);
    /* THREAD_COUNT */
    table->field[2]->store(group->thread_count, true);
    /* ACTIVE_THREAD_COUNT */
    table->field[3]->store(group->active_thread_count, true);
    /* STANDBY_THREAD_COUNT */
    table->field[4]->store(group->waiting_thread_count, true);
    /* QUEUE LENGTH */
    uint queue_len = group->high_prio_queue.elements()
      + group->queue.elements();
    table->field[5]->store(queue_len, true);
    /* HAS_LISTENER */
    table->field[6]->store((longlong)(group->listener != 0), true);
    /* IS_STALLED */
    table->field[7]->store(group->stalled, true);

    mysql_mutex_unlock(&group->mutex);

    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}


static int groups_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::groups_fields_info;
  schema->fill_table = groups_fill_table;
  return 0;
}


namespace Show {

static ST_FIELD_INFO queues_field_info[] =
{
  {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"POSITION", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"PRIORITY", 1, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"CONNECTION_ID", 19, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, 0, 0},
  {"QUEUEING_TIME_MICROSECONDS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

} // namespace Show

typedef connection_queue_t::Iterator connection_queue_iterator;

static int queues_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint group_id = 0;
    group_id < MAX_THREAD_GROUPS && all_groups[group_id].pollfd != -1;
    group_id++)
  {
    thread_group_t* group = &all_groups[group_id];

    mysql_mutex_lock(&group->mutex);
    bool err = false;
    int pos = 0;
    ulonglong now = my_microsecond_getsystime();
    connection_queue_t queues[NQUEUES] = {group->high_prio_queue, group->queue};
    for (uint prio = 0; prio < NQUEUES && !err; prio++)
    {
      connection_queue_iterator it(queues[prio]);
      connection_t* c;
      while ((c = it++) != 0)
      {
        /* GROUP_ID */
        table->field[0]->store(group_id, true);
        /* POSITION */
        table->field[1]->store(pos++, true);
        /* PRIORITY */
        table->field[2]->store(prio, true);
        /* CONNECTION_ID */
        if (c->thd != nullptr) {
          table->field[3]->store(c->thd->thread_id(), true);
        } else {
          table->field[3]->store(0, true);
        }
        /* QUEUEING_TIME */
        table->field[4]->store(now - c->enqueue_time, true);

        err = schema_table_store_record(thd, table);
        if (err)
          break;
      }
    }
    mysql_mutex_unlock(&group->mutex);
    if (err)
      return 1;
  }
  return 0;
}

static int queues_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::queues_field_info;
  schema->fill_table = queues_fill_table;
  return 0;
}

namespace Show {

static ST_FIELD_INFO stats_fields_info[] =
{
  {"GROUP_ID", 6, MYSQL_TYPE_LONG, 0, 0, 0, 0},
  {"THREAD_CREATIONS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"THREAD_CREATIONS_DUE_TO_STALL", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"WAKES", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"WAKES_DUE_TO_STALL", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"THROTTLES", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"STALLS", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"POLLS_BY_LISTENER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"POLLS_BY_WORKER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"DEQUEUES_BY_LISTENER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {"DEQUEUES_BY_WORKER", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

} // namespace Show


static int stats_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (uint i = 0; i < MAX_THREAD_GROUPS && all_groups[i].pollfd != -1; i++)
  {
    table->field[0]->store(i, true);
    thread_group_t* group = &all_groups[i];

    mysql_mutex_lock(&group->mutex);
    thread_group_counters_t* counters = &group->counters;
    table->field[1]->store(counters->thread_creations, true);
    table->field[2]->store(counters->thread_creations_due_to_stall, true);
    table->field[3]->store(counters->wakes, true);
    table->field[4]->store(counters->wakes_due_to_stall, true);
    table->field[5]->store(counters->throttles, true);
    table->field[6]->store(counters->stalls, true);
    table->field[7]->store(counters->polls[LISTENER], true);
    table->field[8]->store(counters->polls[WORKER], true);
    table->field[9]->store(counters->dequeues[LISTENER], true);
    table->field[10]->store(counters->dequeues[WORKER], true);
    mysql_mutex_unlock(&group->mutex);
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

static int stats_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::stats_fields_info;
  schema->fill_table = stats_fill_table;
  return 0;
}


namespace Show {

static ST_FIELD_INFO waits_fields_info[] =
{
  {"REASON", 16, MYSQL_TYPE_STRING, 0, 0, 0, 0},
  {"COUNT", 19, MYSQL_TYPE_LONGLONG, 0, 0, 0, 0},
  {0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0}
};

} // namespace Show

/* See thd_wait_type enum for explanation*/
static const LEX_CSTRING wait_reasons[THD_WAIT_LAST] =
{
  {STRING_WITH_LEN("UNKNOWN")},
  {STRING_WITH_LEN("SLEEP")},
  {STRING_WITH_LEN("DISKIO")},
  {STRING_WITH_LEN("ROW_LOCK")},
  {STRING_WITH_LEN("GLOBAL_LOCK")},
  {STRING_WITH_LEN("META_DATA_LOCK")},
  {STRING_WITH_LEN("TABLE_LOCK")},
  {STRING_WITH_LEN("USER_LOCK")},
  {STRING_WITH_LEN("BINLOG")},
  {STRING_WITH_LEN("GROUP_COMMIT")},
  {STRING_WITH_LEN("SYNC")}
};

extern std::atomic<uint64_t> tp_waits[THD_WAIT_LAST];

static int waits_fill_table(THD* thd, TABLE_LIST* tables, Item*)
{
  if (!all_groups)
    return 0;

  TABLE* table = tables->table;
  for (unsigned int i = 0; i < THD_WAIT_LAST; i++)
  {
    table->field[0]->store(wait_reasons[i].str, wait_reasons[i].length, system_charset_info);
    table->field[1]->store(tp_waits[i], true);
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

static int waits_init(void* p)
{
  ST_SCHEMA_TABLE* schema = (ST_SCHEMA_TABLE*)p;
  schema->fields_info = Show::waits_fields_info;
  schema->fill_table = waits_fill_table;
  return 0;
}

struct st_mysql_daemon thread_pool_plugin =
{ MYSQL_DAEMON_INTERFACE_VERSION  };

static struct st_mysql_information_schema plugin_descriptor =
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

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
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_GROUPS",
  "Vladislav Vaintroub",
  "Provides information about threadpool groups.",
  PLUGIN_LICENSE_GPL,
  groups_init,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  nullptr,
  0,
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_QUEUES",
  "Vladislav Vaintroub",
  "Provides information about threadpool queues.",
  PLUGIN_LICENSE_GPL,
  queues_init,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  nullptr,
  0,
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_STATS",
  "Vladislav Vaintroub",
  "Provides performance counter information for threadpool.",
  PLUGIN_LICENSE_GPL,
  stats_init,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  nullptr,
  0,
},
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &plugin_descriptor,
  "THREAD_POOL_WAITS",
  "Vladislav Vaintroub",
  "Provides wait counters for threadpool.",
  PLUGIN_LICENSE_GPL,
  waits_init,
  nullptr,
  nullptr,
  0x0100,
  nullptr,
  nullptr,
  nullptr,
  0,
}
mysql_declare_plugin_end;

uint tp_get_thdvar_high_prio_tickets(THD *thd) {
  return THDVAR(thd, high_prio_tickets);
}

uint tp_get_thdvar_high_prio_mode(THD *thd) {
  return THDVAR(thd, high_prio_mode);
}

