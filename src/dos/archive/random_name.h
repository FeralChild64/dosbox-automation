// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//
#ifndef DOSBOX_ARCHIVE_RANDOM_NAME_H
#define DOSBOX_ARCHIVE_RANDOM_NAME_H

#include <string>

namespace ArchiveMount {

// Lower-case hex from std::random_device, eight digits per word, for
// names nobody else can predict (temporaries, scratch and trash dirs).
std::string RandomHex(int words);

} // namespace ArchiveMount

#endif
