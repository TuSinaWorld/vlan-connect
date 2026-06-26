#ifndef VLAN_MSVC2015_COMPAT_H
#define VLAN_MSVC2015_COMPAT_H

/*
 * Workaround: Windows SDK 10.0.26100+ uses _mm_loadu_si64() in <wchar.h>,
 * but MSVC 2015 (v140) does not provide this intrinsic in <emmintrin.h>.
 *
 * We define it here in terms of _mm_loadl_epi64() which IS available.
 * This header is force-included via /FI before every compilation unit,
 * so it takes effect before any system header is pulled in.
 */

#if defined(_MSC_VER) && _MSC_VER < 1920

#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

static __forceinline __m128i _mm_loadu_si64(const void* _Addr) {
    return _mm_loadl_epi64((const __m128i*)_Addr);
}

#ifdef __cplusplus
}
#endif

#endif /* _MSC_VER < 1920 */

#endif /* VLAN_MSVC2015_COMPAT_H */
