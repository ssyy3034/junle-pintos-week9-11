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

/* 세마포어에 대한 down 또는 "P" 연산.
   SEMA의 값이 양수가 될 때까지 기다렸다가,
   양수가 되면 그 값을 원자적으로(atomically) 1 감소시킨다.

   이 함수는 실행 도중 스레드가 잠들(sleep) 수 있으므로
   인터럽트 핸들러 내부에서 호출되면 안 된다.
   인터럽트를 비활성화한 상태에서 이 함수를 호출하는 것은 가능하지만,
   만약 이 함수가 잠들게 되면, 다음에 스케줄되는 스레드가
   아마도 인터럽트를 다시 활성화할 것이다.
   이것이 sema_down 함수이다.

   기존 : wait_list 제일 뒤에 삽입
   수정 : wail_list 삽입 후 우선순위 따라 정렬
*/
void sema_down(struct semaphore *sema)
{
   enum intr_level old_level;

   ASSERT(sema != NULL);
   ASSERT(!intr_context());

   old_level = intr_disable();
   while (sema->value == 0)
   {
      // list_push_back (&sema->waiters, &thread_current ()->elem);
      // wait_list에 들어감
      list_insert_ordered(&sema->waiters, &thread_current()->elem, greater_priority, NULL);

      thread_block();
   }
   sema->value--;
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

/* 세마포어에 대한 up 또는 "V" 연산.
   SEMA의 값을 1 증가시키고,
   SEMA를 기다리고 있는 스레드가 있다면 그 중 하나를 깨운다.

   이 함수는 인터럽트 핸들러 내부에서도 호출될 수 있다.

   -> 깨울 때 제일 앞에있는거 깨운다
   -> 정렬 되어있다면 괜찮지 않을까?
   -> 근데 정렬하는 순간은 'sema_down' 걸 때
   -> wait_list 들어간 후 priority가 바뀌면 대응 불가
   -> 꺼내기 전 한번 더 정렬하고 꺼낼까?
   -> wait_list 들어갈 때 정렬하고 들어가고 그 뒤로 priority 안바뀌니 괜찮다

   쓰레드 unblock 한 후, 풀어준 애의 우선순위 검사해 나보다 높으면 양보
   조건)
      - 인터럽트 문맥이 아닐 때
         -> 인터럽트 핸들러 진행 도중 문맥전환이 발생하면 안됨!
      - 기존에 인터럽트가 꺼져있는 상황일 때
         -> 기존에 인터럽트가 켜져있다 == 콜러가 공유자원 접근해서 작업한다
         -> 그때 문맥전환 발생하면 문제가 생김
*/
void sema_up(struct semaphore *sema)
{
   enum intr_level old_level;

   ASSERT(sema != NULL);
   struct thread *wokeup_thread;

   old_level = intr_disable();
   // list_sort(&sema->waiters, push_priority, NULL);
   if (!list_empty(&sema->waiters))
   {
      // donation 때문에 우선순위 변경이 생겼을 수도 있기 때문에 정렬
      list_sort(&sema->waiters, greater_priority, NULL);
      wokeup_thread = list_entry(list_pop_front(&sema->waiters), struct thread, elem);
      thread_unblock(wokeup_thread);
   }
   sema->value++;
   intr_set_level(old_level);

   if (wokeup_thread != NULL && !intr_context() && old_level == INTR_ON)
   {
      if (wokeup_thread->priority > thread_current()->priority)
      {
         thread_yield();
      }
   }
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

/*
   우선순위 기부
   - 러닝중인 쓰레드가 락을 취득하려 할 때,
      우선순위가 더 낮은 쓰레드가 락을 점유하고 있으면
      락의 wait_list에 자신을 등록한 후
      if(helder의 우선순위가 자신보다 낮다면){
         그 쓰레드에게 우선순위를 기부하고 락이 풀리길 기다린다.
      }
   - 기부 방식
      러닝중인 쓰레드가 donate_thread()를 호출
      donate_thread(){
         러닝 쓰레드의 priority > 홀더의 priority{
            홀더의 effective_priority = 러닝 쓰레드의 priority
         }
         if(얘가 다른 락을 기다리고 있었다면){
            그 락을 갖고있는 쓰레드에게 donate_thread()
         }
      }
         -> 이렇게 하는 이유 : 그 쓰레드보다 높은 우선순위의 쓰레드가 기다리기때문

   - 기부 후
      기부받은 쓰레드가 running되고 락을 해제
      락 해제 후 undonate_thread() 호출
      아직 갖고있는 락이 있다면)
         자신이 갖고있는 락 목록중 가장 높은 우선순위의 priority로 변경
      갖고있는 락이 없다면)
         effective_priority 를 기본 priority로 변경

   1. thread에 추가
      - donation_priority
      - holding : 잡고있는 락 목록
      - waiting : 대기하고있는 락 목록

   2. 함수 추가
      - donate_thread(* thread)
      - undonate_thread(* thread)
*/

void donate_priority(struct thread *thread)
{
   /*
      러닝 쓰레드의 priority > 홀더의 priority{
         홀더의 effective_priority = 러닝 쓰레드의 priority
      }
      if(얘가 다른 락을 기다리고 있었다면){
         그 락을 갖고있는 쓰레드에게 donate_thread()
      }
   */

   // int base_priority = thread -> donation_priority;
   struct thread *current = thread_current();

   thread->donation_priority = current->donation_priority;

   struct lock *waiting_lock = thread->waiting_lock;
   // struct list_elem *e;

   // if(current -> donation_priority > base_priority){
   // }
   if (waiting_lock != NULL && waiting_lock->holder != NULL && waiting_lock->holder != current)
   {
      // 홀더가 다른 락 대기중이었을 때
      // -> 그 락의 우선순위가 지금 락보다 낮다 -> 우선순위 기부
      // e : waiting 하고있는 락
      // 그 락의 홀더의 우선순위 검사
      // for(e = list_begin(waiting); e != list_end(waiting); e = list_next(e)){
      //    struct thread *waiting_lock_holder = list_entry(e, struct thread, elem);
      //    if(thread -> donation_priority > waiting_lock_holder -> donation_priority){
      //          donate_priority(waiting_lock_holder);
      //    }
      // }
      if (thread->donation_priority > waiting_lock->holder->donation_priority)
      {
         donate_priority(waiting_lock->holder);
      }
   }
}

//   struct list *holding = &thread -> holding;
//   struct list_elem *e;
//    for(e = list_begin(holding); e != list_end(holding) ; e = list_next(e)){
//       struct lock *holdingLock = list_entry(e, struct lock, elem);
//       struct list *wait_list = list_sort(&(&holdingLock -> semaphore) -> waiters, greater_priority, NULL);
//    }

/* LOCK을 초기화한다. 하나의 락은 어떤 시점이든
   동시에 최대 한 개의 스레드만이 보유할 수 있다.
   우리의 락은 "재귀적(recursive)" 락이 아니므로,
   현재 락을 쥐고 있는 스레드가
   같은 락을 다시 획득하려 하는 것은 오류이다.

   락은 초기 값이 1인 세마포어의 한 특수 형태라고 볼 수 있다.
   하지만 락과 그런 세마포어 사이에는 두 가지 차이가 있다.
   첫째, 세마포어는 값이 1보다 클 수도 있지만,
   락은 어느 순간이든 오직 하나의 스레드만이 소유할 수 있다.
   둘째, 세마포어에는 '소유자(owner)'라는 개념이 없어서
   한 스레드가 세마포어를 down(획득)하고
   다른 스레드가 up(해제)할 수 있다.
   반면 락은 동일한 스레드가 락을 획득(acquire)하고
   해제(release)해야만 한다.

   만약 이런 제약들이 너무 불편하게 느껴진다면,
   그건 락 대신 세마포어를 사용하는 것이 더 적절하다는
   좋은 신호라고 볼 수 있다. */
void lock_init(struct lock *lock)
{
   ASSERT(lock != NULL);

   lock->holder = NULL;
   sema_init(&lock->semaphore, 1);
}

/* LOCK을 획득한다. 필요하다면 LOCK이 사용 가능해질 때까지
   (잠들면서) 기다린다. 현재 스레드가 이미 이 LOCK을
   가지고 있어서는 안 된다.

   이 함수는 실행 도중 sleep(블록)할 수 있으므로
   인터럽트 핸들러 안에서 호출되어서는 안 된다.
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수는 있지만,
   만약 sleep이 필요하다면 인터럽트는 다시 켜지게 된다. */
void lock_acquire(struct lock *lock)
{
   ASSERT(lock != NULL);
   ASSERT(!intr_context());
   ASSERT(!lock_held_by_current_thread(lock));

   // 락의 holder가 있는지 조회
   // 있다 -> 자신보다 우선순위가 낮은지 체크
   // 낮다 -> 우선순위 기부
   if (lock->holder != NULL)
   {
      if (thread_current()->donation_priority > lock->holder->donation_priority)
      {
         thread_current()->waiting_lock = lock;
         donate_priority(lock->holder);
      }
   }
   thread_current()->waiting_lock = lock;
   sema_down(&lock->semaphore);
   lock->holder = thread_current();
   thread_current()->waiting_lock = NULL;
   list_push_back(&thread_current()->holding, &lock->elem);
}

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

/*
   - 기부 후
      기부받은 쓰레드가 running되고 락을 해제
      락 해제 후 undonate_thread() 호출
      아직 갖고있는 락이 있다면)
         자신이 갖고있는 락 목록중 가장 높은 우선순위의 priority로 변경
      갖고있는 락이 없다면)
         effective_priority 를 기본 priority로 변경
*/
void retrieve_priority(struct lock *release_lock)
{
   struct thread *current = thread_current(); // 얘가 홀더
   struct list *holding = &current->holding;

   // 현재 락 제거
   list_remove(&release_lock->elem);

   current->donation_priority = current->priority;

   if (!list_empty(holding))
   {
      struct list_elem *e; // 여기서 e는 holding 리스트 원소
      for (e = list_begin(holding); e != list_end(holding); e = list_next(e))
      {
         // 홀더가 갖고있던 락
         struct lock *holdingLock = list_entry(e, struct lock, elem);

         // 그 락의 대기열 중 가장높은 우선순위를 가져옴
         // 가져오기 전에 정렬하고 가져와야함(우선순위 바꼈을수도 있으니까)
         struct list *waiter = &holdingLock->semaphore.waiters;

         if (!list_empty(waiter))
         {
            list_sort(waiter, greater_priority, NULL);
            struct thread *highest_priority = list_entry(
                list_begin(waiter),
                struct thread, elem);
            if (highest_priority->donation_priority > current->donation_priority)
            {
               current->donation_priority = highest_priority->donation_priority;
            }
         }
      }
   }
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

   retrieve_priority(lock);

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
   struct list_elem elem;      /* List element. */
   struct semaphore semaphore; /* This semaphore. */
};

/* 조건 변수 COND를 초기화한다.
   조건 변수는 한쪽 코드에서 어떤 “조건(condition)”이 충족되었음을
   신호(signal)로 보내고, 그와 협력하는 다른 코드가
   그 신호를 받아서(깨워져서) 그에 따라 동작할 수 있게 해준다. */
void cond_init(struct condition *cond)
{
   ASSERT(cond != NULL);

   list_init(&cond->waiters);
}

/* LOCK을 원자적으로(atomic하게) 해제한 뒤,
   다른 코드에서 COND가 signal 될 때까지 기다린다.
   COND가 signal 된 후에는, 이 함수에서 반환되기 전에
   LOCK을 다시 획득한다. 이 함수를 호출하기 전에
   반드시 LOCK을 보유하고 있어야 한다.

   이 함수가 구현하는 모니터(monitor)는 "Hoare" 스타일이 아니라
   "Mesa" 스타일이다. 즉, signal을 보내는 쪽과 받는 쪽이
   원자적으로 한 덩어리로 동작하지 않는다. 따라서 보통
   wait가 끝난 뒤에는 조건을 다시 확인해야 하고,
   필요하다면 다시 wait 해야 한다.

   하나의 condition variable은 오직 하나의 lock에만
   연관될 수 있지만, 하나의 lock은 여러 개의
   condition variable과 연관될 수 있다.
   즉, lock과 condition variable 사이에는
   1 대 다(1-to-many) 관계가 성립한다.

   이 함수는 실행 도중 sleep 할 수 있으므로
   인터럽트 핸들러 안에서 호출되면 안 된다.
   인터럽트가 비활성화된 상태에서 호출할 수는 있지만,
   우리가 sleep 해야 하는 상황이라면
   인터럽트는 다시 켜지게 된다. */

void cond_wait(struct condition *cond, struct lock *lock)
{
   struct semaphore_elem waiter;

   ASSERT(cond != NULL);
   ASSERT(lock != NULL);
   ASSERT(!intr_context());
   ASSERT(lock_held_by_current_thread(lock));

   sema_init(&waiter.semaphore, 0);
   list_push_back(&cond->waiters, &waiter.elem);
   // list_insert_ordered(&cond->waiters, &waiter.elem, greater_priority, NULL);
   lock_release(lock);
   sema_down(&waiter.semaphore);
   lock_acquire(lock);
}
/* LOCK으로 보호되는 COND 위에서 어떤 스레드들이 기다리고 있다면,
   이 함수는 그들 중 하나에게 신호를 보내서
   기다림에서 깨어나도록 한다.
   이 함수를 호출하기 전에 반드시 LOCK을 보유하고 있어야 한다.

   인터럽트 핸들러는 lock을 획득할 수 없으므로,
   인터럽트 핸들러 안에서 조건 변수에 signal을 보내려고 하는 것은
   의미가 없다.

   깨울때 정렬 vs 들어갈 때 정렬
   - 기존의 쓰레드 우선순위 검사하는 비교함수를 사용하지 못하는 이유
      -> 기존엔 쓰레드의 elem을 넘겼기 때문에, elem -> 쓰레드 -> priority
      -> 지금은 cond_wait에서 만든 semaphore_elem의 elem을 넘긴다
         -> elem을 쓰레드로 해석하고 priority를 조회하려 해도 안됨(쓰레드가 아니니까!)
      -> 쓰레드는 어디 저장되냐?
      -> semaphore_elem -> semaphore -> waiters -> thread.elem
      -> elem을 세마포어_elem으로 변환 후 타고 들어가서 priority 비교
*/
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
   ASSERT(cond != NULL);
   ASSERT(lock != NULL);
   ASSERT(!intr_context());
   ASSERT(lock_held_by_current_thread(lock));

   if (!list_empty(&cond->waiters))
   {
      // 정렬 하고 풀기
      // list_sort (struct list *list, list_less_func *less, void *aux)
      list_sort(&cond->waiters, greater_priority_cond, NULL);
      sema_up(&list_entry(list_pop_front(&cond->waiters),
                          struct semaphore_elem, elem)
                   ->semaphore);
   }
}

// semaphore_elem의 우선순위 정렬 기준
bool greater_priority_cond(const struct list_elem *a,
                           const struct list_elem *b,
                           void *aux)
{

   struct semaphore_elem *a_sema = list_entry(a, struct semaphore_elem, elem);
   struct semaphore_elem *b_sema = list_entry(b, struct semaphore_elem, elem);

   struct thread *a_thread = list_entry(list_front(&(a_sema->semaphore.waiters)), struct thread, elem);
   struct thread *b_thread = list_entry(list_front(&(b_sema->semaphore.waiters)), struct thread, elem);
   if (a_thread->donation_priority > b_thread->donation_priority)
   {
      return true;
   }
   else
   {
      return false;
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
