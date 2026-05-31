#include <user/shared.h>
#include <user/syscall.h>

void shared_write(const char *text)
{
    user_write_string(text);
}