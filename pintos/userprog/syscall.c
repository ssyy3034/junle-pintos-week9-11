#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED) {
    // TODO: Your implementation goes here.
    printf("system call!\n");
    thread_exit();
}
/*
 * =================================================
 * clang-tidy (정적 분석) 작동 테스트용 함수
 * =================================================
 * 이 함수를 process.c 또는 syscall.c 상단에 잠시 추가해 보세요.
 * VS Code에 물결무늬 경고가 표시되어야 합니다.
 */
void test_clang_tidy_warnings(void) {
    int x; // 버그 1: 초기화되지 않은 변수
    int y = 0;
    int *ptr = NULL; // 버그 2: NULL 포인터

    /* * [물결무늬 1] clang-analyzer-core.uninitialized.UndefReturn
     * 'x'가 초기화되지 않은 상태에서 사용될 수 있다고 경고해야 합니다.
     */
    if (x > 10) {
        y = 5;
    }

    /* * [물결무늬 2] clang-analyzer-core.NullDereference
     * 'ptr'이 NULL일 수 있는데 역참조한다고 경고해야 합니다.
     * (Pintos 커널 패닉의 주 원인!)
     */
    *ptr = 1;

    /* * [물결무늬 3] bugprone-assignment-in-if-condition
     * if문 안에서 '==' (비교) 대신 '=' (할당)을 사용했다고 경고해야 합니다.
     */
    if (y = 5) {
        y = 10;
    }

    /* * [물결무늬 4] bugprone-unused-variable
     * 변수를 선언만 하고 사용하지 않았다고 경고해야 합니다.
     */
    int unused_var = 100;
}
/* ================================================= */