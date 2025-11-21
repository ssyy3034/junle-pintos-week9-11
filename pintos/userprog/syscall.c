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
// 추가한 헬퍼함수들 ========
bool is_valid_addr(const void *u_add);
// syscall 함수들 ========
static void sys_halt(void);                                      // 완료
static void sys_exit(int status);                                // 완료
static bool sys_create(const char *file, unsigned initial_size); // 완료
static int sys_open(const char *file);                           // 완료
static int sys_read(int fd, void *buffer, unsigned length);
static int sys_write(int fd, const void *buffer, unsigned length); // 완료
static bool sys_create(const char *file, unsigned initial_size);   // 완료
static int sys_open(const char *file);                             // 완료
// helper 함수들 ========
void check_valid_addr(void *addr);
static int create_fd(struct file *f);

struct file **local_fdt;

static struct lock file_lock;
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
    if (!is_valid_addr(buffer))
    {
        sys_exit(-1);
    }
    uint8_t *buf = (uint8_t *)buffer; // [!] void타입 포인터는 사이즈 알 수 없어 값 넣기/수정불가
    if (fd == 0)                      // 키보드
    {
        for (int i = 0; i < length; i++)
        {
            uint8_t key = input_getc();
            buf[i] = key; // buffer[i] = key; -> void타입 포인터에는 값을 집어넣을 수 없음
        }
        return length;
        /*
            Reads SIZE bytes from FILE into BUFFER,
            * starting at offset FILE_OFS in the file.
            * Returns the number of bytes actually read,

            Returns the number of bytes actually read (0 at end of file),
            or -1 if the file could not be read (due to a condition other than end of file).
            fd 0 reads from the keyboard using input_getc().

        - `read()`는 열려 있는 파일(fd)에서
            size 바이트만큼 데이터를 읽어서 buffer에 넣는다.

        - 반환값은 **실제로 읽은 바이트 수**다.
        - 파일의 끝(EOF)에 도달하면
            → 0을 반환한다.

        - 읽을 수 없는 다른 오류(파일이 없거나, 읽기 불가능한 상황 등)가 발생하면
            → -1을 반환한다.

        - fd = 0 은 키보드(stdin)를 의미한다.
            이 경우 읽기는 `input_getc()`를 사용해 한 글자씩 받아와야 한다.
        Reads SIZE bytes from FILE into BUFFER,
        * starting at the file's current position.
        * Returns the number of bytes actually read,
        * which may be less than SIZE if end of file is reached.
        * Advances FILE's position by the number of bytes read.

        off_t file_read(struct file *file, void *buffer, off_t size)
        {
            off_t bytes_read = inode_read_at(file->inode, buffer, size, file->pos);
            file->pos += bytes_read;
            return bytes_read;
        }

        */
    } else
    {
        // 1) 파일 불러오기
        struct file *f = get_file_from_fd(fd);
        // 2) length만큼 읽기 (file --> buffer)
        off_t read_bytes = file_read(f, buffer, length); // off_t <=> int
        // #) 반환: 실제로 읽은 Byte수
        if (read_bytes == NULL)
        {
            return -1;
        } else if (read_bytes < length)
        { // EOF 도달해서 다 못 읽은 거 아닌지?
            return 0;
        }
        return read_bytes;
    }
}

static int sys_write(int fd, const void *buffer, unsigned length)
{
    if (!is_valid_addr(buffer)) // 끝도 확인하긴해야하는데..
    {
        sys_exit(-1);
    }
    if (fd == 0 || fd == NULL)
    {
        return -1;
    } else if (fd == 1)
    {
        putbuf((const char *)buffer, (size_t)length);
        return length; // 수백바이트 이상이면 한번의 putbuf호출로 전체 버퍼 출력해야하는데
                       //  그거 구현 어떻게해야할지
    } else
    {
        struct file *f = get_file_from_fd(fd);
        if (f == NULL)
        {
            return -1;
        }
        off_t len = file_write(f, buffer, length); // file_write(): 쓰인 바이트수만 반환
        return (int)len;
    }
    // 추가사항: 권한 확인(쓰기가능파일인지), 콘솔 출력시, size>=1000Byte면 여러번 나눠서 출력하도록,
    /*
        char buf = 123;
        write(0x01012342, &buf, 1);
        write(7, &buf, 1);
        write(2546, &buf, 1);
        write(-5, &buf, 1);
        write(-8192, &buf, 1);
        write(INT_MIN + 1, &buf, 1);
        write(INT_MAX - 1, &buf, 1);
    */
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