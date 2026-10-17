// This file is part of the dosbox-automation Project.
// License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
//

#ifndef DOSBOX_FLOPPY_IMAGE_H
#define DOSBOX_FLOPPY_IMAGE_H

#include <filesystem>

namespace FloppyImage {

// imageDisk accepts ksize+1; this adds a BPB check for the extra KB.
bool IsFloppy(const std::filesystem::path& host_path);

} // namespace FloppyImage

#endif // DOSBOX_FLOPPY_IMAGE_H
