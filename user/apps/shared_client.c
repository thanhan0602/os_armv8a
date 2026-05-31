#include <user/shared.h>

int main(void)
{
    shared_write("[shared-client] hello via shared lib\n");
    return 0;
}