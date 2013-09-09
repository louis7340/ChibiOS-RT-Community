/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011,2012,2013 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    chmtx.c
 * @brief   Mutexes code.
 *
 * @addtogroup mutexes
 * @details Mutexes related APIs and services.
 *
 *          <h2>Operation mode</h2>
 *          A mutex is a threads synchronization object that can be in two
 *          distinct states:
 *          - Not owned (unlocked).
 *          - Owned by a thread (locked).
 *          .
 *          Operations defined for mutexes:
 *          - <b>Lock</b>: The mutex is checked, if the mutex is not owned by
 *            some other thread then it is associated to the locking thread
 *            else the thread is queued on the mutex in a list ordered by
 *            priority.
 *          - <b>Unlock</b>: The mutex is released by the owner and the highest
 *            priority thread waiting in the queue, if any, is resumed and made
 *            owner of the mutex.
 *          .
 *          <h2>Constraints</h2>
 *          In ChibiOS/RT the Unlock operations are always performed in
 *          lock-reverse order. The unlock API does not even have a parameter,
 *          the mutex to unlock is selected from an internal, per-thread, stack
 *          of owned mutexes. This both improves the performance and is
 *          required for an efficient implementation of the priority
 *          inheritance mechanism.
 *
 *          <h2>The priority inversion problem</h2>
 *          The mutexes in ChibiOS/RT implements the <b>full</b> priority
 *          inheritance mechanism in order handle the priority inversion
 *          problem.<br>
 *          When a thread is queued on a mutex, any thread, directly or
 *          indirectly, holding the mutex gains the same priority of the
 *          waiting thread (if their priority was not already equal or higher).
 *          The mechanism works with any number of nested mutexes and any
 *          number of involved threads. The algorithm complexity (worst case)
 *          is N with N equal to the number of nested mutexes.
 * @pre     In order to use the mutex APIs the @p CH_CFG_USE_MUTEXES option
 *          must be enabled in @p chconf.h.
 * @post    Enabling mutexes requires 5-12 (depending on the architecture)
 *          extra bytes in the @p thread_t structure.
 * @{
 */

#include "ch.h"

#if CH_CFG_USE_MUTEXES || defined(__DOXYGEN__)

/*===========================================================================*/
/* Module exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Module local types.                                                       */
/*===========================================================================*/

/*===========================================================================*/
/* Module local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Module local functions.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Module exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Initializes s @p mutex_t structure.
 *
 * @param[out] mp       pointer to a @p mutex_t structure
 *
 * @init
 */
void chMtxObjectInit(mutex_t *mp) {

  chDbgCheck(mp != NULL);

  queue_init(&mp->m_queue);
  mp->m_owner = NULL;
}

/**
 * @brief   Locks the specified mutex.
 * @post    The mutex is locked and inserted in the per-thread stack of owned
 *          mutexes.
 *
 * @param[in] mp        pointer to the @p mutex_t structure
 *
 * @api
 */
void chMtxLock(mutex_t *mp) {

  chSysLock();

  chMtxLockS(mp);

  chSysUnlock();
}

/**
 * @brief   Locks the specified mutex.
 * @post    The mutex is locked and inserted in the per-thread stack of owned
 *          mutexes.
 *
 * @param[in] mp        pointer to the @p mutex_t structure
 *
 * @sclass
 */
void chMtxLockS(mutex_t *mp) {
  thread_t *ctp = currp;

  chDbgCheckClassS();
  chDbgCheck(mp != NULL);

  /* Is the mutex already locked? */
  if (mp->m_owner != NULL) {
    /* Priority inheritance protocol; explores the thread-mutex dependencies
       boosting the priority of all the affected threads to equal the priority
       of the running thread requesting the mutex.*/
    thread_t *tp = mp->m_owner;

    /* Does the running thread have higher priority than the mutex
       owning thread? */
    while (tp->p_prio < ctp->p_prio) {
      /* Make priority of thread tp match the running thread's priority.*/
      tp->p_prio = ctp->p_prio;

      /* The following states need priority queues reordering.*/
      switch (tp->p_state) {
      case CH_STATE_WTMTX:
        /* Re-enqueues the mutex owner with its new priority.*/
        queue_prio_insert(queue_dequeue(tp),
                          (threads_queue_t *)tp->p_u.wtobjp);
        tp = ((mutex_t *)tp->p_u.wtobjp)->m_owner;
        continue;
#if CH_CFG_USE_CONDVARS |                                                       \
    (CH_CFG_USE_SEMAPHORES && CH_CFG_USE_SEMAPHORES_PRIORITY) |                     \
    (CH_CFG_USE_MESSAGES && CH_CFG_USE_MESSAGES_PRIORITY)
#if CH_CFG_USE_CONDVARS
      case CH_STATE_WTCOND:
#endif
#if CH_CFG_USE_SEMAPHORES && CH_CFG_USE_SEMAPHORES_PRIORITY
      case CH_STATE_WTSEM:
#endif
#if CH_CFG_USE_MESSAGES && CH_CFG_USE_MESSAGES_PRIORITY
      case CH_STATE_SNDMSGQ:
#endif
        /* Re-enqueues tp with its new priority on the queue.*/
        queue_prio_insert(queue_dequeue(tp),
                          (threads_queue_t *)tp->p_u.wtobjp);
        break;
#endif
      case CH_STATE_READY:
#if CH_DBG_ENABLE_ASSERTS
        /* Prevents an assertion in chSchReadyI().*/
        tp->p_state = CH_STATE_CURRENT;
#endif
        /* Re-enqueues tp with its new priority on the ready list.*/
        chSchReadyI(queue_dequeue(tp));
        break;
      }
      break;
    }

    /* Sleep on the mutex.*/
    queue_prio_insert(ctp, &mp->m_queue);
    ctp->p_u.wtobjp = mp;
    chSchGoSleepS(CH_STATE_WTMTX);

    /* It is assumed that the thread performing the unlock operation assigns
       the mutex to this thread.*/
    chDbgAssert(mp->m_owner == ctp, "not owner");
    chDbgAssert(ctp->p_mtxlist == mp, "not owned");
  }
  else {
    /* It was not owned, inserted in the owned mutexes list.*/
    mp->m_owner = ctp;
    mp->m_next = ctp->p_mtxlist;
    ctp->p_mtxlist = mp;
  }
}

/**
 * @brief   Tries to lock a mutex.
 * @details This function attempts to lock a mutex, if the mutex is already
 *          locked by another thread then the function exits without waiting.
 * @post    The mutex is locked and inserted in the per-thread stack of owned
 *          mutexes.
 * @note    This function does not have any overhead related to the
 *          priority inheritance mechanism because it does not try to
 *          enter a sleep state.
 *
 * @param[in] mp        pointer to the @p mutex_t structure
 * @return              The operation status.
 * @retval true         if the mutex has been successfully acquired
 * @retval false        if the lock attempt failed.
 *
 * @api
 */
bool chMtxTryLock(mutex_t *mp) {
  bool b;

  chSysLock();

  b = chMtxTryLockS(mp);

  chSysUnlock();
  return b;
}

/**
 * @brief   Tries to lock a mutex.
 * @details This function attempts to lock a mutex, if the mutex is already
 *          taken by another thread then the function exits without waiting.
 * @post    The mutex is locked and inserted in the per-thread stack of owned
 *          mutexes.
 * @note    This function does not have any overhead related to the
 *          priority inheritance mechanism because it does not try to
 *          enter a sleep state.
 *
 * @param[in] mp        pointer to the @p mutex_t structure
 * @return              The operation status.
 * @retval true         if the mutex has been successfully acquired
 * @retval false        if the lock attempt failed.
 *
 * @sclass
 */
bool chMtxTryLockS(mutex_t *mp) {

  chDbgCheckClassS();
  chDbgCheck(mp != NULL);

  if (mp->m_owner != NULL)
    return false;

  mp->m_owner = currp;
  mp->m_next = currp->p_mtxlist;
  currp->p_mtxlist = mp;
  return true;
}

/**
 * @brief   Unlocks the next owned mutex in reverse lock order.
 * @pre     The invoking thread <b>must</b> have at least one owned mutex.
 * @post    The mutex is unlocked and removed from the per-thread stack of
 *          owned mutexes.
 *
 * @return              A pointer to the unlocked mutex.
 *
 * @api
 */
mutex_t *chMtxUnlock(void) {
  thread_t *ctp = currp;
  mutex_t *ump, *mp;

  chSysLock();

  chDbgAssert(ctp->p_mtxlist != NULL, "owned mutexes list empty");
  chDbgAssert(ctp->p_mtxlist->m_owner == ctp, "ownership failure");

  /* Removes the top mutex from the thread's owned mutexes list and marks it
     as not owned.*/
  ump = ctp->p_mtxlist;
  ctp->p_mtxlist = ump->m_next;

  /* If a thread is waiting on the mutex then the fun part begins.*/
  if (chMtxQueueNotEmptyS(ump)) {
    thread_t *tp;

    /* Recalculates the optimal thread priority by scanning the owned
       mutexes list.*/
    tprio_t newprio = ctp->p_realprio;
    mp = ctp->p_mtxlist;
    while (mp != NULL) {
      /* If the highest priority thread waiting in the mutexes list has a
         greater priority than the current thread base priority then the final
         priority will have at least that priority.*/
      if (chMtxQueueNotEmptyS(mp) && (mp->m_queue.p_next->p_prio > newprio))
        newprio = mp->m_queue.p_next->p_prio;
      mp = mp->m_next;
    }

    /* Assigns to the current thread the highest priority among all the
       waiting threads.*/
    ctp->p_prio = newprio;

    /* Awakens the highest priority thread waiting for the unlocked mutex and
       assigns the mutex to it.*/
    tp = queue_fifo_remove(&ump->m_queue);
    ump->m_owner = tp;
    ump->m_next = tp->p_mtxlist;
    tp->p_mtxlist = ump;
    chSchWakeupS(tp, MSG_OK);
  }
  else
    ump->m_owner = NULL;
  chSysUnlock();
  return ump;
}

/**
 * @brief   Unlocks the next owned mutex in reverse lock order.
 * @pre     The invoking thread <b>must</b> have at least one owned mutex.
 * @post    The mutex is unlocked and removed from the per-thread stack of
 *          owned mutexes.
 * @post    This function does not reschedule so a call to a rescheduling
 *          function must be performed before unlocking the kernel.
 *
 * @return              A pointer to the unlocked mutex.
 *
 * @sclass
 */
mutex_t *chMtxUnlockS(void) {
  thread_t *ctp = currp;
  mutex_t *ump, *mp;

  chDbgCheckClassS();
  chDbgAssert(ctp->p_mtxlist != NULL, "owned mutexes list empty");
  chDbgAssert(ctp->p_mtxlist->m_owner == ctp, "ownership failure");

  /* Removes the top mutex from the owned mutexes list and marks it as not
     owned.*/
  ump = ctp->p_mtxlist;
  ctp->p_mtxlist = ump->m_next;

  /* If a thread is waiting on the mutex then the fun part begins.*/
  if (chMtxQueueNotEmptyS(ump)) {
    thread_t *tp;

    /* Recalculates the optimal thread priority by scanning the owned
       mutexes list.*/
    tprio_t newprio = ctp->p_realprio;
    mp = ctp->p_mtxlist;
    while (mp != NULL) {
      /* If the highest priority thread waiting in the mutexes list has a
         greater priority than the current thread base priority then the final
         priority will have at least that priority.*/
      if (chMtxQueueNotEmptyS(mp) && (mp->m_queue.p_next->p_prio > newprio))
        newprio = mp->m_queue.p_next->p_prio;

      mp = mp->m_next;
    }
    ctp->p_prio = newprio;

    /* Awakens the highest priority thread waiting for the unlocked mutex and
       assigns the mutex to it.*/
    tp = queue_fifo_remove(&ump->m_queue);
    ump->m_owner = tp;
    ump->m_next = tp->p_mtxlist;
    tp->p_mtxlist = ump;
    chSchReadyI(tp);
  }
  else
    ump->m_owner = NULL;
  return ump;
}

/**
 * @brief   Unlocks all the mutexes owned by the invoking thread.
 * @post    The stack of owned mutexes is emptied and all the found
 *          mutexes are unlocked.
 * @note    This function is <b>MUCH MORE</b> efficient than releasing the
 *          mutexes one by one and not just because the call overhead,
 *          this function does not have any overhead related to the priority
 *          inheritance mechanism.
 *
 * @api
 */
void chMtxUnlockAll(void) {
  thread_t *ctp = currp;

  chSysLock();
  if (ctp->p_mtxlist != NULL) {
    do {
      mutex_t *ump = ctp->p_mtxlist;
      ctp->p_mtxlist = ump->m_next;
      if (chMtxQueueNotEmptyS(ump)) {
        thread_t *tp = queue_fifo_remove(&ump->m_queue);
        ump->m_owner = tp;
        ump->m_next = tp->p_mtxlist;
        tp->p_mtxlist = ump;
        chSchReadyI(tp);
      }
      else
        ump->m_owner = NULL;
    } while (ctp->p_mtxlist != NULL);
    ctp->p_prio = ctp->p_realprio;
    chSchRescheduleS();
  }
  chSysUnlock();
}

#endif /* CH_CFG_USE_MUTEXES */

/** @} */