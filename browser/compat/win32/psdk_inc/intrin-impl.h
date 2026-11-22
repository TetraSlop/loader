// Stands in for mingw-w64's x86 intrinsics, which are inline assembly. winnt.h still names these three.
#ifndef KESTREL_INTRIN_IMPL
#define KESTREL_INTRIN_IMPL
#define __buildpause()
#define __buildmemorybarrier() \
    {                          \
        __sync_synchronize();  \
    }
#define __readfsdword(offset) 0ul
#endif
