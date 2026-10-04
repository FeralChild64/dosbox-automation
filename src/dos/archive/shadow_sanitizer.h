// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_SHADOW_SANITIZER_H
#define DOSBOX_ARCHIVE_SHADOW_SANITIZER_H

// ASan, TSan and MSan reserve terabytes of shadow address space at start,
// which any RLIMIT_AS below that breaks; the helper runs uncapped there.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DOSBOX_SHADOW_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
        __has_feature(memory_sanitizer)
#define DOSBOX_SHADOW_SANITIZER 1
#endif
#endif

namespace ArchiveHelper {

#if defined(DOSBOX_SHADOW_SANITIZER)
constexpr bool ShadowSanitizer = true;
#else
constexpr bool ShadowSanitizer = false;
#endif

} // namespace ArchiveHelper

#endif // DOSBOX_ARCHIVE_SHADOW_SANITIZER_H
