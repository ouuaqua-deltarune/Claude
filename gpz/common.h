/* Shared by C (CPU) and OpenCL C (GPU) compilation of the codecs. */
#ifndef GPZ_COMMON
#define GPZ_COMMON
#ifdef __OPENCL_VERSION__
  #define GP __global
  #define FN
  #define CONSTARR __constant
  typedef uchar u8; typedef ushort u16; typedef uint u32; typedef ulong u64;
#else
  #define GP
  #define FN static
  #define CONSTARR static const
  typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
  typedef unsigned long long u64;
#endif
#endif
