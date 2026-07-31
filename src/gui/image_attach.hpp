#ifndef MOOCODE_GUI_IMAGE_ATTACH_HPP
#define MOOCODE_GUI_IMAGE_ATTACH_HPP

// Turning a clipboard or drag-and-drop payload into images the provider layer
// can send. The encoding itself is agent/image_util.hpp's (base64 + IANA media
// type); what lives here is the Qt half — reading QMimeData, deciding between
// the file on disk and the bitmap Qt decoded for us, and producing the preview
// the composer and the transcript show.
//
// Qt-dependent by nature (QMimeData, QImage), so unlike moogui_theme and
// moogui_convimport this is compiled into the moogui target rather than a
// library a display-free test could link.

#include <QImage>
#include <QString>
#include <QStringList>

#include <vector>

#include "agent/types.hpp"  // ImageBlock

class QMimeData;

namespace moocode::gui {

// One image staged in the composer: the bytes the request will carry, plus
// what the UI needs in order to show it.
struct StagedImage {
    int id = 0;        // assigned by the composer; the handle a chip removes by
    ImageBlock block;  // base64 + media type, ready for a ContentPart
    QImage preview;    // downscaled thumbnail; null when nothing could decode it
    QString label;     // the file name, or "pasted image" for a clipboard bitmap
};

struct AttachResult {
    std::vector<StagedImage> images;  // `id` left 0; the caller assigns them
    QStringList errors;               // one per payload that could not be read
};

// True when `md` carries at least one thing attach_images() would extract.
// Inspects the advertised mime formats only — it decodes nothing, so it is
// cheap enough to call from a drag-enter handler.
bool has_attachable_image(const QMimeData* md);

// Every image in `md`. Local file URLs naming an image extension win: reading
// the file keeps the original bytes and its true media type, where the bitmap
// Qt hands over has been through a decode already. Failing that, the decoded
// bitmap is used — passed through unchanged when the source also offered the
// compressed bytes, and re-encoded as PNG otherwise. Bitmaps larger than
// kMaxImageSide on their long edge are downscaled first: a 4K screenshot costs
// a fortune in tokens and some backends reject it outright, and no model reads
// it at that resolution anyway. Files are never rescaled — a path the user
// pointed at is sent as it is.
//
// Never fails as a whole: an unreadable payload becomes an entry in `errors`
// and the rest still attach.
AttachResult attach_images(const QMimeData* md);

}  // namespace moocode::gui

#endif  // MOOCODE_GUI_IMAGE_ATTACH_HPP
