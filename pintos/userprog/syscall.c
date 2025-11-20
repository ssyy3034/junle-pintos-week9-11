#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/palloc.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

// 유효 메모리 접근 검사
bool is_valid_addr(void *pointer);

/* System call */
void halt();
void exit(int status);
bool create(const char *file, unsigned initial_size);
int open(const char *file);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);

/* file_sys 접근하기 위한 락 */
static struct lock file_lock;

/* fd - file 매핑 구조체 -> thread에서 list로 관리 */
struct fd_mapped {
    struct list_elem elem; /* List element. */
    int fd;
    struct file *file;
};

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
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
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
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    // file락 초기화
    lock_init(&file_lock);
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

    int status;            // exit()
    const char *file;      // exec(),create(),remove(),open()
    int fd;                // filesize(),read(),write(),seek(),tell(),close()
    void *buffer;          // read(),write()
    unsigned initial_size; // create()
    unsigned size;         // read(),write()

    switch (syscall_num)
    {
        case SYS_EXIT:
            /*
                출력되는 메시지 : 프로세스명, 종료코드(status)
                rdi : status
            */
            exit(f->R.rdi);
            break;

        case SYS_HALT:
            halt();
            break;

        case SYS_CREATE:
            file = f->R.rdi;
            initial_size = f->R.rsi;

            // 포인터 접근 시 검사
            if (!is_valid_addr(file))
            {
                exit(-1);
            }
            // 결과 저장
            f->R.rax = create(file, initial_size);
            break;

        case SYS_OPEN:
            file = f->R.rdi;

            // 포인터 접근 시 검사
            if (!is_valid_addr(file))
            {
                exit(-1);
            }
            f->R.rax = open(file);
            break;

        case SYS_READ:
            fd = f->R.rdi;
            buffer = f->R.rsi;
            size = f->R.rdx;

            // 포인터 접근 시 검사
            if (!is_valid_addr(buffer))
            {
                exit(-1);
            }

            f->R.rax = read(fd, buffer, size);
            break;
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
            fd = f->R.rdi;
            buffer = f->R.rsi;
            size = f->R.rdx;

            // 포인터 접근 시 검사
            if (!is_valid_addr(buffer))
            {
                exit(-1);
            }

            // 결과 rax에 저장
            f->R.rax = write(fd, buffer, size);

            break;
        default:
            exit(-1);
    }
}

void halt()
{
    power_off();
}

void exit(int status)
{
    thread_current()->exit_code = status;
    thread_exit();
}

bool create(const char *file, unsigned initial_size)
{
    /*
        - 새 파일 생성 후 크기를 initial_size로 설정
        - 성공 / 실패 리턴
        - open하는것은 아님!
    */

    lock_acquire(&file_lock);
    bool result = filesys_create(file, initial_size);
    lock_release(&file_lock);

    return result;
}

int open(const char *file)
{
    /*
        - 이름이 file인 파일을 연다. 성공하면 fd(file descriptor) 리턴, 실패하면 -1
        - 각 프로스세는 독립적인 fd 집합을 가진다(자식프로세스에 상속)
        - 하나의 파일을 여러번 열면 각기 다른 fd 리턴해줘야 함

        input : 파일 이름
        return : fd 번호

        -> 이름에 해당하는 파일 찾아서 열고,
        -> 현재 쓰레드에 해당 파일 - fd 번호 매핑해 넣고
        -> fd 번호를 리턴한다

        구현
        1. filesys의 filesys_open()으로 file 열고, file 구조체 리턴받음
            -> open() 실패하면 null포인터 리턴 -> null포인터면 open도 -1리턴
        2. 리턴받은 파일 + fd++ 매핑해서 fd_mapped 구조체 선언
            -> fd는 프로세스별로 관리. 새로운 fd 발급시마다 1 증가해서 발급
        3. fd_mapped를 현재 쓰레드의 fd_list에 추가
    */

    lock_acquire(&file_lock);
    struct file *open_file = filesys_open(file);
    lock_release(&file_lock);

    if (open_file == NULL)
    {
        return -1;
    }

    struct thread *curr = thread_current();

    // fd-file 매핑
    struct fd_mapped *mapper = palloc_get_page(PAL_ZERO);
    mapper->fd = curr->max_fd++;
    mapper->file = open_file;

    // 현재 쓰레드의 열린파일목록에 추가
    list_push_back(&curr->open_file_list, &mapper->elem);

    return mapper->fd;
}

int read(int fd, void *buffer, unsigned size)
{
    /*
        - fd로 열린 파일을 size 바이트를 읽어 buffer에 저장
        - fd가 0일 경우 키보드 입력. 이때는 input_getc()를 통해 읽음
    */
}

int write(int fd, const void *buffer, unsigned size)
{
    if (fd == 1)
    {
        putbuf((char *)buffer, size);
    }

    return size;
}

bool is_valid_addr(void *pointer)
{
    /*
        검사해야할 항목
        1. 포인터가 NULL 인지
        2. 포인터가 '유저영역' 인지
        3. 실제 할당받은 영역인지
        (추가) 4. 영역이 들어올 경우 시작점 / 끝점도 유효한지
    */
    return (pointer != NULL && is_user_vaddr(pointer) && pml4_get_page(thread_current()->pml4, pointer) != NULL);
}