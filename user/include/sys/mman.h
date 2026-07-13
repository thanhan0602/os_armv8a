#ifndef SYS_MMAN_H
#define SYS_MMAN_H

#define PROT_NONE       0x0             /* Page can not be accessed.  */
#define PROT_READ       0x1             /* Page can be read.  */
#define PROT_WRITE      0x2             /* Page can be written.  */
#define PROT_EXEC       0x4             /* Page can be executed.  */

#define MAP_SHARED      0x01            /* Share changes.  */
#define MAP_PRIVATE     0x02            /* Changes are private.  */
#define MAP_FIXED       0x10            /* Interpret addr exactly.  */
#define MAP_ANONYMOUS   0x20            /* Don't use a file.  */
#define MAP_ANON        MAP_ANONYMOUS

#include <user/syscall.h>

static inline void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, unsigned long offset)
{
    return user_mmap(addr, length, prot, flags, fd, offset);
}

#endif
