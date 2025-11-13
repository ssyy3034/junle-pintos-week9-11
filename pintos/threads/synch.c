#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore *sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void sema_down(struct semaphore *sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    list_insert_ordered(&sema->waiters, &thread_current()->elem, thread_priority_less, NULL);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up(struct semaphore *sema) {
  enum intr_level old = intr_disable();
  sema->value++;
  if (!list_empty(&sema->waiters)) {
    list_sort(&sema->waiters, thread_priority_less, NULL);
    struct thread *t = list_entry(list_pop_front(&sema->waiters), struct thread, elem);
    thread_unblock(t);
  }
  intr_set_level(old);
  maybe_preempt();
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void *sema_) {
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock. */
void lock_init(struct lock *lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread. */
void lock_acquire(struct lock *lock) {
  struct thread *cur = thread_current();
  ASSERT(lock && !intr_context() && !lock_held_by_current_thread(lock));

  enum intr_level old = intr_disable();
  cur->waiting_lock = lock;
  donate_priority_chain(cur, lock);
  intr_set_level(old);

  sema_down(&lock->semaphore);

  old = intr_disable();
  lock->holder = cur;
  cur->waiting_lock = NULL;
  list_push_back(&cur->lock_held_list, &lock->lock_held);
  intr_set_level(old);
}

/* Tries to acquire LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread. */
bool lock_try_acquire(struct lock *lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success)
    lock->holder = thread_current();
  return success;
}

/* Donates priority recursively through a chain of locks. */
void donate_priority_chain(struct thread *from, struct lock *to_lock) {
  if (!to_lock)
    return;
  int donate = from->priority;
  struct thread *t = to_lock->holder;

  for (int d = 0; t && d < 8; d++) {
    if (t->priority >= donate)
      break;
    t->priority = donate;
    thread_priority_changed(t);
    if (!t->waiting_lock)
      break;
    t = t->waiting_lock->holder;
  }
}

/* Recomputes a thread’s priority after releasing a lock or
   updating donations. */
void thread_update_priority(struct thread *t) {
  enum intr_level old_level = intr_disable();

  int max_priority = t->original_priority;

  if (!list_empty(&t->lock_held_list)) {
    struct list_elem *e;
    for (e = list_begin(&t->lock_held_list); e != list_end(&t->lock_held_list); e = list_next(e)) {
      struct lock *l = list_entry(e, struct lock, lock_held);
      if (!list_empty(&l->semaphore.waiters)) {
        struct thread *waiter = list_entry(list_front(&l->semaphore.waiters), struct thread, elem);
        if (waiter->priority > max_priority)
          max_priority = waiter->priority;
      }
    }
  }

  t->priority = max_priority;

  intr_set_level(old_level);
}

/* Releases LOCK and updates thread priorities. */
void lock_release(struct lock *lock) {
  struct thread *t = thread_current();

  list_remove(&lock->lock_held);
  thread_update_priority(t);

  lock->holder = NULL;
  sema_up(&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false otherwise. */
bool lock_held_by_current_thread(const struct lock *lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem {
  struct list_elem elem;
  struct semaphore semaphore;
};

/* Initializes condition variable COND. */
void cond_init(struct condition *cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/* Comparison function for condition variable waiters by priority. */
bool cond_priority_compare(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
  struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
  struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

  struct thread *ta = list_entry(list_front(&sa->semaphore.waiters), struct thread, elem);
  struct thread *tb = list_entry(list_front(&sb->semaphore.waiters), struct thread, elem);

  return ta->priority > tb->priority;
}

/* Atomically releases LOCK and waits for COND to be signaled. */
void cond_wait(struct condition *cond, struct lock *lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  list_push_back(&cond->waiters, &waiter.elem);
  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

/* Signals one waiting thread, if any. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (!list_empty(&cond->waiters)) {
    list_sort(&cond->waiters, cond_priority_compare, NULL);
    struct semaphore_elem *se = list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);
    sema_up(&se->semaphore);
  }
}

/* Wakes up all threads waiting on COND. */
void cond_broadcast(struct condition *cond, struct lock *lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}
