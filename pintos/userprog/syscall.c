#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/init.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#define FD_MIN 2
#define FD_MAX 128

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

// syscall 함수들 ========
static void sys_halt(void);                                        // 완료
static void sys_exit(int status);                                  // 완료
static int sys_write(int fd, const void *buffer, unsigned length); // 완료
static bool sys_create(const char *file, unsigned initial_size);   // 완료
static int sys_open(const char *file);                             // 완료
// helper 함수들 ========
void check_valid_addr(void *addr);
static int create_fd(struct file *f);

struct file **local_fdt;
struct lock *file_lock;
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
    lock_init(&file_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    // 1) syscall 번호 받기 ========
    uint64_t syscall_no = f->R.rax;
    int fd;
    int status;
    void *buffer;
    unsigned length;
    char *file;
    size_t initial_size;

    printf("\n\n\ngdasfdsf\n\n\n");
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
            file = f->R.rdi;
            initial_size = f->R.rsi;

            f->R.rax = sys_create(file, initial_size);
            break;

        case SYS_OPEN:
            file = f->R.rdi;

            f->R.rax = sys_open(file);
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
    check_valid_addr(file);
    if (file == NULL)
    {
        return false;
    }
    lock_acquire(&file_lock);
    bool is_created = filesys_create(file, initial_size);
    lock_release(&file_lock);

    return is_created;
}

static int sys_open(const char *file)
{
    check_valid_addr(file);

    if (*file == '\0' || file == NULL)
    {
        return -1;
    }

    lock_acquire(&file_lock);
    struct file *open_file = filesys_open(file);
    lock_release(&file_lock);

    if (open_file == NULL)
    {
        return -1;
    }

    int fd = create_fd(open_file);
    return fd;
}

static int sys_write(int fd, const void *buffer, unsigned length)
{
    if (fd == 1)
    {
        putbuf((const char *)buffer, (size_t)length);
    }
    return length;
}

// helper 함수들 =====
void check_valid_addr(void *addr)
{
    if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
    {
        sys_exit(-1);
    }
}
static int create_fd(struct file *f)
{
    local_fdt = thread_current()->file_descriptor_table;

    for (int i = FD_MIN; i < FD_MAX; i++)
    {
        if (local_fdt[i] == NULL)
        {
            local_fdt[i] = f;
            return i;
        }
    }
    file_close(f);
    return -1;
}