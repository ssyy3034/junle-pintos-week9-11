#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "devices/input.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
// 추가한 헬퍼함수들 ========
bool is_valid_addr(const void *u_add);
// syscall 함수들 ========
static void sys_halt(void);                                      // 완료
static void sys_exit(int status);                                // 완료
static bool sys_create(const char *file, unsigned initial_size); // 완료
static int sys_open(const char *file);                           // 완료
static int sys_read(int fd, void *buffer, unsigned length);
static int sys_write(int fd, const void *buffer, unsigned length); // 완료
// fd_table 관련 헬퍼함수들 ========
static int create_fd(struct file *f); // 이거 왜 오류나지. 구조체 때문인듯.
static struct file *get_file_from_fd(int fd);
static struct lock lock;
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

void syscall_init(void)
{
    write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
    write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* The interrupt service rountine should not serve any interrupts
     * until the syscall_entry swaps the userland stack to the kernel
     * mode stack. Therefore, we masked the FLAG_FL. */
    write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

    lock_init(&lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // 1) syscall 번호 받기 ========
    uint64_t syscall_no = f->R.rax;

    int status;            // exit()
    const char *file;      // exec(),create(),remove(),open()
    int fd;                // filesize(),read(),write(),seek(),tell(),close()
    void *buffer;          // read(),write()
    unsigned initial_size; // create()
    unsigned length;       // read(),write()

    // 2) syscall 번호별 인자개수만큼 받고 actions 처리 ========
    switch (syscall_no)
    {
        case SYS_HALT:
            sys_halt();
            break;

        case SYS_EXIT:
            status = f->R.rdi;
            sys_exit(status);
            break;

        case SYS_CREATE:
            if (f->R.rdi == NULL)
            {
                sys_exit(-1);
            }
            file = f->R.rdi;
            initial_size = f->R.rsi;

            f->R.rax = sys_create(file, initial_size);
            break;
        case SYS_OPEN:
            file = f->R.rdi;
            f->R.rax = sys_open(file);
            break;

        case SYS_READ:
            fd = f->R.rdi;
            buffer = f->R.rsi;
            length = f->R.rdx;

            sys_read(fd, buffer, length);
            break;

        case SYS_WRITE:
            fd = f->R.rdi;
            buffer = f->R.rsi;
            length = f->R.rdx;

            f->R.rax = sys_write(fd, buffer, length);
            break;

        default:
            sys_exit(-1);
            break;
    }

    // thread_exit(); ->시스템콜 끝날때마다 무조건 현재 스레드(=프로세스) 종료
}
// 시스템콜 함수들 ========================
static void sys_halt(void)
{
    power_off();
}

static void sys_exit(int status)
{
    struct thread *cur = thread_current();
    cur->exit_code = status;
    thread_exit();
}

static bool sys_create(const char *file, unsigned initial_size)
{
    if (!is_valid_addr(file))
    {
        sys_exit(-1);
    }
    // filesys 접근할때 락 걸기
    lock_acquire(&lock);
    if (filesys_create(file, initial_size))
    { //: Creates a file named NAME with the given INITIAL_SIZE.
        lock_release(&lock);
        return 1;
    } //> filesys_create()에서 file name == '\0' (empty여부) 확인해줌
    else
    {
        lock_release(&lock);
        return 0;
    }
}

static int sys_open(const char *file)
{
    // /* Opens a file for the given INODE, of which it takes ownership,
    //  * and returns the new file.  Returns a null pointer if an
    //  * allocation fails or if INODE is null. */
    if (!is_valid_addr(file))
    {
        sys_exit(-1);
    }
    // 남은것: lock구현해야한다는데 어케하지
    if (file == NULL)
    {
        sys_exit(-1);
    } else if (file == "")
    {
        return -1;
    }
    // file 이름으로 file 열기(filesys_open()) + 파일구조체 받기(inode포함)
    lock_acquire(&lock);
    struct file *f = filesys_open(file); // inode, pos, deny_write(bool)
    lock_release(&lock);
    if (f == NULL)
    {
        return -1;
    }
    // fd 생성해 fd-table에 file구조체내용이랑 같이 저장해두기 -> 해당 fd 반환(int형)
    // fd_table 할당하는걸 못함
    int new_fd = create_fd(f);
    return new_fd; // 실패하면 -1
}

static int sys_read(int fd, void *buffer, unsigned length)
{
}

static int sys_write(int fd, const void *buffer, unsigned length)
{
}
// user v_memory access ========================
bool is_valid_addr(const void *u_add)
{
    uint64_t *pml4 = thread_current()->pml4;
    //  1) 주소가 유저가상메모리영역인지 && pt에 존재하는지
    if (!is_user_vaddr(u_add) || (pml4_get_page(pml4, u_add) == NULL))
    {
        // sys_exit(-1);
        return 0;
    }
    return 1;

    /* 추가해야할 검사요소: 넘겨준 버퍼가 페이지 경계에 걸쳐있을때->끝부분도 유효페이지에 존재하는지 */
}
// open() -> fd 생성 관련 ========================
static int create_fd(struct file *f) // 현재 프로세스의 fd_table 중 빈 fd찾아 file구조체 넣고 fd 리턴
{
    struct thread *current_t = thread_current();
    struct file **ft = current_t->fd_table;
    for (int i = 2; i < FD_MAX; i++)
    {
        if (ft[i] == NULL)
        {
            ft[i] = f;
            return i;
        }
    }
    return -1;
} // struct file **fd_table;

static struct file *get_file_from_fd(int fd)
{
    if (fd < FD_MIN || fd > FD_MAX)
    {
        return NULL;
    }
    struct thread *cur_thread = thread_current();
    struct file **fdt = cur_thread->fd_table;

    return fdt[fd];
}