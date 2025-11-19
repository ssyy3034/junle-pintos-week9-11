#include "tests/lib.h"
#include <random.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syscall.h>

const char *test_name;
bool quiet = false;

static void
vmsg(const char *format, va_list args, const char *suffix)
{
  /* 우리는 전체 메시지를 하나의 버퍼에 채운 뒤,
     단 한 번의 시스템 콜로 출력하기 위해 꽤 신경을 쓴다.
     이렇게 해야 (일반적으로) 그 메시지가 콘솔에 원자적으로 출력되기 때문이다.
     그렇지 않으면 "foo: exit(0)" 같은 커널 메시지들이
     운이 나쁘면 서로 뒤섞여(interleaved) 보일 수 있다. */
  static char buf[1024];

  snprintf(buf, sizeof buf, "(%s) ", test_name);
  vsnprintf(buf + strlen(buf), sizeof buf - strlen(buf), format, args);
  strlcpy(buf + strlen(buf), suffix, sizeof buf - strlen(buf));
  write(STDOUT_FILENO, buf, strlen(buf));
}

void msg(const char *format, ...)
{
  va_list args;

  if (quiet)
    return;
  va_start(args, format);
  vmsg(format, args, "\n");
  va_end(args);
}

void fail(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vmsg(format, args, ": FAILED\n");
  va_end(args);

  exit(1);
}

static void
swap(void *a_, void *b_, size_t size)
{
  uint8_t *a = a_;
  uint8_t *b = b_;
  size_t i;

  for (i = 0; i < size; i++)
  {
    uint8_t t = a[i];
    a[i] = b[i];
    b[i] = t;
  }
}

void shuffle(void *buf_, size_t cnt, size_t size)
{
  char *buf = buf_;
  size_t i;

  for (i = 0; i < cnt; i++)
  {
    size_t j = i + random_ulong() % (cnt - i);
    swap(buf + i * size, buf + j * size, size);
  }
}

void exec_children(const char *child_name, pid_t pids[], size_t child_cnt)
{
  size_t i;

  for (i = 0; i < child_cnt; i++)
  {
    char cmd_line[128];
    snprintf(cmd_line, sizeof cmd_line, "%s %zu", child_name, i);
    if ((pids[i] = fork(child_name)))
    {
      CHECK(pids[i] != PID_ERROR,
            "exec child %zu of %zu: \"%s\"", i + 1, child_cnt, cmd_line);
    }
    else
    {
      exec(cmd_line);
    }
  }
}

void wait_children(pid_t pids[], size_t child_cnt)
{
  size_t i;

  for (i = 0; i < child_cnt; i++)
  {
    int status = wait(pids[i]);
    CHECK(status == (int)i,
          "wait for child %zu of %zu returned %d (expected %zu)",
          i + 1, child_cnt, status, i);
  }
}

void check_file_handle(int fd,
                       const char *file_name, const void *buf_, size_t size)
{
  const char *buf = buf_;
  size_t ofs = 0;
  size_t file_size;

  /* Warn about file of wrong size.  Don't fail yet because we
     may still be able to get more information by reading the
     file. */
  file_size = filesize(fd);
  if (file_size != size)
    msg("size of %s (%zu) differs from expected (%zu)",
        file_name, file_size, size);

  /* Read the file block-by-block, comparing data as we go. */
  while (ofs < size)
  {
    char block[512];
    size_t block_size, ret_val;

    block_size = size - ofs;
    if (block_size > sizeof block)
      block_size = sizeof block;

    ret_val = read(fd, block, block_size);
    if (ret_val != block_size)
      fail("read of %zu bytes at offset %zu in \"%s\" returned %zu",
           block_size, ofs, file_name, ret_val);

    compare_bytes(block, buf + ofs, block_size, ofs, file_name);
    ofs += block_size;
  }

  /* Now fail due to wrong file size. */
  if (file_size != size)
    fail("size of %s (%zu) differs from expected (%zu)",
         file_name, file_size, size);

  msg("verified contents of \"%s\"", file_name);
}

void check_file(const char *file_name, const void *buf, size_t size)
{
  int fd;

  CHECK((fd = open(file_name)) > 1, "open \"%s\" for verification",
        file_name);
  check_file_handle(fd, file_name, buf, size);
  msg("close \"%s\"", file_name);
  close(fd);
}

void compare_bytes(const void *read_data_, const void *expected_data_, size_t size,
                   size_t ofs, const char *file_name)
{
  const uint8_t *read_data = read_data_;
  const uint8_t *expected_data = expected_data_;
  size_t i, j;
  size_t show_cnt;

  if (!memcmp(read_data, expected_data, size))
    return;

  for (i = 0; i < size; i++)
    if (read_data[i] != expected_data[i])
      break;
  for (j = i + 1; j < size; j++)
    if (read_data[j] == expected_data[j])
      break;

  quiet = false;
  msg("%zu bytes read starting at offset %zu in \"%s\" differ "
      "from expected.",
      j - i, ofs + i, file_name);
  show_cnt = j - i;
  if (j - i > 64)
  {
    show_cnt = 64;
    msg("Showing first differing %zu bytes.", show_cnt);
  }
  msg("Data actually read:");
  hex_dump(ofs + i, read_data + i, show_cnt, true);
  msg("Expected data:");
  hex_dump(ofs + i, expected_data + i, show_cnt, true);
  fail("%zu bytes read starting at offset %zu in \"%s\" differ "
       "from expected",
       j - i, ofs + i, file_name);
}
