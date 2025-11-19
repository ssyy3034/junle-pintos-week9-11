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
void error(void) {
    thread_current()->exit_code = -1;
    thread_exit();
}
void check(void *addr) {
    if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
        error();
    }
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED) {
    switch (f->R.rax) {
    case SYS_HALT:
        power_off();
        break;

    case SYS_EXIT:
        int status = f->R.rdi;
        thread_current()->exit_code = status;
        thread_exit();
        break;

    case SYS_CREATE:
        char *file_name = f->R.rdi;
        size_t file_size = f->R.rsi;

        check(file_name);

        if (*file_name == '\0') {
            f->R.rax = 0;
            break;
        } else if (file_name == NULL) {
            error();
        }
        if (!filesys_create(file_name, file_size)) {
            f->R.rax = 0;
        } else {
            f->R.rax = 1;
        }

        break;

    case SYS_OPEN:

        char *file_name_open = f->R.rdi;

        check(file_name_open);

        if (*file_name_open == '\0' || file_name_open == NULL) {
            f->R.rax = -1;
            break;
        }
        struct file *open_file = filesys_open(file_name_open);
        if (open_file == NULL) {
            f->R.rax = -1;
            break;
        } else {
            struct file **local_fdt = thread_current()->file_descriptor_table;
            int open_fd = -1;
            for (int i = 2; i < 512; i++) {
                if (local_fdt[i] == NULL) {
                    local_fdt[i] = open_file;
                    open_fd = i;
                    break;
                }
            }
            if (open_fd == -1) {
                file_close(open_file);
            }
            f->R.rax = open_fd;
        }
        break;

    case SYS_WRITE:

        int fd = f->R.rdi;
        void *buf = f->R.rsi;
        size_t size = f->R.rdx;

        check(buf);

        if (fd == 1) {
            putbuf(buf, size);
            f->R.rax = size;
        }
        break;

    default:
        thread_exit();
        break;
    }
    // TODO: Your implementation goes here.
}
