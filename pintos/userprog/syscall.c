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

void syscall_entry(void);
void syscall_handler(struct intr_frame *);

// syscall 함수들 ========
static void sys_halt(void);                                        // 완료
static void sys_exit(int status);                                  // 완료
static int sys_write(int fd, const void *buffer, unsigned length); // 완료
static bool sys_create(const char *file, unsigned initial_size);
static int sys_open(const char *file);
static void sys_close(int fd);
static int sys_read(int fd, void *buffer, unsigned length);
static int sys_filesize(int fd);

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
}

void check(void *addr)
{
    if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL)
    {
        sys_exit(-1);
    }
}
void check_start_to_end(void *addr, int size)
{
    check(addr);
    check((char *)addr + size - 1);
}
struct file **local_fdt;
/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
    int fd;
    void *buffer;
    unsigned length;
    char *file;
    size_t initial_size;

    switch (f->R.rax)
    {
        case SYS_HALT:
            sys_halt();
            break;

        case SYS_EXIT:
            sys_exit(f->R.rdi);
            break;

        case SYS_WRITE:

            fd = f->R.rdi;
            buffer = f->R.rsi;
            length = f->R.rdx;

            f->R.rax = sys_write(fd, buffer, length);
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
        case SYS_CLOSE:
            fd = f->R.rdi;
            sys_close(fd);
            break;
        case SYS_READ:
            fd = f->R.rdi;
            buffer = f->R.rsi;
            length = f->R.rdx;

            f->R.rax = sys_read(fd, buffer, length);
            break;
        case SYS_FILESIZE:
            fd = f->R.rdi;

            f->R.rax = sys_filesize(fd);
            break;
        default:
            thread_exit();
            break;
    }
    // TODO: Your implementation goes here.
}

/*==============================================
===================시스템콜 함수들==================
================================================*/

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

static int sys_write(int fd, const void *buffer, unsigned length)
{
    check_start_to_end(buffer, length);
    if (fd < 0 || fd >= 128)
    {
        sys_exit(-1);
    }
    if (fd == 1)
    {
        putbuf((const char *)buffer, (size_t)length);
    } else if (fd >= 2)
    {
        off_t written;
        local_fdt = thread_current()->file_descriptor_table;

        written = file_write(local_fdt[fd], buffer, length);
        return written;
    }
    return length;
}
static bool sys_create(const char *file, unsigned initial_size)
{
    check(file);
    if (file == NULL)
    {
        return false;
    }
    return filesys_create(file, initial_size);
}
static int sys_open(const char *file)
{
    check(file);

    if (*file == '\0' || file == NULL)
    {
        return -1;
    }

    struct file *open_file = filesys_open(file);
    if (open_file == NULL)
    {
        return -1;
    } else
    {
        local_fdt = thread_current()->file_descriptor_table;
        int open_fd = -1;

        for (int i = 2; i < 128; i++)
        {
            if (local_fdt[i] == NULL)
            {
                local_fdt[i] = open_file;
                open_fd = i;
                break;
            }
        }
        if (open_fd == -1)
        {
            file_close(open_file);
        }
        return open_fd;
    }
}
static void sys_close(int fd)
{
    local_fdt = thread_current()->file_descriptor_table;

    if (fd < 2 || fd >= 128 || local_fdt[fd] == NULL)
    {
        sys_exit(-1);
    }
    file_close(local_fdt[fd]);
    local_fdt[fd] = NULL;
}

static int sys_read(int fd, void *buffer, unsigned length)
{
    check_start_to_end(buffer, length);
    if (fd < 0 || fd >= 128 || fd == 1)
    {
        sys_exit(-1);
    }
    if (fd == 0)
    {
        int size = length;
        char *ptr = (char *)buffer;
        while (size > 0)
        {
            size--;
            *ptr++ = input_getc();
        }
        return length;
    }
    if (fd >= 2)
    {
        local_fdt = thread_current()->file_descriptor_table;
        off_t read_size = file_read(local_fdt[fd], buffer, length);
        if (read_size < 0)
        {
            sys_exit(-1);
        }
        return read_size;
    }
}
static int sys_filesize(int fd)
{
    local_fdt = thread_current()->file_descriptor_table;
    return file_length(local_fdt[fd]);
}