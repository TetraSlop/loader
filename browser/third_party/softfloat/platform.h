/* SoftFloat build options for tmloader's browser client, shared by the native and Emscripten builds. */
#define LITTLEENDIAN 1
#define SOFTFLOAT_FAST_INT64 1
#ifdef _MSC_VER
#define INLINE static __inline
#else
#define INLINE static inline
#endif
