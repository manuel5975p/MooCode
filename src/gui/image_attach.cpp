#include "gui/image_attach.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QFileInfo>
#include <QMimeData>
#include <QObject>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "agent/image_util.hpp"  // is_image_extension, read_image
#include "agent/strutil.hpp"     // base64_encode

namespace moocode::gui {
namespace {

// Long-edge cap for a re-encoded clipboard bitmap. 1568px is the resolution
// above which Anthropic's vision models stop gaining anything, and it keeps a
// full-screen paste from turning into megabytes of base64.
constexpr int kMaxImageSide = 1568;

// Long edge of the stored preview. One preview serves both the composer chip
// and the thumbnail in the sent message, scaled down at paint time.
constexpr int kPreviewSide = 320;

// Hard cap on the encoded bytes of a single image, matching read_image's own
// default. Past this the request is not worth attempting.
constexpr std::size_t kMaxImageBytes = 20u * 1024u * 1024u;

// Compressed clipboard formats we can forward untouched, best first. Anything
// else goes through a PNG re-encode.
constexpr const char* kPassthroughFormats[] = {"image/png", "image/jpeg",
                                               "image/webp"};

QImage make_preview(const QImage& img) {
    if (img.isNull()) return QImage();
    if (std::max(img.width(), img.height()) <= kPreviewSide) return img;
    return img.scaled(kPreviewSide, kPreviewSide, Qt::KeepAspectRatio,
                      Qt::SmoothTransformation);
}

std::string to_std(const QByteArray& b) {
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

// The decoded bitmap as an ImageBlock, or nullopt with `error` set. `md` is
// consulted for the original compressed bytes; it may be null when the caller
// only has the image.
std::optional<ImageBlock> encode_bitmap(const QImage& img, const QMimeData* md,
                                        QString& error) {
    if (img.isNull()) {
        error = QObject::tr("the clipboard image could not be decoded");
        return std::nullopt;
    }
    const bool oversized =
        std::max(img.width(), img.height()) > kMaxImageSide;

    // Untouched original, when there is one and it does not need rescaling.
    if (!oversized && md) {
        for (const char* fmt : kPassthroughFormats) {
            if (!md->hasFormat(QString::fromLatin1(fmt))) continue;
            const QByteArray raw = md->data(QString::fromLatin1(fmt));
            if (raw.isEmpty()) continue;
            if (static_cast<std::size_t>(raw.size()) > kMaxImageBytes) continue;
            return ImageBlock{.base64_data = base64_encode(to_std(raw)),
                              .media_type = fmt};
        }
    }

    const QImage scaled =
        oversized ? img.scaled(kMaxImageSide, kMaxImageSide, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation)
                  : img;
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    if (!scaled.save(&buf, "PNG")) {
        error = QObject::tr("the image could not be encoded as PNG");
        return std::nullopt;
    }
    buf.close();
    if (static_cast<std::size_t>(png.size()) > kMaxImageBytes) {
        error = QObject::tr("the image is too large (%1 MB encoded)")
                    .arg(png.size() / (1024 * 1024));
        return std::nullopt;
    }
    return ImageBlock{.base64_data = base64_encode(to_std(png)),
                      .media_type = "image/png"};
}

// Local image files named by `md`'s URLs, in order.
QStringList local_image_paths(const QMimeData* md) {
    QStringList paths;
    if (!md->hasUrls()) return paths;
    for (const QUrl& u : md->urls()) {
        if (!u.isLocalFile()) continue;
        const QString path = u.toLocalFile();
        if (!is_image_extension(path.toStdString())) continue;
        paths << path;
    }
    return paths;
}

}  // namespace

bool has_attachable_image(const QMimeData* md) {
    if (!md) return false;
    if (!local_image_paths(md).isEmpty()) return true;
    if (md->hasImage()) return true;
    for (const char* fmt : kPassthroughFormats)
        if (md->hasFormat(QString::fromLatin1(fmt))) return true;
    return false;
}

AttachResult attach_images(const QMimeData* md) {
    AttachResult out;
    if (!md) return out;

    for (const QString& path : local_image_paths(md)) {
        const QString name = QFileInfo(path).fileName();
        auto blk = read_image(std::filesystem::path(path.toStdString()),
                             kMaxImageBytes);
        if (!blk) {
            out.errors << QStringLiteral("%1: %2").arg(
                name, QString::fromStdString(blk.error().msg));
            continue;
        }
        StagedImage s;
        s.block = std::move(*blk);
        s.label = name;
        // Decoded only for the thumbnail; a format Qt cannot read (an exotic
        // TIFF, say) still attaches, just without a preview.
        s.preview = make_preview(QImage(path));
        out.images.push_back(std::move(s));
    }
    // A drag carrying file URLs is about those files: don't also attach the
    // bitmap preview some sources ship alongside them, or every drop from a
    // browser would arrive twice.
    if (!out.images.empty() || !out.errors.isEmpty()) return out;

    QImage img = qvariant_cast<QImage>(md->imageData());
    if (img.isNull()) {
        // Some sources advertise the compressed bytes without an
        // application/x-qt-image conversion Qt can perform for us.
        for (const char* fmt : kPassthroughFormats) {
            if (!md->hasFormat(QString::fromLatin1(fmt))) continue;
            img = QImage::fromData(md->data(QString::fromLatin1(fmt)));
            if (!img.isNull()) break;
        }
    }
    if (img.isNull()) return out;  // nothing attachable; not an error

    QString error;
    auto blk = encode_bitmap(img, md, error);
    if (!blk) {
        if (!error.isEmpty()) out.errors << error;
        return out;
    }
    StagedImage s;
    s.block = std::move(*blk);
    s.label = QObject::tr("pasted image");
    s.preview = make_preview(img);
    out.images.push_back(std::move(s));
    return out;
}

}  // namespace moocode::gui
