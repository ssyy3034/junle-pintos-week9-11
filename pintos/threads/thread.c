#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 목록.
	즉, 실행할 준비는 되었지만 실제로 실행 중은 아닌 프로세스들이다. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환한다.
 * CPU의 스택 포인터 `rsp` 값을 읽은 뒤,
 * 그 값을 페이지의 시작 주소가 되도록 아래 방향으로 내림(round down)한다.
 * `struct thread`는 항상 페이지의 처음에 위치하고,
 * 스택 포인터는 그 페이지의 중간 어딘가를 가리키고 있기 때문에,
 * 이렇게 하면 현재 스레드를 찾아낼 수 있다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화해 선점형 스레드 스케줄링을 시작한다.
   또한 idle 스레드를 생성한다. */
void
thread_start (void) {
	/* idle 쓰레드 생성 */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* idle쓰레드 초기화 완료 대기 */
	sema_down (&idle_started);
}

/* 이 함수는 매 타이머 틱마다 타이머 인터럽트 핸들러가 호출한다. 
	따라서 외부 인터럽트 컨텍스트(인터럽트 처리 중)에서 동작한다.

	-> 쓰레드 틱 올리고 TIME_SLICE 초과했는지 검사
	-> 초과했을 경우 문맥전환 플래그 On
*/
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	// TIME_SLICE 넘어가면 intr_yield_on_return() 호출해서
	// 문맥전환 플래그 On
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 PRIORITY(우선순위)와 이름 NAME을 가진 새로운
   커널 스레드를 생성한다. 이 스레드는 FUNCTION을 실행하며,
   그 인자로 AUX를 전달한다. 생성된 스레드는 레디 큐(ready queue)에
   추가된다. 새로운 스레드의 식별자(thread identifier)를
   반환하며, 생성에 실패하면 TID_ERROR를 반환한다.

   thread_start()가 호출된 이후라면, 새 스레드는
   thread_create()가 반환되기 전에 스케줄될 수도 있다.
   심지어 thread_create()가 반환되기 전에 종료될 수도 있다.
   반대로, 새 스레드가 스케줄되기 전에
   기존 스레드가 얼마든지 오래 실행될 수도 있다.
   실행 순서를 보장해야 한다면 세마포어나 다른 형태의
   동기화 기법을 사용해야 한다.

   현재 제공된 코드는 새 스레드의 `priority` 멤버를
   PRIORITY 값으로 설정만 할 뿐,
   실제 우선순위 스케줄링은 구현되어 있지 않다.
   우선순위 스케줄링은 문제 1-3의 목표이다. 

   -> 쓰레드를 생성 후 레디 큐에 넣을 때 레디큐 정렬 O (unblock)
   -> 문맥전환 발생해서 런 -> 레디큐 할때 또 정렬 O (schedule)
   -> 블록상태에서 다시 레디상태 들어갈 때 정렬 O (unblock)
   -> 세마포어 웨이트리스트에 들어갈 때도 정렬
   -> 락의 세마포어 웨이트리스트도 정렬
*/
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	// unblock된 쓰레드의 우선순위가 running중인 스레드의 우선순위보다 높다면
	// 현재쓰레드 양보
	// ASSERT (!intr_context ());
	// ASSERT (intr_get_level () == INTR_OFF);

	if(t -> donation_priority > thread_current ()-> donation_priority){
		thread_yield();
	}

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단(blocked) 상태에 있는 스레드 T를
	실행 준비(ready-to-run) 상태로 전환한다.
	T가 blocked 상태가 아니라면 이것은 오류이다.
	(실행 중인 스레드를 ready 상태로 만들고 싶다면
	thread_yield()를 사용하라.)

	이 함수는 현재 실행 중인 스레드를 선점(preempt)하지 않는다.
	이는 중요한 성질일 수 있는데, 호출자가 스스로 인터럽트를
	비활성화한 상태라면, 스레드를 깨우는 동작과
	그 외의 데이터 갱신을 원자적으로(atomic하게)
	수행할 수 있다고 기대할 수 있기 때문이다. 

	블록상태의 쓰레드를
	기존 : list_push_back 으로 제일 뒤에 삽입
	수정 : list_insert_ordered 로 정렬하며 삽입
	후 ready상태로 바꾼다

	ready_list에 들어갈 때, 들어가는 쓰레드의 우선순위가 running쓰레드보다 높다면
	현재 쓰레드를 양보해야한다
	-> 틀린말. thread_unblock 에서는 unblock 동작만 해야지,
		쓰레드 변경을 해서는 안된다.
	-> 왜?
		unblock()을 호출하는 콜러에서
		intr_disable()
		unblock()
		.. 추가작업
		intr_set_level(old)
		이렇게 unblock() 후 추가적인 작업을 기대하는 경우가 존재
		-> 이때 바로 yield() 해버리면 문제가 생기기 때문
*/
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem);
	list_insert_ordered(&ready_list, &t->elem, greater_priority, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

// 우선순위 정렬 기준
bool
greater_priority(const struct list_elem *a,
			const struct list_elem *b,
			void *aux){

	struct thread *a_thread = list_entry(a, struct thread, elem);
	struct thread *b_thread = list_entry(b, struct thread, elem);

	if (a_thread -> donation_priority > b_thread ->donation_priority){
		return true;
} else {
		return false;
	}
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}
test;
/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif
	/* 우리의 상태를 ‘dying’으로만 설정하고, 다른 프로세스를 스케줄합니다.
	실제 파괴는 schedule_tail() 호출 동안 이루어집니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 현재 스레드는 잠재우지(sleep) 않으며,
스케줄러의 판단에 따라 곧바로 다시 스케줄될 수도 있습니다. 

기존 : 일단 현재쓰레드 레디큐에 넣고 다시 스케줄
수정 후 : 우선순위 고려해서 insert_ordered로 삽입
*/
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		// list의 맨 뒤에 넣음
		// list_push_back (&ready_list, &curr->elem);
		list_insert_ordered(&ready_list, &curr->elem, greater_priority, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정한다. 
	-> 우선순위 변경 후 레디 큐에 우선순위 더 높은애가 있으면 양보해야함
*/
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
	// 기부자 목록이 없으면 donation_priority도 new_priority로 설정
	if(list_empty(&thread_current() -> holding)){
		thread_current() -> donation_priority = new_priority;
	}

	enum intr_level old_level = intr_disable();

	if(!list_empty(&ready_list)){
		// 우선순위 높은애 있는지 체크
		struct thread *head_thread = list_entry(list_begin(&ready_list), struct thread, elem);
		int highest_priority = head_thread -> donation_priority;
		if(highest_priority > new_priority){
			// 쓰레드 양보
			thread_yield();
		}
	}
	intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->donation_priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	list_init(&t -> holding);
	t->waiting_lock = NULL;          
	t->donation_priority = priority; 
}

/* 다음에 스케줄될 스레드를 선택하여 반환한다.
   실행 큐(run queue)에 스레드가 있다면,
   그 중 하나를 반드시 반환해야 한다.
   (현재 실행 중인 스레드가 계속 실행될 수 있다면,
   그 스레드는 실행 큐 안에 들어 있게 된다.)
   실행 큐가 비어 있다면 idle_thread를 반환한다. 

   pop_front로 제일 앞에걸 사용
   -> 어차피 우선순위 순으로 정렬되어 있을태니 괜찮을듯?
   -> 그럴려면 우선순위 변경을 하더라도 정렬상태가 유지되어야겠지?
   -> 레디 큐는 우선순위 변경될 때 재정렬
   -> 세마포어의 레디큐는 sema_up 할 때 정렬 후 뽑아감
*/
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else{
		list_sort(&ready_list, greater_priority, NULL);
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
	}

}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새로운 프로세스를 스케줄합니다. 진입 시 인터럽트는 꺼져 있어야 합니다.
 * 이 함수는 현재 스레드의 상태를 status로 바꾼 뒤,
 * 실행할 다른 스레드를 찾아 그 스레드로 전환합니다.
 * schedule() 안에서는 printf()를 호출하는 것이 안전하지 않습니다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);		// 인터럽트 비활성 여부 체크
	ASSERT (thread_current()->status == THREAD_RUNNING);	// 현재 스레드가 실행상태인가
	while (!list_empty (&destruction_req)) {	// destruction_req : 파괴 대기 스레드
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();	// 다음 실행할 쓰레드

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
