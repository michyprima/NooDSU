#pragma once
#include <cstdint>
typedef int BOOL;
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cstddef>
typedef int64_t OSTime; typedef uint32_t OSTick;
struct OSThread{int x;}; typedef int (*OSThreadEntryPointFn)(int,const char**);
typedef int OSThreadAttributes;
enum { OS_THREAD_ATTRIB_AFFINITY_CPU0=1, OS_THREAD_ATTRIB_AFFINITY_CPU1=2, OS_THREAD_ATTRIB_AFFINITY_CPU2=4, OS_THREAD_ATTRIB_DETACHED=8 };
enum OSCodeGenMode{ OS_CODEGEN_RW_=0 };
inline uint32_t OSGetCoreId(){return 1;}
inline OSTime OSGetSystemTime(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline OSTick OSGetSystemTick(){return (OSTick)OSGetSystemTime();}
inline int64_t OSTicksToMicroseconds(int64_t t){return t;}
inline OSThread*OSGetCurrentThread(){static OSThread t;return &t;}
inline int OSGetThreadPriority(OSThread*){return 16;} inline int OSGetThreadAffinity(OSThread*){return 2;}
inline bool OSCreateThread(OSThread*,OSThreadEntryPointFn,int,char*,void*,uint32_t,int,int){return false;}
inline void OSSetThreadDeallocator(OSThread*,void(*)(OSThread*,void*)){} inline void OSSetThreadRunQuantum(OSThread*,int){} inline void OSResumeThread(OSThread*){}
inline void OSSetThreadPriority(OSThread*,int){} inline void OSJoinThread(OSThread*,int*){}
inline uint32_t OSSwapAtomic(uint32_t*p,uint32_t v){return __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST);}
inline bool OSCompareAndSwapAtomicEx(uint32_t*p,uint32_t e,uint32_t n,uint32_t*o){bool r=__atomic_compare_exchange_n(p,&e,n,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);*o=e;return r;}
inline void DCFlushRange(void*,uint32_t){} inline void DCInvalidateRange(void*,uint32_t){} inline void ICInvalidateRange(void*,uint32_t){} inline void DCStoreRange(void*,uint32_t){}
inline void OSFatal(const char*m){puts(m);abort();}
inline uint32_t OSGetCodegenCore(){return 1;} inline uint32_t OSGetCodegenMode(){return 0;} inline uint32_t OSGetSecCodeGenMode(){return 0;} enum {CODEGEN_RW_=0,CODEGEN_R_X=1}; inline int OSSwitchSecCodeGenMode(int){return 1;}
inline void OSGetCodegenVirtAddrRange(void**a,uint32_t*s){static void*b=malloc(0x4000000);*a=b;*s=0x4000000;}
