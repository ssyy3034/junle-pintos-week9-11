/* Tests that cond_signal() wakes up the highest-priority thread
   waiting in cond_wait(). */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static thread_func priority_condvar_thread;
static struct lock lock;		   // 보호대상: 대기자리스트를 넣고 뺴는 모든 연산(주도권점유개념)
static struct condition condition; // list waiters

void test_priority_condvar(void)
{
	int i;

	/* This test does not work with the MLFQS. */
	ASSERT(!thread_mlfqs);

	lock_init(&lock);	   // 락 구조체 생성 (holder, semaphore-1)
	cond_init(&condition); // 컨드 구조체 생성 (list waiters)

	thread_set_priority(PRI_MIN); // 현재 스레드 우선순위 꼴찌 만들기
	for (i = 0; i < 10; i++)
	{
		int priority = PRI_DEFAULT - (i + 7) % 10 - 1;
		char name[16];
		snprintf(name, sizeof name, "priority %d", priority);
		thread_create(name, priority, priority_condvar_thread, NULL);
		// 생성되자마자 우선순위 현스레드보다 높으니 즉시 실행시작
	} // 각자 스레드가 선점하다가 결국 다 잠들고 다 wait-list에 들어간채로 다시 메인스레드로 흐름 돌아옴

	for (i = 0; i < 10; i++)
	{
		lock_acquire(&lock);
		msg("Signaling...");
		cond_signal(&condition, &lock);
		lock_release(&lock);
	}
}

static void
priority_condvar_thread(void *aux UNUSED)
{
	msg("Thread %s starting.", thread_name());
	lock_acquire(&lock);		  // sema_down : 1)락 획득 시도  (가장먼저cpu차지한 최고 우선순위 스레드: 1->0만들며 락 획득 성공)
	cond_wait(&condition, &lock); // 2)조건 대기  (:락 해제-> wait_list에들어감(blocked))

	msg("Thread %s woke up.", thread_name());
	lock_release(&lock); // 3)락 해제
}
/*
cond_wait (struct condition *cond, struct lock *lock) { //자동 락해제+ 시그널받기 기다리는 상태 만들기
	struct semaphore_elem waiter;***

====

struct semaphore_elem {
	struct list_elem elem;
	struct semaphore semaphore;      //개인용 세마포어(깨우기/재우기 역할)
};

*/