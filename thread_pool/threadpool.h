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
#ifndef THREADPOOL_H_
#define THREADPOOL_H_

#include "sql/sql_class.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/conn_handler/connection_handler_manager.h"
#include "sql/conn_handler/channel_info.h"

struct SHOW_VAR;

#define MAX_THREAD_GROUPS 128
#define MAX_CONNECTIONS 100000


enum tp_high_prio_mode_t {
  TP_HIGH_PRIO_MODE_TRANSACTIONS,
  TP_HIGH_PRIO_MODE_STATEMENTS,
  TP_HIGH_PRIO_MODE_NONE
};

/* Threadpool parameters */
extern uint threadpool_idle_timeout;  /* Shutdown idle worker threads after this timeout */
extern uint threadpool_size;          /* Number of parallel executing threads */
extern uint threadpool_max_threads;
extern uint threadpool_stall_limit;   /* time interval in 10 ms units for stall checks*/
extern uint threadpool_oversubscribe; /* Maximum active threads in group */
extern uint threadpool_toobusy;       /* Maximum active and waiting threads in group */

/* Possible values for thread_pool_high_prio_mode */
extern const char *threadpool_high_prio_mode_names[];

/* Common thread pool routines, suitable for different implementations */
extern void threadpool_remove_connection(THD *thd);
extern int  threadpool_process_request(THD *thd);
extern int  threadpool_add_connection(THD *thd);

/*
  Functions used by scheduler. 
  OS-specific implementations are in
  threadpool_unix.cc or threadpool_win.cc
*/
extern bool tp_init();
extern void tp_wait_begin(THD *, int);
extern void tp_wait_end(THD *);
extern void tp_post_kill_notification(THD *thd) noexcept;
extern bool tp_add_connection(Channel_info *);
extern void tp_end(void);
extern void tp_fake_end(void);
extern void threadpool_remove_connection(THD *thd);

extern THD_event_functions tp_event_functions;

/*
  Threadpool statistics
*/
struct TP_STATISTICS {
  /* Current number of worker thread. */
  std::atomic<int32> num_worker_threads;
};

extern TP_STATISTICS tp_stats;

/* Functions to set threadpool parameters */
extern void tp_set_threadpool_size(uint val) noexcept;
extern void tp_set_threadpool_stall_limit(uint val) noexcept;

extern uint tp_get_thdvar_high_prio_tickets(THD *thd);
extern uint tp_get_thdvar_high_prio_mode(THD *thd);

#endif // THREADPOOL_H_

