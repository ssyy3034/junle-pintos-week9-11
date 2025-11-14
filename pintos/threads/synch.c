/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "list.h"

static bool
more_priority(const struct list_elem *a, // 새로 넣을 원소
			  const struct list_elem *b, // 전체 ready-리스트 각 원소
			  void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, elem); // a가 속한 thread구조체 포인터 얻음
	const struct thread *tb = list_entry(b, struct thread, elem);
	return ta->priority > tb->priority; // True인 자리가 들어갈 자리
}

bool // ready_list/ sema waiters는 elem, donations: donation_elem 사용
donation_more(const struct list_elem *a,
			  const struct list_elem *b,
			  void *aux UNUSED)
{
	const struct thread *da = list_entry(a, struct thread, donation_elem);
	const struct thread *db = list_entry(b, struct thread, donation_elem);

	return da->priority > db->priority;
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
//    void
// thread_yield (void) {
// 	struct thread *curr = thread_current ();
// 	enum intr_level old_level;

// 	ASSERT (!intr_context ());

// 	old_level = intr_disable ();

// 	if (curr != idle_thread)
// 		//list_push_back (&ready_list, &curr->elem); /* 기존 */
// 		list_insert_ordered(&ready_list, &curr->elem, more_priority, NULL); //우선순위 맞는 자리에 삽입
// 	do_schedule (THREAD_READY);

// 	intr_set_level (old_level);
// }
void sema_down(struct semaphore *sema)
{ // 현재 스레드 요소 제거
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());

	old_level = intr_disable();
	while (sema->value == 0)
	{
		// list_push_back (&sema->waiters, &thread_current ()->elem);
		list_insert_ordered(&sema->waiters, &thread_current()->elem, more_priority, NULL);
		thread_block();
	}
	sema->value--;
	// 이러고 기존 readylist에 있던 애 뽑아와야하는거아닌지?
	intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void ////주의점: 인터럽트 잠금 필요한 핵심 구간
sema_up(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);

	old_level = intr_disable();

	bool flag = false;
	if (!list_empty(&sema->waiters))
	{
		struct thread *t = list_entry(list_pop_front(&sema->waiters),
									  struct thread, elem);
		thread_unblock(t);
		// 선점 기회
		if (t->priority > thread_current()->priority)
		{
			flag = true;
		}
	}

	sema->value++;

	if (flag)
	{
		thread_yield(); // flag로 안빼두면: yield->우선순위높은스레드 run=> sema->value++나중에돼서 문제생김
	}
	intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1); // 락은 세마포어 무조건 <=1
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  ****This function may be called with interrupts disabled*****,
   but interrupts will be turned back on if we need to sleep. */
void // 락 얻기
lock_acquire(struct lock *lock)
{ //** intr_disabled상태로 불러야하는 함수임!!
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	sema_down(&lock->semaphore);
	lock->holder = thread_current(); // 점유자: 현스레드
}
// void lock_acquire(struct lock *lock)
// {
// 	struct thread *cur = thread_current();

// 	if (lock->holder && lock->holder != cur)
// 	{
// 		cur->waiting_lock = lock;
// 		donate_chain(cur); // 여기서 호출!! (연쇄 기부 전파)
// 	}
// 	sema_down(&lock->semaphore);
// 	cur->waiting_lock = NULL;
// 	lock->holder = cur;
// 	list_push_back(&cur->locks, &lock->elem);
// }
/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	lock->holder = NULL;
	sema_up(&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem
{
	struct list_elem elem;		/* List element. */
	struct semaphore semaphore; /* This semaphore. */
};

static bool // cond_waiters > semaphore_elem(=waiter) > semaphore > waiters(=진짜 스레드)
more_priority_sema(const struct list_elem *a,
				   const struct list_elem *b,
				   void *aux UNUSED)
{
	const struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	const struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);
	// #define list_entry(LIST_ELEM, STRUCT, MEMBER)
	const struct thread *ta = list_entry(list_back(&sa->semaphore.waiters), struct thread, elem);
	const struct thread *tb = list_entry(list_front(&sb->semaphore.waiters), struct thread, elem);

	return ta->priority > tb->priority;
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition *cond, struct lock *lock)
{ // 자동 락해제+ 시그널받기 기다리는 상태 만들기
	struct semaphore_elem waiter;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	sema_init(&waiter.semaphore, 0); // 내 전용 알람(깨우기용)
	/* 기존: list_push_back (&cond->waiters, &waiter.elem); */
	// cond_waiter목록에 자기자신 등록
	list_insert_ordered(&cond->waiters, &waiter.elem, more_priority, NULL); // 이렇게하면정렬안됨
	// 그냥 기존대로하고 뽑을때 차라리 바꾸자
	list_push_back(&cond->waiters, &waiter.elem);

	// 락 풀고 잠들기
	lock_release(lock);			  // 공유 락 풀기
	sema_down(&waiter.semaphore); // wait-list로 들어가기
	// 깨어나면 다시 락 잡고 리턴
	lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void // 상태만족됐다! -- cond->waiter중 하나 깨워 락 주기 (단, 인터럽트핸들러 안에서 쓰지 말기)
cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
	{
		// 이미 여기서 다 모여있을거라 여기서 sort해서 각 waiter 대기열에서 비교가능
		list_sort(&cond->waiters, more_priority_sema, NULL); // ###

		sema_up(&list_entry(list_pop_front(&cond->waiters),
							struct semaphore_elem, elem)
					 ->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}
