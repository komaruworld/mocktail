#pragma once
#ifndef __APPLE__
#pragma push_macro("_FORTIFY_SOURCE")
#pragma push_macro("__bos")
#undef _FORTIFY_SOURCE
#undef __bos
#include "../../bionic/libc/include/sys/cdefs.h"
#pragma pop_macro("__bos")
#pragma pop_macro("_FORTIFY_SOURCE")
#endif
#if defined(__has_include_next)
#if __has_include_next(<sys/cdefs.h>)
#include_next <sys/cdefs.h>
#endif
#else
#include_next <sys/cdefs.h>
#endif
#ifdef __BIONIC__
#undef __BIONIC__
#endif
