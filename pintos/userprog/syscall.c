#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
// #include "lib/kernel/console.c"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

/* System call.
 *
 * 예전에는 시스템 콜 서비스가 인터럽트 핸들러에 의해 처리되었다
 * (예: 리눅스의 int 0x80). 하지만 x86-64에서는 제조사가
 * 시스템 콜을 요청하기 위한 더 효율적인 경로인 `syscall`
 * 명령어를 제공한다.
 *
 * syscall 명령어는 모델별 레지스터(Model Specific Register, MSR)에
 * 저장된 값들을 읽어서 동작한다. 자세한 내용은 매뉴얼을 참고하라.
 */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 |
                            ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 인터럽트 서비스 루틴은 syscall_entry가
     * 유저랜드 스택을 커널 모드 스택으로 교체하기 전까지는
     * 어떤 인터럽트도 처리하면 안 된다.
     * 따라서 우리는 FLAG_FL을 마스킹했다.
     * -> syscall 들어올 때는 일단 플래그들 정리하고(특히 인터럽트 끄고)
     * 		안전한 상태에서 커널 코드 시작
     * -> '항상 저렇게 설정 or 최초에만 저렇게 설정' 이 아니라,
     * -> 앞으로 syscall 명령 들어올 때 마다 저렇게 설정하도록 세팅
     * */
    write_msr(MSR_SYSCALL_MASK,
              FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // TODO: Your implementation goes here.
    /*
        rax : 시스템콜 번호
        rdi / rsi / rdx / r10 / r8 / r9 순서로 인자 저장

        여기서 시스템콜 핸들러 동작 정의
        1. 사용자 프로그램에서 시스템 콜 호출
        2. 어셈블리어 단계에서 rax에 '몇번 시스템 콜인지' 저장
        3. syscall_handler(여기)로 점프
        4. 여기서 rax번호로 어떤 시스템콜인지 분기하고, 각 로직 수행
            - 수행 결과(리턴)이 필요하면 rax 에 저장
    */
    int syscall_num = f->R.rax;
    // printf("syscall Num[hdh] : %d\n", syscall_num);
    switch (syscall_num)
    {
    case SYS_WRITE:
        /*
            write의 인자 : fd, buffer, size
            return : 실제로 쓴 바이트 수
            fd 1 : 콘솔
            -> 콘솔에 쓰는 코드는, size가 수백 바이트보다 크지 않은 한
                전체 buffer를 한 번의 putbuf() 호출로 출력해야 한다

            putbuf (const char *buffer, size_t n)

            - fd는 file open / close가 구현되기 전 까지는 사용 불가
            -> 일단 1인 경우만 생각하고 처리
        */
        int fd = f->R.rdi;
        char *buffer = f->R.rsi;
        unsigned size = f->R.rdx;
        int result = write(fd, buffer, size);

        // 결과 rax에 저장
        f->R.rax = result;
        break;
    case SYS_EXIT:
        /*
            출력되는 메시지 : 프로세스명, 종료코드(status)
            rdi : status
        */
        exit(f->R.rdi);
        break;
    case SYS_HALT:
        power_off();
        break;
    }

    // printf("system call!\n");
    // thread_exit();
}

int write(int fd, const void *buffer, unsigned size)
{
    // printf("write 진입. fd : %d\n", fd);
    if (fd == 1)
    {
        putbuf((char *)buffer, size);
    }

    return size;
}

void exit(int status)
{
    thread_current()->exit_code = status;
    thread_exit();
}
